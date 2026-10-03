/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 图片专用的稳定 16 级有序抖动；文字和界面线条不要经过这里。
 * Stable 16-level ordered dither for images only; UI text and rules bypass it.
 */
#pragma once

#include <stdint.h>

/// 把 8 位灰度映射到相邻的两个 4 位物理灰阶，坐标决定稳定的 4x4 Bayer 相位。
/// Map 8-bit gray to adjacent physical 4-bit levels with a stable 4x4 Bayer phase.
uint8_t ui_image_dither_gray(uint8_t gray, int x, int y);
