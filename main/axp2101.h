/**
 * @file  axp2101.h
 * @brief AXP2101 电源管理 IC 驱动（最小可用集，服务于测试而非量产功能）
 *
 * 器件背景：X-Powers AXP2101，I2C 从机，7 位地址 0x34。
 * 板上通常负责：电池充电、多路 DCDC/LDO、电压电流 ADC、电源键与中断。
 *
 * 设计取舍：主测试部分只实现"能验证芯片是好的"所需的最小寄存器访问，
 * 涉及写操作的用例一律采用"读原值 -> 写回原值 -> 比对"，不改变芯片状态。
 *
 * 例外：摄像头供电（下方 LDO 段）。这是**有意**要改变芯片状态的唯一一处，
 * 因为 OV2640 的三路电源挂在本片 LDO 上，不配就没有摄像头。为控制风险，
 * 写操作被限定在 0x90 的 bit1/4/5 与 0x93/0x96/0x97 三个电压寄存器，
 * 绝不碰 DCDC、充电、其他 LDO —— 那些写错会直接掉电。
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

#define AXP2101_ADDR_DEFAULT   0x34

/* 寄存器地址（依据 AXP2101 数据手册寄存器表） */
#define AXP2101_REG_CHIP_ID    0x03   /* 芯片 ID */
#define AXP2101_CHIP_ID_VALUE  0x4A   /* AXP2101 的 ID 应为 0x4A */

#define AXP2101_REG_ADC_EN     0x30   /* ADC 通道使能 */
#define AXP2101_ADC_EN_VBAT    (1u << 0)
#define AXP2101_ADC_EN_TS      (1u << 1)
#define AXP2101_ADC_EN_VBUS    (1u << 2)
#define AXP2101_ADC_EN_VSYS    (1u << 3)
#define AXP2101_ADC_EN_TDIE    (1u << 4)

/* ADC 结果：每通道 16 位，高字节在前 */
#define AXP2101_REG_ADC_VBAT_H 0x34
#define AXP2101_REG_ADC_TS_H   0x36
#define AXP2101_REG_ADC_VBUS_H 0x38
#define AXP2101_REG_ADC_VSYS_H 0x3A
#define AXP2101_REG_ADC_TDIE_H 0x3C

/* VBUS 读数的合理上限。USB-C 拔掉后 VBUS 采样脚悬空，AXP2101 的该通道
 * 会饱和到接近 14 位满量程（实测 16370 mV），而 USB 供电本身不会超过 5.5 V。
 * 超过这个门限就按“无输入 = 0”处理，见 axp2101_read_batt()。 */
#define AXP2101_VBUS_SANE_MAX_MV 6500

#define AXP2101_REG_IRQ_STAT0  0x48   /* 中断状态 1/2/3 连续三个寄存器 */
#define AXP2101_REG_IRQ_STAT_N 3

/* ---------------- 电池与充电状态 ----------------
 *
 * 位定义来自 AXP2101 数据手册 8.2 节（REG 00 / REG 01）。
 * 这两个寄存器都是只读状态，读了不会改变芯片行为，可以随时查询。
 */
#define AXP2101_REG_STATUS1     0x00   /* PMU status1 */
#define AXP2101_REG_STATUS2     0x01   /* PMU status2 */
#define AXP2101_REG_CHG_CTRL    0x18   /* 充电/电量计/看门狗总开关，只观察不写 */
#define AXP2101_REG_BAT_PCT     0xA4   /* 燃料计电量百分比 */

/* REG 0x00 */
#define AXP2101_ST1_VBUS_GOOD   (1u << 5)  /* 1 = VBUS 输入有效 */
#define AXP2101_ST1_BATFET_OPEN (1u << 4)  /* 1 = 电池 FET 导通（电池在供电） */
#define AXP2101_ST1_BAT_PRESENT (1u << 3)  /* 1 = 检测到电池 */
#define AXP2101_ST1_THERM_REGU  (1u << 1)  /* 1 = 触发过热调节 */

/* REG 0x01 */
#define AXP2101_ST2_CUR_DIR_MASK 0x60u     /* bit6:5 电池电流方向 */
#define AXP2101_ST2_SYS_ON      (1u << 4)
#define AXP2101_ST2_VINDPM      (1u << 3)  /* 1 = 输入压降调节（多半是线太细/电源弱） */
#define AXP2101_ST2_CHG_STAT_MASK 0x07u    /* bit2:0 充电阶段 */

