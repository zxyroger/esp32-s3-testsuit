/**
 * @file  test_camera.c
 * @brief OV2640 摄像头接口测试（本板当前未接摄像头）
 *
 * 这块板子的摄像头和 LCD 一样，存在"能证明"与"不能证明"的边界，先说清楚：
 *
 *   **供电和时钟是电学可自证的；模组在不在，只有 SCCB 应答与出厂 ID 能证明。**
 *
 *   所以分组按"证据强度"分三层：
 *   1. 片上可硬断言（不依赖模组）：AXP2101 三路 LDO 的电压与使能、
 *      XCLK 的 LEDC 频率、PWDN 电平翻转、DVP 引脚的输入配置。
 *      没接摄像头也必须 PASS —— 这几项 PASS 的含义是"板级供电与时钟没问题"，
 *      不是"摄像头能用"。
 *   2. 需要模组应答：SCCB 探测 0x30、读 PID/VER/MID。没接就 SKIP。
 *   3. 完全做不到：出图。需要 OV2640 的完整初始化寄存器表（几百条，
 *      属于 esp32-camera / esp_cam_sensor 的职责）以及帧缓冲；本板没开 PSRAM，
 *      一帧 VGA RGB565 就 153.6 KB，内部 RAM 在 WiFi 起来后只剩约 240 KB。
 *      这一层**不假装实现**，只在 dump 里如实标注缺什么。
 *
 * 供电顺序被硬件强制，测试里也把顺序本身当成断言对象：
 *     设电压（并回读核对） -> 使能 -> 等稳定 -> 起 XCLK -> 释放 PWDN -> 探测 SCCB
 * `camera.pmic_cfg` 会先关掉三路再设电压，就是为了证明"设电压时不会顺带使能"——
 * 本板冷启动时 DVDD 的电压寄存器停在 2.8 V 档，而 OV2640 的 DVDD 只要 1.2 V。
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "tf.h"

#include "board.h"
#include "board_config.h"
#include "cam_ov2640.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* DVP 全部输入脚（D0..D7 + PCLK + VSYNC + HREF），顺序固定便于逐脚报告 */
static const struct {
    int         gpio;
    const char *name;
} k_dvp_pins[] = {
    { BOARD_CAM_D0_GPIO, "D0" },     { BOARD_CAM_D1_GPIO, "D1" },
    { BOARD_CAM_D2_GPIO, "D2" },     { BOARD_CAM_D3_GPIO, "D3" },
    { BOARD_CAM_D4_GPIO, "D4" },     { BOARD_CAM_D5_GPIO, "D5" },
    { BOARD_CAM_D6_GPIO, "D6" },     { BOARD_CAM_D7_GPIO, "D7" },
    { BOARD_CAM_PCLK_GPIO, "PCLK" }, { BOARD_CAM_VSYNC_GPIO, "VSYNC" },
    { BOARD_CAM_HREF_GPIO, "HREF" },
};
#define DVP_PIN_COUNT (sizeof(k_dvp_pins) / sizeof(k_dvp_pins[0]))

static bool s_cam_ok = false;

/* ---------------- 前置就绪 ---------------- */

static axp2101_t *require_pmic(void)
{
    return board_pmic();
}

/** 只做资源准备（PWDN 高 = 掉电、SCCB 挂载），不涉及供电与 XCLK。
 *  句柄统一由 board_camera() 提供：cam_jpeg 推流模块也要用同一个实例，
 *  各自 prepare 会把 0x30 重复挂到同一条 I2C 总线上。 */
static cam_t *ensure_cam(void)
{
    cam_t *c = board_camera();
    s_cam_ok = (c != NULL);
    return c;
}

/**
 * 幂等唤醒：供电 -> XCLK -> 释放 PWDN。
 * 顺序不能改，理由见文件头。单独跑某一项时靠它补上前置。
 */
