/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 小纸 Pico 板级：I2C、IOE、SY7636A、EPD 总线与电源轨。
 *
 * Read Pico board: I2C, IO expander, SY7636A, EPD bus and rails.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "epd_board.h"
#include "esp_err.h"
#include "fca9555.h"
#include "sy7636a.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define READ_PICO_I2C_PORT ((i2c_port_t)CONFIG_READ_PICO_I2C_PORT)
#define READ_PICO_I2C_FREQ_HZ CONFIG_READ_PICO_I2C_FREQ_HZ
/// FCA9555 INT#，原理图接到 ESP32-S3 IO41。GPIO41 不是 RTC 脚，只能浅睡唤醒。
/// / FCA9555 INT# on ESP32-S3 IO41. GPIO41 is not RTC, so light-sleep wake only.
#define READ_PICO_IOE_INT_GPIO CONFIG_READ_PICO_IOE_INT_GPIO
/// CST836U INT，原理图 TXD0 / GPIO43。开漏低有效，FPC 侧有 10k 上拉。
/// / CST836U INT on TXD0 / GPIO43. Open-drain, active-low, 10k pull-up on the FPC.
#define READ_PICO_TP_INT_GPIO CONFIG_READ_PICO_TP_INT_GPIO

/// TF 卡接线。Read Pico 是 1 位总线；Metalio 走 SDMMC slot0 的专用 IOMUX，4 位。
/// / Card wiring. Read Pico runs a 1-bit bus; Metalio uses the SDMMC slot 0 dedicated IOMUX in
/// 4-bit mode.
#if defined(PICO_BOARD_METALIO_EINK4_PLUS)
#define READ_PICO_SD_CLK 24
#define READ_PICO_SD_CMD 25
#define READ_PICO_SD_D0 20
#define READ_PICO_SD_D1 21
#define READ_PICO_SD_D2 22
#define READ_PICO_SD_D3 23
#define READ_PICO_SD_WIDTH 4
#else
#define READ_PICO_SD_CLK 38
#define READ_PICO_SD_CMD 42
#define READ_PICO_SD_D0 44
#define READ_PICO_SD_D1 GPIO_NUM_NC
#define READ_PICO_SD_D2 GPIO_NUM_NC
#define READ_PICO_SD_D3 GPIO_NUM_NC
#define READ_PICO_SD_WIDTH 1
#endif

/// 触控芯片的 I2C 地址与中断脚。两颗芯片不同，read_pico_init.c 是共享的，所以差异在这里收敛。
/// / Touch controller address and interrupt pin. The two boards differ and read_pico_init.c is
/// shared, so the difference is concentrated here.
#if defined(PICO_BOARD_METALIO_EINK4_PLUS)
#define READ_PICO_TOUCH_ADDR 0x38
#define READ_PICO_TOUCH_INT_GPIO 5
#else
#define READ_PICO_TOUCH_ADDR CST836U_ADDR_DEFAULT
#define READ_PICO_TOUCH_INT_GPIO CONFIG_READ_PICO_TP_INT_GPIO
#endif

#define READ_PICO_PRODUCT_NAME "Read Pico"
#define READ_PICO_DEVICE_NAME "小纸 Pico"
/// 芯片平放实测 -5,-119,-1005；转到设备坐标后屏幕朝上约为 +Z。
/// / Chip at rest on the bench: -5,-119,-1005. Device frame is about +Z face-up.
#define READ_PICO_ACCEL_ZERO_X_MG 119
#define READ_PICO_ACCEL_ZERO_Y_MG 5
#define READ_PICO_ACCEL_ZERO_Z_MG 1005

// 每块板一个定义，但对外只暴露 epd_board_read_pico 这一个名字，整棵树都只引用它，
// 板级实现在 CMake 里二选一。ESP32-S31 上 Read Pico 的 SY7636A 与 16 位总线都不存在。
// One definition per board, exposed under the single name epd_board_read_pico that the whole
// tree refers to; CMake picks the implementation. On the ESP32-S31 board neither Read Pico's
// SY7636A nor its 16-bit bus exists.
#if defined(PICO_BOARD_METALIO_EINK4_PLUS)
extern const EpdBoardDefinition epd_board_metalio_eink4_plus;
#define epd_board_read_pico epd_board_metalio_eink4_plus
#else
extern const EpdBoardDefinition epd_board_read_pico;
#endif

i2c_master_bus_handle_t read_pico_i2c_bus(void);
sy7636a_handle_t read_pico_sy7636a(void);
fca9555_handle_t read_pico_fca9555(void);

typedef struct {
    uint16_t ioe_input;
    uint16_t ioe_output;
    uint16_t ioe_invert;
    uint16_t ioe_config;
    int ioe_int_level;
    sy7636a_status_t sy;
    bool sy_live;
    bool rails_on;
} read_pico_status_t;

#define READ_PICO_I2C_DEV_N 5

typedef struct {
    const char* name;
    uint8_t addr;
    bool online;
} read_pico_i2c_dev_t;

typedef struct {
    read_pico_i2c_dev_t dev[READ_PICO_I2C_DEV_N];
    int offline_n;
    bool all_online;
} read_pico_i2c_census_t;

/// 轨已上电后再拍。SY7636A 在 EN 低时不应答。
/// / Take after the rails are up. SY7636A NACKs while EN is low.
void read_pico_i2c_census_take(void);
const read_pico_i2c_census_t* read_pico_i2c_census(void);

esp_err_t read_pico_get_status(read_pico_status_t* status);
/// 只读 IN/OUT + INT#，CFG/INV 走缓存。不唤醒 SY7636A。
/// / Read IN/OUT + INT# only; CFG/INV come from cache. Does not wake SY7636A.
esp_err_t read_pico_get_ioe_status(read_pico_status_t* status);
/// 连读 8 寄存器，刷新 CFG/INV 缓存。底栏「读取」和进页用。
/// / Sequential 8-register read; refreshes CFG/INV cache. Footer Read and page enter.
esp_err_t read_pico_get_ioe_status_full(read_pico_status_t* status);
int read_pico_ioe_int_level(void);
bool read_pico_sd_present(void);
bool read_pico_rails_on(void);

/// 翻转 VCOMCTL：1=外部 VCOM_EN，0=自动时序。
/// / Toggle VCOMCTL: 1=external VCOM_EN, 0=auto timing.
esp_err_t read_pico_toggle_vcomctl(void);

/// 拉低再放开触摸芯片的 RST（挂在 FCA9555 上）。触摸进了深度睡眠后不再应答 I2C，
/// 只有这条路能把它叫回来。返回后调用方还要等芯片启动完成。
/// / Pulse the touch RST on the FCA9555. After deep sleep the chip ignores I2C;
/// this is the only wake. Caller must wait for boot after return.
esp_err_t read_pico_touch_reset(void);
/// high=false 拉低复位，high=true 放开。扩展口页要分两帧画脉冲时用。
/// / high=false asserts RST, high=true releases. IOE page draws the pulse in two frames.
esp_err_t read_pico_tp_rst(bool high);

/// 读一遍 9555 输入。INT# 不锁存，输入稳定后会自行松开；浅睡前/醒后都要调。
/// / Read 9555 inputs. INT# is not latched and releases once inputs are stable;
/// call before light sleep and after wake.
void read_pico_clear_ioe_int(void);

#ifdef __cplusplus
}
#endif
