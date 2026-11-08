/**
 * @file  es8311.c
 * @brief ES8311 驱动实现
 */
#include "es8311.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8311";

/* ES8311 在个别板子上第一次 I2C 写会失败（官方驱动也为此连写两次），
 * 这里把超时放得比 PMIC 宽一点，并让调用方按需重试。 */
#define ES8311_XFER_TIMEOUT_MS 100

esp_err_t es8311_init(es8311_t *d, bus_i2c_t *bus, uint8_t addr7)
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
        ESP_LOGE(TAG, "0x%02X 无应答（器件未贴片 / 未上电 / 引脚不对 / 上拉缺失）",
                 addr7);
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

esp_err_t es8311_deinit(es8311_t *d)
{
    if (d == NULL || d->dev == NULL) {
        return ESP_OK;
    }
    esp_err_t err = i2c_master_bus_rm_device(d->dev);
    d->dev   = NULL;
    d->ready = false;
    return err;
}

esp_err_t es8311_read(es8311_t *d, uint8_t reg, uint8_t *buf, size_t len)
{
    if (d == NULL || !d->ready || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(d->dev, &reg, 1, buf, len,
                                       ES8311_XFER_TIMEOUT_MS);
}

esp_err_t es8311_read8(es8311_t *d, uint8_t reg, uint8_t *val)
{
    return es8311_read(d, reg, val, 1);
}

esp_err_t es8311_write8(es8311_t *d, uint8_t reg, uint8_t val)
{
    if (d == NULL || !d->ready) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(d->dev, buf, sizeof(buf),
                                        ES8311_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        /* 官方驱动提到首写偶发失败，这里补一次重试再判失败 */
        vTaskDelay(pdMS_TO_TICKS(2));
        err = i2c_master_transmit(d->dev, buf, sizeof(buf),
                                  ES8311_XFER_TIMEOUT_MS);
    }
    return err;
}

uint8_t es8311_reset_reg(es8311_t *d)
{
    uint8_t v = 0xFF;
    if (es8311_read8(d, ES8311_REG_RESET00, &v) != ESP_OK) {
        return 0xFF;
    }
    return v;
}

void es8311_dump(es8311_t *d)
{
    if (d == NULL || !d->ready) {
        printf("        | 器件未就绪，无法转储\n");
        return;
    }
    for (uint16_t base = 0x00; base <= 0xF0; base += 16) {
        printf("        | %02X:", (unsigned)base);
        for (uint16_t off = 0; off < 16; off++) {
            uint8_t v = 0;
            if (es8311_read8(d, (uint8_t)(base + off), &v) == ESP_OK) {
                printf(" %02X", v);
            } else {
                printf(" --");
            }
        }
        printf("\n");
    }
}
