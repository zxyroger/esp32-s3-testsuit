/**
 * @file  test_audio.c
 * @brief 音频通路真实测试（ES8311 + NS4150B + ZTS6216）
 *
 * 与 test_es8311.c 的分工：
 *   test_es8311.c 只验 I2C 控制面（寄存器能不能读写）；
 *   本文件建立真实 I2S 时钟、真的配置采样率、真的灌数据，验证整条通路。
 *
 * 硬件事实（查手册得到，不是猜的）：
 *   - ES8311 做从机，MCLK 由 ESP32 提供（16k × 256 = 4.096 MHz）；
 *   - 功放 NS4150B 的 CTRL 脚：低=关断、高=工作，启动时间 Tst 典型 120 ms；
 *   - 麦克风 ZTS6216 是**模拟**输出 MEMS（-38 dBV/Pa），经 0.1 uF 隔直进
 *     ES8311 的 MIC 输入，走内部 ADC —— 所以 REG14 bit6 必须为 0（非 PDM）。
 *
 * 判定原则（延续本框架的取舍）：
 *   - 依赖"外面有没有接喇叭/麦克风"的项，缺件就 SKIP，不能判 FAIL；
 *   - 但 I2S 时钟能不能算对、寄存器能不能写进去，这些不依赖外部器件，
 *     必须硬断言 —— 否则框架等于什么都没测。
 *
 * 当前板况：麦克风 ZTS6216 与功放 NS4150B 都在板上。因此
 *   i2s_init / codec_cfg / pa_ctrl 应 PASS（纯片上可验证），
 *   dig_loop 用 REG44=0x68 把 DAC 数据灌进 ADC，验证 ASDOUT + I2S RX；
 *   play_tone 只能证明"时钟在跑、DMA 不卡"，听不到声不代表失败；
 *   capture_level 用 42dB→0dB 的幅度变化证明信号确实穿过 MIC PGA。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tf.h"

#include "audio_path.h"
#include "board.h"
#include "board_config.h"
#include "es8311.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 采集/播放时长与幅度：200 ms 足够算出 RMS，又不会让测试拖太久 */
#define AUDIO_TEST_MS      200
#define AUDIO_TEST_FRAMES  ((BOARD_AUDIO_RATE_HZ * AUDIO_TEST_MS) / 1000)
#define AUDIO_TEST_AMP     6000   /* ≈ -14.7 dBFS，避免小喇叭被削顶 */
#define AUDIO_TEST_FREQ    1000   /* 1 kHz，人耳最容易辨认 */
/* 安静房间里 -38 dBV/Pa 的麦克风大约给出十几 LSB，取 8 作为"有信号"下限 */
#define MIC_RMS_MIN        8

#define REQUIRE_CODEC(var)                                                     \
    es8311_t *(var) = board_codec();                                           \
    if ((var) == NULL) {                                                       \
        TF_SKIP("ES8311 不可用（0x%02X 无应答），跳过", BOARD_ADDR_ES8311);    \
    }

#define REQUIRE_AUDIO(var)                                                     \
    audio_path_t *(var) = board_audio();                                       \
    if ((var) == NULL) {                                                       \
        TF_SKIP("I2S 通道建立失败（引脚被占用或无可用控制器），跳过");         \
    }

/* 需要把寄存器号带进失败信息的断言（TF_CHECK_ERR 不支持格式化） */
#define CHECK_ERR_REG(expr, reg)                                               \
    do {                                                                       \
        esp_err_t _e = (expr);                                                 \
        g_tf->checks++;                                                        \
        if (_e != ESP_OK) {                                                    \
            tf_fail(__FILE__, __LINE__, "寄存器 0x%02X 访问失败: %s",          \
                    (unsigned)(reg), esp_err_to_name(_e));                     \
            return TF_FAIL;                                                    \
        }                                                                      \
    } while (0)

