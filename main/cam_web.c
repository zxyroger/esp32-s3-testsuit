/**
 * @file  cam_web.c
 * @brief HTTP 服务器 + MJPEG 推流
 *
 * 并发模型：
 *   esp_http_server 每个连接一个任务，所以 /stream 的处理函数里可以直接
 *   写死循环推帧，不影响别的连接。
 *
 * 内存策略：
 *   每个 /stream 客户端在 PSRAM 里拿一块与 cam_jpeg_frame_cap() 等长的
 *   私有缓冲（当前 150 KB，按 VGA JPEG 的上界算）。
 *   为什么不共用全局缓冲：cam_jpeg_snapshot() 内部持锁 memcpy，
 *   如果直接在锁内做 TCP 发送，一个慢手机就会把采集任务卡死。
 *   现在锁只包住 memcpy（微秒级），发送在锁外，慢客户端只影响自己。
 *
 * 限速：
 *   采集端 QVGA JPEG 大概 10~20 fps。推流按 CAM_STREAM_MIN_INTERVAL_MS
 *   限制最小帧间隔，避免把一个 20 fps 的流硬塞给 4G 手机。
 */
#include "cam_web.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "cam_jpeg.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cam_web";

#define CAM_HTTP_PORT             80
/* 外网备用端口。
 * 杭州移动家用宽带的 IPv4 入站 80/443/8080 基本都被封，IPv6 入站也常见只
 * 封低端口；所以同一套页面额外挂一个高端口。局域网仍用 80，手机在外网
 * 用 https://[IPv6]:18080/ 访问。两个 httpd 实例跑同一批 handler，互不干扰。 */
#define CAM_HTTP_PORT_ALT         18080
#define CAM_STREAM_BOUNDARY       "frameboundary"
#define CAM_STREAM_MIN_INTERVAL_MS 50   /* 最快 20 fps，够用且不浪费带宽 */
#define CAM_STREAM_MAX_FAIL       3     /* 连续发送失败几次就断开 */

static httpd_handle_t s_server = NULL;
static httpd_handle_t s_server_alt = NULL;

/* /jpg 用一块共享缓冲：同一时刻只服务一个快照请求，够用且省 PSRAM */
static uint8_t          *s_snap_buf = NULL;
static SemaphoreHandle_t s_snap_mtx = NULL;

int cam_web_port(void)
{
    return CAM_HTTP_PORT;
}

int cam_web_alt_port(void)
{
    return CAM_HTTP_PORT_ALT;
}

bool cam_web_running(void)
{
    return s_server != NULL;
}

/* ---------------- 页面 ---------------- */

