/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：分组设置页；网络短时对时后 PMU 持续走时，开机恢复系统时钟。
 * English: Grouped settings; temporary WiFi sync seeds the PMU, whose RTC restores time at boot.
 */
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "app_content_open.h"
#include "app_registry.h"
#include "app_transfer_mode.h"
#include "settings.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "read_pico_transfer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "pmu_selftest.h"
#include "soc/rtc_cntl_reg.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "ui_wallpaper.h"
#include "../assets/app_icons.h"

static char s_notice[96];
typedef enum { SETTINGS_MAIN, SETTINGS_WIFI, SETTINGS_TIME,
               SETTINGS_TIME_EDIT, SETTINGS_SHELF_STYLE, SETTINGS_SYSTEM_FONT,
               SETTINGS_SYSTEM_SIZE, SETTINGS_SYSTEM_CONTRAST, SETTINGS_LOCK_STYLE,
               SETTINGS_WALLPAPER, SETTINGS_WALLPAPER_PREVIEW,
               SETTINGS_FIRMWARE } settings_page_t;
static settings_page_t s_page;
static bool s_boot_pending;
static int s_style_scroll;
static int s_font_page, s_wallpaper_page;
static bool s_sync_pending;
static const char *const TAG = "device_settings";
static const char *system_font_label(const char *path) {
    if (!path || !path[0]) return "思源黑体";
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    static char stem[TTF_FONT_NAME_MAX];
    snprintf(stem, sizeof(stem), "%s", name);
    size_t len = strlen(stem);
    if (len >= 4 && !strcasecmp(stem + len - 4, ".ttf")) stem[len - 4] = 0;
    if (!strcasecmp(stem, "Hei")) return "思源黑体（TF 卡）";
    return ttf_font_localized_name(stem);
}
static int s_year, s_month, s_day, s_hour, s_minute;
#define WALLPAPER_MAX 64
typedef struct { char path[288]; char name[96]; } wallpaper_item_t;
static wallpaper_item_t s_wallpapers[WALLPAPER_MAX];
static int s_wallpaper_count;
static int s_wallpaper_selected;
static bool s_wallpaper_confirm, s_wallpaper_preview_ok;

static void wallpaper_scan_dir(const char *root) {
    DIR *dir = opendir(root);
    if (!dir) return;
    struct dirent *entry;
    while (s_wallpaper_count < WALLPAPER_MAX && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.' || !strncmp(entry->d_name, "._", 2)) continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".png"))) continue;
        wallpaper_item_t *item = &s_wallpapers[s_wallpaper_count];
        if (snprintf(item->path, sizeof(item->path), "%s/%s", root, entry->d_name) >= sizeof(item->path)) continue;
        struct stat st;
        if (stat(item->path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 2 * 1024 * 1024) continue;
        size_t name_len = strnlen(entry->d_name, sizeof(item->name) - 1);
        memcpy(item->name, entry->d_name, name_len);
        item->name[name_len] = 0;
        ++s_wallpaper_count;
    }
    closedir(dir);
}
static void wallpaper_scan(void) {
    s_wallpaper_count = s_wallpaper_page = 0;
    wallpaper_scan_dir("/sdcard/pictures");
    wallpaper_scan_dir("/sdcard/images");
    wallpaper_scan_dir("/sdcard");
}

static int days_in_month(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return 29;
    return days[month - 1];
}

// 公历日期转 UTC 天数；界面统一按北京时间 UTC+8 编辑。
// Convert a Gregorian date to UTC days; the editor uses Beijing time (UTC+8).
static int64_t civil_days(int year, int month, int day) {
    year -= month <= 2;
    int era = year / 400;
    int yoe = year - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static void time_open(void) {
    time_t now = time(NULL);
    if (now < 1704067200) {
        const pmu_snapshot_t *pmu = read_pico_pmu_get();
        if (pmu && pmu->time_synced && pmu->unix_sec >= 1704067200) now = pmu->unix_sec;
    }
    if (now < 1704067200) {
        char month[4] = "Jan"; int day = 1, year = 2024;
        if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3) {
            strcpy(month, "Jan"); day = 1; year = 2024;
        }
        static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        int m = 1; while (m < 12 && strcmp(month, months[m - 1])) ++m;
        s_year = year; s_month = m; s_day = day; s_hour = s_minute = 0;
    } else {
        struct tm date;
        time_t local = now + 8 * 3600;
        gmtime_r(&local, &date);
        s_year = date.tm_year + 1900; s_month = date.tm_mon + 1; s_day = date.tm_mday;
        s_hour = date.tm_hour; s_minute = date.tm_min;
    }
    s_notice[0] = 0;
    s_page = SETTINGS_TIME_EDIT;
}

static void time_adjust(int field, int step) {
    if (field == 0) s_year = s_year + step > 2099 ? 2024 : s_year + step < 2024 ? 2099 : s_year + step;
    if (field == 1) s_month = s_month + step > 12 ? 1 : s_month + step < 1 ? 12 : s_month + step;
    if (field == 2) s_day = s_day + step > days_in_month(s_year, s_month) ? 1 :
                            s_day + step < 1 ? days_in_month(s_year, s_month) : s_day + step;
    if (field == 3) s_hour = (s_hour + step + 24) % 24;
    if (field == 4) s_minute = (s_minute + step + 60) % 60;
    if (s_day > days_in_month(s_year, s_month)) s_day = days_in_month(s_year, s_month);
}

static bool time_save(void) {
    int64_t epoch = civil_days(s_year, s_month, s_day) * 86400 +
                    s_hour * 3600 + s_minute * 60 - 8 * 3600;
    if (epoch < 1704067200 || epoch > 4102444799LL) return false;
    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    if (settimeofday(&tv, NULL)) return false;
    uint8_t payload[4] = {(uint8_t)epoch, (uint8_t)(epoch >> 8),
                          (uint8_t)(epoch >> 16), (uint8_t)(epoch >> 24)};
    esp_err_t err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
    if (err == ESP_OK) err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (err != ESP_OK || !pmu || !pmu->time_synced) {
        snprintf(s_notice, sizeof(s_notice), "系统时间已设置，PMU 同步失败");
        return false;
    }
    snprintf(s_notice, sizeof(s_notice), "时间已保存，并同步到电源管理芯片");
    s_page = SETTINGS_TIME;
    return true;
}

static void row(uint8_t *fb, int y, const char *label, const char *value) {
    ui_text(fb, 54, y + 17, 27, label, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 627, y + 20, 21, value, EPD_DRAW_ALIGN_RIGHT, false);
    ui_hairline(fb, y + 68, 54, 575, UI_GRAY_LIGHT);
}
static void section(uint8_t *fb, int y, const char *title) {
    ui_text(fb, 40, y, 22, title, EPD_DRAW_ALIGN_LEFT, false);
}
static void back_header(uint8_t *fb, const char *title) {
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, title, EPD_DRAW_ALIGN_CENTER, false);
    ui_hairline(fb, 174, 36, 612, UI_GRAY_LIGHT);
}

