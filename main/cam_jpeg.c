/**
 * @file  cam_jpeg.c
 * @brief OV2640 QVGA/JPEG 出图：本工程负责上电与时钟，收帧交给 esp32-camera
 *
 * ─────────────────────────────────────────────────────────────────────────
 * 为什么收帧不用 IDF 自带的 esp_cam_ctlr_dvp（血泪教训）
 *
 * 自研 DVP 路径在本板能建控制器、能注册回调、enable/start 全部返回 ESP_OK，
 * 传感器也确实在出流 —— 用 cam probe 量到的硬证据：
 *
 *   RGB565 输出：PCLK=36360  VSYNC=1  HREF=1789  D0=29119  （300 ms 内）
 *   JPEG   输出：PCLK=35861  VSYNC=1  HREF=5288  D0=3080
 *
 * PCLK/HREF/D0 都在跳、VSYNC 也有边沿，但 esp_cam_ctlr_dvp 的 HAL 始终
 * 不产生 on_trans_finished 回调，累计 0 帧。回归源码查到 cam_hal_init 里
 * cam_ll_enable_vsync_generate_eof(hw, 1) / cam_ll_enable_invert_vsync(hw, 0)
 * 都按"标准接法"配了，仍未定位到根因。
 *
 * 于是改用 espressif__esp32-camera 2.1.7 —— 同款板量产工程
 * （D:\esp\ai-esp32\firmware）用它在**这块硬件上**已经跑通拍照 + LCD 回显。
 * 它走的是自己那套 target/esp32s3/ll_cam.c（lldesc 链 + LCD_CAM 寄存器直操），
 * 与 IDF 的 DVP HAL 不是同一条代码路径，所以不受上面那个问题影响。
 *
 * ─────────────────────────────────────────────────────────────────────────
 * 分工边界（这条线不能挪）
 *
 *   cam_ov2640.c   AXP2101 三路 LDO 上电、LEDC 出 XCLK、PWDN、SCCB 句柄
 *   cam_jpeg.c     调 esp_camera_init 收帧；把最新帧拷到 pub 缓冲
 *   cam_web.c      HTTP/MJPEG，把 pub 缓冲推给手机
 *
 * 供电和 XCLK 必须留在本工程，原因有两个：
 *   1. 本板 AXP2101 的 ALDO2/BLDO1/BLDO2 要"先关断→逐路设压→回读→使能"，
 *      错了会让 DVDD 以 2.8 V 上电（OV2640 只要 1.2 V）。组件没有这个流程。
 *   2. XCLK 必须由 LEDC 产生并**全程保持**。组件在 S3 上默认把 XCLK 交给
 *      LCD_CAM 的 cam_clk，而本板 cam_clk 并未接到 GPIO8（实测交给它之后
 *      传感器完全不响应 SCCB，总线上扫不到 0x30）。所以 camera_config_t 里
 *      pin_xclk 传 -1，组件就不会去动这个引脚（见 ll_cam.c 的
 *      `if (config->pin_xclk >= 0)` 分支）。
 *
 * ─────────────────────────────────────────────────────────────────────────
 * SCCB 复用板上已有的 I2C0
 *
 * pin_sccb_sda = -1 + sccb_i2c_port = I2C_NUM_0，组件的 SCCB_Use_Port() 就只
 * 取总线句柄、不新建总线（sccb-ng.c 的 sccb_owns_i2c_port 会是 false）。
 * 注意它探测到 0x30 后还会再 add 一个同地址的 device handle —— IDF 的
 * i2c_master_bus_add_device 不禁止同地址多 handle，两者并存无副作用，
 * 只是多占几十字节。
 *
 * ─────────────────────────────────────────────────────────────────────────
 * 曾经保留的 cam fmt 诊断已删除
 *
 * 原来本文件里有一份自抄的 OV2640 寄存器表（ov2640_settings.h / ov2640_regs.h），
 * 用于"RGB565 与 JPEG 各初始化一次再量 VSYNC"的对照诊断。改用组件后这份表
 * 与组件里的 sensors/private_include/ov2640_settings.h **同名全局变量重复定义**，
 * 链接直接报 multiple definition；而诊断本身也已经没有必要（组件就是那套
 * 寄存器序列，它出不了帧的话自己会打印 PID 探测失败）。
 * 所以整段删掉，两份头文件一并移除 —— 少一份重复代码，也少约 20 KB flash。
 */
#include "cam_jpeg.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "board_config.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cam_jpeg";

/* ---- 摄像头参数持久化 ----------------------------------------------------
 * 以前分辨率/画质/抗闪/增益上限只活在 RAM 里，板子一看门狗复位就掉回 QVGA，
 * 画面被 2 倍拉伸、横纹看着更重（今天已经踩了三次）。这里把四项存进 NVS，
 * 上电/复位后自动恢复；下面的编译默认值是 NVS 为空（首次烧录）时的兜底。 */
