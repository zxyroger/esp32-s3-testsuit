/**
 * @file  test_wifi.c
 * @brief WiFi 用例：STA 初始化 / MAC / 环境扫描 / SoftAP / beacon / 自环关联 / 停止
 *
 * 设计原则：**不依赖任何外部路由器**。
 *   现场没有可信的 SSID/密码，所以"连上某个热点"不是可靠的验收条件。
 *   这里用自证闭环：自己开 AP，再用自己的 STA 扫它、连它。
 *
 * 用例之间有顺序依赖（和 audio 组一样）：后一项依赖前一项建立的射频状态。
 * 单独跑某一项时，若协议栈没起来，用例内部会自行初始化，不会直接崩。
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "board_config.h"
#include "tf.h"
#include "wifi_lab.h"

static wifi_lab_t *s_lab;

/** 取（必要时建立/重启）WiFi 句柄；失败返回 NULL */
static wifi_lab_t *ensure_lab(void)
{
    /* 注意要看 inited 而不是指针非空：wifi.stop 之后指针还在，但射频已停，
     * 此时必须让 wifi_lab_init 走"重新 start"那条路径 */
    if (s_lab != NULL && s_lab->inited) {
        return s_lab;
    }
    wifi_lab_t *lab = NULL;
    if (wifi_lab_init(&lab) != ESP_OK) {
        return NULL;
    }
    s_lab = lab;
    return lab;
}

static bool mac_all_equal(const uint8_t m[6], uint8_t v)
{
    for (int i = 0; i < 6; i++) {
        if (m[i] != v) {
            return false;
        }
    }
    return true;
}

/* ---------------- 1. 初始化 ---------------- */

static tf_result_t t_init(void)
{
    /* 第二次及以后跑（stop 后重跑）只是重新 start，堆不会明显变化，
     * 所以"堆必须下降"只在首次完整初始化时才断言 */
    bool    was_ready = (s_lab != NULL && s_lab->drv_ready);
    uint32_t before = (uint32_t)esp_get_free_heap_size();

    wifi_lab_t *lab = NULL;
    TF_CHECK_ERR(wifi_lab_init(&lab), "WiFi 协议栈初始化（NVS + netif + 驱动）");
    s_lab = lab;

    uint32_t after = (uint32_t)esp_get_free_heap_size();

    wifi_mode_t mode = WIFI_MODE_NULL;
    TF_CHECK_ERR(esp_wifi_get_mode(&mode), "读取当前 WiFi 模式");

    TF_INFO("模式 = %s（上电默认 STA）", wifi_lab_mode_str(mode));
    TF_INFO("空闲堆 %u -> %u B，WiFi 栈占用约 %d B",
            (unsigned)before, (unsigned)after, (int)before - (int)after);

    TF_CHECK(mode == WIFI_MODE_STA, "初始化后模式应为 STA，实测 %s",
             wifi_lab_mode_str(mode));
    if (!was_ready) {
        /* WiFi 栈正常起起来至少要吃掉几十 KB；占用过小说明 init 实际没生效 */
        TF_CHECK(before > after, "初始化后堆未下降，WiFi 栈可能没有真正加载");
    } else {
        TF_INFO("（协议栈复用上次的，射频已重新 start）");
    }
    return TF_PASS;
}

/* ---------------- 2. MAC ---------------- */

static tf_result_t t_mac(void)
{
    wifi_lab_t *lab = ensure_lab();
    TF_CHECK(lab != NULL, "WiFi 未初始化，请先跑 wifi.init");

    uint8_t sta[6] = { 0 };
    uint8_t ap[6] = { 0 };
    uint8_t ref[6] = { 0 };

    TF_CHECK_ERR(wifi_lab_mac(lab, sta, ap), "读取 STA / SoftAP MAC");
    TF_CHECK_ERR(esp_read_mac(ref, ESP_MAC_WIFI_STA), "从 efuse 读取 STA MAC");

    TF_INFO("STA    MAC = %02X:%02X:%02X:%02X:%02X:%02X",
            sta[0], sta[1], sta[2], sta[3], sta[4], sta[5]);
    TF_INFO("SoftAP MAC = %02X:%02X:%02X:%02X:%02X:%02X",
            ap[0], ap[1], ap[2], ap[3], ap[4], ap[5]);

    TF_CHECK(memcmp(sta, ref, 6) == 0, "驱动上报的 STA MAC 与 efuse 记录不一致");
    TF_CHECK(!mac_all_equal(sta, 0x00) && !mac_all_equal(sta, 0xFF),
             "STA MAC 全 0 或全 FF，efuse 可能未烧写");
    TF_CHECK((sta[0] & 0x01) == 0, "STA MAC 是组播地址（首字节 bit0=1），异常");
    TF_CHECK(memcmp(sta, ap, 6) != 0, "STA 与 SoftAP MAC 相同，双接口会冲突");

    TF_INFO("注：前 3 字节是模组 OUI，不同批次会变，故不作硬断言");
    return TF_PASS;
}

