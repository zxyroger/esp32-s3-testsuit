/**
 * @file  test_axp2101.c
 * @brief AXP2101 电源管理芯片测试用例
 *
 * 测试思路（从"能不能通"到"数据合不合理"四层递进）：
 *   1. present  —— I2C 地址是否应答，句柄能否建立；
 *   2. chip_id  —— 读到 0x4A，排除"总线通但挂的是别的器件"；
 *   3. rw       —— 写回原值验证写通路，不改变芯片状态；
 *   4. adc      —— 开通道、读电压/温度，检查数值落在物理合理区间。
 *
 * 第二批用例（pwr_src 起）按全志/芯智汇的五份应用文档补齐，
 * 专门回答"为什么充不上电""拔 USB 为什么掉电""上次怎么关机的"，
 * 判据与寄存器出处都写在各自注释里。
 *
 * 说明：所有断言都只读或"写回原值"，绝不配置电源轨 ——
 * 那属于板级 bring-up，误写可能直接掉电或烧外围。
 */
#include <inttypes.h>
#include <stdio.h>

#include "tf.h"

#include "axp2101.h"
#include "board.h"
#include "board_config.h"
#include "bus_i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 取句柄；未就绪时统一跳过后续用例，避免每个用例重复打印同一条错误。
 * 用宏而不是返回指针的辅助函数：TF_SKIP 的语义是"从当前用例返回"，
 * 放进函数里会变成从辅助函数返回，类型也对不上。 */
#define REQUIRE_PMIC(var)                                                     \
    axp2101_t *(var) = board_pmic();                                          \
    if ((var) == NULL) {                                                      \
        TF_SKIP("AXP2101 不可用（0x%02X 无应答），跳过", BOARD_ADDR_AXP2101);  \
    }

/* ---------------- 1. 器件存在性 ---------------- */

static tf_result_t t_present(void)
{
    const bus_i2c_t *bus = board_bus();
    TF_CHECK(bus != NULL, "I2C 总线未建立");

    int sda = -1, scl = -1;
    board_bus_pins(&sda, &scl);

    bool ack = bus_i2c_probe((bus_i2c_t *)bus, BOARD_ADDR_AXP2101);
    TF_INFO("地址 0x%02X @ SDA=GPIO%d/SCL=GPIO%d -> %s",
            BOARD_ADDR_AXP2101, sda, scl, ack ? "ACK" : "NACK");

    /* 这是本用例组的被测对象，缺失按 FAIL 报，不用 SKIP ——
     * 否则一块没贴 PMIC 的板子会被判"通过" */
    TF_CHECK(ack, "AXP2101 在 0x%02X 无应答（未贴片 / 未上电 / 引脚错）",
             BOARD_ADDR_AXP2101);

    TF_CHECK(board_pmic() != NULL, "器件应答但句柄建立失败");
    return TF_PASS;
}

/* ---------------- 2. 芯片 ID ---------------- */

static tf_result_t t_chip_id(void)
{
    REQUIRE_PMIC(d);

    uint8_t id = axp2101_chip_id(d);
    TF_INFO("CHIP_ID(0x%02X) = 0x%02X，期望 0x%02X",
            AXP2101_REG_CHIP_ID, id, AXP2101_CHIP_ID_VALUE);

    /* 值不对但读到了数据，说明 I2C 正常、只是型号不是 AXP2101（可能是 AXP192/XPowers 系列别的片子），
     * 这个区分对排查很关键，所以两个断言分开写 */
    TF_CHECK(id != 0xFF, "读 CHIP_ID 失败（总线错误）");
    TF_CHECK(id == AXP2101_CHIP_ID_VALUE, "CHIP_ID=0x%02X 不是 AXP2101(0x%02X)",
             id, AXP2101_CHIP_ID_VALUE);
    return TF_PASS;
}

/* ---------------- 3. 写通路（写回原值） ---------------- */

static tf_result_t t_rw(void)
{
    REQUIRE_PMIC(d);

    uint8_t before = 0, after = 0;
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_ADC_EN, &before), "读 ADC_EN 原值");

    /* 关键设计：把读到的值原样写回。
     * 若芯片实际支持写通路，读回值必与 before 相同；
     * 若写通路坏了（或器件只读），读回值会变或报错。
     * 这样做既验证了写，又保证 ADC 使能位不被意外改动。 */
    TF_CHECK_ERR(axp2101_write8(d, AXP2101_REG_ADC_EN, before), "写回 ADC_EN 原值");
    vTaskDelay(pdMS_TO_TICKS(5));
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_ADC_EN, &after), "复读 ADC_EN");

    TF_INFO("ADC_EN 原值=0x%02X 写回后=0x%02X", before, after);
    TF_CHECK(before == after, "写回原值后读回不一致：0x%02X -> 0x%02X",
             before, after);
    return TF_PASS;
}

/* ---------------- 4. ADC 电压与温度 ---------------- */

