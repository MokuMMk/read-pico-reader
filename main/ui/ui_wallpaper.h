/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "epd_highlevel.h"

/// 按锁屏的等比留白规则绘制 TF 图片；预览与锁屏共用。
/// Draw an SD image with the same aspect-fit rule for preview and lock screen.
bool ui_wallpaper_draw(uint8_t *framebuffer, const char *path, EpdRect area);
