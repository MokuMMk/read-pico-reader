/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui_power_dialog.h"

#include "ui_kit.h"

static const EpdRect k_dialog = {92, 378, 500, 370};
static const EpdRect k_restart = {146, 494, 156, 156};
static const EpdRect k_shutdown = {382, 494, 156, 156};

EpdRect ui_power_dialog_rect(void) { return k_dialog; }

static void draw_power_mark(uint8_t *fb, int cx, int cy, uint8_t color, uint8_t background) {
    epd_draw_circle(cx, cy, 30, color, fb);
    epd_draw_circle(cx, cy, 29, color, fb);
    epd_fill_rect((EpdRect){cx - 5, cy - 38, 10, 35}, background, fb);
    epd_fill_rect((EpdRect){cx - 2, cy - 39, 5, 31}, color, fb);
}

static void draw_restart_mark(uint8_t *fb, int cx, int cy, uint8_t color, uint8_t background) {
    epd_draw_circle(cx, cy, 30, color, fb);
    epd_draw_circle(cx, cy, 29, color, fb);
    epd_fill_rect((EpdRect){cx + 10, cy - 35, 25, 22}, background, fb);
    epd_draw_line(cx + 4, cy - 28, cx + 27, cy - 28, color, fb);
    epd_draw_line(cx + 27, cy - 28, cx + 17, cy - 38, color, fb);
    epd_draw_line(cx + 27, cy - 28, cx + 17, cy - 18, color, fb);
}

void ui_power_dialog_draw(uint8_t *fb) {
    ui_fill_round_rect(fb, k_dialog, 38, 0xd0);
    ui_draw_round_rect(fb, k_dialog, 38, 0x70);
    ui_text(fb, UI_LOCK_WIDTH / 2, 412, 40, "电源选项", EPD_DRAW_ALIGN_CENTER, false);
    ui_text(fb, UI_LOCK_WIDTH / 2, 462, 21, "选择操作，轻触弹窗外取消", EPD_DRAW_ALIGN_CENTER, false);

    epd_fill_circle(224, 566, 66, 0x60, fb);
    epd_fill_circle(460, 566, 66, 0x60, fb);
    draw_restart_mark(fb, 224, 566, UI_GRAY_WHITE, 0x60);
    draw_power_mark(fb, 460, 566, UI_GRAY_WHITE, 0x60);
    ui_text(fb, 224, 665, 29, "重启", EPD_DRAW_ALIGN_CENTER, false);
    ui_text(fb, 460, 665, 29, "关机", EPD_DRAW_ALIGN_CENTER, false);
}

ui_power_action_t ui_power_dialog_handle(const ui_gesture_event_t *event) {
    if (!event) return UI_POWER_ACTION_NONE;
    if (event->type != UI_GESTURE_TAP) return UI_POWER_ACTION_NONE;
    if (!ui_rect_hit(k_dialog, event->x0, event->y0)) return UI_POWER_ACTION_CANCEL;
    if (ui_rect_hit(k_restart, event->x0, event->y0)) return UI_POWER_ACTION_RESTART;
    if (ui_rect_hit(k_shutdown, event->x0, event->y0)) return UI_POWER_ACTION_SHUTDOWN;
    return UI_POWER_ACTION_NONE;
}

void ui_power_final_draw(uint8_t *fb, bool restarting) {
    ui_clear_page(fb);
    const int cx = UI_LOCK_WIDTH / 2;
    if (restarting) draw_restart_mark(fb, cx, 486, UI_GRAY_BLACK, UI_GRAY_WHITE);
    else draw_power_mark(fb, cx, 486, UI_GRAY_BLACK, UI_GRAY_WHITE);
    ui_text(fb, cx, 548, 48, restarting ? "正在重启" : "Pico 已关机",
            EPD_DRAW_ALIGN_CENTER, false);
    if (!restarting)
        ui_text(fb, cx, 624, 26, "长按电源键开机", EPD_DRAW_ALIGN_CENTER, false);
}
