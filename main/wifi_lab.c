/**
 * @file  wifi_lab.c
 * @brief WiFi 射频自检封装实现
 *
 * 关键取舍说明：
 *   1. NVS 必须可用：esp_wifi_init() 内部要读写校准/配置。本工程此前没用过 NVS，
 *      所以这里自己初始化，并对 NVS 满页 / 版本变更做擦除重建（否则第一次上电就挂）。
 *   2. 事件用 EventGroup 等待，不用轮询：连接结果是异步事件，轮询会读到过期状态。
 *   3. esp_wifi 的日志默认很吵（每帧 beacon 都可能打），会把测试报告冲乱，
 *      这里统一降到 WARN；要排错时用 esp_log_level_set("wifi", ESP_LOG_INFO) 临时打开。
 *   4. stop 只停射频、不 deinit：测试里可能要重复 `run wifi`，
 *      反复 init/deinit 容易在 netif 销毁顺序上踩坑，保留协议栈更稳。
 */
#include <string.h>

#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "wifi_lab.h"

static const char *TAG = "wifi_lab";

#define EV_GOT_IP    BIT0
#define EV_DISC      BIT1
#define EV_AP_CLIENT BIT2
#define EV_AP_START  BIT3

static wifi_lab_t        s_lab;
static EventGroupHandle_t s_evt;

/* 自动重连开关。
 * 之前这里完全没有重连逻辑，掉线就永久离线 —— 实测板子跑了 6.6 小时后
 * 跟 AP 断开一次，之后 6 小时一直"未连接"，只能人工发 wifi join 救回来。
 * 现在只要拿过 IP 就打开这个开关，掉线自动重试。
 * 主动停网（wifi_lab_stop / 测试组收尾）会把开关先关掉，否则停不干净。 */
static volatile bool s_auto_reconnect;
static volatile int64_t s_last_retry_us;

/* ---------------- 事件 ---------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        s_lab.last_disc_reason = d->reason;
        if (s_evt != NULL) {
            xEventGroupSetBits(s_evt, EV_DISC);
        }
        /* 掉线自动重连。
         * 节流到 2 s：路由器重启时 AP 要几十秒才起来，不节流会把日志刷爆并
         * 让射频一直在扫描/关联之间空转，反而拖慢恢复。
         * esp_wifi_connect() 是异步的，在事件回调里调用是官方 STA 示例的写法。 */
        if (s_auto_reconnect && s_lab.inited && s_lab.started) {
            int64_t now = esp_timer_get_time();
            if (now - s_last_retry_us >= 2 * 1000 * 1000) {
                s_last_retry_us = now;
                esp_err_t e = esp_wifi_connect();
                ESP_LOGW(TAG, "WiFi 断开(reason=%u)，自动重连 %s",
                         (unsigned)d->reason, esp_err_to_name(e));
            }
        }
    } else if (id == WIFI_EVENT_AP_START) {
        /* AP 接口真的启动了的硬证据：配置成功只代表参数写进去了，
         * 这个事件代表射频侧确实把 AP 带起来了 */
        s_lab.ap_start_event = true;
        if (s_evt != NULL) {
            xEventGroupSetBits(s_evt, EV_AP_START);
        }
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        /* 自环测试里这是最硬的证据：AP 侧真的收到了客户端发来的关联请求。
         * 只看 STA 侧"拿到 IP 了"可能被 DHCP 缓存骗过，AP 侧事件不会。 */
        const wifi_event_ap_staconnected_t *e = (const wifi_event_ap_staconnected_t *)data;
        memcpy(s_lab.ap_client_mac, e->mac, 6);
        s_lab.ap_client_count++;
        if (s_evt != NULL) {
            xEventGroupSetBits(s_evt, EV_AP_CLIENT);
        }
        ESP_LOGI(TAG, "AP 侧观测到客户端接入: %02X:%02X:%02X:%02X:%02X:%02X aid=%u",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                 (unsigned)e->aid);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        s_lab.sta_ip = e->ip_info.ip.addr;
        if (s_evt != NULL) {
            xEventGroupSetBits(s_evt, EV_GOT_IP);
        }
#if CONFIG_LWIP_IPV6
        /* IPv6 链路本地地址必须等接口真正 up 之后再建，否则地址不会被激活，
         * 接口就一直收不到 RA、拿不到全局地址（这个顺序踩过坑）。
         * 放在 GOT_IP 事件里做，是因为此时 netif 一定已经 up。
         * 重复调用会把地址状态打回 TENTATIVE，所以只做一次。 */
        static bool v6_linklocal_done = false;
        if (!v6_linklocal_done && e->esp_netif != NULL) {
            v6_linklocal_done = true;
            esp_err_t err = esp_netif_create_ip6_linklocal(e->esp_netif);
            ESP_LOGI(TAG, "IPv6 链路本地地址: %s", esp_err_to_name(err));
        }
#endif
    }
}