#define CAMCFG_NVS_NS   "camcfg"
#define CAMCFG_DEF_FS   FRAMESIZE_SVGA      /* 800x600 */
#define CAMCFG_DEF_Q    38                 /* 0 最好 / 63 最省，配合公网带宽 */
#define CAMCFG_DEF_HZ   1                 /* 1 = 50 Hz（国内市电），0 = 60 Hz */
#define CAMCFG_DEF_GC   2                 /* AGC 增益上限 2x */
#define CAMCFG_DEF_PCLK 8                 /* R_DVP_SP 的 PCLK 分频，驱动 JPEG 默认 8 */

typedef struct {
    framesize_t fs;
    uint8_t     q;
    uint8_t     hz;
    uint8_t     gc;
    uint8_t     pclk;
} camcfg_t;

static camcfg_t s_cfg_cam = {
    CAMCFG_DEF_FS, CAMCFG_DEF_Q, CAMCFG_DEF_HZ, CAMCFG_DEF_GC, CAMCFG_DEF_PCLK
};

static void camcfg_load(void)
{
    nvs_handle_t h;
    /* 用 READWRITE 打开：namespace 不存在时会自动创建，并把编译默认值逐项
     * 写进去 —— 「默认参数」是真的落在 NVS 里，不是只活在代码里。 */
    if (nvs_open(CAMCFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "摄像头参数：NVS 打不开，用编译默认 %dx%d/q%d",
                 resolution[CAMCFG_DEF_FS].width,
                 resolution[CAMCFG_DEF_FS].height, CAMCFG_DEF_Q);
        return;
    }
    uint8_t v = 0;
    bool dirty = false;

    if (nvs_get_u8(h, "fs", &v) != ESP_OK) {
        v = (uint8_t)CAMCFG_DEF_FS; nvs_set_u8(h, "fs", v); dirty = true;
    }
    if (v <= FRAMESIZE_XGA) s_cfg_cam.fs = (framesize_t)v;

    if (nvs_get_u8(h, "q", &v) != ESP_OK) {
        v = CAMCFG_DEF_Q; nvs_set_u8(h, "q", v); dirty = true;
    }
    if (v <= 63) s_cfg_cam.q = v;

    if (nvs_get_u8(h, "hz", &v) != ESP_OK) {
        v = CAMCFG_DEF_HZ; nvs_set_u8(h, "hz", v); dirty = true;
    }
    if (v <= 1) s_cfg_cam.hz = v;

    if (nvs_get_u8(h, "gc", &v) != ESP_OK) {
        v = CAMCFG_DEF_GC; nvs_set_u8(h, "gc", v); dirty = true;
    }
    switch (v) {
    case 2: case 4: case 8: case 16: case 32: case 64: case 128:
        s_cfg_cam.gc = v; break;
    default: break;
    }

    if (nvs_get_u8(h, "pclk", &v) != ESP_OK) {
        v = CAMCFG_DEF_PCLK; nvs_set_u8(h, "pclk", v); dirty = true;
    }
    if (v >= 4 && v <= 64) s_cfg_cam.pclk = v;

    if (dirty) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "摄像头参数已从 NVS 恢复：%dx%d / q%d / %sHz / 增益 %dx / PCLK分频 %d",
             resolution[s_cfg_cam.fs].width, resolution[s_cfg_cam.fs].height,
             s_cfg_cam.q, s_cfg_cam.hz ? "50" : "60", s_cfg_cam.gc,
             s_cfg_cam.pclk);
}

static void camcfg_save(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(CAMCFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "摄像头参数保存失败（NVS 打不开）");
        return;
    }
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static gainceiling_t camcfg_gc_enum(int x)
{
    switch (x) {
    case 2:   return GAINCEILING_2X;
    case 4:   return GAINCEILING_4X;
    case 8:   return GAINCEILING_8X;
    case 16:  return GAINCEILING_16X;
    case 32:  return GAINCEILING_32X;
    case 64:  return GAINCEILING_64X;
    case 128: return GAINCEILING_128X;
    default:  return GAINCEILING_2X;
    }
}

/* R_DVP_SP（DSP bank 0xD3）：低 7 位是 PCLK 分频，bit7 = auto 模式。
 * esp32-camera 在 JPEG 模式把它写死成 8。SVGA(800x600) 下实测会出现
 * 横向撕裂/色带，像是 DVP 采样余量不够；调大分频降低 PCLK 可以换回稳定，
 * 代价是帧率下降。做成运行时可调并存 NVS，方便按实机选最优值。 */
