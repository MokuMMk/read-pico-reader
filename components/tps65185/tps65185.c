/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TPS65185 的 I2C 寄存器访问。协议与上电时序的寄存器值取自 xiaozhi 体系的
 * Metalio E-Ink4-Plus 演示固件（bus/tps65185.c，同芯片同面板），这里只把旧版
 * i2c_port_t 接口换成 IDF v6 的 i2c_master 句柄接口。
 *
 * English: I2C register access for the TPS65185. The protocol and the register values used by
 * the power sequence come from the Metalio E-Ink4-Plus demo firmware (bus/tps65185.c, same
 * chip and panel); the only change here is the old i2c_port_t interface giving way to IDF v6's
 * i2c_master handle.
 *
 * 移植自 Metalio E-Ink4-Plus 示例固件（MIT，Copyright (c) 2025 Shenzhen Xinzhi Future
 * Technology Co., Ltd. 与 Project Contributors），见 licenses/METALIO-MIT.txt。
 * Portions ported from the Metalio E-Ink4-Plus demo firmware (MIT); see
 * licenses/METALIO-MIT.txt.
 */

#include "tps65185.h"

#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "tps65185"

/// 单次寄存器访问的超时。TPS 在升压过程中会拉伸时钟，短超时容易误判离线。
/// / Timeout for one register access. The TPS stretches the clock while boosting, so a short
/// timeout reads as "offline" too easily.
#define TPS_I2C_TIMEOUT_MS 100

/// 温度转换的轮询上限，按 Metalio 的 100 次。
/// / Temperature conversion poll limit, matching Metalio's 100 tries.
#define TPS_TMST_TRIES 100

struct tps65185_dev_t {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t addr;
};

esp_err_t tps65185_init(i2c_master_bus_handle_t bus, uint8_t addr, tps65185_handle_t* out) {
    if (bus == NULL || out == NULL) return ESP_ERR_INVALID_ARG;

    tps65185_handle_t h = calloc(1, sizeof(*h));
    if (h == NULL) return ESP_ERR_NO_MEM;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &h->dev);
    if (err != ESP_OK) {
        free(h);
        return err;
    }
    h->addr = addr;
    h->bus = bus;
    *out = h;
    return ESP_OK;
}

esp_err_t tps65185_deinit(tps65185_handle_t h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_bus_rm_device(h->dev);
    free(h);
    return err;
}

esp_err_t tps65185_probe(tps65185_handle_t h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    return i2c_master_probe(h->bus, h->addr, TPS_I2C_TIMEOUT_MS);
}

esp_err_t tps65185_read(tps65185_handle_t h, uint8_t reg, uint8_t* value) {
    if (h == NULL || value == NULL) return ESP_ERR_INVALID_ARG;
    // 先写寄存器号再读一字节，就是数据手册的 single-byte read。
    // Write the register number then read one byte, the datasheet's single-byte read.
    return i2c_master_transmit_receive(h->dev, &reg, 1, value, 1, TPS_I2C_TIMEOUT_MS);
}

esp_err_t tps65185_write(tps65185_handle_t h, uint8_t reg, uint8_t value) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(h->dev, buf, sizeof(buf), TPS_I2C_TIMEOUT_MS);
}

int8_t tps65185_temperature(tps65185_handle_t h) {
    if (h == NULL) return -128;

    // TMST1 的 bit7 启动一次转换，bit5 是完成标志；结果是有符号摄氏度。
    // TMST1 bit7 starts a conversion and bit5 is the done flag; the result is signed Celsius.
    if (tps65185_write(h, TPS_REG_TMST1, 0x80) != ESP_OK) return -128;

    uint8_t tmst = 0;
    for (int i = 0; i < TPS_TMST_TRIES; ++i) {
        if (tps65185_read(h, TPS_REG_TMST1, &tmst) == ESP_OK && (tmst & 0x20)) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if ((tmst & 0x20) == 0) {
        ESP_LOGW(TAG, "thermistor conversion timed out");
        return -128;
    }

    uint8_t value = 0;
    if (tps65185_read(h, TPS_REG_TMST_VALUE, &value) != ESP_OK) return -128;
    return (int8_t)value;
}

esp_err_t tps65185_set_vcom(tps65185_handle_t h, unsigned vcom_mv) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    // VCOM 是 9 位、单位 10mV：VCOM2 只放最高位，VCOM1 放低 8 位。
    // VCOM is 9 bits in 10 mV units: VCOM2 carries the top bit, VCOM1 the low eight.
    const unsigned val = vcom_mv / 10;
    esp_err_t err = tps65185_write(h, TPS_REG_VCOM2, (uint8_t)((val & 0x100) >> 8));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "VCOM2 write: %s", esp_err_to_name(err));
        return err;
    }
    err = tps65185_write(h, TPS_REG_VCOM1, (uint8_t)(val & 0xFF));
    if (err != ESP_OK) ESP_LOGW(TAG, "VCOM1 write: %s", esp_err_to_name(err));
    return err;
}
