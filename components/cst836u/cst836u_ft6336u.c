/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：cst836u.h 那套接口的 FT6336U 实现（Metalio E-Ink4-Plus 用的就是它）。
 * 接口名字沿用 cst836u 是为了让 main/ 一行不改；芯片寄存器和读点格式完全不同，
 * 所以这块是整个文件重写，不是改几个地址。
 *
 * English: the FT6336U implementation of the cst836u.h interface (the Metalio E-Ink4-Plus uses
 * this controller). The interface keeps the cst836u names so main/ does not change; the registers
 * and the point format are entirely different, so this is a rewrite rather than an address swap.
 *
 * 移植自 Metalio E-Ink4-Plus 示例固件（MIT，Copyright (c) 2025 Shenzhen Xinzhi Future
 * Technology Co., Ltd. 与 Project Contributors），见 licenses/METALIO-MIT.txt。
 * Portions ported from the Metalio E-Ink4-Plus demo firmware (MIT); see
 * licenses/METALIO-MIT.txt.
 *
 * 冻结：FT6336U 没有"动态上报/深睡"这对模式，set_mode 只在本地记录，不写芯片。
 * Frozen: the FT6336U has no dynamic-report/deep-sleep pair, so set_mode only records locally
 * and never writes the chip.
 */

#include "cst836u.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ft6336u"

/* ---- FT6336U 寄存器 / Registers ---- */
#define FT_REG_TD_STATUS 0x02  ///< 触摸状态与点坐标 / Touch status and points
#define FT_REG_CHIP_ID 0xA3    ///< 芯片 ID，FT6336 读回 0x64 / Chip ID, 0x64 on the FT6336
#define FT_REG_FW_VER 0xA6     ///< 固件版本 / Firmware version
#define FT_REG_INT_MODE 0xA4   ///< 中断模式 / Interrupt mode
#define FT_CHIP_ID_6336 0x64

#define FT_EVENT_DOWN 0    ///< 首次按下 / First press
#define FT_EVENT_CONTACT 2 ///< 持续接触 / Continued contact

/// 面板逻辑尺寸（竖屏）。坐标映射要用。
/// / Panel logical size in portrait; the coordinate mapping needs it.
#define FT_LOGICAL_W 684
#define FT_LOGICAL_H 1216

#define FT_I2C_TIMEOUT_MS 50

struct cst836u_dev_t {
    i2c_master_dev_handle_t dev;
    gpio_num_t int_gpio;
    gpio_num_t rst_gpio;
    esp_err_t (*reset_fn)(void);
    cst836u_mode_t mode;
    uint8_t chip_id;
    uint8_t fw;
};

static esp_err_t ft_read(cst836u_handle_t h, uint8_t reg, uint8_t* buf, size_t n) {
    return i2c_master_transmit_receive(h->dev, &reg, 1, buf, n, FT_I2C_TIMEOUT_MS);
}

static esp_err_t ft_write(cst836u_handle_t h, uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(h->dev, buf, sizeof(buf), FT_I2C_TIMEOUT_MS);
}

/// 芯片自报坐标 → 面板逻辑坐标。芯片按自己的方向出点，横过来时要做一次转置。
/// / Controller coordinates to panel logical coordinates. The controller reports in its own
/// orientation, so a landscape report needs transposing.
static void ft_to_logical(int raw_x, int raw_y, int* lx, int* ly) {
    int x = raw_x;
    int y = raw_y;
    if (raw_x >= FT_LOGICAL_W && raw_y < FT_LOGICAL_W) {
        x = FT_LOGICAL_W - 1 - raw_y;
        y = raw_x;
    }
    if (x < 0) {
        x = 0;
    } else if (x >= FT_LOGICAL_W) {
        x = FT_LOGICAL_W - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= FT_LOGICAL_H) {
        y = FT_LOGICAL_H - 1;
    }
    *lx = x;
    *ly = y;
}

esp_err_t cst836u_init(i2c_master_bus_handle_t bus_handle, const cst836u_config_t* config,
                       cst836u_handle_t* handle) {
    if (bus_handle == NULL || config == NULL || handle == NULL) return ESP_ERR_INVALID_ARG;

    cst836u_handle_t h = calloc(1, sizeof(*h));
    if (h == NULL) return ESP_ERR_NO_MEM;
    h->int_gpio = config->int_gpio;
    h->rst_gpio = config->rst_gpio;
    h->reset_fn = config->reset_fn;
    h->mode = CST836U_MODE_NORMAL;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->i2c_addr,
        .scl_speed_hz = config->scl_speed_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &h->dev);
    if (err != ESP_OK) {
        free(h);
        return err;
    }

    if (h->rst_gpio != GPIO_NUM_NC) {
        gpio_config_t rst = {
            .pin_bit_mask = 1ULL << h->rst_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst);
    }
    if (h->int_gpio != GPIO_NUM_NC) {
        gpio_config_t irq = {
            .pin_bit_mask = 1ULL << h->int_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&irq);
    }

    (void)cst836u_reset(h);

    // 芯片 ID 是判断"对面到底是不是 FT6336"的唯一依据，读不到就当作没接。
    // The chip ID is the only way to tell whether an FT6336 answered; failing to read it means
    // nothing is there.
    err = ft_read(h, FT_REG_CHIP_ID, &h->chip_id, 1);
    if (err != ESP_OK || h->chip_id != FT_CHIP_ID_6336) {
        ESP_LOGW(TAG, "chip id 0x%02X (err %s), expected 0x64", h->chip_id, esp_err_to_name(err));
        i2c_master_bus_rm_device(h->dev);
        free(h);
        return ESP_ERR_NOT_FOUND;
    }
    (void)ft_read(h, FT_REG_FW_VER, &h->fw, 1);
    // 中断模式寄存器清零：交由主机轮询，避免芯片持续拉低 INT。
    // Clear the interrupt-mode register so the host polls, instead of the chip holding INT low.
    (void)ft_write(h, FT_REG_INT_MODE, 0x00);

    ESP_LOGI(TAG, "FT6336U ready id=0x%02X fw=0x%02X", h->chip_id, h->fw);
    *handle = h;
    return ESP_OK;
}

