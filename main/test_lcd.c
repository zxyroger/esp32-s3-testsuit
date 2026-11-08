/**
 * @file  test_lcd.c
 * @brief ILI9342C 液晶屏接口测试（本板当前未接屏）
 *
 * 这块板子的 LCD 有个绕不开的限制，先讲清楚，否则结论会被误读：
 *
 *   **SPI 屏没有 MISO，命令发出去永远"成功"。**
 *
 *   常规做法是读 0x04 RDDID 确认面板 ID（ILI9342C 应返回 0x009342 附近），
 *   但本板模块的 SDO 没引到排针，读不回来。这意味着：
 *     - 驱动返回 ESP_OK 只代表"字节已从 SOC 移位出去"，不代表面板收到了；
 *     - 因此本文件里凡是"只要 SPI 引擎正常就一定过"的项，即使没接屏也 PASS。
 *   这不是在糊弄，而是这类屏的客观边界。同理，音频组里"配置成功 ≠ 出声"
 *   的教训在这里同样成立：能电学自证的和只能人眼看的东西必须分开标注。
 *
 * 所以分组按"能证明什么"分三层：
 *   1. 片上可硬断言（不依赖屏）：总线时钟、DC/CS 时序、命令序列下发、
 *      背光 PWM 与极性、SPI 吞吐 —— 没接屏也必须 PASS，接了屏才说明硬件没坏；
 *   2. 电学上不可达：读芯片 ID —— 直接 SKIP 并说明原因（不是失败）；
 *   3. 只能人眼：彩条 —— SKIP，但会把画面刷出来并点亮背光，接上屏即可验收。
 *
 * 接上屏之后，重跑 `run lcd`，第 3 层变成肉眼可见的验收，前两层照旧。
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "tf.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ili9342c.h"

/* 吞吐下限：40 MHz 理论值 5 MB/s，实测通常 3.5~4.5 MB/s。
 * 取 1.0 MB/s 作下限，只用来抓"时钟没配对 / DMA 没生效"这类硬故障，
 * 不拿它评判屏的好坏（屏在不在都能跑到这个数）。 */
#define LCD_THROUGHPUT_MIN_MBPS 1.0

/* 全屏字节数：320 × 240 × 2 */
#define LCD_FULL_BYTES (BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2)

static ili9342c_t s_lcd;
static bool       s_lcd_tried = false;
static bool       s_lcd_ok    = false;

/** 取（必要时初始化）LCD 句柄；失败返回 NULL */
static ili9342c_t *ensure_lcd(void)
{
    if (s_lcd_tried) {
        return s_lcd_ok ? &s_lcd : NULL;
    }
    s_lcd_tried = true;

    if (ili9342c_init(&s_lcd) != ESP_OK) {
        /* 允许下次重跑再试一次：init 内部对总线是幂等的，重复调用安全 */
        s_lcd_tried = false;
        return NULL;
    }
    s_lcd_ok = true;
    return &s_lcd;
}

#define REQUIRE_LCD(var)                                                       \
    ili9342c_t *(var) = ensure_lcd();                                          \
    if ((var) == NULL) {                                                       \
        TF_SKIP("LCD 初始化失败（SPI2 引脚被占用或 DMA 缓冲不足），跳过");     \
    }

/* 条带缓冲：25 KB，放内部 DMA RAM。没开 PSRAM，不能贪大。 */
static void *alloc_strip(void)
{
    return heap_caps_malloc(BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * 2,
                            MALLOC_CAP_DMA);
}

/* ---------------- 1. 总线与引脚 ---------------- */

