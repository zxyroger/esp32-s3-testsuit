/**
 * @file  health.c
 * @brief 自愈看门狗实现，判据与取舍见 health.h
 */
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "cam_jpeg.h"
#include "cam_mqtt.h"
#include "health.h"
#include "wifi_lab.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "health";

/* --------- 阈值：改这里就行 --------- */
#define TICK_MS           1000   /* 巡检周期 */
#define CAM_STALL_MS      5000   /* 摄像头多久没新帧判定卡死 */
#define CAM_RESTART_MAX   3      /* 连续重启几次还不行就整机重启 */
#define MQTT_STALL_MS     60000  /* 链路多久没成功写出判定假死 */
#define MQTT_RETRY_MAX    2      /* 连续重连几次还不行就整机重启 */
#define MQTT_RETRY_GAP_MS 15000  /* 两次重连之间留的观察窗口 */
#define RESTART_GRACE_MS  500    /* 重启前给日志落盘/串口刷完的时间 */

static health_stat_t s_st;
static bool s_started;
static volatile bool s_force_cam_trip;   /* health test：手动触发一次摄像头自愈 */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 有没有拿到 IP。拿不到协议栈状态时返回 true —— 宁可让 MQTT 自己判，
 * 也不要因为读不到 WiFi 状态就把真正的假死放过。 */
static bool wifi_iface_up(void)
{
    wifi_lab_t *lab = NULL;
    if (wifi_lab_init(&lab) != ESP_OK || lab == NULL) {
        return true;
    }
    uint32_t ip = 0;
    return (wifi_lab_sta_ip(lab, &ip) == ESP_OK && ip != 0);
}

/* 重新初始化传感器。stop() 在最坏情况下要等 5 s（采集任务阻塞在
 * esp_camera_fb_get 里），所以这里必须是独立任务，不能塞进别的循环。 */
static void cam_restart(void)
{
    cam_jpeg_stop();

    cam_t *cam = board_camera();
    axp2101_t *pmic = board_pmic();
    if (cam == NULL || pmic == NULL) {
        ESP_LOGE(TAG, "摄像头/PMIC 句柄不可用，无法重启");
        return;
    }

    /* deinit 与 init 之间让 PWDN、XCLK 落稳，实测不留这段时间偶尔
     * 会 probe 到 0xFF（传感器没醒）。 */
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_err_t err = cam_jpeg_start(cam, pmic);
    if (err == ESP_OK) {
        s_st.cam_restarts++;
        ESP_LOGI(TAG, "摄像头已重启（累计 %u 次）", (unsigned)s_st.cam_restarts);
    } else {
        ESP_LOGE(TAG, "摄像头重启失败: %s", esp_err_to_name(err));
    }
}

