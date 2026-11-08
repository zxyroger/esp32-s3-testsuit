/**
 * @file  batt_log.h
 * @brief 周期性采样电池状态并通过 MQTT 发出去，供网页「电池」页画曲线
 *
 * 为什么单独一个任务：电池是慢变量，和 2~3 fps 的推流节奏完全无关。
 * 挂在推流循环里会被帧率绑架（改成 1 fps 就 1 秒才采一次，改成 5 fps
 * 又白采），独立任务反而更简单、也更好停。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动采样任务（幂等）。MQTT 没连上时这一轮直接丢弃，不缓存。 */
esp_err_t batt_log_start(void);

/** 采样周期，默认 3000 ms */
void batt_log_set_period(int ms);

/** 当前周期（ms） */
int batt_log_period(void);

/** 已成功发出的采样点数（排错用） */
unsigned batt_log_sent(void);

#ifdef __cplusplus
}
#endif