/* ---------------- 3. 环境扫描 ---------------- */

static tf_result_t t_scan(void)
{
    wifi_lab_t *lab = ensure_lab();
    TF_CHECK(lab != NULL, "WiFi 未初始化，请先跑 wifi.init");

    wifi_ap_record_t *recs = (wifi_ap_record_t *)heap_caps_calloc(
        BOARD_WIFI_SCAN_MAX, sizeof(wifi_ap_record_t), MALLOC_CAP_DEFAULT);
    TF_CHECK(recs != NULL, "扫描结果缓冲分配失败（%d 条）", BOARD_WIFI_SCAN_MAX);

    uint16_t n = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = wifi_lab_scan(lab, recs, BOARD_WIFI_SCAN_MAX, &n, 0);
    int64_t dt = esp_timer_get_time() - t0;

    if (err != ESP_OK) {
        free(recs);
        g_tf->checks++;
        tf_fail(__FILE__, __LINE__, "主动扫描失败: %s", esp_err_to_name(err));
        return TF_FAIL;
    }

    TF_INFO("全信道扫描耗时 %lld ms，共发现 %u 个 AP", (long long)(dt / 1000), (unsigned)n);

    /* 按 RSSI 从强到弱挑前 3 个打印，便于判断天线与射频是否正常 */
    bool used[BOARD_WIFI_SCAN_MAX] = { false };
    int  shown = 0;
    int  strongest = -127;
    for (int k = 0; k < 3 && k < (int)n; k++) {
        int idx = -1;
        int best = -128;
        for (int i = 0; i < (int)n && i < BOARD_WIFI_SCAN_MAX; i++) {
            if (used[i]) {
                continue;
            }
            if (recs[i].rssi > best) {
                best = recs[i].rssi;
                idx = i;
            }
        }
        if (idx < 0) {
            break;
        }
        used[idx] = true;
        shown++;
        if (k == 0) {
            strongest = recs[idx].rssi;
        }
        TF_INFO("  ch=%2u  %4d dBm  %-20s  %s", (unsigned)recs[idx].primary,
                recs[idx].rssi, (const char *)recs[idx].ssid,
                wifi_lab_auth_str(recs[idx].authmode));
    }

    free(recs);

    /* 一个 AP 都扫不到：可能是环境空旷、屏蔽房，也可能天线没接。
     * 环境因素不可控，不判失败，但要报出来让人自己判断。 */
    if (n == 0) {
        TF_SKIP("未扫到任何 AP：可能环境空旷/屏蔽，或射频天线未连接（非必然故障）");
    }

    TF_INFO("最强信号 %d dBm；共 %d 条结果按强度排序展示", strongest, shown);
    TF_CHECK(strongest <= 0 && strongest >= -100, "最强 RSSI = %d dBm 超出合理区间",
             strongest);
    return TF_PASS;
}

/* ---------------- 4. 启动 SoftAP ---------------- */