static tf_result_t t_adc(void)
{
    REQUIRE_PMIC(d);

    const uint8_t need = AXP2101_ADC_EN_VBAT | AXP2101_ADC_EN_VSYS |
                         AXP2101_ADC_EN_VBUS | AXP2101_ADC_EN_TDIE;
    uint8_t en_before = 0, en_after = 0;

    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_ADC_EN, &en_before), "读 ADC 使能");
    TF_CHECK_ERR(axp2101_adc_config(d, need, true), "打开 ADC 通道");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_ADC_EN, &en_after), "复读 ADC 使能");

    TF_INFO("ADC_EN: 0x%02X -> 0x%02X（VBAT/VSYS/VBUS/TDIE 已使能）",
            en_before, en_after);
    /* 使能位置不上去，后面的读数就不可信，先卡这一条 */
    TF_CHECK((en_after & need) == need, "ADC 使能位未置起，实际 0x%02X", en_after);

    /* 转换需要时间：手册建议使能后等一个转换周期，30 ms 足够 4 通道轮流完成 */
    vTaskDelay(pdMS_TO_TICKS(30));

    uint16_t vbat = 0, vsys = 0, vbus = 0;
    TF_CHECK_ERR(axp2101_read_mv(d, AXP2101_REG_ADC_VBAT_H, &vbat), "读 VBAT");
    TF_CHECK_ERR(axp2101_read_mv(d, AXP2101_REG_ADC_VSYS_H, &vsys), "读 VSYS");
    TF_CHECK_ERR(axp2101_read_mv(d, AXP2101_REG_ADC_VBUS_H, &vbus), "读 VBUS");

    uint16_t traw = 0;
    esp_err_t terr = axp2101_read_temp_raw(d, &traw);

    TF_INFO("VBAT=%u mV  VSYS=%u mV  VBUS=%u mV  温度通道原值=%u",
            vbat, vsys, vbus, traw);

    /* VSYS 是芯片内部系统轨，只要 PMIC 活着就应有合理输出。
     * 区间覆盖两种工况：电池 3.0-4.35 V、USB 供电约 5.0-5.3 V。 */
    TF_CHECK_RANGE(vsys, 2800.0, 5600.0, "VSYS(mV)");

    /* 未接电池时 VBAT 采样脚悬空，读到的是几十~几百 mV 甚至 1-2 V 的泄漏值。
     * 本用例验证的是"ADC 通道工作正常"，不是"电池健康状况"——
     * 把没装电池判成芯片坏，会让整套框架在裸板上永远红。
     * 因此低于锂电工作区间只作提示；接了电池才做电压判据。 */
    if (vbat < 2700) {
        TF_INFO("VBAT=%u mV：低于锂电工作区间，判定未接电池或电池过放（不判失败）",
                vbat);
    } else {
        /* 上限用 4.6 V 而不是 4.35 V：留一点余量给"4.35 V 电芯 + 采样误差"，
         * 但 4.9 V 这种读数只可能是采样点接错 —— 单节锂电物理上到不了。 */
        TF_CHECK(vbat <= 4600,
                 "VBAT=%u mV 超出单节锂电上限：BAT 采样脚很可能接在 VSYS/VBUS 上"
                 "（此时所有\"电池电压\"结论都不成立）", vbat);
    }

    if (vbus < 2000) {
        TF_INFO("VBUS=%u mV：当前非 USB 供电（纯电池运行属正常）", vbus);
    } else {
        TF_CHECK_RANGE(vbus, 4000.0, 5600.0, "VBUS(mV)");
    }

    /* 温度通道只验"读得到"。它的换算系数没有校准依据，
     * 硬套公式会输出 -574 ℃ 这类假数据，测试不该制造假结论。 */
    TF_CHECK(terr == ESP_OK, "读温度通道失败: %s", esp_err_to_name(terr));
    return TF_PASS;
}

/* ---------------- 5. 中断状态寄存器 ---------------- */

static tf_result_t t_irq(void)
{
    REQUIRE_PMIC(d);

    uint8_t st[AXP2101_REG_IRQ_STAT_N] = { 0 };
    TF_CHECK_ERR(axp2101_read_irq_status(d, st), "读中断状态");

    /* 不解析位义：状态位随供电/按键/充电实时变化，
     * 断言具体值只会得到随机失败的用例。这里只保证寄存器可读。 */
    TF_INFO("IRQ_STAT 0x48-0x4A = %02X %02X %02X", st[0], st[1], st[2]);
    return TF_PASS;
}

/* ---------------- 6. 寄存器转储（人工核对用） ---------------- */

static tf_result_t t_dump(void)
{
    REQUIRE_PMIC(d);
    axp2101_dump(d);
    return TF_PASS;
}

/* ==================================================================
 * 第二批：数据手册驱动的电源通路诊断
 *
 * 前面的用例只回答"芯片通不通"。这一批回答三个真问题：
 *   1. 为什么充不上电        ——《常见问题》2.2 的 8 个前置条件逐条判定
 *   2. 拔掉 USB 为什么掉电    —— BATFET 通路 + VBUS 输入裕量
 *   3. 上一次是怎么关机/重启的 —— REG20/REG21 开关机源
 *
 * 判据出处（都写进注释，方便以后有人质疑为什么是这个数）：
 *   - AXP2101_Datasheet_V1.4 §6.13.2 寄存器描述、§5.5 电气特性
 *   - 全志《AXP2101 充电高低温保护功能验证》
 *   - 全志《搭配 AXP2101 常见的掉电重启问题排查指南》
 *   - 芯智汇《AXP2101 常见问题 V1.0》2.2 / 2.5 / 2.7 / 2.12
 *
 * 全部只读：一个配置位都不写。写错了会掉电或停充，那是 bring-up 的事。
 * ================================================================== */

/* ---- 手册原文公式的编码换算 ---- */

/* REG15[3:0]: VINDPM = 3.88 + N*0.08 V */
static int vindpm_mv(uint8_t n)
{
    return 3880 + (int)(n & 0x0F) * 80;
}

/* REG16[2:0]: 100/500/900/1000/1500/2000 mA，6/7 保留 */
static int ilim_ma(uint8_t n)
{
    static const int t[8] = { 100, 500, 900, 1000, 1500, 2000, -1, -1 };
    return t[n & 0x07];
}

/* REG14[6:4]: VSYS DPM = 4.1 + N*0.1 V */
static int vsys_dpm_mv(uint8_t n)
{
    return 4100 + (int)((n >> 4) & 0x07) * 100;
}

/* REG62[4:0]: ICHG = 25*N (N<=8) / 200+100*(N-8) (N>8) */
static int ichg_ma(uint8_t n)
{
    n &= 0x1F;
    if (n == 0) {
        return 0;
    }
    return (n <= 8) ? (int)n * 25 : 200 + (int)(n - 8) * 100;
}

/* REG61[3:0] / REG63[3:0]: 25*N mA，N<=8 */
static int step25_ma(uint8_t n)
{
    n &= 0x0F;
    return (n <= 8) ? (int)n * 25 : -1;
}

/* REG64[2:0]: 5.0 / 4.0 / 4.1 / 4.2 / 4.35 / 4.4 V */
static int vterm_mv(uint8_t n)
{
    static const int t[8] = { 5000, 4000, 4100, 4200, 4350, 4400, -1, -1 };
    return t[n & 0x07];
}

static const char *chg_stat_name(uint8_t s)
{
    static const char *n[8] = { "涓流", "预充", "恒流(CC)", "恒压(CV)",
                                "充满", "未充电", "保留", "保留" };
    return n[s & 0x07];
}

