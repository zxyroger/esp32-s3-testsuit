/**
 * @file  cam_jpeg.h
 * @brief OV2640 JPEG 出图：传感器初始化 + DVP 收帧 + 最新帧发布
 *
 * 分层：
 *   cam_ov2640.c  供电 / XCLK / PWDN / SCCB（已实测通过）
 *   cam_jpeg.c    esp32-camera 收帧（组件自带 OV2640 初始化表）+ 最新帧发布
 *   cam_web.c     HTTP 服务器，把最新帧推给手机
 *
 * 为什么用 JPEG 而不是 RGB565：
 *   QVGA RGB565 一帧 153.6 KB，WiFi 上根本推不动，JPEG 一帧只要十几 KB。
 *   而且 OV2640 **自带 JPEG 编码器**，不占 CPU —— 只是初始化表长一些。
 *
 * 缓冲策略：
 *   DMA 双缓冲（收帧时另一块可被读取）+ 一块"已发布"缓冲。
 *   发布时 memcpy 一次（约 20 KB，几十微秒），HTTP 侧只在锁内拷贝，
 *   不会因为某个手机慢就把采集卡住。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cam_ov2640.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 运行统计，用于网页上的状态行与排错 */
typedef struct {
    uint16_t width;      /* 输出宽度（像素） */
    uint16_t height;     /* 输出高度 */
    uint32_t seq;        /* 已发布帧序号，0 = 还没有可用帧 */
    uint32_t frames;     /* 累计成功收帧数 */
    uint32_t dropped;    /* 丢弃帧数：超时 / SOI 不对 / 长度异常 */
    uint32_t last_len;   /* 最近一帧字节数 */
    uint32_t avg_fps;    /* 从启动到现在的平均帧率 */
    uint32_t uptime_ms;  /* 已运行时长 */
    uint32_t errors;     /* 驱动层错误次数（非超时） */
} cam_jpeg_stat_t;

/**
 * 启动出图。
 *
 * 内部按硬件强制顺序执行：
 *   供电（AXP2101 三路 LDO）-> XCLK -> 释放 PWDN -> esp_camera_init -> 采集任务
 *
 * @param cam  由 board_camera() 取得的句柄（SCCB/XCLK/PWDN 已准备好）
 * @param pmic AXP2101 句柄，用于给摄像头上电
 *
 * 可重复调用：已在运行时直接返回 ESP_OK。
 */
esp_err_t cam_jpeg_start(cam_t *cam, axp2101_t *pmic);

/** 停止收帧、反初始化 esp32-camera（传感器保持供电，便于快速重启） */
esp_err_t cam_jpeg_stop(void);

bool cam_jpeg_running(void);

/** 单帧缓冲容量（= width*height，JPEG 最坏情况也不会超） */
size_t cam_jpeg_frame_cap(void);

/**
 * 取最新一帧的拷贝。
 *
 * @param dst  目标缓冲，建议用 cam_jpeg_frame_cap() 大小
 * @param cap  dst 容量
 * @param len  输出实际字节数
 * @return 帧序号；0 表示当前还没有可用帧（调用方应回 503 而不是空图）
 *
 * 只做 memcpy，不涉及网络，持锁时间在微秒级。
 */
uint32_t cam_jpeg_snapshot(uint8_t *dst, size_t cap, size_t *len);

void cam_jpeg_get_stat(cam_jpeg_stat_t *out);

/**
 * 开关传感器的内建彩条测试图案。
 *
 * 用途：定位"画面有条纹/错位"到底出在哪一层。OV2640 自己生成的彩条是
 * 完全规则的竖直色带，不经过真实感光，所以：
 *   - 彩条干净     -> 数据通路是好的，问题在曝光/光照/镜头（比如工频闪烁）
 *   - 彩条也错乱   -> D0..D7 映射、PCLK 采样、DMA 时序这一层出了问题
 *
 * 需要先 cam_jpeg_start。
 */
esp_err_t cam_jpeg_set_colorbar(bool on);

/**
 * 手动曝光 / 恢复自动曝光。
 *
 * 背景：OV2640 默认按 60 Hz 计算自动曝光，在国内 50 Hz 的灯下每帧曝光时间
 * 与光周期不同步，会拍出一道道水平明暗条纹（JPEG 编码后还会串色成彩色横纹）。
 * 彩条图案是干净的，说明这不是数据通路问题，而是曝光。
 *
 * @param value >= 0 固定曝光行数（QVGA 下 100~800 是比较合理的区间），
 *              同时关掉 AEC；传 -1 恢复自动曝光。
 */
esp_err_t cam_jpeg_set_exposure(int value);

/** 调 JPEG 画质档（0 最好、63 最省）。用于排查"高压缩下色度错乱"这类问题 */
esp_err_t cam_jpeg_set_quality(int q);

/**
 * 切输出分辨率：true = VGA 640x480，false = QVGA 320x240。
 *
 * 只改传感器输出窗口，不重新 init 驱动 —— 因为 JPEG 模式下驱动那边的
 * 帧缓冲按 CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE 固定分配，与分辨率无关。
 *
 * VGA 画质好但一帧 40~90 KB，公网隧道（~50 KB/s）只能跑到 1 fps 上下；
 * 局域网满速没问题。远程看着卡就切 QVGA。
 */
esp_err_t cam_jpeg_set_res(bool vga);

/** 开关 50 Hz 抗闪（banding filter）。国内室内灯下出横纹先试这个 */
esp_err_t cam_jpeg_set_50hz(bool on);

/** 开关灰度（丢弃色度分量）。用于判断横纹是在亮度还是色度上 */
esp_err_t cam_jpeg_set_gray(bool on);

/**
 * 设降噪等级（OV2640 支持 0~4，0 = 关闭）。
 *
 * 为什么需要：暗光下 AEC 会把增益拉到 32×~128×，电源/照明的微小噪声被放大后
 * 表现为满屏细横纹。降噪能在传感器侧先压一道，比事后软件滤更有效。
 */
esp_err_t cam_jpeg_set_denoise(int level);

/**
 * 设增益上限（只能取 2/4/8/16/32/64/128 之一）。
 *
 * 压低上限 = 牺牲暗光亮度换取干净画面：暗光下传感器宁可欠曝也不再往上拉增益，
 * 横纹和彩色噪点会明显减少。默认是 128×（几乎不设限）。
 */
esp_err_t cam_jpeg_set_gain_ceiling(int x);

#ifdef __cplusplus
}
#endif