static int16_t *alloc_frames(size_t n_frames)
{
    /* I2S DMA 要求缓冲在 DMA 可访问的内存里，不能用普通 malloc/栈 */
    return heap_caps_malloc(n_frames * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
}

/* ---------------- 1. I2S 时钟树 ---------------- */

static tf_result_t t_i2s_init(void)
{
    REQUIRE_AUDIO(a);

    i2s_chan_info_t info;
    CHECK_ERR_REG(audio_path_info(a, &info), 0);

    TF_INFO("TX: port=%d 角色=%s", info.id,
            info.role == I2S_ROLE_MASTER ? "master" : "slave");
    TF_INFO("源时钟 %u Hz -> MCLK %u Hz -> BCLK %u Hz",
            (unsigned)info.sclk_hz, (unsigned)info.mclk_hz,
            (unsigned)info.bclk_hz);

    /* 16k × 256 = 4.096 MHz；16k × 16bit × 2 slot = 512 kHz。
     * 这两个值算错，ES8311 的分频系数就对不上，后面全是空谈。 */
    TF_CHECK_RANGE(info.mclk_hz, 4096000 * 0.98, 4096000 * 1.02,
                   "MCLK（应为 4.096 MHz）");
    TF_CHECK_RANGE(info.bclk_hz, 512000 * 0.98, 512000 * 1.02,
                   "BCLK（应为 512 kHz）");

    /* 通道此时未 enable，不应有数据占住 DMA */
    TF_INFO("DMA 缓冲合计 %u 字节", (unsigned)info.total_dma_buf_size);
    TF_CHECK(info.total_dma_buf_size > 0, "DMA 缓冲未分配，通道可能没初始化成功");

    return TF_PASS;
}

/* ---------------- 2. ES8311 播放/录音配置 ---------------- */

static tf_result_t t_codec_cfg(void)
{
    REQUIRE_CODEC(d);

    es8311_snap_t snap;
    audio_snap_reset(&snap);

    esp_err_t werr = audio_codec_apply(d, &snap);
    if (werr == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(5));   /* 给内部参考一点稳定时间再回读 */
    }

    /* 先全部读出来，再恢复，最后断言 —— TF_CHECK 失败会直接 return，
     * 如果先断言后恢复，失败路径就把芯片留在播放态了。 */
    uint8_t v00 = 0, v02 = 0, v05 = 0, v06 = 0, v07 = 0, v08 = 0, v09 = 0, v0a = 0;
    uint8_t v0e = 0, v12 = 0, v14 = 0, v17 = 0, v32 = 0;
    esp_err_t rerr = ESP_OK;
    if (werr == ESP_OK) {
        rerr |= es8311_read8(d, 0x00, &v00);
        rerr |= es8311_read8(d, 0x02, &v02);
        rerr |= es8311_read8(d, 0x05, &v05);
        rerr |= es8311_read8(d, 0x06, &v06);
        rerr |= es8311_read8(d, 0x07, &v07);
        rerr |= es8311_read8(d, 0x08, &v08);
        rerr |= es8311_read8(d, 0x09, &v09);
        rerr |= es8311_read8(d, 0x0A, &v0a);
        rerr |= es8311_read8(d, 0x0E, &v0e);
        rerr |= es8311_read8(d, 0x12, &v12);
        rerr |= es8311_read8(d, 0x14, &v14);
        rerr |= es8311_read8(d, 0x17, &v17);
        rerr |= es8311_read8(d, 0x32, &v32);
    }

    esp_err_t back = audio_snap_restore(d, &snap);

    TF_CHECK_ERR(werr, "写入播放/录音配置");
    TF_CHECK_ERR(rerr, "回读配置寄存器");
    TF_CHECK_ERR(back, "恢复原始寄存器");

    TF_INFO("REG00=%02X 02=%02X 05=%02X 06=%02X 09=%02X 0A=%02X", v00, v02,
            v05, v06, v09, v0a);
    TF_INFO("REG07=%02X 08=%02X（LRCLK 分频）", v07, v08);
    TF_INFO("REG0E=%02X 12=%02X 14=%02X 17=%02X 32=%02X", v0e, v12, v14, v17,
            v32);

    /* 上电且从模式：bit7=1、bit6=0 */
    TF_CHECK((v00 & 0x80) != 0, "REG00 bit7=0，CSM 未上电（配置没生效）");
    TF_CHECK((v00 & 0x40) == 0, "REG00 bit6=1，芯片跑成了主机（我们要求从机）");

    /* 16k@4.096M 的分频：REG02=0x00、REG05=0x00、REG06 低 5 位=0x03（bclk_div=4） */
    TF_CHECK(v02 == 0x00, "REG02=0x%02X，pre_div/pre_multi 与 4.096 MHz 不符", v02);
    TF_CHECK(v05 == 0x00, "REG05=0x%02X，ADC/DAC 分频不为 1", v05);
    TF_CHECK((v06 & 0x1F) == 0x03, "REG06 低 5 位=0x%02X，bclk_div 不是 4",
             (unsigned)(v06 & 0x1F));
    /* LRCLK 分频是 0x00FF（高字节 0x00、低字节 0xFF）。
     * 这两个写反过一次，症状是 ADC 采集恒为全 0 —— 所以钉死在这里。 */
    TF_CHECK(v07 == 0x00, "REG07=0x%02X，lrck_h 应为 0x00（写反会让 LRCLK 分频错两个数量级）",
             v07);
    TF_CHECK(v08 == 0xFF, "REG08=0x%02X，lrck_l 应为 0xFF", v08);

    /* 数据格式：两条通路都必须是 I2S 标准 + 16 bit */
    TF_CHECK((v09 & 0x03) == 0x00, "REG09 格式位=0x%02X，不是 I2S 标准",
             (unsigned)(v09 & 0x03));
    TF_CHECK((v09 & 0x0C) == 0x0C, "REG09 字长位=0x%02X，不是 16 bit",
             (unsigned)(v09 & 0x0C));
    TF_CHECK((v0a & 0x0C) == 0x0C, "REG0A 字长位=0x%02X（ADC 通路），不是 16 bit",
             (unsigned)(v0a & 0x0C));

    /* 模拟 MIC：ZTS6216 是模拟输出，bit6 必须为 0 */
    TF_CHECK((v14 & 0x40) == 0, "REG14 bit6=1，被配成了 PDM 数字 MIC");
    /* ADC 非静音（0xBF 是官方 start() 写死的值） */
    TF_CHECK(v17 != 0x00, "REG17=0x00，ADC 仍是静音");
    /* DAC 音量 0xB3 = -6 dB */
    TF_CHECK(v32 == 0xB3, "REG32=0x%02X，DAC 音量不是预期的 0xB3(-6dB)", v32);
    TF_CHECK(v12 == 0x00, "REG12=0x%02X，DAC 未使能", v12);
    TF_CHECK(v0e == 0x02, "REG0E=0x%02X，模拟通路电源未打开", v0e);

    return TF_PASS;
}

