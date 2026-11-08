/**
 * @file  test_es8311.c
 * @brief ES8311 音频 CODEC 测试用例（I2C 地址 0x18）
 *
 * 测试边界（重要）：
 *   本框架不建立 I2S 时钟（MCLK/BCLK/LRCK 均未驱动），因此这里只验证
 *   **I2C 控制面**是否健康，不验证音频数据通路、采样率与主从模式。
 *   把"没有音频输出"当成芯片坏，会得到永远红的用例 —— 那不是芯片的问题。
 *
 * 递进层次：
 *   1. present —— 0x18 是否应答，句柄能否建立；
 *   2. id      —— 读芯片 ID / 版本寄存器（只报值 + 排除"读了但全是 0xFF"）；
 *   3. rw      —— DAC/ADC 音量寄存器写回原值，验证写通路且不改状态；
 *   4. power   —— REG00 的 CSM 上电位做一次 1->0->1 翻转，证明写入真的生效；
 *   5. clk     —— 读时钟管理器寄存器组，核对连续两次读取一致；
 *   6. dump    —— 0x00-0xFF 原始转储，人工核对用。
 *
 * 关于 chip id 为什么不写死期望值：官方 esp_codec_dev 驱动从不校验 0xFD/0xFE/0xFF
 * 的内容，也没有任何权威资料给出固定值。凭空写一个 0x83 之类的"期望值"，
 * 只会在换了批次/版本时误判为坏片，属于编数据。
 */
#include <inttypes.h>
#include <stdio.h>

#include "tf.h"

#include "board.h"
#include "board_config.h"
#include "bus_i2c.h"
#include "es8311.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 取句柄；未就绪时统一跳过后续用例。
 * 用宏而不是辅助函数：TF_SKIP 的语义是"从当前用例返回"。 */
#define REQUIRE_CODEC(var)                                                     \
    es8311_t *(var) = board_codec();                                           \
    if ((var) == NULL) {                                                       \
        TF_SKIP("ES8311 不可用（0x%02X 无应答），跳过", BOARD_ADDR_ES8311);    \
    }

/* 需要把寄存器号/器件名带进失败信息时的断言。
 * TF_CHECK_ERR 只接受固定的 what 字符串，无法格式化，所以这里自备一个。 */
#define CHECK_ERR_AT(expr, what, reg)                                          \
    do {                                                                       \
        esp_err_t _e = (expr);                                                 \
        g_tf->checks++;                                                        \
        if (_e != ESP_OK) {                                                    \
            tf_fail(__FILE__, __LINE__, "%s 寄存器 0x%02X 访问失败: %s",       \
                    (what), (unsigned)(reg), esp_err_to_name(_e));             \
            return TF_FAIL;                                                    \
        }                                                                      \
    } while (0)

/* ---------------- 1. 器件存在性 ---------------- */

static tf_result_t t_present(void)
{
    const bus_i2c_t *bus = board_bus();
    TF_CHECK(bus != NULL, "I2C 总线未建立");

    int sda = -1, scl = -1;
    board_bus_pins(&sda, &scl);

    bool ack = bus_i2c_probe((bus_i2c_t *)bus, BOARD_ADDR_ES8311);
    TF_INFO("地址 0x%02X @ SDA=GPIO%d/SCL=GPIO%d -> %s",
            BOARD_ADDR_ES8311, sda, scl, ack ? "ACK" : "NACK");

    /* 本组的被测对象，缺失按 FAIL 报；否则一块没贴 CODEC 的板子会被判"通过" */
    TF_CHECK(ack, "ES8311 在 0x%02X 无应答（未贴片 / 未上电 / 引脚错）",
             BOARD_ADDR_ES8311);
    TF_CHECK(board_codec() != NULL, "器件应答但句柄建立失败");
    return TF_PASS;
}

/* ---------------- 2. 芯片 ID 与版本 ---------------- */

