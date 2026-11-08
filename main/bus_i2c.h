/**
 * @file  bus_i2c.h
 * @brief I2C 主机总线封装（基于 ESP-IDF v5.2+ 新驱动 i2c_master）
 *
 * 只做三件事：建总线、探器件、扫总线。
 * 器件驱动（axp2101 等）不直接碰 i2c_master API，全部经这里，
 * 便于将来加"总线错误注入"或换成软件 I2C 而不改驱动。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t bus;
    int      sda;
    int      scl;
    uint32_t hz;
    bool     ready;
} bus_i2c_t;

/** 建立总线。失败时 b->ready 保持 false。 */
esp_err_t bus_i2c_begin(bus_i2c_t *b, int sda, int scl, uint32_t hz);

/** 释放总线 */
esp_err_t bus_i2c_end(bus_i2c_t *b);

/** 探测单个 7 位地址是否应答（不发数据，只发地址 + R/W 位） */
bool bus_i2c_probe(bus_i2c_t *b, uint8_t addr7);

/** 扫描 0x08..0x77，返回命中的地址个数；out 可为 NULL */
esp_err_t bus_i2c_scan(bus_i2c_t *b, uint8_t *out, size_t max, size_t *count);

/**
 * 在候选引脚对上依次建总线并 probe 目标地址，命中即保留该总线。
 *
 * 为什么要这个：AXP2101 的 I2C 脚位若不确定，盲写死引脚只会 NACK。
 * SDA/SCL 反接同样只是 NACK，所以候选表里两序都试，命中即定。
 */
esp_err_t bus_i2c_autodetect(bus_i2c_t *b, uint8_t addr7,
                             const int8_t (*cand)[2], size_t count,
                             uint32_t hz, int *hit_sda, int *hit_scl);

#ifdef __cplusplus
}
#endif
