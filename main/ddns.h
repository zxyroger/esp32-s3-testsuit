/**
 * @file  ddns.h
 * @brief 板子自己把公网 IPv6 上报到 DDNS（不依赖 PC、不依赖路由器 DDNS）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 读 NVS 配置并按需起后台任务（幂等，可重复调用） */
esp_err_t ddns_init(void);

/** 配置 DynDNS 风格服务：域名 + token（模板留空时默认用 dynv6 的接口） */
esp_err_t ddns_set_host(const char *host, const char *token);

/** 免注册模式：域名由当前 IPv6 自动编成 xxx.sslip.io，不联网也能算出来 */
esp_err_t ddns_set_auto(bool on);

/** 自定义上报接口模板，可用 {host} {token} {user} {pass} {ipv6} 占位符 */
esp_err_t ddns_set_url(const char *tmpl);

/** HTTP Basic 鉴权（花生壳那类接口需要） */
esp_err_t ddns_set_auth(const char *user, const char *pass);

/** 开关 */
esp_err_t ddns_set_enabled(bool on);

/** 立刻上报一次，返回结果 */
esp_err_t ddns_update_now(void);

/** 串口打印当前配置与状态 */
void ddns_dump(void);

/** 拼好的固定网址（http://<域名>:<端口>/），未配置域名时返回 NULL */
const char *ddns_public_url(void);

#ifdef __cplusplus
}
#endif
