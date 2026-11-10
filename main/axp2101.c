/**
 * @file  axp2101.c
 * @brief AXP2101 驱动实现
 */
#include "axp2101.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "axp2101";

#define AXP_XFER_TIMEOUT_MS 50

esp_err_t axp2101_init(axp2101_t *d, bus_i2c_t *bus, uint8_t addr7)
{
    if (d == NULL || bus == NULL || !bus->ready) {
        return ESP_ERR_INVALID_ARG;
    }
    d->bus   = bus;
    d->addr  = addr7;
    d->ready = false;

    /* 先 probe：地址不应答时 add_device 也能成功，但后续每次传输都会超时，
     * 早失败能把错误定位在"器件不在总线上"而不是"读写失败"。 */
    if (!bus_i2c_probe(bus, addr7)) {
        ESP_LOGE(TAG, "0x%02X 无应答（器件未上电 / 引脚不对 / 上拉缺失）", addr7);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr7,
        .scl_speed_hz    = bus->hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus->bus, &cfg, &d->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载器件失败: %s", esp_err_to_name(err));
        return err;
    }
    d->ready = true;
    return ESP_OK;
}

esp_err_t axp2101_deinit(axp2101_t *d)
{
    if (d == NULL || d->dev == NULL) {
        return ESP_OK;
    }
    esp_err_t err = i2c_master_bus_rm_device(d->dev);
    d->dev   = NULL;
    d->ready = false;
    return err;
}

