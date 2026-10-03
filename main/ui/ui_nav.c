/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：绘制与命中四栏导航；细线图标分别表示首页、书架、文件和设置。
 * English: Draw and hit-test the four-tab navigation with thin-line home, books, files and settings icons.
 */
#include "ui_nav.h"
#include <stdio.h>
#include <time.h>
#include "app_registry.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "read_pico_transfer.h"
#include "ui_kit.h"
#include "assets/wifi_glyph.h"

static const char *const labels[] = {"首页", "书架", "文件管理", "设置"};

void ui_nav_back(uint8_t *fb, int x, int y) {
    // 参考系统题头缩小返回键，中心仍保持在 y=113。/ Match the smaller reference back control while retaining the title centerline.
    ui_fill_round_rect(fb, (EpdRect){x, y + 13, 44, 44}, 22, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, (EpdRect){x, y + 13, 44, 44}, 22, 0xc0);
    epd_draw_line(x + 27, y + 24, x + 17, y + 35, UI_GRAY_BLACK, fb);
    epd_draw_line(x + 17, y + 35, x + 27, y + 46, UI_GRAY_BLACK, fb);
}

void ui_nav_wifi_icon(uint8_t *fb, int cx, int cy, int size, uint8_t gray) {
    if (!fb || size < 16 || size > 40) return;
    // 直接绘制预览原图的灰阶掩模；24 像素状态栏使用同款缩放数据。
    // Draw the preview glyph mask directly; the 24 px status icon uses its matching downsample.
    const int source_size = size <= 26 ? 24 : 32;
    const uint8_t *mask = source_size == 24 ? wifi_glyph_24 : wifi_glyph_32;
    const int background = source_size == 24 ? 0xe0 : 0xff;
    for (int y = 0; y < size; ++y) {
        int sy = y * source_size / size;
        for (int x = 0; x < size; ++x) {
            int sx = x * source_size / size;
            int pixel = sy * source_size + sx;
            uint8_t byte = mask[pixel / 2];
            int alpha = pixel & 1 ? byte & 15 : byte >> 4;
            if (!alpha) continue;
            int value = (background * (15 - alpha) + gray * alpha + 7) / 15;
            epd_draw_pixel(cx - size / 2 + x, cy - size / 2 + y,
                           ui_contrast_gray((uint8_t)value), fb);
        }
    }
}

static void icon(uint8_t *fb, int tab, int cx, int y) {
    if (tab == 0) {
        epd_draw_line(cx - 19, y + 16, cx, y + 1, UI_GRAY_BLACK, fb);
        epd_draw_line(cx, y + 1, cx + 19, y + 16, UI_GRAY_BLACK, fb);
        epd_draw_line(cx - 14, y + 13, cx - 14, y + 35, UI_GRAY_BLACK, fb);
        epd_draw_line(cx + 14, y + 13, cx + 14, y + 35, UI_GRAY_BLACK, fb);
        epd_draw_line(cx - 14, y + 35, cx + 14, y + 35, UI_GRAY_BLACK, fb);
        epd_draw_rect((EpdRect){cx - 5, y + 21, 10, 14}, UI_GRAY_BLACK, fb);
    } else if (tab == 1) {
        epd_draw_rect((EpdRect){cx - 19, y + 4, 9, 30}, UI_GRAY_BLACK, fb);
        epd_draw_line(cx - 15, y + 7, cx - 15, y + 29, UI_GRAY_BLACK, fb);
        epd_draw_rect((EpdRect){cx - 7, y + 8, 9, 26}, UI_GRAY_BLACK, fb);
        epd_draw_line(cx - 3, y + 11, cx - 3, y + 29, UI_GRAY_BLACK, fb);
        epd_draw_line(cx + 8, y + 3, cx + 17, y + 5, UI_GRAY_BLACK, fb);
        epd_draw_line(cx + 17, y + 5, cx + 12, y + 34, UI_GRAY_BLACK, fb);
        epd_draw_line(cx + 12, y + 34, cx + 3, y + 32, UI_GRAY_BLACK, fb);
        epd_draw_line(cx + 3, y + 32, cx + 8, y + 3, UI_GRAY_BLACK, fb);
        epd_draw_line(cx + 9, y + 27, cx + 13, y + 28, UI_GRAY_BLACK, fb);
    } else if (tab == 2) {
        epd_draw_line(cx - 19, y + 8, cx - 5, y + 8, UI_GRAY_BLACK, fb);
        epd_draw_line(cx - 5, y + 8, cx, y + 13, UI_GRAY_BLACK, fb);
        epd_draw_line(cx, y + 13, cx + 19, y + 13, UI_GRAY_BLACK, fb);
        epd_draw_line(cx + 19, y + 13, cx + 19, y + 34, UI_GRAY_BLACK, fb);
        epd_draw_line(cx + 19, y + 34, cx - 19, y + 34, UI_GRAY_BLACK, fb);
        epd_draw_line(cx - 19, y + 34, cx - 19, y + 8, UI_GRAY_BLACK, fb);
        epd_draw_line(cx - 18, y + 16, cx + 18, y + 16, UI_GRAY_BLACK, fb);
    } else {
        // 用三组滑杆表示设置，线宽与其他底栏图标保持一致。
        // Three sliders identify Settings with a stroke weight consistent with the other tabs.
        const int knob_x[] = {cx - 7, cx + 8, cx - 2};
        for (int row = 0; row < 3; ++row) {
            int line_y = y + 7 + row * 12;
            epd_draw_line(cx - 18, line_y, cx + 18, line_y, UI_GRAY_BLACK, fb);
            epd_fill_circle(knob_x[row], line_y, 5, UI_GRAY_WHITE, fb);
            epd_draw_circle(knob_x[row], line_y, 5, UI_GRAY_BLACK, fb);
        }
    }
}

