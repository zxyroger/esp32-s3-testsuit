/**
 * @file  ddns.c
 * @brief 板子自己上报 IPv6 地址到 DDNS，换地址也不用管
 *
 * 为什么必须在板子上做，而不是路由器做：
 *   IPv6 没有 NAT。花生壳那类路由器 DDNS 通常只写 A 记录（公网 IPv4），
 *   就算写 AAAA 也是写**路由器自己**的 WAN 地址，包到路由器就到头了，
 *   永远进不了 192.168.0.x 后面的板子。所以固定网址必须指向板子自己的
 *   2409:... 地址，那就只能由板子自己上报。
 *
 * 上报方式做成"URL 模板"而不是绑定某一家：
 *   模板里用 {host} / {token} / {ipv6} 三个占位符，拼成一条 GET 请求发出去。
 *   - dynv6：https://dynv6.com/api/update?hostname={host}&token={token}&ipv6={ipv6}
 *   - 花生壳：http://ddns.oray.com/ph/update?hostname={host}&myip={ipv6}
 *     （账号密码走 HTTP Basic，用 "ddns auth 用户名 密码" 配）
 *   - 其它 DynDNS2 兼容服务同理，把地址换成它家的 update 接口即可。
 *
 * 什么时候上报：
 *   开机联网后一次；之后每 60 s 比一次当前 IPv6，地址变了立刻补报；
 *   就算没变也每 30 分钟重报一次（防止对端记录过期被清）。
 */
#include <stdio.h>
#include <string.h>

#include "ddns.h"

#include "cam_web.h"
#include "wifi_lab.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ddns";

#define DDNS_NVS_NS        "ddns"
#define DDNS_DEFAULT_URL   "https://dynv6.com/api/update?hostname={host}&token={token}&ipv6={ipv6}"
#define DDNS_TMPL_MAX      200
#define DDNS_HOST_MAX      80
#define DDNS_SECRET_MAX    80
#define DDNS_POLL_MS       60000
#define DDNS_REFRESH_S     1800

/* 上报模式：
 *   MODE_MANUAL —— 用 NVS 里存的域名 + token，报给 dynv6 这类服务（地址永久、好记）
 *   MODE_AUTO   —— 不注册任何账号：把当前 IPv6 直接编进 sslip.io 的通配域名，
 *                  DNS 那边会解析回同一个地址，零配置、零账号，代价是名字长且
 *                  IPv6 一变名字就变（前缀变了才变，通常几天到几周一次）。
 * 两种模式共用同一条上报链路，auto 模式其实不需要发请求——域名是算出来的。 */
#define MODE_MANUAL 0
#define MODE_AUTO   1

typedef struct {
    bool  enabled;
    uint8_t mode;
    char  host[DDNS_HOST_MAX];
    char  token[DDNS_SECRET_MAX];
    char  user[DDNS_SECRET_MAX];
    char  pass[DDNS_SECRET_MAX];
    char  url[DDNS_TMPL_MAX];
    char  last_ip6[64];      /* 上次成功上报的地址，用来判断变没变 */
    int64_t last_ok_us;
    int   last_code;
} ddns_ctx_t;

static ddns_ctx_t s_ctx;
static bool s_started;
static bool s_task_up;
static char s_public_url[160];
static char s_body[160];      /* 服务端返回的正文，dynv6 会用一句话说明成功/失败 */
static char s_auto_host[DDNS_HOST_MAX];

/* ---------------- NVS ---------------- */

static void load_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t sz = len;
    if (nvs_get_str(h, key, out, &sz) != ESP_OK) {
        out[0] = '\0';
    }
}