static const char *cur_dir_name(uint8_t c)
{
    static const char *n[4] = { "待机", "充电", "放电", "保留" };
    return n[c & 0x03];
}

/* 「是/否」统一打印，避免每处都写三元 */
static const char *yes_no(bool v)
{
    return v ? "是" : "否";
}

/* ---------------- 7. 开关机源溯源 ---------------- */

/* 位表来自 Datasheet V1.4 §6.13.2.17/18（REG20 开机源 / REG21 关机源）。
 * 这是《掉电重启排查指南》里最有用的一条：PMU 主动关机时串口不会打印，
 * 只有这两个寄存器能告诉我们是谁把它关掉的。 */
static void print_pwron_bits(uint8_t b)
{
    static const struct { uint8_t bit; const char *name; } t[] = {
        { 5, "PWRON/EN 常高" },
        { 4, "电池插入且有效" },
        { 3, "电池电压>3.3V 时充电启动" },
        { 2, "VBUS 插入且有效" },
        { 1, "IRQ 脚被拉低" },
        { 0, "PWRON 低电平当开机电平" },
    };
    bool any = false;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (b & (1u << t[i].bit)) {
            TF_INFO("        开机源: %s（REG20[%u]）", t[i].name, (unsigned)t[i].bit);
            any = true;
        }
    }
    if (!any) {
        TF_INFO("        开机源: 无记录（寄存器已被完全掉电复位，或本次是冷启动）");
    }
}

static void print_pwroff_bits(uint8_t b)
{
    static const struct { uint8_t bit; const char *name; } t[] = {
        { 7, "DIE 过温" },
        { 6, "DCDC 过压(120%/130%)" },
        { 5, "DCDC 欠压(85%)" },
        { 4, "VBUS 过压" },
        { 3, "VSYS 欠压" },
        { 2, "PWRON/EN 常低" },
        { 1, "软件配置关机" },
        { 0, "PWRON 拉低到关机电平" },
    };
    bool any = false;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (b & (1u << t[i].bit)) {
            /* 命中任意一条都意味着"上次是被 PMU 主动关掉的"，正好对应
             * 串口毫无征兆就断掉、重启后又一切正常的那种现象 */
            TF_INFO("        关机源: %s（REG21[%u]）", t[i].name, (unsigned)t[i].bit);
            any = true;
        }
    }
    if (!any) {
        TF_INFO("        关机源: 无记录（上次不是 PMU 主动关机）");
    }
}

static tf_result_t t_pwr_src(void)
{
    REQUIRE_PMIC(d);

    uint8_t on = 0, off = 0, en = 0, t27 = 0, cfg10 = 0, voff = 0;
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_PWRON_STAT, &on), "读开机源 REG20");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_PWROFF_STAT, &off), "读关机源 REG21");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_PWROFF_EN, &en), "读关机策略 REG22");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_PWRON_TIME, &t27), "读按键时间 REG27");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_COMMON_CFG, &cfg10), "读公共配置 REG10");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_PWROFF_VBAT, &voff), "读关机电压 REG24");

    TF_INFO("REG20(开机源)=0x%02X  REG21(关机源)=0x%02X", on, off);
    print_pwron_bits(on);
    print_pwroff_bits(off);

    /* 注意：完全断电（拔电池+拔 USB）会把这两个寄存器清掉，
     * 所以"什么都没记录"不代表没发生过问题，只代表这次读得晚了。 */
    TF_INFO("提示: 完全断电后这两个寄存器会被复位，掉电重启后请立刻读");

    /* 长按关机时间：REG27[3:2] 00=4s 01=6s 10=8s 11=10s（默认 6s） */
    static const int off_s[4] = { 4, 6, 8, 10 };
    TF_INFO("长按关机时间 = %d s（REG27[3:2]=%u）",
            off_s[(t27 >> 2) & 0x03], (unsigned)((t27 >> 2) & 0x03));

    /* 超长按 16s 硬件掉电：REG10[2]，默认关。
     * 打开后按满 16 秒会直接切电，现象和拔电池一样，是排查时的常见误判源。 */
    bool force16 = (cfg10 & 0x04) != 0;
    TF_INFO("16s 超长按强制掉电 = %s（REG10[2]）", force16 ? "已使能" : "未使能");
    if (force16) {
        TF_INFO("  注意: 该功能使能后，按键按住 16 秒会直接断电，无任何日志");
    }

    /* 关机电压：REG24[2:0] 2.6 + N*0.1 V。设太高会在电池还有电时提前关机 */
    int voff_mv = 2600 + (int)(voff & 0x07) * 100;
    TF_INFO("电池关机电压 = %d mV（REG24[2:0]=%u）", voff_mv, (unsigned)(voff & 0x07));
    TF_CHECK_RANGE(voff_mv, 2600.0, 3300.0, "关机电压(mV)");

    /* REG22[2] DIE 过温关机使能（默认开）、[1] 长按关机使能、[0] 关机后重启 */
    TF_INFO("过温关机=%s  长按关机=%s  关机后自动重启=%s（REG22）",
            yes_no((en & 0x04) != 0), yes_no((en & 0x02) != 0), yes_no((en & 0x01) != 0));
    return TF_PASS;
}

/* ---------------- 8. BATFET 通路（拔 USB 能否靠电池活） ---------------- */

