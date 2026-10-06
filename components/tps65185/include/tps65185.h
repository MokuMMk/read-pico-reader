/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TPS65185 电子纸电源管理芯片的寄存器访问。只做 I2C 读写与寄存器语义，不含上电时序——
 * 时序在板级（read_pico_board_metalio.c）里，因为它要配合扩展器的 PWR_GOOD 与面板状态。
 *
 * English: register access for the TPS65185 e-paper PMIC. I2C and register semantics only; the
 * power sequence lives in the board layer, because it has to interleave with the expander's
 * PWR_GOOD line and the panel state.
 *
 * 冻结：不要在这里做延时或状态机。本文件只负责"读一个寄存器""写一个寄存器"。
 * Frozen: no delays and no state machine here. This file only reads and writes registers.
 *
 * 移植自 Metalio E-Ink4-Plus 示例固件（MIT，Copyright (c) 2025 Shenzhen Xinzhi Future
 * Technology Co., Ltd. 与 Project Contributors），见 licenses/METALIO-MIT.txt。
 * Portions ported from the Metalio E-Ink4-Plus demo firmware (MIT); see
 * licenses/METALIO-MIT.txt.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// TPS65185 的 7 位地址，由 A0/A1 决定；板上接法是 0x68。
/// / TPS65185 7-bit address, set by A0/A1; the board straps it to 0x68.
#define TPS65185_ADDR_DEFAULT 0x68

/* ---- 寄存器 / Registers ---- */
#define TPS_REG_TMST_VALUE 0x00
#define TPS_REG_ENABLE 0x01
#define TPS_REG_VADJ 0x02
#define TPS_REG_VCOM1 0x03
#define TPS_REG_VCOM2 0x04
#define TPS_REG_INT_EN1 0x05
#define TPS_REG_INT_EN2 0x06
#define TPS_REG_INT1 0x07
#define TPS_REG_INT2 0x08
#define TPS_REG_UPSEQ0 0x09
#define TPS_REG_UPSEQ1 0x0A
#define TPS_REG_DWNSEQ0 0x0B
#define TPS_REG_DWNSEQ1 0x0C
#define TPS_REG_TMST1 0x0D
#define TPS_REG_TMST2 0x0E
#define TPS_REG_PG 0x0F
#define TPS_REG_REVID 0x10

/// ENABLE 里同时打开五路输出的值：升压到 PG 全绿之后才写。
/// / ENABLE value that turns on all five rails; written only after PG reports good.
#define TPS_ENABLE_ALL_RAILS 0x3F

/// PG 寄存器里代表各路就绪的位掩码。
/// / Bits in PG that mean the rails are up.
#define TPS_PG_ALL_READY_MASK 0xFA

typedef struct tps65185_dev_t* tps65185_handle_t;

/// 绑定到已有 I2C 总线。不探测芯片——上电时序要在特定时刻才 probe。
/// / Bind to an existing I2C bus. Does not probe the chip: the power sequence probes at a
/// specific moment.
esp_err_t tps65185_init(i2c_master_bus_handle_t bus, uint8_t addr, tps65185_handle_t* out);
esp_err_t tps65185_deinit(tps65185_handle_t h);

/// 芯片是否在线（单次寻址 ACK）。STANDBY 之前芯片不应答。
/// / Whether the chip ACKs its address. It will not answer before STANDBY.
esp_err_t tps65185_probe(tps65185_handle_t h);

esp_err_t tps65185_read(tps65185_handle_t h, uint8_t reg, uint8_t* value);
esp_err_t tps65185_write(tps65185_handle_t h, uint8_t reg, uint8_t value);

/// 读温度寄存器（TMST_VALUE），返回摄氏度。失败返回 -128。
/// / Read TMST_VALUE; returns degrees Celsius, or -128 on failure.
int8_t tps65185_temperature(tps65185_handle_t h);

/// 设置 VCOM，单位毫伏绝对值：1600 → -1.6V。写 VCOM1/VCOM2 两个寄存器。
/// / Set VCOM in absolute millivolts: 1600 means -1.6 V. Writes VCOM1 and VCOM2.
esp_err_t tps65185_set_vcom(tps65185_handle_t h, unsigned vcom_mv);

#ifdef __cplusplus
}
#endif
