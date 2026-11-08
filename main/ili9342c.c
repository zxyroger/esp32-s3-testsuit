/**
 * @file  ili9342c.c
 * @brief ILI9342C 4 线 SPI 驱动实现
 *
 * 时序要点（为什么这么写）：
 *
 *   1) CS 自己控，不用硬件 CS。
 *      ILI9341/9342 要求"命令字节 + 全部参数"这一整段 CS 保持低、DC 在
 *      命令字节之后翻高。若把 CS 交给 spi_master 自动控制，每个 transaction
 *      之间 CS 会抬高一次，参数可能被面板当成新命令的第一个字节。
 *
 *   2) 短传输走 polling，长传输走 DMA。
 *      命令/参数最多几十字节，polling 省掉中断开销；像素数据是几十 KB，
 *      必须交给 DMA，否则 40 MHz 下 CPU 会被完全占住。
 *
 *   3) 条带刷新，不一次要 150 KB。
 *      本板没开 PSRAM（CONFIG_SPIRAM 未设置），内部 RAM 还要留给 WiFi，
 *      一次性 malloc 全屏帧缓冲很容易失败。320×40×2 = 25 KB 是安全的量级。
 *
 * 初始化序列的参数来源：同款板量产工程
 *   D:\esp\ai-esp32\firmware\main\boards\chatbot-esp32-s3\chatbot_esp32_s3_board.cc
 *   （vendor_specific_init），那边是拿 esp_lcd_ili9341 跑通的。
 * 顺序按 ILI9341 手册 §9.1 重排过：原序列把 SLPOUT(0x11) 放在 MADCTL 之前
 * 且 delay 写 0，那是错的 —— 手册要求 SLPOUT 后必须等 120 ms 才能发下一条。
 */
#include "ili9342c.h"

#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ili9342c";

/* 0x36 MADCTL：bit3 = BGR 排列（量产固件实测就是置上的），
 * bit6/bit7 = MX/MY（mirror），bit5 = MV（swap_xy）。
 * 若接上屏后红蓝互换，先查 bit3；若上下/左右颠倒，查 MX/MY。 */
#define LCD_MADCTL_VALUE                                                      \
    (0x08 | (BOARD_LCD_MIRROR_X ? 0x40 : 0) | (BOARD_LCD_MIRROR_Y ? 0x80 : 0) | \
     (BOARD_LCD_SWAP_XY ? 0x20 : 0))

#define LCD_DC_CMD  0
#define LCD_DC_DATA 1

/* 条带缓冲上限：单次 SPI 传输的字节数，总线初始化时按这个开 DMA 描述符 */
#define LCD_STRIP_BYTES (BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * 2)

/* ---------------- 总线（全局唯一，因为 SPI2 只有一个） ---------------- */

static bool                s_bus_ok = false;
static spi_device_handle_t s_spi = NULL;
static bool                s_bl_ok = false;

/* ---------------- 低层收发 ---------------- */

static esp_err_t lcd_tx(ili9342c_t *dev, const void *data, size_t len, int dc)
{
    if (dev == NULL || dev->spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        return ESP_OK;
    }

    gpio_set_level(BOARD_LCD_DC_GPIO, dc);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = data;

    if (len <= 64) {
        return spi_device_polling_transmit(dev->spi, &t);
    }
    return spi_device_transmit(dev->spi, &t);
}

esp_err_t ili9342c_set_dc(ili9342c_t *dev, int level)
{
    (void)dev;
    gpio_set_level(BOARD_LCD_DC_GPIO, level ? 1 : 0);
    return ESP_OK;
}

esp_err_t ili9342c_set_cs(ili9342c_t *dev, int level)
{
    (void)dev;
    gpio_set_level(BOARD_LCD_CS_GPIO, level ? 1 : 0);
    return ESP_OK;
}

/* ---------------- 命令 ---------------- */

