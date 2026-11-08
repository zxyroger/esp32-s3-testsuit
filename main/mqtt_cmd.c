/**
 * @file  mqtt_cmd.c
 * @brief 独立的 MQTT 命令通道：订阅 <前缀>/cmd → 本机执行 → 结果发回 <前缀>/serial/tx
 *
 * 报文只实现 MQTT 3.1.1 里需要的几个：CONNECT / SUBSCRIBE / PINGREQ，
 * 下行只认 PUBLISH(QoS0) 和 PINGRESP，其余类型读出剩余长度直接丢弃。
 */
#include "mqtt_cmd.h"

#include <stdio.h>
#include <string.h>

#include "cam_mqtt.h"
#include "mqtt_console.h"
#include "sec_crypto.h"
#include "tf.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mqtt_cmd";

#define CMD_SUFFIX     "cmd"          /* 订阅：网页发命令到这里 */
#define CHAL_SUFFIX    "chal"         /* 发布：防重放挑战码（每 20 秒换一次） */
#define OUT_SUFFIX     "serial/tx"    /* 发布：与网页串口页原来的显示通道一致 */
#define KEEPALIVE_S    60
#define RX_TIMEOUT_MS  3000           /* 读超时，用来定期醒来发心跳 */
#define PING_EVERY_S   20
#define CHAL_EVERY_S   8            /* 挑战码轮换间隔：页面几乎总能拿到最新的 */
#define CMDLINE_MAX       160            /* 一条命令最长接受多少字符 */
#define OUT_CHUNK      900            /* 单条 PUBLISH 的回传上限 */
#define PKT_MAX        1200

static esp_tls_t *s_tls;
static bool       s_started;
static uint32_t   s_runs;
static uint32_t   s_reject;       /* 密文校验/挑战码被拒的次数 */

/* 防重放：板子每 20 秒发一个新挑战码，命令必须带上当前或上一个。
 * 这样截获的旧报文最多在 40 秒内还能用一次，之后自动失效。 */
static char s_chal[17];
static char s_prev_chal[17];

/* ---------------- 报文辅助 ---------------- */

static int enc_remlen(uint8_t *p, int len)
{
    int n = 0;
    do {
        uint8_t b = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0) {
            b |= 0x80;
        }
        p[n++] = b;
    } while (len > 0);
    return n;
}

static bool tls_write_all(const uint8_t *data, int len)
{
    const uint8_t *p = data;
    int left = len;
    while (left > 0) {
        int w = esp_tls_conn_write(s_tls, p, left);
        if (w <= 0) {
            return false;
        }
        p += w;
        left -= w;
    }
    return true;
}

/* 返回 0 读满、1 一个字节都没读到就超时、-1 出错/被关。
 * 注意：esp_tls 在读超时时不一定返回 ESP_TLS_ERR_SSL_TIMEOUT——socket 层面
 * 的超时会被 mbedtls 折成 WANT_READ 交上来。三种都当"暂时没数据"，否则
 * 命令通道会每 3 秒自己断一次（实测踩过）。 */
static int read_n(uint8_t *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = esp_tls_conn_read(s_tls, buf + got, n - got);
        if (r == 0) {
            return -1;
        }
        if (r < 0) {
            if (r == ESP_TLS_ERR_SSL_TIMEOUT || r == ESP_TLS_ERR_SSL_WANT_READ ||
                r == ESP_TLS_ERR_SSL_WANT_WRITE) {
                return got == 0 ? 1 : -1;
            }
            ESP_LOGW(TAG, "读失败 code=%d", r);
            return -1;
        }
        got += r;
    }
    return 0;
}

static int read_remlen(int *out)
{
    int mult = 1;
    int val = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = 0;
        if (read_n(&b, 1) != 0) {
            return -1;
        }
        val += (b & 0x7F) * mult;
        if ((b & 0x80) == 0) {
            *out = val;
            return 0;
        }
        mult *= 128;
    }
    return -1;
}

/* ---------------- 主题 ---------------- */

static void topic_of(char *out, size_t cap, const char *suffix)
{
    const char *base = cam_mqtt_topic();      /* 例：esp32cam/<前缀>/jpg */
    const char *slash = strrchr(base, '/');
    int prefix = slash ? (int)(slash - base) : (int)strlen(base);
    if (prefix <= 0) {
        prefix = (int)strlen(base);
    }
    snprintf(out, cap, "%.*s/%s", prefix, base, suffix);
}

/* ---------------- 连接 ---------------- */

static void rx_close(void)
{
    if (s_tls != NULL) {
        esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
    }
}

