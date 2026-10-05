/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "epd_highlevel.h"

/// 按原比例居中裁切并铺满区域；预览与锁屏共用。
/// Center-crop an SD image at its original aspect ratio to cover preview and lock screen.
bool ui_wallpaper_draw(uint8_t *framebuffer, const char *path, EpdRect area);

/// 居中填满圆角区域并裁切四角，用于设置页头像。
/// Center-crop into a rounded area for the settings avatar.
bool ui_wallpaper_draw_rounded(uint8_t *framebuffer, const char *path, EpdRect area, int radius);
