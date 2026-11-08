/**
 * @file  bus_i2c.c
 * @brief I2C 主机总线封装实现
 */
#include "bus_i2c.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bus_i2c";

/* 单次传输超时。I2C 400k 下读 1 字节约 25 us，50 ms 已是三个数量级的余量；
 * 卡住说明总线被拉死（器件没上电 / 上拉缺失 / SDA 被短接），快失败好过挂住。 */
#define I2C_XFER_TIMEOUT_MS 50

esp_err_t bus_i2c_begin(bus_i2c_t *b, int sda, int scl, uint32_t hz)
{
    if (b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(b, 0, sizeof(*b));
    b->sda = sda;
    b->scl = scl;
    b->hz  = hz;

    i2c_master_bus_config_t cfg = {
        .i2c_port          = -1,          /* -1 = 由驱动自动挑选可用控制器 */
        .sda_io_num        = sda,
        .scl_io_num        = scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,           /* 7 个时钟周期的毛刺过滤，板间干扰常见 */
        .intr_priority     = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &b->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "建总线失败 SDA=%d SCL=%d: %s", sda, scl, esp_err_to_name(err));
        b->bus = NULL;
        return err;
    }
    b->ready = true;
    ESP_LOGI(TAG, "总线就绪 SDA=%d SCL=%d %" PRIu32 " Hz", sda, scl, hz);
    return ESP_OK;
}

esp_err_t bus_i2c_end(bus_i2c_t *b)
{
    if (b == NULL || b->bus == NULL) {
        return ESP_OK;
    }
    esp_err_t err = i2c_del_master_bus(b->bus);
    b->bus   = NULL;
    b->ready = false;
    return err;
}

bool bus_i2c_probe(bus_i2c_t *b, uint8_t addr7)
{
    if (b == NULL || !b->ready) {
        return false;
    }
    return i2c_master_probe(b->bus, addr7, I2C_XFER_TIMEOUT_MS) == ESP_OK;
}

esp_err_t bus_i2c_scan(bus_i2c_t *b, uint8_t *out, size_t max, size_t *count)
{
    if (b == NULL || !b->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t n = 0;
    /* 0x00-0x07 与 0x78-0x7F 是 I2C 保留地址，扫它们只会得到假阳性 */
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(b->bus, addr, I2C_XFER_TIMEOUT_MS) == ESP_OK) {
            if (out != NULL && n < max) {
                out[n] = addr;
            }
            n++;
        }
    }
    if (count != NULL) {
        *count = n;
    }
    return ESP_OK;
}

esp_err_t bus_i2c_autodetect(bus_i2c_t *b, uint8_t addr7,
                             const int8_t (*cand)[2], size_t count,
                             uint32_t hz, int *hit_sda, int *hit_scl)
{
    if (b == NULL || cand == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++) {
        int sda = cand[i][0];
        int scl = cand[i][1];
        if (sda < 0 || scl < 0) {
            continue;
        }
        bus_i2c_t tmp;
        if (bus_i2c_begin(&tmp, sda, scl, hz) != ESP_OK) {
            continue;
        }
        bool hit = bus_i2c_probe(&tmp, addr7);
        if (hit) {
            *b = tmp;                       /* 直接把建好的总线交出去 */
            if (hit_sda) *hit_sda = sda;
            if (hit_scl) *hit_scl = scl;
            ESP_LOGI(TAG, "在 SDA=%d SCL=%d 命中 0x%02X", sda, scl, addr7);
            return ESP_OK;
        }
        bus_i2c_end(&tmp);                  /* 没命中就放掉，避免控制器被占 */
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_ERR_NOT_FOUND;
}
