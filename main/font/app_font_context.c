/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 用户修订：异常启动暂用内建字体，避免自动重载故障资源；不覆盖用户字体设置。
 * User revision: recovery starts use the built-in face without retrying faulty resources or overwriting saved font choices.
 * 中文：系统默认使用内建思源黑体子集，TF 卡黑体补足缺字；用户选择的字体覆盖默认外观。
 * English: The built-in Source Han Sans face is the default; the SD face fills missing glyphs and explicit choices override it.
 * 用户修订：票根始终借用系统字体，浅睡解锁恢复锁屏前的实际字体和上下文，不覆盖保存的选择。
 * User revision: tickets always borrow the system face; light-sleep wake restores the actual previous face/context without altering saved choices.
 */
#include "app_font_context.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "settings.h"
#include "boot_state.h"
#include "ttf_font.h"
#include "ui_kit.h"

static bool s_reading;

static const char* system_path(char fallback[TTF_FONT_PATH_MAX]) {
    if (pico_boot_recovery()) return TTF_FONT_BUILTIN;
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
    if (!ttf_font_ready() || strcmp(path, ttf_font_path()))
        changed = ttf_font_open_builtin() == ESP_OK || changed;
    // 未选字体时由内建思源黑体绘制界面；TTF 仍保留完整字库供文件名缺字回退。
    // Built-in Source Han Sans draws default UI; the full TTF remains loaded for missing glyphs.
    ui_text_set_system_font(!pico_boot_recovery() && chosen[0] && ttf_font_ready() &&
                           !strcmp(chosen, ttf_font_path()) && !ttf_font_is_builtin());
    ui_text_set_system_scale(true);
    return changed;
}

bool app_font_activate_reading(void) {
    s_reading = true;
    const char* saved = pico_boot_recovery() ? TTF_FONT_BUILTIN : app_settings_font_path();
    bool changed = activate(ttf_font_path_is_builtin(saved) ? TTF_FONT_BUILTIN : saved);
    ui_text_set_system_font(false);
    ui_text_set_system_scale(false);
    return changed;
}

bool app_font_retry_active(void) {
    return s_reading ? app_font_activate_reading() : app_font_activate_system();
}

void app_font_begin_lock(app_lock_font_t *saved) {
    if (!saved) return;
    snprintf(saved->path, sizeof(saved->path), "%s", ttf_font_path());
    saved->reading = s_reading;
    app_font_activate_system();
}

void app_font_end_lock(const app_lock_font_t *saved) {
    if (!saved) return;
    if (strcmp(saved->path, ttf_font_path()) && !activate(saved->path))
        (void)ttf_font_open_builtin();
    s_reading = saved->reading;
    const char *chosen = app_settings_system_font_path();
    ui_text_set_system_font(!s_reading && !pico_boot_recovery() && chosen[0] &&
                           !strcmp(chosen, ttf_font_path()) && !ttf_font_is_builtin());
    ui_text_set_system_scale(!s_reading);
}