static esp_err_t ensure_awake(void)
{
    cam_t *c = ensure_cam();
    if (c == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    axp2101_t *pmic = require_pmic();
    if (pmic == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!c->power_ok) {
        esp_err_t err = cam_power_on(c, pmic, 0);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!c->xclk_ok) {
        esp_err_t err = cam_xclk_start(c);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (c->pwdn_level != 0) {
        esp_err_t err = cam_pwdn(c, false);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(BOARD_CAM_XCLK_SETTLE_MS));
    }
    return ESP_OK;
}

#define REQUIRE_PMIC(var)                                                      \
    axp2101_t *(var) = require_pmic();                                         \
    if ((var) == NULL) {                                                       \
        TF_SKIP("AXP2101 未应答，无法配置摄像头供电");                         \
    }

/* ---------------- 1. 三路 LDO 电压 ---------------- */

static tf_result_t t_pmic_cfg(void)
{
    REQUIRE_PMIC(pmic);

    /* 先记录原值：万一 ALDO2/BLDO1/BLDO2 在别的用途上还开着，
     * 这里打出来能让人一眼看出"我动了哪几路"。 */
    uint8_t raw0 = 0;
    if (axp2101_ldo_raw_onoff(pmic, &raw0) != ESP_OK) {
        TF_CHECK(false, "读 LDO 使能寄存器 0x90 失败");
    }
    TF_INFO("0x90 原值 = 0x%02X（bit1=ALDO2 bit4=BLDO1 bit5=BLDO2）", raw0);
    if ((raw0 & 0x32) == 0x32) {
        /* 实测本板上电即 0x77，三路默认已使能。这条必须在报告里点明，
         * 否则"先设电压再使能"的顺序会被误以为已经足够安全。 */
        TF_INFO("注意：上电默认三路即已使能（0x%02X 含 bit1|bit4|bit5），", raw0);
        TF_INFO("所以必须先断电再改电压 —— 带电改 DVDD 会从 2.8V 直接跳到 1.2V");
    }

    /* 关掉三路，从确定状态开始 —— 这样下面能硬断言"设电压没有顺带使能" */
    cam_t *c = ensure_cam();
    if (c == NULL) {
        TF_SKIP("SCCB/PWDN 资源准备失败，无法继续");
    }
    TF_CHECK_ERR(cam_power_off(c, pmic), "先关闭摄像头三路 LDO");

    /* 逐路设电压。axp2101_ldo_set_mv 内部已做"写后回读"校验，
     * 这里再独立读一次，是为了在报告里留下实测值而不只是"成功"。 */
    struct {
        uint8_t     reg;
        uint16_t    mv;
        const char *name;
    } rails[] = {
        { AXP2101_REG_ALDO2_VOL, BOARD_CAM_PMIC_ALDO2_MV, "ALDO2" },
        { AXP2101_REG_BLDO1_VOL, BOARD_CAM_PMIC_BLDO1_MV, "BLDO1" },
        { AXP2101_REG_BLDO2_VOL, BOARD_CAM_PMIC_BLDO2_MV, "BLDO2" },
    };

    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
        esp_err_t err = axp2101_ldo_set_mv(pmic, rails[i].reg, rails[i].mv);
        if (err != ESP_OK) {
            TF_CHECK(false, "%s 设为 %u mV 失败: %s", rails[i].name, rails[i].mv,
                     esp_err_to_name(err));
        }
        uint16_t back = 0;
        if (axp2101_ldo_get_mv(pmic, rails[i].reg, &back) != ESP_OK) {
            TF_CHECK(false, "回读 %s 电压失败", rails[i].name);
        }
        TF_INFO("%s = %u mV（目标 %u mV，寄存器 0x%02X）", rails[i].name,
                (unsigned)back, rails[i].mv, rails[i].reg);
        TF_CHECK(back == rails[i].mv, "%s 回读 %u mV，与目标 %u mV 不符",
                 rails[i].name, (unsigned)back, rails[i].mv);
    }

    /* 关键断言：只设电压，绝不能让 LDO 顺手打开。
     * 若这里就使能了，OV2640 的 DVDD 会以旧档位（本板冷启动是 2.8 V）上电。 */
    bool on = true;
    if (cam_power_is_on(pmic, &on) != ESP_OK) {
        TF_CHECK(false, "读三路使能状态失败");
    }
    uint8_t raw1 = 0;
    axp2101_ldo_raw_onoff(pmic, &raw1);
    TF_INFO("设完电压后 0x90 = 0x%02X，三路使能状态 = %s", raw1,
            on ? "已使能" : "未使能");
    TF_CHECK(!on, "设电压阶段三路已被使能（0x90=0x%02X），"
                  "存在以错误电压上电的风险", raw1);

    TF_INFO("顺序已验证：三路电压就位，此时仍处于关断");
    return TF_PASS;
}