static bool rx_connect(const char *sub_topic)
{
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = RX_TIMEOUT_MS,
    };
    const char *host = cam_mqtt_host();
    uint16_t    port = cam_mqtt_port();

    s_tls = esp_tls_init();
    if (s_tls == NULL) {
        return false;
    }
    if (esp_tls_conn_new_sync(host, (int)strlen(host), port, &cfg, s_tls) != 1) {
        ESP_LOGW(TAG, "命令通道 TLS 连接 %s:%u 失败", host, (unsigned)port);
        rx_close();
        return false;
    }

    /* CONNECT（CleanSession，client id 带随机后缀，避免和推流那条撞号） */
    char cid[40];
    snprintf(cid, sizeof(cid), "esp32s3cmd-%02x%02x%02x",
             (unsigned)(esp_timer_get_time() & 0xFF),
             (unsigned)((esp_timer_get_time() >> 8) & 0xFF),
             (unsigned)((esp_timer_get_time() >> 16) & 0xFF));
    uint8_t pkt[128];
    int n = 0;
    int cidlen = (int)strlen(cid);
    pkt[n++] = 0x10;
    n += enc_remlen(&pkt[n], 10 + 2 + cidlen);
    pkt[n++] = 0x00; pkt[n++] = 0x04;
    memcpy(&pkt[n], "MQTT", 4); n += 4;
    pkt[n++] = 0x04;
    pkt[n++] = 0x02;
    pkt[n++] = 0x00; pkt[n++] = KEEPALIVE_S;
    pkt[n++] = (uint8_t)(cidlen >> 8);
    pkt[n++] = (uint8_t)(cidlen & 0xFF);
    memcpy(&pkt[n], cid, (size_t)cidlen); n += cidlen;
    if (!tls_write_all(pkt, n)) {
        rx_close();
        return false;
    }

    uint8_t ack[4] = {0};
    for (int i = 0; i < 4; i++) {
        if (read_n(&ack[i], 1) != 0) {
            break;
        }
    }
    if (ack[0] != 0x20 || ack[3] != 0x00) {
        ESP_LOGW(TAG, "命令通道 CONNACK 异常 (0x%02x/%d)", ack[0], ack[3]);
        rx_close();
        return false;
    }

    /* SUBSCRIBE，QoS0 */
    int tlen = (int)strlen(sub_topic);
    uint8_t sp[192];
    int m = 0;
    sp[m++] = 0x82;
    m += enc_remlen(&sp[m], 2 + 2 + tlen + 1);
    sp[m++] = 0x00; sp[m++] = 0x01;        /* packet id = 1 */
    sp[m++] = (uint8_t)(tlen >> 8);
    sp[m++] = (uint8_t)(tlen & 0xFF);
    memcpy(&sp[m], sub_topic, (size_t)tlen); m += tlen;
    sp[m++] = 0x00;                        /* 请求 QoS0 */
    if (!tls_write_all(sp, m)) {
        rx_close();
        return false;
    }

    ESP_LOGI(TAG, "命令通道已连上，订阅 %s", sub_topic);
    return true;
}

/* ---------------- 执行与回传 ---------------- */

/* 按行切块发回去：一条 PUBLISH 塞 900 字节，切点尽量落在换行上，
 * 免得把多字节汉字劈成两半，页面上出现一串乱码。 */
static void publish_output(const char *txt, size_t len)
{
    char part[OUT_CHUNK + 1];
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > OUT_CHUNK) {
            chunk = OUT_CHUNK;
            /* 往回找最后一个换行 */
            for (size_t i = off + chunk; i > off + chunk / 2; i--) {
                if (txt[i - 1] == '\n') {
                    chunk = i - off;
                    break;
                }
            }
        }
        memcpy(part, txt + off, chunk);
        part[chunk] = '\0';
        cam_mqtt_publish_text(OUT_SUFFIX, part);
        off += chunk;
        /* 让出一点时间，别把发送互斥量长时间占住（推流还在发帧） */
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

/* ---------------- 防重放挑战码 ---------------- */

static void chal_rotate(void)
{
    memcpy(s_prev_chal, s_chal, sizeof(s_chal));
    uint8_t r[8];
    esp_fill_random(r, sizeof(r));
    snprintf(s_chal, sizeof(s_chal), "%02x%02x%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    /* 挑战码本身也走加密通道，和别的话题一样 */
    cam_mqtt_publish_text(CHAL_SUFFIX, s_chal);
}

/* ---------------- 执行 ---------------- */

static void run_line(const char *data, int len)
{
    char line[CMDLINE_MAX + 1];
    int n = 0;

    if (len > CMDLINE_MAX) {
        len = CMDLINE_MAX;
    }
    for (int i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r' || c == '\n' || c == '\0') {
            continue;
        }
        line[n++] = c;
    }
    line[n] = '\0';
    if (n == 0) {
        return;
    }

    s_runs++;
    ESP_LOGI(TAG, "远程命令 #%u: %s", (unsigned)s_runs, line);

    mqtt_console_begin();
    tf_exec_line(line);
    size_t got = mqtt_console_end();

    if (got == 0) {
        /* 命令确实执行了但没打印任何东西，给页面一句回执，免得像没反应 */
        cam_mqtt_publish_text(OUT_SUFFIX, "[已执行，无输出]\n");
        return;
    }
    publish_output(mqtt_console_text(), got);
}

