/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS 读写。打开失败就用深睡默认值，不擦除整个分区。
 *
 * NVS load/store. A failed open keeps the deep-sleep default; the
 * partition is not erased.
 */

#include "settings.h"

#include <stdio.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "read_pico_sd.h"

#define TAG "settings"
#define NVS_NS "read_pico"
#define NVS_KEY_SLEEP "sleep"
#define NVS_KEY_FONT "font"
#define NVS_KEY_SYS_FONT "sys_font"
#define NVS_KEY_SYS_SIZE "sys_size"
#define NVS_KEY_SYS_CONTRAST "sys_contrast"
#define NVS_KEY_LOCK_STYLE "lock_ui"
#define NVS_KEY_WALLPAPER "wallpaper"
#define NVS_KEY_WAKE "lwake"
#define NVS_KEY_BOOT "lboot"
#define NVS_KEY_PICKUP "pickup"
#define NVS_KEY_BOOK_PX "bk_px"
#define NVS_KEY_BOOK_SHAKE "bk_shake"
#define NVS_KEY_READER_FULL "rd_gc16"
#define NVS_KEY_READER_TURN "rd_turn"
#define NVS_KEY_POWER_TURN "rd_power"
#define NVS_KEY_IMMERSIVE "rd_immersive"
#define NVS_KEY_BOOK_LINE "bk_line"
#define NVS_KEY_BOOK_PARA "bk_para"
#define NVS_KEY_BOOK_MARGIN "bk_margin"
#define NVS_KEY_BOOK_TRACK "bk_track"
#define NVS_KEY_BOOK_RULE "bk_rule"
#define NVS_KEY_SHELF_STYLE "shelf_ui"
#define NVS_KEY_SHELF_V22 "shelf_v22"
#define NVS_KEY_BOOKS_DIR "books_dir"
#define NVS_KEY_FONTS_DIR "fonts_dir"
#define FONT_PATH_MAX 160
#define MEDIA_DIR_MAX 96

static app_sleep_mode_t s_sleep = APP_SLEEP_DEEP;
static char s_font[FONT_PATH_MAX];
static char s_system_font[FONT_PATH_MAX];
static uint8_t s_system_size = 120;
static uint8_t s_system_contrast = 130;
static uint8_t s_lock_style;
static char s_wallpaper[288];
static uint8_t s_last_wake;
static uint8_t s_last_boot;
static bool s_pickup_wake;
static uint8_t s_book_px = 48;
static bool s_book_shake;
static uint8_t s_reader_full_pages = 15;
static uint8_t s_reader_turn_effect;
static uint8_t s_book_line = 150, s_book_para = 50, s_book_margin = 36;
static bool s_reader_power_turn;
static bool s_reader_immersive;
static uint8_t s_book_tracking = 2, s_book_reading_line;
static uint8_t s_shelf_style = 2;
static char s_books_dir[MEDIA_DIR_MAX] = "/sdcard/books";
static char s_fonts_dir[MEDIA_DIR_MAX] = "/sdcard/fonts";
static void nvs_put_u8(const char* key, uint8_t value);

static bool valid_media_dir(const char* path) {
    if (!path || strncmp(path, "/sdcard/", 8) || !path[8] ||
        strlen(path) >= MEDIA_DIR_MAX || path[strlen(path) - 1] == '/') return false;
    const char *segment = path + 8;
    for (const unsigned char *p = (const unsigned char *)segment; *p; ++p) {
        if (*p < 32 || *p == 127 || *p == '\\' || *p == ':') return false;
        if (*p == '/' && (p == (const unsigned char *)segment || p[-1] == '/')) return false;
    }
    for (const char *p = segment; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        if (!end) break;
        p = end + 1;
    }
    return true;
}

static bool set_media_dir(char *dst, const char *key, const char *path) {
    if (!valid_media_dir(path)) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, key, path);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;
    strlcpy(dst, path, MEDIA_DIR_MAX);
    return true;
}

