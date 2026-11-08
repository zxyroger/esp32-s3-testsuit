/**
 * @file  wifi_lab.h
 * @brief WiFi 射频自检封装：STA 扫描 + SoftAP + 自环关联
 *
 * 为什么不用「连一个固定路由器」的常规写法：
 *   现场环境里没有可信的 SSID/密码，测试台换地方就失效。这里改成**自证闭环**：
 *
 *     STA ──扫描──▶ 周边 AP          （证明接收链路 + 射频前端在工作）
 *     AP  ──beacon──▶ 空气
 *     STA ──扫描──▶ 自己的 SSID      （证明发射链路真的把信号发出去并被收到）
 *     STA ──关联──▶ 自己的 AP        （证明 beacon/认证/关联/DHCP 全链路）
 *
 *   全程不需要第二个设备，也不需要知道任何外部网络。
 *
 * 事件通知用 EventGroup 而不是轮询：ESP-IDF 的 WiFi 连接结果是异步事件，
 * 轮询 esp_wifi_sta_get_ap_info() 在断开瞬间会返回旧值，容易误判。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool         drv_ready;   /* esp_wifi_init 已成功（协议栈常驻，不随 stop 撤销） */
    bool         started;     /* esp_wifi_start 已生效且未被 stop */
    bool         inited;      /* = drv_ready && started，用例里判断"射频可用"看这个 */
    bool         ap_up;       /* SoftAP 已配置起来 */
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    char         ap_ssid[33]; /* 含结尾 NUL */
    char         ap_pass[65];
    uint8_t      ap_channel;
    uint32_t     ap_ip;       /* 网络字节序，方便直接打印 */
    uint32_t     ap_gw;
    uint32_t     sta_ip;
    uint8_t      sta_mac[6];
    uint8_t      ap_mac[6];
    uint8_t      last_disc_reason; /* 最近一次 STA 断开原因码，排错用 */
    /* AP 侧观测到的客户端接入 —— 自环测试的关键证据 */
    uint16_t     ap_client_count;
    uint8_t      ap_client_mac[6];
    bool         ap_start_event; /* 收到过 WIFI_EVENT_AP_START（AP 接口真的起来了） */
} wifi_lab_t;

/**
 * 初始化协议栈并启动 STA 模式（幂等，重复调用直接返回已有句柄）。
 * 内部会按需初始化 NVS / netif / 默认事件循环 —— 调用方不用管顺序。
 */
esp_err_t wifi_lab_init(wifi_lab_t **out);

/** 读 STA / AP 两个接口的 MAC（任一指针可为 NULL） */
esp_err_t wifi_lab_mac(wifi_lab_t *lab, uint8_t sta[6], uint8_t ap[6]);

/**
 * 主动扫描。
 * @param out     结果缓冲
 * @param cap     缓冲容量（条）
 * @param n       实际条数
 * @param channel 0=全信道主动扫描（快）；非 0 只扫该信道并改为**被动**长驻留，
 *                用于尝试捕获自家 SoftAP 的 beacon（单射频下 beacon 会被 STA 扫描抢占，
 *                驻留时间越长越有机会撞上 AP 的发送窗口）
 *
 * 注意：ESP-IDF 的 esp_wifi_scan_get_ap_records() 会把内部结果一次性转移给调用方
 * 并释放内部存储，所以本函数每次扫描都重新拿全量，不做跨次缓存。
 */
esp_err_t wifi_lab_scan(wifi_lab_t *lab, wifi_ap_record_t *out, uint16_t cap,
                        uint16_t *n, uint8_t channel);

/** 开 SoftAP（切到 APSTA 模式，STA 侧保留以便自环关联） */
esp_err_t wifi_lab_ap_start(wifi_lab_t *lab, const char *ssid, const char *pass,
                            uint8_t channel);

/** 取 SoftAP 的 IP / 网关；未启动返回 ESP_ERR_INVALID_STATE */
esp_err_t wifi_lab_ap_ip(wifi_lab_t *lab, uint32_t *ip, uint32_t *gw);

/** STA 关联到指定 AP，成功即已拿到 IP；超时返回 ESP_ERR_TIMEOUT */
esp_err_t wifi_lab_sta_join(wifi_lab_t *lab, const char *ssid, const char *pass,
                            uint32_t timeout_ms);

/**
 * 主动踢掉 STA 连接一次（不清配置、不关自动重连）。
 *
 * 用途：验证"掉线自动重连"是否真的生效。路由器重启、AP 踢客户端这类
 * 场景没法在实验室里复现，但 esp_wifi_disconnect() 触发的断开事件和
 * 真实掉线走的是同一条路径，所以踢一下能如实反映重连逻辑的好坏。
 */
esp_err_t wifi_lab_sta_kick(wifi_lab_t *lab);

/** 取 STA 当前 IP（未连接返回 0） */
esp_err_t wifi_lab_sta_ip(wifi_lab_t *lab, uint32_t *ip);

/**
 * 取 STA 接口上第一个**全局** IPv6 地址（路由器 RA 下发的 SLAAC 地址）。
 *
 * 为什么要它：家里没有公网 IPv4，板子要让手机从外网直接连，只能走 IPv6。
 * fe80:: 链路本地地址出了这个网段就没用，必须拿 2409:/240e: 这类全局地址。
 *
 * @param buf 输出缓冲，建议 48 字节
 * @param len 缓冲长度
 * @return 有全局地址时返回 buf，否则返回 NULL（此时 buf 为空串）
 */
const char *wifi_lab_sta_ip6(wifi_lab_t *lab, char *buf, size_t len);

/**
 * 取当前接入 SoftAP 的客户端列表。
 *
 * 这是**唯一不依赖单射频自环**的 AP 验收方式：手机连上来之后，
 * 这里能读到真实客户端的 MAC / RSSI / PHY 模式，等于证明了 AP 的收发射频都在工作。
 */
esp_err_t wifi_lab_ap_clients(wifi_lab_t *lab, wifi_sta_list_t *list);

/** 断开并停止射频；保留协议栈，便于后续用例再次启动 */
esp_err_t wifi_lab_stop(wifi_lab_t *lab);

/** 当前模式名（"%s" 用） */
const char *wifi_lab_mode_str(wifi_mode_t mode);

/** WiFi 认证方式名（"%s" 用） */
const char *wifi_lab_auth_str(wifi_auth_mode_t auth);

/** 把 4 字节大端 IP 转成点分字符串 */
void wifi_lab_ip_str(uint32_t ip, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