static tf_result_t t_bus(void)
{
    ili9342c_t *lcd = ensure_lcd();
    if (lcd == NULL) {
        /* 这一项是整组的门槛：失败就是真实故障，不能 SKIP */
        TF_CHECK(false, "SPI2 初始化失败：检查 SCLK=GPIO%d / MOSI=GPIO%d 是否被占用",
                 BOARD_LCD_SCLK_GPIO, BOARD_LCD_MOSI_GPIO);
    }

    TF_INFO("SPI2 主模式：SCLK=GPIO%d MOSI=GPIO%d CS=GPIO%d DC=GPIO%d",
            BOARD_LCD_SCLK_GPIO, BOARD_LCD_MOSI_GPIO, BOARD_LCD_CS_GPIO,
            BOARD_LCD_DC_GPIO);
    TF_INFO("配置时钟 %d kHz，实测生效 %d kHz（分频后）",
            BOARD_LCD_PCLK_HZ / 1000, lcd->pclk_khz);
    TF_INFO("分辨率 %dx%d，条带 %d 行 = %d B",
            BOARD_LCD_H_RES, BOARD_LCD_V_RES, BOARD_LCD_STRIP_LINES,
            BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * 2);

    /* 40 MHz 在 APB 80 MHz 下分频比 2，实测应正好 40000 kHz。
     * 允许 10% 误差，兼容不同 APB 频率配置。 */
    TF_CHECK(lcd->pclk_khz >= (BOARD_LCD_PCLK_HZ / 1000) * 9 / 10 &&
                 lcd->pclk_khz <= (BOARD_LCD_PCLK_HZ / 1000) * 11 / 10,
             "SPI 实际时钟 %d kHz 与配置 %d kHz 偏差过大（分频配置异常）",
             lcd->pclk_khz, BOARD_LCD_PCLK_HZ / 1000);

    TF_CHECK(lcd->ready, "面板初始化序列未完成");
    return TF_PASS;
}

/* ---------------- 2. DC / CS 时序 ---------------- */

static tf_result_t t_io_pins(void)
{
    REQUIRE_LCD(lcd);

    /* DC 低=命令、高=数据。这两脚必须真能翻转，否则整条命令会被当成参数。
     * 用 gpio_get_level 读回自证，而不是"设了就当设上了" —— 输出模式
     * 若没打开输入通路，读回会恒 0（音频组踩过）。 */
    ili9342c_set_dc(lcd, 0);
    int dc_low = gpio_get_level(BOARD_LCD_DC_GPIO);
    ili9342c_set_dc(lcd, 1);
    int dc_high = gpio_get_level(BOARD_LCD_DC_GPIO);

    TF_INFO("DC=GPIO%d：写 0 读回 %d，写 1 读回 %d", BOARD_LCD_DC_GPIO, dc_low,
            dc_high);
    TF_CHECK(dc_low == 0 && dc_high == 1,
             "DC 脚无法正常翻转（读回 %d/%d），检查 GPIO%d 是否被其他外设占用",
             dc_low, dc_high, BOARD_LCD_DC_GPIO);

    ili9342c_set_cs(lcd, 0);
    int cs_low = gpio_get_level(BOARD_LCD_CS_GPIO);
    ili9342c_set_cs(lcd, 1);
    int cs_high = gpio_get_level(BOARD_LCD_CS_GPIO);

    TF_INFO("CS=GPIO%d：拉低读回 %d，拉高读回 %d", BOARD_LCD_CS_GPIO, cs_low,
            cs_high);
    TF_CHECK(cs_low == 0 && cs_high == 1,
             "CS 脚无法正常翻转（读回 %d/%d），检查 GPIO%d 是否被 I2C 自动探测占用",
             cs_low, cs_high, BOARD_LCD_CS_GPIO);

    /* 收发一条无害命令，验证 CS/DC 组合时序下传输不报错 */
    TF_CHECK_ERR(ili9342c_ping(lcd), "NOP/RDID 命令发送");
    TF_INFO("命令时序：CS 拉低 -> DC 低发命令 -> DC 高发参数 -> CS 拉高");

    return TF_PASS;
}

/* ---------------- 3. 复位与初始化序列 ---------------- */