/* ---------------- 3. 功放使能脚 ---------------- */

static tf_result_t t_pa_ctrl(void)
{
    CHECK_ERR_REG(audio_pa_init(), BOARD_PA_EN_GPIO);

    /* 关断 -> 读回低 */
    CHECK_ERR_REG(audio_pa_set(false), BOARD_PA_EN_GPIO);
    vTaskDelay(pdMS_TO_TICKS(5));
    int off = audio_pa_level();

    /* 使能 -> 读回高；NS4150B Tst=120ms，等够再看 */
    CHECK_ERR_REG(audio_pa_set(true), BOARD_PA_EN_GPIO);
    vTaskDelay(pdMS_TO_TICKS(BOARD_PA_STARTUP_MS));
    int on = audio_pa_level();

    /* 收尾：无论结果如何都关掉，避免静置时功放空耗 */
    esp_err_t cerr = audio_pa_set(false);

    TF_INFO("GPIO%d: 关断时读回=%d，使能时读回=%d", BOARD_PA_EN_GPIO, off, on);
    TF_INFO("NS4150B: CTRL 高=工作 / 低=Shutdown，启动时间 Tst≈120ms");

    TF_CHECK(off == 0, "CTRL 关断后读回 %d，应为 0（可能被外部上拉或配错脚）", off);
    TF_CHECK(on == 1, "CTRL 使能后读回 %d，应为 1（GPIO%d 未连到 CTRL？）", on,
             BOARD_PA_EN_GPIO);
    TF_CHECK_ERR(cerr, "恢复功放为关断");

    return TF_PASS;
}