/* ---------------- 2. 三路 LDO 使能 ---------------- */

static tf_result_t t_pmic_on(void)
{
    REQUIRE_PMIC(pmic);

    cam_t *c = ensure_cam();
    if (c == NULL) {
        TF_SKIP("SCCB/PWDN 资源准备失败，无法继续");
    }

    /* 走完整的上电流程（内部会再核对一遍电压，电压不符就拒绝使能） */
    esp_err_t err = cam_power_on(c, pmic, 0);
    TF_CHECK_ERR(err, "摄像头三路上电（设电压 -> 使能 -> 稳定）");

    uint16_t a = 0, b = 0, d = 0;
    TF_CHECK_ERR(cam_power_read_mv(pmic, &a, &b, &d), "读回三路电压");

    bool on = false;
    TF_CHECK_ERR(cam_power_is_on(pmic, &on), "读三路使能状态");

    uint8_t raw = 0;
    axp2101_ldo_raw_onoff(pmic, &raw);

    TF_INFO("ALDO2=%u mV  BLDO1=%u mV  BLDO2=%u mV", (unsigned)a, (unsigned)b,
            (unsigned)d);
    TF_INFO("0x90 = 0x%02X（期望 bit1|bit4|bit5 = 0x32 已置位），等待 %d ms",
            raw, BOARD_CAM_POWER_SETTLE_MS);
    TF_INFO("电压核对：VDDCAM_3V3 应 3.3V 档附近（本板取 2.8V）。");

    TF_CHECK(on, "三路 LDO 未全部使能（0x90=0x%02X）", raw);
    TF_CHECK((raw & 0x32) == 0x32, "0x90 = 0x%02X，ALDO2/BLDO1/BLDO2 未全部打开",
             raw);
    TF_CHECK(a == BOARD_CAM_PMIC_ALDO2_MV && b == BOARD_CAM_PMIC_BLDO1_MV &&
                 d == BOARD_CAM_PMIC_BLDO2_MV,
             "使能后电压漂移：ALDO2=%u BLDO1=%u BLDO2=%u", (unsigned)a,
             (unsigned)b, (unsigned)d);
    return TF_PASS;
}

/* ---------------- 3. XCLK ---------------- */

static tf_result_t t_xclk(void)
{
    cam_t *c = ensure_cam();
    if (c == NULL) {
        TF_SKIP("SCCB/PWDN 资源准备失败，无法继续");
    }

    /* 先确保已上电：传感器要在有电的前提下拿时钟才有意义 */
    esp_err_t err = ensure_awake();
    TF_CHECK_ERR(err, "供电 -> XCLK -> 释放 PWDN 全流程");

    uint32_t hz = c->xclk_hz;
    TF_INFO("XCLK = GPIO%d，LEDC TIMER_%d/CH%d，%d bit 分辨率",
            BOARD_CAM_XCLK_GPIO, (int)BOARD_CAM_LEDC_TIMER,
            (int)BOARD_CAM_LEDC_CHANNEL, BOARD_CAM_XCLK_PWM_BITS);
    TF_INFO("目标 %u Hz，实测 %u Hz", (unsigned)BOARD_CAM_XCLK_HZ, (unsigned)hz);

    /* 允许 5% 偏差：LEDC 只能整数分频，3 bit 分辨率下 80/8 正好是整数，
     * 但如果 APB 被改频（DFS），实际值会跟着变，这里给出容差而不是死等 10M。 */
    uint32_t lo = BOARD_CAM_XCLK_HZ / 100 * 95;
    uint32_t hi = BOARD_CAM_XCLK_HZ / 100 * 105;
    TF_CHECK(hz >= lo && hz <= hi,
             "XCLK 实测 %u Hz 偏离目标 %u Hz 超过 5%%（分频比非整数会让 SCCB 出错）",
             (unsigned)hz, (unsigned)BOARD_CAM_XCLK_HZ);

    TF_INFO("为什么要 10 MHz 而不是 20/24 MHz：24 MHz 分频比 1.667 非整数，");
    TF_INFO("SCCB 读数据会出错（地址 ACK 但 PID 乱码）；20 MHz 出图有横纹。");
    return TF_PASS;
}