static void apply_pclk(sensor_t *s)
{
    if (s == NULL || s->set_reg == NULL) {
        return;
    }
    uint8_t v = (uint8_t)(s_cfg_cam.pclk & 0x7F);
    int r = s->set_reg(s, 0x00D3, 0xFF, v);
    ESP_LOGI(TAG, "PCLK 分频 = %d（R_DVP_SP=0x%02X，返回 %d）",
             s_cfg_cam.pclk, v, r);
}

/**
 * 一路 JPEG 的最大字节数，必须等于 sdkconfig 的
 * CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE —— 组件里 cam_hal 直接把
 * fb_size = recv_size = 该配置值，超了就整帧丢弃。
 *
 * 早期只有 QVGA，40 KB 很宽裕；提到 VGA(640x480) 后细节场景一帧能到
 * 50~90 KB，所以放到 150 KB 留足余量（VGA 双缓冲 = 300 KB PSRAM，无压力）。
 */
#define CAM_FB_BYTES      (150u * 1024u)

/* 当前输出分辨率。VGA 画质明显好，但公网隧道带宽只有 ~50 KB/s，
 * 觉得远程太卡就发 cam res qvga 换回 320x240（只改传感器窗口，
 * 不用重新 init，见 cam_jpeg_set_res）。 */
static int s_res_w = BOARD_CAM_H_RES;
static int s_res_h = BOARD_CAM_V_RES;

#define CAM_JPEG_WARMUP   2    /* 丢弃前 N 帧：JPEG 流刚起来时长度不稳 */
#define CAM_TASK_STACK    4096
#define CAM_TASK_PRIO     5
#define CAM_BUF_ALIGN     64   /* cache line 32 B 的整数倍；PSRAM 分配统一按此对齐 */

typedef struct {
    TaskHandle_t      task;
    SemaphoreHandle_t mtx;      /* 保护 pub / pub_len / seq */
    uint8_t          *pub;      /* 最新一帧的副本，HTTP 侧只读这里 */
    size_t            cap;
    size_t            pub_len;
    volatile bool     run;
    volatile uint32_t seq;
    bool              inited;   /* esp_camera_init 已成功，需要 deinit */
    cam_jpeg_stat_t   stat;
    int64_t           t_start;
} cam_jpeg_ctx_t;

static cam_jpeg_ctx_t s_j;

/* ══════════════════════════════════════════════════════════════════════
 * 第一部分：上电与引脚准备
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * 供电 -> XCLK -> 释放 PWDN -> 等起振 -> 确认 SCCB 应答。
 * 顺序被硬件强制，不能调换（见 cam_ov2640.h 头注释）。
 */