esp_err_t axp2101_read(axp2101_t *d, uint8_t reg, uint8_t *buf, size_t len)
{
    if (d == NULL || !d->ready || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(d->dev, &reg, 1, buf, len,
                                       AXP_XFER_TIMEOUT_MS);
}

esp_err_t axp2101_read8(axp2101_t *d, uint8_t reg, uint8_t *val)
{
    return axp2101_read(d, reg, val, 1);
}

esp_err_t axp2101_write8(axp2101_t *d, uint8_t reg, uint8_t val)
{
    if (d == NULL || !d->ready) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(d->dev, buf, sizeof(buf), AXP_XFER_TIMEOUT_MS);
}

uint8_t axp2101_chip_id(axp2101_t *d)
{
    uint8_t v = 0xFF;
    if (axp2101_read8(d, AXP2101_REG_CHIP_ID, &v) != ESP_OK) {
        return 0xFF;
    }
    return v;
}

esp_err_t axp2101_adc_config(axp2101_t *d, uint8_t mask, bool enable)
{
    uint8_t cur = 0;
    esp_err_t err = axp2101_read8(d, AXP2101_REG_ADC_EN, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t next = enable ? (cur | mask) : (uint8_t)(cur & ~mask);
    if (next == cur) {
        return ESP_OK;                   /* 已经是目标状态，不必产生总线写 */
    }
    return axp2101_write8(d, AXP2101_REG_ADC_EN, next);
}

esp_err_t axp2101_read_raw16(axp2101_t *d, uint8_t reg_h, uint16_t *raw)
{
    if (raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[2] = { 0, 0 };
    esp_err_t err = axp2101_read(d, reg_h, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    *raw = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
    return ESP_OK;
}

esp_err_t axp2101_read_mv(axp2101_t *d, uint8_t reg_h, uint16_t *mv)
{
    if (mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t raw = 0;
    esp_err_t err = axp2101_read_raw16(d, reg_h, &raw);
    if (err != ESP_OK) {
        return err;
    }
    /* LSB = 1 mV，原值即毫伏。见 axp2101.h 里对该换算的实测依据说明。 */
    *mv = raw;
    return ESP_OK;
}

esp_err_t axp2101_read_temp_raw(axp2101_t *d, uint16_t *raw)
{
    if (raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp2101_read_raw16(d, AXP2101_REG_ADC_TDIE_H, raw);
}

esp_err_t axp2101_read_irq_status(axp2101_t *d, uint8_t out[AXP2101_REG_IRQ_STAT_N])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp2101_read(d, AXP2101_REG_IRQ_STAT0, out, AXP2101_REG_IRQ_STAT_N);
}

/* ---------------- 电池与充电状态 ---------------- */

esp_err_t axp2101_read_batt(axp2101_t *d, axp2101_batt_t *out)
{
    if (d == NULL || !d->ready || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(out, 0, sizeof(*out));
    out->pct = 0xFF;

    /* 0x00 / 0x01 相邻，一次事务读两字节，避免两次读之间状态已经翻变 */
    uint8_t st[2] = { 0, 0 };
    esp_err_t err = axp2101_read(d, AXP2101_REG_STATUS1, st, sizeof(st));
    if (err != ESP_OK) {
        return err;
    }
    out->st1 = st[0];
    out->st2 = st[1];

    out->vbus_good  = (st[0] & AXP2101_ST1_VBUS_GOOD) != 0;
    out->batfet_open = (st[0] & AXP2101_ST1_BATFET_OPEN) != 0;
    out->bat_present = (st[0] & AXP2101_ST1_BAT_PRESENT) != 0;
    out->therm_regu  = (st[0] & AXP2101_ST1_THERM_REGU) != 0;
    out->cur_dir     = (uint8_t)((st[1] & AXP2101_ST2_CUR_DIR_MASK) >> 5);
    out->vindpm      = (st[1] & AXP2101_ST2_VINDPM) != 0;
    out->chg_stat    = (uint8_t)(st[1] & AXP2101_ST2_CHG_STAT_MASK);

    /* 0x18 只观察：里面是充电/电量计/看门狗的总开关，写错会停充，绝不回写 */
    axp2101_read8(d, AXP2101_REG_CHG_CTRL, &out->chg_ctrl);

    /* ADC 通道不一定是开着的（冷启动默认关，只有跑过测试用例才会被打开），
     * 这里补开一次；本来开着就只补一个转换周期的等待。 */
    const uint8_t need = AXP2101_ADC_EN_VBAT | AXP2101_ADC_EN_VBUS |
                         AXP2101_ADC_EN_VSYS;
    uint8_t en = 0;
    bool just_on = false;
    if (axp2101_read8(d, AXP2101_REG_ADC_EN, &en) == ESP_OK && (en & need) != need) {
        if (axp2101_adc_config(d, need, true) == ESP_OK) {
            just_on = true;
        }
    }
    if (just_on) {
        /* 三通道轮流转一次，手册没给确切时间，实测 30 ms 足够，留 40 ms 余量 */
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    /* 单路读失败不返回错误：状态位已经拿到了，电压缺失只是少几行信息，
     * 让调用方自己看 0 值判断，比整个函数失败更有用。 */
    (void)axp2101_read_mv(d, AXP2101_REG_ADC_VBAT_H, &out->vbat_mv);
    (void)axp2101_read_mv(d, AXP2101_REG_ADC_VSYS_H, &out->vsys_mv);
    (void)axp2101_read_mv(d, AXP2101_REG_ADC_VBUS_H, &out->vbus_mv);

    /* VBUS 通道没人接时会饱和到 ADC 满量程，不是 0。
     * 实测：USB-C 拔掉、纯电池运行时读回 16370 mV（0x3FF2，14 位满量程 0x3FFF
     * 的 99.9%），此时 ST1 的 VBUS_GOOD=0 说明 PMIC 自己也知道输入不存在。
     * 这个值的成因是 VBUS 采样脚悬空后拉到参考电平，属于芯片行为而非接线错误；
     * 而 USB 输入的物理上限就是 5 V（本板没有 PD 诱骗，拿不到 9/15/20 V），
     * 所以超过 6500 mV 一律判为"无输入"，报 0，避免上层把 16 V 当真。
     * 注意不能简单用 vbus_good 门控：USB 在插着、但 VBUS < VBAT+250 mV 时
     * vbus_good 也是 0，那种情况下 5 V 读数是有意义的，要保留。 */
    if (out->vbus_mv > AXP2101_VBUS_SANE_MAX_MV) {
        out->vbus_mv = 0;
    }

    /* 燃料计百分比：没使能时该寄存器可能保留旧值或为 0，超过 100 一律当无效 */
    uint8_t pct = 0;
    if (axp2101_read8(d, AXP2101_REG_BAT_PCT, &pct) == ESP_OK && pct <= 100) {
        out->pct = pct;
    }
    return ESP_OK;
}

void axp2101_dump(axp2101_t *d)
{
    if (d == NULL || !d->ready) {
        printf("        | 器件未就绪，无法转储\n");
        return;
    }
    /* 0x00~0x6A：状态、配置、充电、TS/JEITA 全在这段。
     * 只读，不解释 —— 解释放在测试用例里（那里的判据有文档出处）。 */
    for (uint16_t base = 0x00; base <= 0x60; base += 16) {
        printf("        | %02X:", (unsigned)base);
        for (uint16_t off = 0; off < 16; off++) {
            uint8_t v = 0;
            if (axp2101_read8(d, (uint8_t)(base + off), &v) == ESP_OK) {
                printf(" %02X", v);
            } else {
                printf(" --");
            }
        }
        printf("\n");
    }
    /* 0x90~0x97 是 LDO 使能与电压，不在上面那段范围内，单独打一行 */
    printf("        | 90:");
    for (uint8_t reg = AXP2101_REG_LDO_ONOFF0; reg <= AXP2101_REG_BLDO2_VOL; reg++) {
        uint8_t v = 0;
        if (axp2101_read8(d, reg, &v) == ESP_OK) {
            printf(" %02X", v);
        } else {
            printf(" --");
        }
    }
    printf("   (LDO onoff + ALDO2/BLDO1/BLDO2 电压)\n");
}

/* ---------------- LDO 电源轨 ---------------- */

/** 三道门都过了才允许写：句柄就绪、寄存器在白名单内、电压在合法区间 */
static bool ldo_reg_allowed(uint8_t reg)
{
    return reg == AXP2101_REG_ALDO2_VOL || reg == AXP2101_REG_BLDO1_VOL ||
           reg == AXP2101_REG_BLDO2_VOL;
}

esp_err_t axp2101_ldo_set_mv(axp2101_t *d, uint8_t reg, uint16_t mv)
{
    if (d == NULL || !d->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ldo_reg_allowed(reg)) {
        /* 白名单外一律拒绝：这几个字节里 0x92/0x94/0x95 分别是 ALDO1/3/4，
         * 0x9x 一带还有 DCDC 相关，误写会直接掉电，不能"顺手"支持。 */
        ESP_LOGE(TAG, "拒绝写入 0x%02X：只允许 ALDO2(0x93)/BLDO1(0x96)/BLDO2(0x97)",
                 reg);
        return ESP_ERR_INVALID_ARG;
    }
    if (mv < AXP2101_LDO_MV_MIN || mv > AXP2101_LDO_MV_MAX) {
        ESP_LOGE(TAG, "电压 %u mV 超出 [%d, %d]", mv, AXP2101_LDO_MV_MIN,
                 AXP2101_LDO_MV_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t n = (uint16_t)((mv - AXP2101_LDO_MV_MIN) / AXP2101_LDO_MV_STEP);
    uint16_t actual = (uint16_t)(AXP2101_LDO_MV_MIN + n * AXP2101_LDO_MV_STEP);
    if (actual != mv) {
        /* 只打日志不报错：调用方要的是"就近的一档"，不是精确值 */
        ESP_LOGW(TAG, "%u mV 不是 100 mV 整数倍，落到 %u mV", mv, actual);
    }

    uint8_t cur = 0;
    esp_err_t err = axp2101_read8(d, reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    /* 高 3 位保留：AXP2101 的 LDO 电压寄存器只有低 5 位有用，其余位原样写回 */
    uint8_t target = (uint8_t)((cur & 0xE0) | (n & 0x1F));
    if (target == cur) {
        return ESP_OK;
    }
    err = axp2101_write8(d, reg, target);
    if (err != ESP_OK) {
        return err;
    }

    /* 写后回读：I2C 写成功不代表寄存器真的接受了（有些位是只读或需要解锁） */
    uint8_t back = 0;
    err = axp2101_read8(d, reg, &back);
    if (err != ESP_OK) {
        return err;
    }
    if (back != target) {
        ESP_LOGE(TAG, "0x%02X 写 0x%02X 后回读 0x%02X，未生效", reg, target, back);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t axp2101_ldo_get_mv(axp2101_t *d, uint8_t reg, uint16_t *mv)
{
    if (d == NULL || !d->ready || mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ldo_reg_allowed(reg)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t v = 0;
    esp_err_t err = axp2101_read8(d, reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    *mv = (uint16_t)(AXP2101_LDO_MV_MIN + (v & 0x1F) * AXP2101_LDO_MV_STEP);
    return ESP_OK;
}

esp_err_t axp2101_ldo_enable(axp2101_t *d, uint8_t mask, bool on)
{
    if (d == NULL || !d->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 只允许动这三路，其他位（ALDO1/3/4、BLDO1..）一律不许碰 */
    const uint8_t allowed = AXP2101_LDO_ALDO2 | AXP2101_LDO_BLDO1 | AXP2101_LDO_BLDO2;
    if (mask == 0 || (mask & (uint8_t)~allowed) != 0) {
        ESP_LOGE(TAG, "使能掩码 0x%02X 含非摄像头电源位（允许 0x%02X）", mask, allowed);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t cur = 0;
    esp_err_t err = axp2101_read8(d, AXP2101_REG_LDO_ONOFF0, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t target = on ? (uint8_t)(cur | mask) : (uint8_t)(cur & (uint8_t)~mask);
    if (target == cur) {
        return ESP_OK;
    }
    return axp2101_write8(d, AXP2101_REG_LDO_ONOFF0, target);
}

esp_err_t axp2101_ldo_is_on(axp2101_t *d, uint8_t mask, bool *on)
{
    if (d == NULL || !d->ready || on == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t cur = 0;
    esp_err_t err = axp2101_read8(d, AXP2101_REG_LDO_ONOFF0, &cur);
    if (err != ESP_OK) {
        return err;
    }
    *on = (cur & mask) == mask;
    return ESP_OK;
}

esp_err_t axp2101_ldo_raw_onoff(axp2101_t *d, uint8_t *raw)
{
    if (d == NULL || !d->ready || raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp2101_read8(d, AXP2101_REG_LDO_ONOFF0, raw);
}