esp_err_t cst836u_deinit(cst836u_handle_t h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_bus_rm_device(h->dev);
    free(h);
    return err;
}

esp_err_t cst836u_read(cst836u_handle_t h, cst836u_touch_t* touch) {
    if (h == NULL || touch == NULL) return ESP_ERR_INVALID_ARG;
    memset(touch, 0, sizeof(*touch));

    uint8_t buf[6] = { 0 };
    esp_err_t err = ft_read(h, FT_REG_TD_STATUS, buf, sizeof(buf));
    if (err != ESP_OK) return err;
    memcpy(touch->raw, buf, sizeof(buf));

    const int n = buf[0] & 0x0F;
    if (n == 0 || n > CST836U_MAX_POINTS) return ESP_OK;

    // buf[1] 高两位是事件，低四位是 X 高位；Y 的高位在 buf[3] 低四位。
    // buf[1] carries the event in its top two bits and X's high nibble in its low four; Y's high
    // nibble sits in the low four bits of buf[3].
    const int ev = (buf[1] >> 6) & 0x03;
    if (ev != FT_EVENT_DOWN && ev != FT_EVENT_CONTACT) return ESP_OK;

    const int raw_x = ((buf[1] & 0x0F) << 8) | buf[2];
    const int raw_y = ((buf[3] & 0x0F) << 8) | buf[4];
    int lx = 0;
    int ly = 0;
    ft_to_logical(raw_x, raw_y, &lx, &ly);

    touch->touched = true;
    touch->count = (uint8_t)n;
    touch->x = (uint16_t)lx;
    touch->y = (uint16_t)ly;
    return ESP_OK;
}

esp_err_t cst836u_get_info(cst836u_handle_t h, cst836u_info_t* info) {
    if (h == NULL || info == NULL) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    info->id = h->chip_id;
    info->fw = h->fw;
    // FT6336U 没有 CST836U 那组模块/项目号，留 0；type 低字节放芯片 ID 便于界面显示。
    // The FT6336U has no CST836U module/project numbers, so those stay 0; the chip ID goes in the
    // low byte of type so the UI can show something meaningful.
    info->type = h->chip_id;
    return ESP_OK;
}

esp_err_t cst836u_set_mode(cst836u_handle_t h, cst836u_mode_t mode) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    h->mode = mode;
    return ESP_OK;
}

cst836u_mode_t cst836u_get_mode(cst836u_handle_t h) {
    return h == NULL ? CST836U_MODE_NORMAL : h->mode;
}

esp_err_t cst836u_reset(cst836u_handle_t h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (h->reset_fn != NULL) return h->reset_fn();
    if (h->rst_gpio == GPIO_NUM_NC) return ESP_OK;
    // FT6336U：拉低至少 5ms，放开后要等 300ms 才应答。
    // FT6336U: hold low for at least 5 ms and wait 300 ms after release before it answers.
    gpio_set_level(h->rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(h->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    return ESP_OK;
}

esp_err_t cst836u_wake(cst836u_handle_t h) {
    esp_err_t err = cst836u_reset(h);
    if (err != ESP_OK) return err;
    // 复位后重新交回轮询模式，否则 INT 会一直被拉低。
    // Put it back into polled mode after the reset, or INT stays asserted.
    return ft_write(h, FT_REG_INT_MODE, 0x00);
}

bool cst836u_int_asserted(cst836u_handle_t h) {
    return cst836u_int_level(h) == 0;
}

int cst836u_int_level(cst836u_handle_t h) {
    if (h == NULL || h->int_gpio == GPIO_NUM_NC) return 1;
    return gpio_get_level(h->int_gpio);
}

gpio_num_t cst836u_int_gpio(cst836u_handle_t h) {
    return h == NULL ? GPIO_NUM_NC : h->int_gpio;
}

const char* cst836u_mode_name(cst836u_mode_t mode) {
    return mode == CST836U_MODE_DEEPSLEEP ? "sleep" : "normal";
}

const char* cst836u_event_name(uint8_t event) {
    // CST836U 的事件码与 FT6336 的 0/2 语义一致，界面文案两边通用。
    // The CST836U event codes share the 0/2 meaning used by the FT6336, so the labels are common.
    switch (event) {
        case 0: return "down";
        case 1: return "up";
        case 2: return "contact";
        default: return "?";
    }
}