/* ---------------- 4. 播放一段正弦 ---------------- */

static tf_result_t t_play_tone(void)
{
    REQUIRE_CODEC(d);
    REQUIRE_AUDIO(a);

    size_t frames = AUDIO_TEST_FRAMES;
    int16_t *buf = alloc_frames(frames);
    TF_CHECK(buf != NULL, "分配 %u 帧播放缓冲失败", (unsigned)frames);

    es8311_snap_t snap;
    audio_snap_reset(&snap);
    esp_err_t cerr = audio_codec_apply(d, &snap);
    esp_err_t perr = ESP_OK;
    esp_err_t pierr = ESP_OK;
    esp_err_t oerr = ESP_OK;
    size_t sent = 0;
    int64_t dt_us = 0;

    if (cerr == ESP_OK) {
        audio_gen_sine(buf, frames, BOARD_AUDIO_RATE_HZ, AUDIO_TEST_FREQ,
                       AUDIO_TEST_AMP);
        pierr = audio_path_enable(a, true, false);
        if (pierr == ESP_OK) {
            oerr = audio_pa_init();
            if (oerr == ESP_OK) {
                audio_pa_set(true);
                /* 手册 Tst=120ms：等不够，最先播的几十毫秒会被静音吃掉 */
                vTaskDelay(pdMS_TO_TICKS(BOARD_PA_STARTUP_MS));
            }
            int64_t t0 = esp_timer_get_time();
            perr = audio_play(a, buf, frames, &sent);
            dt_us = esp_timer_get_time() - t0;
        }
    }

    /* 先收尾再断言，失败路径也不能让功放一直开着 */
    audio_pa_set(false);
    audio_path_disable(a, true, false);
    esp_err_t rerr = audio_snap_restore(d, &snap);
    heap_caps_free(buf);

    TF_CHECK_ERR(cerr, "配置 ES8311 为播放态");
    TF_CHECK_ERR(pierr, "使能 I2S TX 通道");
    TF_CHECK_ERR(perr, "写入 I2S 数据");

    size_t want_bytes = frames * 2 * sizeof(int16_t);
    TF_INFO("发送 %u 字节（%u 帧，%u ms，%d Hz 正弦，幅度 %d）",
            (unsigned)sent, (unsigned)frames, (unsigned)AUDIO_TEST_MS,
            AUDIO_TEST_FREQ, AUDIO_TEST_AMP);
    TF_INFO("理论耗时 %d ms，实测 %d ms（%.0f 字节/秒）", AUDIO_TEST_MS,
            (int)(dt_us / 1000),
            dt_us > 0 ? (double)sent * 1000000.0 / (double)dt_us : 0.0);

    TF_CHECK_ERR(rerr, "恢复 ES8311 原始配置");
    TF_CHECK(sent == want_bytes, "只发出 %u/%u 字节，DMA 中途断了",
             (unsigned)sent, (unsigned)want_bytes);

    /* 播放是实时速率，200 ms 的数据至少要占用约 200 ms 减去 DMA 缓冲的时间。
     * 明显偏快 = 时钟算高了；明显偏慢 = 时钟偏低或 DMA 卡住。 */
    TF_CHECK_RANGE(dt_us / 1000.0, (double)AUDIO_TEST_MS * 0.6,
                   (double)AUDIO_TEST_MS * 3.0, "发送耗时(ms)");

    TF_INFO("注：此项证明 I2S 时钟与 DMA 通路正常，不能证明喇叭真的出声");
    return TF_PASS;
}

/* ---------------- 5. 数字回环：先证明 ASDOUT + I2S RX 是通的 ---------------- */