static esp_err_t cam_hw_prepare(cam_t *cam, axp2101_t *pmic)
{
    esp_err_t err;

    if (!cam->power_ok) {
        err = cam_power_on(cam, pmic, 0);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!cam->xclk_ok) {
        err = cam_xclk_start(cam);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = cam_pwdn(cam, false);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(BOARD_CAM_XCLK_SETTLE_MS));

    if (!cam_sccb_present(cam)) {
        ESP_LOGE(TAG, "SCCB 0x%02X 无应答，摄像头未接或供电/时钟异常",
                 BOARD_CAM_SCCB_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * 第二部分：esp32-camera 配置与采集任务
 * ══════════════════════════════════════════════════════════════════════ */

/* 非 const：启动时 camcfg_load() 会用 NVS 里保存的分辨率/画质覆盖它 */
static camera_config_t s_cam_cfg = {
    /* 三个 -1 都是刻意的，各自对应一条踩过的坑：
     *   pin_pwdn  = -1  本工程自己拉 PWDN（cam_pwdn），避免组件在
     *                   我们上电流程中间把它翻一次
     *   pin_xclk  = -1  XCLK 由 LEDC 常驻输出。传具体引脚的话组件会把
     *                   GPIO8 从 LEDC 抢回 LCD_CAM 的 cam_clk，本板
     *                   cam_clk 没接到 GPIO8 —— 传感器立刻收不到时钟，
     *                   SCCB 直接扫不到 0x30
     *   pin_sccb_sda = -1  复用板上已建好的 I2C0，不新建总线 */
    .pin_pwdn     = -1,
    .pin_reset    = -1,
    .pin_xclk     = -1,
    .pin_sccb_sda = -1,
    .pin_sccb_scl = -1,

    .pin_d7    = BOARD_CAM_D7_GPIO,
    .pin_d6    = BOARD_CAM_D6_GPIO,
    .pin_d5    = BOARD_CAM_D5_GPIO,
    .pin_d4    = BOARD_CAM_D4_GPIO,
    .pin_d3    = BOARD_CAM_D3_GPIO,
    .pin_d2    = BOARD_CAM_D2_GPIO,
    .pin_d1    = BOARD_CAM_D1_GPIO,
    .pin_d0    = BOARD_CAM_D0_GPIO,
    .pin_vsync = BOARD_CAM_VSYNC_GPIO,
    .pin_href  = BOARD_CAM_HREF_GPIO,
    .pin_pclk  = BOARD_CAM_PCLK_GPIO,

    .xclk_freq_hz = BOARD_CAM_XCLK_HZ,   /* 10 MHz，理由见 board_config.h */

    /* 这两个字段只在组件自己产 XCLK 时才会用到（pin_xclk >= 0）。
     * 填成本工程 LEDC 实际占用的资源，纯粹为了"读代码的人不会误以为
     * 它用了别的通道"，组件不会去动它。 */
    .ledc_timer   = LEDC_TIMER_2,
    .ledc_channel = LEDC_CHANNEL_7,

    .pixel_format = PIXFORMAT_JPEG,      /* OV2640 硬件直出 JPEG，不占 CPU */
    /* 编译默认 SVGA(800x600)/q38：分辨率比 VGA 高 56%，q 调大让单帧体积和
     * 原来的 VGA/q30 接近，公网带宽不至于被拖垮。
     * 真正生效的值在 cam_jpeg_start() 里由 NVS 覆盖（camcfg_load），
     * 所以 cam res / cam q 改过的参数复位后能自动恢复。 */
    .frame_size   = CAMCFG_DEF_FS,
    .jpeg_quality = CAMCFG_DEF_Q,

    .fb_count     = 2,                   /* 双缓冲：一块在写、一块可取 */
    .fb_location  = CAMERA_FB_IN_PSRAM,  /* 内部 RAM 留给 WiFi */
    .grab_mode    = CAMERA_GRAB_LATEST,  /* 手机只看最新画面，不追旧帧 */

    .sccb_i2c_port = 0,                  /* = I2C_NUM_0，板上已建好的那条 */
};

static bool jpeg_ok(const uint8_t *buf, size_t n)
{
    if (buf == NULL || n < 128) {
        return false;
    }
    /* SOI = FF D8 */
    if (buf[0] != 0xFF || buf[1] != 0xD8) {
        return false;
    }
    /* EOI = FF D9。长度对但末尾没 EOI，说明缓冲被写满截断了，
     * 这种帧发给浏览器会显示半张图，不如丢掉。 */
    if (buf[n - 2] != 0xFF || buf[n - 1] != 0xD9) {
        return false;
    }
    return true;
}

static void cam_task(void *arg)
{
    cam_jpeg_ctx_t *j = (cam_jpeg_ctx_t *)arg;
    int warm = 0;

    ESP_LOGI(TAG, "采集任务启动：%dx%d，发布缓冲 %u B",
             BOARD_CAM_H_RES, BOARD_CAM_V_RES, (unsigned)j->cap);

    while (j->run) {
        /* esp_camera_fb_get() 内部是 xQueueReceive(4000ms)，所以：
         *   - 有流时每帧立刻返回；
         *   - 无流时最多 4 s 也会返回 NULL，任务不会永久睡死，
         *     cam_jpeg_stop() 的等待循环才有意义。 */
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            j->stat.errors++;
            continue;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            j->stat.dropped++;
        } else if (!jpeg_ok(fb->buf, fb->len)) {
            j->stat.dropped++;
        } else if (warm < CAM_JPEG_WARMUP) {
            warm++;             /* 刚起流的前两帧长度不稳，丢掉 */
        } else if (fb->len > j->cap) {
            /* 理论上不该发生：cap 取 40 KB，等于组件那档帧缓冲上限。
             * 真超出了说明 sdkconfig 与这里的 CAM_FB_BYTES 没对齐。 */
            j->stat.dropped++;
            ESP_LOGW(TAG, "帧 %u B 超过发布缓冲 %u B，丢弃",
                     (unsigned)fb->len, (unsigned)j->cap);
        } else if (xSemaphoreTake(j->mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
            memcpy(j->pub, fb->buf, fb->len);
            j->pub_len = fb->len;
            j->stat.last_len = fb->len;
            j->stat.frames++;
            j->seq++;
            xSemaphoreGive(j->mtx);
        } else {
            j->stat.dropped++;   /* HTTP 侧持锁异常，这一帧不要了 */
        }

        esp_camera_fb_return(fb);
    }

    ESP_LOGI(TAG, "采集任务退出：共 %u 帧，丢弃 %u",
             (unsigned)j->stat.frames, (unsigned)j->stat.dropped);
    j->task = NULL;
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * 第三部分：对外接口
 * ══════════════════════════════════════════════════════════════════════ */

size_t cam_jpeg_frame_cap(void)
{
    return s_j.cap ? s_j.cap : (size_t)CAM_FB_BYTES;
}

bool cam_jpeg_running(void)
{
    return s_j.run;
}

static void free_pub(void)
{
    /* 必须持锁释放：看门狗会在别的任务里调 cam_jpeg_stop()，
     * 而 HTTP 侧可能正好在 cam_jpeg_snapshot() 里 memcpy 这块 PSRAM。
     * 不持锁就会 use-after-free（表现为莫名其妙 LoadProhibited 复位）。 */
    bool locked = false;
    if (s_j.mtx != NULL) {
        locked = (xSemaphoreTake(s_j.mtx, pdMS_TO_TICKS(500)) == pdTRUE);
    }
    if (s_j.pub) {
        heap_caps_free(s_j.pub);
        s_j.pub = NULL;
    }
    s_j.pub_len = 0;
    if (locked) {
        xSemaphoreGive(s_j.mtx);
    }
}

esp_err_t cam_jpeg_start(cam_t *cam, axp2101_t *pmic)
{
    if (cam == NULL || pmic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_j.run) {
        return ESP_OK;
    }

    /* 0) PSRAM 必须在：驱动双帧缓冲各 150 KB + 发布缓冲 150 KB ≈ 450 KB，
     *    内部 RAM 在 WiFi 起来后根本没这么多。留 768 KB 门限比较稳。 */
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 768u * 1024u) {
        ESP_LOGE(TAG, "PSRAM 不足，无法出图（需要约 450 KB）");
        return ESP_ERR_NO_MEM;
    }

    /* 1) 供电 -> XCLK -> 释放 PWDN -> 确认 SCCB */
    esp_err_t err = cam_hw_prepare(cam, pmic);
    if (err != ESP_OK) {
        return err;
    }

    /* 2) 发布缓冲：HTTP 侧只读这里，与驱动缓冲解耦。
     *    必须按 cache line（32 B）对齐分配 —— PSRAM 走 PSRAM-CACHE，
     *    后续若走 esp_cache_msync 路径，起址不对齐会直接 ESP_ERR_INVALID_ARG。 */
    s_j.cap = (size_t)CAM_FB_BYTES;
    if (s_j.pub == NULL) {
        s_j.pub = heap_caps_aligned_alloc(CAM_BUF_ALIGN, s_j.cap,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_j.pub == NULL) {
            ESP_LOGE(TAG, "PSRAM 分配发布缓冲失败（%u B）", (unsigned)s_j.cap);
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_j.mtx == NULL) {
        s_j.mtx = xSemaphoreCreateMutex();
        if (s_j.mtx == NULL) {
            free_pub();
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_j.stat, 0, sizeof(s_j.stat));
    s_j.stat.width  = (uint16_t)s_res_w;
    s_j.stat.height = (uint16_t)s_res_h;
    s_j.seq = 0;
    s_j.pub_len = 0;
    s_j.t_start = esp_timer_get_time();

    /* 3) 驱动初始化。组件内部按 probe -> reset -> set_framesize ->
     *    set_pixformat -> set_quality -> cam_start 的顺序做完，
     *    日志里会打印 PID / 检测到的型号 / 缓冲大小。
     *    先读 NVS，用上次保存的分辨率/画质覆盖编译默认值 —— 复位不再掉回 QVGA。 */
    camcfg_load();
    s_cam_cfg.frame_size   = s_cfg_cam.fs;
    s_cam_cfg.jpeg_quality = s_cfg_cam.q;
    if (!s_j.inited) {
        err = esp_camera_init(&s_cam_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_camera_init 失败: %s（0x%X）",
                     esp_err_to_name(err), err);
            free_pub();
            return err;
        }
        s_j.inited = true;
    }

    /* 4) 取分辨率核对一次：组件可能因为传感器档位上限而改过 */
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        /* 坏点校正（BPC）：esp32-camera 的 OV2640 初始化序列漏写了 CTRL3 bit7，
         * 而 Linux 官方驱动是开的（CTRL3=0xD0）。暗光下坏点会以散点/热像素冒出来，
         * 这里补上。WPC 初始化序列里已开，BPC 单独补。 */
        if (s->set_bpc) {
            s->set_bpc(s, 1);
        }
        /* AGC 增益上限设为 2x，对齐 ai-esp 官方 OV2640 参数（新版 esp_cam_sensor
         * 的 ov2640_settings_cif 写死 COM9_AGC_GAIN_2x）。esp32-camera 旧驱动的
         * 初始化序列默认是 8x —— 暗光下 AEC 会把增益拉到 8x，行读出横纹幅度随
         * 增益被放大最多 8 倍；压到 2x 后，暗光宁可欠曝（靠更长曝光补光）也不再
         * 拉高增益，从源头压低暗光横纹。白天增益本就低（gain≈0x03），不受影响。 */
        if (s->set_gainceiling) {
            s->set_gainceiling(s, camcfg_gc_enum(s_cfg_cam.gc));
        }
        /* 抗闪频段跟着 NVS 恢复（组件默认 60Hz，国内市电是 50Hz）。
         * 只写寄存器不写 NVS，避免把刚读出来的值又回写一遍。 */
        if (s->set_reg && s->set_exposure_ctrl) {
            s->set_exposure_ctrl(s, 1);
            s->set_reg(s, 0x010C, 0x06, s_cfg_cam.hz ? 0x04 : 0x00);
        }
        /* 驱动在 set_window() 里会重置 R_DVP_SP，所以必须在 init 之后再写一次 */
        apply_pclk(s);
        s_res_w = resolution[s->status.framesize].width;
        s_res_h = resolution[s->status.framesize].height;
        s_j.stat.width  = (uint16_t)s_res_w;
        s_j.stat.height = (uint16_t)s_res_h;
        ESP_LOGI(TAG, "传感器状态：PID=0x%02X framesize=%d(%dx%d) pixformat=%d",
                 s->id.PID, s->status.framesize, s_j.stat.width, s_j.stat.height,
                 s->pixformat);
        if (s->pixformat != PIXFORMAT_JPEG) {
            ESP_LOGW(TAG, "不是 JPEG 输出（pixformat=%d），网页会显示坏图",
                     s->pixformat);
        }
    }

    /* 5) 采集任务 */
    s_j.run = true;
    if (xTaskCreate(cam_task, "cam_cap", CAM_TASK_STACK, &s_j, CAM_TASK_PRIO,
                    &s_j.task) != pdPASS) {
        s_j.run = false;
        esp_camera_deinit();
        s_j.inited = false;
        free_pub();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "出图已启动：%dx%d JPEG，XCLK=%u Hz",
             s_j.stat.width, s_j.stat.height, (unsigned)cam->xclk_hz);
    return ESP_OK;
}

esp_err_t cam_jpeg_stop(void)
{
    if (!s_j.run && !s_j.inited) {
        return ESP_OK;
    }

    s_j.run = false;

    /* 等任务退出：它在 esp_camera_fb_get() 里最多阻塞 4 s，
     * 正常有流时 100 ms 内就退出了。给 5 s 上限。 */
    for (int i = 0; i < 50 && s_j.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_j.task != NULL) {
        /* 只在流彻底停了（传感器不出帧）时才会走到这里。
         * 删掉一个阻塞在 xQueueReceive 上的任务是安全的。 */
        ESP_LOGW(TAG, "采集任务未按时退出，强制删除");
        vTaskDelete(s_j.task);
        s_j.task = NULL;
    }

    if (s_j.inited) {
        esp_err_t err = esp_camera_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_camera_deinit 返回 %s", esp_err_to_name(err));
        }
        s_j.inited = false;
    }

    free_pub();
    ESP_LOGI(TAG, "出图已停止");
    return ESP_OK;
}

uint32_t cam_jpeg_snapshot(uint8_t *dst, size_t cap, size_t *len)
{
    if (dst == NULL || len == NULL || s_j.mtx == NULL) {
        if (len) *len = 0;
        return 0;
    }

    uint32_t seq = 0;
    if (xSemaphoreTake(s_j.mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        *len = 0;
        return 0;
    }
    if (s_j.pub_len > 0 && s_j.pub_len <= cap) {
        memcpy(dst, s_j.pub, s_j.pub_len);
        *len = s_j.pub_len;
        seq = s_j.seq;
    } else {
        *len = 0;
    }
    xSemaphoreGive(s_j.mtx);
    return seq;
}

void cam_jpeg_get_stat(cam_jpeg_stat_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_j.mtx != NULL && xSemaphoreTake(s_j.mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_j.stat;
        out->seq = s_j.seq;
        xSemaphoreGive(s_j.mtx);
    } else {
        *out = s_j.stat;
        out->seq = s_j.seq;
    }

    int64_t dt = esp_timer_get_time() - s_j.t_start;
    out->uptime_ms = (dt > 0) ? (uint32_t)(dt / 1000) : 0;
    out->avg_fps = (dt > 0) ? (uint32_t)((uint64_t)out->frames * 1000000ull / (uint64_t)dt) : 0;
}

esp_err_t cam_jpeg_set_colorbar(bool on)
{
    if (!s_j.inited) {
        ESP_LOGW(TAG, "彩条：摄像头还没启动，先发 cam start");
        return ESP_ERR_INVALID_STATE;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_colorbar == NULL) {
        ESP_LOGW(TAG, "彩条：当前传感器不支持");
        return ESP_ERR_NOT_SUPPORTED;
    }

    int r = s->set_colorbar(s, on ? 1 : 0);
    ESP_LOGI(TAG, "彩条测试图案 %s（set_colorbar 返回 %d）", on ? "已开启" : "已关闭", r);
    return (r == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t cam_jpeg_set_exposure(int value)
{
    if (!s_j.inited) {
        ESP_LOGW(TAG, "曝光：摄像头还没启动，先发 cam start");
        return ESP_ERR_INVALID_STATE;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (value < 0) {
        s->set_exposure_ctrl(s, 1);
        if (s->set_aec2 != NULL) {
            s->set_aec2(s, 1);
        }
        ESP_LOGI(TAG, "曝光：已恢复自动（AEC + AEC2 打开）");
        return ESP_OK;
    }

    /* 固定曝光前必须先关 AEC，否则下一次 AEC 周期就把值覆盖回去 */
    s->set_exposure_ctrl(s, 0);
    int r = s->set_aec_value(s, value);
    ESP_LOGI(TAG, "曝光：已固定为 %d 行（AEC 已关，set_aec_value 返回 %d）", value, r);
    return (r == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t cam_jpeg_set_quality(int q)
{
    if (!s_j.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_quality == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (q < 0) {
        q = 0;
    }
    if (q > 63) {
        q = 63;
    }
    int r = s->set_quality(s, q);
    ESP_LOGI(TAG, "JPEG 质量 = %d（0 最好 / 63 最省），返回 %d", q, r);
    if (r == 0) {
        s_cfg_cam.q = (uint8_t)q;
        camcfg_save("q", (uint8_t)q);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t cam_jpeg_set_framesize(int fs)
{
    if (!s_j.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_framesize == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (fs < 0 || fs > FRAMESIZE_XGA) {
        return ESP_ERR_INVALID_ARG;
    }

    int r = s->set_framesize(s, (framesize_t)fs);
    if (r != 0) {
        ESP_LOGE(TAG, "切分辨率失败（set_framesize 返回 %d）", r);
        return ESP_FAIL;
    }
    /* set_framesize 内部会重写 R_DVP_SP，把保存的 PCLK 分频补回去 */
    apply_pclk(s);

    s_res_w = resolution[s->status.framesize].width;
    s_res_h = resolution[s->status.framesize].height;
    s_j.stat.width  = (uint16_t)s_res_w;
    s_j.stat.height = (uint16_t)s_res_h;

    /* 刚切完窗口的那几帧长度和曝光都还没稳，序号清零让统计重新算，
     * HTTP 侧看到 seq 归零会自然丢弃缓存，不需要额外通知。 */
    s_j.seq     = 0;
    s_j.pub_len = 0;
    s_j.t_start = esp_timer_get_time();

    /* 存进 NVS：以后看门狗/断电复位都直接回到这一档，不再掉回 QVGA */
    s_cfg_cam.fs = s->status.framesize;
    camcfg_save("fs", (uint8_t)s_cfg_cam.fs);
    ESP_LOGI(TAG, "输出分辨率已切到 %dx%d（已存 NVS，复位后保持）", s_res_w, s_res_h);
    return ESP_OK;
}

/* 旧接口保留，避免调用方全改一遍 */
esp_err_t cam_jpeg_set_res(bool vga)
{
    return cam_jpeg_set_framesize(vga ? FRAMESIZE_VGA : FRAMESIZE_QVGA);
}

esp_err_t cam_jpeg_set_pclk(int div)
{
    if (!s_j.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (div < 4 || div > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_reg == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_cfg_cam.pclk = (uint8_t)div;
    apply_pclk(s);
    camcfg_save("pclk", (uint8_t)div);
    return ESP_OK;
}

esp_err_t cam_jpeg_set_50hz(bool on)
{
    if (!s_j.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_reg == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* OV2640 的 50/60Hz 抗闪选择在 COM3（地址 0x0C）的 bit2:1：
     *   0b01(0x02) = 自动   0b10(0x04) = 50Hz   0b00(0x00) = 60Hz
     * 组件初始化序列没写 COM3，默认 0x38 → 60Hz；中国市电是 50Hz，
     * 暗光长曝光下 banding 就会把曝光对齐到错误的周期，横纹反而更重。
     *
     * 注意：BD50/BD60 那两个寄存器（0x4F/0x50）驱动已经按当前分辨率算好，
     * 这里不用碰。旧代码写 0x2E/0x2C 是别的寄存器，导致一开就过曝全白。 */
    int r;
    if (on) {
        /* banding 靠 AEC 把曝光对齐到 10 ms 整数倍，曝光必须由 AEC 控制 */
        s->set_exposure_ctrl(s, 1);
        r = s->set_reg(s, 0x010C, 0x06, 0x04);   /* COM3(sensor bank) band = 50Hz */
        ESP_LOGI(TAG, "抗闪已切 50 Hz（COM3 band=10b，返回 %d）", r);
    } else {
        r = s->set_reg(s, 0x010C, 0x06, 0x00);   /* COM3(sensor bank) band = 60Hz */
        ESP_LOGI(TAG, "抗闪已切 60 Hz（COM3 band=00b，返回 %d）", r);
    }
    if (r == 0) {
        s_cfg_cam.hz = on ? 1 : 0;
        camcfg_save("hz", s_cfg_cam.hz);
        return ESP_OK;
    }
    return ESP_FAIL;
}

void cam_jpeg_dump_status(void)
{
    cam_t *cam = board_camera();
    if (cam == NULL || !cam->sccb_ok) {
        printf("[CAM] 摄像头未就绪（先 cam start）\n");
        return;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_reg == NULL) {
        printf("[CAM] 传感器句柄无效\n");
        return;
    }

    /* 把 bank 同步到 sensor bank：用组件的 set_reg 做一次无害读改写。
     * 它会 set_bank(1)，让组件内部的 bank 缓存和芯片实际 bank 都停在
     * sensor bank —— 之后下面 cam_reg_read 直接读就是对的，也不会破坏
     * 组件的缓存（cam_reg_read 本身不切 bank）。 */
    s->set_reg(s, 0x010C, 0x00, 0x00);

    uint8_t r45 = 0, aec = 0, r04 = 0, gain = 0, com3 = 0, com8 = 0;
    cam_reg_read(cam, 0x45, &r45);
    cam_reg_read(cam, 0x10, &aec);
    cam_reg_read(cam, 0x04, &r04);
    cam_reg_read(cam, 0x00, &gain);
    cam_reg_read(cam, 0x0C, &com3);
    cam_reg_read(cam, 0x13, &com8);

    int aec_lines = ((r45 & 0x3F) << 10) | (aec << 2) | (r04 & 0x03);
    const char *band = (com3 & 0x06) == 0x04 ? "50Hz"
                     : (com3 & 0x06) == 0x02 ? "auto" : "60Hz";
    printf("[CAM] AEC=%d行 gain=0x%02X COM3=0x%02X(%s) COM8=0x%02X"
           "(AEC=%d AGC=%d BAND=%d)\n",
           aec_lines, gain, com3, band, com8,
           com8 & 0x01, (com8 >> 2) & 1, (com8 >> 5) & 1);
    printf("[CAM] 当前输出 %dx%d | NVS 默认: %dx%d / q%d / %s / 增益 %dx / PCLK分频 %d\n",
           resolution[s->status.framesize].width,
           resolution[s->status.framesize].height,
           resolution[s_cfg_cam.fs].width, resolution[s_cfg_cam.fs].height,
           s_cfg_cam.q, s_cfg_cam.hz ? "50Hz" : "60Hz", s_cfg_cam.gc,
           s_cfg_cam.pclk);
}

esp_err_t cam_jpeg_set_denoise(int level)
{    if (!s_j.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_denoise == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (level < 0) {
        level = 0;
    }
    if (level > 4) {
        level = 4;
    }
    int r = s->set_denoise(s, level);
    ESP_LOGI(TAG, "降噪等级 = %d（0 关 / 4 最强），返回 %d", level, r);
    return (r == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t cam_jpeg_set_gain_ceiling(int x)
{
    if (!s_j.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_gainceiling == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* 直接把倍数映射到枚举：枚举顺序就是 2/4/8/16/32/64/128 */
    switch (x) {
    case 2: case 4: case 8: case 16: case 32: case 64: case 128: break;
    default: return ESP_ERR_INVALID_ARG;
    }
    int r = s->set_gainceiling(s, camcfg_gc_enum(x));
    ESP_LOGI(TAG, "增益上限 = %dx，返回 %d", x, r);
    if (r == 0) {
        s_cfg_cam.gc = (uint8_t)x;
        camcfg_save("gc", (uint8_t)x);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t cam_jpeg_set_gray(bool on)
{
    if (!s_j.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_special_effect == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* effect 2 = 灰度。这一档只保留 Y 分量、丢掉 CbCr，用来判断
     * "横纹到底在亮度里还是在色度里"：灰度干净说明亮度没问题，
     * 之前的彩色横纹是色度通道被污染。 */
    int r = s->set_special_effect(s, on ? 2 : 0);
    ESP_LOGI(TAG, "灰度效果 %s（返回 %d）", on ? "已开" : "已关", r);
    return (r == 0) ? ESP_OK : ESP_FAIL;
}