/* ---------------- 4. PWDN ---------------- */

static tf_result_t t_pwdn(void)
{
    cam_t *c = ensure_cam();
    if (c == NULL) {
        TF_SKIP("SCCB/PWDN 资源准备失败，无法继续");
    }
    if (BOARD_CAM_PWDN_GPIO < 0) {
        TF_SKIP("本板 PWDN 未引出（GPIO 定义为 -1）");
    }

    esp_err_t err = ensure_awake();
    TF_CHECK_ERR(err, "供电与 XCLK 就绪");

    /* 上电后应为工作态（拉低）；再拉高进掉电，最后拉回工作。
     * 用读回电平自证，不能只信"设了就算设上了"。 */
    TF_CHECK_ERR(cam_pwdn(c, false), "PWDN 拉低（工作）");
    int lv_run = -1;
    TF_CHECK_ERR(cam_pwdn_read(c, &lv_run), "读回 PWDN 电平（工作态）");

    TF_CHECK_ERR(cam_pwdn(c, true), "PWDN 拉高（掉电）");
    int lv_off = -1;
    TF_CHECK_ERR(cam_pwdn_read(c, &lv_off), "读回 PWDN 电平（掉电态）");

    /* 恢复到工作态，后面的 SCCB 探测要靠它 */
    TF_CHECK_ERR(cam_pwdn(c, false), "PWDN 恢复为工作态");
    vTaskDelay(pdMS_TO_TICKS(BOARD_CAM_XCLK_SETTLE_MS));

    TF_INFO("PWDN = GPIO%d：工作态读回 %d，掉电态读回 %d", BOARD_CAM_PWDN_GPIO,
            lv_run, lv_off);
    TF_INFO("OV2640 的 PWDN 高电平掉电、低电平工作（与 PA 的 NS4150B 正好相反）");

    TF_CHECK(lv_run == 0, "工作态 PWDN 读回 %d（应为 0），GPIO%d 可能被占用",
             lv_run, BOARD_CAM_PWDN_GPIO);
    TF_CHECK(lv_off == 1, "掉电态 PWDN 读回 %d（应为 1）", lv_off);
    return TF_PASS;
}

/* ---------------- 5. DVP 数据引脚 ---------------- */

static tf_result_t t_dvp_pins(void)
{
    int configured = 0;
    char levels[96];
    int pos = 0;
    int stuck_high = 0;

    for (size_t i = 0; i < DVP_PIN_COUNT; i++) {
        int gpio = k_dvp_pins[i].gpio;
        if (gpio < 0) {
            continue;
        }
        gpio_config_t io;
        memset(&io, 0, sizeof(io));
        io.pin_bit_mask = 1ULL << gpio;
        io.mode = GPIO_MODE_INPUT;
        /* 下拉：悬空时读到 0，这样"读到 1"就确实能说明有外部驱动，
         * 不会和"浮空随机值"混为一谈。 */
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        io.pull_down_en = GPIO_PULLDOWN_ENABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&io) != ESP_OK) {
            TF_CHECK(false, "%s(GPIO%d) 配置为输入失败，可能被其他外设占用",
                     k_dvp_pins[i].name, gpio);
        }
        configured++;

        int lv = gpio_get_level(gpio);
        if (lv) {
            stuck_high++;
        }
        if (pos < (int)sizeof(levels) - 12) {
            pos += snprintf(levels + pos, sizeof(levels) - pos, "%.8s=%d ",
                            k_dvp_pins[i].name, lv);
        }
    }

    TF_INFO("已把 %d 个 DVP 脚配成输入并下拉：%s", configured, levels);
    TF_CHECK(configured == (int)DVP_PIN_COUNT,
             "只配置成功 %d/%d 个 DVP 引脚", configured, (int)DVP_PIN_COUNT);

    /* 没接模组时全部应为 0；接了模组时 PCLK/HREF/VSYNC 有活动可能读到 1，
     * 因此这里只报告不判失败，避免接上摄像头反而报错。 */
    if (stuck_high > 0) {
        TF_INFO("有 %d 个脚读到高电平：若已接模组属正常（数据线被驱动），"
                "未接模组则说明该脚上有上拉或被别处占用", stuck_high);
    } else {
        TF_INFO("全部读到 0：与「未接模组 + 内部下拉」一致");
    }

    TF_INFO("DVP 只在控制器里被采样，本组不建 DVP 控制器（理由见 cam.dump）");
    return TF_PASS;
}