static tf_result_t t_chip_id(void)
{
    REQUIRE_CODEC(d);

    uint8_t id1 = 0, id2 = 0, ver = 0;
    TF_CHECK_ERR(es8311_read8(d, ES8311_REG_ID1_FD, &id1), "读 CHIP_ID1(0xFD)");
    TF_CHECK_ERR(es8311_read8(d, ES8311_REG_ID2_FE, &id2), "读 CHIP_ID2(0xFE)");
    TF_CHECK_ERR(es8311_read8(d, ES8311_REG_VER_FF, &ver), "读 VERSION(0xFF)");

    TF_INFO("CHIP_ID1(0xFD)=0x%02X  CHIP_ID2(0xFE)=0x%02X  VERSION(0xFF)=0x%02X",
            id1, id2, ver);
    TF_INFO("（官方驱动不校验这三个值，故只报实测值，请对照数据手册判断）");

    /* 器件能 ACK、读却拿回三个 0xFF，通常是"地址应答了但数据没真正传回来"
     * （典型如总线上拉不足、器件处于未初始化态）。这个结论要报出来。 */
    TF_CHECK(!(id1 == 0xFF && id2 == 0xFF && ver == 0xFF),
             "三个 ID 寄存器全为 0xFF，疑似读到的不是有效数据（查上拉/供电）");
    return TF_PASS;
}

/* ---------------- 3. 写通路（写回原值） ---------------- */

static tf_result_t t_rw(void)
{
    REQUIRE_CODEC(d);

    static const struct {
        uint8_t     reg;
        const char *name;
    } items[] = {
        { ES8311_REG_DAC_VOL, "DAC 音量(0x32)" },
        { ES8311_REG_ADC_VOL, "ADC 音量(0x17)" },
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        uint8_t before = 0, after = 0;

        CHECK_ERR_AT(es8311_read8(d, items[i].reg, &before), items[i].name,
                     items[i].reg);
        /* 关键：把读到的值原样写回。写通路正常则读回值不变；
         * 写通路坏或器件只读则读回值变化 / 报错。
         * 这样既验证了写，又保证音量设置不被意外改动。 */
        CHECK_ERR_AT(es8311_write8(d, items[i].reg, before), items[i].name,
                     items[i].reg);
        vTaskDelay(pdMS_TO_TICKS(3));
        CHECK_ERR_AT(es8311_read8(d, items[i].reg, &after), items[i].name,
                     items[i].reg);

        TF_INFO("%s 原值=0x%02X 写回后=0x%02X", items[i].name, before, after);
        TF_CHECK(before == after, "%s 写回原值后读回不一致：0x%02X -> 0x%02X",
                 items[i].name, before, after);
    }
    return TF_PASS;
}

/* ---------------- 4. REG00 状态机（CSM 上电位翻转） ---------------- */