typedef struct {
    uint8_t  st1;
    uint8_t  st2;
    uint8_t  chg_ctrl;      /* 0x18 原值，仅参考 */
    bool     vbus_good;
    bool     bat_present;
    bool     batfet_open;
    bool     therm_regu;
    bool     vindpm;
    uint8_t  cur_dir;       /* 0 待机 1 充电 2 放电 3 保留 */
    uint8_t  chg_stat;      /* 0 涓流 1 预充 2 恒流 3 恒压 4 充满 5 未充 6/7 保留 */
    uint16_t vbat_mv;
    uint16_t vsys_mv;
    uint16_t vbus_mv;
    uint8_t  pct;           /* 0..100；0xFF = 读不到（燃料计未使能） */
} axp2101_batt_t;

/* ---------------- 充电/电源通路寄存器（测试用例直接解码用） ----------------
 *
 * 地址与位义来自 AXP2101_Datasheet_V1.4 第 6.13.2 节寄存器描述，
 * 以及三份全志应用文档（充电高低温保护验证 / 掉电重启排查指南 / 常见问题）。
 * 这些宏只给测试和诊断读，驱动本身不去写它们 —— 写错会掉电或停充。
 */
#define AXP2101_REG_COMMON_CFG    0x10   /* bit2 PWRON 长按16s强制关机 bit1 复位SoC bit0 软关机 */
#define AXP2101_REG_BATFET_CTRL   0x12   /* bit3 BATFET 在"仅电池+关机"时是否使能 */
#define AXP2101_REG_DIE_OTP       0x13   /* bit2:1 过温保护档  bit0 检测使能 */
#define AXP2101_REG_VSYS_DPM      0x14   /* bit6:4 线性充电 VSYS DPM 4.1+N*0.1V */
#define AXP2101_REG_VINDPM        0x15   /* bit3:0 输入压降门限 3.88+N*0.08V */
#define AXP2101_REG_ILIM          0x16   /* bit2:0 输入限流 100/500/900/1000/1500/2000mA */
#define AXP2101_REG_PWRON_STAT    0x20   /* 上一次开机源（只读，掉电不丢） */
#define AXP2101_REG_PWROFF_STAT   0x21   /* 上一次关机源（只读，掉电不丢） */
#define AXP2101_REG_PWROFF_EN     0x22   /* bit2 过温关机使能 bit1 长按关机 bit0 关机后重启 */
#define AXP2101_REG_DCDC_OVUV     0x23   /* DCDC 过压/欠压是否关机 */
#define AXP2101_REG_PWROFF_VBAT   0x24   /* bit2:0 关机电压 2.6~3.3V */
#define AXP2101_REG_PWROK_CTRL    0x25
#define AXP2101_REG_SLEEP_CTRL    0x26
#define AXP2101_REG_PWRON_TIME    0x27   /* bit3:2 长按关机 4/6/8/10s  bit0:1 短按时间 */
#define AXP2101_REG_TS_CTRL       0x50   /* bit4 TS 是否影响充电  bit1:0 电流源 */
#define AXP2101_REG_TS_HYSL2H     0x52
#define AXP2101_REG_TS_HYSH2L     0x53
#define AXP2101_REG_VLTF_CHG      0x54   /* T1，N*32mV，默认 0x29 ≈ 0℃ */
#define AXP2101_REG_VHTF_CHG      0x55   /* T4，N*2mV，默认 0x58 ≈ 55℃ */
#define AXP2101_REG_VLTF_WORK     0x56
#define AXP2101_REG_VHTF_WORK     0x57
#define AXP2101_REG_JEITA_EN      0x58   /* bit0 JEITA 使能 */
#define AXP2101_REG_JEITA_CV      0x59
#define AXP2101_REG_JEITA_COOL    0x5A   /* T2，N*16mV，默认 0x37 ≈ 10℃ */
#define AXP2101_REG_JEITA_WARM    0x5B   /* T3，N*8mV，默认 0x1E ≈ 45℃ */
#define AXP2101_REG_IPRECHG       0x61   /* bit3:0 预充电流 25*N mA */
#define AXP2101_REG_ICHG          0x62   /* bit4:0 恒流充电 25*N（N<=8）/200+100*(N-8) */
#define AXP2101_REG_ITERM         0x63   /* bit4 终止使能  bit3:0 终止电流 25*N mA */
#define AXP2101_REG_VTERM         0x64   /* bit2:0 恒压点 4.0/4.1/4.2/4.35/4.4V */
#define AXP2101_REG_THERM_REG     0x65   /* bit1:0 过热调节 60/80/100/120℃ */
#define AXP2101_REG_CHG_TIMER     0x67   /* 安全定时器 */
#define AXP2101_REG_BAT_DETECT    0x68   /* bit0 电池检测使能 */
#define AXP2101_REG_GAUGE_CTRL    0xA2   /* bit3 电量计使能位于 0x18，这里只读配置 */

