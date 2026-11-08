/**
 * @file  audio_path.h
 * @brief 音频通路：I2S 主时钟 + ES8311 从模式配置 + NS4150B 功放使能
 *
 * 信号链（本板实测管脚见 board_config.h）：
 *
 *   ESP32-S3 (I2S master)                ES8311 (slave, 0x18)        NS4150B
 *   GPIO38 MCLK ───────────────────────▶ MCLK                        CTRL ◀─ GPIO47
 *   GPIO14 BCLK ───────────────────────▶ SCLK                        INP/INN
 *   GPIO13 LRCK ───────────────────────▶ LRCK              DAC ────▶ (D 类) ──▶ 喇叭
 *   GPIO45 DOUT ───────────────────────▶ SDIN
 *   GPIO12 DIN  ◀─────────────────────── SDOUT
 *                                        MIC ◀── 0.1uF ── ZTS6216(模拟 MEMS)
 *
 * 为什么 ES8311 必须做从机：MCLK/BCLK/LRCK 全部由 ESP32 产生，两边的分频
 * 关系才唯一确定（16k × 256 = 4.096 MHz，正好落在 ES8311 系数表里）。
 *
 * 所有寄存器值来自 esp_codec_dev 的 device/es8311/es8311.c，不是凭记忆写的；
 * 每条后面标了它在官方驱动里对应哪一步。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "driver/i2s_std.h"
#include "es8311.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- I2S ---------------- */

typedef struct {
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    bool              ready;
} audio_path_t;

/** 建立全双工 I2S 主通道：16 kHz / 16 bit / 双 slot / MCLK = 256 fs */
esp_err_t audio_path_init(audio_path_t *a);

/** 释放通道（重复调用安全） */
esp_err_t audio_path_deinit(audio_path_t *a);

/** 使能收发通道；通道未使能时 write/read 会直接报错 */
esp_err_t audio_path_enable(audio_path_t *a, bool tx, bool rx);

/** 关闭收发通道 */
esp_err_t audio_path_disable(audio_path_t *a, bool tx, bool rx);

/** 取通道实际时钟，用于验证时钟树算得对不对（不需要接任何模拟器件） */
esp_err_t audio_path_info(const audio_path_t *a, i2s_chan_info_t *out);

/* ---------------- ES8311 配置与无损回滚 ----------------
 *
 * 配置会改动十几个寄存器，直接改完不管会把芯片留在播放态。
 * 这里用"第一次写某寄存器时先存原值"的快照，跑完按逆序写回，
 * 保证用例失败也不会污染器件状态。
 */

#define AUDIO_SNAP_MAX 48

typedef struct {
    uint8_t n;
    uint8_t reg[AUDIO_SNAP_MAX];
    uint8_t val[AUDIO_SNAP_MAX];
} es8311_snap_t;

void audio_snap_reset(es8311_snap_t *s);

/** 写入 16k/16bit/从模式/模拟 MIC 的完整配置；snap 可为 NULL（不留快照） */
esp_err_t audio_codec_apply(es8311_t *codec, es8311_snap_t *snap);

/**
 * 同 audio_codec_apply，并把 MIC PGA 增益设到 db 档（0/6/12/.../42 dB）。
 *
 * 为什么单独一个入口：ES8311 的 MIC 增益寄存器 REG16 官方是**整字节**写
 * 档位枚举（0=0dB、1=6dB … 7=42dB）。上电默认 0x24，对 -38 dBV/Pa 的
 * ZTS6216 在安静房间里只剩个位数 LSB，很容易被误判成"麦克风没接"。
 * 参考 ai-esp32（D:\esp\ai-esp32）里跑通的量产值固件，它用
 * esp_codec_dev_set_in_gain(30) 下发，落到 REG16 就是 0x05。
 */
esp_err_t audio_codec_apply_mic(es8311_t *codec, es8311_snap_t *snap,
                                int db);

/**
 * 打开/关闭 ES8311 的「数字回环」：把 DAC 的数据直接灌给 ADC 的数字输入端。
 *
 * 靠 REG44（GPIO/参考选择）实现，bit6:4 = ADCDAT_SEL：
 *   0x58 = 0b101 -> ADCL + DACR，正常录音（模拟 MIC 走 ADC）
 *   0x68 = 0b110 -> DACL + DACR，DAC 数据直接回给 ADC —— 数字回环
 * bit3 = MICBIAS_EN（两种取值都是 1，麦克风偏置保持开启）。
 *
 * 为什么非要有这个开关：采集恒为全 0 时，原因只可能是下面两者之一——
 *   (a) 模拟前端没信号（麦克风/偏置/隔直电容）；
 *   (b) 数字通路根本没跑（MCLK 没到、ASDOUT 没驱动、I2S RX 没收）。
 * 从 I2C 侧完全看不出区别。打开数字回环再播一段正弦，如果这边能收到非零
 * 数据，就说明 (b) 整条是通的，问题必然在 (a)；反之则与麦克风无关。
 */
esp_err_t audio_codec_loopback(es8311_t *codec, es8311_snap_t *snap,
                               bool enable);

/** 按逆序写回快照里的原值 */
esp_err_t audio_snap_restore(es8311_t *codec, es8311_snap_t *snap);

/* ---------------- 功放（NS4150B） ----------------
 * CTRL 低电平 = Shutdown，高电平 = 工作；使能后需等 Tst(120ms) 才真正出声。
 */

esp_err_t audio_pa_init(void);
esp_err_t audio_pa_set(bool on);

/** 读回 CTRL 脚电平（0/1），失败返回 -1 */
int audio_pa_level(void);

/* ---------------- 数据收发 ---------------- */

/**
 * 生成正弦波，写成立体声交错（左右相同）。
 * @param n_frames 帧数（每帧 = 左 + 右两个 int16）
 * @return 实际写入的 int16 个数（= 2 * n_frames）
 */
size_t audio_gen_sine(int16_t *frames, size_t n_frames, uint32_t rate,
                      uint32_t freq, int16_t amp);

/** 发送若干帧；sent 返回实际发送的字节数 */
esp_err_t audio_play(audio_path_t *a, const int16_t *frames, size_t n_frames,
                     size_t *sent);

/** 接收若干帧；got 返回实际收到的字节数 */
esp_err_t audio_capture(audio_path_t *a, int16_t *frames, size_t n_frames,
                        size_t *got);

/** 单声道电平统计 */
typedef struct {
    int    rms;      /* 均方根，单位 LSB（满量程 32767） */
    int    peak;     /* 绝对值峰值 */
    size_t nonzero;  /* 非零采样个数 —— 用来区分"没有信号"和"通道错位" */
} audio_level_t;

/**
 * 分别统计左右声道。
 *
 * 必须分开看：ES8311 的 ADC 是单声道的，数据只会出现在一个 slot 里。
 * 如果只统计左声道，一旦 WS 极性 / slot 对齐与预期相反，就会读到整段 0，
 * 从而把"接得好好的麦克风"误报成"没接"。
 */
void audio_level_lr(const int16_t *frames, size_t n_frames, audio_level_t *l,
                    audio_level_t *r);

/** 对左声道求 RMS 与峰值（都按 int16 满量程 32767 归一化前的原始 LSB） */
void audio_level(const int16_t *frames, size_t n_frames, int *rms, int *peak);

#ifdef __cplusplus
}
#endif
