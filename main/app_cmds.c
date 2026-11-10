/**
 * @file  app_cmds.c
 * @brief 串口命令行里的应用命令：cam / wifi
 *
 * 与测试框架分开：框架只管跑用例，这里管"真正开一个服务"。
 * 由 main.c 调 app_cmds_register() 把命令表注册进框架，tf.c 不需要认识摄像头。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "board_config.h"
#include "axp2101.h"
#include "batt_log.h"
#include "cam_jpeg.h"
#include "esp_camera.h"
#include "cam_web.h"
#include "ddns.h"
#include "health.h"
#include "cam_mqtt.h"
#include "mqtt_cmd.h"
#include "sec_crypto.h"
#include "app_cmds.h"
#include "tf.h"
#include "wifi_lab.h"

#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static wifi_lab_t *s_lab = NULL;

/* ---------------- 前置：WiFi 必须先在跑 ---------------- */

static wifi_lab_t *ensure_wifi(void)
{
    /* 注意判断 inited 而不是指针非空：wifi_lab_stop() 之后指针还在，
     * 但射频已经停了。只看指针会让 wifi join 直接返回 INVALID_STATE，
     * 于是跑完 wifi 组之后既连不上网、又没法用命令救回来，只能重启。 */
    if (s_lab != NULL && s_lab->inited) {
        return s_lab;
    }
    wifi_lab_t *lab = s_lab;
    if (wifi_lab_init(&lab) != ESP_OK) {
        printf("[CAM] WiFi 协议栈初始化失败\n");
        return NULL;
    }
    s_lab = lab;
    return lab;
}

static bool wifi_has_ip(wifi_lab_t *lab, uint32_t *ip_out)
{
    uint32_t ip = 0;
    if (wifi_lab_sta_ip(lab, &ip) != ESP_OK || ip == 0) {
        return false;
    }
    if (ip_out) {
        *ip_out = ip;
    }
    return true;
}

static const char *auth_note(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT:
        return "超时（多半是信号弱或 SSID 是 5G 频段，S3 只有 2.4G）";
    case ESP_ERR_INVALID_ARG:
        return "参数非法";
    default:
        return esp_err_to_name(err);
    }
}

/* ---------------- cam probe：量引脚到底有没有在翻转 ---------------- */

/**
 * 停掉 DVP，把 VSYNC/PCLK/D0..D7 当普通输入，用忙循环数跳变。
 *
 * 这一步是为了把"传感器不出流"和"控制器收不到"分开：
 *   - VSYNC 有跳变、PCLK 有跳变  -> 传感器在出流，问题在控制器/配置
 *   - 全是 0                     -> 传感器没出流（初始化没真正生效，或 PWDN/XCLK 不对）
 * 没有示波器时，这是唯一能拿到硬证据的办法。
 */
