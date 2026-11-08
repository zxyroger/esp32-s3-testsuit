/**
 * @file  ili9342c.h
 * @brief ILI9342C（320×240）4 线 SPI 屏的最小驱动
 *
 * 为什么自己写而不引 esp_lcd 的面板组件：
 *   IDF v6 把 esp_lcd_ili9341 拆成了独立组件，要联网从组件仓库拉。
 *   本框架一直刻意做到"只用 IDF 自带能力"，和 axp2101/es8311 一样，
 *   这里就用 spi_master + GPIO 手写，代码不到 300 行，行为完全可控。
 *
 * 接线（本板，详见 board_config.h）：
 *
 *   ESP32-S3                                 ILI9342C 模块
 *   GPIO41 SCLK ────────────────────────────▶ SCL
 *   GPIO40 MOSI ────────────────────────────▶ SDA
 *   GPIO21 CS   ────────────────────────────▶ CS   （软件控制）
 *   GPIO39 DC   ────────────────────────────▶ DC   （低=命令，高=数据）
 *   GPIO42 BL   ────────────────────────────▶ LED  （低电平点亮，PWM 调光）
 *   GND     ────────────────────────────────▶ GND
 *   3V3     ────────────────────────────────▶ VCC
 *   （无 RST、无 MISO —— 见下方"能力边界"）
 *
 * 能力边界（决定了测试能断言什么、不能断言什么）：
 *   - 没有 RST：只能发 0x01 软复位；
 *   - 没有 MISO：**读不到 0x04 RDDID**，所以无法在电学上确认面板是否存在。
 *     SPI 是单向移位，命令写出去永远"成功"，哪怕对面什么都没接。
 *     因此驱动层所有函数返回 ESP_OK 只代表"字节已经移出 SOC"，
 *     屏是否收到，只能靠人眼看画面。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 常用颜色（RGB565） ---------------- */

#define LCD_COLOR_BLACK   0x0000
#define LCD_COLOR_WHITE   0xFFFF
#define LCD_COLOR_RED     0xF800
#define LCD_COLOR_GREEN   0x07E0
#define LCD_COLOR_BLUE    0x001F
#define LCD_COLOR_YELLOW  0xFFE0
#define LCD_COLOR_CYAN    0x07FF
#define LCD_COLOR_MAGENTA 0xF81F
#define LCD_COLOR_GRAY    0x8410

typedef struct {
    spi_device_handle_t spi;
    bool                ready;
    int                 pclk_khz;   /* 实际生效的时钟，分频后可能与配置值不同 */
    uint8_t             madctl;     /* 实际写入 0x36 的值，便于报告核对 */
} ili9342c_t;

/* ---------------- 总线与面板 ---------------- */

/**
 * 初始化 SPI 总线、DC/CS 引脚并下发完整初始化序列。
 *
 * 幂等：重复调用只重发初始化序列，不会重复建总线（spi_bus_initialize
 * 第二次会返回 ESP_ERR_INVALID_STATE，和 esp_wifi_init 是同一类坑）。
 *
 * @return ESP_OK       总线建成且命令序列全部发出（不代表屏收到了）
 * @return 其他         引脚被占用 / DMA 分配失败等真实错误
 */
esp_err_t ili9342c_init(ili9342c_t *dev);

/** 释放 SPI 设备与总线（重复调用安全） */
esp_err_t ili9342c_deinit(ili9342c_t *dev);

/** 软件复位（0x01 + 120 ms），无硬件 RST 时的唯一手段 */
esp_err_t ili9342c_reset(ili9342c_t *dev);

/**
 * 发一条命令 + 可选参数。
 *
 * DC 的时序要求：整条命令（命令字节 + 全部参数）期间 CS 必须保持低、
 * DC 在命令字节后翻高。这里自己控 CS 而不用硬件 CS，就是为了保证这一点 ——
 * 硬件 CS 会在每次 transaction 之间抖一下，个别模块会因此丢参数。
 */
esp_err_t ili9342c_cmd(ili9342c_t *dev, uint8_t cmd, const uint8_t *data,
                       size_t len);

/** 单独控制 DC/CS 电平，供时序验证用例使用 */
esp_err_t ili9342c_set_dc(ili9342c_t *dev, int level);
esp_err_t ili9342c_set_cs(ili9342c_t *dev, int level);

/* ---------------- 像素写入 ---------------- */

/**
 * 开一个写窗口：CASET + RASET + RAMWR，且 **CS 保持低**，
 * 直到 ili9342c_end_write() 才释放。中间用 ili9342c_send_pixels() 灌数据。
 *
 * 分三步而不是一个 fill_rect，是为了让上层能用 25 KB 的条带缓冲
 * 分多次灌满一整屏 —— 没开 PSRAM 时一次性要 150 KB 是拿不到的。
 */
esp_err_t ili9342c_begin_write(ili9342c_t *dev, uint16_t x0, uint16_t y0,
                               uint16_t x1, uint16_t y1);

/** 灌像素数据（原样发送，调用者负责字节序：RGB565 高字节在前） */
esp_err_t ili9342c_send_pixels(ili9342c_t *dev, const void *buf, size_t bytes);

/** 结束写窗口，释放 CS */
esp_err_t ili9342c_end_write(ili9342c_t *dev);

/** 全屏填一个颜色（内部按条带分块，缓冲由调用者给，可为 NULL 则临时分配） */
esp_err_t ili9342c_fill(ili9342c_t *dev, uint16_t color, void *strip_buf,
                        size_t strip_bytes);

/** 在 (x,y) 起画一个 sw×sh 的实心矩形（用于彩条、边框） */
esp_err_t ili9342c_fill_rect(ili9342c_t *dev, uint16_t x, uint16_t y,
                             uint16_t w, uint16_t h, uint16_t color,
                             void *strip_buf, size_t strip_bytes);

/* ---------------- 背光 ---------------- */

/** 配置 LEDC 通道；初始为全灭（不影响上电瞬间的花屏） */
esp_err_t ili9342c_bl_init(void);

/**
 * 设背光亮度 0~100。
 * 注意极性：本板低电平点亮，所以内部换算成 duty = 255 × (100 - level) / 100。
 */
esp_err_t ili9342c_bl_set(int level);

/** 读回 LEDC 当前 duty（0~255），用于验证换算与极性 */
uint32_t ili9342c_bl_get_duty(void);

/** 读回背光脚的真实电平（0/1）。level=0 应为 1，level=100 应为 0 */
esp_err_t ili9342c_bl_read_pin(int *level);

/* ---------------- 只读探测（本板不可用） ---------------- */

/**
 * 尝试读 RDDID(0x04) 拿 3 字节 ID。
 *
 * 本板 MISO 未引出，调用会直接返回 ESP_ERR_NOT_SUPPORTED —— 这是如实回答，
 * 不是失败。换成接了 SDO 的模块（改 BOARD_LCD_MISO_GPIO）后即可读到
 * 0x009342 / 0x009341 之类的值。
 */
esp_err_t ili9342c_read_id(ili9342c_t *dev, uint32_t *id);

/* ---------------- 自检辅助 ---------------- */

/** 0x36 应该写的值，由 board_config.h 的 MIRROR/SWAP 算出来 */
uint8_t ili9342c_madctl_value(void);

/** 发一串"已知无害"的查询命令，验证 SPI 传输链路本身不报错 */
esp_err_t ili9342c_ping(ili9342c_t *dev);

#ifdef __cplusplus
}
#endif