esp_err_t ili9342c_cmd(ili9342c_t *dev, uint8_t cmd, const uint8_t *data,
                       size_t len)
{
    esp_err_t err;

    ili9342c_set_cs(dev, 0);
    err = lcd_tx(dev, &cmd, 1, LCD_DC_CMD);
    if (err == ESP_OK && len > 0 && data != NULL) {
        err = lcd_tx(dev, data, len, LCD_DC_DATA);
    }
    ili9342c_set_cs(dev, 1);

    return err;
}

/* ---------------- 初始化序列 ---------------- */

typedef struct {
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  data[15];
    uint16_t delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_init_seq[] = {
    /* --- 厂家扩展：Power Control B / Driver Timing / Power On Sequence 等 --- */
    { 0xCF, 3, { 0x00, 0xC1, 0x30 }, 0 },
    { 0xED, 4, { 0x64, 0x03, 0x12, 0x81 }, 0 },
    { 0xE8, 3, { 0x85, 0x00, 0x78 }, 0 },
    { 0xCB, 5, { 0x39, 0x2C, 0x00, 0x34, 0x02 }, 0 },
    { 0xF7, 1, { 0x20 }, 0 },
    { 0xEA, 2, { 0x00, 0x00 }, 0 },

    /* --- 电源控制 --- */
    { 0xC0, 1, { 0x13 }, 0 },          /* Power Control 1：GVDD = 4.6 V */
    { 0xC1, 1, { 0x13 }, 0 },          /* Power Control 2 */
    { 0xC5, 2, { 0x22, 0x35 }, 0 },    /* VCOM 控制 */
    { 0xC7, 1, { 0xBD }, 0 },          /* VCOM 偏移 */

    /* --- 帧率 / 显示功能 / 接口 --- */
    { 0xB1, 2, { 0x00, 0x1B }, 0 },    /* 帧率 ~70 Hz */
    { 0xB6, 2, { 0x0A, 0xA2 }, 0 },    /* 显示功能控制 */
    { 0xF6, 2, { 0x01, 0x30 }, 0 },    /* 接口控制：16 bit 并行时序参数 */
    { 0x36, 1, { LCD_MADCTL_VALUE }, 0 }, /* 扫描方向（见宏注释） */
    { 0x3A, 1, { 0x55 }, 0 },          /* COLMOD = RGB565，SPI 下唯一的 16bit 选项 */
    { 0x26, 1, { 0x01 }, 0 },          /* Gamma Set = Curve 1 */

    /* --- Gamma 曲线（正/负极性），15 点 --- */
    { 0xE0, 15, { 0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49,
                  0xA7, 0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00 }, 0 },
    { 0xE1, 15, { 0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36,
                  0x58, 0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F }, 0 },

    /* --- 退出睡眠 → 反相 → 开显示 ---
     * 0x21 必须发：面板默认输出与 RGB565 极性相反，不发就黑白颠倒
     * （表现形式是"白底变黑底"，很容易误判成"屏没点亮"）。 */
    { 0x11, 0, { 0 }, 120 },           /* SLPOUT，手册要求 120 ms */
    { 0x21, 0, { 0 }, 0 },             /* INVON */
    { 0x29, 0, { 0 }, 20 },            /* DISPON */
};

uint8_t ili9342c_madctl_value(void)
{
    return (uint8_t)LCD_MADCTL_VALUE;
}

/* ---------------- 复位 ---------------- */

esp_err_t ili9342c_reset(ili9342c_t *dev)
{
    esp_err_t err = ili9342c_cmd(dev, 0x01, NULL, 0);   /* SWRESET */
    if (err != ESP_OK) {
        return err;
    }
    /* 手册 §9.1：SWRESET 后必须等 120 ms，期间不能发任何命令 */
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

/* ---------------- GPIO ---------------- */

static esp_err_t lcd_gpio_init(void)
{
    /* DC 与 CS 都配成 INPUT_OUTPUT：输入通路必须打开，否则 gpio_get_level()
     * 永远读到 0，用例就无法证明"电平真的翻转了"。
     * 这是音频组踩过的坑（GPIO_MODE_OUTPUT 下读回恒 0）。 */
    gpio_config_t io;
    memset(&io, 0, sizeof(io));
    io.pin_bit_mask = (1ULL << BOARD_LCD_DC_GPIO) | (1ULL << BOARD_LCD_CS_GPIO);
    io.mode = GPIO_MODE_INPUT_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    /* 空闲态：CS 高（未选中）、DC 高（数据），这是 ILI9341 的复位默认语义 */
    gpio_set_level(BOARD_LCD_CS_GPIO, 1);
    gpio_set_level(BOARD_LCD_DC_GPIO, 1);
    return ESP_OK;
}

/* ---------------- 背光 ---------------- */

esp_err_t ili9342c_bl_init(void)
{
    if (s_bl_ok) {
        return ESP_OK;
    }

    ledc_timer_config_t timer;
    memset(&timer, 0, sizeof(timer));
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = (ledc_timer_bit_t)BOARD_LCD_BL_PWM_BITS;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = BOARD_LCD_BL_PWM_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;

    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.gpio_num = BOARD_LCD_BL_GPIO;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = LEDC_CHANNEL_0;
    ch.timer_sel = LEDC_TIMER_0;
    /* 初值给满占空比 = 背光关（低电平点亮），避免上电就白屏刺眼。
     * 注意上限是 2^bits = 256，不是 255 —— 255 只有 99.6%，还差一个
     * 计数周期的高电平，读回引脚时会偶发读到 0。 */
    ch.duty = (1u << BOARD_LCD_BL_PWM_BITS);
    ch.hpoint = 0;

    err = ledc_channel_config(&ch);
    if (err != ESP_OK) {
        return err;
    }

    /* 只打开输入通路，绝对不能调 gpio_set_direction()！
     * 那会把引脚切成"普通 GPIO 输出"，把 LEDC 从 GPIO matrix 上摘下来，
     * 结果就是 duty 怎么设、引脚都恒为 0（本项第一次实测就是这么 FAIL 的）。
     * gpio_input_enable() 只置输入使能位，输出路径仍归 LEDC。 */
    err = gpio_input_enable(BOARD_LCD_BL_GPIO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "背光脚输入使能失败: %s（不影响亮度，只影响电平读回）",
                 esp_err_to_name(err));
    }

    s_bl_ok = true;
    ESP_LOGI(TAG, "背光 PWM 就绪：GPIO%d %d Hz / %d bit",
             BOARD_LCD_BL_GPIO, BOARD_LCD_BL_PWM_HZ, BOARD_LCD_BL_PWM_BITS);
    return ESP_OK;
}

esp_err_t ili9342c_bl_set(int level)
{
    if (!s_bl_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    if (level < 0) {
        level = 0;
    }
    if (level > 100) {
        level = 100;
    }

    uint32_t full = 1u << BOARD_LCD_BL_PWM_BITS;
    uint32_t duty;
#if BOARD_LCD_BL_ACTIVE_LOW
    /* 低电平点亮：亮度 ∝ 低电平占比 = 1 - duty/full */
    duty = (full * (uint32_t)(100 - level)) / 100u;
#else
    duty = (full * (uint32_t)level) / 100u;
#endif

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint32_t ili9342c_bl_get_duty(void)
{
    if (!s_bl_ok) {
        return 0;
    }
    return ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t ili9342c_bl_read_pin(int *level)
{
    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *level = gpio_get_level(BOARD_LCD_BL_GPIO);
    return ESP_OK;
}

/* ---------------- 初始化 / 释放 ---------------- */

esp_err_t ili9342c_init(ili9342c_t *dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1) 总线：只建一次。重复调用 spi_bus_initialize 会返回
     *    ESP_ERR_INVALID_STATE —— 和 esp_wifi_init 一样，必须自己做幂等。 */
    if (!s_bus_ok) {
        spi_bus_config_t bus;
        memset(&bus, 0, sizeof(bus));
        bus.mosi_io_num = BOARD_LCD_MOSI_GPIO;
        bus.miso_io_num = BOARD_LCD_MISO_GPIO;   /* -1：本板没接 */
        bus.sclk_io_num = BOARD_LCD_SCLK_GPIO;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = LCD_STRIP_BYTES;

        esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI2 总线初始化失败: %s", esp_err_to_name(err));
            return err;
        }

        spi_device_interface_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.clock_speed_hz = BOARD_LCD_PCLK_HZ;
        cfg.mode = 0;              /* CPOL=0 CPHA=0，ILI9341 系列固定 */
        cfg.spics_io_num = -1;     /* 关键：CS 由软件控制，见文件头 */
        cfg.queue_size = 8;

        err = spi_bus_add_device(SPI2_HOST, &cfg, &s_spi);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI2 添加设备失败: %s", esp_err_to_name(err));
            spi_bus_free(SPI2_HOST);
            return err;
        }
        s_bus_ok = true;
    }

    dev->spi = s_spi;
    int khz = 0;
    if (spi_device_get_actual_freq(s_spi, &khz) == ESP_OK) {
        dev->pclk_khz = khz;
    }
    dev->madctl = ili9342c_madctl_value();

    /* 2) DC / CS */
    esp_err_t err = lcd_gpio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DC/CS 引脚配置失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 3) 背光（失败不算致命，屏还能用，只是看不见） */
    esp_err_t bl_err = ili9342c_bl_init();
    if (bl_err != ESP_OK) {
        ESP_LOGW(TAG, "背光 PWM 初始化失败: %s", esp_err_to_name(bl_err));
    }

    /* 4) 复位 + 初始化序列 */
    err = ili9342c_reset(dev);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < sizeof(s_init_seq) / sizeof(s_init_seq[0]); i++) {
        const lcd_init_cmd_t *c = &s_init_seq[i];
        err = ili9342c_cmd(dev, c->cmd, c->len ? c->data : NULL, c->len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "初始化序列第 %u 条 (0x%02X) 失败: %s",
                     (unsigned)(i + 1), (unsigned)c->cmd, esp_err_to_name(err));
            return err;
        }
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    dev->ready = true;
    ESP_LOGI(TAG, "初始化完成：%u 条命令，SPI %d kHz，MADCTL=0x%02X",
             (unsigned)(sizeof(s_init_seq) / sizeof(s_init_seq[0])),
             dev->pclk_khz, dev->madctl);
    return ESP_OK;
}