static const char k_index_html[] =
"<!DOCTYPE html><html lang=\"zh-CN\"><head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
"<meta name=\"apple-mobile-web-app-capable\" content=\"yes\">"
"<title>ESP32-S3 摄像头</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"html,body{height:100%;background:#0b0d10;color:#e6e9ef;"
"font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;overflow:hidden}"
"#wrap{position:fixed;inset:0;display:flex;align-items:center;justify-content:center}"
"#v{max-width:100%;max-height:100%;width:auto;height:auto;display:block;"
"background:#000;object-fit:contain}"
"#hud{position:fixed;top:0;left:0;right:0;padding:10px 14px;display:flex;"
"justify-content:space-between;align-items:center;font-size:13px;"
"background:linear-gradient(180deg,rgba(0,0,0,.72),rgba(0,0,0,0));"
"pointer-events:none;font-variant-numeric:tabular-nums}"
"#hud b{color:#4ade80;font-weight:600}"
"#hud .dim{color:#9aa4b2}"
"#err{position:fixed;inset:0;display:none;align-items:center;justify-content:center;"
"padding:24px;text-align:center;font-size:15px;line-height:1.7;color:#fca5a5}"
"</style></head><body>"
"<div id=\"wrap\"><img id=\"v\" alt=\"camera\"></div>"
"<div id=\"hud\"><span><b id=\"fps\">--</b> fps <span class=\"dim\" id=\"res\"></span></span>"
"<span class=\"dim\" id=\"state\">连接中…</span></div>"
"<div id=\"err\"></div>"
"<script>"
"var v=document.getElementById('v'),fpsEl=document.getElementById('fps'),"
"resEl=document.getElementById('res'),st=document.getElementById('state'),"
"er=document.getElementById('err');"
"var frames=0,nbytes=0,t0=performance.now(),lastUrl=null,running=false;"
"function showFrame(u8){"
"var url=URL.createObjectURL(new Blob([u8],{type:'image/jpeg'}));"
"v.src=url;var old=lastUrl;lastUrl=url;"
"if(old){setTimeout(function(){URL.revokeObjectURL(old);},800);}"
"frames++;nbytes+=u8.length;"
"}"
"function scanEoh(b){var n=Math.min(b.length,512);"
"for(var i=0;i+3<n;i++){if(b[i]===13&&b[i+1]===10&&b[i+2]===13&&b[i+3]===10)return i+4;}"
"return -1;}"
"function contentLen(b,n){var s='';for(var i=0;i<n;i++)s+=String.fromCharCode(b[i]);"
"var m=/Content-Length:\\s*(\\d+)/i.exec(s);return m?parseInt(m[1],10):-1;}"
"async function run(){"
"if(running)return;running=true;"
"try{"
"var r=await fetch('/stream',{cache:'no-store'});"
"if(!r.ok||!r.body)throw new Error('http '+r.status);"
"st.textContent='在线';er.style.display='none';"
"var rd=r.body.getReader(),buf=new Uint8Array(0);"
"while(true){"
"var q=await rd.read();"
"if(q.done)break;"
"var nb=new Uint8Array(buf.length+q.value.length);"
"nb.set(buf);nb.set(q.value,buf.length);buf=nb;"
"while(true){"
"var eoh=scanEoh(buf);if(eoh<0)break;"
"var len=contentLen(buf,eoh);if(len<=0||buf.length<eoh+len)break;"
"showFrame(buf.subarray(eoh,eoh+len));"
"buf=buf.subarray(eoh+len);"
"}"
"if(buf.length>262144)buf=new Uint8Array(0);"
"}"
"}catch(e){"
"st.textContent='重连中…';"
"}"
"running=false;"
"setTimeout(run,1200);"
"}"
"setInterval(function(){"
"var now=performance.now(),dt=(now-t0)/1000;"
"if(dt>0){fpsEl.textContent=(frames/dt).toFixed(1);"
"resEl.textContent=(v.naturalWidth||0)+'x'+(v.naturalHeight||0)+' · '+"
"Math.round(nbytes/dt/1024)+'KB/s';}"
"if(frames===0&&running)st.textContent='等待画面…';"
"frames=0;nbytes=0;t0=now;"
"},1000);"
"run();"
"document.body.addEventListener('click',function(){"
"if(document.fullscreenElement){document.exitFullscreen();}"
"else if(document.documentElement.requestFullscreen){document.documentElement.requestFullscreen();}});"
"</script></body></html>";

/* 把请求来源打到串口。
 * 用途：手机从 4G 打进来失败时，串口里有没有这一行，能直接把问题一分为二——
 *   有这行 = 包进了板子，问题在回程（MTU/防火墙单向拦截）；
 *   没这行 = 包根本没到板子，拦截在路由器/光猫/运营商。 */
static void log_client(const char *uri, httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return;
    }
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) {
        return;
    }
    char host[64] = "?";
    if (ss.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, host, sizeof(host));
    } else if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, host, sizeof(host));
    }
    ESP_LOGI(TAG, "%s 来源 %s", uri, host);
}

static esp_err_t h_index(httpd_req_t *req)
{
    log_client("/", req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, k_index_html, HTTPD_RESP_USE_STRLEN);
}

/* ---------------- 单帧 ---------------- */