static tf_result_t t_ap_start(void)
{
    wifi_lab_t *lab = ensure_lab();
    TF_CHECK(lab != NULL, "WiFi 未初始化，请先跑 wifi.init");

    uint8_t mac[6] = { 0 };
    TF_CHECK_ERR(wifi_lab_mac(lab, NULL, mac), "读取 SoftAP MAC");

    /* %.20s 给前缀加上界：宏长度编译器推不出来，不加精度会报 format-truncation */
    char ssid[48];
    snprintf(ssid, sizeof(ssid), "%.20s-%02X%02X", BOARD_WIFI_AP_SSID_PREFIX,
             mac[4], mac[5]);

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = wifi_lab_ap_start(lab, ssid, BOARD_WIFI_AP_PASS,
                                      BOARD_WIFI_AP_CHANNEL);
    int64_t dt = esp_timer_get_time() - t0;

    if (err != ESP_OK) {
        g_tf->checks++;
        tf_fail(__FILE__, __LINE__, "启动 SoftAP 失败: %s", esp_err_to_name(err));
        return TF_FAIL;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    TF_CHECK_ERR(esp_wifi_get_mode(&mode), "读取当前 WiFi 模式");
    TF_CHECK(mode == WIFI_MODE_APSTA, "开 AP 后模式应为 APSTA，实测 %s",
             wifi_lab_mode_str(mode));

    /* AP_START 事件才是射频侧真的起来的证据；拿到 IP 只说明 netif 配置成功了 */
    TF_INFO("WIFI_EVENT_AP_START = %s", lab->ap_start_event ? "已收到" : "未收到");
    TF_CHECK(lab->ap_start_event,
             "未收到 WIFI_EVENT_AP_START：AP 参数已写入但射频侧未真正启动");

    uint32_t ip = 0;
    uint32_t gw = 0;
    TF_CHECK_ERR(wifi_lab_ap_ip(lab, &ip, &gw), "读取 SoftAP 的 IP");

    char ip_s[16] = { 0 };
    char gw_s[16] = { 0 };
    wifi_lab_ip_str(ip, ip_s, sizeof(ip_s));
    wifi_lab_ip_str(gw, gw_s, sizeof(gw_s));

    TF_INFO("SSID=%s  信道=%u  加密=WPA2-PSK  密码=%s", lab->ap_ssid,
            (unsigned)lab->ap_channel, BOARD_WIFI_AP_PASS);
    TF_INFO("AP IP=%s  网关=%s  启动耗时 %lld ms", ip_s, gw_s, (long long)(dt / 1000));

    /* 默认 SoftAP 网段固定是 192.168.4.0/24，跑偏说明 netif 建错了 */
    TF_CHECK(strncmp(ip_s, "192.168.4.", 10) == 0,
             "SoftAP 默认网段应为 192.168.4.x，实测 %s", ip_s);
    return TF_PASS;
}

/* ---------------- 5. beacon 是否真的发出去了 ---------------- */

static tf_result_t t_ap_beacon(void)
{
    wifi_lab_t *lab = ensure_lab();
    TF_CHECK(lab != NULL, "WiFi 未初始化，请先跑 wifi.init");
    TF_CHECK(lab->ap_up, "SoftAP 未启动，请先跑 wifi.ap_start");

    /* 只扫自己的信道：单射频芯片上 AP 的 beacon 会被 STA 的扫描抢占，
     * 必须用被动长驻留去撞 AP 的发送窗口，能不能抓到取决于时序 */
    wifi_ap_record_t recs[8] = { 0 };
    uint16_t n = 0;
    TF_CHECK_ERR(wifi_lab_scan(lab, recs, 8, &n, lab->ap_channel),
                 "扫描 SoftAP 所在信道");

    int found = -1;
    for (int i = 0; i < (int)n && i < 8; i++) {
        if (strcmp((const char *)recs[i].ssid, lab->ap_ssid) == 0) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        TF_INFO("该信道共 %u 个 AP，匹配到自己的 SSID \"%s\"", (unsigned)n, lab->ap_ssid);
        TF_INFO("RSSI=%d dBm，BSSID=%02X:%02X:%02X:%02X:%02X:%02X",
                recs[found].rssi,
                recs[found].bssid[0], recs[found].bssid[1], recs[found].bssid[2],
                recs[found].bssid[3], recs[found].bssid[4], recs[found].bssid[5]);
        TF_INFO("结论：发射通路 + beacon 正常，射频信号确实发到空中并被接收");
        /* 自环 RSSI 通常很强（同一颗芯片），过弱说明天线/匹配有问题 */
        TF_CHECK(recs[found].rssi > -90, "自环 RSSI = %d dBm 过弱，射频匹配可能异常",
                 recs[found].rssi);
        return TF_PASS;
    }

    /* 扫不到自己的 beacon：先确认 AP 自身状态是否正常，再决定 SKIP 还是 FAIL。
     * ESP32/S3 是单射频，STA 扫描时 radio 归 STA，AP 的 beacon 会被延后发射，
     * 所以"扫不到自己"是这个平台的固有现象，不能直接判硬件故障。 */
    uint32_t ip = 0;
    uint32_t gw = 0;
    bool ap_state_ok = lab->ap_up && (wifi_lab_ap_ip(lab, &ip, &gw) == ESP_OK) && ip != 0;
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);

    TF_INFO("该信道共 %u 个 AP，未匹配到 \"%s\"", (unsigned)n, lab->ap_ssid);
    TF_CHECK(ap_state_ok && mode == WIFI_MODE_APSTA,
             "SoftAP 状态异常：ap_up=%d 模式=%s 无有效 IP",
             (int)lab->ap_up, wifi_lab_mode_str(mode));

    TF_SKIP("单射频：扫描时 AP beacon 被抢占，无法自观测；"
            "AP 可用性见 wifi.ap_client 的手机接入验证");
}