/* ---------------- 初始化 ---------------- */

static void quiet_wifi_logs(void)
{
    /* 报告要能一眼看完，射频栈的逐帧日志默认全关到 WARN */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("phy", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("wifi_lab", ESP_LOG_INFO);
}

esp_err_t wifi_lab_init(wifi_lab_t **out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lab.started) {
        *out = &s_lab;
        return ESP_OK;
    }

    quiet_wifi_logs();

    /* 协议栈已经 init 过、只是被 stop 了：不能重复 esp_wifi_init（会返回 INIT_STATE），
     * 只需要重新 start。这条路径保证 `run wifi` 可以反复跑。 */
    if (s_lab.drv_ready) {
        esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "重新设置 STA 模式失败: %s", esp_err_to_name(e));
            return e;
        }
        e = esp_wifi_start();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "重启 WiFi 失败: %s", esp_err_to_name(e));
            return e;
        }
        esp_wifi_set_ps(WIFI_PS_NONE);
        s_lab.ap_up = false;
        s_lab.started = true;
        s_lab.inited = true;
        if (s_evt != NULL) {
            xEventGroupClearBits(s_evt, EV_GOT_IP | EV_DISC | EV_AP_CLIENT);
        }
        ESP_LOGI(TAG, "WiFi 重新启动（协议栈复用）");
        *out = &s_lab;
        return ESP_OK;
    }

    /* 1. NVS —— WiFi 驱动依赖它存校准与配置 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建（%s），擦除后重新初始化", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. netif 与默认事件循环。二者都可能已被别处建过，重复调用要容忍。 */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (s_evt == NULL) {
        s_evt = xEventGroupCreate();
        if (s_evt == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_evt, EV_GOT_IP | EV_DISC);

    /* 3. 默认 STA netif（自动带 DHCP client） */
    s_lab.sta_netif = esp_netif_create_default_wifi_sta();
    if (s_lab.sta_netif == NULL) {
        ESP_LOGE(TAG, "创建默认 STA netif 失败");
        return ESP_FAIL;
    }

    /* 3.5 IPv6 链路本地地址要等联网成功后再建，见 on_ip_event() */

    /* 4. 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }

    /* 配置只放 RAM：测试台不该把 SSID/密码写进 flash 反复磨损 */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_ip_event, NULL, NULL));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }

    /* 测试场景要的是响应速度，不是省电 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_lab.drv_ready = true;
    s_lab.started = true;
    s_lab.inited = true;
    s_lab.ap_channel = BOARD_WIFI_AP_CHANNEL;

    err = wifi_lab_mac(&s_lab, s_lab.sta_mac, s_lab.ap_mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读 MAC 失败: %s", esp_err_to_name(err));
    }

    *out = &s_lab;
    ESP_LOGI(TAG, "WiFi 已启动，模式 STA，STA MAC %02X:%02X:%02X:%02X:%02X:%02X",
             s_lab.sta_mac[0], s_lab.sta_mac[1], s_lab.sta_mac[2],
             s_lab.sta_mac[3], s_lab.sta_mac[4], s_lab.sta_mac[5]);
    return ESP_OK;
}

/* ---------------- MAC / 扫描 ---------------- */

