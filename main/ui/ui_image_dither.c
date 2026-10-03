/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui_image_dither.h"

uint8_t ui_image_dither_gray(uint8_t gray, int x, int y) {
    static const uint8_t bayer4[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };
    const unsigned base = gray / 17u;
    if (base >= 15u) return 0xff;
    const unsigned remainder = gray - base * 17u;
    const unsigned threshold = bayer4[(unsigned)y & 3u][(unsigned)x & 3u];
    const unsigned bump = remainder * 16u > threshold * 17u ? 1u : 0u;
    return (uint8_t)((base + bump) * 17u);
}