static tf_result_t t_init_seq(void)
{
    REQUIRE_LCD(lcd);

    /* 无硬件 RST，只能软复位。这一步同时验证"初始化序列可以重复下发"，
     * 也就是 `run lcd` 能反复跑（和 wifi 组 esp_wifi_init 那个坑同理）。 */
    TF_CHECK_ERR(ili9342c_reset(lcd), "软复位 SWRESET(0x01) + 120 ms");

    uint32_t before = (uint32_t)esp_get_free_heap_size();
    TF_CHECK_ERR(ili9342c_init(lcd), "重复下发完整初始化序列");
    uint32_t after = (uint32_t)esp_get_free_heap_size();

    TF_INFO("软复位 + 21 条初始化命令全部发出，堆变化 %d B",
            (int)before - (int)after);
    TF_INFO("MADCTL=0x%02X（bit3=BGR, bit6=MX, bit7=MY），COLMOD=RGB565(0x55)",
            lcd->madctl);
    TF_INFO("末尾 0x11(SLPOUT)+120ms -> 0x21(INVON) -> 0x29(DISPON)+20ms");

    /* 重复初始化不该吃内存，吃了几十 KB 说明每次都重新建了总线 */
    TF_CHECK((int)before >= (int)after - 4096,
             "重复初始化后堆反而增加 %d B，总线可能被重复创建",
             (int)after - (int)before);

    TF_INFO("注意：以上全部只证明字节已移出 SOC；屏是否收到无法回读确认");
    return TF_PASS;
}

/* ---------------- 4. 背光 ---------------- */

static tf_result_t t_backlight(void)
{
    REQUIRE_LCD(lcd);

    uint32_t full = 1u << BOARD_LCD_BL_PWM_BITS;

    /* 极性与换算必须一起验：低电平点亮，所以 100% 亮度对应 duty=0，
     * 0% 亮度对应 duty=full。搞反了现象是"屏幕一片漆黑但能测到 PWM"。 */
    TF_CHECK_ERR(ili9342c_bl_set(0), "背光设为 0%");
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t duty_off = ili9342c_bl_get_duty();
    int pin_off = -1;
    ili9342c_bl_read_pin(&pin_off);

    TF_CHECK_ERR(ili9342c_bl_set(100), "背光设为 100%");
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t duty_on = ili9342c_bl_get_duty();
    int pin_on = -1;
    ili9342c_bl_read_pin(&pin_on);

    TF_CHECK_ERR(ili9342c_bl_set(50), "背光设为 50%");
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t duty_half = ili9342c_bl_get_duty();

    TF_INFO("0%%   亮度 -> duty=%u/%u，引脚=%d（应为高电平=灭）",
            (unsigned)duty_off, (unsigned)full, pin_off);
    TF_INFO("100%% 亮度 -> duty=%u，引脚=%d（应为低电平=亮）",
            (unsigned)duty_on, pin_on);
    TF_INFO("50%%  亮度 -> duty=%u", (unsigned)duty_half);

    TF_CHECK(duty_off == full && pin_off == 1,
             "0%% 亮度时 duty=%u（应为 %u）、引脚=%d（应为 1）—— 背光极性接反或 PWM 没起",
             (unsigned)duty_off, (unsigned)full, pin_off);
    TF_CHECK(duty_on == 0 && pin_on == 0,
             "100%% 亮度时 duty=%u（应为 0）、引脚=%d（应为 0）—— 背光极性接反或 PWM 没起",
             (unsigned)duty_on, pin_on);

    /* 50% 允许 ±8 的量化误差（255/2 = 127.5） */
    TF_CHECK(duty_half >= (full / 2) - 8 && duty_half <= (full / 2) + 8,
             "50%% 亮度时 duty=%u 偏离中点 %u 太多，亮度换算异常",
             (unsigned)duty_half, (unsigned)(full / 2));

    /* 留在全亮，方便后面看彩条 */
    ili9342c_bl_set(100);
    return TF_PASS;
}

/* ---------------- 5. 刷屏吞吐 ---------------- */