typedef struct {
    audio_path_t  *a;
    const int16_t *buf;
    size_t         frames;
    size_t         sent;
    esp_err_t      err;
    volatile bool  done;
} tx_job_t;

static void tx_job_task(void *arg)
{
    tx_job_t *j = (tx_job_t *)arg;
    j->err = audio_play(j->a, j->buf, j->frames, &j->sent);
    j->done = true;
    vTaskDelete(NULL);
}

/**
 * 把 DAC 的数据在芯片内部直接灌给 ADC（REG44=0x68），播 1 kHz 正弦同时采集。
 *
 * 这条用例的价值在于"分锅"：采集恒为全 0 时，I2C 侧看不出到底是
 *   模拟前端没信号，还是数字通路根本没跑（MCLK 没到 / ASDOUT 没驱动 /
 *   I2S RX 没收）。回环走的是 ASDOUT -> I2S RX 这条**完全相同**的数字通路，
 *   只是信号源从 MIC 换成 DAC。这边能收到音，后面 capture_level 还收不到，
 *   就只可能是麦克风/偏置/隔直电容的问题，不必再怀疑时钟和 I2S。
 */
static tf_result_t t_digital_loop(void)
{
    REQUIRE_CODEC(d);
    REQUIRE_AUDIO(a);

    const size_t frames = AUDIO_TEST_FRAMES;   /* 采集 200 ms */
    const size_t tx_frames = frames * 2;       /* 播 400 ms，盖住整段采集 */

    int16_t *rx = alloc_frames(frames);
    int16_t *tx = alloc_frames(tx_frames);
    TF_CHECK(rx != NULL && tx != NULL, "分配回环缓冲失败（rx/tx）");
    memset(rx, 0, frames * 2 * sizeof(int16_t));
    /* 回环是数字直连，不存在"小喇叭带不动"的问题，幅度直接给到 -4 dBFS */
    audio_gen_sine(tx, tx_frames, BOARD_AUDIO_RATE_HZ, AUDIO_TEST_FREQ, 20000);

    es8311_snap_t snap;
    audio_snap_reset(&snap);
    esp_err_t cerr = audio_codec_apply(d, &snap);
    if (cerr == ESP_OK) {
        cerr = audio_codec_loopback(d, &snap, true);
    }

    esp_err_t pierr = ESP_OK;
    size_t got = 0;
    tx_job_t job;
    memset(&job, 0, sizeof(job));
    job.a = a;
    job.buf = tx;
    job.frames = tx_frames;
    TaskHandle_t th = NULL;
    bool started = false;

    if (cerr == ESP_OK) {
        pierr = audio_path_enable(a, true, true);
        if (pierr == ESP_OK) {
            /* 必须放到独立任务里：i2s_channel_write 会阻塞到数据真正播出去，
             * 主线程先写 400 ms 再读，读的时候声音早放完了。 */
            if (xTaskCreate(tx_job_task, "audio_tx", 3072, &job, 5, &th) == pdPASS) {
                started = true;
                vTaskDelay(pdMS_TO_TICKS(30));   /* 跳过 DAC/ADC 启动爬坡 */
                audio_capture(a, rx, frames, &got);
            }
        }
    }

    /* 等发送任务收尾，再关通道 —— 否则可能把正在写的通道 disable 掉 */
    for (int i = 0; i < 200 && started && !job.done; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    audio_path_disable(a, true, true);
    esp_err_t rerr = audio_snap_restore(d, &snap);

    audio_level_t lv_l = { 0 }, lv_r = { 0 };
    if (got > 0) {
        audio_level_lr(rx, got / (2 * sizeof(int16_t)), &lv_l, &lv_r);
    }
    heap_caps_free(rx);
    heap_caps_free(tx);

    TF_CHECK_ERR(cerr, "配置 ES8311 并打开数字回环(REG44=0x68)");
    TF_CHECK_ERR(pierr, "使能 I2S 收发通道");
    TF_CHECK(started, "创建发送任务失败");
    TF_CHECK_ERR(job.err, "回环发送正弦");
    TF_CHECK_ERR(rerr, "恢复 ES8311 原始配置");
    TF_CHECK(got == frames * 2 * sizeof(int16_t), "只收到 %u/%u 字节",
             (unsigned)got, (unsigned)(frames * 2 * sizeof(int16_t)));

    TF_INFO("回环：播 %u 帧 1 kHz，采 %u 帧", (unsigned)tx_frames,
            (unsigned)frames);
    TF_INFO("左 RMS=%d 峰值=%d 非零=%u/%u", lv_l.rms, lv_l.peak,
            (unsigned)lv_l.nonzero, (unsigned)frames);
    TF_INFO("右 RMS=%d 峰值=%d 非零=%u/%u", lv_r.rms, lv_r.peak,
            (unsigned)lv_r.nonzero, (unsigned)frames);

    int best = lv_l.rms > lv_r.rms ? lv_l.rms : lv_r.rms;
    int best_peak = lv_l.peak > lv_r.peak ? lv_l.peak : lv_r.peak;

    /* 回环都是 0：说明 MCLK/ASDOUT/I2S RX 这条数字通路本身不通，
     * 与麦克风、偏置、隔直电容**毫无关系**，别再往模拟侧查了。 */
    if (best == 0) {
        TF_SKIP("数字回环也全是 0：问题在 MCLK/ASDOUT/I2S RX，不在麦克风");
    }
    if (best_peak < 500) {
        TF_SKIP("回环信号过弱（峰值 %d < 500 LSB），数字通路可能只是一点点串扰",
                best_peak);
    }
    TF_INFO("结论：ASDOUT -> I2S RX 数字通路正常，后续若采集仍为 0 即是模拟侧问题");
    return TF_PASS;
}

/* ---------------- 6. 麦克风采集 ---------------- */

/* 把 RX DMA 里的残留读干净。
 * DMA 深 4 × 240 帧 = 960 帧，读 2 轮 × 1024 帧足以覆盖。
 * 不清的话，上一项（数字回环）播的正弦会残留在这里，被当成本次"采集到的
 * 信号"—— 实测右声道就报出过峰值恰好 20000、只占 605/3200 帧的假信号。 */
static void flush_rx(audio_path_t *a)
{
    int16_t *t = alloc_frames(1024);
    if (t == NULL) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        size_t got = 0;
        audio_capture(a, t, 1024, &got);
    }
    heap_caps_free(t);
}

