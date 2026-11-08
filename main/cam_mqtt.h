/**
 * @file  cam_mqtt.h
 * @brief 把最新一帧 JPEG 推到公共 MQTT broker，手机网页订阅后直接看
 *
 * 为什么走 MQTT 而不是开入站端口：
 *   板子在 192.168.0.x 后面，公网没有它的地址。所有"让别人连进来"的方案
 *   （DDNS + 端口映射、IPv6 直连）都卡在同一件事上——运营商/路由器把入站
 *   掐了。MQTT 反过来：板子主动向 broker 建一条长连接，页面订阅同一个
 *   topic。全程没有入站、不需要公网 IP、不需要改路由器。
 *
 * 代价：
 *   - 多一跳 broker，延迟比局域网大（通常 200~600 ms）
 *   - 公共 broker 有带宽限制，帧率要压到 1~3 fps
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 读 NVS 配置并按需启动（幂等） */
esp_err_t cam_mqtt_init(void);

/** 开关推流 */
esp_err_t cam_mqtt_set_enabled(bool on);

/** 改 topic（页面要跟着改，一对多会互相干扰） */
esp_err_t cam_mqtt_set_topic(const char *topic);

/** 推流帧率上限，1~10 */
esp_err_t cam_mqtt_set_rate(int fps);

/** 串口打印当前配置与连接状态 */
void cam_mqtt_dump(void);

/** 当前 topic（未配置时返回默认值） */
const char *cam_mqtt_topic(void);

/** broker 主机名与端口（命令通道要另建一条连接，复用同一套配置） */
const char *cam_mqtt_host(void);
uint16_t    cam_mqtt_port(void);

/** 推流是否处于开启状态（改摄像头参数前要查这个） */
bool cam_mqtt_enabled(void);

/* ---------------- 链路健康（health.c 自愈看门狗用） ---------------- */

/**
 * 距最近一次成功写出多少毫秒。
 * 返回 0xFFFFFFFF 表示本次开机还没成功发出去过（不是"刚成功"）。
 */
uint32_t cam_mqtt_idle_ms(void);

/** 本次开机是否至少成功发出过一条（没成功过就别重启板子，重启也救不了） */
bool cam_mqtt_ever_ok(void);

/** 请求推流任务拆掉现有连接重连（异步，下一轮循环生效） */
void cam_mqtt_reconnect(void);

/**
 * 当前链路是否可用（已连上 broker）。
 *
 * 用途：自检结果要"一联网就自动推上去"。开机时自检早于联网完成，
 * 所以需要一个能查询"现在能不能发"的接口，而不是假设连上了。
 */
bool cam_mqtt_up(void);

/**
 * 发一条文本到 <主 topic 前缀>/<sub>，并置 MQTT RETAIN 标志。
 *
 * 给"状态型"消息用（自检结果）：broker 会留着最后一条，
 * 后打开网页的人一订阅就拿到，不必等板子重连或再跑一次。
 */
esp_err_t cam_mqtt_publish_text_retain(const char *sub, const char *text);

/**
 * 往主 topic 的同一前缀下另开一条通道发一段文本。
 *
 * 例：主 topic 是 esp32cam/<前缀>/jpg，sub 传 "batt"，
 *     实际发到 esp32cam/<前缀>/batt。
 *
 * 用途：电池采样这类小 JSON，不必为它再建一条 MQTT 连接。
 * 与推流任务共用 TLS 连接，内部已用互斥量串行化发送。
 *
 * @return ESP_ERR_INVALID_STATE 表示当前没连上（这条丢弃，不缓存）
 */
esp_err_t cam_mqtt_publish_text(const char *sub, const char *text);

#ifdef __cplusplus
}
#endif
