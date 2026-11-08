/**
 * @file  cam_ov2640.h
 * @brief OV2640（DVP 8 位）板级封装 —— 供电、时钟、PWDN、SCCB
 *
 * 本文件只做"让传感器能被访问到"这件事，不做出图：
 *   1. 经 AXP2101 打开三路 LDO（ALDO2 / BLDO1 / BLDO2）；
 *   2. 用 LEDC 在 XCLK 脚输出 10 MHz（**不能**用 LCD_CAM 的 cam_clk，见
 *      board_config.h 里的说明，否则传感器完全不响应 SCCB）；
 *   3. 控制 PWDN；
 *   4. 在板上已有的 I2C0 上挂一个 0x30 的 SCCB 从机，读写传感器寄存器。
 *
 * 出图（DVP 收帧）不在这里：那需要 OV2640 的完整初始化寄存器表
 * （几百条，属于 esp32-camera / esp_cam_sensor 的职责），且本板未开 PSRAM，
 * 一帧 VGA RGB565 有 153.6 KB。详见 test_camera.c 的分层说明。
 *
 * 上电顺序是被硬件强制的，不能调换：
 *     设 LDO 电压 -> 使能 LDO -> 等稳定 -> 起 XCLK -> 释放 PWDN -> 探测 SCCB
 * 反过来（先使能再设电压）会让 DVDD 以 2.8 V 上电，而 OV2640 的 DVDD 只要 1.2 V。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "axp2101.h"
#include "bus_i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 传感器出厂 ID（OV2640 各字节均为固定值，可作"模组真的在"的硬证据） */
typedef struct {
    uint8_t pid;   /* REG 0x0A，sensor bank，OV2640 = 0x26 */
    uint8_t ver;   /* REG 0x0B，OV2640 = 0x42 */
    uint8_t midh;  /* REG 0x1C，OmniVision = 0x7F */
    uint8_t midl;  /* REG 0x1D，OV2640 = 0xA2 */
} cam_id_t;

typedef struct {
    i2c_master_dev_handle_t sccb;      /* SCCB 从机（挂在板上 I2C0） */
    bool                    sccb_ok;
    bool                    pwdn_ok;   /* PWDN 脚已配置成输出 */
    bool                    xclk_ok;   /* LEDC 已在输出 */
    bool                    power_ok;  /* 三路 LDO 已使能 */
    uint32_t                xclk_hz;   /* LEDC 回报的实际频率 */
    uint8_t                 pwdn_level;/* 当前 PWDN 电平，1 = 掉电 */
} cam_t;

/* ---------------- 资源准备（不涉及供电） ----------------
 * 配置 PWDN 为输出并保持掉电；在 bus 上挂 0x30 的 SCCB 从机；起 XCLK。
 * 可重复调用（幂等），`run camera` 反复跑不会重复占资源。
 */
esp_err_t cam_prepare(cam_t *c, bus_i2c_t *bus);

/** 释放 SCCB 从机与 XCLK（PWDN 保持掉电，不留隐患）
 *
 * 注意不叫 cam_deinit：esp32-camera 组件的 cam_hal.c 里已有同名全局函数，
 * 会撞符号。那边管的是采集控制器，这里管的是总线与时钟。 */
esp_err_t cam_release(cam_t *c);

/* ---------------- 供电 ---------------- */

/**
 * 三路 LDO：先逐一设电压（并回读核对），再一次性使能，最后等稳定。
 * 只操作 ALDO2/BLDO1/BLDO2，不碰 DCDC 与其他 LDO。
 *
 * @param settle_ms 使能后等待时间，0 表示用 BOARD_CAM_POWER_SETTLE_MS
 */
esp_err_t cam_power_on(cam_t *c, axp2101_t *pmic, uint32_t settle_ms);

/** 关闭三路 LDO（PWDN 会被先拉高，避免掉电瞬间在数据线上产生倒灌电流） */
esp_err_t cam_power_off(cam_t *c, axp2101_t *pmic);

/** 读回三路当前设定电压（mV），用于核对写入是否生效 */
esp_err_t cam_power_read_mv(axp2101_t *pmic, uint16_t *aldo2, uint16_t *bldo1,
                            uint16_t *bldo2);

/** 三路是否全部使能 */
esp_err_t cam_power_is_on(axp2101_t *pmic, bool *on);

/* ---------------- PWDN / XCLK ---------------- */

/** control = true 拉高进入掉电，false 拉低进入工作 */
esp_err_t cam_pwdn(cam_t *c, bool power_down);

/** 读回 PWDN 引脚实际电平（验证输出真的翻转了，而不是"设了就当设上了"） */
esp_err_t cam_pwdn_read(cam_t *c, int *level);

esp_err_t cam_xclk_start(cam_t *c);
esp_err_t cam_xclk_stop(cam_t *c);

/* ---------------- SCCB ---------------- */

/** 探测 0x30 是否应答（只发地址，不读写数据） */
bool cam_sccb_present(cam_t *c);

esp_err_t cam_reg_read(cam_t *c, uint8_t reg, uint8_t *val);
esp_err_t cam_reg_write(cam_t *c, uint8_t reg, uint8_t val);

/**
 * 读出厂 ID。内部会先写 0xFF=0x01 切到 sensor bank ——
 * 0x0A/0x0B/0x1C/0x1D 只在 sensor bank（1）里是 ID，bank0 里不是，
 * 漏了这一步会读到无关寄存器、把好模组判成"不认识"。
 */
esp_err_t cam_read_id(cam_t *c, cam_id_t *id);

#ifdef __cplusplus
}
#endif
