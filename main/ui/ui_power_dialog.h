/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 长按电源键弹出的纵向滑动菜单。
 * Vertical swipe menu shown after a long power-key press.
 */
#pragma once

#include "ui_gesture.h"

typedef enum {
    UI_POWER_ACTION_NONE = 0,
    UI_POWER_ACTION_CANCEL,
    UI_POWER_ACTION_SHUTDOWN,
    UI_POWER_ACTION_RESTART,
} ui_power_action_t;

EpdRect ui_power_dialog_rect(void);
void ui_power_dialog_draw(uint8_t *fb);
ui_power_action_t ui_power_dialog_handle(const ui_gesture_event_t *event);
void ui_power_final_draw(uint8_t *fb, bool restarting);
