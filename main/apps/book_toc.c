/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：目录只呈现章节导航，和书架列表、旧示例菜单完全分离。
 * English: The directory only navigates chapters and shares no shelf or demo-menu rendering.
 */
#include "book_toc.h"

#include <stdio.h>
#include <string.h>
#include "book_source.h"
#include "ui_kit.h"
#include "ui_nav.h"

enum { TOC_LEFT = 36, TOC_RIGHT = 648, TOC_ROW_TOP = 252,
       TOC_ROW_STEP = 108, TOC_ROW_HEIGHT = 94 };

int book_toc_pages(size_t chapters) {
    return chapters ? (int)(1 + (chapters - 1) / BOOK_TOC_ROWS) : 1;
}

EpdRect book_toc_row_rect(int row) {
    return (EpdRect){TOC_LEFT, TOC_ROW_TOP + row * TOC_ROW_STEP,
                     TOC_RIGHT - TOC_LEFT, TOC_ROW_HEIGHT};
}

// 把 EPUB/TXT 中的换行、制表符和控制符收敛为一个空格，防止绘制越过本行。
// Collapse line breaks, tabs and controls from EPUB/TXT to one space so glyphs stay in one row.
static void one_line(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t out = 0;
    bool gap = false;
    for (const unsigned char *p = (const unsigned char *)src; *p && out + 1 < cap;) {
        unsigned char c = *p;
        if (c <= 0x20 || c == 0x7f) { gap = out != 0; ++p; continue; }
        if (gap && out + 2 < cap) dst[out++] = ' ';
        gap = false;
        int bytes = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (out + (size_t)bytes >= cap) break;
        for (int i = 0; i < bytes && *p; ++i) dst[out++] = (char)*p++;
    }
    while (out && dst[out - 1] == ' ') --out;
    dst[out] = 0;
}

static void fit_line(char *text, int px, int width) {
    size_t len = strlen(text);
    bool trimmed = false;
    while (len && (ui_text_fixed_width_px(px, text) > width || len > 96)) {
        do { --len; } while (len && ((unsigned char)text[len] & 0xc0) == 0x80);
        text[len] = 0;
        trimmed = true;
    }
    if (trimmed && len > 3) {
        // Cut a little more before adding the ellipsis so its glyph cannot cross the status area.
        // 添加省略号前预留字形宽度，避免跨过右侧当前章节标记。
        while (len && ui_text_fixed_width_px(px, text) + ui_text_fixed_width_px(px, "…") > width) {
            do { --len; } while (len && ((unsigned char)text[len] & 0xc0) == 0x80);
            text[len] = 0;
        }
        if (len + sizeof("…") <= 160) strcat(text, "…");
    }
}

int book_toc_hit(uint16_t x, uint16_t y, int page, size_t chapters) {
    if (x < 100 && y >= 76 && y < 151) return BOOK_TOC_BACK;
    if (y >= 1033 && y < 1111) {
        if (x < 183) return BOOK_TOC_PREV;
        if (x >= 501) return BOOK_TOC_NEXT;
    }
    for (int row = 0; row < BOOK_TOC_ROWS; ++row) {
        size_t index = (size_t)page * BOOK_TOC_ROWS + row;
        if (index >= chapters) break;
        if (ui_rect_hit(book_toc_row_rect(row), x, y)) return (int)index;
    }
    return -1;
}

void book_toc_render(uint8_t *fb, const char *book_title, size_t chapters,
                     size_t current, int page, const char *message) {
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){0, 0, UI_LOCK_WIDTH, UI_LOCK_HEIGHT}, 0xe0, fb);
    ui_nav_status(fb);
    ui_nav_back(fb, TOC_LEFT, 79);
    ui_text_fixed(fb, 342, 93, 34, "目录", EPD_DRAW_ALIGN_CENTER, false);

    char title[160];
    one_line(title, sizeof(title), book_title ? book_title : "");
    if (!title[0]) strcpy(title, "当前书籍");
    fit_line(title, 21, 445);
    ui_text_fixed(fb, TOC_LEFT, 169, 21, title, EPD_DRAW_ALIGN_LEFT, false);
    char total[48];
    snprintf(total, sizeof(total), "%u 节", (unsigned)chapters);
    ui_text_fixed(fb, TOC_RIGHT, 171, 18, total, EPD_DRAW_ALIGN_RIGHT, false);
    ui_hairline(fb, 213, TOC_LEFT, TOC_RIGHT - TOC_LEFT, 0x90);

    if (message && *message) {
        char info[160]; one_line(info, sizeof(info), message); fit_line(info, 23, 540);
        ui_text_fixed(fb, 342, 408, 23, info, EPD_DRAW_ALIGN_CENTER, false);
    } else if (!chapters) {
        ui_text_fixed(fb, 342, 408, 25, "这本书没有可用目录", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        for (int row = 0; row < BOOK_TOC_ROWS; ++row) {
            size_t index = (size_t)page * BOOK_TOC_ROWS + row;
            if (index >= chapters) break;
            size_t source_index = book_navigation_chapter(index);
            if (source_index == SIZE_MAX) break;
            EpdRect r = book_toc_row_rect(row);
            bool selected = index == current;
            ui_fill_round_rect(fb, r, 20, selected ? 0xc0 : UI_GRAY_WHITE);
            char number[16]; snprintf(number, sizeof(number), "%02u", (unsigned)index + 1);
            ui_text_fixed(fb, r.x + 22, r.y + 33, 19, number, EPD_DRAW_ALIGN_LEFT, false);
            char raw[160] = "", label[160];
            if (book_navigation_title(index, raw, sizeof(raw)) != ESP_OK || !raw[0])
                snprintf(raw, sizeof(raw), "第 %u 节", (unsigned)index + 1);
            char spine_default[32];
            snprintf(spine_default, sizeof(spine_default), "第 %u 节", (unsigned)source_index + 1);
            if (strcmp(raw, spine_default) == 0)
                snprintf(raw, sizeof(raw), "第 %u 节", (unsigned)index + 1);
            one_line(label, sizeof(label), raw);
            if (!label[0]) snprintf(label, sizeof(label), "第 %u 节", (unsigned)index + 1);
            fit_line(label, 27, selected ? 427 : 482);
            ui_text_fixed(fb, r.x + 79, r.y + 28, 27, label, EPD_DRAW_ALIGN_LEFT, false);
            if (selected)
                ui_text_fixed(fb, r.x + r.width - 22, r.y + 34, 18, "当前",
                              EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
    const int pages = book_toc_pages(chapters);
    if (page > 0) ui_text_fixed(fb, TOC_LEFT, 1052, 21, "‹ 上一页", EPD_DRAW_ALIGN_LEFT, false);
    if (page + 1 < pages) ui_text_fixed(fb, TOC_RIGHT, 1052, 21, "下一页 ›", EPD_DRAW_ALIGN_RIGHT, false);
    char position[32]; snprintf(position, sizeof(position), "%02d / %02d", page + 1, pages);
    ui_text_fixed(fb, 342, 1054, 18, position, EPD_DRAW_ALIGN_CENTER, false);
}