static tf_result_t t_power(void)
{
    REQUIRE_CODEC(d);

    uint8_t orig = 0, v = 0, back = 0;
    TF_CHECK_ERR(es8311_read8(d, ES8311_REG_RESET00, &orig), "读 REG00 原值");
    TF_INFO("REG00 原值=0x%02X（CSM 上电=%d，模式=%s）", orig,
            (orig & ES8311_REG00_CSM_POWER) ? 1 : 0,
            (orig & ES8311_REG00_MASTER) ? "主" : "从");

    /* 若当前是掉电态，先置起 CSM，保证下一步能验证"1 -> 0"的翻转；
     * 否则这一步会退化成"0 -> 0"，什么都证明不了。 */
    if ((orig & ES8311_REG00_CSM_POWER) == 0) {
        esp_err_t werr = es8311_write8(d, ES8311_REG_RESET00,
                                       ES8311_REG00_POWER_ON |
                                           (orig & ES8311_REG00_MASTER));
        vTaskDelay(pdMS_TO_TICKS(5));
        uint8_t v1 = 0;
        esp_err_t rerr = es8311_read8(d, ES8311_REG_RESET00, &v1);
        if (werr != ESP_OK || rerr != ESP_OK ||
            (v1 & ES8311_REG00_CSM_POWER) == 0) {
            /* 先尽力恢复原值再报错：TF_CHECK 失败会直接 return，
             * 把恢复放在断言之后等于永远不执行。 */
            es8311_write8(d, ES8311_REG_RESET00, orig);
            g_tf->checks++;
            tf_fail(__FILE__, __LINE__,
                    "置起 CSM 失败（写:%s 读:%s 值=0x%02X）",
                    esp_err_to_name(werr), esp_err_to_name(rerr), v1);
            return TF_FAIL;
        }
        TF_INFO("置起 CSM 后 REG00=0x%02X", v1);
    }

    /* 官方驱动用 0x1F 表示掉电/复位态，bit7=0 */
    esp_err_t down_err = es8311_write8(d, ES8311_REG_RESET00,
                                       ES8311_REG00_POWER_DOWN);
    vTaskDelay(pdMS_TO_TICKS(5));
    esp_err_t rd_err = es8311_read8(d, ES8311_REG_RESET00, &v);

    /* 先恢复，再断言：保证任何一条断言失败都不会把芯片留在掉电态 */
    esp_err_t up_err = es8311_write8(d, ES8311_REG_RESET00, orig);
    vTaskDelay(pdMS_TO_TICKS(5));
    esp_err_t bk_err = es8311_read8(d, ES8311_REG_RESET00, &back);

    TF_CHECK_ERR(down_err, "写 0x1F 进入掉电态");
    TF_CHECK_ERR(rd_err, "读回 REG00（掉电后）");
    TF_INFO("写 0x1F 后 REG00=0x%02X", v);
    TF_CHECK((v & ES8311_REG00_CSM_POWER) == 0,
             "写 0x1F 后 CSM 位仍为 1，说明写入没有真正生效，实际 0x%02X", v);

    TF_CHECK_ERR(up_err, "恢复 REG00 原值");
    TF_CHECK_ERR(bk_err, "读回 REG00（恢复后）");
    TF_INFO("恢复后 REG00=0x%02X（原值 0x%02X）", back, orig);
    TF_CHECK((back & ES8311_REG00_CSM_POWER) == (orig & ES8311_REG00_CSM_POWER),
             "恢复后 CSM 位与原文不符：0x%02X -> 0x%02X", orig, back);
    return TF_PASS;
}

/* ---------------- 5. 时钟管理器寄存器 ---------------- */

static tf_result_t t_clk(void)
{
    REQUIRE_CODEC(d);

    uint8_t first[8] = { 0 }, second[8] = { 0 };
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t reg = (uint8_t)(ES8311_REG_CLK01 + i);
        CHECK_ERR_AT(es8311_read8(d, reg, &first[i]), "时钟", reg);
    }
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t reg = (uint8_t)(ES8311_REG_CLK01 + i);
        CHECK_ERR_AT(es8311_read8(d, reg, &second[i]), "时钟", reg);
    }

    printf("        | CLK 0x01-0x08:");
    for (uint8_t i = 0; i < 8; i++) {
        printf(" %02X", first[i]);
    }
    printf("\n");

    /* 两次读取必须一致：不一致说明总线/器件不稳定，比任何数值判断都关键 */
    for (uint8_t i = 0; i < 8; i++) {
        TF_CHECK(first[i] == second[i],
                 "时钟寄存器 0x%02X 两次读取不一致：0x%02X / 0x%02X",
                 (unsigned)(ES8311_REG_CLK01 + i), first[i], second[i]);
    }
    return TF_PASS;
}

/* ---------------- 6. 寄存器转储 ---------------- */

static tf_result_t t_dump(void)
{
    REQUIRE_CODEC(d);
    es8311_dump(d);
    return TF_PASS;
}

/* ---------------- 用例注册 ---------------- */
TF_GROUP_BEGIN(es8311)
    TF_ITEM(es8311, present, "I2C 地址 0x18 应答与句柄建立", t_present, TF_FLAG_HW),
    TF_ITEM(es8311, chip_id, "读取芯片 ID/版本寄存器", t_chip_id, TF_FLAG_HW),
    TF_ITEM(es8311, rw, "DAC/ADC 音量写通路（写回原值，不改状态）", t_rw, TF_FLAG_HW),
    TF_ITEM(es8311, power, "REG00 CSM 上电位翻转并恢复", t_power, TF_FLAG_HW),
    TF_ITEM(es8311, clk, "时钟管理器寄存器可读且两次一致", t_clk, TF_FLAG_HW),
    TF_ITEM(es8311, dump, "原始寄存器转储 0x00-0xFF", t_dump, TF_FLAG_HW),
TF_GROUP_END(es8311)
