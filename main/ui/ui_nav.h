/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：四个产品主页面共用的底部导航；阅读页自行隐藏。
 * English: Shared four-tab product navigation; the reader hides it.
 */
#pragma once

#include "app.h"

#define UI_NAV_TOP 1096

void ui_nav_draw(uint8_t *fb, int active);
void ui_nav_status(uint8_t *fb);
/// 绘制设置页与状态栏共用的 WiFi 标志。/ Draw the WiFi mark shared by Settings and the status bar.
void ui_nav_wifi_icon(uint8_t *fb, int cx, int cy, int size, uint8_t gray);
/// 二级页面共用的圆形返回按钮。/ Shared circular back control for secondary pages.
void ui_nav_back(uint8_t *fb, int x, int y);
int ui_nav_hit(uint16_t x, uint16_t y);
void ui_nav_request(app_ctx_t *ctx, int index);