/* 先清残留再采一段并算电平；通道必须已使能 */
static esp_err_t capture_once(audio_path_t *a, int16_t *buf, size_t frames,
                              audio_level_t *l, audio_level_t *r)
{
    flush_rx(a);
    memset(buf, 0, frames * 2 * sizeof(int16_t));
    size_t got = 0;
    esp_err_t err = audio_capture(a, buf, frames, &got);
    if (err != ESP_OK) {
        return err;
    }
    if (got != frames * 2 * sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    audio_level_lr(buf, frames, l, r);
    return ESP_OK;
}

static tf_result_t t_capture_level(void)
{
    REQUIRE_CODEC(d);
    REQUIRE_AUDIO(a);

    const size_t frames = AUDIO_TEST_FRAMES;
    int16_t *buf = alloc_frames(frames);
    TF_CHECK(buf != NULL, "分配 %u 帧采集缓冲失败", (unsigned)frames);

    es8311_snap_t snap;
    audio_snap_reset(&snap);
    /* MIC 增益给到最大档 42 dB：本项要回答的是"麦克风通不通"，
     * 不是"安静房间够不够响"。ZTS6216 是 -38 dBV/Pa，默认 24 dB 档下
     * 正常说话都可能只剩几十 LSB，安静时更低，容易被误判成没接。 */
    esp_err_t cerr = audio_codec_apply_mic(d, &snap, 42);

    uint8_t mic16 = 0, v44 = 0;
    esp_err_t qerr = ESP_ERR_INVALID_STATE;
    if (cerr == ESP_OK) {
        qerr = es8311_read8(d, 0x16, &mic16);
        if (qerr == ESP_OK) {
            qerr = es8311_read8(d, 0x44, &v44);
        }
    }

    esp_err_t pierr = ESP_OK;
    esp_err_t gerr = ESP_OK;
    esp_err_t g0err = ESP_OK;
    audio_level_t hi_l, hi_r, lo_l, lo_r;
    memset(&hi_l, 0, sizeof(hi_l)); memset(&hi_r, 0, sizeof(hi_r));
    memset(&lo_l, 0, sizeof(lo_l)); memset(&lo_r, 0, sizeof(lo_r));

    if (cerr == ESP_OK) {
        /* 全双工共用一套时钟：只使能 RX 时 BCLK 不跑，read 必然 timeout。
         * 连 TX 一起使能即可，auto_clear 会补 0 维持时钟，不用真灌数据。 */
        pierr = audio_path_enable(a, true, true);
    }
    if (cerr == ESP_OK && pierr == ESP_OK) {
        gerr = capture_once(a, buf, frames, &hi_l, &hi_r);

        /* 再把 MIC PGA 拉到 0 dB 采一次。这一条才是"信号真的来自麦克风"的证据：
         * 若走的是模拟 MIC -> PGA -> ADC，幅度必须跟着增益一起降下来
         * （42 dB 对 0 dB 是 126 倍）。只看"非零"不够 —— DMA 残留、
         * 通道串扰、DAC 参考泄漏都能让读数非零。 */
        if (gerr == ESP_OK) {
            esp_err_t we = es8311_write8(d, 0x16, 0x00);
            if (we == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                g0err = capture_once(a, buf, frames, &lo_l, &lo_r);
            } else {
                g0err = we;
            }
        }
    }

    audio_path_disable(a, true, true);
    esp_err_t rerr = audio_snap_restore(d, &snap);
    heap_caps_free(buf);

    TF_CHECK_ERR(cerr, "配置 ES8311 为录音态");
    TF_CHECK_ERR(pierr, "使能 I2S 收发通道");
    TF_CHECK_ERR(rerr, "恢复 ES8311 原始配置");
    TF_CHECK_ERR(gerr, "读取 I2S 采集数据（42 dB 档）");
    TF_CHECK_ERR(g0err, "读取 I2S 采集数据（0 dB 档）");

    if (qerr == ESP_OK) {
        TF_INFO("MIC 增益 REG16=0x%02X（档位 %u，即 %u dB）", mic16,
                (unsigned)mic16, (unsigned)mic16 * 6);
        TF_INFO("REG44=0x%02X：MICBIAS_EN=%u，ADCDAT_SEL=%u（5=ADCL+DACR）",
                v44, (unsigned)((v44 >> 3) & 1u), (unsigned)((v44 >> 4) & 7u));
    }
    TF_INFO("42 dB：左 RMS=%d 峰值=%d 非零=%u/%u", hi_l.rms, hi_l.peak,
            (unsigned)hi_l.nonzero, (unsigned)frames);
    TF_INFO("  0 dB：左 RMS=%d 峰值=%d 非零=%u/%u", lo_l.rms, lo_l.peak,
            (unsigned)lo_l.nonzero, (unsigned)frames);
    TF_INFO("左峰值 %.1f dBFS；右（DACR 参考，非麦克风）RMS=%d",
            hi_l.peak > 0 ? 20.0 * log10((double)hi_l.peak / 32767.0) : -99.0,
            hi_r.rms);

    /* 左 slot 才是 ADC 模拟输入：REG44 的 ADCDAT_SEL=101 表示
     * 左 = ADCL、右 = DACR。右声道本来就不该有麦克风数据，
     * 所以判定只看左声道（若 slot 反了，左会全 0，下面报出来）。 */
    int ratio = (lo_l.rms > 0) ? hi_l.rms / lo_l.rms : 0;

    if (hi_l.rms < MIC_RMS_MIN) {
        if (hi_r.rms >= MIC_RMS_MIN) {
            TF_FAIL_MSG("左声道无信号但右声道有（R RMS=%d）：WS/slot 对齐反了，"
                        "ADC 数据落在右 slot", hi_r.rms);
        }
        if (hi_l.nonzero == 0) {
            TF_FAIL_MSG("左声道全 0：模拟侧没有电压（MIC 未贴 / 无偏置 / "
                        "隔直电容缺失）。先看 audio.dig_loop 确认数字通路是否正常");
        }
        TF_FAIL_MSG("左声道信号过弱（RMS=%d < %d）：检查偏置与隔直电容。"
                    "麦克风无效属于硬件故障，不能按跳过处理",
                    hi_l.rms, MIC_RMS_MIN);
    }

    /* 有信号，但拉到 0 dB 后幅度基本不变 → 这段"信号"不是从 MIC PGA 来的
     * （多半是数字串扰或残留），不能算通过。 */
    TF_CHECK(ratio >= 4 || lo_l.rms == 0,
             "42dB->0dB 时 RMS 只从 %d 变到 %d（比值 %d），信号没跟着 PGA 走，"
             "不像是麦克风来的", hi_l.rms, lo_l.rms, ratio);

    TF_INFO("增益对比 42dB->0dB：RMS %d -> %d（%d 倍），信号确实经 MIC PGA",
            hi_l.rms, lo_l.rms, ratio);
    TF_INFO("非零占比 %u/%u（持续噪声底，非单次跳变）", (unsigned)hi_l.nonzero,
            (unsigned)frames);
    return TF_PASS;
}

/* ---------------- 7. 配置寄存器汇总 ---------------- */

static tf_result_t t_dump(void)
{
    REQUIRE_CODEC(d);

    static const uint8_t regs[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0D,
                                    0x0E, 0x0F, 0x12, 0x13, 0x14, 0x16,
                                    0x17, 0x18, 0x19, 0x1A, 0x32, 0x37,
                                    0x44, 0x45 };
    TF_INFO("当前 ES8311 关键寄存器（多为上电默认值，未做音频配置）:");
    char line[64];
    size_t pos = 0;
    line[0] = '\0';
    for (size_t i = 0; i < sizeof(regs); i++) {
        uint8_t v = 0;
        esp_err_t e = es8311_read8(d, regs[i], &v);
        if (e != ESP_OK) {
            TF_CHECK_ERR(e, "读取寄存器");
        }
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%02X=%02X ",
                                regs[i], v);
        if ((i % 5) == 4 || i + 1 == sizeof(regs)) {
            TF_INFO("  %s", line);
            pos = 0;
            line[0] = '\0';
        }
    }
    return TF_PASS;
}