esp_err_t wifi_lab_mac(wifi_lab_t *lab, uint8_t sta[6], uint8_t ap[6])
{
    if (lab == NULL || !lab->inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sta != NULL) {
        esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, sta);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (ap != NULL) {
        /* AP 接口未启用时 get_mac(WIFI_IF_AP) 会返回 NOT_STARTED，
         * 改从 efuse 直接取（ESP_MAC_WIFI_SOFTAP），任何时刻都能读。 */
        esp_err_t err = esp_read_mac(ap, ESP_MAC_WIFI_SOFTAP);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t wifi_lab_scan(wifi_lab_t *lab, wifi_ap_record_t *out, uint16_t cap,
                        uint16_t *n, uint8_t channel)
{
    if (lab == NULL || !lab->inited || out == NULL || n == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *n = 0;

    wifi_scan_config_t sc = { 0 };
    sc.ssid = NULL;
    sc.bssid = NULL;
    sc.channel = channel;      /* 0 = 全信道 */
    sc.show_hidden = true;
    if (channel == 0) {
        /* 环境普查：主动探测，每信道停留压到下限，全信道 1~2 秒出结果 */
        sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        sc.scan_time.active.min = 60;
        sc.scan_time.active.max = 120;
    } else {
        /* 单信道定点：被动长驻留。单射频芯片上 AP 的 beacon 会被 STA 的扫描抢占，
         * 驻留越久越有机会撞上 AP 的发送窗口，600 ms 是实测能抓到的下限附近。 */
        sc.scan_type = WIFI_SCAN_TYPE_PASSIVE;
        sc.scan_time.passive = 600;
    }

    esp_err_t err = esp_wifi_scan_start(&sc, true); /* true = 阻塞到扫描结束 */
    if (err != ESP_OK) {
        return err;
    }

    uint16_t num = 0;
    err = esp_wifi_scan_get_ap_num(&num);
    if (err != ESP_OK) {
        return err;
    }
    if (num > cap) {
        num = cap; /* 超出缓存的部分丢弃；get_ap_records 仍会释放内部全部记录 */
    }
    uint16_t got = num;
    err = esp_wifi_scan_get_ap_records(&got, out);
    if (err != ESP_OK) {
        return err;
    }
    *n = got;
    return ESP_OK;
}

/* ---------------- SoftAP ---------------- */

esp_err_t wifi_lab_ap_start(wifi_lab_t *lab, const char *ssid, const char *pass,
                            uint8_t channel)
{
    if (lab == NULL || !lab->inited || ssid == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (lab->ap_netif == NULL) {
        lab->ap_netif = esp_netif_create_default_wifi_ap();
        if (lab->ap_netif == NULL) {
            return ESP_FAIL;
        }
    }

    size_t sl = strlen(ssid);
    if (sl > 32) {
        sl = 32;
    }
    memcpy(lab->ap_ssid, ssid, sl);
    lab->ap_ssid[sl] = '\0';
    lab->ap_pass[0] = '\0';

    wifi_config_t cfg = { 0 };
    memcpy(cfg.ap.ssid, ssid, sl);
    cfg.ap.ssid_len = (uint8_t)sl;
    cfg.ap.channel = channel;
    cfg.ap.max_connection = BOARD_WIFI_AP_MAX_CONN;
    cfg.ap.pmf_cfg.required = false;

    size_t pl = (pass != NULL) ? strlen(pass) : 0;
    if (pl >= 8) {
        if (pl > 64) {
            pl = 64;
        }
        memcpy(cfg.ap.password, pass, pl);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        memcpy(lab->ap_pass, pass, pl);
        lab->ap_pass[pl] = '\0';
    } else {
        /* WPA2 要求密码 >=8 位，短密码只能开开放网络 */
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    lab->ap_channel = channel;
    lab->ap_start_event = false;
    if (s_evt != NULL) {
        xEventGroupClearBits(s_evt, EV_AP_START | EV_AP_CLIENT);
    }

    /* 切 APSTA：保留 STA 接口，后面要靠它去关联自己 */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    lab->ap_up = true;

    /* 读回 AP 接口实际 MAC：STA 直连时要用它做 BSSID 绑定（跳过扫描，
     * 这是单射频下能连上自己 AP 的关键，见 wifi_lab_sta_join 的注释） */
    if (esp_wifi_get_mac(WIFI_IF_AP, lab->ap_mac) != ESP_OK) {
        esp_read_mac(lab->ap_mac, ESP_MAC_WIFI_SOFTAP);
    }

    /* 等 AP_START 事件：这才是"射频侧真的把 AP 带起来了"的判据。
     * 拿到 IP 只说明 netif 软件层配置好了，不代表射频在工作。 */
    if (s_evt != NULL) {
        xEventGroupWaitBits(s_evt, EV_AP_START, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
    }

    /* beacon 起播到射频稳定再留一点余量 */
    vTaskDelay(pdMS_TO_TICKS(500));

    return wifi_lab_ap_ip(lab, &lab->ap_ip, &lab->ap_gw);
}

esp_err_t wifi_lab_ap_ip(wifi_lab_t *lab, uint32_t *ip, uint32_t *gw)
{
    if (lab == NULL || !lab->ap_up || lab->ap_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t info;
    esp_err_t err = esp_netif_get_ip_info(lab->ap_netif, &info);
    if (err != ESP_OK) {
        return err;
    }
    if (ip != NULL) {
        *ip = info.ip.addr;
    }
    if (gw != NULL) {
        *gw = info.gw.addr;
    }
    return ESP_OK;
}

/* ---------------- STA 关联 ---------------- */

esp_err_t wifi_lab_sta_kick(wifi_lab_t *lab)
{
    if (lab == NULL || !lab->inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 故意不动 s_auto_reconnect：这里要模拟的是"意外掉线"。
     * 走 esp_wifi_disconnect 而不是 esp_wifi_stop，射频不会停，
     * 事件回调里的重连路径跟真实掉线完全一致。 */
    return esp_wifi_disconnect();
}

esp_err_t wifi_lab_sta_join(wifi_lab_t *lab, const char *ssid, const char *pass,
                            uint32_t timeout_ms)
{
    if (lab == NULL || !lab->inited || ssid == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 先关自动重连再 disconnect，否则下面那句断开会立刻触发回调重连，
     * 跟即将写入的新配置打架（换 SSID 时会连回旧 AP）。 */
    s_auto_reconnect = false;
    esp_wifi_disconnect(); /* 允许从任意状态开始，失败无妨 */
    lab->sta_ip = 0;
    xEventGroupClearBits(s_evt, EV_GOT_IP | EV_DISC | EV_AP_CLIENT);

    wifi_config_t cfg = { 0 };
    size_t sl = strlen(ssid);
    if (sl > 32) {
        sl = 32;
    }
    memcpy(cfg.sta.ssid, ssid, sl);
    if (pass != NULL && strlen(pass) >= 8) {
        size_t pl = strlen(pass);
        if (pl > 64) {
            pl = 64;
        }
        memcpy(cfg.sta.password, pass, pl);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    /* 关键：绑定 BSSID + 信道，跳过扫描直接关联。
     * 单射频芯片上，STA 做全信道扫描时 AP 的 beacon 会被扫描抢占发射机会，
     * 实测表现为 NO_AP_FOUND(201)，STA 永远连不上自己的 AP。
     * 指定 bssid/channel 后 STA 在目标信道上直接发 probe/auth，不再依赖先听到 beacon，
     * 这是自环关联唯一稳定的走法。 */
    if (lab->ap_up) {
        cfg.sta.bssid_set = 1;
        memcpy(cfg.sta.bssid, lab->ap_mac, 6);
        cfg.sta.channel = lab->ap_channel;
        cfg.sta.scan_method = WIFI_FAST_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    } else {
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }
    /* 从这一刻起允许自动重连：配置已经写进驱动，之后掉线都是"意外"。 */
    s_last_retry_us = esp_timer_get_time();
    s_auto_reconnect = true;

    EventBits_t bits = xEventGroupWaitBits(s_evt, EV_GOT_IP, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & EV_GOT_IP) == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t wifi_lab_sta_ip(wifi_lab_t *lab, uint32_t *ip)
{
    if (lab == NULL || ip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lab->sta_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t info;
    esp_err_t err = esp_netif_get_ip_info(lab->sta_netif, &info);
    if (err != ESP_OK) {
        return err;
    }
    lab->sta_ip = info.ip.addr;
    *ip = info.ip.addr;
    return ESP_OK;
}

const char *wifi_lab_sta_ip6(wifi_lab_t *lab, char *buf, size_t len)
{
    if (lab == NULL || buf == NULL || len < 8) {
        return NULL;
    }
    buf[0] = '\0';
    if (lab->sta_netif == NULL) {
        return NULL;
    }

    /* 一个接口上可能同时挂着 fe80:: 链路本地和 RA 下发的全局地址，
     * 还有 SLAAC 的临时地址；只有 2000::/3 这段（全球单播）才是能从外网打的。 */
    esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    int n = esp_netif_get_all_ip6(lab->sta_netif, addrs);
    for (int i = 0; i < n && i < CONFIG_LWIP_IPV6_NUM_ADDRESSES; i++) {
        char tmp[64] = {0};
        if (inet_ntop(AF_INET6, addrs[i].addr, tmp, sizeof(tmp)) == NULL) {
            continue;
        }
        ESP_LOGI(TAG, "IPv6[%d] %s", i, tmp);
        const uint8_t *b = (const uint8_t *)addrs[i].addr;
        if ((b[0] & 0xE0) != 0x20) {   /* 2000::/3 之外的都不要 */
            continue;
        }
        snprintf(buf, len, "%s", tmp);
        return buf;
    }
    return NULL;
}

esp_err_t wifi_lab_ap_clients(wifi_lab_t *lab, wifi_sta_list_t *list)
{
    if (lab == NULL || list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lab->ap_up) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(list, 0, sizeof(*list));
    return esp_wifi_ap_get_sta_list(list);
}

/* ---------------- 停止 ---------------- */

esp_err_t wifi_lab_stop(wifi_lab_t *lab)
{
    if (lab == NULL || !lab->inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 必须在 disconnect 之前关掉自动重连：否则 STOP 触发的断开事件
     * 会马上把连接又拉起来，esp_wifi_stop() 之后网卡还在乱动。 */
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();
    lab->ap_up = false;
    lab->sta_ip = 0;
    lab->started = false;
    lab->inited = false;   /* 协议栈仍在（drv_ready 保持），但射频已停 */
    return err;
}

/* ---------------- 字符串辅助 ---------------- */

const char *wifi_lab_mode_str(wifi_mode_t mode)
{
    switch (mode) {
    case WIFI_MODE_NULL:  return "NULL";
    case WIFI_MODE_STA:   return "STA";
    case WIFI_MODE_AP:    return "AP";
    case WIFI_MODE_APSTA: return "APSTA";
    default:              return "?";
    }
}

const char *wifi_lab_auth_str(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    default:                        return "?";
    }
}

void wifi_lab_ip_str(uint32_t ip, char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    /* esp_ip4_addr_t.addr 是网络字节序：低地址字节 = 第一个十进制段 */
    snprintf(buf, len, "%u.%u.%u.%u",
             (unsigned)(ip & 0xffu), (unsigned)((ip >> 8) & 0xffu),
             (unsigned)((ip >> 16) & 0xffu), (unsigned)((ip >> 24) & 0xffu));
}
