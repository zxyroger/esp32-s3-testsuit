/**
 * @file  cam_mqtt.c
 * @brief 板子主动连公共 MQTT broker，把 JPEG 帧推上去（MQTT 3.1.1 手写实现）
 *
 * 为什么不用 esp-mqtt 组件：ESP-IDF 6.0 里 mqtt 已经拆成托管组件，要额外
 * 拉依赖、带一堆用不上的订阅/outbox 逻辑，还占内存。这里只需要"连上 +
 * 发 PUBLISH + 发心跳"三件事，手写不到 200 行，反而更可控。
 *
 * 链路：板子 --TLS:8883--> broker.emqx.io <--WSS:8084-- 手机网页
 * 全程板子只做**出站**连接，不需要公网 IP、不需要端口映射、不需要路由器放行。
 */
#include <stdio.h>
#include <string.h>

#include "cam_mqtt.h"

#include "cam_jpeg.h"

#include "sec_crypto.h"
#include "lab_secrets.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "cam_mqtt";

#define MQTT_NVS_NS       "mqtt"
#define DEF_HOST          "broker.emqx.io"
#define DEF_PORT          8883
#define DEF_TOPIC         SECRET_MQTT_PREFIX "/jpg"
#define DEF_RATE          2          /* 帧/秒，公共 broker 别贪 */
#define KEEPALIVE_S       60
#define META_EVERY_S      10

typedef struct {
    bool     enabled;
    char     host[64];
    uint16_t port;
    char     topic[96];
    int      rate;
} mqtt_cfg_t;

static mqtt_cfg_t s_cfg;
static esp_tls_t *s_tls;
static bool  s_up;
static bool  s_started;
static char  s_meta_topic[100];
static uint8_t *s_buf;          /* 取帧用的 PSRAM 缓冲 */
static size_t  s_buf_cap;
static uint8_t *s_enc;          /* 加密输出缓冲（比 s_buf 多 SEC_OVERHEAD） */
static size_t  s_enc_cap;
static uint32_t s_sent;         /* 累计发出的帧 */
static uint32_t s_fail;         /* 累计失败次数 */
/* TLS 连接是多条任务共用的（推流 + 电池采样），PUBLISH 必须串行写，
 * 否则两个任务的固定头和载荷会交错，broker 直接判定协议错并断开。 */
static SemaphoreHandle_t s_tx_lock;

/* ---------------- 链路健康（health.c 用） ----------------
 * 只记录"最后一次真正写出去的时刻"。写成功 ≠ broker 收到，但 socket
 * 已经断掉时 esp_tls_conn_write 会立刻返回错误，所以它是目前最便宜、
 * 又不引入额外心跳包判据。用 uint32 毫秒并存，32 位读写天然原子。 */
static volatile uint32_t s_last_ok_ms;   /* 0 = 本次开机还没成功发出过 */
static volatile bool     s_ever_ok;      /* 本次开机是否曾经发出去过 */
static volatile bool     s_force_rc;     /* 外部要求重连（看门狗置位） */

/* ---------------- NVS ---------------- */