esp_err_t ili9342c_deinit(ili9342c_t *dev)
{
    if (dev != NULL) {
        dev->ready = false;
        dev->spi = NULL;
    }
    if (s_spi != NULL) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    if (s_bus_ok) {
        spi_bus_free(SPI2_HOST);
        s_bus_ok = false;
    }
    return ESP_OK;
}

/* ---------------- 像素写入 ---------------- */

esp_err_t ili9342c_begin_write(ili9342c_t *dev, uint16_t x0, uint16_t y0,
                               uint16_t x1, uint16_t y1)
{
    esp_err_t err;

    /* CS 从命令开始一直压到 ili9342c_end_write()，中间不能松 */
    ili9342c_set_cs(dev, 0);

    const uint8_t cmd_caset = 0x2A;
    uint8_t caset[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                         (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    err = lcd_tx(dev, &cmd_caset, 1, LCD_DC_CMD);
    if (err == ESP_OK) {
        err = lcd_tx(dev, caset, 4, LCD_DC_DATA);
    }
    if (err != ESP_OK) {
        ili9342c_set_cs(dev, 1);
        return err;
    }

    const uint8_t cmd_raset = 0x2B;
    uint8_t raset[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                         (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    err = lcd_tx(dev, &cmd_raset, 1, LCD_DC_CMD);
    if (err == ESP_OK) {
        err = lcd_tx(dev, raset, 4, LCD_DC_DATA);
    }
    if (err != ESP_OK) {
        ili9342c_set_cs(dev, 1);
        return err;
    }

    const uint8_t cmd_ramwr = 0x2C;
    err = lcd_tx(dev, &cmd_ramwr, 1, LCD_DC_CMD);
    if (err != ESP_OK) {
        ili9342c_set_cs(dev, 1);
    }
    return err;
}

esp_err_t ili9342c_send_pixels(ili9342c_t *dev, const void *buf, size_t bytes)
{
    return lcd_tx(dev, buf, bytes, LCD_DC_DATA);
}

esp_err_t ili9342c_end_write(ili9342c_t *dev)
{
    ili9342c_set_cs(dev, 1);
    return ESP_OK;
}

/* 把条带缓冲填成同一种颜色（RGB565 高字节在前，面板要求） */
static void fill_color(void *buf, size_t pixels, uint16_t color)
{
    uint8_t *p = (uint8_t *)buf;
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);
    for (size_t i = 0; i < pixels; i++) {
        p[2 * i]     = hi;
        p[2 * i + 1] = lo;
    }
}

esp_err_t ili9342c_fill_rect(ili9342c_t *dev, uint16_t x, uint16_t y,
                             uint16_t w, uint16_t h, uint16_t color,
                             void *strip_buf, size_t strip_bytes)
{
    if (dev == NULL || !dev->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (w == 0 || h == 0) {
        return ESP_OK;
    }

    /* 条带按"行"切：一次最多 flush_lines 行，缓冲至少要能放下一行 */
    size_t row_bytes = (size_t)w * 2;
    size_t max_lines = strip_bytes / row_bytes;
    if (max_lines == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (max_lines > h) {
        max_lines = h;
    }

    uint8_t *buf = (uint8_t *)strip_buf;
    bool own = false;
    if (buf == NULL) {
        size_t bytes = row_bytes * max_lines;
        buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA);
        if (buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        strip_bytes = bytes;
        own = true;
    }

    /* 缓冲里重复的是同一行像素，一次填好，后面每个条带都复用它 */
    fill_color(buf, (size_t)w * max_lines, color);

    esp_err_t err = ESP_OK;
    uint16_t row = 0;
    while (row < h) {
        uint16_t lines = (uint16_t)((h - row < max_lines) ? (h - row) : max_lines);
        err = ili9342c_begin_write(dev, x, (uint16_t)(y + row),
                                   (uint16_t)(x + w - 1),
                                   (uint16_t)(y + row + lines - 1));
        if (err != ESP_OK) {
            break;
        }
        err = ili9342c_send_pixels(dev, buf, (size_t)lines * row_bytes);
        ili9342c_end_write(dev);
        if (err != ESP_OK) {
            break;
        }
        row = (uint16_t)(row + lines);
    }

    if (own) {
        heap_caps_free(buf);
    }
    return err;
}

esp_err_t ili9342c_fill(ili9342c_t *dev, uint16_t color, void *strip_buf,
                        size_t strip_bytes)
{
    return ili9342c_fill_rect(dev, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES,
                              color, strip_buf, strip_bytes);
}

/* ---------------- 只读探测 ---------------- */

esp_err_t ili9342c_read_id(ili9342c_t *dev, uint32_t *id)
{
    (void)dev;
    if (id != NULL) {
        *id = 0;
    }
    if (BOARD_LCD_MISO_GPIO < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* 有 MISO 的板子在此实现：发 0x04 后连续读 3 字节（dummy + ID1 + ID2）。
     * 本板 SDO 未引出，走不到这里。 */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ili9342c_ping(ili9342c_t *dev)
{
    /* NOP(0x00) 对面板无副作用；RDID(0x04) 在没有 MISO 时也安全
     * （面板只是把 ID 推到 SDO，没人收而已）。两发一通即可证明
     * "SPI 引擎能按 DC/CS 时序把字节移出去"。 */
    esp_err_t err = ili9342c_cmd(dev, 0x00, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    return ili9342c_cmd(dev, 0x04, NULL, 0);
}
