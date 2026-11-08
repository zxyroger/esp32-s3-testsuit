/**
 * @file  board_config.h
 * @brief 板级引脚与外设配置 —— 换板子只改这个文件
 *
 * 本板依据《chatbot v1.0 · ESP32-S3-WROOM 引脚定义》确定，绿色为板级网络名：
 *   I2C_SDA = GPIO1 (ADC1_CH0)
 *   I2C_SCL = GPIO2 (ADC1_CH1)
 *   USB_D-  = GPIO19, USB_D+ = GPIO20
 *   BOOT    = GPIO0
 *   LCD_DC / LCD_SCK / LCD_MOSI / LCD_CS = GPIO39 / GPIO41 / GPIO40 / GPIO21
 *   LCD_BL = GPIO42（低电平点亮，见下方 LCD 段）
 *   DVP_D0..D7 = GPIO7/5/4/6/15/17/18/9，PCLK = GPIO16，XCLK = GPIO8
 *   DVP_VSYNC = GPIO3，HREF = GPIO46，PWDN = GPIO48（后四个图上未标，取自量产工程）
 *   I2S_MCK = GPIO38
 *
 * 注意：GPIO35/36/37 标注为未连接；GPIO0 已作 BOOT，勿再占用。
 */
#pragma once

#include <stdint.h>

/* 私密配置（Wi-Fi 账密等）走 lab_secrets.h，真实值放在不进版本库的 secrets.h 里。 */
#include "lab_secrets.h"
#include <stdbool.h>

/* ---------------- I2C 总线 ---------------- */

#define BOARD_I2C0_SDA_GPIO   1
#define BOARD_I2C0_SCL_GPIO   2

/* 引脚已知时仍保留自动探测兜底：万一图上 SDA/SCL 标反，或换了同系列板子，
 * 不重新编译也能自证。I2C 开漏，扫错引脚不会损伤器件。 */
#define BOARD_I2C_AUTODETECT  1

/* 100 kHz 而非 400 kHz：本板 I2C 只挂慢速器件（PMIC），
 * 100k 对走线容差更宽容；且内部上拉（约 45k）在 100k 下勉强可用，
 * 便于外部上拉缺失时仍能读到数据。 */
#define BOARD_I2C0_SPEED_HZ   100000

/* 候选引脚对（SDA, SCL）
 *   1/2    本板（I2C_SDA=GPIO1, I2C_SCL=GPIO2）
 *   2/1    本板反接兜底
 *   18/8   LilyGO T-Deck
 *   10/11  LilyGO T-Watch S3
 *   21/22  T-Watch 2020 系列
 *   43/44  ESP32-S3-DevKitC 常用扩展口 */
static const int8_t BOARD_I2C_CANDIDATES[][2] = {
    {  1,  2 },
    {  2,  1 },
    { 18,  8 },
    { 10, 11 },
    { 21, 22 },
    { 43, 44 },
};
#define BOARD_I2C_CANDIDATE_COUNT \
    (sizeof(BOARD_I2C_CANDIDATES) / sizeof(BOARD_I2C_CANDIDATES[0]))

/* ---------------- 已知器件地址 ---------------- */

#define BOARD_ADDR_AXP2101    0x34   /* 7 位地址 */
#define BOARD_ADDR_ES8311     0x18   /* 音频 CODEC，7 位地址 */