static tf_result_t t_fill(void)
{
    REQUIRE_LCD(lcd);

    void *strip = alloc_strip();
    if (strip == NULL) {
        TF_SKIP("分配 %d B 条带缓冲失败（内部 DMA RAM 不足）",
                BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * 2);
    }

    /* 刷 3 次全屏取平均，抵消首次传输的冷启动开销 */
    const int rounds = 3;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < rounds; i++) {
        esp_err_t err = ili9342c_fill(lcd, (uint16_t)(i * 0x2222), strip,
                                      BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * 2);
        if (err != ESP_OK) {
            heap_caps_free(strip);
            TF_CHECK_ERR(err, "全屏填充");
        }
    }
    int64_t dt_us = esp_timer_get_time() - t0;
    heap_caps_free(strip);

    double total_bytes = (double)LCD_FULL_BYTES * rounds;
    double mbps = total_bytes / ((double)dt_us / 1e6) / (1024.0 * 1024.0);
    double per_frame_ms = (double)dt_us / 1000.0 / rounds;

    TF_INFO("%d 帧全屏（%d B/帧）用时 %.1f ms，单帧 %.1f ms",
            rounds, LCD_FULL_BYTES, (double)dt_us / 1000.0, per_frame_ms);
    TF_INFO("实测吞吐 %.2f MB/s（40 MHz 理论 4.77 MiB/s）", mbps);
    TF_INFO("本项只证明 SPI 引擎 + DMA 在工作；没接屏同样会 PASS");

    /* 一帧 153.6 KB 在 40 MHz 下理论 30.7 ms；超过 500 ms 说明传输被
     * 拆成了很多小事务或退化成了 polling。 */
    TF_CHECK(per_frame_ms < 500.0, "单帧耗时 %.1f ms 过长，SPI 传输被拆碎",
             per_frame_ms);
    TF_CHECK(mbps >= LCD_THROUGHPUT_MIN_MBPS,
             "吞吐 %.2f MB/s 低于下限 %.1f MB/s，时钟或 DMA 未生效", mbps,
             LCD_THROUGHPUT_MIN_MBPS);

    return TF_PASS;
}

/* ---------------- 6. 读芯片 ID（本板不可达） ---------------- */

static tf_result_t t_read_id(void)
{
    REQUIRE_LCD(lcd);

    uint32_t id = 0;
    esp_err_t err = ili9342c_read_id(lcd, &id);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        TF_SKIP("MISO 未引出（GPIO%d），无法读 RDDID；"
                "屏是否在位只能靠 lcd.bars 的彩条人眼确认",
                BOARD_LCD_MISO_GPIO);
    }
    TF_CHECK_ERR(err, "读 RDDID(0x04)");

    TF_INFO("读回 ID = 0x%06" PRIX32 "（0x9342/0x9341 为 ILI934x 系列）", id);
    /* 读得到就说明 SDO 接上了，此时可以硬断言 */
    TF_CHECK((id >> 8) == 0x9342 || (id >> 8) == 0x9341,
             "读回 ID 0x%06" PRIX32 " 不是 ILI934x 系列", id);
    TF_INFO("意外收获：本板竟然能回读，说明模块 SDO 已接到 GPIO%d",
            BOARD_LCD_MISO_GPIO);
    return TF_PASS;
}

/* ---------------- 7. 彩条（人眼验收） ---------------- */

static tf_result_t t_bars(void)
{
    REQUIRE_LCD(lcd);

    void *strip = alloc_strip();
    if (strip == NULL) {
        TF_SKIP("分配条带缓冲失败，无法绘制彩条");
    }

    /* 8 条竖条，每条 40 px 宽。顺序固定，便于一眼看出红蓝是否互换
     * （互换 = MADCTL 的 BGR 位反了）以及整屏是否黑屏（背光/INVON 问题）。 */
    static const uint16_t colors[8] = {
        LCD_COLOR_RED,  LCD_COLOR_GREEN, LCD_COLOR_BLUE, LCD_COLOR_YELLOW,
        LCD_COLOR_CYAN, LCD_COLOR_MAGENTA, LCD_COLOR_WHITE, LCD_COLOR_BLACK,
    };
    static const char *names[8] = { "红", "绿", "蓝", "黄",
                                    "青", "品红", "白", "黑" };

    esp_err_t err = ESP_OK;
    for (int i = 0; i < 8 && err == ESP_OK; i++) {
        err = ili9342c_fill_rect(lcd, (uint16_t)(i * (BOARD_LCD_H_RES / 8)), 0,
                                 (uint16_t)(BOARD_LCD_H_RES / 8),
                                 BOARD_LCD_V_RES, colors[i], strip,
                                 BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * 2);
    }
    heap_caps_free(strip);

    if (err != ESP_OK) {
        TF_CHECK_ERR(err, "绘制彩条");
    }

    /* 刷完再点背光，避免看到刷屏过程的花屏 */
    ili9342c_bl_set(100);
    vTaskDelay(pdMS_TO_TICKS(50));

    char order[80];
    int pos = 0;
    for (int i = 0; i < 8 && pos < (int)sizeof(order) - 8; i++) {
        pos += snprintf(order + pos, sizeof(order) - pos, "%s ", names[i]);
    }
    TF_INFO("已刷 8 条竖彩条并点亮背光，顺序：%s", order);
    TF_INFO("若整屏无反应：先确认 FPC 是否插好、VCC 是否 3V3");

    /* 没有 MISO，代码无法判断画面对不对，只能交给人眼 —— 判 SKIP 而不是 PASS，
     * 避免报告出现"LCD 已通过"这种无法成立的结论。 */
    TF_SKIP("已刷彩条并点亮背光，需人眼确认（无 MISO 无法回读）");
}