static void setting_icon(uint8_t *fb, int index, int cx, int cy) {
    if (index == 0) {
        ui_nav_wifi_icon(fb, cx, cy, 32, 0x6a);
        return;
    }
    if (index == 7) {
        epd_draw_circle(cx, cy, 13, 0x50, fb);
        epd_fill_rect((EpdRect){cx - 13, cy, 26, 13}, 0x50, fb);
        epd_draw_line(cx - 18, cy, cx - 15, cy, 0x50, fb);
        epd_draw_line(cx + 15, cy, cx + 18, cy, 0x50, fb);
        epd_draw_line(cx, cy - 18, cx, cy - 15, 0x50, fb);
        epd_draw_line(cx, cy + 15, cx, cy + 18, 0x50, fb);
        return;
    }
    const uint8_t *image = pico_setting_icons[index];
    for (int y = 0; y < PICO_SETTING_ICON_SIZE; ++y)
        for (int x = 0; x < PICO_SETTING_ICON_SIZE; ++x) {
            int pixel = y * PICO_SETTING_ICON_SIZE + x;
            uint8_t byte = image[pixel / 2];
            unsigned gray = pixel & 1 ? byte & 15 : byte >> 4;
            if (gray < 15)
                epd_draw_pixel(cx - 16 + x, cy - 16 + y, ui_contrast_gray((uint8_t)(gray << 4)), fb);
        }
}

static void fit_value(char *value, int width) {
    while (*value && ttf_text_width_px(ui_text_effective_px(20), value) > width) {
        size_t n = strlen(value) - 1;
        while (n && ((unsigned char)value[n] & 0xc0) == 0x80) --n;
        value[n] = 0;
    }
}

