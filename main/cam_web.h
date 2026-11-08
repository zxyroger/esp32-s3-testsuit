/**
 * @file  cam_web.h
 * @brief 把摄像头画面做成网页：单帧 / 实时 MJPEG / 状态 JSON
 *
 * 端口 80，四个入口：
 *   /          手机页（自适应铺满，顶部显示帧率与分辨率）
 *   /jpg       单张快照，浏览器直接看，也方便脚本抓图
 *   /stream    实时流（multipart/x-mixed-replace），浏览器原生支持
 *   /stats     运行状态 JSON
 *
 * 为什么不自己写 WebSocket：MJPEG 是浏览器原生支持的，不用任何 JS 库，
 * 手机上 WebView 也能直接显示。WebSocket 还要实现帧分片，收益不大。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 HTTP 服务器（要求 WiFi 已经有 IP，否则手机访问不到） */
esp_err_t cam_web_start(void);

/** 停止 HTTP 服务器并断开所有客户端 */
esp_err_t cam_web_stop(void);

bool cam_web_running(void);

/** 监听端口，供日志与文档引用 */
int cam_web_port(void);

/** 外网备用端口（家用宽带封 80 时用这个，手机走 http://域名:18080/） */
int cam_web_alt_port(void);

#ifdef __cplusplus
}
#endif