static esp_err_t h_jpg(httpd_req_t *req)
{
    if (s_snap_buf == NULL || s_snap_mtx == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no buffer");
    }

    size_t len = 0;
    uint32_t seq = 0;

    /* 缓冲是共享的，必须串行化；持锁只有一次 memcpy */
    if (xSemaphoreTake(s_snap_mtx, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    seq = cam_jpeg_snapshot(s_snap_buf, cam_jpeg_frame_cap(), &len);
    esp_err_t err = ESP_OK;
    if (seq == 0 || len == 0) {
        xSemaphoreGive(s_snap_mtx);
        /* 还没有帧：503 比返回空图更诚实，浏览器/脚本能据此重试。
         * IDF 没有 HTTPD_503 常量，只能手动设状态码。 */
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "no frame yet", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, (const char *)s_snap_buf, len);
    xSemaphoreGive(s_snap_mtx);
    return err;
}

/* ---------------- 实时流 ---------------- */

static esp_err_t h_stream(httpd_req_t *req)
{
    const size_t cap = cam_jpeg_frame_cap();
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }

    esp_err_t err = httpd_resp_set_type(
        req, "multipart/x-mixed-replace;boundary=" CAM_STREAM_BOUNDARY);
    if (err != ESP_OK) {
        heap_caps_free(buf);
        return err;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    ESP_LOGI(TAG, "流客户端接入（剩余堆 %u B）",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    uint32_t last_seq = 0;
    int fails = 0;
    int64_t last_send = 0;

    /* 先把第一个分隔行发出去，让浏览器立刻进入 multipart 解析状态 */
    static const char k_first_part[] =
        "--" CAM_STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\n";
    if (httpd_resp_send_chunk(req, k_first_part, sizeof(k_first_part) - 1) != ESP_OK) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    while (true) {
        size_t len = 0;
        uint32_t seq = cam_jpeg_snapshot(buf, cap, &len);

        /* 没有新帧就等一会儿，避免空转占 CPU 和网络 */
        if (seq == 0 || seq == last_seq || len == 0) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        int64_t now = esp_timer_get_time();
        if (last_send != 0 && (now - last_send) < CAM_STREAM_MIN_INTERVAL_MS * 1000) {
            vTaskDelay(pdMS_TO_TICKS(CAM_STREAM_MIN_INTERVAL_MS / 2));
            continue;
        }
        last_send = now;

        /* 每帧的长度头必须单独发：分块发送时长度未知，用 Content-Length
         * 让浏览器知道这一帧到哪结束。 */
        char part[96];
        int plen = snprintf(part, sizeof(part),
                            "Content-Length: %u\r\n\r\n", (unsigned)len);
        /* 分隔行的长度必须用 strlen 数，不能手写常数 —— 
         * 之前把 19 字节的 "\r\n--frameboundary\r\n" 写成 26，
         * 多发的 7 个字节会把下一帧的头冲掉，浏览器直接显示花屏。 */
        static const char k_next_part[] = "\r\n--" CAM_STREAM_BOUNDARY "\r\n";
        if (httpd_resp_send_chunk(req, part, plen) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)buf, len) != ESP_OK ||
            httpd_resp_send_chunk(req, k_next_part, sizeof(k_next_part) - 1) != ESP_OK) {
            if (++fails >= CAM_STREAM_MAX_FAIL) {
                /* 客户端已经走了（手机锁屏/切网），正常退出，不打错误日志 */
                ESP_LOGI(TAG, "流客户端断开");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        fails = 0;
        last_seq = seq;
    }

    heap_caps_free(buf);
    return ESP_OK;
}

/* ---------------- 状态 ---------------- */

static esp_err_t h_stats(httpd_req_t *req)
{
    log_client("/stats", req);
    cam_jpeg_stat_t st;
    cam_jpeg_get_stat(&st);

    char json[320];
    int n = snprintf(json, sizeof(json),
                     "{\"w\":%u,\"h\":%u,\"fps\":%u,\"frames\":%u,\"dropped\":%u,"
                     "\"kb\":%u,\"seq\":%u,\"uptime\":%u,\"running\":%s}",
                     (unsigned)st.width, (unsigned)st.height, (unsigned)st.avg_fps,
                     (unsigned)st.frames, (unsigned)st.dropped,
                     (unsigned)((st.last_len + 512) / 1024), (unsigned)st.seq,
                     (unsigned)(st.uptime_ms / 1000),
                     cam_jpeg_running() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

/* ---------------- 生命周期 ---------------- */

esp_err_t cam_web_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    if (s_snap_buf == NULL) {
        s_snap_buf = heap_caps_malloc(cam_jpeg_frame_cap(),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_snap_buf == NULL) {
            ESP_LOGE(TAG, "PSRAM 分配快照缓冲失败");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_snap_mtx == NULL) {
        s_snap_mtx = xSemaphoreCreateMutex();
        if (s_snap_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CAM_HTTP_PORT;
    /* 推流处理函数里要栈放 96 字节头 + 调用链，4096 偏紧，给 6144 */
    cfg.stack_size = 6144;
    /* 6 个 socket 给手机留足余量：主连接一条 /stream，外加 /jpg、/stats、
     * 页面本身、以及切换网络时残留的半死连接。
     *
     * 注意 IDF 的 httpd 是**单任务串行**处理请求的，不是每个 socket 一个线程。
     * 一个 /stream 长连接会把 httpd 任务一直占住，此时 /stats、/jpg 全部排队
     * 到超时（实测 8 s 拿不到响应）。所以前端页面不能一边开视频流一边轮询
     * /stats —— 它改成自己用 fetch 读 multipart 流、在浏览器侧算帧率，
     * 全程只占这一条连接。改这个文件里的 k_index_html 时别把轮询加回去。 */
    cfg.max_open_sockets = 6;
    /* 手机切网会留下半死的 socket，开长连接回收 */
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务器启动失败: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uri_index  = { .uri = "/",       .method = HTTP_GET, .handler = h_index  };
    const httpd_uri_t uri_jpg    = { .uri = "/jpg",    .method = HTTP_GET, .handler = h_jpg    };
    const httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = h_stream };
    const httpd_uri_t uri_stats  = { .uri = "/stats",  .method = HTTP_GET, .handler = h_stats  };

    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_jpg);
    httpd_register_uri_handler(s_server, &uri_stream);
    httpd_register_uri_handler(s_server, &uri_stats);

    /* 备用端口：ISP 封 80 时的外网入口。ctrl_port 必须与第一个实例错开，
     * 否则第二个实例起不来（httpd 的控制 socket 也是独占端口）。 */
    httpd_config_t cfg2 = cfg;
    cfg2.server_port = CAM_HTTP_PORT_ALT;
    cfg2.ctrl_port = cfg.ctrl_port + 1;
    if (httpd_start(&s_server_alt, &cfg2) == ESP_OK) {
        httpd_register_uri_handler(s_server_alt, &uri_index);
        httpd_register_uri_handler(s_server_alt, &uri_jpg);
        httpd_register_uri_handler(s_server_alt, &uri_stream);
        httpd_register_uri_handler(s_server_alt, &uri_stats);
        ESP_LOGI(TAG, "外网备用端口 %d 已启动", CAM_HTTP_PORT_ALT);
    } else {
        s_server_alt = NULL;
        ESP_LOGW(TAG, "外网备用端口 %d 启动失败（不影响 %d）",
                 CAM_HTTP_PORT_ALT, CAM_HTTP_PORT);
    }

    ESP_LOGI(TAG, "HTTP 服务器已启动，端口 %d（/  /jpg  /stream  /stats）",
             CAM_HTTP_PORT);
    return ESP_OK;
}

esp_err_t cam_web_stop(void)
{
    if (s_server_alt != NULL) {
        httpd_stop(s_server_alt);
        s_server_alt = NULL;
    }
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;

    if (s_snap_buf) {
        heap_caps_free(s_snap_buf);
        s_snap_buf = NULL;
    }
    ESP_LOGI(TAG, "HTTP 服务器已停止");
    return err;
}