/* ---------------- 8. 配置汇总 ---------------- */

static tf_result_t t_dump(void)
{
    REQUIRE_LCD(lcd);

    TF_INFO("控制器   : ILI9342C（用 ILI9341 兼容序列驱动）");
    TF_INFO("接口     : 4 线 SPI2，mode 0，%d kHz", lcd->pclk_khz);
    TF_INFO("管脚     : SCLK=%d MOSI=%d CS=%d DC=%d BL=%d",
            BOARD_LCD_SCLK_GPIO, BOARD_LCD_MOSI_GPIO, BOARD_LCD_CS_GPIO,
            BOARD_LCD_DC_GPIO, BOARD_LCD_BL_GPIO);
    TF_INFO("未连接   : MISO=%d（无 SDO）、RST=%d（无硬件复位）",
            BOARD_LCD_MISO_GPIO, BOARD_LCD_RST_GPIO);
    TF_INFO("分辨率   : %dx%d，RGB565，MADCTL=0x%02X", BOARD_LCD_H_RES,
            BOARD_LCD_V_RES, lcd->madctl);
    TF_INFO("背光     : %d Hz / %d bit，%s，当前 duty=%u",
            BOARD_LCD_BL_PWM_HZ, BOARD_LCD_BL_PWM_BITS,
            BOARD_LCD_BL_ACTIVE_LOW ? "低电平点亮" : "高电平点亮",
            (unsigned)ili9342c_bl_get_duty());
    TF_INFO("初始化   : 21 条命令（厂家扩展 + 电源 + Gamma），0x11 后等 120 ms");

    TF_CHECK(lcd->ready, "面板未处于就绪状态");
    return TF_PASS;
}

/* ---------------- 注册 ---------------- */

TF_GROUP_BEGIN(lcd)
    TF_ITEM(lcd, bus,       "SPI2 总线与 40 MHz 时钟",        t_bus,        TF_FLAG_HW),
    TF_ITEM(lcd, io_pins,   "DC/CS 时序与电平翻转",           t_io_pins,    TF_FLAG_HW),
    TF_ITEM(lcd, init_seq,  "软复位 + 22 条初始化命令",       t_init_seq,   TF_FLAG_HW),
    TF_ITEM(lcd, backlight, "背光 PWM 与低电平点亮极性",      t_backlight,  TF_FLAG_HW),
    TF_ITEM(lcd, fill,      "全屏填充吞吐（SPI+DMA）",        t_fill,       TF_FLAG_HW),
    TF_ITEM(lcd, read_id,   "读 RDDID 确认面板型号",          t_read_id,    TF_FLAG_HW),
    TF_ITEM(lcd, bars,      "彩条验收（需人眼）",             t_bars,       TF_FLAG_HW),
    TF_ITEM(lcd, dump,      "配置汇总",                       t_dump,       TF_FLAG_HW),
TF_GROUP_END(lcd)
