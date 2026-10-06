/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：绘制与命中四栏导航；细线图标分别表示首页、书架、文件和设置，图形来自 Lucide。
 * English: Draw and hit-test the four-tab navigation with thin-line home, books, files and settings icons from Lucide.
 */
#include "ui_nav.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_registry.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "read_pico_transfer.h"
#include "ui_kit.h"
#include "settings.h"
#include "ble_page_turner.h"

// 底栏图标盒、返回键箭头、充电闪电的边长。
// Box sizes for the bar icons, the back chevron and the charging bolt.
#define UI_NAV_TAB_ICON_PX 44
#define UI_NAV_BACK_ICON_PX 38
#define UI_NAV_BOLT_ICON_PX 26

static const char *const labels[] = {"首页", "书架", "文件管理", "设置"};

void ui_nav_back(uint8_t *fb, int x, int y) {
    // 参考系统题头缩小返回键，中心仍保持在 y=113。/ Match the smaller reference back control while retaining the title centerline.
    ui_fill_round_rect(fb, (EpdRect){x, y + 13, 44, 44}, 22, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, (EpdRect){x, y + 13, 44, 44}, 22, 0xc0);
    // Lucide chevron-left 的笔画占画布 14/24 高，取 38 像素复现原来 22 像素的视觉高度。
    // Lucide chevron-left spans 14/24 of its canvas, so 38 px reproduces the old 22 px visual height.
    ui_draw_icon(fb, x + 22, y + 35, UI_NAV_BACK_ICON_PX, UI_ICON_CHEVRON_LEFT, UI_GRAY_BLACK);
}

void ui_nav_wifi_icon(uint8_t *fb, int cx, int cy, int size, uint8_t gray) {
    if (!fb || size < 16 || size > 40) return;
    ui_draw_icon(fb, cx, cy, size, UI_ICON_WIFI, gray);
}

static void icon(uint8_t *fb, int tab, int cx, int y) {
    // 四个底栏图标固定用 Lucide 细线原图，线宽与状态栏一致；y 是图标盒顶边。
    // The four bar icons stay on upstream Lucide line art at the status-bar stroke weight; y is the box top.
    static const ui_icon_t tabs[4] = {
        UI_ICON_HOUSE, UI_ICON_LIBRARY_BIG,
        UI_ICON_FOLDER_OPEN, UI_ICON_SLIDERS_HORIZONTAL,
    };
    if (tab < 0 || tab >= 4) return;
    ui_draw_icon(fb, cx, y + 18, UI_NAV_TAB_ICON_PX, tabs[tab], UI_GRAY_BLACK);
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
    const bool wifi_visible = network.network_ready && network.mode == READ_PICO_TRANSFER_MODE_STA;
    const bool bluetooth_visible = app_settings_ble_turner();
    // 双图标时为签名留出安全间距；三者共用中心线。/ Reserve signature clearance for both icons on one centerline.
    const int signature_width = wifi_visible && bluetooth_visible ? 220 : 280;
    char signature[96];
    snprintf(signature, sizeof(signature), "%s", app_settings_status_signature());
    while (signature[0] && ui_text_fixed_width_px(21, signature) > signature_width) {
        size_t n = strlen(signature) - 1;
        while (n && ((unsigned char)signature[n] & 0xc0) == 0x80) --n;
        signature[n] = 0;
    }
    if (signature[0]) ui_text_fixed(fb, UI_LOCK_WIDTH / 2, 29, 21, signature, EPD_DRAW_ALIGN_CENTER, false);
    if (wifi_visible) ui_nav_wifi_icon(fb, 515, status_center_y, 30, 0x6a);
    if (bluetooth_visible)
        ui_draw_icon(fb, wifi_visible ? 479 : 515, status_center_y, 30, UI_ICON_BLUETOOTH,
                     ble_pt_running() ? 0x6a : 0x8a);
    EpdRect battery = {606, status_center_y - 11, 40, 22};
    ui_draw_round_rect(fb, battery, 6, UI_GRAY_BLACK);
    ui_fill_round_rect(fb, (EpdRect){648, status_center_y - 4, 5, 8}, 2, UI_GRAY_BLACK);
    char percent[8] = "--%";
    bool charging = pmu_battery_charging(pmu);
    int gauge_percent = pmu_battery_percent(pmu);
    if (gauge_percent >= 0) {
        snprintf(percent, sizeof(percent), "%u%%", (unsigned)gauge_percent);
        int width = (31 * gauge_percent + 50) / 100;
        if (width > 0) ui_fill_round_rect(fb, (EpdRect){610, status_center_y - 7, width, 14}, 4, UI_GRAY_BLACK);
    }
    if (charging) {
        // 闪电只落在深色电量块上：透明像素保留底色，白色墨在浅色区自然消失，
        // 不必再按电量高低切换墨色。
        // The bolt shows only over the dark fill: transparent pixels keep the background,
        // so white ink vanishes on the light part and no SOC-dependent colour is needed.
        ui_draw_icon(fb, 627, status_center_y, UI_NAV_BOLT_ICON_PX, UI_ICON_ZAP, UI_GRAY_WHITE);
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