static void cfg_load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", DEF_HOST);
    snprintf(s_cfg.topic, sizeof(s_cfg.topic), "%s", DEF_TOPIC);
    s_cfg.port = DEF_PORT;
    s_cfg.rate = DEF_RATE;

    nvs_handle_t h;
    if (nvs_open(MQTT_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t on = 0;
    if (nvs_get_u8(h, "on", &on) == ESP_OK) {
        s_cfg.enabled = (on != 0);
    }
    size_t sz = sizeof(s_cfg.host);
    nvs_get_str(h, "host", s_cfg.host, &sz);
    sz = sizeof(s_cfg.topic);
    nvs_get_str(h, "topic", s_cfg.topic, &sz);
    uint16_t p = 0;
    if (nvs_get_u16(h, "port", &p) == ESP_OK && p) {
        s_cfg.port = p;
    }
    uint8_t r = 0;
    if (nvs_get_u8(h, "rate", &r) == ESP_OK && r) {
        s_cfg.rate = r;
    }
    nvs_close(h);
}

static esp_err_t cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MQTT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_u8(h, "on", s_cfg.enabled ? 1 : 0);
    nvs_set_str(h, "host", s_cfg.host);
    nvs_set_str(h, "topic", s_cfg.topic);
    nvs_set_u16(h, "port", s_cfg.port);
    nvs_set_u8(h, "rate", (uint8_t)s_cfg.rate);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---------------- MQTT 报文 ---------------- */

/* 剩余长度是 7 bit 一组的变长整数，最长 4 字节 */
static int enc_remlen(uint8_t *p, int len)
{
    int n = 0;
    do {
        uint8_t b = len % 128;
        len /= 128;
        if (len > 0) {
            b |= 0x80;
        }
        p[n++] = b;
    } while (len > 0);
    return n;
}

static int tls_write_all(const void *data, int len)
{
    const uint8_t *p = (const uint8_t *)data;
    int left = len;
    while (left > 0) {
        int w = esp_tls_conn_write(s_tls, p, left);
        if (w <= 0) {
            return -1;
        }
        p += w;
        left -= w;
    }
    return 0;
}

static void mqtt_close(void)
{
    if (s_tls) {
        esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
    }
    s_up = false;
}

/* 记一次"链路是活的"。写成功只是必要条件（broker 不一定收到），
 * 但连接断了以后 write 一定会失败，所以够用作看门狗判据。 */
static void mark_ok(void)
{
    s_last_ok_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_ever_ok = true;
}

/* 建 TLS 连接 + 发 CONNECT + 等 CONNACK */
static bool mqtt_connect(void)
{
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    s_tls = esp_tls_init();
    if (s_tls == NULL) {
        return false;
    }
    int ret = esp_tls_conn_new_sync(s_cfg.host, strlen(s_cfg.host), s_cfg.port, &cfg, s_tls);
    if (ret != 1) {
        ESP_LOGW(TAG, "TLS 连接 %s:%d 失败", s_cfg.host, s_cfg.port);
        mqtt_close();
        return false;
    }

    /* CONNECT：协议名 MQTT、级别 4、CleanSession、keepalive 60 */
    char cid[40];
    snprintf(cid, sizeof(cid), "esp32s3cam-%02x%02x", (unsigned)(esp_random() & 0xFF),
             (unsigned)(esp_random() & 0xFF));
    uint8_t pkt[128];
    int n = 0;
    pkt[n++] = 0x10;                          /* CONNECT */
    int cidlen = (int)strlen(cid);
    int rem = 10 + 2 + cidlen;
    n += enc_remlen(&pkt[n], rem);
    pkt[n++] = 0x00; pkt[n++] = 0x04;         /* 协议名长度 */
    memcpy(&pkt[n], "MQTT", 4); n += 4;
    pkt[n++] = 0x04;                          /* 协议级别 4 = 3.1.1 */
    pkt[n++] = 0x02;                          /* CleanSession */
    pkt[n++] = 0x00; pkt[n++] = KEEPALIVE_S;
    pkt[n++] = (uint8_t)(cidlen >> 8);
    pkt[n++] = (uint8_t)(cidlen & 0xFF);
    memcpy(&pkt[n], cid, cidlen); n += cidlen;

    if (tls_write_all(pkt, n) != 0) {
        mqtt_close();
        return false;
    }

    /* CONNACK 固定 4 字节：0x20 0x02 标志 返回码 */
    uint8_t ack[4] = {0};
    int got = 0;
    for (int i = 0; i < 4; i++) {
        int r = esp_tls_conn_read(s_tls, &ack[got], 1);
        if (r <= 0) {
            break;
        }
        got += r;
    }
    if (got < 4 || ack[0] != 0x20 || ack[3] != 0x00) {
        ESP_LOGW(TAG, "CONNACK 异常 (%d 字节, code %d)", got, got >= 4 ? ack[3] : -1);
        mqtt_close();
        return false;
    }
    s_up = true;
    mark_ok();
    ESP_LOGI(TAG, "已连上 %s:%d，topic %s", s_cfg.host, s_cfg.port, s_cfg.topic);
    return true;
}

/* QoS0 PUBLISH：固定头 + topic + payload，分两次写，不复制整帧。
 * 调用方负责持有 s_tx_lock（见 mqtt_publish 包装）。 */
static bool mqtt_publish_raw_ex(const char *topic, const uint8_t *data, int len,
                                bool retain)
{
    if (!s_up || s_tls == NULL) {
        return false;
    }
    int tlen = (int)strlen(topic);
    int rem = 2 + tlen + len;
    /* 头部 = 1 字节类型 + 最长 4 字节剩余长度 + 2 字节 topic 长度 + topic，
     * 之前只给了 8 字节，topic 一长就越界写栈 —— 表现为推一帧就断连。 */
    uint8_t hdr[160];
    int n = 0;
    /* bit0 = RETAIN：给"状态型"消息用（自检结果），
     * 后开页面的客户端一订阅就能立刻拿到最后一份，不用等板子重发。 */
    hdr[n++] = (uint8_t)(0x30 | (retain ? 0x01 : 0x00));   /* PUBLISH, QoS0 */
    n += enc_remlen(&hdr[n], rem);
    hdr[n++] = (uint8_t)(tlen >> 8);
    hdr[n++] = (uint8_t)(tlen & 0xFF);
    memcpy(&hdr[n], topic, tlen);
    n += tlen;

    if (tls_write_all(hdr, n) != 0) {
        ESP_LOGW(TAG, "写 PUBLISH 头失败 (len=%d)", len);
        mqtt_close();
        return false;
    }
    if (len > 0 && tls_write_all(data, len) != 0) {
        ESP_LOGW(TAG, "写 PUBLISH 载荷失败 (len=%d)", len);
        mqtt_close();
        return false;
    }
    mark_ok();
    return true;
}

/* 不带 RETAIN 的普通发布（帧、电池、meta、命令回传都走这条） */
static bool mqtt_publish_raw(const char *topic, const uint8_t *data, int len)
{
    return mqtt_publish_raw_ex(topic, data, len, false);
}

/* 文本类消息（meta / 电池 JSON / 命令回传）的加密缓冲。
 * 放在静态区而不是任务栈上：batt_log 任务只有 3 KB 栈，之前在这里放
 * 1.2 KB 的数组直接把栈打爆（实测 "stack overflow in task batt_log" 重启循环）。
 * 由 s_tx_lock 保护，同一时刻只有一个任务用它。 */
static uint8_t s_small_enc[1280];

/* 大文本（自检结果 JSON，约 2~3 KB）用的加密缓冲，放 PSRAM。
 * 只有"偶尔发一次"的消息走这里；帧和电池仍走上面那块小缓冲，
 * 避免为了自检结果把常驻的小缓冲放大到几 KB。 */
static uint8_t *s_big_enc;
static size_t   s_big_cap;

/* 加密 + 发布一步到位，调用方不需要准备缓冲。
 * 小消息用 s_small_enc（命令回传按 900 字节分块，够用）；
 * 超过小缓冲容量时改走 s_big_enc，仍放不下才丢弃。 */
static bool mqtt_publish_secure_ex(const char *topic, const uint8_t *data,
                                   int len, bool retain)
{
    if (!s_up || s_tls == NULL) {
        return false;
    }
    uint8_t *out = s_small_enc;
    size_t cap = sizeof(s_small_enc);
    if (len < 0) {
        return false;
    }
    if ((size_t)len + SEC_OVERHEAD > cap) {
        if (s_big_enc == NULL || (size_t)len + SEC_OVERHEAD > s_big_cap) {
            ESP_LOGW(TAG, "%s 超过加密缓冲（%d 字节），丢弃", topic, len);
            return false;
        }
        out = s_big_enc;
        cap = s_big_cap;
    }
    bool locked = false;
    if (s_tx_lock != NULL) {
        locked = (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2000)) == pdTRUE);
        if (!locked) {
            ESP_LOGW(TAG, "发送互斥超时，丢弃一条 %s", topic);
            return false;
        }
    }
    size_t n = sec_seal(topic, data, (size_t)len, out, cap);
    bool ok = (n > 0) && mqtt_publish_raw_ex(topic, out, (int)n, retain);
    if (locked) {
        xSemaphoreGive(s_tx_lock);
    }
    return ok;
}