static void probe_pins(const char *label)
{
    static const struct { int gpio; const char *name; } pins[] = {
        { BOARD_CAM_PCLK_GPIO,  "PCLK"  },
        { BOARD_CAM_VSYNC_GPIO, "VSYNC" },
        { BOARD_CAM_HREF_GPIO,  "HREF"  },
        { BOARD_CAM_D0_GPIO,    "D0"    },
    };
    const int n = sizeof(pins) / sizeof(pins[0]);

    for (int i = 0; i < n; i++) {
        gpio_config_t io = {
            .pin_bit_mask = 1ull << pins[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
    }

    int prev[4] = { 0 };
    int edges[4] = { 0 };
    int last[4] = { 0 };
    for (int i = 0; i < n; i++) {
        prev[i] = gpio_get_level(pins[i].gpio);
    }

    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < 300000) {   /* 300 ms */
        for (int i = 0; i < n; i++) {
            int v = gpio_get_level(pins[i].gpio);
            if (v != prev[i]) {
                edges[i]++;
                prev[i] = v;
            }
            last[i] = v;
        }
    }

    printf("\n[PROBE] %s：300 ms 内跳变次数\n", label);
    for (int i = 0; i < n; i++) {
        printf("[PROBE]   %-5s = %6d 次（末态 %d）\n", pins[i].name, edges[i], last[i]);
    }
    printf("[PROBE]   帧边界(VSYNC)：%s\n",
           edges[1] > 0 ? "有" : "没有 —— 收不到 EOF");
}

static void do_cam_probe(void)
{
    if (cam_jpeg_running()) {
        printf("[PROBE] 先停掉出图，避免引脚被 CAM 外设占用…\n");
        cam_web_stop();
        cam_jpeg_stop();
    }
    probe_pins("当前格式（JPEG 初始化后）");
    printf("[PROBE] 需要恢复出图时发 cam start\n\n");
}

/* ---------------- cam ---------------- */

static void print_status(void)
{
    cam_jpeg_stat_t st;
    cam_jpeg_get_stat(&st);

    printf("[CAM] 画面: %s\n", cam_jpeg_running() ? "运行中" : "未启动");
    if (st.frames > 0) {
        printf("[CAM] 分辨率 %ux%u  平均 %u fps  最近一帧 %u B\n",
               (unsigned)st.width, (unsigned)st.height, (unsigned)st.avg_fps,
               (unsigned)st.last_len);
    }
    printf("[CAM] 累计 %u 帧，丢弃 %u，驱动错误 %u，运行 %u s\n",
           (unsigned)st.frames, (unsigned)st.dropped, (unsigned)st.errors,
           (unsigned)(st.uptime_ms / 1000));
    printf("[CAM] 网页服务: %s（端口 %d）\n",
           cam_web_running() ? "已启动" : "未启动", cam_web_port());

    if (s_lab != NULL) {
        uint32_t ip = 0;
        char buf[16];
        if (wifi_has_ip(s_lab, &ip)) {
            wifi_lab_ip_str(ip, buf, sizeof(buf));
            printf("[CAM] 局域网访问: http://%s/\n", buf);
        } else {
            printf("[CAM] 网络: STA 未连接（发 wifi join 加入路由器）\n");
        }
    }
}

static void do_cam_start(void)
{
    wifi_lab_t *lab = ensure_wifi();
    if (lab == NULL) {
        return;
    }

    uint32_t ip = 0;
    if (!wifi_has_ip(lab, &ip)) {
        printf("[CAM] 还没联网，先加入 \"%s\" …\n", BOARD_WIFI_STA_SSID);
        esp_err_t err = wifi_lab_sta_join(lab, BOARD_WIFI_STA_SSID,
                                          BOARD_WIFI_STA_PASS,
                                          BOARD_WIFI_STA_TIMEOUT_MS);
        if (err != ESP_OK) {
            printf("[CAM] 加入失败: %s\n", auth_note(err));
            printf("[CAM] 可改用 SoftAP：run wifi.ap_start，手机直连 ESP32S3-LAB-xxxx\n");
            return;
        }
        wifi_lab_sta_ip(lab, &ip);
    }

    char ipbuf[16];
    wifi_lab_ip_str(ip, ipbuf, sizeof(ipbuf));
    printf("[CAM] 已联网，IP = %s\n", ipbuf);

    cam_t *cam = board_camera();
    axp2101_t *pmic = board_pmic();
    if (cam == NULL) {
        printf("[CAM] 摄像头句柄不可用（I2C 或 SCCB 挂载失败）\n");
        return;
    }
    if (pmic == NULL) {
        printf("[CAM] AXP2101 不可用，无法给摄像头上电\n");
        return;
    }

    if (!cam_jpeg_running()) {
        printf("[CAM] 正在初始化传感器与 DVP …\n");
        esp_err_t err = cam_jpeg_start(cam, pmic);
        if (err != ESP_OK) {
            printf("[CAM] 出图启动失败: %s\n", esp_err_to_name(err));
            return;
        }
    }

    if (!cam_web_running()) {
        esp_err_t err = cam_web_start();
        if (err != ESP_OK) {
            printf("[CAM] 网页服务启动失败: %s\n", esp_err_to_name(err));
            return;
        }
    }

    /* 等第一帧：首帧要等传感器出第一个 VSYNC，通常 100~300 ms */
    for (int i = 0; i < 40; i++) {
        cam_jpeg_stat_t st;
        cam_jpeg_get_stat(&st);
        if (st.frames > 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    printf("\n[CAM] ======== 可以看了 ========\n");
    printf("[CAM] 手机/电脑同一网络:  http://%s/\n", ipbuf);
    printf("[CAM] 单张快照:           http://%s/jpg\n", ipbuf);
    printf("[CAM] 实时流:             http://%s/stream\n", ipbuf);
    printf("[CAM] 状态 JSON:          http://%s/stats\n", ipbuf);
    printf("[CAM] 公网看：在本机跑 cloudflared 反代到这个地址即可\n\n");
    print_status();
}

static void do_cam_stop(void)
{
    cam_web_stop();
    cam_jpeg_stop();
    printf("[CAM] 已停止出图与网页服务\n");
}

/* ---------------- 自检结果上报 ---------------- */

/**
 * 把最近一次自检结果推到 <前缀>/test。
 *
 * 为什么用"轮询版本号"而不是"跑完就发"：开机自检跑完时 MQTT 还没连上
 * （要先联网、再 TLS 握手），那时发一定失败。轮询两个条件即可覆盖全部场景：
 *   1) gen 变了（手动 run 了一次）→ 发；
 *   2) 链路从断到通（开机首次连上 / 掉线重连）→ 重发，页面刷新后不会空着。
 */
static void test_result_task(void *arg)
{
    (void)arg;
    uint32_t sent_gen = 0;
    bool was_up = false;

    for (;;) {
        const char *js = tf_results_json();
        uint32_t gen = tf_results_gen();
        bool up = cam_mqtt_up();

        if (js != NULL && up && (!was_up || gen != sent_gen)) {
            /* 用 RETAIN：后打开网页的人一订阅就能拿到最后一份，
             * 不用等板子重连，也不用手动点"拉取"。 */
            if (cam_mqtt_publish_text_retain("test", js) == ESP_OK) {
                sent_gen = gen;
                ESP_LOGI("app", "自检结果已上报（%d 字节，gen=%u）",
                         (int)strlen(js), (unsigned)gen);
            }
        }
        if (!up) {
            /* 重连后要再发一次：broker 不保存非 retained 的历史消息 */
            sent_gen = 0;
        }
        was_up = up;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void test_result_start(void)
{
    static bool inited;
    if (inited) {
        return;
    }
    inited = true;
    xTaskCreate(test_result_task, "test_pub", 4096, NULL, 2, NULL);
}

/* ---------------- wifi ---------------- */

static void do_wifi_join(const char *ssid, const char *pass);

/**
 * 跑完会断网的测试后恢复联网。
 *
 * 触发路径：`run wifi` / `cat <分组>` 里有 TF_FLAG_NONET 的用例执行过 →
 * tf.c 在批次末尾回调这里。此时射频可能已被 esp_wifi_stop() 关掉，
 * 所以要先 ensure_wifi()（会走 wifi_lab_init 的重启分支）再重新加入。
 */
static void net_restore_after_test(void)
{
    wifi_lab_t *lab = ensure_wifi();
    if (lab == NULL) {
        printf("[WIFI] 恢复联网失败：协议栈没起来，需要重启\n");
        return;
    }
    do_wifi_join(NULL, NULL);   /* 加入板级默认路由器 */

    /* WiFi 回来不代表 MQTT 立刻回来：推流任务只在"写失败"后才重连，
     * 命令通道有自己的重试节奏。主动戳一下，页面不用等看门狗那 60 秒。 */
    cam_mqtt_reconnect();
}

static void do_wifi_join(const char *ssid, const char *pass)
{
    wifi_lab_t *lab = ensure_wifi();
    if (lab == NULL) {
        return;
    }

    const char *use_ssid = (ssid && *ssid) ? ssid : BOARD_WIFI_STA_SSID;
    const char *use_pass = (pass && *pass) ? pass : BOARD_WIFI_STA_PASS;

    printf("[WIFI] 正在加入 \"%s\" …\n", use_ssid);
    esp_err_t err = wifi_lab_sta_join(lab, use_ssid, use_pass,
                                      BOARD_WIFI_STA_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("[WIFI] 失败: %s（原因码 %u）\n", auth_note(err),
               (unsigned)lab->last_disc_reason);
        return;
    }

    uint32_t ip = 0;
    if (wifi_lab_sta_ip(lab, &ip) == ESP_OK) {
        char buf[16];
        wifi_lab_ip_str(ip, buf, sizeof(buf));
        printf("[WIFI] 已连接，IP = %s，网关可达即说明 DHCP 正常\n", buf);
    }
}

/* ---------------- batt：电池与充电状态 ---------------- */

/** 充电阶段（REG 0x01 bit2:0）的中文说法，位义见数据手册 8.2 */
static const char *chg_stat_name(uint8_t s)
{
    switch (s) {
    case 0:  return "涓流充电（电池过放，先用小电流唤醒）";
    case 1:  return "预充电（电压偏低，小电流回升中）";
    case 2:  return "恒流充电（主力阶段，电流最大）";
    case 3:  return "恒压充电（电压到顶，电流逐渐收小）";
    case 4:  return "充电完成";
    case 5:  return "未在充电";
    default: return "保留值（手册未定义）";
    }
}

/** 电池电流方向（REG 0x01 bit6:5） */
static const char *cur_dir_name(uint8_t d)
{
    switch (d) {
    case 0:  return "待机（几乎无电流）";
    case 1:  return "充电（电流流向电池）";
    case 2:  return "放电（电池在供系统）";
    default: return "保留值";
    }
}

static void do_batt(void)
{
    axp2101_t *pmic = board_pmic();
    if (pmic == NULL) {
        printf("[BATT] AXP2101 不可用（I2C 未挂上），读不到电池状态\n");
        return;
    }

    axp2101_batt_t b;
    esp_err_t err = axp2101_read_batt(pmic, &b);
    if (err != ESP_OK) {
        printf("[BATT] 读状态失败: %s\n", esp_err_to_name(err));
        return;
    }

    printf("\n[BATT] ======== 电池与充电状态 ========\n");
    printf("[BATT] 原始寄存器  ST1(0x00)=0x%02X  ST2(0x01)=0x%02X  CHG(0x18)=0x%02X\n",
           (unsigned)b.st1, (unsigned)b.st2, (unsigned)b.chg_ctrl);

    /* --- 1. 输入侧 --- */
    printf("[BATT] USB 输入 : %s，VBUS = %u mV\n",
           b.vbus_good ? "已接入（VBUS 有效）" : "未接入", (unsigned)b.vbus_mv);

    /* --- 2. 电池本体 --- */
    if (b.bat_present) {
        printf("[BATT] 电池     : 已检测到，VBAT = %u mV（%.2f V）\n",
               (unsigned)b.vbat_mv, b.vbat_mv / 1000.0);
    } else if (b.vbat_mv > 2700) {
        printf("[BATT] 电池     : 状态位报\"未检测到\"，但 VBAT = %u mV 有真实电压\n",
               (unsigned)b.vbat_mv);
        printf("                   —— 两者矛盾时以电压为准（状态位可能要 BATFET 导通才刷新）\n");
    } else {
        printf("[BATT] 电池     : 未检测到（VBAT = %u mV，悬空）\n",
               (unsigned)b.vbat_mv);
    }

    /* --- 3. 充电阶段 --- */
    printf("[BATT] 充电阶段 : %s（CHG_STAT=%u）\n",
           chg_stat_name(b.chg_stat), (unsigned)b.chg_stat);
    printf("[BATT] 电池电流 : %s（bit6:5=%u）\n",
           cur_dir_name(b.cur_dir), (unsigned)b.cur_dir);
    printf("[BATT] 系统轨   : VSYS = %u mV\n", (unsigned)b.vsys_mv);

    if (b.pct != 0xFF) {
        printf("[BATT] 电量     : %u%%（燃料计，未做满充满放校准，仅供参考）\n",
               (unsigned)b.pct);
    } else {
        printf("[BATT] 电量     : 读不到（燃料计未使能，属正常，不影响充电）\n");
    }

    /* --- 4. 异常提示 --- */
    if (b.vbat_mv > 4400 && b.bat_present) {
        printf("[BATT] 异常     : VBAT = %u mV 超出单节锂电满电上限 4.2~4.35 V\n",
               (unsigned)b.vbat_mv);
        printf("                  要么电池不是单节锂电，要么 VBAT 采样点接到了系统轨上，\n");
        printf("                  要么电池没接好（读到的其实是 VBUS/VSYS 的串扰）。\n");
    }
    if (b.therm_regu) {
        printf("[BATT] 注意     : 芯片进入过热调节，充电电流已被压小\n");
    }
    if (b.vindpm) {
        printf("[BATT] 注意     : 触发 VINDPM（输入电压被拉低），换粗线或更强电源\n");
    }

    /* --- 5. 结论：把上面几项合成一句人话 --- */
    printf("[BATT] 结论     : ");
    if (b.chg_stat <= 3) {
        printf("正在充电（%s）\n", chg_stat_name(b.chg_stat));
    } else if (b.chg_stat == 4) {
        printf("已充满，充电器已停止灌电流（VBAT ≈ %u mV）\n", (unsigned)b.vbat_mv);
    } else if (b.vbus_good) {
        printf("插着电但没在充：电池已满 / 充电未使能 / 电池温度超范围\n");
    } else if (b.vbus_mv > 4000) {
        /* 关键判据：AXP2101 要 VBUS > VBAT + 250 mV 才认为输入可用。
         * 这里 VBUS 有电压却不 good，最常见就是被 VBAT 顶掉了。 */
        printf("VBUS 上有 %u mV，但 PMIC 判为\"不可用\"——\n", (unsigned)b.vbus_mv);
        printf("                  常见原因：VBUS(%u mV) 没比 VBAT(%u mV) 高出 250 mV 以上；\n",
               (unsigned)b.vbus_mv, (unsigned)b.vbat_mv);
        printf("                  或 VBUS 检测脚没接到 USB 5 V。充电器因此不启动。\n");
    } else {
        printf("没有外部供电，电池独立供电——想充电请插 USB\n");
    }
    printf("\n");
}

/** 每 2 秒采一次，用来观察插拔 USB / 充电时的电压走向 */
static void do_batt_watch(int seconds)
{
    axp2101_t *pmic = board_pmic();
    if (pmic == NULL) {
        printf("[BATT] AXP2101 不可用\n");
        return;
    }
    printf("\n[BATT] 开始连续采样 %d 秒（每 2 秒一行）\n", seconds);
    printf("[BATT]   时刻   VBAT   VSYS   VBUS  充电阶段  电流方向\n");
    for (int t = 0; t < seconds; t += 2) {
        axp2101_batt_t b;
        if (axp2101_read_batt(pmic, &b) == ESP_OK) {
            printf("[BATT]  %3d s  %5u  %5u  %5u    %u(%s)   %u\n",
                   t, (unsigned)b.vbat_mv, (unsigned)b.vsys_mv,
                   (unsigned)b.vbus_mv, (unsigned)b.chg_stat,
                   chg_stat_name(b.chg_stat), (unsigned)b.cur_dir);
        } else {
            printf("[BATT]  %3d s  读取失败\n", t);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    printf("[BATT] 采样结束\n\n");
}

/* ---------------- BAT 脚耦合探针 ----------------
 *
 * 为什么需要它：单看 VBAT 的绝对值，没法区分「电池在回路但采样偏高」（FAQ 2.17
 * 说的内阻压降）和「BAT 脚本来就与 VSYS 同电位」这两种完全相反的情况。
 *
 * 物理依据：电池是毫欧级内阻的硬电压源；摄像头取图、WiFi 发包都会在 VSYS 上打出
 * 几十 mV 的负载阶跃。于是——
 *   BAT 脚接着真电池 → VBAT 基本不跟 VSYS 动，(VSYS-VBAT) 的峰峰值很大；
 *   BAT 脚就是 VSYS     → VBAT 一比一跟着抖，(VSYS-VBAT) 反而恒定。
 * 所以判据落在"差值的峰峰值"上，而不是任何一个绝对值。
 */
static void do_batt_probe(void)
{
    axp2101_t *pmic = board_pmic();
    if (pmic == NULL) {
        printf("[PROBE] AXP2101 不可用\n");
        return;
    }

    enum { PROBE_N = 400, PROBE_MS = 8 };
    /* 静态数组：控制台任务栈不宽裕，600 字节别再压上去了 */
    static uint16_t s_vb[PROBE_N];
    static uint16_t s_vs[PROBE_N];

    printf("\n[PROBE] BAT 脚耦合判定：连采 %d 点 / %d ms 间隔（约 %d 秒）\n",
           PROBE_N, PROBE_MS, PROBE_N * PROBE_MS / 1000);
    printf("[PROBE] 需要负载波动才有激励，摄像头推流中效果最好\n");

    int n = 0;
    for (int i = 0; i < PROBE_N; i++) {
        uint16_t vb = 0, vs = 0;
        if (axp2101_read_mv(pmic, AXP2101_REG_ADC_VBAT_H, &vb) == ESP_OK &&
            axp2101_read_mv(pmic, AXP2101_REG_ADC_VSYS_H, &vs) == ESP_OK) {
            s_vb[n] = vb;
            s_vs[n] = vs;
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
    }
    if (n < 20) {
        printf("[PROBE] 有效样本只有 %d 个，放弃判定\n\n", n);
        return;
    }

    int vb_min = 0xFFFF, vb_max = 0, vs_min = 0xFFFF, vs_max = 0;
    int d_min = 0x7FFF, d_max = -0x7FFF;
    long vb_sum = 0, vs_sum = 0;
    for (int i = 0; i < n; i++) {
        int vb = s_vb[i], vs = s_vs[i], d = vs - vb;
        if (vb < vb_min) vb_min = vb;
        if (vb > vb_max) vb_max = vb;
        if (vs < vs_min) vs_min = vs;
        if (vs > vs_max) vs_max = vs;
        if (d < d_min) d_min = d;
        if (d > d_max) d_max = d;
        vb_sum += vb;
        vs_sum += vs;
    }
    int vb_pp = vb_max - vb_min;
    int vs_pp = vs_max - vs_min;
    int d_pp  = d_max - d_min;
    int vb_avg = (int)(vb_sum / n);
    int vs_avg = (int)(vs_sum / n);

    printf("[PROBE] 样本 %d 个\n", n);
    printf("[PROBE] VBAT   %4d mV 均   %4d~%4d  峰峰 %2d mV\n", vb_avg, vb_min, vb_max, vb_pp);
    printf("[PROBE] VSYS   %4d mV 均   %4d~%4d  峰峰 %2d mV\n", vs_avg, vs_min, vs_max, vs_pp);
    printf("[PROBE] VSYS-VBAT  %4d~%4d  峰峰 %2d mV\n", d_min, d_max, d_pp);

    /* 判据：同一节点的差值几乎不动；独立节点的话差值会把两边抖动叠加起来。
     * 但前提是激励够大 —— 5 mV 级别的抖动全在 ADC 噪声里，任何结论都是假的，
     * 所以先卡一道"激励门槛"，宁可说"测不出来"也不给假答案。 */
    int bigger = vb_pp > vs_pp ? vb_pp : vs_pp;
    printf("\n[PROBE] 判定：");
    if (bigger < 12) {
        printf("激励不足（两侧峰峰仅 %d/%d mV），无法区分\n", vb_pp, vs_pp);
        printf("[PROBE]   → 说明 VSYS 极稳，靠这点纹波判不出来；改看绝对值：\n");
        printf("[PROBE]     VBAT=%d mV，而芯片自己的恒压点是 4200 mV（REG64=3）\n", vb_avg);
        printf("[PROBE]     一节锂电满电 4.2 V，读到 4.9 V 只可能是采样点不在电芯上\n");
    } else if (d_pp <= bigger / 3) {
        printf("BAT 脚与 VSYS 是同一节点（差值峰峰 %d mV，远小于单侧 %d mV）\n", d_pp, bigger);
        printf("[PROBE]   → 电池的能量到不了系统：拔掉 USB 必定掉电，与固件无关\n");
        printf("[PROBE]   → 查三处：电池座焊盘是否虚焊 / JST 插头线序 / 电池保护板是否锁死\n");
    } else if (d_pp * 4 >= bigger * 3) {
        printf("BAT 脚是独立节点（差值峰峰 %d mV，接近两侧抖动之和）\n", d_pp);
        printf("[PROBE]   → 电池确实接在 BAT 脚上，这时 VBAT 偏高要按 FAQ 2.17 查内阻/线阻\n");
    } else {
        printf("不确定（差值峰峰 %d mV，两侧峰峰 %d/%d mV）\n", d_pp, vb_pp, vs_pp);
        printf("[PROBE]   → 让摄像头保持推流再测一次\n");
    }
    printf("\n");
}

/* ---------------- 命令分发 ---------------- */

/* 改传感器参数（分辨率/画质）前必须先停推流：
 * 推流任务正在从 DMA 缓冲里 memcpy，此时 set_framesize/set_quality 会让
 * 驱动重配缓冲，实测必重启（哪怕先停 0.7 s 也不够）。所以干脆要求先 mqtt off，
 * 由人确认停稳了再改，改完再 mqtt on。 */
static bool cam_reconfig_blocked(void)
{
    if (cam_mqtt_enabled()) {
        printf("[CAM] 推流中不能改分辨率/画质：驱动会重配 DMA 缓冲，实测直接重启。\n");
        printf("      先发 mqtt off，等两秒再改，改完发 mqtt on。\n\n");
        return true;
    }
    return false;
}

static void do_sec(void)
{
    uint8_t fp[4];
    sec_key_fp(fp);
    printf("[SEC] 加密     : %s\n", sec_ready() ? "已启用（AES-256-GCM）" : "未启用");
    printf("[SEC] 密钥指纹 : %02x%02x%02x%02x\n", fp[0], fp[1], fp[2], fp[3]);
    printf("[SEC] 覆盖范围 : jpg / meta / batt / serial-tx / chal 全部是密文\n");
    printf("[SEC] 认证方式 : GCM tag + 话题绑定（密文搬到别的 topic 会校验失败）\n");
    printf("[SEC] 防重放   : 命令必须带挑战码，8 秒一换，只认当前和上一个\n");
    printf("[SEC] 换密钥   : sec key <64 位十六进制>（重启生效，页面用 ?k=<同一串> 打开）\n");
}

static bool cmds_dispatch(const char *cmd)
{
    if (strcmp(cmd, "sec") == 0) {
        do_sec();
        return true;
    }
    if (strncmp(cmd, "sec key ", 8) == 0) {
        esp_err_t err = sec_set_key_hex(cmd + 8);
        if (err == ESP_OK) {
            printf("[SEC] 已写入 NVS，重启后生效。\n");
            printf("[SEC] 页面要跟着换：打开 …?sharecode=…&k=<同一串 64 位 hex>\n");
        } else {
            printf("[SEC] 失败：%s（需要正好 64 个十六进制字符）\n", esp_err_to_name(err));
        }
        return true;
    }
    if (strcmp(cmd, "batt") == 0) {
        do_batt();
        return true;
    }
    if (strcmp(cmd, "health") == 0) {
        health_dump();
        return true;
    }
    if (strcmp(cmd, "health test") == 0) {
        printf("[健康] 触发一次摄像头自愈：停流 -> 重新 init -> 恢复出图\n");
        printf("[健康] 3 秒后用 health 复查，seq 应该重新开始增长\n");
        health_force_trip();
        return true;
    }
    if (strncmp(cmd, "batt probe", 10) == 0) {
        do_batt_probe();
        return true;
    }
    if (strncmp(cmd, "batt watch", 10) == 0) {
        int sec = atoi(cmd + 10);
        if (sec <= 0) {
            sec = 20;
        }
        if (sec > 120) {
            sec = 120;
        }
        do_batt_watch(sec);
        return true;
    }
    if (strcmp(cmd, "ddns") == 0) {
        ddns_dump();
        return true;
    }
    if (strcmp(cmd, "ddns now") == 0) {
        esp_err_t err = ddns_update_now();
        const char *u = ddns_public_url();
        if (err == ESP_OK && u != NULL) {
            printf("[DDNS] 已更新，固定网址 %s\n", u);
        } else {
            printf("[DDNS] 更新失败: %s\n", esp_err_to_name(err));
        }
        ddns_dump();
        return true;
    }
    if (strcmp(cmd, "ddns auto") == 0) {
        esp_err_t err = ddns_set_auto(true);
        const char *u = ddns_public_url();
        printf("[DDNS] 免注册模式%s，网址 %s\n",
               err == ESP_OK ? "已启用" : "启用失败", u ? u : "(还没拿到 IPv6)");
        return true;
    }
    if (strcmp(cmd, "ddns manual") == 0) {
        ddns_set_auto(false);
        printf("[DDNS] 已切回手动模式，用 ddns set <域名> <token> 配置\n");
        return true;
    }
    if (strncmp(cmd, "ddns set ", 9) == 0) {
        /* ddns set <域名> [token]，token 省略时用已保存的 */
        char host[80] = {0}, tok[80] = {0};
        const char *p = cmd + 9;
        if (sscanf(p, "%79s %79s", host, tok) >= 1) {
            esp_err_t err = ddns_set_host(host, tok[0] ? tok : NULL);
            printf("[DDNS] %s，域名 %s\n", err == ESP_OK ? "已保存" : "保存失败", host);
            printf("[DDNS] 发 ddns now 立即上报一次\n");
        } else {
            printf("用法: ddns set <域名> [token]\n");
        }
        return true;
    }
    if (strncmp(cmd, "ddns url ", 9) == 0) {
        esp_err_t err = ddns_set_url(cmd + 9);
        printf("[DDNS] 模板%s: %s\n", err == ESP_OK ? "已保存" : "保存失败", cmd + 9);
        return true;
    }
    if (strncmp(cmd, "ddns auth ", 10) == 0) {
        char user[80] = {0}, pass[80] = {0};
        if (sscanf(cmd + 10, "%79s %79s", user, pass) == 2) {
            ddns_set_auth(user, pass);
            printf("[DDNS] 鉴权已保存（用户 %s）\n", user);
        } else {
            printf("用法: ddns auth <用户名> <密码>\n");
        }
        return true;
    }
    if (strcmp(cmd, "ddns on") == 0 || strcmp(cmd, "ddns off") == 0) {
        bool on = (cmd[6] == 'o' && cmd[7] == 'n');
        ddns_set_enabled(on);
        printf("[DDNS] 已%s\n", on ? "开启" : "关闭");
        return true;
    }
    if (strcmp(cmd, "mqtt") == 0) {
        cam_mqtt_dump();
        return true;
    }
    if (strcmp(cmd, "mqtt on") == 0 || strcmp(cmd, "mqtt off") == 0) {
        bool on = (cmd[5] == 'o' && cmd[6] == 'n');
        cam_mqtt_set_enabled(on);
        printf("[MQTT] 已%s推流\n", on ? "开启" : "关闭");
        return true;
    }
    if (strncmp(cmd, "mqtt topic ", 11) == 0) {
        esp_err_t err = cam_mqtt_set_topic(cmd + 11);
        printf("[MQTT] 主题%s: %s\n", err == ESP_OK ? "已保存" : "保存失败", cam_mqtt_topic());
        printf("[MQTT] 网页端也要同步改成这个主题，否则订阅不到\n");
        return true;
    }
    if (strncmp(cmd, "mqtt rate ", 10) == 0) {
        int fps = atoi(cmd + 10);
        cam_mqtt_set_rate(fps);
        printf("[MQTT] 帧率已设为 %d fps（公共 broker 建议 1~3）\n", fps);
        return true;
    }
    if (strcmp(cmd, "cam") == 0) {
        print_status();
        return true;
    }
    if (strcmp(cmd, "cam start") == 0 || strcmp(cmd, "cam on") == 0) {
        do_cam_start();
        return true;
    }
    if (strcmp(cmd, "cam probe") == 0) {
        do_cam_probe();
        return true;
    }
    if (strcmp(cmd, "cam bar") == 0 || strcmp(cmd, "cam colorbar") == 0) {
        cam_jpeg_set_colorbar(true);
        printf("[CAM] 已切到彩条图案。常见判定：\n");
        printf("      彩条干净竖直 -> 数据通路正常，之前的条纹是光照/工频/镜头造成的\n");
        printf("      彩条也错乱   -> D0..D7 映射或 PCLK 采样有问题\n");
        printf("      发 cam bar off 可恢复真实画面\n\n");
        return true;
    }
    if (strcmp(cmd, "cam bar off") == 0) {
        cam_jpeg_set_colorbar(false);
        printf("[CAM] 已恢复真实画面\n\n");
        return true;
    }
    if (strncmp(cmd, "cam exp", 7) == 0) {
        const char *arg = cmd + 7;
        while (*arg == ' ') {
            arg++;
        }
        if (*arg == '\0') {
            printf("用法: cam exp <行数> | cam exp auto\n");
            printf("      QVGA 下先试 200 / 400 / 800，看横纹在哪一档消失\n\n");
            return true;
        }
        if (strcmp(arg, "auto") == 0) {
            cam_jpeg_set_exposure(-1);
        } else {
            int v = atoi(arg);
            if (v <= 0) {
                printf("曝光值要是正整数（或 auto）\n\n");
                return true;
            }
            cam_jpeg_set_exposure(v);
        }
        /* 改曝光后 AEC 会重新收敛，等它稳一下再让调用方去看画面 */
        vTaskDelay(pdMS_TO_TICKS(300));
        printf("[CAM] 已生效，抓 http://<板子IP>/jpg 看效果\n\n");
        return true;
    }
    if (strncmp(cmd, "cam q", 5) == 0) {
        const char *arg = cmd + 5;
        while (*arg == ' ') {
            arg++;
        }
        int v = atoi(arg);
        if (v < 0 || v > 63) {
            printf("用法: cam q <0..63>（0 最清晰、63 最省流量）\n\n");
            return true;
        }
        if (cam_reconfig_blocked()) {
            return true;
        }
        cam_jpeg_set_quality(v);
        vTaskDelay(pdMS_TO_TICKS(200));
        printf("[CAM] 画质已设为 %d\n\n", v);
        return true;
    }
    if (strcmp(cmd, "cam res vga") == 0 || strcmp(cmd, "cam res 640") == 0) {
        if (cam_reconfig_blocked()) {
            return true;
        }
        cam_jpeg_set_res(true);
        vTaskDelay(pdMS_TO_TICKS(300));
        printf("[CAM] 已切 640x480（画质好，公网带宽吃紧）\n\n");
        return true;
    }
    if (strcmp(cmd, "cam res qvga") == 0 || strcmp(cmd, "cam res 320") == 0) {
        if (cam_reconfig_blocked()) {
            return true;
        }
        cam_jpeg_set_res(false);
        vTaskDelay(pdMS_TO_TICKS(300));
        printf("[CAM] 已切 320x240（省带宽，公网更顺）\n\n");
        return true;
    }
    if (strcmp(cmd, "cam res svga") == 0 || strcmp(cmd, "cam res 800") == 0) {
        if (cam_reconfig_blocked()) {
            return true;
        }
        esp_err_t e = cam_jpeg_set_framesize(FRAMESIZE_SVGA);
        vTaskDelay(pdMS_TO_TICKS(300));
        printf("[CAM] 已切 800x600（已存 NVS，复位保持）-> %s\n\n",
               esp_err_to_name(e));
        return true;
    }
    if (strcmp(cmd, "cam res xga") == 0 || strcmp(cmd, "cam res 1024") == 0) {
        if (cam_reconfig_blocked()) {
            return true;
        }
        esp_err_t e = cam_jpeg_set_framesize(FRAMESIZE_XGA);
        vTaskDelay(pdMS_TO_TICKS(300));
        printf("[CAM] 已切 1024x768（已存 NVS，复位保持；公网会很卡）-> %s\n\n",
               esp_err_to_name(e));
        return true;
    }
    if (strncmp(cmd, "cam pclk ", 9) == 0) {
        int v = atoi(cmd + 9);
        esp_err_t e = cam_jpeg_set_pclk(v);
        if (e == ESP_ERR_INVALID_ARG) {
            printf("[CAM] PCLK 分频只能取 4~64\n\n");
        } else {
            printf("[CAM] PCLK 分频 = %d（已存 NVS，复位保持）-> %s\n\n",
                   v, esp_err_to_name(e));
        }
        return true;
    }
    if (strcmp(cmd, "cam 50hz") == 0 || strcmp(cmd, "cam 50") == 0) {
        cam_jpeg_set_50hz(true);
        printf("[CAM] 已开 50 Hz 抗闪，等 1 秒看横纹是否消失\n\n");
        return true;
    }
    if (strcmp(cmd, "cam 50hz off") == 0) {
        cam_jpeg_set_50hz(false);
        printf("[CAM] 已关 50 Hz 抗闪\n\n");
        return true;
    }
    if (strcmp(cmd, "cam status") == 0 || strcmp(cmd, "cam info") == 0) {
        cam_jpeg_dump_status();
        return true;
    }
    if (strncmp(cmd, "cam dn ", 7) == 0) {
        int v = atoi(cmd + 7);
        esp_err_t e = cam_jpeg_set_denoise(v);
        printf("[CAM] 降噪 %d -> %s\n\n", v, esp_err_to_name(e));
        return true;
    }
    if (strncmp(cmd, "cam gain ", 9) == 0) {
        int v = atoi(cmd + 9);
        esp_err_t e = cam_jpeg_set_gain_ceiling(v);
        if (e == ESP_ERR_INVALID_ARG) {
            printf("[CAM] 只能取 2/4/8/16/32/64/128\n\n");
        } else {
            printf("[CAM] 增益上限 %dx -> %s（暗光宁可欠曝也不再拉高增益）\n\n",
                   v, esp_err_to_name(e));
        }
        return true;
    }
    if (strcmp(cmd, "cam gray") == 0) {
        cam_jpeg_set_gray(true);
        printf("[CAM] 已切灰度。若横纹消失 -> 问题在色度通道；仍在 -> 亮度也有问题\n\n");
        return true;
    }
    if (strcmp(cmd, "cam gray off") == 0) {
        cam_jpeg_set_gray(false);
        printf("[CAM] 已恢复彩色\n\n");
        return true;
    }
    if (strcmp(cmd, "cam stop") == 0 || strcmp(cmd, "cam off") == 0) {
        do_cam_stop();
        return true;
    }
    if (strcmp(cmd, "wifi knock") == 0) {
        /* 自测自动重连：踢掉当前连接，然后盯 15 秒看板子能不能自己回来。
         * 真实掉线（AP 重启/被踢/信号瞬断）走的也是这条路径。 */
        esp_err_t e = wifi_lab_sta_kick(s_lab);
        printf("[WIFI] 已主动断开 (%s)，观察自动重连…\n", esp_err_to_name(e));
        uint32_t ip = 0;
        /* 第一步：等 netif 上的 IP 真的被清掉，否则旧地址还在，
         * 立刻查就能"查到"，会误判成重连成功。 */
        int64_t t_kick = esp_timer_get_time();
        bool cleared = false;
        while ((esp_timer_get_time() - t_kick) < 4LL * 1000 * 1000) {
            vTaskDelay(pdMS_TO_TICKS(200));
            if (!wifi_has_ip(s_lab, &ip)) {
                cleared = true;
                break;
            }
        }
        if (!cleared) {
            printf("[WIFI] 断开后 IP 没被清（4 s），无法判断重连结果\n\n");
            return true;
        }
        /* 第二步：从 IP 清空的那一刻开始计时，看多久拿回来 */
        int64_t t0 = esp_timer_get_time();
        while ((esp_timer_get_time() - t0) < 15LL * 1000 * 1000) {
            vTaskDelay(pdMS_TO_TICKS(300));
            if (wifi_has_ip(s_lab, &ip)) {
                char b[16];
                wifi_lab_ip_str(ip, b, sizeof(b));
                printf("[WIFI] 自动重连成功，用时 %.1f s，IP = %s\n\n",
                       (double)(esp_timer_get_time() - t0) / 1e6, b);
                return true;
            }
        }
        printf("[WIFI] 15 s 内没连回来 —— 自动重连有问题\n\n");
        return true;
    }
    if (strcmp(cmd, "wifi join") == 0) {
        do_wifi_join(NULL, NULL);
        return true;
    }
    if (strncmp(cmd, "wifi join ", 10) == 0) {
        /* wifi join <ssid> [pass]，密码里可能有空格，所以整体当密码前的部分 */
        const char *arg = cmd + 10;
        const char *sp = strchr(arg, ' ');
        char ssid[33] = { 0 };
        char pass[65] = { 0 };
        if (sp == NULL) {
            snprintf(ssid, sizeof(ssid), "%s", arg);
            do_wifi_join(ssid, NULL);
        } else {
            size_t n = (size_t)(sp - arg);
            if (n >= sizeof(ssid)) {
                n = sizeof(ssid) - 1;
            }
            memcpy(ssid, arg, n);
            snprintf(pass, sizeof(pass), "%s", sp + 1);
            do_wifi_join(ssid, pass);
        }
        return true;
    }
    if (strcmp(cmd, "wifi ip") == 0) {
        if (s_lab == NULL) {
            printf("[WIFI] 尚未初始化\n");
            return true;
        }
        uint32_t ip = 0;
        if (wifi_has_ip(s_lab, &ip)) {
            char buf[16];
            wifi_lab_ip_str(ip, buf, sizeof(buf));
            printf("[WIFI] STA IP = %s\n", buf);
        } else {
            printf("[WIFI] 未连接\n");
        }
        char v6[64];
        if (wifi_lab_sta_ip6(s_lab, v6, sizeof(v6)) != NULL) {
            printf("[WIFI] IPv6   = %s\n", v6);
            printf("[WIFI] 外网地址 http://[%s]/\n", v6);
        } else {
            printf("[WIFI] IPv6   = 无（路由器没发 RA，或 IPv6 没开）\n");
        }
#if CONFIG_LWIP_IPV6
        /* 诊断：把接口上所有 IPv6 地址原样列出来，并试着手工补建链路本地地址。
         * 只有 fe80:: 说明 RA 没被接收/没带前缀；一个都没有说明链路本地没建起来。 */
        if (s_lab->sta_netif != NULL) {
            esp_ip6_addr_t a6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
            int n6 = esp_netif_get_all_ip6(s_lab->sta_netif, a6);
            printf("[WIFI] IPv6 地址数 = %d\n", n6);
            for (int i = 0; i < n6 && i < CONFIG_LWIP_IPV6_NUM_ADDRESSES; i++) {
                char t[64] = {0};
                if (inet_ntop(AF_INET6, a6[i].addr, t, sizeof(t))) {
                    printf("        [%d] %s\n", i, t);
                }
            }
            if (n6 == 0) {
                esp_err_t e6 = esp_netif_create_ip6_linklocal(s_lab->sta_netif);
                printf("[WIFI] 补建链路本地 = %s\n", esp_err_to_name(e6));
            }
        }
#endif
        return true;
    }
    return false;
}

/** 把应用命令挂进串口命令行（main.c 启动时调用一次） */
void app_cmds_register(void)
{
    tf_shell_set_hook(cmds_dispatch);
    /* 跑完会断网的用例（wifi 组里的 ap_* 和 stop）之后，由这里把网拉回来。
     * 放在 tf.c 之外是有意的：测试框架不该认识 WiFi。 */
    tf_set_net_restore_hook(net_restore_after_test);
}

void app_cmds_autostart(void)
{
    /* 开机直接把画面准备好：命令行还没人敲的时候，手机就已经能看了。
     * 任何一步失败都只在串口打印原因，不阻塞后面的命令行。 */
    (void)cmds_dispatch("cam start");

    /* DDNS：配过就跟着开机跑，IPv6 一变就自动补报，全程不用人管。
     * 刚开机时 IPv6 通常还没 SLAAC 完成，任务里会每 60 s 重试。 */
    ddns_init();
    const char *u = ddns_public_url();
    if (u != NULL) {
        printf("[DDNS] 固定网址 %s（域名还没解析到板子时先看 wifi ip 的地址）\n", u);
    }

    /* MQTT 推流：不需要任何入站端口，手机网页订阅 topic 就能看。
     * 局域网 http://192.168.0.105:18080/ 仍然同时可用。 */
    cam_mqtt_init();

    /* 远程命令通道：板子自己订阅 <前缀>/cmd 并执行，网页串口页因此不再
     * 需要电脑上的桥接脚本。用独立的一条连接，命令执行再慢也不卡推流。 */
    mqtt_cmd_start();

    /* 电池采样：每 3 秒读一次 AXP2101，走同一条 MQTT 连接发到 <前缀>/batt，
     * 网页「电池」页据此画曲线。必须在 cam_mqtt_init 之后启动。 */
    batt_log_start();

    /* 自检结果上报：开机那次自检在联网之前就跑完了，结果缓存在 tf.c 里。
     * 这个任务盯着"刚连上"和"结果版本变了"两个条件，满足就发一次
     * <前缀>/test，网页「测试」页因此不用点按钮也能拿到本次启动的结论。 */
    test_result_start();

    /* 自愈看门狗：摄像头连续 5 s 没有新帧就重新 init 传感器，MQTT 连续
     * 60 s 没有成功写出就拆连接重连，两者连续失效才整机重启。
     * 放在最后启动，保证前面几路服务都已经跑起来，判据才有意义。 */
    health_start();
}
