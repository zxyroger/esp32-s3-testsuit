/**
 * @file  sec_crypto.h
 * @brief 负载级端到端加密（AES-256-GCM）
 *
 * 为什么需要：MQTT 走的是公共 broker（broker.emqx.io），TLS 只保护到 broker 那一段，
 * broker 之后是明文。任何知道 topic 的人订阅一下就能看画面、看串口输出。
 * 所以在应用层再加一层：**板子加密 → broker 只看到密文 → 浏览器解密**。
 *
 * 线格式（每个 PUBLISH 的 payload）：
 *   [0]      版本号，固定 0x01
 *   [1]      保留位，固定 0x00
 *   [2..13]  12 字节随机 IV
 *   [14..]   密文，长度与明文相同
 *   [..+16]  16 字节 GCM tag（在密文末尾）
 *
 * 附加认证数据（AAD）用 topic 本身：密文被搬到别的话题上会直接校验失败。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** 每包固定开销：1 版本 + 1 保留 + 12 IV + 16 tag */
#define SEC_OVERHEAD 30

/**
 * 读取密钥（NVS 里有就用 NVS 的，没有就用编译进去的默认值）。
 * 幂等，可重复调用。
 */
esp_err_t sec_init(void);

/** 当前是否已就绪（密钥长度正确、mbedtls 初始化成功） */
bool sec_ready(void);

/**
 * 加密一段数据。
 *
 * @param aad      附加认证数据（传 topic）
 * @param pt       明文
 * @param pt_len   明文长度
 * @param out      输出缓冲，需要 pt_len + SEC_OVERHEAD 字节
 * @param out_cap  输出缓冲容量
 * @return 写出的总长度；失败返回 0
 */
size_t sec_seal(const char *aad, const uint8_t *pt, size_t pt_len, uint8_t *out, size_t out_cap);

/** 密钥指纹（前 4 字节），用于和页面的 KEY_HEX 对照 */
void sec_key_fp(uint8_t out[4]);

/**
 * 换密钥：hex 必须是 64 个十六进制字符（32 字节）。写进 NVS，重启后生效。
 * 换完记得把页面也换到同一把（页面支持 ?k=<64 位 hex> 参数）。
 */
esp_err_t sec_set_key_hex(const char *hex);

/**
 * 解密一段数据。
 *
 * @return 明文长度；认证失败 / 格式不对返回 -1
 */
int sec_open(const char *aad, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);