/* 普通的加密发布（不保留） */
static bool mqtt_publish_secure(const char *topic, const uint8_t *data, int len)
{
    return mqtt_publish_secure_ex(topic, data, len, false);
}

/* 所有发布都从这里进：拿不到锁就放弃这一条（QoS0 本来就是可丢的），
 * 绝不阻塞推流任务。电池 JSON 每条 200 字节，实际几乎不会等。 */
static bool mqtt_publish(const char *topic, const uint8_t *data, int len)
{
    if (!s_up || s_tls == NULL) {
        return false;
    }
    bool locked = false;
    if (s_tx_lock != NULL) {
        locked = (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2000)) == pdTRUE);
        if (!locked) {
            ESP_LOGW(TAG, "发送互斥超时，丢弃一条 %s", topic);
            return false;
        }
    }
    bool ok = mqtt_publish_raw(topic, data, len);
    if (locked) {
        xSemaphoreGive(s_tx_lock);
    }
    return ok;
}

static void mqtt_ping(void)
{
    uint8_t p[2] = {0xC0, 0x00};
    if (s_up && tls_write_all(p, 2) != 0) {
        mqtt_close();
        return;
    }
    if (s_up) {
        mark_ok();
    }
}

/* ---------------- 推流任务 ---------------- */

static void publish_meta(void)
{
    cam_jpeg_stat_t st;
    cam_jpeg_get_stat(&st);
    char js[160];
    int n = snprintf(js, sizeof(js),
                     "{\"w\":%u,\"h\":%u,\"fps\":%u,\"kb\":%u,\"seq\":%u,\"sent\":%u,\"up\":1}",
                     (unsigned)st.width, (unsigned)st.height, (unsigned)st.avg_fps,
                     (unsigned)(st.last_len / 1024), (unsigned)st.seq, (unsigned)s_sent);
    if (s_meta_topic[0] == '\0') {
        snprintf(s_meta_topic, sizeof(s_meta_topic), "%s", s_cfg.topic);
        char *slash = strrchr(s_meta_topic, '/');
        if (slash) {
            snprintf(slash, sizeof(s_meta_topic) - (slash - s_meta_topic), "/meta");
        }
    }
    /* 元数据也是密文：帧率/累计帧数同样属于可被利用的侧信道 */
    (void)mqtt_publish_secure(s_meta_topic, (const uint8_t *)js, n);
}