/* ---------------- 6. SCCB 探测 ---------------- */

static tf_result_t t_sccb(void)
{
    esp_err_t err = ensure_awake();
    if (err != ESP_OK) {
        TF_SKIP("前级（供电/时钟）未就绪: %s", esp_err_to_name(err));
    }

    if (!cam_sccb_present(ensure_cam())) {
        TF_SKIP("SCCB 0x%02X 无应答：模组未接，或供电/时钟未到位",
                BOARD_CAM_SCCB_ADDR);
    }

    TF_INFO("SCCB 0x%02X 应答正常（I2C0，SDA=GPIO%d SCL=GPIO%d）",
            BOARD_CAM_SCCB_ADDR, BOARD_I2C0_SDA_GPIO, BOARD_I2C0_SCL_GPIO);

    /* 能读到一个非全 0/全 FF 的寄存器值，说明数据线真的能回读，
     * 而不只是地址 ACK。0xFF 是 bank 切换寄存器，读回应为 0x00 或 0x01。 */
    uint8_t bank = 0xFF;
    TF_CHECK_ERR(cam_reg_read(ensure_cam(), 0xFF, &bank), "读 bank 选择寄存器 0xFF");
    TF_INFO("当前 bank = 0x%02X（0=DSP，1=sensor）", bank);
    TF_CHECK(bank == 0x00 || bank == 0x01,
             "bank 寄存器读回 0x%02X，SCCB 数据线可能有问题（地址能 ACK 但数据不对）",
             bank);
    return TF_PASS;
}

/* ---------------- 7. 出厂 ID ---------------- */

static tf_result_t t_read_id(void)
{
    esp_err_t err = ensure_awake();
    if (err != ESP_OK) {
        TF_SKIP("前级（供电/时钟）未就绪: %s", esp_err_to_name(err));
    }
    if (!cam_sccb_present(ensure_cam())) {
        TF_SKIP("SCCB 无应答，无法读 ID（模组未接）");
    }

    cam_id_t id;
    memset(&id, 0, sizeof(id));
    err = cam_read_id(ensure_cam(), &id);
    if (err != ESP_OK) {
        TF_CHECK(false, "读 OV2640 出厂 ID 失败: %s", esp_err_to_name(err));
    }

    TF_INFO("PID=0x%02X VER=0x%02X MIDH=0x%02X MIDL=0x%02X（sensor bank）",
            id.pid, id.ver, id.midh, id.midl);
    TF_INFO("OV2640 期望：PID=0x%02X VER=0x%02X MIDH=0x%02X MIDL=0x%02X",
            BOARD_CAM_PID, BOARD_CAM_VER, BOARD_CAM_MIDH, BOARD_CAM_MIDL);

    /* 这四项是出厂固化的常量，读对就说明"模组在、供电对、SCCB 通、时钟对"
     * 四件事同时成立 —— 是全组最强的一条证据。 */
    TF_CHECK(id.pid == BOARD_CAM_PID,
             "PID=0x%02X 不是 OV2640 的 0x%02X（读成 0x00/0xFF 多为供电或时钟问题）",
             id.pid, BOARD_CAM_PID);
    TF_CHECK(id.midh == BOARD_CAM_MIDH && id.midl == BOARD_CAM_MIDL,
             "厂商 ID = 0x%02X%02X，OmniVision 应为 0x%02X%02X", id.midh, id.midl,
             BOARD_CAM_MIDH, BOARD_CAM_MIDL);

    TF_INFO("结论：模组在位、供电正确、SCCB 与 XCLK 都通了");
    return TF_PASS;
}

