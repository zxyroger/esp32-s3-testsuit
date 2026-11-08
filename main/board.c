/**
 * @file  board.c
 * @brief 板级资源延迟初始化
 *
 * 引脚策略（三级回退）：
 *   1. board_config.h 里给定的 SDA/SCL 直接建总线；
 *   2. 若目标器件不应答，试一次 SDA/SCL 反接（反接只会 NACK，不会损坏）；
 *   3. 仍不行就按候选表逐对扫描，命中 AXP2101 即锁定。
 * 这样引脚信息不全时也能自证，而不是直接判定"器件坏了"。
 */
#include "board.h"

#include <stddef.h>

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static bus_i2c_t s_bus;
static bool      s_bus_tried = false;
static bool      s_bus_ok    = false;
static int       s_sda = -1;
static int       s_scl = -1;

static axp2101_t s_pmic;
static bool      s_pmic_tried = false;
static bool      s_pmic_ok    = false;

static es8311_t  s_codec;
static bool      s_codec_tried = false;
static bool      s_codec_ok    = false;

static audio_path_t s_audio;
static bool         s_audio_tried = false;
static bool         s_audio_ok    = false;

static cam_t        s_cam;
static bool         s_cam_tried = false;
static bool         s_cam_ok    = false;

static void ensure_bus(void)
{
    if (s_bus_tried) {
        return;
    }
    s_bus_tried = true;

#if (BOARD_I2C0_SDA_GPIO >= 0) && (BOARD_I2C0_SCL_GPIO >= 0)
    /* 1) 直连配置引脚 */
    if (bus_i2c_begin(&s_bus, BOARD_I2C0_SDA_GPIO, BOARD_I2C0_SCL_GPIO,
                      BOARD_I2C0_SPEED_HZ) == ESP_OK) {
        s_bus_ok = true;
        s_sda = BOARD_I2C0_SDA_GPIO;
        s_scl = BOARD_I2C0_SCL_GPIO;
        if (!bus_i2c_probe(&s_bus, BOARD_ADDR_AXP2101)) {
            /* 2) 反接再试一次 */
            ESP_LOGW(TAG, "SDA=%d/SCL=%d 未见 0x%02X，尝试反接", s_sda, s_scl,
                     BOARD_ADDR_AXP2101);
            bus_i2c_end(&s_bus);
            s_bus_ok = false;
            if (bus_i2c_begin(&s_bus, BOARD_I2C0_SCL_GPIO, BOARD_I2C0_SDA_GPIO,
                              BOARD_I2C0_SPEED_HZ) == ESP_OK) {
                s_bus_ok = true;
                s_sda = BOARD_I2C0_SCL_GPIO;
                s_scl = BOARD_I2C0_SDA_GPIO;
            }
        }
    }
#endif

    /* 3) 候选表扫描兜底 */
    if (!s_bus_ok) {
#if BOARD_I2C_AUTODETECT
        int hit_sda = -1, hit_scl = -1;
        ESP_LOGI(TAG, "按候选表扫描 I2C 引脚 …");
        if (bus_i2c_autodetect(&s_bus, BOARD_ADDR_AXP2101,
                               BOARD_I2C_CANDIDATES,
                               BOARD_I2C_CANDIDATE_COUNT,
                               BOARD_I2C0_SPEED_HZ,
                               &hit_sda, &hit_scl) == ESP_OK) {
            s_bus_ok = true;
            s_sda = hit_sda;
            s_scl = hit_scl;
        } else {
            ESP_LOGE(TAG, "候选表内未找到 0x%02X", BOARD_ADDR_AXP2101);
        }
#endif
    }

    if (s_bus_ok) {
        ESP_LOGI(TAG, "I2C 就绪：SDA=%d SCL=%d", s_sda, s_scl);
    }
}

const bus_i2c_t *board_bus(void)
{
    ensure_bus();
    return s_bus_ok ? &s_bus : NULL;
}

void board_bus_pins(int *sda, int *scl)
{
    if (sda) *sda = s_sda;
    if (scl) *scl = s_scl;
}

axp2101_t *board_pmic(void)
{
    if (s_pmic_tried) {
        return s_pmic_ok ? &s_pmic : NULL;
    }
    s_pmic_tried = true;

    bus_i2c_t *bus = (bus_i2c_t *)board_bus();
    if (bus == NULL) {
        return NULL;
    }
    if (axp2101_init(&s_pmic, bus, BOARD_ADDR_AXP2101) != ESP_OK) {
        return NULL;
    }
    s_pmic_ok = true;
    return &s_pmic;
}

es8311_t *board_codec(void)
{
    if (s_codec_tried) {
        return s_codec_ok ? &s_codec : NULL;
    }
    s_codec_tried = true;

    bus_i2c_t *bus = (bus_i2c_t *)board_bus();
    if (bus == NULL) {
        return NULL;
    }
    if (es8311_init(&s_codec, bus, BOARD_ADDR_ES8311) != ESP_OK) {
        return NULL;
    }
    s_codec_ok = true;
    return &s_codec;
}

audio_path_t *board_audio(void)
{
    if (s_audio_tried) {
        return s_audio_ok ? &s_audio : NULL;
    }
    s_audio_tried = true;

    /* I2S 只依赖 GPIO，与 I2C 总线无关：CODEC 没贴片也能建通道，
     * 这样"没有声音"时才能分清是数据通路的问题还是器件的问题。 */
    if (audio_path_init(&s_audio) != ESP_OK) {
        return NULL;
    }
    s_audio_ok = true;
    return &s_audio;
}

cam_t *board_camera(void)
{
    if (s_cam_tried) {
        return s_cam_ok ? &s_cam : NULL;
    }
    s_cam_tried = true;

    bus_i2c_t *bus = (bus_i2c_t *)board_bus();
    if (bus == NULL) {
        return NULL;
    }
    if (cam_prepare(&s_cam, bus) != ESP_OK) {
        return NULL;
    }
    s_cam_ok = true;
    return &s_cam;
}