static void ddns_load(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    nvs_handle_t h;
    if (nvs_open(DDNS_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t on = 0;
    if (nvs_get_u8(h, "on", &on) == ESP_OK) {
        s_ctx.enabled = (on != 0);
    }
    uint8_t mode = MODE_MANUAL;
    if (nvs_get_u8(h, "mode", &mode) == ESP_OK) {
        s_ctx.mode = mode;
    }
    load_str(h, "host",  s_ctx.host,  sizeof(s_ctx.host));
    load_str(h, "token", s_ctx.token, sizeof(s_ctx.token));
    load_str(h, "user",  s_ctx.user,  sizeof(s_ctx.user));
    load_str(h, "pass",  s_ctx.pass,  sizeof(s_ctx.pass));
    load_str(h, "url",   s_ctx.url,   sizeof(s_ctx.url));
    load_str(h, "last",  s_ctx.last_ip6, sizeof(s_ctx.last_ip6));
    nvs_close(h);
    if (s_ctx.url[0] == '\0') {
        snprintf(s_ctx.url, sizeof(s_ctx.url), "%s", DDNS_DEFAULT_URL);
    }
}

static esp_err_t ddns_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DDNS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_u8(h, "on", s_ctx.enabled ? 1 : 0);
    nvs_set_u8(h, "mode", s_ctx.mode);
    nvs_set_str(h, "host",  s_ctx.host);
    nvs_set_str(h, "token", s_ctx.token);
    nvs_set_str(h, "user",  s_ctx.user);
    nvs_set_str(h, "pass",  s_ctx.pass);
    nvs_set_str(h, "url",   s_ctx.url);
    nvs_set_str(h, "last",  s_ctx.last_ip6);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---------------- 免注册域名（sslip.io） ----------------
 * sslip.io 是一个通配 DNS：域名里写成 IP，它就把域名解析回这个 IP。
 * 写法是把 IPv6 的冒号换成短横线，例如
 *   2409:8a28:a61:f9c4:46b1:76ff:fea4:1288
 *   -> 2409-8a28-a61-f9c4-46b1-76ff-fea4-1288.sslip.io
 * 好处：不用注册、不用 token、不用任何服务器；坏处：地址一变名字就跟着变。
 * 拿它做"先通了再谈域名"的第一步很合适。 */
static const char *auto_host_from_ip6(const char *ip6)
{
    size_t o = 0;
    s_auto_host[0] = '\0';
    for (size_t i = 0; ip6[i] != '\0' && o + 2 < sizeof(s_auto_host); i++) {
        s_auto_host[o++] = (ip6[i] == ':') ? '-' : ip6[i];
    }
    s_auto_host[o] = '\0';
    strncat(s_auto_host, ".sslip.io", sizeof(s_auto_host) - strlen(s_auto_host) - 1);
    return s_auto_host;
}

/* ---------------- URL 拼装 ---------------- */

/* 把模板里的 {host} / {token} / {user} / {pass} / {ipv6} 替换掉。
 * 只做一次扫描，替换结果里不会再被二次展开，避免 token 里含 "{" 时出怪异行为。 */
static void build_url(char *out, size_t len, const char *ip6)
{
    out[0] = '\0';
    size_t o = 0;
    for (size_t i = 0; i < strlen(s_ctx.url) && o + 1 < len; ) {
        struct { const char *k; const char *v; } map[] = {
            { "{host}",  s_ctx.host  },
            { "{token}", s_ctx.token },
            { "{user}",  s_ctx.user  },
            { "{pass}",  s_ctx.pass  },
            { "{ipv6}",  ip6         },
        };
        bool hit = false;
        for (size_t m = 0; m < sizeof(map) / sizeof(map[0]); m++) {
            size_t kl = strlen(map[m].k);
            if (strncmp(s_ctx.url + i, map[m].k, kl) == 0) {
                size_t vl = strlen(map[m].v);
                if (o + vl >= len) {
                    vl = len - o - 1;
                }
                memcpy(out + o, map[m].v, vl);
                o += vl;
                i += kl;
                hit = true;
                break;
            }
        }
        if (!hit) {
            out[o++] = s_ctx.url[i++];
        }
    }
    out[o] = '\0';
}

/* ---------------- 上报 ---------------- */

static esp_err_t ddns_http_get(const char *url, int *code_out)
{
    s_body[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,      /* dynv6 从国内访问要 3~4 s，10 s 太紧 */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_GET,
        .buffer_size = 512,
    };
    if (s_ctx.user[0] != '\0') {
        cfg.username = s_ctx.user;
        cfg.password = s_ctx.pass;
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
    }

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 不用 perform()：需要把返回正文读出来。
     * dynv6 成功回 "addresses updated"，失败回 "invalid token" / "no such host"，
     * 光看 HTTP 码分不清是 token 错还是域名没建。 */
    esp_err_t err = esp_http_client_open(cli, 0);
    int code = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        code = esp_http_client_get_status_code(cli);
        int n = esp_http_client_read(cli, s_body, sizeof(s_body) - 1);
        s_body[n > 0 ? n : 0] = '\0';
        /* 去掉尾部换行，串口看起来干净 */
        for (int i = (int)strlen(s_body) - 1; i >= 0 && (s_body[i] == '\n' || s_body[i] == '\r'); i--) {
            s_body[i] = '\0';
        }
        esp_http_client_close(cli);
    }
    if (code_out) {
        *code_out = code;
    }
    if (err == ESP_OK && code >= 400) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "上报失败: %s (HTTP %d) %s",
                 esp_err_to_name(err), code, s_body[0] ? s_body : "");
    }
    esp_http_client_cleanup(cli);
    return err;
}

