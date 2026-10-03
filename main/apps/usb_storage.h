/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：在 TF 卡和电脑之间独占切换 USB 磁盘访问。
 * English: Switch exclusive SD access between firmware and USB host.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t usb_storage_start(void);
esp_err_t usb_storage_stop(void);
bool usb_storage_active(void);
bool usb_storage_connected(void);
