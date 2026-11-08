/**
 * @file  test_core.c
 * @brief 核心自检用例：芯片信息、堆、时钟
 *
 * 这组用例先跑，用来排除"板子本身没起来"的干扰 ——
 * 如果连堆都不够，后面的外设测试失败就没意义了。
 */
#include <inttypes.h>
#include <stdio.h>

#include "tf.h"

#include "board_config.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static tf_result_t t_chip_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    TF_INFO("型号=%d 核数=%d 版本=v%d.%d flash=%" PRIu32 " MB MAC=%02X:%02X:%02X:%02X:%02X:%02X",
            (int)info.model, info.cores, info.revision / 100, info.revision % 100,
            flash_bytes / (1024 * 1024),
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    TF_CHECK(info.model == CHIP_ESP32S3, "芯片型号 %d，期望 ESP32-S3(%d)",
             (int)info.model, (int)CHIP_ESP32S3);
    TF_CHECK(info.cores >= 1, "核数异常: %d", info.cores);
    /* 本板为 16 MB；低于 8 MB 说明模组型号不对，后续 OTA/分区方案都要改 */
    TF_CHECK(flash_bytes >= 8u * 1024u * 1024u,
             "flash 仅 %" PRIu32 " 字节，低于 8 MB", flash_bytes);
    return TF_PASS;
}

static tf_result_t t_heap(void)
{
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t free_min = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    TF_INFO("当前空闲=%u B 历史最低=%u B 最大连续块=%u B",
            (unsigned)free_now, (unsigned)free_min, (unsigned)largest);

    /* 阈值按"启动完成后不应低于"的经验值设定；低于此值说明有任务提前吃掉了内存 */
    TF_CHECK(free_now > 64u * 1024u, "空闲堆仅 %u B，低于 64 KB", (unsigned)free_now);
    TF_CHECK(largest > 16u * 1024u, "最大连续块仅 %u B，无法分配中等缓冲",
             (unsigned)largest);
    return TF_PASS;
}

static tf_result_t t_timer(void)
{
    const TickType_t ticks = pdMS_TO_TICKS(20);
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(ticks);
    int64_t dt_us = esp_timer_get_time() - t0;

    /* 阈值必须按 tick 量化来算，不能凭"20 ms 就卡 15 ms"。
     *
     * vTaskDelay(N) 的真实时长落在 ((N-1)·T, N·T] 之间（T = 1 个 tick）：
     * 任务被唤醒的时刻对齐到 tick 中断，若进入延时的那一刻刚过一个 tick
     * 边界，实际只等 (N-1) 个 tick 就到期了。
     *
     * 本板 CONFIG_FREERTOS_HZ = 100，T = 10 ms，N = 2，所以下界是 10 ms。
     * 之前把下限写成 15 ms 会造成随机 FAIL —— 实测跑出过 14297 us，
     * 这既不是 tick 配置错也不是时钟有问题，纯粹是阈值假设错了。 */
    const double tick_ms = 1000.0 / (double)configTICK_RATE_HZ;
    const double lo_ms = (ticks > 0 ? (double)(ticks - 1) : 0.0) * tick_ms;
    const double hi_ms = (double)ticks * tick_ms + 60.0; /* 60 ms 容忍日志抢占 */

    TF_INFO("vTaskDelay(%u tick) 实测 %lld us；tick=%u Hz（%.1f ms/tick）",
            (unsigned)ticks, (long long)dt_us, (unsigned)configTICK_RATE_HZ,
            tick_ms);
    TF_INFO("该调用的合法区间：%.1f ~ %.1f ms（下界 = (N-1) 个 tick）", lo_ms,
            hi_ms);

    /* 下限用严格值：低于 (N-1) 个 tick 才说明 tick 被改错或延时没生效 */
    TF_CHECK_RANGE(dt_us / 1000.0, lo_ms, hi_ms, "延时 20 ms 实测");
    return TF_PASS;
}

/**
 * PSRAM 自检。
 *
 * 为什么单独一项：摄像头一帧 JPEG 有几十 KB，DVP 收帧还要双缓冲，
 * 内部 RAM 在 WiFi 起来后只剩 200 多 KB，放不下。PSRAM 是否真的可用
 * 直接决定出图能不能做，所以必须在上电就确认，而不是等到摄像头报错。
 *
 * 板 A 没有 PSRAM（未确认型号），所以找不到时判 SKIP 而不是 FAIL ——
 * 这不是故障，是这块板没有这个器件。
 */
static tf_result_t t_psram(void)
{
    size_t total = esp_psram_get_size();

    if (total == 0) {
        TF_SKIP("未检测到 PSRAM（本板可能没有此器件）；摄像头出图功能将不可用");
    }

    size_t free_sp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    TF_INFO("PSRAM 总容量 %u KB，当前空闲 %u KB，最大连续块 %u KB",
            (unsigned)(total / 1024), (unsigned)(free_sp / 1024),
            (unsigned)(largest / 1024));

    /* 8 MB 是 ESP32-S3R8 的片上容量；模式配错（QUAD/OCT）时通常直接为 0 */
    TF_CHECK(total >= 2u * 1024u * 1024u,
             "PSRAM 只有 %u KB，与 8 MB 片上规格不符；检查 CONFIG_SPIRAM_MODE_OCT",
             (unsigned)(total / 1024));
    /* 摄像头要用 PSRAM 分配帧缓冲，连续块太小说明初始化没真正生效 */
    TF_CHECK(largest >= 1024u * 1024u,
             "PSRAM 最大连续块仅 %u KB，无法分配帧缓冲", (unsigned)(largest / 1024));
    return TF_PASS;
}

/* ---------------- 用例注册 ---------------- */
TF_GROUP_BEGIN(core)    TF_ITEM(core, chip_info, "芯片型号 / 核数 / flash 容量 / MAC", t_chip_info, TF_FLAG_NONE),
    TF_ITEM(core, heap, "堆使用情况与最大连续块", t_heap, TF_FLAG_NONE),
    TF_ITEM(core, timer, "系统时钟与 tick 精度", t_timer, TF_FLAG_NONE),
    TF_ITEM(core, psram, "PSRAM 容量与可用连续块", t_psram, TF_FLAG_NONE),
TF_GROUP_END(core)