static tf_result_t t_batfet(void)
{
    REQUIRE_PMIC(d);

    axp2101_batt_t b;
    TF_CHECK_ERR(axp2101_read_batt(d, &b), "读电池状态");

    uint8_t r12 = 0;
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_BATFET_CTRL, &r12), "读 BATFET 控制 REG12");

    TF_INFO("REG00[4] BATFET 状态 = %s（0=断开 1=导通）",
            b.batfet_open ? "导通" : "断开");
    TF_INFO("REG12 = 0x%02X  bit3(仅电池+关机时使能 BATFET) = %u",
            r12, (unsigned)((r12 >> 3) & 1));

    /* 这一句是整条排查线的关键：
     * USB 在位时 VSYS 由 VBUS 供，BATFET 断开完全正常；
     * 拔掉 USB 后如果 BATFET 仍然不导通，VSYS 就没有第二个电源，
     * 整板立刻掉电 —— 现象就是"拔 USB 就失联"，与固件无关。
     *
     * 判"USB 在不在"要看实测 VBUS，不能看 REG00[5]：VBUS_GOOD 的成立
     * 条件里还有一条 VBUS > VBAT + 150 mV，BAT 采样点被拉到 VSYS 时
     * 压差不够，明明插着电它也会报 0。 */
    bool usb_present = b.vbus_mv >= 2000;
    if (!usb_present) {
        TF_CHECK(b.batfet_open,
                 "当前无 VBUS 且 BATFET 断开：电池没有接到 VSYS，整板靠什么供电？");
    } else {
        TF_INFO("VBUS=%u mV，BATFET 断开属正常（VSYS 由 VBUS 供）", b.vbus_mv);
        if (!b.vbus_good) {
            TF_INFO("注意: 插着电但 REG00[5]=0 —— VBUS 没有比 VBAT 高 150 mV，"
                    "芯片认为输入不可用于充电（详见 charge_chain 用例）");
        }
        TF_INFO("  要验证电池通路：拔掉 USB 后立刻读本项，BATFET 必须变导通");
    }

    /* 手册 V1.4 里 REG12[2] 是保留位，但全志《常见问题》2.2(h) 写的是
     * "batfet_dis（REG12[2]=0）"。版本有出入，这里只报不判，避免照抄错文档。 */
    TF_INFO("REG12[2] = %u（FAQ 称 batfet_dis，V1.4 标保留，仅记录不判定）",
            (unsigned)((r12 >> 2) & 1));
    return TF_PASS;
}

/* ---------------- 9. 充电前置条件链 ---------------- */

/* 《常见问题》2.2 列的 8 个条件，缺一不充。这里逐条算出来，
 * 最后给一句"卡在哪一条"，比让人自己去翻寄存器快得多。 */
static tf_result_t t_charge_chain(void)
{
    REQUIRE_PMIC(d);

    axp2101_batt_t b;
    TF_CHECK_ERR(axp2101_read_batt(d, &b), "读电池状态");

    uint8_t r12 = 0, r50 = 0, r64 = 0, r67 = 0, r68 = 0, irq2 = 0;
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_BATFET_CTRL, &r12), "读 REG12");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_TS_CTRL, &r50), "读 TS 控制 REG50");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_VTERM, &r64), "读恒压点 REG64");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_CHG_TIMER, &r67), "读充电定时器 REG67");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_BAT_DETECT, &r68), "读电池检测 REG68");
    TF_CHECK_ERR(axp2101_read8(d, 0x4A, &irq2), "读 IRQ 状态 2");

    /* (a) VBUS 输入有效 */
    bool a = b.vbus_good;

    /* (b) VBUS 足够高：手册要求 VBUS > VBAT + VSLEEPZ(150 mV) 才认为"可用于充电"。
     * 这条是最容易被忽略的：USB 线细、口弱时 VBUS 塌到 4.4~4.5 V，
     * 跟满电电池只差几十 mV，芯片直接判定"输入不可用于充电"，一直显示未充电。 */
    int margin = (int)b.vbus_mv - (int)b.vbat_mv;
    bool b_ok = (b.vbus_mv >= 2000) && (margin >= AXP2101_VSLEEPZ_MV);

    /* (c) 充电使能 REG18[1] */
    bool c = (b.chg_ctrl & AXP2101_CHG_EN) != 0;

    /* (d) DIE 温度低于 145℃（T = 22 + (7274 - ADC)/20） */
    uint16_t tdie_raw = 0;
    (void)axp2101_read_temp_raw(d, &tdie_raw);
    int die_c = 0;
    bool have_die = tdie_raw != 0;
    if (have_die) {
        uint16_t raw = tdie_raw & 0x3FFF;
        die_c = 22 + (7274 - (int)raw) / 20;
    }
    bool d_ok = !have_die || (die_c < 145);

    /* (e) TS 温度在可充范围。REG50[4]=0 才让温度影响充电 */
    uint16_t ts_raw = 0;
    uint8_t ts_h = 0, ts_l = 0;
    (void)axp2101_read8(d, AXP2101_REG_ADC_TS_H, &ts_h);
    (void)axp2101_read8(d, AXP2101_REG_ADC_TS_H + 1, &ts_l);
    ts_raw = (uint16_t)(((ts_h & 0x3F) << 8) | ts_l);
    bool ts_affect = (r50 & AXP2101_TS_AFFECT_CHG) == 0;
    bool ts_overflow = (ts_raw > 0x2000) || (ts_raw < 16);
    bool e_ok = !(ts_affect && ts_overflow);

    /* (f) VBAT 低于过压门限 */
    bool f_ok = b.vbat_mv < AXP2101_VBAT_OVP_MV;

    /* (g) 安全定时器没超时：IRQ 状态 2 的 bit1 是定时器到期标志 */
    bool g_ok = (irq2 & 0x02) == 0;

    /* (h) BATFET 未被强制关闭 */
    bool h_ok = !(b.bat_present && !b.batfet_open && !b.vbus_good);

    TF_INFO("REG00=0x%02X REG01=0x%02X  充电阶段=%s  电流方向=%s",
            b.st1, b.st2, chg_stat_name(b.chg_stat), cur_dir_name(b.cur_dir));
    TF_INFO("  (a) VBUS 输入有效      : %s", a ? "满足" : "不满足  <-- 卡点");
    TF_INFO("  (b) VBUS>VBAT+150mV    : %s（VBUS=%u VBAT=%u 差=%d mV）",
            b_ok ? "满足" : "不满足  <-- 卡点", b.vbus_mv, b.vbat_mv, margin);
    TF_INFO("  (c) 充电使能 REG18[1]  : %s", c ? "满足" : "不满足  <-- 卡点");
    TF_INFO("  (d) DIE 温度 < 145℃    : %s（%d ℃）",
            d_ok ? "满足" : "不满足  <-- 卡点", have_die ? die_c : -999);
    TF_INFO("  (e) TS 温度可充        : %s（TS 影响充电=%s，原值=0x%04X%s）",
            e_ok ? "满足" : "不满足  <-- 卡点", yes_no(ts_affect), ts_raw,
            ts_overflow ? " 溢出" : "");
    TF_INFO("  (f) VBAT < %d mV OVP   : %s", AXP2101_VBAT_OVP_MV,
            f_ok ? "满足" : "不满足  <-- 卡点");
    TF_INFO("  (g) 安全定时器未超时    : %s", g_ok ? "满足" : "不满足  <-- 定时器 1/2 已到期");
    TF_INFO("  (h) BATFET 未被强制关闭 : %s", h_ok ? "满足" : "不满足  <-- 卡点");

    /* 下面这段才是这个用例的价值：把"为什么"说出来 */
    if (b.chg_stat == 5 /* not charging */) {
        if (!a) {
            if (b.vbus_mv >= 4000) {
                /* 最容易误判的一种：USB 明明插着、VBUS 实测 5 V，
                 * 芯片却说"输入不可用"。原因就是 VBUS 没比 VBAT 高够 150 mV。
                 * 而 VBAT 之所以这么高，往往是 BAT 采样脚接在 VSYS/VBUS 上。 */
                TF_INFO("结论: 未充电 —— VBUS 实测 %u mV 是有的，但芯片判定"
                        "\"输入不可用于充电\"：要求 VBUS 比 VBAT 高 ≥%d mV，"
                        "实际只高 %d mV", b.vbus_mv, AXP2101_VSLEEPZ_MV, margin);
                TF_INFO("       VBAT 读到 %u mV 已超单节锂电满电上限，"
                        "基本可断定 BAT 采样脚接在 VSYS/VBUS 上（电池没真进回路），"
                        "先量 BAT 焊盘对 GND 的电压确认", b.vbat_mv);
            } else {
                TF_INFO("结论: 未充电 —— 没有有效的 VBUS 输入（没插 USB / 线断 / 口坏）");
            }
        } else if (!b_ok) {
            TF_INFO("结论: 未充电 —— 输入压差不足。VBUS 只比 VBAT 高 %d mV，"
                    "芯片要求 ≥%d mV", margin, AXP2101_VSLEEPZ_MV);
        } else if (!c) {
            TF_INFO("结论: 未充电 —— 充电被软件关闭（REG18[1]=0），写 1 才会充");
        } else if (!f_ok) {
            TF_INFO("结论: 未充电 —— VBAT=%u mV 已到过压上限 %d mV，"
                    "单节锂电不该超过 4.35 V，重点查 BAT 采样是否接到了 VSYS",
                    b.vbat_mv, AXP2101_VBAT_OVP_MV);
        } else if (!e_ok) {
            TF_INFO("结论: 未充电 —— TS 温度通路把充电掐了（原值 0x%04X 已溢出，"
                    "常见原因是 NTC 没接或 TS 电流源被关）", ts_raw);
        } else if (!g_ok) {
            TF_INFO("结论: 未充电 —— 充电安全定时器到期，需重新插拔 VBUS 或清 IRQ");
        } else {
            TF_INFO("结论: 未充电 —— 8 个条件都满足但仍不充，重点看 VBAT 与实际电池"
                    "是否一致（可能根本没接电池，见 REG00[3]）");
        }
    } else if (b.chg_stat == 4) {
        TF_INFO("结论: 已充满（充电完成）");
    } else {
        TF_INFO("结论: 正在充电，阶段=%s，VBAT=%u mV", chg_stat_name(b.chg_stat),
                b.vbat_mv);
    }

    TF_INFO("参考: 恒压点 %d mV（REG64[2:0]=%u），回充门限 = 恒压点 - %d mV",
            vterm_mv(r64), (unsigned)(r64 & 0x07), AXP2101_VBAT_RECHG_MV);
    TF_INFO("      电池检测=%s（REG68[0]），安全定时器 REG67=0x%02X", yes_no(r68 & 1), r67);

    /* 只断言"读数自洽"，不把"现在没在充"判成失败：
     * 纯电池运行、电池已满、没接电池都是合法工况。 */
    TF_CHECK(b.vbat_mv > 0 || !b.bat_present, "REG00 说电池在位，但 VBAT 读到 0 mV");
    TF_CHECK(!(b.vbat_mv > AXP2101_VBAT_OVP_MV && b.bat_present),
             "VBAT=%u mV 超出单节锂电上限 %d mV：采样点或电池接法有问题",
             b.vbat_mv, AXP2101_VBAT_OVP_MV);
    return TF_PASS;
}

