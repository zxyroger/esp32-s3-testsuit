/**
 * @file  main.c
 * @brief 板级测试框架入口
 *
 * 上电流程：
 *   1. 打印板卡信息与框架用法；
 *   2. 自动跑一遍全部用例（受测项一目了然，不用敲命令）；
 *   3. 进入命令行，可重复跑单项 / 分组。
 *
 * 加新芯片测试：写 <chip>.h/.c 驱动 + test_<chip>.c，
 * 在 CMakeLists 里加源文件即可，注册表靠 constructor 自动收集。
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "tf.h"
#include "app_cmds.h"
#include "board_config.h"

static const char *TAG = "main";

/* 所有测试分组集中在这里声明。
 * 新增一颗芯片的测试 = 写 test_<chip>.c（内含 TF_GROUP_BEGIN/END）
 *                    + 在下面 X(...) 里加一行 + 在 CMakeLists 加源文件。 */
#define TF_GROUPS(X) \
    X(core)          \
    X(i2c)           \
    X(axp2101)       \
    X(es8311)        \
    X(audio)         \
    X(lcd)           \
    X(camera)        \
    X(wifi)

#define TF_DECLARE(group) extern const tf_case_t *tf_group_##group(size_t *count);
TF_GROUPS(TF_DECLARE)
#undef TF_DECLARE

#define TF_COLLECT(group)                                     \
    do {                                                      \
        size_t _n = 0;                                        \
        const tf_case_t *_c = tf_group_##group(&_n);           \
        tf_register_group(_c, _n);                            \
    } while (0);

static void register_all_groups(void)
{
    TF_GROUPS(TF_COLLECT)
}

void app_main(void)
{
    /* USB-Serial-JTAG 重新枚举后，主机侧串口终端要重新打开，
     * 这里留一点时间，避免最开头的横幅被吞掉 */
    vTaskDelay(pdMS_TO_TICKS(300));

    printf("\n\n");
    printf("============================================================\n");
    printf(" ESP32-S3 板级测试框架  (board: chatbot v1.0 / ESP32-S3-WROOM)\n");
    printf(" 芯片测试: AXP2101 PMIC @ I2C 0x34, SDA=GPIO1 SCL=GPIO2\n");
    printf("           ES8311  CODEC @ I2C 0x18（I2C 控制面）\n");
    printf(" 音频通路: I2S MCLK=38 BCLK=14 WS=13 DOUT=45 DIN=12, PA_EN=47\n");
    printf(" 液晶屏  : ILI9342C 320x240 SPI2 SCK=41 MOSI=40 CS=21 DC=39 BL=42\n");
    printf(" 摄像头  : OV2640 DVP 8bit, XCLK=8 PCLK=16 VSYNC=3 HREF=46 PWDN=48\n");
    printf("           供电经 AXP2101: ALDO2=2.8V BLDO1=2.8V BLDO2=1.2V\n");
    printf(" 无线     : WiFi STA(%s) + SoftAP 兜底\n", BOARD_WIFI_STA_SSID);
    printf("------------------------------------------------------------\n");
    printf(" 上电即自动跑全部用例；跑完自动联网并开摄像头网页。\n");
    printf(" 常用: list | run camera | cam | cam stop | wifi ip | json\n");
    printf("============================================================\n");

    ESP_LOGI(TAG, "开始自动测试");

    register_all_groups();
    tf_run_all();

    /* 用例跑完再开画面：wifi 组最后会 stop 掉射频，先开也会被关。
     * 注册命令钩子放在这里，保证命令行里能敲 cam / wifi。 */
    app_cmds_register();
    app_cmds_autostart();

    tf_shell_start();

    /* 命令行任务已接管交互，这里不需要再做周期任务。
     * app_main 返回后其栈会被回收，框架的注册表是静态存储，不受影响。 */
    ESP_LOGI(TAG, "自动测试结束，进入命令行模式");
}