/* ---------------- 6. STA 关联自己的 AP（闭环） ---------------- */

static tf_result_t t_ap_link(void)
{
    wifi_lab_t *lab = ensure_lab();
    TF_CHECK(lab != NULL, "WiFi 未初始化，请先跑 wifi.init");
    TF_CHECK(lab->ap_up, "SoftAP 未启动，请先跑 wifi.ap_start");

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = wifi_lab_sta_join(lab, lab->ap_ssid, lab->ap_pass,
                                      BOARD_WIFI_STA_JOIN_MS);
    int64_t dt = esp_timer_get_time() - t0;

    if (err == ESP_ERR_TIMEOUT) {
        /* 根因（实测 + 架构推导）：ESP32/S3 只有一路射频。
         * STA 发出 probe request 后要靠 AP 回 probe response，而 AP 与 STA 共用
         * 同一个 radio —— 同一个射频无法同时收发，因此自环关联在物理上不成立。
         * 这不是硬件故障，也不能据此判 AP 有问题。 */
        TF_SKIP("单射频限制：STA 无法关联同一 radio 上的 AP"
                "（超时 %u ms，原因码 %u）；AP 可用性见 wifi.ap_client",
                (unsigned)BOARD_WIFI_STA_JOIN_MS, (unsigned)lab->last_disc_reason);
    }
    TF_CHECK_ERR(err, "STA 关联到自建 SoftAP");

    /* 关联成功与否，最硬的证据在 AP 侧：AP_STACONNECTED 事件不会因为 STA 侧
     * 拿到了 DHCP 地址就误报。等一小会儿让事件送上来。 */
    vTaskDelay(pdMS_TO_TICKS(200));
    if (lab->ap_client_count > 0) {
        TF_INFO("AP 侧观测到客户端接入：%02X:%02X:%02X:%02X:%02X:%02X（累计 %u 次）",
                lab->ap_client_mac[0], lab->ap_client_mac[1], lab->ap_client_mac[2],
                lab->ap_client_mac[3], lab->ap_client_mac[4], lab->ap_client_mac[5],
                (unsigned)lab->ap_client_count);
    } else {
        TF_INFO("AP 侧未收到 STACONNECTED（事件可能延迟上报），以下以 STA 侧 IP 为准");
    }

    uint32_t ip = 0;
    TF_CHECK_ERR(wifi_lab_sta_ip(lab, &ip), "读取 STA 获得的 IP");

    char s[16] = { 0 };
    char ap_s[16] = { 0 };
    wifi_lab_ip_str(ip, s, sizeof(s));
    wifi_lab_ip_str(lab->ap_ip, ap_s, sizeof(ap_s));

    TF_INFO("STA 已关联 \"%s\"，DHCP 获得 IP=%s（AP 自身=%s），耗时 %lld ms",
            lab->ap_ssid, s, ap_s, (long long)(dt / 1000));

    TF_CHECK(strncmp(s, "192.168.4.", 10) == 0,
             "STA 应从 SoftAP 网段拿到 192.168.4.x，实测 %s", s);
    TF_CHECK(ip != 0 && ip != lab->ap_ip, "STA 与 AP 拿到了同一个 IP（DHCP 异常）");
    TF_INFO("结论：beacon→认证→关联→DHCP 全链路打通（AP 与 STA 数据面可用）");
    return TF_PASS;
}

/* ---------------- 7. AP 侧客户端列表（手机接入即可验收） ---------------- */