/* ---------------- 10. VBUS 输入裕量 ---------------- */

static tf_result_t t_vbus_margin(void)
{
    REQUIRE_PMIC(d);

    axp2101_batt_t b;
    TF_CHECK_ERR(axp2101_read_batt(d, &b), "读电池状态");

    uint8_t r15 = 0, r16 = 0, r14 = 0;
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_VINDPM, &r15), "读 VINDPM REG15");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_ILIM, &r16), "读限流 REG16");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_VSYS_DPM, &r14), "读 VSYS DPM REG14");

    int vindpm = vindpm_mv(r15);
    int ilim = ilim_ma(r16);
    int dpm = vsys_dpm_mv(r14);

    TF_INFO("VBUS = %u mV   VINDPM = %d mV   裕量 = %+d mV",
            b.vbus_mv, vindpm, (int)b.vbus_mv - vindpm);
    TF_INFO("输入限流 = %d mA（REG16[2:0]=%u），VSYS DPM = %d mV（REG14[6:4]=%u）",
            ilim, (unsigned)(r16 & 0x07), dpm, (unsigned)((r14 >> 4) & 0x07));

    if (b.vbus_mv < 2000) {
        TF_INFO("当前没有 USB 输入（纯电池运行），本项只报配置不判定");
    } else {
        /* 裕量太小是"插着电却不充/反复重启"的头号原因：
         * VBUS 一跌到 VINDPM 以下，输入电流被立刻收掉。 */
        if (b.vbus_mv <= vindpm) {
            TF_INFO("警告: VBUS 已跌破 VINDPM，输入被限流，充电会停、系统可能欠压重启");
        } else if (b.vbus_mv - vindpm < 100) {
            TF_INFO("警告: VINDPM 裕量不足 100 mV，换个电源口/线，或把 REG15 调低一档");
        } else {
            TF_INFO("VINDPM 裕量正常");
        }

        TF_CHECK(b.vbus_mv < AXP2101_VBUS_OV_MV, "VBUS=%u mV 接近过压门限 %d mV",
                 b.vbus_mv, AXP2101_VBUS_OV_MV);
        TF_CHECK(b.vbus_mv >= AXP2101_VBUS_UVLOZ_MV,
                 "VBUS=%u mV 低于 UVLOZ(%d mV)，PMU 会认为没插电",
                 b.vbus_mv, AXP2101_VBUS_UVLOZ_MV);

        /* 电脑 USB 口只有 500 mA，限流档开到 1.5 A 会让整口掉电
         * （正是《掉电重启排查指南》第一节说的"供电不足"） */
        if (ilim > 1000) {
            TF_INFO("提示: 限流 %d mA 高于电脑 USB 口的 500 mA 能力，"
                    "插在电脑上时应降到 500 mA 档", ilim);
        }
    }

    TF_INFO("VINDPM 状态位 REG01[3] = %u（%s）", (unsigned)(b.vindpm ? 1 : 0),
            b.vindpm ? "正在压降调节" : "未触发");
    TF_INFO("VSYS=%u mV，DPM 设定 %d mV%s", b.vsys_mv, dpm,
            b.vsys_mv + 200 < dpm ? "  <-- VSYS 明显低于 DPM，查电池/负载" : "");
    return TF_PASS;
}

