/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：阅读目录独立页面的布局、绘制与命中定义。
 * English: Layout, rendering and hit testing for the standalone reading directory.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "epdiy.h"

#define BOOK_TOC_ROWS 7
#define BOOK_TOC_BACK (-2)
#define BOOK_TOC_PREV (-3)
#define BOOK_TOC_NEXT (-4)

int book_toc_pages(size_t chapters);
int book_toc_hit(uint16_t x, uint16_t y, int page, size_t chapters);
EpdRect book_toc_row_rect(int row);
void book_toc_render(uint8_t *fb, const char *book_title, size_t chapters,
                     size_t current, int page, const char *message);
