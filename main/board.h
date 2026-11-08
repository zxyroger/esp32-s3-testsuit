/**
 * @file  board.h
 * @brief 板级资源的延迟初始化与共享
 *
 * 多个用例会用到同一条 I2C 总线 / 同一个 AXP2101 句柄。
 * 这里做一次性初始化并缓存，避免每个用例重复建总线（重复建会占满控制器）。
 */
#pragma once

#include <stdbool.h>

#include "bus_i2c.h"
#include "axp2101.h"
#include "es8311.h"
#include "audio_path.h"
#include "cam_ov2640.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 取板级 I2C 总线；初始化失败返回 NULL */
const bus_i2c_t *board_bus(void);

/** 实际使用的引脚（探测可能换脚，报告里要如实打印） */
void board_bus_pins(int *sda, int *scl);

/** 取 AXP2101 句柄；器件不在总线上返回 NULL */
axp2101_t *board_pmic(void);

/** 取 ES8311 音频 CODEC 句柄；器件不在总线上返回 NULL */
es8311_t *board_codec(void);

/**
 * 取音频通路句柄（I2S 通道）。
 * 独立于 board_codec()：CODEC 在 I2C 上应答，不代表 I2S 一定建得起来，
 * 两者失败原因不同，报告里必须能分开定位。
 */
audio_path_t *board_audio(void);

/**
 * 取摄像头 SCCB/XCLK/PWDN 句柄（**不含供电**）。
 *
 * 为什么共享：SCCB 是挂在 I2C0 上的从机设备句柄，重复 prepare 会在同一条
 * 总线上重复挂 0x30；测试组和推流模块都要用它，必须共用同一个实例。
 * 供电由调用方按顺序打开（cam_power_on），因为掉电重上电的时序不同场景不一样。
 */
cam_t *board_camera(void);

#ifdef __cplusplus
}
#endif
