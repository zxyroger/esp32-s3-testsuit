/**
 * @file  cam_ov2640.c
 * @brief OV2640 板级封装实现（供电 / XCLK / PWDN / SCCB）
 */
#include "cam_ov2640.h"

#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cam";

/* SCCB 单次传输超时：100 kHz 下 2 字节不到 0.2 ms，100 ms 是很宽松的上限。
 * 传感器没接时这里会真的等满 100 ms 再报超时，不会无限挂住。 */
#define SCCB_TIMEOUT_MS 100

/* OV2640 的 bank 切换寄存器 */
#define OV2640_REG_BANK_SEL 0xFF
#define OV2640_BANK_SENSOR  0x01

/* sensor bank 里的 ID 寄存器 */
#define OV2640_REG_PID      0x0A
#define OV2640_REG_VER      0x0B
#define OV2640_REG_MIDH     0x1C
#define OV2640_REG_MIDL     0x1D

/* board_config.h 里存的是数字（配置头不引驱动），这里转成 LEDC 枚举 */
#define CAM_LEDC_TIMER   ((ledc_timer_t)BOARD_CAM_LEDC_TIMER)
#define CAM_LEDC_CHANNEL ((ledc_channel_t)BOARD_CAM_LEDC_CHANNEL)

/* ---------------- XCLK ---------------- */

esp_err_t cam_xclk_start(cam_t *c)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ledc_timer_config_t timer;
    memset(&timer, 0, sizeof(timer));
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = CAM_LEDC_TIMER;
    timer.freq_hz = BOARD_CAM_XCLK_HZ;
    timer.duty_resolution = (ledc_timer_bit_t)BOARD_CAM_XCLK_PWM_BITS;
    timer.clk_cfg = LEDC_AUTO_CLK;

    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "XCLK 定时器配置失败: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.gpio_num = BOARD_CAM_XCLK_GPIO;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = CAM_LEDC_CHANNEL;
    ch.timer_sel = CAM_LEDC_TIMER;
    ch.duty = BOARD_CAM_XCLK_DUTY;
    ch.hpoint = 0;

    err = ledc_channel_config(&ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "XCLK 通道配置失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 读回实际频率：LEDC 按 clk/(2^bits*div) 整数分频，
     * 若分辨率选得不巧，10 MHz 会落在 9.5 或 11 MHz 上，传感器可能就认不到。 */
    c->xclk_hz = ledc_get_freq(LEDC_LOW_SPEED_MODE, CAM_LEDC_TIMER);
    c->xclk_ok = true;

    ESP_LOGI(TAG, "XCLK 已输出：GPIO%d，目标 %u Hz，实测 %u Hz（%d bit 分辨率）",
             BOARD_CAM_XCLK_GPIO, (unsigned)BOARD_CAM_XCLK_HZ,
             (unsigned)c->xclk_hz, BOARD_CAM_XCLK_PWM_BITS);
    return ESP_OK;
}

esp_err_t cam_xclk_stop(cam_t *c)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 把占空比打成 0 即停止翻转；不调 ledc_stop，避免把 GPIO 交回普通模式后
     * 被别的模块抢去。 */
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, CAM_LEDC_CHANNEL, 0);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, CAM_LEDC_CHANNEL);
    }
    c->xclk_ok = false;
    return err;
}

/* ---------------- PWDN ---------------- */

static esp_err_t pwdn_config_output(void)
{
    if (BOARD_CAM_PWDN_GPIO < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    gpio_config_t io;
    memset(&io, 0, sizeof(io));
    io.pin_bit_mask = 1ULL << BOARD_CAM_PWDN_GPIO;
    /* 用 INPUT_OUTPUT 而不是纯 OUTPUT：纯输出模式下 gpio_get_level 读回恒 0
     * （音频组 PA 脚踩过这个坑），那样就无法证明电平真的翻转了。 */
    io.mode = GPIO_MODE_INPUT_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    /* 上电前先保持掉电（PWDN 高），避免 LDO 还没配好就有电流灌进模组 */
    return gpio_set_level(BOARD_CAM_PWDN_GPIO, 1);
}

esp_err_t cam_pwdn(cam_t *c, bool power_down)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!c->pwdn_ok) {
        esp_err_t err = pwdn_config_output();
        if (err != ESP_OK) {
            return err;
        }
        c->pwdn_ok = true;
    }
    /* OV2640 的 PWDN 是高电平掉电、低电平工作 */
    int level = power_down ? 1 : 0;
    esp_err_t err = gpio_set_level(BOARD_CAM_PWDN_GPIO, level);
    if (err == ESP_OK) {
        c->pwdn_level = (uint8_t)level;
    }
    return err;
}

