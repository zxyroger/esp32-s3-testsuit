/**
 * @file  test_i2c.c
 * @brief I2C 总线层用例：引脚确认、总线扫描
 */
#include <inttypes.h>
#include <stdio.h>

#include "tf.h"

#include "board.h"
#include "board_config.h"
#include "bus_i2c.h"

static tf_result_t t_bus_pins(void)
{
    const bus_i2c_t *bus = board_bus();
    TF_CHECK(bus != NULL, "I2C 总线建立失败（候选引脚全部无响应）");

    int sda = -1, scl = -1;
    board_bus_pins(&sda, &scl);
    TF_INFO("实际使用 SDA=GPIO%d SCL=GPIO%d @ %" PRIu32 " Hz", sda, scl, bus->hz);

    TF_CHECK(bus->hz > 0, "总线速率未设置");
    return TF_PASS;
}

static tf_result_t t_scan(void)
{
    bus_i2c_t *bus = (bus_i2c_t *)board_bus();
    TF_CHECK(bus != NULL, "I2C 总线未建立，无法扫描");

    uint8_t found[16] = { 0 };
    size_t  n = 0;
    TF_CHECK_ERR(bus_i2c_scan(bus, found, sizeof(found), &n), "扫描总线");

    printf("        | 扫描到 %u 个器件:", (unsigned)n);
    for (size_t i = 0; i < n && i < sizeof(found); i++) {
        printf(" 0x%02X", found[i]);
        if (found[i] == BOARD_ADDR_AXP2101) {
            printf("(AXP2101)");
        } else if (found[i] == BOARD_ADDR_ES8311) {
            printf("(ES8311)");
        }
    }
    printf("\n");

    /* 总线上一个器件都没有，通常是：模组没供电、上拉电阻缺失、
     * 或者 SDA/SCL 接到了未焊接的焊盘。这个结论必须报出来，不能放过。 */
    TF_CHECK(n > 0, "总线上未发现任何器件（查供电 / 上拉 / 引脚）");
    return TF_PASS;
}

/* ---------------- 用例注册 ---------------- */
TF_GROUP_BEGIN(i2c)
    TF_ITEM(i2c, bus_pins, "I2C 引脚与速率确认", t_bus_pins, TF_FLAG_HW),
    TF_ITEM(i2c, scan, "扫描总线并列出所有应答地址", t_scan, TF_FLAG_HW),
TF_GROUP_END(i2c)