/* ---------------- 音频通路（ES8311 + NS4150B + ZTS6216） ----------------
 *
 * 信号链：
 *   ESP32 I2S(主) --MCLK/BCLK/LRCK--> ES8311(从)
 *      ES8311 DAC --> NS4150B INP/INN --(D 类)--> 喇叭
 *      ZTS6216(模拟 MEMS) --隔直 0.1uF--> ES8311 MIC --> ADC --> I2S DIN
 *
 * 管脚来自《chatbot v1.0 · ESP32-S3-WROOM 引脚定义》的绿色网络名。
 * 注意：那里写的 20/21/22/26/31 是**排针脚位序号**，不是 GPIO 号 ——
 *       对应 GPIO12/13/14/45/38，别当成 GPIO 直接抄。
 *   排针 20 = GPIO12 I2S_DI、21 = GPIO13 I2S_WS、22 = GPIO14 I2S_BCK、
 *   26 = GPIO45 I2S_DO、31 = GPIO38 I2S_MCK、24 = GPIO47 PA_EN。
 *
 * 器件依据（都是查手册得到的，不是猜的）：
 *   - NS4150B  Pin1 = CTRL：低电平关断、高电平工作（VIL<=0.2V, VIH>=1.2V）；
 *              启动时间 Tst 典型 120 ms、关断 80 ms —— 使能后必须等，否则
 *              第一段音频会被静音吃掉。
 *   - ZTS6216  模拟输出 MEMS 麦克风（-38 dBV/Pa），必须走 ES8311 的模拟 MIC
 *              输入，因此 ES8311 REG14 bit6（digital_mic）要保持 0，不能用 PDM。
 */
#define BOARD_I2S_MCK_GPIO    38
#define BOARD_I2S_BCLK_GPIO   14
#define BOARD_I2S_WS_GPIO     13
#define BOARD_I2S_DOUT_GPIO   45   /* ESP32 -> ES8311 SDIN */
#define BOARD_I2S_DIN_GPIO    12   /* ES8311 SDOUT -> ESP32 */

#define BOARD_PA_EN_GPIO      47   /* NS4150B CTRL */
#define BOARD_PA_ACTIVE_HIGH  1    /* 手册：低=Shutdown、高=工作 */
#define BOARD_PA_STARTUP_MS   130  /* Tst=120ms，留 10ms 余量 */

#define BOARD_AUDIO_RATE_HZ   16000
#define BOARD_AUDIO_MCLK_MULT 256  /* 16k × 256 = 4.096 MHz，ES8311 系数表里有这一档 */

/* ---------------- LCD（ILI9342C，4 线 SPI，320x240） ----------------
 *
 * 管脚来源有两处，互相印证：
 *   1. 《chatbot v1.0 · ESP32-S3-WROOM 引脚定义》绿色网络名的
 *      LCD_DC / LCD_SCK / LCD_MOSI / LCD_BL；
 *   2. 同款板量产工程 D:\esp\ai-esp32\firmware\main\boards\chatbot-esp32-s3\config.h
 *      里的 DISPLAY_SPI_CS_PIN —— 图上没标 CS，只能从这里取，是 GPIO21。
 *
 * 四个必须知道的事实（都影响测试怎么判）：
 *
 *   1) 面板是 ILI9342C，但**用 ILI9341 的初始化序列驱动**。
 *      ILI9342C 与 ILI9341 命令集兼容，同款板量产固件就是拿
 *      esp_lcd_ili9341 + 厂家 vendor_specific_init 跑起来的。本驱动直接沿用
 *      那一套参数（这是"已经跑通过"的事实，不是照手册猜的）。
 *
 *   2) 没有硬件复位脚（reset_gpio_num = NC），只能发 0x01 软复位。
 *
 *   3) 没有 MISO —— 模块的 SDO 没引到排针上。**这条最关键**：SPI 屏通常靠
 *      读 0x04 RDDID 判断"芯片在不在"，本板读不了。所以"屏是否真的接上"
 *      在电学上无法自证，只能靠人眼看彩条。测试代码必须如实反映这一点，
 *      不能把"命令发出去了"当成"屏收到了"。
 *
 *   4) 背光是低电平点亮（量产工程 DISPLAY_BACKLIGHT_OUTPUT_INVERT = true），
 *      所以 LEDC 的 duty 越大反而越暗；duty=255 是全灭，duty=0 才是全亮。
 *
 * 引脚冲突提醒：CS 用的 GPIO21 同时出现在上方 I2C 候选表里（{21,22} 那一对）。
 * 本板 SDA/SCL=1/2 能直接命中 AXP2101，不会走到候选表分支，因此不冲突；
 * 但换板或 AXP2101 不应答时，I2C 自动探测会把 GPIO21 抢去当 SDA，
 * 那种情况下 LCD 会白屏 —— 排查时先看日志里有没有"按候选表扫描 I2C 引脚"。
 */
