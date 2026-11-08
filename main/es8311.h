/**
 * @file  es8311.h
 * @brief ES8311 音频 CODEC 驱动（最小可用集，服务于测试而非播放功能）
 *
 * 器件背景：Everest Semiconductor ES8311，单声道低功耗音频 CODEC，
 * I2C 从机地址 0x18，负责 ADC（麦克风）与 DAC（耳机/功放）通路的配置。
 *
 * 寄存器定义来源：本机 esp_codec_dev 组件里的 device/es8311/es8311_reg.h
 * （Espressif 官方驱动），不靠记忆拼寄存器表。
 *
 * 设计取舍：
 *   - 只做 I2C 控制面：本框架不建立 I2S 时钟，因此不验证音频数据通路，
 *     也不配置采样率/主从模式 —— 那需要 MCLK/BCLK/LRCK 真的在跑才有意义。
 *   - 写操作一律"读原值 -> 写回原值 -> 比对"，只有 REG00 的状态翻转测试
 *     会临时改值，且结束时恢复原值。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "bus_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 常量 ---------------- */

#define ES8311_ADDR_DEFAULT      0x18   /* 7 位地址 */

/* 寄存器地址（源自 esp_codec_dev 的 es8311_reg.h） */
#define ES8311_REG_RESET00       0x00   /* 复位 / CSM 状态 / 主从模式 */
#define ES8311_REG_CLK01         0x01   /* MCLK 源选择与时钟使能 */
#define ES8311_REG_CLK02         0x02   /* 时钟分频与倍频 */
#define ES8311_REG_CLK03         0x03   /* ADC 帧模式与 OSR */
#define ES8311_REG_CLK04         0x04   /* DAC OSR */
#define ES8311_REG_CLK05         0x05   /* ADC/DAC 分频 */
#define ES8311_REG_CLK06         0x06   /* BCLK 反相与分频 */
#define ES8311_REG_CLK07         0x07   /* 三态 / LRCK 分频 */
#define ES8311_REG_CLK08         0x08   /* LRCK 分频 */

#define ES8311_REG_SDPIN09       0x09   /* DAC 串行数据口 */
#define ES8311_REG_SDPOUT0A      0x0A   /* ADC 串行数据口 */

#define ES8311_REG_ADC_VOL       0x17   /* ADC 音量（可读写，范围 0x00-0xFF） */
#define ES8311_REG_DAC_VOL       0x32   /* DAC 音量（可读写，范围 0x00-0xFF） */

#define ES8311_REG_ID1_FD        0xFD   /* CHIP ID1 */
#define ES8311_REG_ID2_FE        0xFE   /* CHIP ID2 */
#define ES8311_REG_VER_FF        0xFF   /* VERSION */

#define ES8311_REG_DUMP_MAX      0xFF

/*
 * REG00 位义 —— 依据官方驱动的实际用法反推，不是猜：
 *   驱动初始化写 0x80 上电、关闭时写 0x00 再写 0x1F 掉电；
 *   主从模式靠读-改-写 bit6 实现（master 置 1 / slave 清 0）。
 * 因此：bit7 = CSM 上电位，bit6 = 主从选择。
 */
#define ES8311_REG00_CSM_POWER   (1u << 7)
#define ES8311_REG00_MASTER      (1u << 6)
#define ES8311_REG00_POWER_ON    0x80   /* CSM 置起、Slave 模式 */
#define ES8311_REG00_POWER_DOWN  0x1F   /* 官方驱动使用的掉电/复位值 */

typedef struct {
    bus_i2c_t              *bus;
    i2c_master_dev_handle_t dev;
    uint8_t                 addr;
    bool                    ready;
} es8311_t;

/* ---------------- 生命周期 ---------------- */

/** 挂到已建好的总线上并校验器件存在（probe 0x18） */
esp_err_t es8311_init(es8311_t *d, bus_i2c_t *bus, uint8_t addr7);

/** 摘除器件句柄（不动总线） */
esp_err_t es8311_deinit(es8311_t *d);

/* ---------------- 寄存器访问 ---------------- */

esp_err_t es8311_read(es8311_t *d, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t es8311_read8(es8311_t *d, uint8_t reg, uint8_t *val);
esp_err_t es8311_write8(es8311_t *d, uint8_t reg, uint8_t val);

/** 读 REG00（复位/状态/主从寄存器）；失败返回 0xFF */
uint8_t es8311_reset_reg(es8311_t *d);

/** 原始寄存器转储 0x00..0xFF，用于人工核对，不做任何解释 */
void es8311_dump(es8311_t *d);

#ifdef __cplusplus
}
#endif