esp_err_t ddns_update_now(void)
{
    wifi_lab_t *lab = NULL;
    if (wifi_lab_init(&lab) != ESP_OK || lab == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char ip6[64];
    if (wifi_lab_sta_ip6(lab, ip6, sizeof(ip6)) == NULL) {
        ESP_LOGW(TAG, "还没拿到全局 IPv6，无法上报");
        return ESP_ERR_NOT_FOUND;
    }

    /* auto 模式：域名是算出来的，没有对端要通知，算完即完成 */
    if (s_ctx.mode == MODE_AUTO) {
        auto_host_from_ip6(ip6);
        snprintf(s_ctx.host, sizeof(s_ctx.host), "%s", s_auto_host);
        snprintf(s_ctx.last_ip6, sizeof(s_ctx.last_ip6), "%s", ip6);
        s_ctx.last_ok_us = esp_timer_get_time();
        s_ctx.last_code = 200;
        (void)ddns_save();
        ESP_LOGI(TAG, "自动域名已就绪: %s", s_auto_host);
        return ESP_OK;
    }

    char url[DDNS_TMPL_MAX + 128];
    build_url(url, sizeof(url), ip6);

    ESP_LOGI(TAG, "上报 %s -> %s", ip6, s_ctx.host[0] ? s_ctx.host : "(模板)");
    int code = 0;
    esp_err_t err = ddns_http_get(url, &code);
    s_ctx.last_code = code;
    if (err == ESP_OK) {
        snprintf(s_ctx.last_ip6, sizeof(s_ctx.last_ip6), "%s", ip6);
        s_ctx.last_ok_us = esp_timer_get_time();
        (void)ddns_save();
        ESP_LOGI(TAG, "上报成功 (HTTP %d) %s，固定网址: %s",
                 code, s_body[0] ? s_body : "", ddns_public_url());
    }
    return err;
}

/* ---------------- 对外接口 ---------------- */

const char *ddns_public_url(void)
{
    if (s_ctx.host[0] == '\0' && s_ctx.mode == MODE_AUTO) {
        /* 还没跑过上报就直接问网址的场景（比如刚开机敲 ddns） */
        wifi_lab_t *lab = NULL;
        char ip6[64];
        if (wifi_lab_init(&lab) == ESP_OK && lab != NULL &&
            wifi_lab_sta_ip6(lab, ip6, sizeof(ip6)) != NULL) {
            snprintf(s_ctx.host, sizeof(s_ctx.host), "%s", auto_host_from_ip6(ip6));
        }
    }
    if (s_ctx.host[0] == '\0') {
        return NULL;
    }
    /* 外网走备用端口：家用宽带入站 80 基本被封，18080 能过 */
    snprintf(s_public_url, sizeof(s_public_url), "http://%s:%d/",
             s_ctx.host, cam_web_alt_port());
    return s_public_url;
}

static void ddns_task(void *arg)
{
    (void)arg;
    int64_t last_check_us = 0;
    for (;;) {
        if (s_ctx.enabled) {
            wifi_lab_t *lab = NULL;
            char ip6[64] = {0};
            bool have = (wifi_lab_init(&lab) == ESP_OK && lab != NULL &&
                         wifi_lab_sta_ip6(lab, ip6, sizeof(ip6)) != NULL);
            if (have) {
                bool changed = (strcmp(ip6, s_ctx.last_ip6) != 0);
                bool stale = (esp_timer_get_time() - s_ctx.last_ok_us) > (int64_t)DDNS_REFRESH_S * 1000000;
                if (changed || stale) {
                    (void)ddns_update_now();
                }
            }
            last_check_us = esp_timer_get_time();
            (void)last_check_us;
        }
        vTaskDelay(pdMS_TO_TICKS(DDNS_POLL_MS));
    }
}

esp_err_t ddns_init(void)
{
    if (s_started) {
        return ESP_OK;
    }
    ddns_load();
    s_started = true;

    if (s_ctx.host[0] != '\0') {
        ESP_LOGI(TAG, "DDNS 配置: %s（%s）", s_ctx.host,
                 s_ctx.enabled ? "开启" : "关闭");
    }
    if (s_ctx.enabled) {
        xTaskCreate(ddns_task, "ddns", 4096, NULL, 3, NULL);
        s_task_up = true;
    }
    return ESP_OK;
}

esp_err_t ddns_set_host(const char *host, const char *token)
{
    if (host == NULL || host[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_ctx.host, sizeof(s_ctx.host), "%s", host);
    if (token != NULL) {
        snprintf(s_ctx.token, sizeof(s_ctx.token), "%s", token);
    }
    if (s_ctx.url[0] == '\0') {
        snprintf(s_ctx.url, sizeof(s_ctx.url), "%s", DDNS_DEFAULT_URL);
    }
    s_ctx.mode = MODE_MANUAL;   /* 一旦给了真域名，就不再走 sslip.io */
    s_ctx.enabled = true;
    s_ctx.last_ip6[0] = '\0';   /* 强制下一次必定上报 */
    return ddns_save();
}

/* 切换到免注册模式：域名由当前 IPv6 算出来（sslip.io），一条命令就能用 */
esp_err_t ddns_set_auto(bool on)
{
    if (on) {
        s_ctx.mode = MODE_AUTO;
        s_ctx.enabled = true;
        s_ctx.last_ip6[0] = '\0';      /* 强制立刻算一次 */
        esp_err_t err = ddns_save();
        if (s_started && !s_task_up) {
            xTaskCreate(ddns_task, "ddns", 4096, NULL, 3, NULL);
            s_task_up = true;
        }
        (void)ddns_update_now();
        return err;
    }
    s_ctx.mode = MODE_MANUAL;
    return ddns_save();
}

esp_err_t ddns_set_url(const char *tmpl)
{
    if (tmpl == NULL || tmpl[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_ctx.url, sizeof(s_ctx.url), "%s", tmpl);
    return ddns_save();
}

esp_err_t ddns_set_auth(const char *user, const char *pass)
{
    snprintf(s_ctx.user, sizeof(s_ctx.user), "%s", user ? user : "");
    snprintf(s_ctx.pass, sizeof(s_ctx.pass), "%s", pass ? pass : "");
    return ddns_save();
}

esp_err_t ddns_set_enabled(bool on)
{
    s_ctx.enabled = on;
    if (on && s_started && !s_task_up) {
        /* 之前是关闭状态，任务没起过 */
        xTaskCreate(ddns_task, "ddns", 4096, NULL, 3, NULL);
        s_task_up = true;
    }
    return ddns_save();
}

void ddns_dump(void)
{
    printf("[DDNS] 状态   : %s\n", s_ctx.enabled ? "开启" : "关闭");
    printf("[DDNS] 模式   : %s\n", s_ctx.mode == MODE_AUTO ? "自动（sslip.io，免注册）" : "手动（域名 + token）");
    printf("[DDNS] 域名   : %s\n", s_ctx.host[0] ? s_ctx.host : "(未设置)");
    if (s_ctx.mode != MODE_AUTO) {
        printf("[DDNS] 模板   : %s\n", s_ctx.url);
        printf("[DDNS] 鉴权   : %s\n", s_ctx.user[0] ? s_ctx.user : "(无)");
    }
    printf("[DDNS] 已上报 : %s\n", s_ctx.last_ip6[0] ? s_ctx.last_ip6 : "(还没成功过)");
    if (s_ctx.last_code) {
        printf("[DDNS] 上次码 : HTTP %d %s\n", s_ctx.last_code, s_body[0] ? s_body : "");
    }
    const char *u = ddns_public_url();
    if (u) {
        printf("[DDNS] 网址   : %s\n", u);
    }
}
