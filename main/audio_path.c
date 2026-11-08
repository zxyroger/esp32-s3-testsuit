/**
 * @file  audio_path.c
 * @brief 音频通路实现（I2S 主模式 + ES8311 从模式 + NS4150B 使能）
 *
 * 寄存器配置逐条对应 esp_codec_dev 的 device/es8311/es8311.c：
 *   es8311_open()           -> 第 1、2、4(部分) 段
 *   es8311_config_sample()  -> 第 3 段（16k @ 4.096MHz 的系数行）
 *   es8311_start()          -> 第 5 段
 *   es8311_config_fmt/set_bits_per_sample() -> 第 4 段（0x09/0x0A 的位处理）
 */
#include "audio_path.h"

#include <math.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_path";

/* ES8311 常用寄存器（官方 es8311_reg.h 的编号，这里只列本文件用到的） */
#define R_RESET     0x00
#define R_CLK01     0x01
#define R_CLK02     0x02
#define R_CLK03     0x03
#define R_CLK04     0x04
#define R_CLK05     0x05
#define R_CLK06     0x06
#define R_CLK07     0x07
#define R_CLK08     0x08
#define R_SDPIN09   0x09
#define R_SDPOUT0A  0x0A
#define R_SYS0B     0x0B
#define R_SYS0C     0x0C
#define R_SYS0D     0x0D
#define R_SYS0E     0x0E
#define R_SYS10     0x10
#define R_SYS11     0x11
#define R_SYS12     0x12
#define R_SYS13     0x13
#define R_SYS14     0x14
#define R_ADC15     0x15
#define R_ADC16     0x16
#define R_ADC17     0x17
#define R_ADC1B     0x1B
#define R_ADC1C     0x1C
#define R_DAC32     0x32
#define R_DAC37     0x37
#define R_GP44      0x44
#define R_GP45      0x45

/* ---------------- I2S ---------------- */

esp_err_t audio_path_init(audio_path_t *a)
{
    if (a == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (a->ready) {
        return ESP_OK;
    }
    memset(a, 0, sizeof(*a));

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 240;
    /* 发送队列空了自动补 0，否则 DMA 会把上一段波形循环播下去 */
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &a->tx, &a->rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)BOARD_I2S_MCK_GPIO,
            .bclk = (gpio_num_t)BOARD_I2S_BCLK_GPIO,
            .ws   = (gpio_num_t)BOARD_I2S_WS_GPIO,
            .dout = (gpio_num_t)BOARD_I2S_DOUT_GPIO,
            .din  = (gpio_num_t)BOARD_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* 16k × 256 = 4.096 MHz，ES8311 系数表里有这一档，两边才能对上 */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(a->tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(a->rx, &std_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "初始化 std 模式失败: %s", esp_err_to_name(err));
        if (a->tx) {
            i2s_del_channel(a->tx);
        }
        if (a->rx) {
            i2s_del_channel(a->rx);
        }
        a->tx = NULL;
        a->rx = NULL;
        return err;
    }

    a->ready = true;
    ESP_LOGI(TAG, "I2S 就绪: MCLK=GPIO%d BCLK=GPIO%d WS=GPIO%d DOUT=GPIO%d DIN=GPIO%d",
             BOARD_I2S_MCK_GPIO, BOARD_I2S_BCLK_GPIO, BOARD_I2S_WS_GPIO,
             BOARD_I2S_DOUT_GPIO, BOARD_I2S_DIN_GPIO);
    return ESP_OK;
}

esp_err_t audio_path_deinit(audio_path_t *a)
{
    if (a == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!a->ready) {
        return ESP_OK;
    }
    if (a->tx) {
        i2s_channel_disable(a->tx);
        i2s_del_channel(a->tx);
        a->tx = NULL;
    }
    if (a->rx) {
        i2s_channel_disable(a->rx);
        i2s_del_channel(a->rx);
        a->rx = NULL;
    }
    a->ready = false;
    return ESP_OK;
}