static void remove_legacy_weread_session(void) {
    // 卸载在线阅读功能后，清除旧版保存的登录凭据；用户的本地书籍不受影响。
    // Clear credentials left by the removed online reader without touching local books.
    nvs_handle_t h;
    if (nvs_open("pico_weread", NVS_READONLY, &h) != ESP_OK) return;
    size_t size = 0;
    esp_err_t err = nvs_get_str(h, "cookie", NULL, &size);
    nvs_close(h);
    if (err != ESP_OK || nvs_open("pico_weread", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, "cookie") == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
}

static uint8_t valid_book_px(uint8_t px) {
    return px >= 36 && px <= 72 ? px : 48;
}

void app_settings_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init %s, use deep sleep", esp_err_to_name(err));
        return;
    }

    remove_legacy_weread_session();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t raw = APP_SLEEP_DEEP;
    if (nvs_get_u8(h, NVS_KEY_SLEEP, &raw) == ESP_OK && raw <= APP_SLEEP_OFF) {
        s_sleep = (app_sleep_mode_t)raw;
    }
    size_t font_len = sizeof(s_font);
    if (nvs_get_str(h, NVS_KEY_FONT, s_font, &font_len) != ESP_OK) {
        s_font[0] = '\0';
    }
    font_len = sizeof(s_system_font);
    if (nvs_get_str(h, NVS_KEY_SYS_FONT, s_system_font, &font_len) != ESP_OK)
        s_system_font[0] = 0;
    uint8_t system_size = 120;
    if (nvs_get_u8(h, NVS_KEY_SYS_SIZE, &system_size) == ESP_OK &&
        system_size >= 100 && system_size <= 140 && system_size % 10 == 0)
        s_system_size = system_size;
    uint8_t system_contrast = 130;
    if (nvs_get_u8(h, NVS_KEY_SYS_CONTRAST, &system_contrast) == ESP_OK &&
        system_contrast >= 100 && system_contrast <= 140 && system_contrast % 10 == 0)
        s_system_contrast = system_contrast;
    uint8_t lock_style = 0;
    if (nvs_get_u8(h, NVS_KEY_LOCK_STYLE, &lock_style) == ESP_OK && lock_style <= 1)
        s_lock_style = lock_style;
    size_t wallpaper_len = sizeof(s_wallpaper);
    if (nvs_get_str(h, NVS_KEY_WALLPAPER, s_wallpaper, &wallpaper_len) != ESP_OK ||
        strncmp(s_wallpaper, "/sdcard/", 8)) s_wallpaper[0] = 0;
    uint8_t wake = 0;
    if (nvs_get_u8(h, NVS_KEY_WAKE, &wake) == ESP_OK) s_last_wake = wake;
    uint8_t boot = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOT, &boot) == ESP_OK) s_last_boot = boot;
    uint8_t pickup = 0;
    if (nvs_get_u8(h, NVS_KEY_PICKUP, &pickup) == ESP_OK) s_pickup_wake = pickup != 0;
    uint8_t book_px = 48, book_shake = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_PX, &book_px) == ESP_OK) s_book_px = valid_book_px(book_px);
    if (nvs_get_u8(h, NVS_KEY_BOOK_SHAKE, &book_shake) == ESP_OK) s_book_shake = book_shake != 0;
    uint8_t reader_full_pages = 15;
    if (nvs_get_u8(h, NVS_KEY_READER_FULL, &reader_full_pages) == ESP_OK &&
        (reader_full_pages == 5 || reader_full_pages == 10 || reader_full_pages == 15))
        s_reader_full_pages = reader_full_pages;
    uint8_t reader_turn_effect = 0;
    if (nvs_get_u8(h, NVS_KEY_READER_TURN, &reader_turn_effect) == ESP_OK && reader_turn_effect <= 1)
        s_reader_turn_effect = reader_turn_effect;
    uint8_t power_turn = 0;
    if (nvs_get_u8(h, NVS_KEY_POWER_TURN, &power_turn) == ESP_OK) s_reader_power_turn = power_turn == 1;
    uint8_t immersive = 0;
    if (nvs_get_u8(h, NVS_KEY_IMMERSIVE, &immersive) == ESP_OK) s_reader_immersive = immersive == 1;
    uint8_t tracking = 2, reading_line = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_TRACK, &tracking) == ESP_OK && tracking <= 4)
        s_book_tracking = tracking;
    if (nvs_get_u8(h, NVS_KEY_BOOK_RULE, &reading_line) == ESP_OK && reading_line <= 2)
        s_book_reading_line = reading_line;
    uint8_t line = 150, para = 50, margin = 36;
    if (nvs_get_u8(h, NVS_KEY_BOOK_LINE, &line) == ESP_OK) {
        if (line >= 110 && line <= 150) s_book_line = line;
        else if (line == 180) s_book_line = 150;
    }
    if (nvs_get_u8(h, NVS_KEY_BOOK_PARA, &para) == ESP_OK && para <= 75 && para % 25 == 0) s_book_para = para;
    if (nvs_get_u8(h, NVS_KEY_BOOK_MARGIN, &margin) == ESP_OK && margin >= 24 && margin <= 60)
        s_book_margin = margin;
    uint8_t shelf_style = 2;
    if (nvs_get_u8(h, NVS_KEY_SHELF_STYLE, &shelf_style) == ESP_OK && shelf_style >= 1 && shelf_style <= 3)
        s_shelf_style = shelf_style;
    uint8_t shelf_v22 = 0;
    bool migrate_shelf = nvs_get_u8(h, NVS_KEY_SHELF_V22, &shelf_v22) != ESP_OK || shelf_v22 != 1;
    if (migrate_shelf) s_shelf_style = 2;
    char folder[MEDIA_DIR_MAX];
    size_t folder_len = sizeof(folder);
    if (nvs_get_str(h, NVS_KEY_BOOKS_DIR, folder, &folder_len) == ESP_OK && valid_media_dir(folder))
        strlcpy(s_books_dir, folder, sizeof(s_books_dir));
    folder_len = sizeof(folder);
    if (nvs_get_str(h, NVS_KEY_FONTS_DIR, folder, &folder_len) == ESP_OK && valid_media_dir(folder))
        strlcpy(s_fonts_dir, folder, sizeof(s_fonts_dir));
    nvs_close(h);
    if (migrate_shelf && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        // 首次刷入此版只切换书架外观；随后尊重用户手动选择。/ Set acrylic once on upgrade, then preserve manual choices.
        esp_err_t saved = nvs_set_u8(h, NVS_KEY_SHELF_STYLE, 2);
        if (saved == ESP_OK) saved = nvs_set_u8(h, NVS_KEY_SHELF_V22, 1);
        if (saved == ESP_OK) (void)nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(
        TAG, "sleep mode %s, font %s",
        app_sleep_mode_name(s_sleep),
        s_font[0] != '\0' ? s_font : "(builtin)"
    );
}

