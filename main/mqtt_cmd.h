/**
 * @file  mqtt_cmd.h
 * @brief 板子自己订阅 MQTT 命令主题并执行 —— 串口页不再需要 PC
 *
 * 之前的链路是：网页 → MQTT → PC 上的 bridge_serial_mqtt.py → COM14 → 板子。
 * PC 一关机、一休眠、USB 一拔，远程串口就没了；电池供电时更是彻底用不了。
 *
 * 这里换个方向：板子用自己的第二条 TLS 连接连同一个 broker，订阅
 * <前缀>/cmd，收到命令就在本机执行，输出再发回 <前缀>/serial/tx。
 *
 * 为什么要单开一条连接，而不是在推流那条上收：
 *   esp_tls_conn_read 是阻塞的，混在推流循环里会让出帧卡住；而命令执行
 *   本身可能耗时（batt watch 8 就 8 秒）。分开之后推流完全不受影响。
 *
 * 为什么命令主题用 /cmd 而不是复用 /serial/rx：
 *   桥接脚本（PC 上）也在订阅 /serial/rx。如果板子也订阅同一个，网页发
 *   一条命令会被两边同时执行 —— reboot 会执行两次的那种问题。用独立主题，
 *   两边互不干扰，桥接留着也不冲突。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动命令通道任务（幂等；MQTT 没开启时也会跑，连不上就慢慢重试） */
esp_err_t mqtt_cmd_start(void);

#ifdef __cplusplus
}
#endif