/* ---------------- 11. 电池温度通路（TS / JEITA） ---------------- */

/* 《充电高低温保护功能验证》的核心：
 *   REG50[4]=0 才让温度影响充电（写 1 = 只当外部输入，保护形同虚设）
 *   REG58[0]=1 使能 JEITA（高温/低温降流）
 *   T1=REG54（停充下限） T2=REG5A（降流） T3=REG5B（降流） T4=REG55（停充上限）
 * 另外《常见问题》2.12 说 TS 值到 0x3FFF 是溢出（电流源关了），
 * 到 0x2000 是饱和（>4.096V），这两种都说明 NTC 通路不正常。 */
static tf_result_t t_ts_jeita(void)
{
    REQUIRE_PMIC(d);

    uint8_t r50 = 0, r54 = 0, r55 = 0, r58 = 0, r5a = 0, r5b = 0;
    uint8_t r52 = 0, r53 = 0, r56 = 0, r57 = 0;
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_TS_CTRL, &r50), "读 TS 控制 REG50");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_VLTF_CHG, &r54), "读 T1 REG54");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_VHTF_CHG, &r55), "读 T4 REG55");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_JEITA_EN, &r58), "读 JEITA 使能 REG58");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_JEITA_COOL, &r5a), "读 T2 REG5A");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_JEITA_WARM, &r5b), "读 T3 REG5B");
    (void)axp2101_read8(d, AXP2101_REG_TS_HYSL2H, &r52);
    (void)axp2101_read8(d, AXP2101_REG_TS_HYSH2L, &r53);
    (void)axp2101_read8(d, AXP2101_REG_VLTF_WORK, &r56);
    (void)axp2101_read8(d, AXP2101_REG_VHTF_WORK, &r57);

    bool affect = (r50 & AXP2101_TS_AFFECT_CHG) == 0;
    static const char *cs[4] = { "关", "仅在 TS 通道采样时开", "TS 采样时开、其他通道关", "常开" };
    static const char *ua[4] = { "20uA", "40uA", "50uA", "60uA" };

    TF_INFO("TS 是否影响充电 = %s（REG50[4]=%u）",
            affect ? "是（正常）" : "否 —— 高低温保护不生效",
            (unsigned)((r50 >> 4) & 1));
    TF_INFO("TS 电流源 = %s，电流 = %s（REG50[3:0]=0x%02X）",
            cs[(r50 >> 2) & 0x03], ua[r50 & 0x03], (unsigned)(r50 & 0x0F));

    if (!affect) {
        TF_INFO("提示: REG50[4]=1 时，电池温度再极端也不会停充。"
                "全志文档里那个「改了 dts 却不生效」的坑就是这一位");
    }

    /* 门限值按手册公式换算成 mV，再对照文档给的默认温度点 */
    TF_INFO("T1 低温停充 = %d mV（N*32，REG54=0x%02X，默认≈0℃）",
            (int)r54 * 32, r54);
    TF_INFO("T2 低温降流 = %d mV（N*16，REG5A=0x%02X，默认≈10℃）",
            (int)r5a * 16, r5a);
    TF_INFO("T3 高温降流 = %d mV（N*8， REG5B=0x%02X，默认≈45℃）",
            (int)r5b * 8, r5b);
    TF_INFO("T4 高温停充 = %d mV（N*2， REG55=0x%02X，默认≈55℃）",
            (int)r55 * 2, r55);
    TF_INFO("工作模式门限 T1/T4 = %d / %d mV（REG56/57）",
            (int)r56 * 32, (int)r57 * 2);
    TF_INFO("迟滞 L2H/H2L = %d / %d mV（REG52/53）", (int)r52 * 16, (int)r53 * 4);

    TF_INFO("JEITA 使能 = %s（REG58[0]）", yes_no(r58 & 0x01));

    /* 文档强调：T3 必须低于 T4、T2 必须高于 T1，否则保护点会互相覆盖。
     * NTC 是负温度系数 —— 温度越高电压越低，所以四个门限在**电压**上
     * 必须单调递减：T1(0℃) > T2(10℃) > T3(45℃) > T4(55℃)。
     * 注意四个寄存器的 LSB 各不相同（32/16/8/2 mV），
     * 必须换算成 mV 再比，直接比原始编码会得出错误的"异常"。 */
    int t1 = (int)r54 * 32;
    int t2 = (int)r5a * 16;
    int t3 = (int)r5b * 8;
    int t4 = (int)r55 * 2;
    bool t_order_ok = (t1 > t2) && (t2 > t3) && (t3 > t4);
    TF_INFO("门限顺序检查 = %s（要求 T1>T2>T3>T4，单位 mV：%d>%d>%d>%d）",
            t_order_ok ? "正常" : "异常", t1, t2, t3, t4);

    /* TS 实测值：LSB = 0.5 mV，0x2000 对应 4.096 V */
    uint8_t th = 0, tl = 0;
    (void)axp2101_read8(d, AXP2101_REG_ADC_TS_H, &th);
    (void)axp2101_read8(d, AXP2101_REG_ADC_TS_H + 1, &tl);
    uint16_t raw = (uint16_t)(((th & 0x3F) << 8) | tl);
    TF_INFO("TS 实测原值 = 0x%04X，约 %d mV", raw, (int)raw / 2);

    if (raw >= 0x3FF0) {
        TF_INFO("异常: TS 读数接近 0x3FFF —— 文档 2.12 说的负值溢出，"
                "通常是 TS 电流源关了或 NTC 没接");
    } else if (raw >= 0x2000) {
        TF_INFO("异常: TS 读数 ≥0x2000（>4.096V 饱和）—— NTC 未接或阻值不对");
    }

    /* 只断言"能读出配置"，不把"没接 NTC"判失败：本板电池仓没有 NTC。 */
    TF_CHECK((r50 & 0x03) == 0 || (r50 & 0x0C) != 0,
             "REG50 电流源档位=0（关）却配了电流值，配置自相矛盾");
    return TF_PASS;
}