void ui_nav_draw(uint8_t *fb, int active) {
    epd_fill_rect((EpdRect){0, UI_NAV_TOP, UI_LOCK_WIDTH, UI_LOCK_HEIGHT - UI_NAV_TOP}, 0xe0, fb);
    ui_hairline(fb, UI_NAV_TOP, 0, UI_LOCK_WIDTH, UI_GRAY_LIGHT);
    for (int i = 0; i < 4; ++i) {
        int center = (i * 2 + 1) * UI_LOCK_WIDTH / 8;
        if (i == active) epd_fill_rect((EpdRect){center - 28, UI_NAV_TOP + 1, 56, 5}, UI_GRAY_BLACK, fb);
        icon(fb, i, center, UI_NAV_TOP + 27);
        ui_text(fb, center, UI_NAV_TOP + 74, 17, labels[i], EPD_DRAW_ALIGN_CENTER, false);
    }
}

void ui_nav_status(uint8_t *fb) {
    time_t now = time(NULL);
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (now < 1704067200 && pmu && pmu->time_synced && pmu->unix_sec >= 1704067200)
        now = pmu->unix_sec;
    char clock[12] = "--:--";
    if (now > 1704067200) {
        time_t china = now + 8 * 3600;
        struct tm date;
        if (gmtime_r(&china, &date)) snprintf(clock, sizeof(clock), "%02d:%02d", date.tm_hour, date.tm_min);
    }
    const int status_center_y = 40;
    // 固定状态栏字号，让系统字体缩放时文字仍与图标共用中心线。
    // Keep status type fixed so text and glyphs share one centerline at every system font scale.
    ui_text_fixed(fb, 36, 28, 24, clock, EPD_DRAW_ALIGN_LEFT, false);
    read_pico_transfer_status_t network = {0};
    read_pico_transfer_get_status(&network);
    if (network.network_ready && network.mode == READ_PICO_TRANSFER_MODE_STA) {
        ui_nav_wifi_icon(fb, 515, status_center_y, 30, 0x6a);
    }
    EpdRect battery = {606, status_center_y - 11, 40, 22};
    ui_draw_round_rect(fb, battery, 6, UI_GRAY_BLACK);
    ui_fill_round_rect(fb, (EpdRect){648, status_center_y - 4, 5, 8}, 2, UI_GRAY_BLACK);
    char percent[8] = "--%";
    bool charging = pmu && pmu->status_ok &&
        (pmu->flags & (PMU_STATUS_CHARGING_ACTIVE | PMU_STATUS_CHARGE_PIN_HIGH));
    if (pmu && pmu->soc_permille <= 1000) {
        snprintf(percent, sizeof(percent), "%u%%", (unsigned)(pmu->soc_permille + 5) / 10);
        int width = (31 * (int)pmu->soc_permille + 500) / 1000;
        if (pmu->soc_permille == 1000) width = 31;
        if (width > 0) ui_fill_round_rect(fb, (EpdRect){610, status_center_y - 7, width, 14}, 4, UI_GRAY_BLACK);
    }
    if (charging) {
        const int x[] = {628, 621, 627, 624, 633, 628};
        const int y[] = {29, 40, 40, 51, 38, 38};
        for (int i = 0; i < 5; ++i)
            epd_draw_line(x[i], y[i], x[i + 1], y[i + 1],
                          pmu->soc_permille >= 550 ? UI_GRAY_WHITE : UI_GRAY_BLACK, fb);
    }
    ui_text_fixed(fb, 595, 29, 22, percent, EPD_DRAW_ALIGN_RIGHT, false);
}

int ui_nav_hit(uint16_t x, uint16_t y) {
    if (x >= UI_LOCK_WIDTH || y < UI_NAV_TOP || y >= UI_LOCK_HEIGHT) return -1;
    int tab = x * 4 / UI_LOCK_WIDTH;
    return tab < 4 ? tab : -1;
}

void ui_nav_request(app_ctx_t *ctx, int index) {
    if (index >= 0 && index < 4) ctx->request_app = app_at(index);
}