/* REG 0x18 */
#define AXP2101_CHG_EN          (1u << 1)  /* 1 = 允许给单节电池充电 */
#define AXP2101_GAUGE_EN        (1u << 3)  /* 1 = 电量计使能 */

/* REG 0x12 */
#define AXP2101_BATFET_PWROFF_EN (1u << 3) /* 仅电池且关机时是否保持 BATFET 导通 */

/* REG 0x50 */
#define AXP2101_TS_AFFECT_CHG   (1u << 4)  /* 0 = TS 影响充电  1 = TS 只做外部输入 */

/* 电气特性（Datasheet 5.5 节），测试判据要用 */
#define AXP2101_VSLEEPZ_MV      150   /* 进入充电要求 VBUS > VBAT + 该值 */
#define AXP2101_VBUS_UVLOZ_MV   3750  /* VBUS 欠压门限 */
#define AXP2101_VBUS_OV_MV      7000  /* VBUS 过压门限 */
#define AXP2101_VBAT_OVP_MV     4500  /* 电池过压上限（手册 VBAT 工作区间上限） */
#define AXP2101_VBAT_UVLOZ_MV   2400  /* 电池欠压门限 */
#define AXP2101_VBAT_RECHG_MV   100   /* 充满后跌回该值以下才重新充 */

/* ---------------- LDO 电源轨（摄像头供电） ----------------
 *
 * 寄存器地址与位定义来自同款板量产工程 chatbot-esp32-s3_board.cc：
 *   0x90 = LDO 使能，bit1=ALDO2, bit4=BLDO1, bit5=BLDO2
 *   0x93 = ALDO2 电压，0x96 = BLDO1 电压，0x97 = BLDO2 电压
 * 电压编码：低 5 位 N，电压 = 500 + N*100 (mV)，高 3 位保留须保持原值。
 *
 * 注意 0x90 是**多路共用**的使能寄存器（同一字节里还有 ALDO1/3/4、BLDO1/2 等），
 * 所以只能 read-modify-write，整字节写会把别的电源轨一起打开或关掉。
 */
#define AXP2101_REG_LDO_ONOFF0 0x90
#define AXP2101_REG_ALDO2_VOL  0x93
#define AXP2101_REG_BLDO1_VOL  0x96
#define AXP2101_REG_BLDO2_VOL  0x97

#define AXP2101_LDO_ALDO2      (1u << 1)
#define AXP2101_LDO_BLDO1      (1u << 4)
#define AXP2101_LDO_BLDO2      (1u << 5)

#define AXP2101_LDO_MV_MIN     500
#define AXP2101_LDO_MV_MAX     3500
#define AXP2101_LDO_MV_STEP    100

typedef struct {
    bus_i2c_t              *bus;
    i2c_master_dev_handle_t dev;
    uint8_t                 addr;
    bool                    ready;
} axp2101_t;

/* ---------------- 生命周期 ---------------- */

/** 挂到已建好的总线上并校验器件存在（probe 0x34） */
esp_err_t axp2101_init(axp2101_t *d, bus_i2c_t *bus, uint8_t addr7);

/** 摘除器件句柄（不动总线） */
esp_err_t axp2101_deinit(axp2101_t *d);

/* ---------------- 寄存器访问 ---------------- */