#define BOARD_LCD_SCLK_GPIO    41   /* SPI 时钟 */
#define BOARD_LCD_MOSI_GPIO    40   /* SPI 数据（只写） */
#define BOARD_LCD_CS_GPIO      21   /* 片选，软件控制：整条命令周期拉低 */
#define BOARD_LCD_DC_GPIO      39   /* 命令/数据选择：低=命令，高=数据 */
#define BOARD_LCD_BL_GPIO      42   /* 背光 PWM */
#define BOARD_LCD_MISO_GPIO    (-1) /* 未引出，所以无法回读 ID */
#define BOARD_LCD_RST_GPIO     (-1) /* 无硬件复位脚 */

/* 用 SPI2_HOST（枚举值 1，不是数字 2 —— SPI1_HOST 才是 0）。
 * 代码里直接写 SPI2_HOST，这里只做记录，避免有人按数字 2 去查。 */

#define BOARD_LCD_H_RES        320
#define BOARD_LCD_V_RES        240
#define BOARD_LCD_PCLK_HZ      (40 * 1000 * 1000) /* 量产工程实测值 */

/* 横屏：mirror X/Y 同时置位 = 旋转 180°，swap_xy 不用 */
#define BOARD_LCD_MIRROR_X     1
#define BOARD_LCD_MIRROR_Y     1
#define BOARD_LCD_SWAP_XY      0
#define BOARD_LCD_INVERT_COLOR 1   /* 面板需要 INVON(0x21)，否则黑白颠倒 */

#define BOARD_LCD_BL_ACTIVE_LOW 1
#define BOARD_LCD_BL_PWM_HZ     5000  /* 背光用低频 PWM 避免可见闪烁 */
#define BOARD_LCD_BL_PWM_BITS   8     /* 256 级，够细 */

/* 刷屏条带行数：320 × 40 × 2 = 25600 B，240 行正好 6 条。
 * 为什么不分一整屏 153600 B：本板没开 PSRAM，内部 RAM 还要留给 WiFi，
 * 一次要 150 KB 的 DMA 缓冲很容易分配失败。条带法只用 25 KB。 */
#define BOARD_LCD_STRIP_LINES   40

/* ---------------- WiFi（片上射频，无外部器件） ----------------
 *
 * 测试策略：不依赖外部路由器（现场没有已知 SSID/密码也能跑）。
 *   - STA 侧：主动扫描周边 AP，用"能扫到几个、RSSI 多少"证明接收通路正常；
 *   - AP 侧：自己开 SoftAP，再用自己的 STA 去扫描/关联它，构成闭环自证。
 *
 * AP 密码必须 >=8 位才允许 WPA2；低于 8 位会退化成开放网络（usable 但会打印提示）。
 * 信道选 6：落在 2.4G 常用段中部，避开 1/11 边缘，且与手机上常见的默认信道不冲突。
 */
#define BOARD_WIFI_AP_SSID_PREFIX "ESP32S3-LAB"
#define BOARD_WIFI_AP_PASS        "esp32s3lab"
#define BOARD_WIFI_AP_CHANNEL     6
#define BOARD_WIFI_AP_MAX_CONN    4
#define BOARD_WIFI_SCAN_MAX       16    /* 扫描结果缓存上限 */
/* 自环探测只作"尽力而为"：单射频芯片物理上无法自己给自己回 probe response，
 * 这个超时到了就 SKIP，不必久等。留 6 s 已经足够覆盖正常关联耗时。 */
#define BOARD_WIFI_STA_JOIN_MS    6000  /* STA 自连 SoftAP 的超时 */