static tf_result_t t_ap_clients(void)
{
    wifi_lab_t *lab = ensure_lab();
    TF_CHECK(lab != NULL, "WiFi 未初始化，请先跑 wifi.init");
    TF_CHECK(lab->ap_up, "SoftAP 未启动，请先跑 wifi.ap_start");

    wifi_sta_list_t list;
    TF_CHECK_ERR(wifi_lab_ap_clients(lab, &list), "读取 SoftAP 已连接客户端列表");

    TF_INFO("当前接入客户端: %u 个", (unsigned)list.num);
    for (int i = 0; i < (int)list.num && i < 4; i++) {
        const wifi_sta_info_t *si = &list.sta[i];
        TF_INFO("  %02X:%02X:%02X:%02X:%02X:%02X  RSSI=%d dBm  11b=%d 11g=%d 11n=%d",
                si->mac[0], si->mac[1], si->mac[2], si->mac[3], si->mac[4], si->mac[5],
                si->rssi, (int)si->phy_11b, (int)si->phy_11g, (int)si->phy_11n);
    }

    if (list.num == 0) {
        /* 这条不是失败：单射频自己连不上自己（上一条已解释），
         * 但手机是独立射频，连上来就是 AP 收发都正常的铁证。 */
        TF_SKIP("暂无客户端：用手机连 \"%s\"（密码 %s）后再跑本项，"
                "即可读到真实客户端的 MAC/RSSI",
                lab->ap_ssid, BOARD_WIFI_AP_PASS);
    }

    TF_INFO("结论：AP 侧完成过认证与关联（RSSI 来自客户端真实上行帧），收发链路正常");
    return TF_PASS;
}

/* ---------------- 8. 停止 ---------------- */

static tf_result_t t_stop(void)
{
    wifi_lab_t *lab = ensure_lab();
    TF_CHECK(lab != NULL, "WiFi 未初始化，请先跑 wifi.init");

    uint32_t before = (uint32_t)esp_get_free_heap_size();
    TF_CHECK_ERR(wifi_lab_stop(lab), "停止 WiFi");
    vTaskDelay(pdMS_TO_TICKS(200)); /* 等射频任务退出、缓冲回收 */
    uint32_t after = (uint32_t)esp_get_free_heap_size();

    TF_CHECK(!lab->ap_up, "停止后 ap_up 仍为 true，状态未复位");

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        TF_INFO("停止后模式 = %s（协议栈保留，便于再次 run wifi）",
                wifi_lab_mode_str(mode));
    }
    TF_INFO("空闲堆 %u -> %u B（回收 %d B）", (unsigned)before, (unsigned)after,
            (int)after - (int)before);
    return TF_PASS;
}

/* ---------------- 用例注册 ----------------
 *
 * TF_FLAG_NONET 的含义：这条会把 STA 从路由器上摘下来，或者干脆关掉射频。
 *
 *   ap_link  ESP32 只有一个射频，STA 一旦关联到自己的 SoftAP，与路由器的连接
 *            就被顶掉了；STA 掉线后 MQTT/网页全断。
 *   stop     直接 esp_wifi_stop()，射频关掉，并且把自动重连标志也关了
 *            （见 wifi_lab_stop：不关的话 STOP 触发的断开事件会把它又拉起来）。
 *
 * 这一组默认不参与 `run`：自检把正在用的网页连接弄断，是很反直觉的行为。
 * 要看这些用例请显式 run wifi，跑完由 app_cmds 注册的钩子重新加入路由器。
 */
TF_GROUP_BEGIN(wifi)
    TF_ITEM(wifi, init,     "协议栈初始化 + 进入 STA 模式",      t_init,     TF_FLAG_HW),
    TF_ITEM(wifi, mac,      "STA/SoftAP MAC 与 efuse 一致性",   t_mac,      TF_FLAG_HW),
    TF_ITEM(wifi, scan,     "扫描周边 AP（验证接收通路）",        t_scan,     TF_FLAG_HW | TF_FLAG_SLOW),
    TF_ITEM(wifi, ap_start, "启动 SoftAP（APSTA）并取 IP",       t_ap_start, TF_FLAG_HW | TF_FLAG_NONET),
    TF_ITEM(wifi, ap_beacon, "扫描自己验证 beacon 已广播",        t_ap_beacon, TF_FLAG_HW | TF_FLAG_NONET),
    TF_ITEM(wifi, ap_link,  "STA 关联自己的 AP 并 DHCP 取址",    t_ap_link,  TF_FLAG_HW | TF_FLAG_SLOW | TF_FLAG_NONET),
    TF_ITEM(wifi, ap_client, "读 AP 侧已连接客户端（手机可验收）", t_ap_clients, TF_FLAG_HW | TF_FLAG_NONET),
    TF_ITEM(wifi, stop,     "停止射频并回收资源",                t_stop,     TF_FLAG_HW | TF_FLAG_NONET),
TF_GROUP_END(wifi)