esp_err_t audio_path_enable(audio_path_t *a, bool tx, bool rx)
{
    if (a == NULL || !a->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (tx) {
        err = i2s_channel_enable(a->tx);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (rx) {
        err = i2s_channel_enable(a->rx);
    }
    return err;
}

esp_err_t audio_path_disable(audio_path_t *a, bool tx, bool rx)
{
    if (a == NULL || !a->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (tx) {
        err = i2s_channel_disable(a->tx);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (rx) {
        err = i2s_channel_disable(a->rx);
    }
    return err;
}

esp_err_t audio_path_info(const audio_path_t *a, i2s_chan_info_t *out)
{
    if (a == NULL || out == NULL || !a->ready) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return i2s_channel_get_info(a->tx, out);
}

/* ---------------- 寄存器快照 ---------------- */

void audio_snap_reset(es8311_snap_t *s)
{
    if (s != NULL) {
        s->n = 0;
    }
}

static esp_err_t snap_keep(es8311_t *d, es8311_snap_t *s, uint8_t reg)
{
    if (s == NULL) {
        return ESP_OK;
    }
    for (uint8_t i = 0; i < s->n; i++) {
        if (s->reg[i] == reg) {
            return ESP_OK;   /* 只记第一次写入前的值 */
        }
    }
    if (s->n >= AUDIO_SNAP_MAX) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t v = 0;
    esp_err_t err = es8311_read8(d, reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    s->reg[s->n] = reg;
    s->val[s->n] = v;
    s->n++;
    return ESP_OK;
}

static esp_err_t wr(es8311_t *d, es8311_snap_t *s, uint8_t reg, uint8_t val)
{
    esp_err_t err = snap_keep(d, s, reg);
    if (err != ESP_OK) {
        return err;
    }
    return es8311_write8(d, reg, val);
}

esp_err_t audio_codec_apply(es8311_t *codec, es8311_snap_t *snap)
{
    if (codec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t e = ESP_OK;
    uint8_t v = 0;

    /* ---- 1. 对应 es8311_open() 的前半段：先关内部参考，避免切换时爆音 ---- */
    e = wr(codec, snap, R_SYS0D, 0xFA); if (e) return e;
    e = wr(codec, snap, R_GP44, 0x08);  if (e) return e;
    /* 官方注释：第一次 I2C 写常失败，连写两次保证可靠 */
    e = wr(codec, snap, R_GP44, 0x08);  if (e) return e;

    e = wr(codec, snap, R_CLK01, 0x30); if (e) return e;
    e = wr(codec, snap, R_CLK02, 0x00); if (e) return e;
    e = wr(codec, snap, R_CLK03, 0x10); if (e) return e;
    e = wr(codec, snap, R_ADC16, 0x24); if (e) return e;   /* MIC 增益出厂档 */
    e = wr(codec, snap, R_CLK04, 0x10); if (e) return e;
    e = wr(codec, snap, R_CLK05, 0x00); if (e) return e;
    e = wr(codec, snap, R_SYS0B, 0x00); if (e) return e;
    e = wr(codec, snap, R_SYS0C, 0x00); if (e) return e;
    e = wr(codec, snap, R_SYS10, 0x1F); if (e) return e;
    e = wr(codec, snap, R_SYS11, 0x7F); if (e) return e;

    /* ---- 2. CSM 上电 + 从模式（bit7=1，bit6=0） ---- */
    e = wr(codec, snap, R_RESET, 0x80); if (e) return e;
    /* 用外部 MCLK（bit7=0）、MCLK 不反相（bit6=0） */
    e = wr(codec, snap, R_CLK01, 0x3F); if (e) return e;

    /* SCLK 不反相：只清 bit5，其它位保留 */
    if ((e = es8311_read8(codec, R_CLK06, &v)) != ESP_OK) return e;
    e = wr(codec, snap, R_CLK06, (uint8_t)(v & ~0x20u)); if (e) return e;

    e = wr(codec, snap, R_SYS13, 0x10); if (e) return e;
    e = wr(codec, snap, R_ADC1B, 0x0A); if (e) return e;
    e = wr(codec, snap, R_ADC1C, 0x6A); if (e) return e;
    /* 内部参考取 ADCL + DACR 的平均，官方默认 */
    e = wr(codec, snap, R_GP44, 0x58);  if (e) return e;

    /* ---- 3. 采样率 16 kHz @ MCLK 4.096 MHz ----
     * 系数行（coeff_div 中 {4096000, 16000, ...} 那一行）：
     *   pre_div=0x01 pre_multi=x1 adc_div=1 dac_div=1 fs_mode=0
     *   lrck_h=0x00 lrck_l=0xFF bclk_div=0x04 adc_osr=0x10 dac_osr=0x20
     *
     * 注意 lrck 是「高字节 0x00 / 低字节 0xFF」，bclk_div=4 而不是 16。
     * 早先按「16k×16bit×2slot = 512kHz，4.096M/512k = 8」自己算了一遍，
     * 把 REG07/REG08 写成 0xFF/0x04 并令 bclk=16 —— 结果 LRCLK 分频差了
     * 两个数量级，ADC 一个采样都出不来（采集恒为全 0）。系数表是权威，
     * 不要在旁边另算一套。
     *
     * 同时按官方 config_sample() 的写法做 read-modify-write：
     * REG02/03/04/06/07 的高位不归本字段管，整字节写会连带改掉别的配置。 */
    e = es8311_read8(codec, R_CLK02, &v); if (e) return e;
    e = wr(codec, snap, R_CLK02, (uint8_t)((v & 0x07u) | ((0x01 - 1) << 5) |
                                          (0 << 3)));          if (e) return e;
    e = wr(codec, snap, R_CLK05,
            (uint8_t)(((0x01 - 1) << 4) | (0x01 - 1)));        if (e) return e;

    e = es8311_read8(codec, R_CLK03, &v); if (e) return e;
    e = wr(codec, snap, R_CLK03, (uint8_t)((v & 0x80u) | (0 << 6) | 0x10));
    if (e) return e;

    e = es8311_read8(codec, R_CLK04, &v); if (e) return e;
    e = wr(codec, snap, R_CLK04, (uint8_t)((v & 0x80u) | 0x20));  if (e) return e;

    e = es8311_read8(codec, R_CLK07, &v); if (e) return e;
    e = wr(codec, snap, R_CLK07, (uint8_t)((v & 0xC0u) | 0x00));  if (e) return e;
    e = wr(codec, snap, R_CLK08, 0xFF);                            if (e) return e;

    /* bclk_div=4 -> 寄存器写 3；小于 19 时官方写的是 (div-1) */
    e = es8311_read8(codec, R_CLK06, &v); if (e) return e;
    e = wr(codec, snap, R_CLK06, (uint8_t)((v & 0xE0u) | (0x04 - 1))); if (e) return e;

    /* ---- 4. 数据格式：I2S 标准(00) + 16 bit(bit3:2=11)，两条通路都打开 ---- */
    uint8_t d9 = 0, da = 0;
    if ((e = es8311_read8(codec, R_SDPIN09, &d9)) != ESP_OK) return e;
    if ((e = es8311_read8(codec, R_SDPOUT0A, &da)) != ESP_OK) return e;
    d9 = (uint8_t)(((d9 & ~0x03u) | 0x0Cu) & ~0x40u);
    da = (uint8_t)(((da & ~0x03u) | 0x0Cu) & ~0x40u);
    e = wr(codec, snap, R_SDPIN09, d9); if (e) return e;
    e = wr(codec, snap, R_SDPOUT0A, da); if (e) return e;

    /* ---- 5. 对应 es8311_start()：解静音、开 ADC/DAC、选模拟 MIC ---- */
    e = wr(codec, snap, R_ADC17, 0xBF); if (e) return e;   /* ADC 音量拉满、非静音 */
    e = wr(codec, snap, R_SYS0E, 0x02); if (e) return e;
    e = wr(codec, snap, R_SYS12, 0x00); if (e) return e;   /* DAC 使能 */
    /* bit6=0：模拟 MIC。ZTS6216 是模拟输出，不能开 PDM 数字 MIC */
    e = wr(codec, snap, R_SYS14, 0x1A); if (e) return e;
    e = wr(codec, snap, R_SYS0D, 0x01); if (e) return e;
    e = wr(codec, snap, R_ADC15, 0x40); if (e) return e;
    e = wr(codec, snap, R_DAC37, 0x08); if (e) return e;
    e = wr(codec, snap, R_GP45, 0x00);  if (e) return e;

    /* DAC 音量：官方映射 0x00=-95.5 dB、0xFF=+32 dB（0.5 dB/步），
     * 所以 0 dB = 0xBF；这里取 -6 dB（0xB3），避免小喇叭削顶失真。 */
    e = wr(codec, snap, R_DAC32, 0xB3); if (e) return e;
    return ESP_OK;
}

esp_err_t audio_codec_apply_mic(es8311_t *codec, es8311_snap_t *snap, int db)
{
    esp_err_t e = audio_codec_apply(codec, snap);
    if (e != ESP_OK) {
        return e;
    }
    /* 档位枚举：0=0dB、1=6dB … 7=42dB（官方 es8311_mic_gain_t）。
     * 官方 es8311_set_mic_gain() 是**整字节**写档位（如 30dB 写 0x05），
     * ai-esp32 的量产固件也正是走这条路（esp_codec_dev_set_in_gain(30)
     * -> REG16=0x05）。所以这里照抄，不做 read-modify-write —— 保留高 4 位
     * 反而可能写进保留位。 */
    int step = db / 6;
    if (step < 0) {
        step = 0;
    }
    if (step > 7) {
        step = 7;
    }
    return wr(codec, snap, R_ADC16, (uint8_t)step);
}

esp_err_t audio_codec_loopback(es8311_t *codec, es8311_snap_t *snap,
                               bool enable)
{
    if (codec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* bit6:4 = ADCDAT_SEL，bit3 = MICBIAS_EN（保持 1，麦克风偏置一直开着）
     *   0x58 -> 101 = ADCL + DACR（正常录音：左声道取 ADC，右声道取 DAC 参考）
     *   0x68 -> 110 = DACL + DACR（数字回环：ADC 直接拿到 DAC 数据）
     * 官方驱动只用 0x58/0x08 两种，0x68 这条来自同型号芯片的实测记录：
     * 回环时能稳定收到播放的正弦，证明 ASDOUT + I2S RX + 采集 DSP 都正常。 */
    return wr(codec, snap, R_GP44, enable ? 0x68 : 0x58);
}

esp_err_t audio_snap_restore(es8311_t *codec, es8311_snap_t *snap)
{
    if (codec == NULL || snap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t first = ESP_OK;
    /* 逆序写回：先恢复后写的寄存器，尽量回到最初状态 */
    for (int i = (int)snap->n - 1; i >= 0; i--) {
        esp_err_t e = es8311_write8(codec, snap->reg[i], snap->val[i]);
        if (e != ESP_OK && first == ESP_OK) {
            first = e;
        }
    }
    return first;
}

/* ---------------- 功放 ---------------- */

esp_err_t audio_pa_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_PA_EN_GPIO,
        /* 必须是 INPUT_OUTPUT：纯 OUTPUT 模式会关掉输入使能，
         * 之后 gpio_get_level() 恒返回 0，读回校验就变成假失败。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* 默认关断：上电/复位期间 CTRL 悬空可能被拉高，先明确置为关闭 */
    return audio_pa_set(false);
}

esp_err_t audio_pa_set(bool on)
{
#if BOARD_PA_ACTIVE_HIGH
    int level = on ? 1 : 0;
#else
    int level = on ? 0 : 1;
#endif
    return gpio_set_level((gpio_num_t)BOARD_PA_EN_GPIO, level);
}

int audio_pa_level(void)
{
    return gpio_get_level((gpio_num_t)BOARD_PA_EN_GPIO);
}

/* ---------------- 数据收发 ---------------- */

size_t audio_gen_sine(int16_t *frames, size_t n_frames, uint32_t rate,
                      uint32_t freq, int16_t amp)
{
    if (frames == NULL || n_frames == 0 || rate == 0) {
        return 0;
    }
    const float two_pi = 6.28318530718f;
    float step = two_pi * (float)freq / (float)rate;
    for (size_t i = 0; i < n_frames; i++) {
        int16_t s = (int16_t)(sinf(step * (float)i) * (float)amp);
        frames[2 * i]     = s;   /* 左 */
        frames[2 * i + 1] = s;   /* 右（ES8311 单声道，两槽同数据） */
    }
    return n_frames * 2;
}

esp_err_t audio_play(audio_path_t *a, const int16_t *frames, size_t n_frames,
                     size_t *sent)
{
    if (sent != NULL) {
        *sent = 0;
    }
    if (a == NULL || !a->ready || frames == NULL || n_frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t bytes = n_frames * 2 * sizeof(int16_t);
    size_t written = 0;
    /* 超时按 1s 给：16k 立体声 200ms = 12.8 KB，正常几十毫秒就发完 */
    esp_err_t err = i2s_channel_write(a->tx, frames, bytes, &written,
                                      pdMS_TO_TICKS(1000));
    if (sent != NULL) {
        *sent = written;
    }
    return err;
}

esp_err_t audio_capture(audio_path_t *a, int16_t *frames, size_t n_frames,
                        size_t *got)
{
    if (got != NULL) {
        *got = 0;
    }
    if (a == NULL || !a->ready || frames == NULL || n_frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t bytes = n_frames * 2 * sizeof(int16_t);
    size_t read = 0;
    esp_err_t err = i2s_channel_read(a->rx, frames, bytes, &read,
                                     pdMS_TO_TICKS(1000));
    if (got != NULL) {
        *got = read;
    }
    return err;
}

static void level_one(const int16_t *frames, size_t n_frames, int ch,
                      audio_level_t *out)
{
    out->rms = 0;
    out->peak = 0;
    out->nonzero = 0;
    if (frames == NULL || n_frames == 0) {
        return;
    }
    int64_t sum = 0;
    int pk = 0;
    size_t nz = 0;
    for (size_t i = 0; i < n_frames; i++) {
        int32_t v = frames[2 * i + ch];
        if (v != 0) {
            nz++;
        }
        int32_t a = v < 0 ? -v : v;
        if (a > pk) {
            pk = a;
        }
        sum += (int64_t)v * v;
    }
    int64_t mean = sum / (int64_t)n_frames;
    out->rms = (mean > 0) ? (int)sqrtf((float)mean) : 0;
    out->peak = pk;
    out->nonzero = nz;
}

void audio_level_lr(const int16_t *frames, size_t n_frames, audio_level_t *l,
                    audio_level_t *r)
{
    audio_level_t tmp;
    if (l == NULL) {
        l = &tmp;
    }
    if (r == NULL) {
        r = &tmp;
    }
    level_one(frames, n_frames, 0, l);
    level_one(frames, n_frames, 1, r);
}

void audio_level(const int16_t *frames, size_t n_frames, int *rms, int *peak)
{
    audio_level_t l;
    audio_level_lr(frames, n_frames, &l, NULL);
    if (rms != NULL) {
        *rms = l.rms;
    }
    if (peak != NULL) {
        *peak = l.peak;
    }
}
