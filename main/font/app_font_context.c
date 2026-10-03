/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：系统默认使用内建思源黑体子集，TF 卡黑体补足缺字；用户选择的字体覆盖默认外观。
 * English: The built-in Source Han Sans face is the default; the SD face fills missing glyphs and explicit choices override it.
 */
#include "app_font_context.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "settings.h"
#include "ttf_font.h"
#include "ui_kit.h"

static bool s_reading;

static const char* system_path(char fallback[TTF_FONT_PATH_MAX]) {
    const char* chosen = app_settings_system_font_path();
    if (chosen[0]) return chosen;
    if (snprintf(fallback, TTF_FONT_PATH_MAX, "%s/Hei.ttf", app_settings_fonts_dir()) >= TTF_FONT_PATH_MAX)
        return TTF_FONT_BUILTIN;
    struct stat st;
    if (stat(fallback, &st) == 0 && S_ISREG(st.st_mode)) return fallback;
    if (!ttf_font_count()) ttf_font_scan();
    const ttf_font_item_t *item = ttf_font_item(0);
    return item ? item->path : TTF_FONT_BUILTIN;
}

static bool activate(const char* path) {
    if (ttf_font_ready() && !strcmp(ttf_font_path(), path)) return false;
    return ttf_font_open(path) == ESP_OK;
}

bool app_font_activate_system(void) {
    s_reading = false;
    const char* chosen = app_settings_system_font_path();
    char fallback[TTF_FONT_PATH_MAX];
    const char* path = system_path(fallback);
    bool changed = activate(path);
    // 未选字体时由内建思源黑体绘制界面；TTF 仍保留完整字库供文件名缺字回退。
    // Built-in Source Han Sans draws default UI; the full TTF remains loaded for missing glyphs.
    ui_text_set_system_font(chosen[0] && ttf_font_ready() && !ttf_font_is_builtin());
    ui_text_set_system_scale(true);
    return changed;
}

bool app_font_activate_reading(void) {
    s_reading = true;
    const char* saved = app_settings_font_path();
    bool changed = activate(ttf_font_path_is_builtin(saved) ? TTF_FONT_BUILTIN : saved);
    ui_text_set_system_font(false);
    ui_text_set_system_scale(false);
    return changed;
}

bool app_font_retry_active(void) {
    return s_reading ? app_font_activate_reading() : app_font_activate_system();
}