esp_err_t cam_pwdn_read(cam_t *c, int *level)
{
    if (c == NULL || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!c->pwdn_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    *level = gpio_get_level(BOARD_CAM_PWDN_GPIO);
    return ESP_OK;
}

/* ---------------- 供电 ---------------- */

esp_err_t cam_power_read_mv(axp2101_t *pmic, uint16_t *aldo2, uint16_t *bldo1,
                            uint16_t *bldo2)
{
    if (pmic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t a = 0, b = 0, c = 0;
    esp_err_t err = axp2101_ldo_get_mv(pmic, AXP2101_REG_ALDO2_VOL, &a);
    if (err == ESP_OK) {
        err = axp2101_ldo_get_mv(pmic, AXP2101_REG_BLDO1_VOL, &b);
    }
    if (err == ESP_OK) {
        err = axp2101_ldo_get_mv(pmic, AXP2101_REG_BLDO2_VOL, &c);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (aldo2) *aldo2 = a;
    if (bldo1) *bldo1 = b;
    if (bldo2) *bldo2 = c;
    return ESP_OK;
}

esp_err_t cam_power_is_on(axp2101_t *pmic, bool *on)
{
    if (pmic == NULL || on == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t mask = AXP2101_LDO_ALDO2 | AXP2101_LDO_BLDO1 | AXP2101_LDO_BLDO2;
    return axp2101_ldo_is_on(pmic, mask, on);
}

esp_err_t cam_power_on(cam_t *c, axp2101_t *pmic, uint32_t settle_ms)
{
    if (c == NULL || pmic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t mask = AXP2101_LDO_ALDO2 | AXP2101_LDO_BLDO1 | AXP2101_LDO_BLDO2;

    /* 第零步：先关断三路。
     *
     * 这一步是实测发现后补上的，很关键：本板上电后 0x90 的实测值是 0x77，
     * 也就是 ALDO2/BLDO1/BLDO2 **默认就是使能状态**。既然使能位已经置上，
     * 单纯"先写电压寄存器再写使能位"根本挡不住 —— 电压是在带电状态下
     * 被改掉的，DVDD 会从 2.8 V 直接跳到 1.2 V。
     *
     * 量产工程写的是"先设电压、再使能"，那是假设冷启动时电压是默认值且
     * 使能位为 0。本板实测不成立，所以这里显式断电 -> 设电压 -> 再上电，
     * 保证模组的 DVDD 第一次见到电就是 1.2 V。 */
    esp_err_t err = axp2101_ldo_enable(pmic, mask, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "预关断三路 LDO 失败: %s", esp_err_to_name(err));
        return err;
    }
    c->power_ok = false;
    vTaskDelay(pdMS_TO_TICKS(5)); /* 让输出电容放电，确保下一次是真正的"重新上电" */

    /* 第一步：三路电压**逐一**设好并回读核对。 */
    struct {
        uint8_t  reg;
        uint16_t mv;
        const char *name;
    } rails[] = {
        { AXP2101_REG_ALDO2_VOL, BOARD_CAM_PMIC_ALDO2_MV, "ALDO2 (VDDCAM_3V3)" },
        { AXP2101_REG_BLDO1_VOL, BOARD_CAM_PMIC_BLDO1_MV, "BLDO1 (AVDD)" },
        { AXP2101_REG_BLDO2_VOL, BOARD_CAM_PMIC_BLDO2_MV, "BLDO2 (DVDD)" },
    };

    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
        esp_err_t err = axp2101_ldo_set_mv(pmic, rails[i].reg, rails[i].mv);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s 设 %u mV 失败: %s", rails[i].name, rails[i].mv,
                     esp_err_to_name(err));
            return err;
        }
        uint16_t back = 0;
        err = axp2101_ldo_get_mv(pmic, rails[i].reg, &back);
        if (err != ESP_OK) {
            return err;
        }
        if (back != rails[i].mv) {
            ESP_LOGE(TAG, "%s 目标 %u mV，回读 %u mV", rails[i].name, rails[i].mv,
                     back);
            /* 电压不对就**绝不**使能：让模组以错误电压上电可能直接损坏 */
            return ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGI(TAG, "%s = %u mV", rails[i].name, back);
    }

    /* 第二步：一次性使能三路（0x90 是多路共用字节，内部 read-modify-write） */
    err = axp2101_ldo_enable(pmic, mask, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO 使能失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 第三步：等 LDO 稳定。不等就跑 SCCB，探测会随机失败。 */
    uint32_t wait = (settle_ms > 0) ? settle_ms : BOARD_CAM_POWER_SETTLE_MS;
    vTaskDelay(pdMS_TO_TICKS(wait));

    bool on = false;
    err = cam_power_is_on(pmic, &on);
    if (err != ESP_OK) {
        return err;
    }
    if (!on) {
        ESP_LOGE(TAG, "使能后回读 0x90，三路未全部置位");
        return ESP_ERR_INVALID_STATE;
    }

    c->power_ok = true;
    ESP_LOGI(TAG, "摄像头供电就绪（等待 %u ms）", (unsigned)wait);
    return ESP_OK;
}

esp_err_t cam_power_off(cam_t *c, axp2101_t *pmic)
{
    if (pmic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 先断电前让模组进掉电态，再切 LDO：
     * 反过来的话传感器还在工作时供电消失，DVP 数据线上可能出现倒灌。 */
    if (c != NULL && c->pwdn_ok) {
        cam_pwdn(c, true);
    }
    const uint8_t mask = AXP2101_LDO_ALDO2 | AXP2101_LDO_BLDO1 | AXP2101_LDO_BLDO2;
    esp_err_t err = axp2101_ldo_enable(pmic, mask, false);
    if (c != NULL) {
        c->power_ok = false;
    }
    return err;
}

/* ---------------- SCCB ---------------- */

static esp_err_t sccb_attach(cam_t *c, bus_i2c_t *bus)
{
    if (c->sccb_ok) {
        return ESP_OK;
    }
    if (bus == NULL || !bus->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_CAM_SCCB_ADDR,
        .scl_speed_hz = bus->hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus->bus, &cfg, &c->sccb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载 SCCB(0x%02X) 失败: %s", BOARD_CAM_SCCB_ADDR,
                 esp_err_to_name(err));
        return err;
    }
    c->sccb_ok = true;
    return ESP_OK;
}

bool cam_sccb_present(cam_t *c)
{
    if (c == NULL || !c->sccb_ok) {
        return false;
    }
    /* 用一个无副作用的读操作判断应答：只发地址 + 寄存器号，不需要数据回来。
     * 注意不能用 i2c_master_probe 之后就当"传感器好"——地址 ACK 只说明
     * 有器件应答，PID 对不对是另一回事（见 cam.read_id）。 */
    uint8_t reg = OV2640_REG_BANK_SEL;
    uint8_t val = 0;
    return i2c_master_transmit_receive(c->sccb, &reg, 1, &val, 1,
                                       SCCB_TIMEOUT_MS) == ESP_OK;
}

esp_err_t cam_reg_read(cam_t *c, uint8_t reg, uint8_t *val)
{
    if (c == NULL || !c->sccb_ok || val == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(c->sccb, &reg, 1, val, 1, SCCB_TIMEOUT_MS);
}

esp_err_t cam_reg_write(cam_t *c, uint8_t reg, uint8_t val)
{
    if (c == NULL || !c->sccb_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(c->sccb, buf, sizeof(buf), SCCB_TIMEOUT_MS);
}

esp_err_t cam_read_id(cam_t *c, cam_id_t *id)
{
    if (c == NULL || id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 必须切到 sensor bank：0x0A/0x0B/0x1C/0x1D 只在 bank1 里是 ID。
     * bank0 里 0x0A 是别的功能寄存器，读出来的数会被误判成"型号不认识"。 */
    esp_err_t err = cam_reg_write(c, OV2640_REG_BANK_SEL, OV2640_BANK_SENSOR);
    if (err != ESP_OK) {
        return err;
    }
    /* bank 切换后给一个极短的建立时间。手册没要求，但 SCCB 背靠背读
     * 有实测偶发读到旧 bank 值的先例，1 ms 成本可以忽略。 */
    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t regs[4] = { OV2640_REG_PID, OV2640_REG_VER, OV2640_REG_MIDH,
                        OV2640_REG_MIDL };
    uint8_t vals[4] = { 0 };
    for (int i = 0; i < 4; i++) {
        err = cam_reg_read(c, regs[i], &vals[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "读寄存器 0x%02X 失败: %s", regs[i], esp_err_to_name(err));
            return err;
        }
    }
    id->pid = vals[0];
    id->ver = vals[1];
    id->midh = vals[2];
    id->midl = vals[3];
    return ESP_OK;
}

/* ---------------- 生命周期 ---------------- */

esp_err_t cam_prepare(cam_t *c, bus_i2c_t *bus)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(c, 0, sizeof(*c));
    c->pwdn_level = 1;

    esp_err_t err = pwdn_config_output();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWDN(GPIO%d) 配置失败: %s", BOARD_CAM_PWDN_GPIO,
                 esp_err_to_name(err));
        return err;
    }
    c->pwdn_ok = true;

    err = sccb_attach(c, bus);
    if (err != ESP_OK) {
        return err;
    }

    return cam_xclk_start(c);
}

/* 名字必须叫 cam_release 而不是 cam_deinit：
 * esp32-camera 组件的 cam_hal.c 里有同名全局函数 cam_deinit()，
 * 链接期会报 multiple definition。本工程的这个动作语义是"释放
 * SCCB 句柄与 XCLK"，而组件的那个是"拆除采集控制器"，本来也不是一回事。 */
esp_err_t cam_release(cam_t *c)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = cam_xclk_stop(c);
    if (c->sccb_ok) {
        i2c_master_bus_rm_device(c->sccb);
        c->sccb = NULL;
        c->sccb_ok = false;
    }
    if (c->pwdn_ok) {
        /* 留在掉电态，别让传感器悬着耗电 */
        gpio_set_level(BOARD_CAM_PWDN_GPIO, 1);
        c->pwdn_level = 1;
    }
    return err;
}