static void setting_group(uint8_t *fb, int title_y, const char *title,
                          int card_y, const int icons[], const char *const labels[],
                          const char *const values[], int count) {
    ui_text(fb, 42, title_y, 20, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_fill_round_rect(fb, (EpdRect){36, card_y, 612, count * 74}, 22, UI_GRAY_WHITE);
    for (int i = 0; i < count; ++i) {
        int y = card_y + i * 74;
        if (i) ui_hairline(fb, y, 88, 544, 0xd0);
        setting_icon(fb, icons[i], 67, y + 37);
        ui_text(fb, 100, y + 22, 25, labels[i], EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 619, y + 25, 20, values[i], EPD_DRAW_ALIGN_RIGHT, false);
    }
}

static void draw_style_thumbnail(uint8_t *fb, int style, int top) {
    if (top < 242 || top + 170 >= UI_NAV_TOP) return;
    const int left = 99;
    for (int i = 0; i < 3; ++i) {
        int x = left + 20 + i * 155;
        EpdRect cover = {x, top, 116, 145};
        epd_fill_rect(cover, i == 1 ? 0x60 : i == 2 ? 0xa0 : 0x40, fb);
        ui_draw_round_rect(fb, cover, 1, 0x30);
        ui_draw_round_rect(fb, (EpdRect){x + 7, top + 7, 102, 130}, 1, i == 1 ? 0xc0 : 0xe0);
        ui_hairline(fb, top + 47, x + 28, 60, i == 1 ? 0xc0 : 0xe0);
        ui_hairline(fb, top + 54, x + 39, 38, i == 1 ? 0xc0 : 0xe0);
        ui_hairline(fb, top + 111, x + 44, 28, i == 1 ? 0xc0 : 0xe0);
    }
    if (style == 1) {
        epd_fill_rect((EpdRect){left, top + 143, 486, 16}, 0x30, fb);
        ui_hairline(fb, top + 143, left, 486, 0x80);
    } else if (style == 2) {
        ui_draw_acrylic_guard(fb, (EpdRect){left, top + 94, 486, 70});
    } else {
        for (int i = 0; i < 3; ++i)
            ui_draw_frosted_pocket(fb, (EpdRect){left + 8 + i * 155, top + 65, 143, 100});
    }
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (s_page == SETTINGS_MAIN || s_page == SETTINGS_SHELF_STYLE)
        epd_fill_rect((EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_TOP}, 0xe0, fb);
    ui_nav_status(fb);
    if (s_page == SETTINGS_SHELF_STYLE) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "书架样式", EPD_DRAW_ALIGN_CENTER, false);
        ui_text(fb, 36, 207, 23, "每页最多显示 9 本书", EPD_DRAW_ALIGN_LEFT, false);
        static const char *const styles[] = {"深色书轨", "亚克力书架", "半透明书袋"};
        static const char *const descriptions[] = {"封面落在书轨上", "透明亚克力挡板", "每本独立透明书袋"};
        for (int i = 0; i < 3; ++i) {
            int y = 263 + i * 253 - s_style_scroll;
            if (y + 230 < 242 || y > 1095) continue;
            EpdRect card = {36, y, 612, 230};
            int style = i + 1;
            bool active = app_settings_shelf_style() == style;
            ui_fill_round_rect(fb, card, 24, active ? 0xd0 : UI_GRAY_WHITE);
            ui_draw_round_rect(fb, card, 24, active ? 0x90 : 0xd0);
            draw_style_thumbnail(fb, style, y + 11);
            if (y + 190 < 1096) ui_text(fb, 68, y + 188, 23, styles[i], EPD_DRAW_ALIGN_LEFT, false);
            if (y + 195 < 1096) ui_text(fb, 440, y + 192, 17, descriptions[i], EPD_DRAW_ALIGN_LEFT, false);
            if (y + 220 < 1096) {
                epd_draw_circle(609, y + 207, 13, UI_GRAY_BLACK, fb);
                if (active) epd_fill_circle(609, y + 207, 6, UI_GRAY_BLACK, fb);
            }
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_TIME_EDIT) {
        ui_nav_back(fb, 36, 79);
        ui_text(fb, 36, 151, 47, "日期与时间", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 36, 216, 23, "北京时间 · UTC+8", EPD_DRAW_ALIGN_LEFT, false);
        ui_hairline(fb, 259, 36, 612, UI_GRAY_LIGHT);
        static const char *const labels[] = {"年", "月", "日", "时", "分"};
        int values[] = {s_year, s_month, s_day, s_hour, s_minute};
        for (int i = 0; i < 5; ++i) {
            int x = 36 + i * 122;
            ui_text(fb, x + 52, 300, 24, labels[i], EPD_DRAW_ALIGN_CENTER, false);
            ui_draw_round_rect(fb, (EpdRect){x, 354, 106, 62}, 8, UI_GRAY_BLACK);
            ui_text_vc(fb, x + 53, 385, 30, "+", EPD_DRAW_ALIGN_CENTER, false);
            char value[8]; snprintf(value, sizeof(value), i == 0 ? "%04d" : "%02d", values[i]);
            ui_text(fb, x + 53, 456, i == 0 ? 34 : 39, value, EPD_DRAW_ALIGN_CENTER, false);
            ui_draw_round_rect(fb, (EpdRect){x, 535, 106, 62}, 8, UI_GRAY_BLACK);
            ui_text_vc(fb, x + 53, 566, 30, "−", EPD_DRAW_ALIGN_CENTER, false);
        }
        ui_text(fb, 36, 649, 23, "点上方 + 或下方 − 调整数字", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){36, 767, 612, 78}, "保存时间", false);
        ui_draw_button(fb, (EpdRect){36, 874, 612, 78}, "取消", false);
        if (s_notice[0]) ui_text(fb, 36, 996, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    if (s_page == SETTINGS_WIFI) {
        back_header(fb, "WiFi");
        char ssid[33] = {0}; bool saved = false;
        (void)read_pico_transfer_get_saved_wifi(ssid, &saved);
        section(fb, 253, "已保存网络");
        row(fb, 296, saved ? ssid : "尚未配置", saved ? "已保存" : "");
        row(fb, 408, "连接或更改网络", "前往传输  ›");
        ui_text(fb, 54, 529, 21, "配网后可在日期与时间中进行 WiFi 对时。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_FONT) {
        back_header(fb, "系统字体");
        section(fb, 244, "只影响首页、书架和设置等系统界面");
        const char *selected = app_settings_system_font_path();
        int count = ttf_font_count();
        int first = s_font_page * 8;
        for (int slot = 0; slot < 8; ++slot) {
            int item_index = first + slot;
            if (item_index > count) break;
            EpdRect box = {36, 292 + slot * 92, 612, 78};
            const ttf_font_item_t *item = item_index ? ttf_font_item(item_index - 1) : NULL;
            bool active = item ? !strcmp(selected, item->path) : !selected[0];
            ui_fill_round_rect(fb, box, 18, active ? 0xd0 : UI_GRAY_WHITE);
            ui_draw_round_rect(fb, box, 18, active ? 0x90 : 0xd0);
            ui_text_vc(fb, 60, box.y + 39, 26,
                       item ? system_font_label(item->path) : "思源黑体（内建）", EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 39, 6, UI_GRAY_BLACK, fb);
        }
        char pages[32]; snprintf(pages, sizeof(pages), "%d / %d", s_font_page + 1, (count + 8) / 8);
        ui_text(fb, 648, 1050, 20, pages, EPD_DRAW_ALIGN_RIGHT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_SIZE) {
        back_header(fb, "系统字号");
        section(fb, 248, "界面文字大小");
        for (int i = 0; i < 5; ++i) {
            int value = 100 + i * 10;
            EpdRect box = {36, 298 + i * 116, 612, 90};
            bool active = app_settings_system_font_size() == value;
            ui_fill_round_rect(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE);
            char label[48]; snprintf(label, sizeof(label), "%d%%", value);
            ui_text_vc(fb, 64, box.y + 45, 29, label, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(605, box.y + 45, 8, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_CONTRAST) {
        back_header(fb, "系统对比度");
        section(fb, 248, "调整系统界面的灰阶层次与清晰度");
        static const char *const names[] = {"柔和", "标准", "清晰", "鲜明", "高对比"};
        for (int i = 0; i < 5; ++i) {
            int value = 100 + i * 10;
            EpdRect box = {36, 298 + i * 116, 612, 90};
            bool active = app_settings_system_contrast() == value;
            ui_fill_round_rect(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE);
            ui_text_vc(fb, 64, box.y + 45, 28, names[i], EPD_DRAW_ALIGN_LEFT, false);
            char shown[16]; snprintf(shown, sizeof(shown), "%d%%", value);
            ui_text_vc(fb, 570, box.y + 45, 22, shown, EPD_DRAW_ALIGN_RIGHT, false);
            if (active) epd_fill_circle(605, box.y + 45, 8, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_LOCK_STYLE) {
        back_header(fb, "锁屏样式");
        section(fb, 248, "选择电源键锁屏后的画面");
        const char *labels[] = {"壁纸锁屏", "阅读票根"};
        const char *details[] = {"使用 TF 卡中的 JPG / PNG 图片", "书封、进度和阅读记录"};
        const uint8_t values[] = {1, 0};
        for (int i = 0; i < 2; ++i) {
            EpdRect box = {36, 300 + i * 154, 612, 132};
            bool active = app_settings_lock_style() == values[i];
            ui_fill_round_rect(fb, box, 22, active ? 0xd0 : UI_GRAY_WHITE);
            ui_text(fb, 64, box.y + 23, 31, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 64, box.y + 79, 20, details[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 66, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 54, 659, 22, "选择壁纸后，可继续从 TF 卡更换图片", EPD_DRAW_ALIGN_LEFT, false);
        if (app_settings_wallpaper_path()[0])
            row(fb, 710, "当前壁纸", strrchr(app_settings_wallpaper_path(), '/') + 1);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_WALLPAPER) {
        back_header(fb, "选择壁纸");
        section(fb, 244, "TF 卡图片 · JPG / PNG · 不超过 2 MB");
        if (!s_wallpaper_count)
            ui_text(fb, 54, 325, 25, "未找到图片，请放入 TF 卡的 pictures 文件夹", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < 8; ++i) {
            int index = s_wallpaper_page * 8 + i;
            if (index >= s_wallpaper_count) break;
            EpdRect box = {36, 292 + i * 92, 612, 78};
            bool active = !strcmp(s_wallpapers[index].path, app_settings_wallpaper_path());
            ui_fill_round_rect(fb, box, 18, active ? 0xd0 : UI_GRAY_WHITE);
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[index].name);
            fit_value(name, 490);
            ui_text_vc(fb, 60, box.y + 39, 24, name, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 39, 6, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW) {
        back_header(fb, "预览壁纸");
        EpdRect image = {166, 194, 352, 626};
        ui_fill_round_rect(fb, image, 8, UI_GRAY_WHITE);
        s_wallpaper_preview_ok = s_wallpaper_selected >= 0 && s_wallpaper_selected < s_wallpaper_count &&
            ui_wallpaper_draw(fb, s_wallpapers[s_wallpaper_selected].path, image);
        ui_draw_round_rect(fb, image, 8, UI_GRAY_LIGHT);
        if (!s_wallpaper_preview_ok)
            ui_text_vc(fb, 342, 507, 23, "图片无法预览", EPD_DRAW_ALIGN_CENTER, false);
        if (s_wallpaper_selected >= 0 && s_wallpaper_selected < s_wallpaper_count) {
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[s_wallpaper_selected].name);
            fit_value(name, 570);
            ui_text_vc(fb, 342, 859, 24, name, EPD_DRAW_ALIGN_CENTER, false);
            char position[40];
            snprintf(position, sizeof(position), "%d / %d · 上下滑动切换", s_wallpaper_selected + 1, s_wallpaper_count);
            ui_text_vc(fb, 342, 908, 20, position, EPD_DRAW_ALIGN_CENTER, false);
        }
        if (s_wallpaper_preview_ok) {
            EpdRect button = {94, 962, 496, 70};
            ui_fill_round_rect(fb, button, 22, 0x30);
            ui_text_vc(fb, 342, 997, 25, "设为锁屏壁纸", EPD_DRAW_ALIGN_CENTER, true);
        }
        if (s_wallpaper_confirm) {
            EpdRect dialog = {68, 435, 548, 306};
            ui_fill_round_rect(fb, dialog, 26, UI_GRAY_WHITE);
            ui_draw_round_rect(fb, dialog, 26, 0x50);
            ui_text_vc(fb, 342, 503, 31, "设为壁纸锁屏？", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 342, 565, 20, "确认后，短按电源键将显示此图片", EPD_DRAW_ALIGN_CENTER, false);
            EpdRect cancel = {94, 643, 232, 68}, confirm = {358, 643, 232, 68};
            ui_fill_round_rect(fb, cancel, 18, 0xe0);
            ui_fill_round_rect(fb, confirm, 18, 0x30);
            ui_text_vc(fb, 210, 677, 24, "取消", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 474, 677, 24, "确认", EPD_DRAW_ALIGN_CENTER, true);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_TIME) {
        back_header(fb, "日期与时间");
        time_t now = time(NULL);
        char shown[64] = "未设置";
        if (now >= 1704067200) {
            struct tm tm; time_t local = now + 8 * 3600;
            if (gmtime_r(&local, &tm)) snprintf(shown, sizeof(shown), "%04d-%02d-%02d  %02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        }
        section(fb, 253, "当前时间 · 北京时间");
        row(fb, 296, shown, "");
        row(fb, 407, "通过 WiFi 对时", "立即同步  ›");
        row(fb, 518, "手动设置", "编辑  ›");
        ui_text(fb, 54, 656, 21, "对时成功后由内部时钟持续走时；无需保持联网。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 694, 21, "长时间离线可能产生误差，建议定期重新对时。", EPD_DRAW_ALIGN_LEFT, false);
        if (s_notice[0]) ui_text(fb, 54, 772, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_FIRMWARE) {
        back_header(fb, "固件升级");
        section(fb, 249, "从电脑刷写 Pico");
        ui_fill_round_rect(fb, (EpdRect){36, 297, 612, 286}, 22, UI_GRAY_WHITE);
        ui_text(fb, 61, 327, 29, "进入 BOOT 模式", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 61, 391, 22, "连接电脑，在 Chrome 或 Edge 打开", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 61, 433, 22, "Pico 的 GitHub 网页刷机页。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 61, 502, 20, "进入后，屏幕会停留在当前画面。", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){36, 638, 612, 80},
                       s_boot_pending ? "正在进入 BOOT 模式" : "进入 BOOT 模式", false);
        ui_text(fb, 54, 770, 20, "刷写完成后，Pico 会重新启动。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    ui_text(fb, 36, 91, 52, "设置", EPD_DRAW_ALIGN_LEFT, false);
    ui_fill_round_rect(fb, (EpdRect){36, 171, 612, 127}, 24, UI_GRAY_WHITE);
    ui_fill_round_rect(fb, (EpdRect){57, 192, 82, 84}, 20, 0x30);
    ui_text(fb, 98, 207, 49, "P", EPD_DRAW_ALIGN_CENTER, true);
    ui_text(fb, 164, 198, 30, "Pico", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 164, 241, 19, "墨水屏阅读器 · 684 × 1216", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 618, 241, 19, "升级  ›", EPD_DRAW_ALIGN_RIGHT, false);
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (pmu && pmu->soc_permille <= 1000) {
        char battery[12]; snprintf(battery, sizeof(battery), "%u%%", (unsigned)pmu->soc_permille / 10);
        ui_text(fb, 618, 205, 22, battery, EPD_DRAW_ALIGN_RIGHT, false);
    }
    char wifi_ssid[33] = {0}; bool wifi_saved = false;
    (void)read_pico_transfer_get_saved_wifi(wifi_ssid, &wifi_saved);
    const char *wireless_labels[] = {"WiFi"};
    char wireless_value[56]; snprintf(wireless_value, sizeof(wireless_value), "%s  ›", wifi_saved ? wifi_ssid : "未配置");
    fit_value(wireless_value, 235);
    const char *wireless_values[] = {wireless_value};
    static const int wireless_icons[] = {0};
    setting_group(fb, 324, "无线连接", 365, wireless_icons, wireless_labels, wireless_values, 1);
    const char *reading_labels[] = {"系统字体", "系统字号", "系统对比度", "书架样式"};
    char font[96];
    const char *chosen_font = app_settings_system_font_path();
    snprintf(font, sizeof(font), "%s  ›", system_font_label(chosen_font));
    fit_value(font, 235);
    char size[32]; snprintf(size, sizeof(size), "%u%%  ›", app_settings_system_font_size());
    char contrast[32]; snprintf(contrast, sizeof(contrast), "%u%%  ›", app_settings_system_contrast());
    static const char *const styles[] = {"深色书轨  ›", "深色书轨  ›", "亚克力书架  ›", "半透明书袋  ›"};
    const char *reading_values[] = {font, size, contrast, styles[app_settings_shelf_style()]};
    static const int reading_icons[] = {2, 3, 7, 4};
    setting_group(fb, 469, "显示", 510, reading_icons, reading_labels, reading_values, 4);
    const char *display_labels[] = {"锁屏样式", "日期与时间"};
    const char *display_values[] = {app_settings_lock_style() ? "壁纸  ›" : "阅读票根  ›", "设置  ›"};
    static const int display_icons[] = {5, 6};
    setting_group(fb, 836, "锁屏与时间", 877, display_icons, display_labels, display_values, 2);
    ui_nav_draw(fb, 3);
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_notice[0] = 0;
    s_boot_pending = false;
    s_page = SETTINGS_MAIN;
    s_style_scroll = s_font_page = s_wallpaper_page = 0;
    s_wallpaper_selected = -1;
    s_wallpaper_confirm = s_wallpaper_preview_ok = false;
    s_sync_pending = false;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    (void)ctx;
    if (s_boot_pending) {
        s_boot_pending = false;
        pmu_selftest_prepare_powerdown();
        uint8_t req[2] = {0, 0};
        esp_err_t err = read_pico_pmu_cmd(PMU_CMD_HOST_REQUEST_RESET, req, sizeof(req));
        if (err != ESP_OK) ESP_LOGW(TAG, "BOOT PMU notice: %s", esp_err_to_name(err));
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
        return APP_REDRAW_NONE;
    }
    if (!s_sync_pending) return APP_REDRAW_NONE;
    s_sync_pending = false;
    uint32_t utc = 0;
    read_pico_transfer_status_t network = {0};
    read_pico_transfer_get_status(&network);
    esp_err_t err = read_pico_transfer_sync_time(&utc);
    bool network_time_received = err == ESP_OK;
    if (network_time_received) {
        uint8_t payload[4] = {(uint8_t)utc, (uint8_t)(utc >> 8), (uint8_t)(utc >> 16), (uint8_t)(utc >> 24)};
        err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
        if (err == ESP_OK) err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
        if (err == ESP_OK) {
            const pmu_snapshot_t *pmu = read_pico_pmu_get();
            if (!pmu || !pmu->time_synced || pmu->unix_sec < utc || pmu->unix_sec - utc > 10)
                err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_LOGI(TAG, "time sync network=%d ready=%d utc=%lu result=%s",
             network.mode, network.network_ready, (unsigned long)utc, esp_err_to_name(err));
    const char *notice = err == ESP_OK ? "WiFi 对时完成，内部时钟已接管" :
        network_time_received ? "已获取网络时间，内部时钟写入失败" :
        err == ESP_ERR_NOT_FOUND ? "请先在 WiFi 设置中配置网络" :
        err == ESP_ERR_INVALID_STATE ? "WiFi 尚未连接，请稍后再试" :
        err == ESP_ERR_TIMEOUT ? "网络对时超时，请检查网络后重试" :
        "网络对时失败，请检查网络后重试";
    snprintf(s_notice, sizeof(s_notice), "%s", notice);
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_page == SETTINGS_WALLPAPER_PREVIEW && !s_wallpaper_confirm &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int next = s_wallpaper_selected + (ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
        if (next < 0 || next >= s_wallpaper_count) return APP_REDRAW_NONE;
        s_wallpaper_selected = next;
        s_wallpaper_page = next / 8;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_SHELF_STYLE && (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D))
        return APP_REDRAW_NONE;
    if ((s_page == SETTINGS_SYSTEM_FONT || s_page == SETTINGS_WALLPAPER) &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int *page = s_page == SETTINGS_SYSTEM_FONT ? &s_font_page : &s_wallpaper_page;
        int count = s_page == SETTINGS_SYSTEM_FONT ? ttf_font_count() + 1 : s_wallpaper_count;
        int pages = count > 0 ? (count + 7) / 8 : 1;
        if (ev->type == UI_GESTURE_SWIPE_U && *page + 1 < pages) ++*page;
        if (ev->type == UI_GESTURE_SWIPE_D && *page > 0) --*page;
        return APP_REDRAW_PAGE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (s_page == SETTINGS_WALLPAPER_PREVIEW && s_wallpaper_confirm) {
        if (ev->y0 >= 643 && ev->y0 < 711 && ev->x0 >= 358 && ev->x0 < 590) {
            const char *path = s_wallpapers[s_wallpaper_selected].path;
            app_settings_set_wallpaper_path(path);
            if (!strcmp(app_settings_wallpaper_path(), path)) {
                app_settings_set_lock_style(1);
                s_page = SETTINGS_LOCK_STYLE;
            }
        }
        s_wallpaper_confirm = false;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_TIME_EDIT) {
        if (ev->y0 < 190) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
        if (ev->y0 >= 354 && ev->y0 < 597) {
            int field = (ev->x0 - 36) / 122;
            if (ev->x0 >= 36 && field >= 0 && field < 5 && (ev->x0 - 36) % 122 < 106) {
                if (ev->y0 < 416) time_adjust(field, 1);
                else if (ev->y0 >= 535) time_adjust(field, -1);
                else return APP_REDRAW_NONE;
                return APP_REDRAW_PAGE;
            }
        }
        if (ev->y0 >= 767 && ev->y0 < 845) { (void)time_save(); return APP_REDRAW_PAGE; }
        if (ev->y0 >= 874 && ev->y0 < 952) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    int tab = ui_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    int y = ev->y0;
    if (s_page == SETTINGS_WALLPAPER_PREVIEW) {
        if (y < 190) { s_page = SETTINGS_WALLPAPER; return APP_REDRAW_PAGE; }
        if (s_wallpaper_preview_ok && y >= 194 && y < 1032) {
            s_wallpaper_confirm = true;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_WALLPAPER && y < 190) {
        s_page = SETTINGS_LOCK_STYLE;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_SHELF_STYLE) {
        if (y < 190) { s_page = SETTINGS_MAIN; return APP_REDRAW_PAGE; }
        for (int i = 0; i < 3; ++i) {
            if (y >= 263 + i * 253 - s_style_scroll && y < 493 + i * 253 - s_style_scroll && y < 1096) {
                app_settings_set_shelf_style((uint8_t)(i + 1));
                s_page = SETTINGS_MAIN;
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
    if (s_page != SETTINGS_MAIN && y < 190) { s_page = SETTINGS_MAIN; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_WIFI) {
        if (y >= 408 && y < 478) {
            extern const app_desc_t app_transfer;
            app_transfer_request_wifi_setup();
            ctx->request_app = &app_transfer;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SYSTEM_FONT) {
        if (y < 292 || y >= 1028) return APP_REDRAW_NONE;
        int index = s_font_page * 8 + (y - 292) / 92;
        int count = ttf_font_count();
        if (index > count) return APP_REDRAW_NONE;
        const ttf_font_item_t *item = index ? ttf_font_item(index - 1) : NULL;
        app_settings_set_system_font_path(item ? item->path : "");
        app_font_activate_system();
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_SYSTEM_SIZE) {
        if (y < 298 || y >= 878) return APP_REDRAW_NONE;
        int index = (y - 298) / 116;
        app_settings_set_system_font_size((uint8_t)(100 + index * 10));
        ui_text_set_system_scale(true);
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_SYSTEM_CONTRAST) {
        if (y < 298 || y >= 878) return APP_REDRAW_NONE;
        int index = (y - 298) / 116;
        app_settings_set_system_contrast((uint8_t)(100 + index * 10));
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_LOCK_STYLE) {
        if (y >= 300 && y < 432) {
            wallpaper_scan();
            s_page = SETTINGS_WALLPAPER;
            return APP_REDRAW_PAGE;
        }
        if (y >= 454 && y < 586) {
            app_settings_set_lock_style(0);
            return APP_REDRAW_PAGE;
        }
        if (y >= 710 && app_settings_wallpaper_path()[0]) {
            wallpaper_scan();
            s_page = SETTINGS_WALLPAPER;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_WALLPAPER) {
        if (y < 292 || y >= 1028) return APP_REDRAW_NONE;
        int index = s_wallpaper_page * 8 + (y - 292) / 92;
        if (index >= s_wallpaper_count) return APP_REDRAW_NONE;
        s_wallpaper_selected = index;
        s_wallpaper_confirm = false;
        s_page = SETTINGS_WALLPAPER_PREVIEW;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_TIME) {
        if (y >= 407 && y < 477) {
            s_notice[0] = 0; s_sync_pending = true;
            snprintf(s_notice, sizeof(s_notice), "正在获取网络时间…");
            return APP_REDRAW_PAGE;
        }
        if (y >= 518 && y < 588) { time_open(); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_FIRMWARE) {
        if (y >= 638 && y < 718) {
            s_boot_pending = true;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (y >= 171 && y < 298) {
        s_page = SETTINGS_FIRMWARE;
        return APP_REDRAW_PAGE;
    }
    if (y >= 365 && y < 439) {
        extern const app_desc_t app_transfer;
        app_transfer_request_wifi_setup();
        ctx->request_app = &app_transfer;
        return APP_REDRAW_NONE;
    }
    if (y >= 510 && y < 584) {
        ttf_font_scan();
        s_font_page = 0;
        s_page = SETTINGS_SYSTEM_FONT;
        return APP_REDRAW_PAGE;
    }
    if (y >= 584 && y < 658) {
        s_page = SETTINGS_SYSTEM_SIZE;
        return APP_REDRAW_PAGE;
    }
    if (y >= 658 && y < 732) {
        s_page = SETTINGS_SYSTEM_CONTRAST;
        return APP_REDRAW_PAGE;
    }
    if (y >= 732 && y < 806) {
        s_page = SETTINGS_SHELF_STYLE;
        s_style_scroll = 0;
        return APP_REDRAW_PAGE;
    }
    if (y >= 877 && y < 951) {
        s_page = SETTINGS_LOCK_STYLE;
        return APP_REDRAW_PAGE;
    }
    if (y >= 951 && y < 1025) {
        s_page = SETTINGS_TIME;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (key == 1) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW) {
        if (s_wallpaper_confirm) s_wallpaper_confirm = false;
        else s_page = SETTINGS_WALLPAPER;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_WALLPAPER) { s_page = SETTINGS_LOCK_STYLE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_TIME_EDIT) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
    if (s_page != SETTINGS_MAIN) { s_page = SETTINGS_MAIN; return APP_REDRAW_PAGE; }
    ui_nav_request(ctx, 0);
    return APP_REDRAW_NONE;
}
static bool no_menu_handle(app_ctx_t *ctx) { (void)ctx; return false; }

const app_desc_t app_device_settings = {
    .title = "设置 Settings", .detail = "显示、连接与设备", .enter_full = false,
    .owns_keys = true, .menu_handle_enabled = no_menu_handle,
    .on_enter = on_enter, .render = render, .on_tick = on_tick, .on_gesture = on_gesture, .on_key = on_key,
};