static void mqtt_task(void *arg)
{
    (void)arg;
    int64_t last_meta = 0;
    int64_t last_ping = 0;

    for (;;) {
        int period_ms = 1000 / (s_cfg.rate > 0 ? s_cfg.rate : 1);
        int64_t iter_t0 = esp_timer_get_time();
        /* 看门狗发现链路假死时置位：这里主动拆掉重连。
         * 不直接让外部调 mqtt_close()，避免和正在写的本任务抢 s_tls。 */
        if (s_force_rc) {
            s_force_rc = false;
            ESP_LOGW(TAG, "看门狗要求重连");
            mqtt_close();
        }
        if (!s_cfg.enabled) {
            if (s_up) {
                mqtt_close();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!s_up && !mqtt_connect()) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        if (s_buf == NULL) {
            s_buf_cap = cam_jpeg_frame_cap();
            s_buf = heap_caps_malloc(s_buf_cap, MALLOC_CAP_SPIRAM);
            if (s_buf == NULL) {
                ESP_LOGE(TAG, "PSRAM 缓冲分配失败");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            s_enc_cap = s_buf_cap + SEC_OVERHEAD + 16;
            s_enc = heap_caps_malloc(s_enc_cap, MALLOC_CAP_SPIRAM);
            if (s_enc == NULL) {
                ESP_LOGE(TAG, "加密缓冲分配失败");
                heap_caps_free(s_buf);
                s_buf = NULL;
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }

        size_t len = 0;
        uint32_t seq = cam_jpeg_snapshot(s_buf, s_buf_cap, &len);
        if (seq != 0 && len > 0) {
            int64_t t0 = esp_timer_get_time();
            size_t elen = sec_seal(s_cfg.topic, s_buf, len, s_enc, s_enc_cap);
            bool ok = (elen > 0) && mqtt_publish(s_cfg.topic, s_enc, (int)elen);
            int ms = (int)((esp_timer_get_time() - t0) / 1000);
            if (ok) {
                s_sent++;
                /* 帧变小了网速却上不去时，这条日志能直接指出瓶颈在发送 */
                if (ms > 800) {
                    ESP_LOGW(TAG, "发布慢: %d ms / %d KB", ms, (int)(len / 1024));
                }
            } else {
                s_fail++;
            }
        }

        int64_t now = esp_timer_get_time() / 1000000;
        if (now - last_meta >= META_EVERY_S) {
            last_meta = now;
            publish_meta();
        }
        if (now - last_ping >= 30) {
            last_ping = now;
            mqtt_ping();
        }
        /* 扣掉这一轮真正花掉的时间再睡，否则"发一帧 + 睡 500 ms"会把帧率
         * 压到 1 fps 以下（发 10 KB 到公网就要 300~400 ms）。 */
        int used_ms = (int)((esp_timer_get_time() - iter_t0) / 1000);
        int wait_ms = period_ms - used_ms;
        vTaskDelay(pdMS_TO_TICKS(wait_ms > 10 ? wait_ms : 10));
    }
}

/* ---------------- 对外接口 ---------------- */

const char *cam_mqtt_topic(void)
{
    return s_cfg.topic;
}

const char *cam_mqtt_host(void)
{
    return s_cfg.host;
}

uint16_t cam_mqtt_port(void)
{
    return s_cfg.port;
}

static esp_err_t publish_text_ex(const char *sub, const char *text, bool retain)
{
    if (sub == NULL || text == NULL || sub[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || !s_up) {
        /* 还没连上就当这条不存在：电池是周期采样的，等下一轮即可，
         * 缓存历史需要额外内存和时序假设，不值得。 */
        return ESP_ERR_INVALID_STATE;
    }

    /* 与 /meta 同样的做法：把主 topic 的最后一段换成 sub，
     * 这样改 mqtt topic 时页面上的所有通道会一起跟着走。 */
    char topic[128];
    const char *slash = strrchr(s_cfg.topic, '/');
    int prefix = slash ? (int)(slash - s_cfg.topic) : (int)strlen(s_cfg.topic);
    if (prefix <= 0 || prefix > (int)sizeof(topic) - 32) {
        prefix = (int)strlen(s_cfg.topic);
        if (prefix > (int)sizeof(topic) - 32) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    int n = snprintf(topic, sizeof(topic), "%.*s/%s", prefix, s_cfg.topic, sub);
    if (n <= 0 || n >= (int)sizeof(topic)) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* 电池 JSON、命令回传都是密文（缓冲在静态区，见 mqtt_publish_secure） */
    return mqtt_publish_secure_ex(topic, (const uint8_t *)text,
                                  (int)strlen(text), retain)
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t cam_mqtt_publish_text(const char *sub, const char *text)
{
    return publish_text_ex(sub, text, false);
}

esp_err_t cam_mqtt_publish_text_retain(const char *sub, const char *text)
{
    return publish_text_ex(sub, text, true);
}

bool cam_mqtt_enabled(void)
{
    return s_cfg.enabled;
}

uint32_t cam_mqtt_idle_ms(void)
{
    uint32_t last = s_last_ok_ms;
    if (last == 0) {
        /* 本次开机还没成功发出去过。返回最大值而不是 0：
         * 0 会让看门狗以为"刚刚才成功"，把真正的失败藏起来。 */
        return 0xFFFFFFFFu;
    }
    return (uint32_t)(esp_timer_get_time() / 1000) - last;
}

bool cam_mqtt_ever_ok(void)
{
    return s_ever_ok;
}

void cam_mqtt_reconnect(void)
{
    s_force_rc = true;
}

bool cam_mqtt_up(void)
{
    return s_started && s_up;
}

esp_err_t cam_mqtt_init(void)
{
    if (s_started) {
        return ESP_OK;
    }
    cfg_load();
    sec_init();
    s_started = true;
    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutex();
        if (s_tx_lock == NULL) {
            ESP_LOGE(TAG, "发送互斥量创建失败");
            s_started = false;
            return ESP_ERR_NO_MEM;
        }
    }
    /* 自检结果 JSON 是一次性的大文本，单独给一块 PSRAM 缓冲；
     * 分不到也不致命 —— 大文本会被丢弃，帧和电池仍走小缓冲。 */
    if (s_big_enc == NULL) {
        s_big_cap = 8 * 1024;
        s_big_enc = heap_caps_malloc(s_big_cap, MALLOC_CAP_SPIRAM);
        if (s_big_enc == NULL) {
            s_big_cap = 0;
            ESP_LOGW(TAG, "PSRAM 大文本缓冲分配失败：超过 1.2 KB 的消息将无法发送");
        }
    }
    ESP_LOGI(TAG, "MQTT 配置: %s:%d topic=%s rate=%d fps（%s）",
             s_cfg.host, s_cfg.port, s_cfg.topic, s_cfg.rate,
             s_cfg.enabled ? "开启" : "关闭");
    xTaskCreate(mqtt_task, "cam_mqtt", 6144, NULL, 3, NULL);
    return ESP_OK;
}

esp_err_t cam_mqtt_set_enabled(bool on)
{
    s_cfg.enabled = on;
    return cfg_save();
}

esp_err_t cam_mqtt_set_topic(const char *topic)
{
    if (topic == NULL || topic[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_cfg.topic, sizeof(s_cfg.topic), "%s", topic);
    s_meta_topic[0] = '\0';      /* 下次重新算 meta 主题 */
    return cfg_save();
}

esp_err_t cam_mqtt_set_rate(int fps)
{
    if (fps < 1) {
        fps = 1;
    }
    if (fps > 10) {
        fps = 10;
    }
    s_cfg.rate = fps;
    return cfg_save();
}

void cam_mqtt_dump(void)
{
    printf("[MQTT] 状态   : %s（%s）\n", s_cfg.enabled ? "开启" : "关闭",
           s_up ? "已连上" : "未连接");
    printf("[MQTT] 服务器 : %s:%u\n", s_cfg.host, (unsigned)s_cfg.port);
    printf("[MQTT] 主题   : %s\n", s_cfg.topic);
    printf("[MQTT] 帧率   : %d fps\n", s_cfg.rate);
    printf("[MQTT] 已发送 : %u 帧，失败 %u 次\n", (unsigned)s_sent, (unsigned)s_fail);
    printf("[MQTT] 加密   : %s（AES-256-GCM，每包 +%d 字节）\n",
           sec_ready() ? "已启用" : "未启用", SEC_OVERHEAD);
}