static void health_task(void *arg)
{
    (void)arg;

    uint32_t last_seq = 0;
    uint32_t seq_ms = now_ms();
    int cam_streak = 0;              /* 连续"重启了还是没帧"的次数 */
    int mqtt_streak = 0;             /* 连续"重连了还是没写出"的次数 */
    uint32_t mqtt_wait_until = 0;

    ESP_LOGI(TAG, "看门狗启动：摄像头 %d s 无帧重启，MQTT %d s 无成功写出重连",
             CAM_STALL_MS / 1000, MQTT_STALL_MS / 1000);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        uint32_t now = now_ms();

        /* ---------------- 摄像头 ---------------- */
        s_st.cam_running = cam_jpeg_running();
        if (!s_st.cam_running) {
            /* 用户自己 cam stop 的，别去拉起；计数清零 */
            cam_streak = 0;
            last_seq = 0;
            seq_ms = now;
            s_st.cam_seq = 0;
            s_st.cam_idle_ms = 0;
        } else {
            cam_jpeg_stat_t cs;
            cam_jpeg_get_stat(&cs);
            s_st.cam_seq = cs.seq;

            if (cs.seq != last_seq) {
                last_seq = cs.seq;
                seq_ms = now;
                cam_streak = 0;      /* 又出帧了，之前的失败不算连续 */
            }
            s_st.cam_idle_ms = now - seq_ms;

            if (s_force_cam_trip) {
                /* 手动验证用（health test）：假装已经卡了很久，
                 * 下面的自愈流程会真的走一遍 stop -> init -> 恢复出图。
                 * 这样上线前就能确认"救得回来"，不用等真的卡死。 */
                s_force_cam_trip = false;
                s_st.cam_idle_ms = CAM_STALL_MS;
                ESP_LOGW(TAG, "手动触发摄像头自愈测试");
            }

            if (s_st.cam_idle_ms >= CAM_STALL_MS) {
                s_st.cam_trips++;
                cam_streak++;
                if (cam_streak > CAM_RESTART_MAX) {
                    ESP_LOGE(TAG, "摄像头连续 %d 次重启仍无帧，整机重启%s",
                             cam_streak - 1,
                             wifi_iface_up() ? "" : "（网络此时也不通）");
                    s_st.reboots++;
                    vTaskDelay(pdMS_TO_TICKS(RESTART_GRACE_MS));
                    esp_restart();
                }
                ESP_LOGW(TAG, "摄像头 %u 秒无新帧（seq=%u），第 %d 次重启",
                         (unsigned)(s_st.cam_idle_ms / 1000),
                         (unsigned)cs.seq, cam_streak);
                cam_restart();
                /* start 会把 seq 归零，重新开始计 */
                last_seq = 0;
                seq_ms = now_ms();
            }
        }

        /* ---------------- MQTT 链路 ---------------- */
        if (!cam_mqtt_enabled()) {
            mqtt_streak = 0;
            s_st.mqtt_idle_ms = 0;
            continue;
        }

        uint32_t idle = cam_mqtt_idle_ms();
        s_st.mqtt_idle_ms = idle;
        s_st.mqtt_ever_ok = cam_mqtt_ever_ok();

        if (idle <= MQTT_STALL_MS) {
            mqtt_streak = 0;             /* 链路是活的 */
        } else if (!s_st.mqtt_ever_ok) {
            /* 开机到现在一次都没发出去：多半没网 / 证书 / broker 挂了。
             * 整机重启对这三件事都没帮助，只能等，别做成重启炸弹。 */
            mqtt_streak = 0;
        } else if (now < mqtt_wait_until) {
            /* 刚发过重连请求，给 TLS 握手续命的时间 */
        } else if (!wifi_iface_up()) {
            /* 没有 IP：这是 WiFi 的事，不记 MQTT 的账 */
            mqtt_streak = 0;
        } else {
            mqtt_streak++;
            if (mqtt_streak > MQTT_RETRY_MAX) {
                ESP_LOGE(TAG, "MQTT 连续 %d 次重连仍无成功写出，整机重启",
                         mqtt_streak - 1);
                s_st.reboots++;
                vTaskDelay(pdMS_TO_TICKS(RESTART_GRACE_MS));
                esp_restart();
            }
            ESP_LOGW(TAG, "MQTT %u 秒没有成功写出，第 %d 次重连",
                     (unsigned)(idle / 1000), mqtt_streak);
            cam_mqtt_reconnect();
            s_st.mqtt_reconnects++;
            mqtt_wait_until = now + MQTT_RETRY_GAP_MS;
        }
    }
}

esp_err_t health_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    memset(&s_st, 0, sizeof(s_st));
    s_started = true;

    /* 4 KB 栈：cam_jpeg_stop() 里要等采集任务退出，日志格式化也在本任务里 */
    if (xTaskCreate(health_task, "health", 4096, NULL, 2, NULL) != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "看门狗任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void health_get_stat(health_stat_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_st;
}

void health_force_trip(void)
{
    s_force_cam_trip = true;
}

void health_dump(void)
{
    health_stat_t s;
    health_get_stat(&s);

    if (!s_started) {
        printf("[健康] 看门狗未启动\n");
        return;
    }

    printf("[健康] 阈值   : 摄像头 %d s 无帧 / MQTT %d s 无成功写出\n",
           CAM_STALL_MS / 1000, MQTT_STALL_MS / 1000);

    if (s.cam_running) {
        printf("[健康] 摄像头 : 运行中，seq=%u，最近 %u 秒无新帧\n",
               (unsigned)s.cam_seq, (unsigned)(s.cam_idle_ms / 1000));
    } else {
        printf("[健康] 摄像头 : 未启动（手动 stop 的不管）\n");
    }
    printf("[健康] 摄像头 : 判定卡死 %u 次，重启 %u 次\n",
           (unsigned)s.cam_trips, (unsigned)s.cam_restarts);

    if (!cam_mqtt_enabled()) {
        printf("[健康] MQTT   : 推流已关闭，不巡检\n");
    } else if (s.mqtt_idle_ms == 0xFFFFFFFFu) {
        printf("[健康] MQTT   : 本次开机还没有成功写出过（不升级为重启）\n");
    } else {
        printf("[健康] MQTT   : 最近成功写出 %u 秒前，重连 %u 次\n",
               (unsigned)(s.mqtt_idle_ms / 1000), (unsigned)s.mqtt_reconnects);
    }

    printf("[健康] 整机重启 : %u 次（仅统计本次运行）\n", (unsigned)s.reboots);
}