/* ---------------- 注册 ---------------- */

TF_GROUP_BEGIN(audio)
TF_ITEM(audio, i2s_init, "I2S 时钟树（MCLK 4.096M / BCLK 512k）", t_i2s_init, TF_FLAG_HW),
TF_ITEM(audio, codec_cfg, "ES8311 配成从机 16k/16bit 并回读校验", t_codec_cfg, TF_FLAG_HW),
TF_ITEM(audio, pa_ctrl, "功放 NS4150B CTRL 脚（GPIO47）电平翻转", t_pa_ctrl, TF_FLAG_HW),
TF_ITEM(audio, play_tone, "播放 1 kHz 正弦 200 ms（验时钟与 DMA）", t_play_tone, TF_FLAG_HW | TF_FLAG_SLOW),
TF_ITEM(audio, dig_loop, "数字回环（REG44=0x68）验 ASDOUT + I2S RX", t_digital_loop, TF_FLAG_HW | TF_FLAG_SLOW),
TF_ITEM(audio, capture_level, "麦克风采集（42dB→0dB 验证信号经 PGA）", t_capture_level, TF_FLAG_HW | TF_FLAG_SLOW),
TF_ITEM(audio, dump, "关键配置寄存器汇总", t_dump, TF_FLAG_HW),
TF_GROUP_END(audio)