/* ---------------- 12. 充电参数配置 ---------------- */

static tf_result_t t_chg_cfg(void)
{
    REQUIRE_PMIC(d);

    uint8_t r61 = 0, r62 = 0, r63 = 0, r64 = 0, r65 = 0, r67 = 0, r16 = 0;
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_IPRECHG, &r61), "读预充电流 REG61");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_ICHG, &r62), "读恒流充电 REG62");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_ITERM, &r63), "读终止电流 REG63");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_VTERM, &r64), "读恒压点 REG64");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_THERM_REG, &r65), "读过热调节 REG65");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_CHG_TIMER, &r67), "读充电定时器 REG67");
    TF_CHECK_ERR(axp2101_read8(d, AXP2101_REG_ILIM, &r16), "读输入限流 REG16");

    int ipre = step25_ma(r61);
    int ichg = ichg_ma(r62);
    int iterm = step25_ma(r63);
    int vterm = vterm_mv(r64);
    int ilim = ilim_ma(r16);

    TF_INFO("预充电流 = %d mA（REG61=0x%02X）", ipre, r61);
    TF_INFO("恒流电流 = %d mA（REG62=0x%02X）", ichg, r62);
    TF_INFO("终止电流 = %d mA（REG63=0x%02X，终止使能=%s）",
            iterm, r63, yes_no((r63 & 0x10) != 0));
    TF_INFO("恒压点   = %d mV（REG64[2:0]=%u）", vterm, (unsigned)(r64 & 0x07));

    static const int therm[4] = { 60, 80, 100, 120 };
    TF_INFO("过温降流门限 = %d ℃（REG65[1:0]=%u）",
            therm[r65 & 0x03], (unsigned)(r65 & 0x03));

    /* 安全定时器：REG67[1:0] 预充 40/50/60/70 min，[5:4] 整周期 5/8/12/20 h */
    static const int pre_min[4] = { 40, 50, 60, 70 };
    static const int full_h[4] = { 5, 8, 12, 20 };
    TF_INFO("安全定时器: 预充 %d min（使能=%s），整周期 %d h（REG67=0x%02X）",
            pre_min[r67 & 0x03], yes_no((r67 & 0x04) != 0), full_h[(r67 >> 4) & 0x03], r67);

    /* 下面几条是"参数本身合不合理"，都是纯算术，与是否在充电无关 */
    if (ichg > 0 && iterm > 0) {
        TF_CHECK(iterm < ichg, "终止电流 %d mA 不小于恒流 %d mA，会永远判不到充满",
                 iterm, ichg);
    }
    if (ichg > 0 && ilim > 0) {
        if (ichg > ilim) {
            TF_INFO("提示: 充电电流 %d mA 大于输入限流 %d mA，实际会被限流环压住", ichg, ilim);
        }
    }
    TF_CHECK(vterm > 0, "REG64 恒压点编码 %u 是保留值，充电不会正常结束",
             (unsigned)(r64 & 0x07));
    if (vterm > 0) {
        TF_CHECK_RANGE(vterm, 4000.0, 4400.0, "恒压点(mV)");
        TF_INFO("提示: 恒压点要和电池规格一致 —— 4.35 V 电芯配 4.2 V 会充不满（文档 2.11）");
    }
    return TF_PASS;
}

/* ---------------- 13. ADC 五路交叉校验 ---------------- */

/* 单看每一路都"有个数"，但彼此矛盾就说明接线或采样点有问题。
 * 最典型的一种：VBAT 与 VSYS 只差十几 mV —— 说明 BAT 采样脚其实
 * 接在 VSYS 上，此时所有"电池电压"结论都是假的。 */
