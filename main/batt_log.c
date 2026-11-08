/**
 * @file  batt_log.c
 * @brief 每 3 秒读一次 AXP2101 电池状态，拼成一行 JSON 推到 MQTT
 *
 * 发出去的样子（topic = <主 topic 前缀>/batt）：
 *   {"up":123,"vbat":3965,"vsys":4123,"vbus":5034,"chg":2,"dir":1,"pct":87,...}
 *
 * 字段含义：
 *   up   上电以来的秒数（板子没有 NTP，时间基准用页面收到时的本地时间）
 *   vbat 电池电压 mV      vsys 系统轨 mV      vbus 输入 mV
 *   chg  充电阶段 0涓流 1预充 2恒流 3恒压 4充满 5未充
 *   dir  电流方向 0待机 1充电 2放电
 *   pct  电量百分比，-1 表示燃料计读不到
 *   st1/st2  两个状态寄存器原值，页面留着排错用
 *   bp/vg    bat_present / vbus_good，0 或 1
 *
 * 关于「充电电流」：AXP2101 的 ADC 只有电压和温度通道，没有电流采样。
 * 电流只能靠充电阶段推断，或者用电压变化率（mV/min）间接反映充电快慢。
 * 页面显示的「充电速率」就是后者，不冒充真实电流。
 */
#include "batt_log.h"

#include <stdio.h>

#include "axp2101.h"
#include "board.h"
#include "cam_mqtt.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "batt_log";

#define DEF_PERIOD_MS 3000

static TaskHandle_t s_task;
static int      s_period_ms = DEF_PERIOD_MS;
static unsigned s_sent;

static void batt_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        axp2101_t *pmic = board_pmic();
        if (pmic == NULL) {
            /* PMIC 没挂上（I2C 没通）时不要刷日志，等下一轮 */
            vTaskDelay(pdMS_TO_TICKS(s_period_ms));
            continue;
        }

        axp2101_batt_t b;
        if (axp2101_read_batt(pmic, &b) == ESP_OK) {
            char js[224];
            int n = snprintf(js, sizeof(js),
                             "{\"up\":%u,\"vbat\":%u,\"vsys\":%u,\"vbus\":%u,"
                             "\"chg\":%u,\"dir\":%u,\"pct\":%d,"
                             "\"st1\":%u,\"st2\":%u,\"bp\":%u,\"vg\":%u}",
                             (unsigned)(esp_timer_get_time() / 1000000),
                             (unsigned)b.vbat_mv, (unsigned)b.vsys_mv,
                             (unsigned)b.vbus_mv,
                             (unsigned)b.chg_stat, (unsigned)b.cur_dir,
                             b.pct == 0xFF ? -1 : (int)b.pct,
                             (unsigned)b.st1, (unsigned)b.st2,
                             b.bat_present ? 1u : 0u, b.vbus_good ? 1u : 0u);
            if (n > 0 && cam_mqtt_publish_text("batt", js) == ESP_OK) {
                s_sent++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(s_period_ms));
    }
}

esp_err_t batt_log_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    /* 栈 3072：只有 snprintf + I2C 读，用不了多少，但 TLS 写是在
     * cam_mqtt 的任务里做的，这里不占 TLS 的栈 */
    if (xTaskCreate(batt_task_entry, "batt_log", 3072, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "电池采样已启动，周期 %d ms", s_period_ms);
    return ESP_OK;
}

void batt_log_set_period(int ms)
{
    if (ms < 1000) {
        ms = 1000;
    }
    if (ms > 60000) {
        ms = 60000;
    }
    s_period_ms = ms;
}

int batt_log_period(void)
{
    return s_period_ms;
}

unsigned batt_log_sent(void)
{
    return s_sent;
}
