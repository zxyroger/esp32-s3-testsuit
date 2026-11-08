/**
 * @file  health.h
 * @brief 自愈看门狗：摄像头卡死 / MQTT 假死时自动恢复
 *
 * 为什么需要它：
 *   板子跑久了会出现"有电但失联"的状态 —— 摄像头驱动那边一直
 *   "Failed to get frame: timeout"，推流任务因为没有新帧而不再发布，
 *   网页上就是永远"连接中"。这种状态下插拔 USB、重启浏览器都没用，
 *   只有板子自己知道该重来一次。
 *
 * 两条独立的判据，互不干扰：
 *   1) 摄像头：cam_jpeg 的帧序号连续 5 秒不动 → 重新 init 传感器；
 *      连续 3 次都救不回来 → 整机重启（此时基本是硬件/供电问题）。
 *   2) MQTT：链路 60 秒没有一次成功写出 → 拆连接重连；连续 2 次
 *      重连都无效 → 整机重启。
 *
 * 刻意不做的事：
 *   - 摄像头不是 watchdog 启动的（用户 cam stop 之后不该被拉起来）；
 *   - MQTT 从没成功连上过（开机就没网）不升级成重启，重启救不了没网；
 *   - WiFi 没有 IP 时不把账算在 MQTT 头上，等 WiFi 自己重连。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     cam_running;     /* 摄像头是否在跑（手动 stop 后为 false） */
    uint32_t cam_seq;         /* 最近一次读到的帧序号 */
    uint32_t cam_idle_ms;     /* 距上一帧过去多久 */
    uint32_t cam_trips;       /* 判定"卡死"的累计次数，含已自愈的 */
    uint32_t cam_restarts;    /* 成功重启摄像头的次数 */
    uint32_t mqtt_idle_ms;    /* 距上次成功写出多久，0xFFFFFFFF = 没成功过 */
    uint32_t mqtt_reconnects; /* 看门狗触发的 MQTT 重连次数 */
    bool     mqtt_ever_ok;    /* 本次开机 MQTT 是否成功发出过 */
    uint32_t reboots;         /* 已触发的整机重启次数（重启后归零） */
} health_stat_t;

/** 启动看门狗任务（幂等） */
esp_err_t health_start(void);

/** 读一份快照，给串口命令用 */
void health_get_stat(health_stat_t *out);

/** 串口打印当前健康状态 */
void health_dump(void);

/**
 * 手动触发一次摄像头自愈（health test）。
 * 不会伪造硬件故障：停流 -> 重新 init -> 恢复出图 的完整流程会真跑一遍，
 * 用来确认"卡死了确实能救回来"，不用等真的卡死。
 */
void health_force_trip(void);

#ifdef __cplusplus
}
#endif