app_sleep_mode_t app_settings_sleep_mode(void) {
    return s_sleep;
}

const char* app_sleep_mode_name(app_sleep_mode_t mode) {
    switch (mode) {
        case APP_SLEEP_LIGHT: return "light";
        case APP_SLEEP_DEEP: return "deep";
        case APP_SLEEP_OFF: return "off";
        default: return "?";
    }
}

void app_settings_set_sleep_mode(app_sleep_mode_t mode) {
    if (mode > APP_SLEEP_OFF) mode = APP_SLEEP_DEEP;
    s_sleep = mode;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SLEEP, (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

const char* app_settings_font_path(void) {
    return s_font;
}

const char* app_settings_system_font_path(void) { return s_system_font; }
void app_settings_set_system_font_path(const char* path) {
    if (!path) path = "";
    if (strnlen(path, sizeof(s_system_font)) >= sizeof(s_system_font)) return;
    strlcpy(s_system_font, path, sizeof(s_system_font));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_SYS_FONT, s_system_font);
    nvs_commit(h);
    nvs_close(h);
}
uint8_t app_settings_system_font_size(void) { return s_system_size; }
void app_settings_set_system_font_size(uint8_t percent) {
    if (percent < 100 || percent > 140 || percent % 10 || percent == s_system_size) return;
    s_system_size = percent;
    nvs_put_u8(NVS_KEY_SYS_SIZE, percent);
}
uint8_t app_settings_system_contrast(void) { return s_system_contrast; }
void app_settings_set_system_contrast(uint8_t percent) {
    if (percent < 100 || percent > 140 || percent % 10 || percent == s_system_contrast) return;
    s_system_contrast = percent;
    nvs_put_u8(NVS_KEY_SYS_CONTRAST, percent);
}
uint8_t app_settings_lock_style(void) { return s_lock_style; }
void app_settings_set_lock_style(uint8_t style) {
    if (style > 1 || style == s_lock_style) return;
    s_lock_style = style;
    nvs_put_u8(NVS_KEY_LOCK_STYLE, style);
}
const char* app_settings_wallpaper_path(void) { return s_wallpaper; }
void app_settings_set_wallpaper_path(const char* path) {
    if (!path || (path[0] && strncmp(path, "/sdcard/", 8)) ||
        strnlen(path, sizeof(s_wallpaper)) >= sizeof(s_wallpaper)) return;
    strlcpy(s_wallpaper, path, sizeof(s_wallpaper));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_WALLPAPER, s_wallpaper);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_put_u8(const char* key, uint8_t value) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_last_wake(void) {
    return s_last_wake;
}

void app_settings_set_last_wake(uint8_t src) {
    if (src == s_last_wake) return;
    s_last_wake = src;
    nvs_put_u8(NVS_KEY_WAKE, src);
}

uint8_t app_settings_last_boot(void) {
    return s_last_boot;
}

void app_settings_set_last_boot(uint8_t reason) {
    if (reason == 0 || reason == s_last_boot) return;
    s_last_boot = reason;
    nvs_put_u8(NVS_KEY_BOOT, reason);
}

bool app_settings_pickup_wake(void) {
    return s_pickup_wake;
}

void app_settings_set_pickup_wake(bool on) {
    if (s_pickup_wake == on) return;
    s_pickup_wake = on;
    nvs_put_u8(NVS_KEY_PICKUP, on ? 1 : 0);
}

void app_settings_set_font_path(const char* path) {
    if (path == NULL) path = "";
    strlcpy(s_font, path, sizeof(s_font));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_FONT, s_font);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_book_px(void) {
    return s_book_px;
}

void app_settings_set_book_px(uint8_t px) {
    px = valid_book_px(px);
    if (s_book_px == px) return;
    s_book_px = px;
    nvs_put_u8(NVS_KEY_BOOK_PX, px);
}

bool app_settings_book_shake(void) {
    return s_book_shake;
}

void app_settings_set_book_shake(bool on) {
    if (s_book_shake == on) return;
    s_book_shake = on;
    nvs_put_u8(NVS_KEY_BOOK_SHAKE, on ? 1 : 0);
}
uint8_t app_settings_reader_full_pages(void) { return s_reader_full_pages; }
void app_settings_set_reader_full_pages(uint8_t pages) {
    if ((pages != 5 && pages != 10 && pages != 15) || pages == s_reader_full_pages) return;
    s_reader_full_pages = pages;
    nvs_put_u8(NVS_KEY_READER_FULL, pages);
}
uint8_t app_settings_reader_turn_effect(void) { return s_reader_turn_effect; }
void app_settings_set_reader_turn_effect(uint8_t effect) {
    if (effect > 1 || effect == s_reader_turn_effect) return;
    s_reader_turn_effect = effect;
    nvs_put_u8(NVS_KEY_READER_TURN, effect);
}
bool app_settings_reader_power_turn(void) { return s_reader_power_turn; }
void app_settings_set_reader_power_turn(bool on) {
    if (s_reader_power_turn == on) return;
    s_reader_power_turn = on;
    nvs_put_u8(NVS_KEY_POWER_TURN, on ? 1 : 0);
}
bool app_settings_reader_immersive(void) { return s_reader_immersive; }
void app_settings_set_reader_immersive(bool on) {
    if (s_reader_immersive == on) return;
    s_reader_immersive = on;
    nvs_put_u8(NVS_KEY_IMMERSIVE, on ? 1 : 0);
}
uint8_t app_settings_book_tracking(void) { return s_book_tracking; }
void app_settings_set_book_tracking(uint8_t index) {
    if (index > 4 || index == s_book_tracking) return;
    s_book_tracking = index;
    nvs_put_u8(NVS_KEY_BOOK_TRACK, index);
}
uint8_t app_settings_book_reading_line(void) { return s_book_reading_line; }
void app_settings_set_book_reading_line(uint8_t style) {
    if (style > 2 || style == s_book_reading_line) return;
    s_book_reading_line = style;
    nvs_put_u8(NVS_KEY_BOOK_RULE, style);
}
uint8_t app_settings_book_line_spacing(void) { return s_book_line; }
void app_settings_set_book_line_spacing(uint8_t percent) {
    if (percent < 110 || percent > 150) return;
    if (s_book_line == percent) return;
    s_book_line = percent;
    nvs_put_u8(NVS_KEY_BOOK_LINE, percent);
}
uint8_t app_settings_book_margin(void) { return s_book_margin; }
void app_settings_set_book_margin(uint8_t px) {
    if (px < 24 || px > 60 || s_book_margin == px) return;
    s_book_margin = px;
    nvs_put_u8(NVS_KEY_BOOK_MARGIN, px);
}
uint8_t app_settings_book_paragraph_spacing(void) { return s_book_para; }
void app_settings_set_book_paragraph_spacing(uint8_t percent) {
    if (percent > 75 || percent % 25) return;
    if (s_book_para == percent) return;
    s_book_para = percent;
    nvs_put_u8(NVS_KEY_BOOK_PARA, percent);
}
uint8_t app_settings_shelf_style(void) { return s_shelf_style; }
void app_settings_set_shelf_style(uint8_t style) {
    if (style < 1 || style > 3 || s_shelf_style == style) return;
    s_shelf_style = style;
    nvs_put_u8(NVS_KEY_SHELF_STYLE, style);
}
const char* app_settings_books_dir(void) { return s_books_dir; }
const char* app_settings_fonts_dir(void) { return s_fonts_dir; }
bool app_settings_set_books_dir(const char* path) {
    return set_media_dir(s_books_dir, NVS_KEY_BOOKS_DIR, path);
}
bool app_settings_set_fonts_dir(const char* path) {
    return set_media_dir(s_fonts_dir, NVS_KEY_FONTS_DIR, path);
}

#ifndef APP_SETTINGS_BACKUP_ROOT
#define APP_SETTINGS_BACKUP_ROOT "/sdcard"
#endif
#define BACKUP_FILE APP_SETTINGS_BACKUP_ROOT "/Pico-settings.backup"
#define BACKUP_TEMP APP_SETTINGS_BACKUP_ROOT "/Pico-settings.backup.tmp"
#define BACKUP_PREVIOUS APP_SETTINGS_BACKUP_ROOT "/Pico-settings.backup.previous"

// All fields are bytes, so the v1 disk layout is independent of structure padding.
// 所有字段均为字节，v1 磁盘格式不依赖编译器的结构体填充。
typedef struct {
    char magic[8];
    uint8_t flags[17];
    char font[FONT_PATH_MAX];
    char system_font[FONT_PATH_MAX];
    char wallpaper[288];
    char books_dir[MEDIA_DIR_MAX];
    char fonts_dir[MEDIA_DIR_MAX];
    uint8_t checksum[4];
} settings_backup_v1_t;

enum {
    BK_SLEEP, BK_PICKUP, BK_SYS_SIZE, BK_SYS_CONTRAST, BK_LOCK,
    BK_BOOK_PX, BK_SHAKE, BK_FULL_PAGES, BK_TURN, BK_POWER_TURN,
    BK_IMMERSIVE, BK_TRACKING, BK_READING_LINE, BK_LINE_SPACING,
    BK_MARGIN, BK_PARAGRAPH, BK_SHELF,
};

static uint32_t backup_checksum(const settings_backup_v1_t *backup) {
    const uint8_t *data = (const uint8_t *)backup;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(settings_backup_v1_t, checksum); ++i)
        hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

static void backup_seal(settings_backup_v1_t *backup) {
    uint32_t value = backup_checksum(backup);
    for (int i = 0; i < 4; ++i) backup->checksum[i] = (uint8_t)(value >> (i * 8));
}

static bool backup_card_ready(void) {
    read_pico_sd_info_t info = {0};
    return read_pico_sd_get_info(&info) == ESP_OK && info.mounted;
}

esp_err_t app_settings_backup_save(void) {
    if (!backup_card_ready()) return ESP_ERR_INVALID_STATE;
    settings_backup_v1_t backup = {0};
    memcpy(backup.magic, "PICOSET1", sizeof(backup.magic));
    uint8_t *f = backup.flags;
    f[BK_SLEEP] = s_sleep;
    f[BK_PICKUP] = s_pickup_wake;
    f[BK_SYS_SIZE] = s_system_size;
    f[BK_SYS_CONTRAST] = s_system_contrast;
    f[BK_LOCK] = s_lock_style;
    f[BK_BOOK_PX] = s_book_px;
    f[BK_SHAKE] = s_book_shake;
    f[BK_FULL_PAGES] = s_reader_full_pages;
    f[BK_TURN] = s_reader_turn_effect;
    f[BK_POWER_TURN] = s_reader_power_turn;
    f[BK_IMMERSIVE] = s_reader_immersive;
    f[BK_TRACKING] = s_book_tracking;
    f[BK_READING_LINE] = s_book_reading_line;
    f[BK_LINE_SPACING] = s_book_line;
    f[BK_MARGIN] = s_book_margin;
    f[BK_PARAGRAPH] = s_book_para;
    f[BK_SHELF] = s_shelf_style;
    strlcpy(backup.font, !strcmp(s_font, "builtin") ? "" : s_font, sizeof(backup.font));
    strlcpy(backup.system_font, s_system_font, sizeof(backup.system_font));
    strlcpy(backup.wallpaper, s_wallpaper, sizeof(backup.wallpaper));
    strlcpy(backup.books_dir, s_books_dir, sizeof(backup.books_dir));
    strlcpy(backup.fonts_dir, s_fonts_dir, sizeof(backup.fonts_dir));
    backup_seal(&backup);

    FILE *file = fopen(BACKUP_TEMP, "wb");
    if (!file) return ESP_FAIL;
    bool ok = fwrite(&backup, 1, sizeof(backup), file) == sizeof(backup);
    if (ok) ok = fflush(file) == 0;
    if (ok) ok = fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    bool rotated = false;
    if (ok && remove(BACKUP_PREVIOUS) != 0 && errno != ENOENT) ok = false;
    if (ok && rename(BACKUP_FILE, BACKUP_PREVIOUS) == 0) rotated = true;
    else if (ok && errno != ENOENT) ok = false;
    if (ok && rename(BACKUP_TEMP, BACKUP_FILE) != 0) ok = false;
    if (!ok && rotated) (void)rename(BACKUP_PREVIOUS, BACKUP_FILE);
    if (ok && rotated) (void)remove(BACKUP_PREVIOUS);
    if (!ok) { (void)remove(BACKUP_TEMP); return ESP_FAIL; }
    return ESP_OK;
}

static bool backup_path_valid(const char *path, size_t capacity) {
    size_t len = strnlen(path, capacity);
    if (len == capacity) return false;
    if (!len) return true;
    if (strncmp(path, "/sdcard/", 8) || !path[8]) return false;
    for (size_t i = 8; i < len; ++i)
        if ((unsigned char)path[i] < 32 || path[i] == '\\' || path[i] == ':') return false;
    for (const char *part = path + 8; *part;) {
        const char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (!end) break;
        part = end + 1;
    }
    return true;
}

static bool backup_valid(const settings_backup_v1_t *backup) {
    const uint8_t *f = backup->flags;
    uint32_t checksum = 0;
    for (int i = 0; i < 4; ++i) checksum |= (uint32_t)backup->checksum[i] << (i * 8);
    if (memcmp(backup->magic, "PICOSET1", 8) || checksum != backup_checksum(backup)) return false;
    if (f[BK_SLEEP] > APP_SLEEP_OFF || f[BK_PICKUP] > 1 ||
        f[BK_SYS_SIZE] < 100 || f[BK_SYS_SIZE] > 140 || f[BK_SYS_SIZE] % 10 ||
        f[BK_SYS_CONTRAST] < 100 || f[BK_SYS_CONTRAST] > 140 || f[BK_SYS_CONTRAST] % 10 ||
        f[BK_LOCK] > 1 || f[BK_BOOK_PX] < 36 || f[BK_BOOK_PX] > 72 ||
        f[BK_SHAKE] > 1 || (f[BK_FULL_PAGES] != 5 && f[BK_FULL_PAGES] != 10 && f[BK_FULL_PAGES] != 15) ||
        f[BK_TURN] > 1 || f[BK_POWER_TURN] > 1 || f[BK_IMMERSIVE] > 1 ||
        f[BK_TRACKING] > 4 || f[BK_READING_LINE] > 2 ||
        f[BK_LINE_SPACING] < 110 || f[BK_LINE_SPACING] > 150 ||
        f[BK_MARGIN] < 24 || f[BK_MARGIN] > 60 ||
        f[BK_PARAGRAPH] > 75 || f[BK_PARAGRAPH] % 25 ||
        f[BK_SHELF] < 1 || f[BK_SHELF] > 3) return false;
    if (strnlen(backup->books_dir, sizeof(backup->books_dir)) == sizeof(backup->books_dir) ||
        strnlen(backup->fonts_dir, sizeof(backup->fonts_dir)) == sizeof(backup->fonts_dir)) return false;
    return backup_path_valid(backup->font, sizeof(backup->font)) &&
           backup_path_valid(backup->system_font, sizeof(backup->system_font)) &&
           backup_path_valid(backup->wallpaper, sizeof(backup->wallpaper)) &&
           valid_media_dir(backup->books_dir) && valid_media_dir(backup->fonts_dir);
}

static bool backup_file_exists(const char *path, bool directory) {
    struct stat st;
    return stat(path, &st) == 0 && (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}

esp_err_t app_settings_backup_restore(void) {
    if (!backup_card_ready()) return ESP_ERR_INVALID_STATE;
    FILE *file = fopen(BACKUP_FILE, "rb");
    if (!file) file = fopen(BACKUP_PREVIOUS, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    settings_backup_v1_t backup;
    bool ok = fread(&backup, 1, sizeof(backup), file) == sizeof(backup);
    if (ok) ok = fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0) ok = false;
    if (!ok || !backup_valid(&backup)) return ESP_ERR_INVALID_RESPONSE;

    // The backup contains paths, not the corresponding font or image bytes.
    // 备份只含资源路径；资源已被删除时回退到安全的内建选项。
    if (backup.font[0] && !backup_file_exists(backup.font, false)) backup.font[0] = 0;
    if (backup.system_font[0] && !backup_file_exists(backup.system_font, false)) backup.system_font[0] = 0;
    if (backup.wallpaper[0] && !backup_file_exists(backup.wallpaper, false)) {
        backup.wallpaper[0] = 0;
        backup.flags[BK_LOCK] = 0;
    }
    if (!backup.wallpaper[0]) backup.flags[BK_LOCK] = 0;
    if (!backup_file_exists(backup.books_dir, true)) strlcpy(backup.books_dir, "/sdcard/books", sizeof(backup.books_dir));
    if (!backup_file_exists(backup.fonts_dir, true)) strlcpy(backup.fonts_dir, "/sdcard/fonts", sizeof(backup.fonts_dir));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
#define BACKUP_SET_U8(key, index) do { if (err == ESP_OK) err = nvs_set_u8(h, key, backup.flags[index]); } while (0)
#define BACKUP_SET_STR(key, value) do { if (err == ESP_OK) err = nvs_set_str(h, key, value); } while (0)
    BACKUP_SET_U8(NVS_KEY_SLEEP, BK_SLEEP);
    BACKUP_SET_U8(NVS_KEY_PICKUP, BK_PICKUP);
    BACKUP_SET_U8(NVS_KEY_SYS_SIZE, BK_SYS_SIZE);
    BACKUP_SET_U8(NVS_KEY_SYS_CONTRAST, BK_SYS_CONTRAST);
    BACKUP_SET_U8(NVS_KEY_LOCK_STYLE, BK_LOCK);
    BACKUP_SET_U8(NVS_KEY_BOOK_PX, BK_BOOK_PX);
    BACKUP_SET_U8(NVS_KEY_BOOK_SHAKE, BK_SHAKE);
    BACKUP_SET_U8(NVS_KEY_READER_FULL, BK_FULL_PAGES);
    BACKUP_SET_U8(NVS_KEY_READER_TURN, BK_TURN);
    BACKUP_SET_U8(NVS_KEY_POWER_TURN, BK_POWER_TURN);
    BACKUP_SET_U8(NVS_KEY_IMMERSIVE, BK_IMMERSIVE);
    BACKUP_SET_U8(NVS_KEY_BOOK_TRACK, BK_TRACKING);
    BACKUP_SET_U8(NVS_KEY_BOOK_RULE, BK_READING_LINE);
    BACKUP_SET_U8(NVS_KEY_BOOK_LINE, BK_LINE_SPACING);
    BACKUP_SET_U8(NVS_KEY_BOOK_MARGIN, BK_MARGIN);
    BACKUP_SET_U8(NVS_KEY_BOOK_PARA, BK_PARAGRAPH);
    BACKUP_SET_U8(NVS_KEY_SHELF_STYLE, BK_SHELF);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_SHELF_V22, 1);
    BACKUP_SET_STR(NVS_KEY_FONT, backup.font);
    BACKUP_SET_STR(NVS_KEY_SYS_FONT, backup.system_font);
    BACKUP_SET_STR(NVS_KEY_WALLPAPER, backup.wallpaper);
    BACKUP_SET_STR(NVS_KEY_BOOKS_DIR, backup.books_dir);
    BACKUP_SET_STR(NVS_KEY_FONTS_DIR, backup.fonts_dir);
#undef BACKUP_SET_U8
#undef BACKUP_SET_STR
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    const uint8_t *f = backup.flags;
    s_sleep = (app_sleep_mode_t)f[BK_SLEEP];
    s_pickup_wake = f[BK_PICKUP];
    s_system_size = f[BK_SYS_SIZE];
    s_system_contrast = f[BK_SYS_CONTRAST];
    s_lock_style = f[BK_LOCK];
    s_book_px = f[BK_BOOK_PX];
    s_book_shake = f[BK_SHAKE];
    s_reader_full_pages = f[BK_FULL_PAGES];
    s_reader_turn_effect = f[BK_TURN];
    s_reader_power_turn = f[BK_POWER_TURN];
    s_reader_immersive = f[BK_IMMERSIVE];
    s_book_tracking = f[BK_TRACKING];
    s_book_reading_line = f[BK_READING_LINE];
    s_book_line = f[BK_LINE_SPACING];
    s_book_margin = f[BK_MARGIN];
    s_book_para = f[BK_PARAGRAPH];
    s_shelf_style = f[BK_SHELF];
    strlcpy(s_font, backup.font, sizeof(s_font));
    strlcpy(s_system_font, backup.system_font, sizeof(s_system_font));
    strlcpy(s_wallpaper, backup.wallpaper, sizeof(s_wallpaper));
    strlcpy(s_books_dir, backup.books_dir, sizeof(s_books_dir));
    strlcpy(s_fonts_dir, backup.fonts_dir, sizeof(s_fonts_dir));
    return ESP_OK;
}