/* 下行报文先解密再校验挑战码。格式：c=<16 位十六进制>|<命令> */
static void handle_publish(const char *topic, const uint8_t *p, int len)
{
    if (len < 2) {
        return;
    }
    int tl = (p[0] << 8) | p[1];
    if (len < 2 + tl) {
        return;
    }
    const uint8_t *payload = p + 2 + tl;
    int plen = len - 2 - tl;

    static uint8_t plain[PKT_MAX];
    int n = sec_open(topic, payload, (size_t)plen, plain, sizeof(plain) - 1);
    if (n < 0) {
        /* 密钥不对 / 被篡改 / 被搬了话题：一律当攻击处理，不回显任何内容 */
        s_reject++;
        ESP_LOGW(TAG, "密文校验失败，丢弃（累计 %u 次）", (unsigned)s_reject);
        return;
    }
    plain[n] = '\0';

    if (n < 20 || memcmp(plain, "c=", 2) != 0 || plain[18] != '|') {
        s_reject++;
        ESP_LOGW(TAG, "命令格式不对，丢弃");
        cam_mqtt_publish_text(OUT_SUFFIX, "[安全] 命令缺少挑战码，已拒绝\n");
        return;
    }
    /* 限速：合法客户端不会 250 ms 内连发两条，挡住"拿着密文疯狂重放"的玩法 */
    static int64_t last_run_us;
    int64_t now_us = esp_timer_get_time();
    if (last_run_us != 0 && now_us - last_run_us < 250000) {
        s_reject++;
        ESP_LOGW(TAG, "命令过于频繁，丢弃");
        return;
    }
    if (memcmp(plain + 2, s_chal, 16) != 0 && memcmp(plain + 2, s_prev_chal, 16) != 0) {
        s_reject++;
        ESP_LOGW(TAG, "挑战码过期，丢弃");
        cam_mqtt_publish_text(OUT_SUFFIX, "[安全] 挑战码已过期，请重发\n");
        return;
    }
    last_run_us = now_us;
    run_line((const char *)(plain + 19), n - 19);
}

/* ---------------- 任务 ---------------- */

static void cmd_task(void *arg)
{
    (void)arg;
    char sub[128];
    uint8_t hdr;
    uint8_t pkt[PKT_MAX];

    for (;;) {
        topic_of(sub, sizeof(sub), CMD_SUFFIX);

        if (!rx_connect(sub)) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        /* 连上就先发一个挑战码，页面不用等满 20 秒才能发命令 */
        chal_rotate();

        int64_t last_ping = esp_timer_get_time() / 1000000;
        int64_t last_chal = last_ping;
        bool broken = false;

        while (!broken) {
            int r = read_n(&hdr, 1);
            if (r == 1) {
                /* 只是没命令：定期心跳，别让 broker 把这条连接踢掉 */
                int64_t now = esp_timer_get_time() / 1000000;
                if (now - last_ping >= PING_EVERY_S) {
                    last_ping = now;
                    const uint8_t ping[2] = {0xC0, 0x00};
                    if (!tls_write_all(ping, 2)) {
                        broken = true;
                    }
                }
                if (now - last_chal >= CHAL_EVERY_S) {
                    last_chal = now;
                    /* 每 8 秒换一次，只认当前和上一个：截获的旧命令最多 16 秒有效 */
                    chal_rotate();
                }
                continue;
            }
            if (r < 0) {
                broken = true;
                break;
            }

            int type = hdr >> 4;
            int rem = 0;
            if (read_remlen(&rem) != 0) {
                broken = true;
                break;
            }
            if (rem < 0 || rem > (int)sizeof(pkt)) {
                /* 太长的报文没有业务价值，直接断开重连，避免半包残留 */
                broken = true;
                break;
            }
            if (rem > 0 && read_n(pkt, rem) != 0) {
                broken = true;
                break;
            }

            if (type == 3) {
                handle_publish(sub, pkt, rem);
            } else if (type == 13) {
                /* PINGRESP，无事可做 */
            } else if (type == 9) {
                ESP_LOGI(TAG, "订阅确认 SUBACK");
            }
        }

        ESP_LOGW(TAG, "命令通道断开，2 秒后重连");
        rx_close();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t mqtt_cmd_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    /* 8 KB 栈：tf_exec_line 里会走到 app_cmds、cam_mqtt_dump 等一串 printf，
     * 再加上 TLS 读，4 KB 太紧。 */
    if (xTaskCreate(cmd_task, "mqtt_cmd", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "命令通道任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