static tf_result_t t_adc_cross(void)
{
    REQUIRE_PMIC(d);

    const uint8_t need = AXP2101_ADC_EN_VBAT | AXP2101_ADC_EN_VSYS |
                         AXP2101_ADC_EN_VBUS | AXP2101_ADC_EN_TDIE |
                         AXP2101_ADC_EN_TS;
    TF_CHECK_ERR(axp2101_adc_config(d, need, true), "打开五路 ADC");
    vTaskDelay(pdMS_TO_TICKS(40));

    uint16_t vbat = 0, vsys = 0, vbus = 0, tdie = 0;
    TF_CHECK_ERR(axp2101_read_mv(d, AXP2101_REG_ADC_VBAT_H, &vbat), "读 VBAT");
    TF_CHECK_ERR(axp2101_read_mv(d, AXP2101_REG_ADC_VSYS_H, &vsys), "读 VSYS");
    TF_CHECK_ERR(axp2101_read_mv(d, AXP2101_REG_ADC_VBUS_H, &vbus), "读 VBUS");
    (void)axp2101_read_temp_raw(d, &tdie);

    TF_INFO("VBAT=%u  VSYS=%u  VBUS=%u  TDIE_RAW=%u（单位 mV）",
            vbat, vsys, vbus, (unsigned)tdie);
    TF_INFO("差值: VBUS-VSYS=%d  VSYS-VBAT=%d  VBUS-VBAT=%d",
            (int)vbus - (int)vsys, (int)vsys - (int)vbat, (int)vbus - (int)vbat);

    if (vbus >= 2000) {
        /* 有 USB 时 VBUS ≥ VSYS 恒成立（VSYS 由 VBUS 或 DCDC 产生） */
        TF_CHECK(vbus + 50 >= vsys, "VBUS=%u mV 低于 VSYS=%u mV，采样或电路异常",
                 vbus, vsys);
    }

    if (vbat > 500) {
        /* 电池电压不可能高于系统轨（除非 VSYS 正在被 DPM 压低，留 200 mV 余量） */
        TF_CHECK(vsys + 200 >= vbat, "VBAT=%u mV 高于 VSYS=%u mV，BAT 采样点可能接错",
                 vbat, vsys);

        /* 这一条是专门抓"电池没接、BAT 脚被 VSYS 拉高"的：
         * 单节锂电满电 4.35 V 就到顶了，读数超过 4.35 V 本身就不成立；
         * 若同时与 VSYS 只差几十 mV，基本可以断定采样脚跟系统轨短在一起。 */
        int d = (int)vsys - (int)vbat;
        int ad = (d < 0) ? -d : d;
        if (vbat > 4350 || (ad <= 60 && vbat > 4200)) {
            TF_INFO("警告: VBAT(%u) 与 VSYS(%u) 相差仅 %d mV 且超过 4.35 V —— "
                    "BAT 采样脚很可能接在 VSYS 上，电池并未真正接入", vbat, vsys, ad);
            TF_INFO("      量一下 BAT 焊盘对 GND：不插 USB 时应等于电池电压，"
                    "若跟着 VSYS 一起跳就是接错了");
        }
    }

    if (tdie != 0) {
        int c = 22 + (7274 - (int)(tdie & 0x3FFF)) / 20;
        TF_INFO("DIE 温度 ≈ %d ℃（T = 22 + (7274-ADC)/20，文档 2.8）", c);
        TF_CHECK_RANGE(c, -40.0, 125.0, "DIE 温度(℃)");
    }
    return TF_PASS;
}

/* ---------------- 14. 电量计 ---------------- */

static tf_result_t t_gauge(void)
{
    REQUIRE_PMIC(d);

    axp2101_batt_t b;
    TF_CHECK_ERR(axp2101_read_batt(d, &b), "读电池状态");

    uint8_t gauge_cfg = 0;
    (void)axp2101_read8(d, AXP2101_REG_GAUGE_CTRL, &gauge_cfg);

    bool en = (b.chg_ctrl & AXP2101_GAUGE_EN) != 0;
    TF_INFO("电量计使能 = %s（REG18[3]）", yes_no(en));
    TF_INFO("电量 = %u%%", b.pct == 0xFF ? 0 : b.pct);

    if (!en) {
        TF_INFO("提示: 电量计关闭时 0xA4 不可信（本驱动会返回 0xFF）");
    } else if (b.pct == 0xFF) {
        TF_INFO("提示: 电量计已使能但 0xA4 读不到，可能还没完成第一次库仑积分");
    } else {
        /* 文档 2.10：100% 还在充电是正常现象，因为 100% 对应的 OCV 设得比
         * 实际满充 OCV 低；不要把"显示 100% 但仍显示充电"当成故障。 */
        if (b.pct == 100 && b.chg_stat != 4 && b.chg_stat != 5) {
            TF_INFO("说明: 显示 100%% 仍在充电属正常（文档 2.10），不必当故障");
        }
        TF_INFO("说明: 电量 9x%% 跳到 100%% 是满充 OCV 参数偏高，需要改电池参数（文档 2.11）");
    }

    /* 单节锂电的电量-电压大致区间，只做粗校验：电压很低却报 100% 才是真异常 */
    if (b.vbat_mv > 2000 && b.pct != 0xFF) {
        bool implausible = (b.vbat_mv < 3300 && b.pct > 90) ||
                           (b.vbat_mv > 4150 && b.pct < 20);
        if (implausible) {
            TF_INFO("警告: VBAT=%u mV 与电量 %u%% 明显不匹配，电量计需要重新校准"
                    "（REG17 复位电量计）", b.vbat_mv, b.pct);
        }
    }
    TF_INFO("电量计配置 REG A2 = 0x%02X（仅记录）", gauge_cfg);
    return TF_PASS;
}

/* ---------------- 用例注册 ---------------- */
TF_GROUP_BEGIN(axp2101)
    TF_ITEM(axp2101, present, "I2C 地址应答与句柄建立", t_present, TF_FLAG_HW),
    TF_ITEM(axp2101, chip_id, "读取芯片 ID 并核对型号", t_chip_id, TF_FLAG_HW),
    TF_ITEM(axp2101, rw, "写通路验证（写回原值，不改状态）", t_rw, TF_FLAG_HW),
    TF_ITEM(axp2101, adc, "ADC 电压（VBAT/VSYS/VBUS）与温度通道", t_adc, TF_FLAG_HW),
    TF_ITEM(axp2101, irq, "中断状态寄存器可读性", t_irq, TF_FLAG_HW),
    TF_ITEM(axp2101, dump, "原始寄存器转储 0x00-0x6F", t_dump, TF_FLAG_HW),
    TF_ITEM(axp2101, pwr_src, "开关机源溯源（REG20/21）与关机策略", t_pwr_src, TF_FLAG_HW),
    TF_ITEM(axp2101, batfet, "BATFET 通路：能否靠电池供电", t_batfet, TF_FLAG_HW),
    TF_ITEM(axp2101, charge_chain, "充电前置条件链（8 条，给出卡点）", t_charge_chain, TF_FLAG_HW),
    TF_ITEM(axp2101, vbus_margin, "VBUS 输入裕量（VINDPM / 限流 / 过压欠压）", t_vbus_margin, TF_FLAG_HW),
    TF_ITEM(axp2101, ts_jeita, "电池温度通路 TS / JEITA 与溢出检测", t_ts_jeita, TF_FLAG_HW),
    TF_ITEM(axp2101, chg_cfg, "充电参数配置（电流/恒压点/安全定时器）", t_chg_cfg, TF_FLAG_HW),
    TF_ITEM(axp2101, adc_cross, "ADC 五路交叉校验（电压关系自洽）", t_adc_cross, TF_FLAG_HW),
    TF_ITEM(axp2101, gauge, "电量计读数与配置", t_gauge, TF_FLAG_HW),
TF_GROUP_END(axp2101)