/* ---------------- 家庭路由器（远端看图用） ----------------
 *
 * 为什么要连外部路由器而不是只开 SoftAP：
 *   手机开 SoftAP 直连时，PC 必须也连到这块板才能反代，而 PC 一连过来就断网，
 *   云端隧道随之断掉，"手机在任何地方都能看"就不成立。
 *   所以让板子作为 STA 加入和 PC 同一个路由器，PC 反代到它，手机走隧道看。
 *
 * 注意只能用 **2.4G**：ESP32-S3 只支持 2.4 GHz，本机路由器同时广播
 * 家里的 2.4G 与 5G 两个 SSID 同名不同后缀，填 5G 那个会一直连不上。
 *
 * SSID/密码不在本文件填，写在 main/secrets.h（不进版本库）。
 */
#define BOARD_WIFI_STA_SSID       SECRET_WIFI_SSID
#define BOARD_WIFI_STA_PASS       SECRET_WIFI_PASS
#define BOARD_WIFI_STA_TIMEOUT_MS 15000

/* ---------------- 摄像头（OV2640，DVP 8 位并口） ----------------
 *
 * 依据：同款板量产工程
 *   D:\esp\ai-esp32\firmware\main\boards\chatbot-esp32-s3\{config.h, CAMERA.md}
 * 那份工程在同一块硬件上已经跑通"拍照 + LCD 回显"，所以这里的管脚、供电、
 * 时钟全部照搬它，不按引脚图猜。
 *
 * 引脚图（《chatbot v1.0 · ESP32-S3-WROOM 引脚定义》）只标了
 *   DVP_D0..D6 / PCLK / XCLK = GPIO7/6/5/4/15/17/18 + GPIO16 / GPIO8，
 * 与量产工程的实际接线有三处差异，**以量产工程为准**：
 *   1) D1/D2/D3 的对应关系是 D1=5、D2=4、D3=6（不是图上 6/5/4 的顺序）；
 *   2) 图上没标 D7 = GPIO9，也没标 VSYNC=3 / HREF=46 / PWDN=48；
 *   3) SCCB 不单独占脚，复用 I2C0（SDA=GPIO1 / SCL=GPIO2），从机地址 0x30。
 *
 * 三条硬约束（都是从量产工程踩坑记录里来的，不照做就会误判成"模组坏了"）：
 *
 *   1) XCLK 必须由 LEDC 产生，不能用 LCD_CAM 的 cam_clk。
 *      在 S3 上 esp32-camera 把 XCLK 交给 LCD_CAM（cam_clk_sel=3，源 160M PLL），
 *      但实测该时钟并没有输出到 GPIO8：传感器拿不到时钟就完全不响应 SCCB，
 *      总线上扫不到 0x30。改用 LEDC 后立刻恢复。
 *      因此配置 DVP 控制器时 xclk_io 要设 NC / external_xtal 置 1，
 *      不能让它把 GPIO8 从 LEDC 手里抢回 LCD_CAM。
 *
 *   2) XCLK 取 10 MHz，不要 20/24 MHz。
 *      24 MHz 时 80 MHz 分频比是 1.667（非整数），输出带抖动，SCCB 读数据出错
 *      （地址能 ACK 但读回的 PID 是乱码 → "camera not supported"）；
 *      20 MHz 分频比整数、SCCB 稳，但实机出图有横纹（PCLK 由 XCLK 分频，
 *      采样余量不足）；10 MHz 分频比 8，采样余量翻倍，代价是帧率下降。
 *
 *   3) 三路供电挂在 AXP2101 上，顺序必须"先设电压、再使能"。
 *      本板冷启动时 DVDD 的电压寄存器停在 2.8 V 档，而 OV2640 的 DVDD 只要 1.2 V。
 *      若先使能，模组会以错误电压上电（可能损坏，或表现为 SCCB 探测不到）。
 *
 * LEDC 通道资源：S3 只有 8 个，LCD 背光固定占用 TIMER_0/CH0，相机固定
 * TIMER_2/CH7（与量产工程一致，把 CH1~CH6 留给舵机，本板目前没有舵机）。
 */
#define BOARD_CAM_XCLK_GPIO    8
#define BOARD_CAM_PCLK_GPIO    16
#define BOARD_CAM_VSYNC_GPIO   3
#define BOARD_CAM_HREF_GPIO    46
#define BOARD_CAM_PWDN_GPIO    48
#define BOARD_CAM_RESET_GPIO   (-1)  /* 模组 RESET 未引出 */