/* ---------------- 8. 汇总 ---------------- */

static tf_result_t t_dump(void)
{
    REQUIRE_PMIC(p);

    uint16_t a = 0, b = 0, d = 0;
    bool on = false;
    cam_power_read_mv(p, &a, &b, &d);
    cam_power_is_on(p, &on);
    uint8_t raw = 0;
    axp2101_ldo_raw_onoff(p, &raw);

    TF_INFO("传感器   : OV2640，DVP 8 位并口，SCCB 地址 0x%02X",
            BOARD_CAM_SCCB_ADDR);
    TF_INFO("供电     : ALDO2=%u mV / BLDO1=%u mV / BLDO2=%u mV，%s（0x90=0x%02X）",
            (unsigned)a, (unsigned)b, (unsigned)d, on ? "已使能" : "已关闭", raw);
    TF_INFO("DVP 管脚 : D0..D7=%d/%d/%d/%d/%d/%d/%d/%d", BOARD_CAM_D0_GPIO,
            BOARD_CAM_D1_GPIO, BOARD_CAM_D2_GPIO, BOARD_CAM_D3_GPIO,
            BOARD_CAM_D4_GPIO, BOARD_CAM_D5_GPIO, BOARD_CAM_D6_GPIO,
            BOARD_CAM_D7_GPIO);
    TF_INFO("           PCLK=%d VSYNC=%d HREF=%d PWDN=%d RESET=%d",
            BOARD_CAM_PCLK_GPIO, BOARD_CAM_VSYNC_GPIO, BOARD_CAM_HREF_GPIO,
            BOARD_CAM_PWDN_GPIO, BOARD_CAM_RESET_GPIO);
    TF_INFO("时钟     : XCLK=GPIO%d，%u Hz（LEDC 产生，非 LCD_CAM）",
            BOARD_CAM_XCLK_GPIO,
            (unsigned)(s_cam_ok && board_camera() ? board_camera()->xclk_hz : 0));
    TF_INFO("未覆盖   : 出图需要 OV2640 完整初始化表 + 帧缓冲；本板未开 PSRAM，");
    TF_INFO("           一帧 VGA RGB565 = 153.6 KB，故本组只验证到 SCCB/ID 层");

    TF_CHECK(on, "汇总时三路供电不是使能状态");
    return TF_PASS;
}

/* ---------------- 注册 ---------------- */

TF_GROUP_BEGIN(camera)
    TF_ITEM(camera, pmic_cfg,  "AXP2101 三路 LDO 电压设置（不使能）", t_pmic_cfg,  TF_FLAG_HW),
    TF_ITEM(camera, pmic_on,   "三路 LDO 使能与电压回读",             t_pmic_on,   TF_FLAG_HW),
    TF_ITEM(camera, xclk,      "XCLK 10 MHz（LEDC 输出）",            t_xclk,      TF_FLAG_HW),
    TF_ITEM(camera, pwdn,      "PWDN 电平控制与读回",                 t_pwdn,      TF_FLAG_HW),
    TF_ITEM(camera, dvp_pins,  "DVP 11 个数据/同步脚输入配置",        t_dvp_pins,  TF_FLAG_HW),
    TF_ITEM(camera, sccb,      "SCCB 0x30 应答与 bank 读回",          t_sccb,      TF_FLAG_HW),
    TF_ITEM(camera, read_id,   "读 OV2640 出厂 ID",                   t_read_id,   TF_FLAG_HW),
    TF_ITEM(camera, dump,      "配置汇总",                            t_dump,      TF_FLAG_HW),
TF_GROUP_END(camera)