esp_err_t axp2101_read(axp2101_t *d, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t axp2101_read8(axp2101_t *d, uint8_t reg, uint8_t *val);
esp_err_t axp2101_write8(axp2101_t *d, uint8_t reg, uint8_t val);

/** 读芯片 ID；失败返回 0xFF */
uint8_t axp2101_chip_id(axp2101_t *d);

/* ---------------- ADC ---------------- */

/** 按位设置/清除 ADC 通道使能（read-modify-write，只动 mask 覆盖的位） */
esp_err_t axp2101_adc_config(axp2101_t *d, uint8_t mask, bool enable);

/** 读 16 位原始值（高字节在前） */
esp_err_t axp2101_read_raw16(axp2101_t *d, uint8_t reg_h, uint16_t *raw);

/**
 * 读电压，单位 mV。
 *
 * 换算依据（实测校准，不是照抄手册）：本板 USB 供电时寄存器 0x38/0x39 读到
 * 0x1449 = 5193，实测 USB 5 V 轨；0x3A/0x3B 为 0x1454 = 5204。
 * 即"16 位原值、LSB = 1 mV"。
 * 曾按某库的"14 位高 8 低 6 + 1.25 mV/LSB"换算，得到 1.6 V，明显不成立。
 */
esp_err_t axp2101_read_mv(axp2101_t *d, uint8_t reg_h, uint16_t *mv);

/**
 * 读芯片温度通道原始值。
 * 只给原值：该通道的线性换算系数未经校准验证，直接套公式会输出
 * 诸如 -574 ℃ 的假数据，作为测试框架宁可"不解释"也不"编数据"。
 */
esp_err_t axp2101_read_temp_raw(axp2101_t *d, uint16_t *raw);

/** 读中断状态三字节（不清理，只观察） */
esp_err_t axp2101_read_irq_status(axp2101_t *d, uint8_t out[AXP2101_REG_IRQ_STAT_N]);

/* ---------------- 电池与充电状态 ---------------- */

/**
 * 读一次电池与充电状态。
 *
 * 内部会按需打开 VBAT/VSYS/VBUS 三路 ADC（只置位，不清除已有通道），
 * 然后等一个转换周期再取数。读的全是只读状态，不写任何配置位。
 *
 * 用途是回答"电池插着没有 / 在不在充电 / 充到哪个阶段"，
 * 不做电量库仑积分，也不碰充电电流与终止电压的配置。
 */
esp_err_t axp2101_read_batt(axp2101_t *d, axp2101_batt_t *out);

/* ---------------- LDO 电源轨 ---------------- */

/**
 * 设置某路 LDO 的输出电压（read-modify-write，只动低 5 位，高 3 位保留）。
 *
 * @param reg  只能是 AXP2101_REG_ALDO2_VOL / BLDO1_VOL / BLDO2_VOL；
 *             传其他值直接返回 ESP_ERR_INVALID_ARG —— 防止误改 DCDC 导致掉电。
 * @param mv   500..3500，按 100 mV 取整（余数向下取整，并打日志说明）。
 *
 * **不会自动使能**：OV2640 的 DVDD 只有 1.2 V，而本板冷启动时该寄存器停在
 * 2.8 V 档。必须先设电压、再使能，顺序反了会让模组以错误电压上电。
 */
esp_err_t axp2101_ldo_set_mv(axp2101_t *d, uint8_t reg, uint16_t mv);

/** 读回某路 LDO 的设定电压（mV，按编码换算，非实测） */
esp_err_t axp2101_ldo_get_mv(axp2101_t *d, uint8_t reg, uint16_t *mv);

/** 按位使能/关闭 LDO（只动 mask 覆盖的位，其他电源轨不受影响） */
esp_err_t axp2101_ldo_enable(axp2101_t *d, uint8_t mask, bool on);

/** 查询 mask 内的位是否**全部**已使能 */
esp_err_t axp2101_ldo_is_on(axp2101_t *d, uint8_t mask, bool *on);

/** 读 0x90 原始字节（排错用：想知道同一字节里还有哪些轨被打开） */
esp_err_t axp2101_ldo_raw_onoff(axp2101_t *d, uint8_t *raw);

/** 原始寄存器转储 0x00..0x4F，用于人工核对，不做任何解释 */
void axp2101_dump(axp2101_t *d);

#ifdef __cplusplus
}
#endif