/* DVP 数据位 D0..D7，顺序不能乱（对调会导致图像错位但看起来"有画面"） */
#define BOARD_CAM_D0_GPIO      7
#define BOARD_CAM_D1_GPIO      5
#define BOARD_CAM_D2_GPIO      4
#define BOARD_CAM_D3_GPIO      6
#define BOARD_CAM_D4_GPIO      15
#define BOARD_CAM_D5_GPIO      17
#define BOARD_CAM_D6_GPIO      18
#define BOARD_CAM_D7_GPIO      9

/* SCCB 挂到板上已有的 I2C0，不新建总线 */
#define BOARD_CAM_SCCB_ADDR    0x30

/* OV2640 出厂 ID（读 sensor bank 的 REG 0x0A/0x0B/0x1C/0x1D） */
#define BOARD_CAM_PID          0x26
#define BOARD_CAM_VER          0x42
#define BOARD_CAM_MIDH         0x7F
#define BOARD_CAM_MIDL         0xA2

#define BOARD_CAM_XCLK_HZ      (10 * 1000 * 1000)
/* 用数字而不是 LEDC_TIMER_2 / LEDC_CHANNEL_7 枚举：本文件是纯配置头，
 * 不该为了两个宏把 driver/ledc.h 拉进所有包含者。使用处再转成枚举。 */
#define BOARD_CAM_LEDC_TIMER   2   /* = LEDC_TIMER_2，LCD 背光占 TIMER_0 */
#define BOARD_CAM_LEDC_CHANNEL 7   /* = LEDC_CHANNEL_7，LCD 背光占 CH0 */

/* XCLK 用 3 bit 分辨率：80 MHz / 10 MHz = 8，分频比正好为 1（无小数误差）。
 * duty 取 4/8 = 50% 方波，这是传感器要的形态。 */
#define BOARD_CAM_XCLK_PWM_BITS 3
#define BOARD_CAM_XCLK_DUTY     4

/* AXP2101 三路 LDO 目标电压（mV）。寄存器写法：电压 = 500 + N*100，N 占低 5 位 */
#define BOARD_CAM_PMIC_ALDO2_MV 2800  /* VDDCAM_3V3，摄像头 I/O */
#define BOARD_CAM_PMIC_BLDO1_MV 2800  /* AVDD，模拟 */
#define BOARD_CAM_PMIC_BLDO2_MV 1200  /* DVDD，数字内核 */

/* 使能后等 LDO 稳定：不等的话 SCCB 探测会随机失败（量产工程同样取 80 ms） */
#define BOARD_CAM_POWER_SETTLE_MS 80

/* XCLK 起振到传感器响应 SCCB 的等待：太短会探测不到 0x30 */
#define BOARD_CAM_XCLK_SETTLE_MS  10

/* 摄像头默认输出分辨率。
 *
 * 定成 VGA(640x480)：参考 ai-esp32 那套工程（它用 VGA + JPEG q9），
 * 之前用 QVGA 只有四分之一像素，是画面糊、条纹明显的主因。
 * 运行时想省带宽发 cam res qvga 即可切回 320x240（只改传感器窗口，
 * 不需要重新 init，见 cam_jpeg_set_res 的注释）。
 *
 * 注意：test_camera.c 的分层验证也用这两个值算帧大小，
 * 那组用例只验证 DVP 配置、不抓整帧，所以改成 VGA 不影响它。 */
/* 远程看图走 MQTT 出公网，默认用 QVGA。
 * 原因：VGA 一帧 40~70 KB，到 broker 的 RTT 约 300 ms、板子 TCP 有效吞吐
 * 只有 50 KB/s 上下，VGA 只能跑到 0.7 fps；QVGA 一帧 10~16 KB，能稳在 2~3 fps。
 * 局域网 http 想看大图就在串口发 cam res vga。 */
#define BOARD_CAM_H_RES        320
#define BOARD_CAM_V_RES        240
