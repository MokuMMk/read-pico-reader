/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：分组设置页；网络短时对时后 PMU 持续走时，开机恢复系统时钟。
 * English: Grouped settings; temporary WiFi sync seeds the PMU, whose RTC restores time at boot.
 */
#include <stdio.h>
#include "esp_heap_caps.h"
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
#include "ble_page_turner.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "read_pico_transfer.h"
#include "esp_log.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "ui_wallpaper.h"
#include "read_pico_search.h"
#include "book_store.h"
#include "../assets/app_icons.h"

static char s_notice[96];
typedef enum { SETTINGS_MAIN, SETTINGS_WIFI, SETTINGS_TIME,
               SETTINGS_TIME_EDIT, SETTINGS_SHELF_STYLE, SETTINGS_SYSTEM_FONT,
               SETTINGS_SYSTEM_SIZE, SETTINGS_SYSTEM_CONTRAST, SETTINGS_LOCK_STYLE,
               SETTINGS_WALLPAPER, SETTINGS_WALLPAPER_PREVIEW,
               SETTINGS_READING, SETTINGS_CONFIG,
               SETTINGS_POWER_SLEEP, SETTINGS_PROFILE, SETTINGS_AVATAR,
               SETTINGS_BLUETOOTH, SETTINGS_BLE_SCAN,
               SETTINGS_TEXT_EDIT } settings_page_t;
static settings_page_t s_page;
static int s_style_scroll, s_main_scroll;
// 蓝牙翻页器子页：滚动位置、正在学习哪个动作（0 未学，1 上一页，2 下一页）、提示行。
// Bluetooth sub-page: scroll offset, which action is being learned (0 idle, 1 prev, 2 next),
// and a notice line.
static int s_ble_scroll, s_ble_learning;
static char s_ble_notice[64];
static int s_font_page, s_wallpaper_page;
static bool s_sync_pending;
static bool s_config_confirm;
static const char *const TAG = "device_settings";
#define SETTINGS_WIRELESS_Y 315
#define SETTINGS_DISPLAY_Y 427
#define SETTINGS_DEVICE_Y 876
#define SETTINGS_ROW_H 68
// 「阅读与设备」组的行数。滚动上限由它推导：主页面最后一行必须能完整落在
// 底部导航栏（UI_NAV_TOP）之上的可点区里，否则最后一行永远露不出来，也点不到。
// Row count of the 阅读与设备 group. The scroll limit is derived from it: the last main-page row
// must be able to sit fully inside the tappable band above the bottom nav (UI_NAV_TOP), or it
// can never be revealed or tapped.
#define SETTINGS_DEVICE_ROWS 6
// 设置行图标：32 像素盒，在行高 68 里垂直居中；墨色统一，避免一行一个灰度。
// Setting row icons: a 32 px box centred in the 68 px row with one shared ink level.
#define SETTINGS_ICON_PX 32
#define SETTINGS_ICON_INK 0x58
// 主页面滚动上限：把最后一组的最后一行刚好推到导航栏之上，多留 8px 余量。
// Main-page scroll limit: just enough to bring the last row of the last group above the nav bar,
// with 8 px to spare.
#define SETTINGS_SCROLL_MAX \
    (SETTINGS_DEVICE_Y + SETTINGS_DEVICE_ROWS * SETTINGS_ROW_H - UI_NAV_TOP + 8)
static char s_editor[96], s_editor_pinyin[24], s_editor_notice[80];
static bool s_editor_chinese;
static bool s_editor_uppercase;
static bool s_editor_signature;
static int s_editor_candidate_page;
static size_t s_editor_candidate_count;
static uint32_t s_editor_candidates[5];
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
// 壁纸列表放 PSRAM，理由同上。
// The wallpaper list lives in PSRAM for the same reason.
static wallpaper_item_t *s_wallpapers;

static bool wallpapers_alloc(void) {
    if (!s_wallpapers)
        s_wallpapers = heap_caps_calloc(WALLPAPER_MAX, sizeof(wallpaper_item_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_wallpapers != NULL;
}
static int s_wallpaper_count;
static int s_wallpaper_selected;
static bool s_wallpaper_confirm, s_wallpaper_preview_ok;

static void wallpaper_scan_dir(const char *root) {
    if (!wallpapers_alloc()) return;
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

static void profile_editor_refresh(void) {
    s_editor_candidate_count = s_editor_chinese && s_editor_pinyin[0]
        ? read_pico_search_candidates(s_editor_pinyin, s_editor_candidates, 5,
                                      s_editor_candidate_page * 5) : 0;
}
static void profile_editor_open(bool signature) {
    s_editor_signature = signature;
    snprintf(s_editor, sizeof(s_editor), "%s", signature ? app_settings_status_signature() : app_settings_device_name());
    s_editor_pinyin[0] = s_editor_notice[0] = 0;
    s_editor_candidate_page = 0;
    s_editor_candidate_count = 0;
    s_editor_chinese = true;
    s_editor_uppercase = false;
    s_page = SETTINGS_TEXT_EDIT;
}
static void profile_editor_append(const char *text) {
    size_t used = strlen(s_editor), added = strlen(text);
    if (used + added < sizeof(s_editor)) {
        memcpy(s_editor + used, text, added + 1);
        s_editor_notice[0] = 0;
    } else snprintf(s_editor_notice, sizeof(s_editor_notice), "文字已达到长度上限");
}
static void profile_editor_backspace(void) {
    if (s_editor_pinyin[0]) {
        s_editor_pinyin[strlen(s_editor_pinyin) - 1] = 0;
        s_editor_candidate_page = 0;
        profile_editor_refresh();
        return;
    }
    size_t n = strlen(s_editor);
    if (!n) return;
    do { --n; } while (n && ((unsigned char)s_editor[n] & 0xc0) == 0x80);
    s_editor[n] = 0;
}
static void profile_editor_candidate(int index) {
    if (index < 0 || (size_t)index >= s_editor_candidate_count) return;
    uint32_t cp = s_editor_candidates[index];
    if (cp < 0x800 || cp > 0xffff) return;
    char glyph[4] = {(char)(0xe0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 63)),
                     (char)(0x80 | (cp & 63)), 0};
    profile_editor_append(glyph);
    s_editor_pinyin[0] = 0;
    s_editor_candidate_page = 0;
    profile_editor_refresh();
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

static void settings_card(uint8_t *fb, EpdRect rect, int radius, uint8_t fill, uint8_t edge) {
    ui_fill_round_rect(fb, rect, radius, fill);
    ui_draw_round_rect(fb, rect, radius, edge);
    ui_draw_round_rect(fb, (EpdRect){rect.x + 1, rect.y + 1,
                                     rect.width - 2, rect.height - 2}, radius - 1, edge);
}
static void settings_divider(uint8_t *fb, int y, int x, int width) {
    epd_fill_rect((EpdRect){x, y, width, 2}, 0x78, fb);
}
static void row(uint8_t *fb, int y, const char *label, const char *value) {
    ui_text(fb, 54, y + 17, 27, label, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 627, y + 20, 21, value, EPD_DRAW_ALIGN_RIGHT, false);
    settings_divider(fb, y + 68, 54, 575);
}
static void section(uint8_t *fb, int y, const char *title) {
    ui_text(fb, 40, y, 22, title, EPD_DRAW_ALIGN_LEFT, false);
}

/* ---- 蓝牙翻页器子页的版式 / Layout of the Bluetooth sub-page ---- */

// 行高比 setting_group 的 68 略大，因为这一页每行右侧常常要放一个按钮。
// Rows are a little taller than setting_group's 68 because these rows often carry a button.
#define BLE_ROW_H 76
// 按钮右缘与 row() 的值右缘对齐；值文字右端再让出按钮宽度，两者不重叠。
// The button's right edge aligns with row()'s value edge; the value text then stops short of the
// button, so the two never overlap.
#define BLE_BTN_W 140
#define BLE_BTN_RIGHT 627
#define BLE_VALUE_RIGHT (BLE_BTN_RIGHT - BLE_BTN_W - 17)

// 渲染和命中测试都调这个函数，两边看到的是同一份坐标——否则会出现「按钮画在这儿、
// 点在别处」，这一页之前就是这么坏的。
// Rendering and hit testing both call this, so they see identical coordinates. Without it a
// control ends up drawn in one place and tapped in another, which is exactly how this page was
// broken before.
typedef struct {
    int toggle_top;                  // 开关卡片顶；-1 表示被学习提示取代 / switch card top, -1 when the learn prompt replaces it
    int status_top;
    int notice_top;                  // -1 表示没有提示 / -1 when absent
    int bonds_title_top;
    int bond_top[BLE_PT_MAX_BONDS];  // 已配对行顶；-1 表示该位不存在 / bonded row tops, -1 when absent
    int bond_rows;
    int scan_entry_top;              // 「扫描设备 ›」入口行顶 / the "scan devices" entry row
    int learn_title_top;
    int learn_top[2];
    int footer_top;
    int content_bottom;              // 内容底，用于滚动上限 / content bottom, for the scroll limit
} ble_layout_t;

static ble_layout_t ble_layout(void) {
    ble_layout_t l;
    memset(&l, 0, sizeof(l));
    int y = 196 - s_ble_scroll;
    l.toggle_top = s_ble_learning ? -1 : y;
    if (!s_ble_learning) y += 126;
    y += 38;
    l.status_top = y;
    y += 84;
    l.notice_top = s_ble_notice[0] ? y - 20 : -1;
    if (s_ble_notice[0]) y += 34;
    y += 38;
    l.bonds_title_top = y - 38;
    l.bond_rows = (int)ble_pt_bond_count();
    for (int i = 0; i < BLE_PT_MAX_BONDS; ++i)
        l.bond_top[i] = i < l.bond_rows ? y + i * BLE_ROW_H : -1;
    y += l.bond_rows ? l.bond_rows * BLE_ROW_H : 68;
    l.scan_entry_top = y;
    y += BLE_ROW_H;
    y += 38;
    l.learn_title_top = y - 38;
    l.learn_top[0] = y;
    l.learn_top[1] = y + BLE_ROW_H;
    y += 2 * BLE_ROW_H + 40;
    l.footer_top = y - 36;
    l.content_bottom = y;
    return l;
}

// 一行的左标签 + 让出按钮后的右值，外加分隔线。与 row() 同高，但右端不会压到按钮。
// A row's left label plus a right value that stops short of the button, with the usual divider.
// Same height as row(), but the value never runs under the button.
static void ble_row(uint8_t *fb, int y, const char *label, const char *value) {
    ui_text(fb, 54, y + 17, 27, label, EPD_DRAW_ALIGN_LEFT, false);
    if (value && value[0]) ui_text(fb, BLE_VALUE_RIGHT, y + 20, 21, value, EPD_DRAW_ALIGN_RIGHT, false);
    settings_divider(fb, y + 68, 54, 575);
}

// 行右侧的按钮。竖直居中于该行，右缘与值文字对齐。
// The button on a row's right: vertically centred in the row, right edge aligned with the value.
static EpdRect ble_button_rect(int row_top) {
    return (EpdRect){BLE_BTN_RIGHT - BLE_BTN_W, row_top + 6, BLE_BTN_W, 60};
}

// 三级页：进页即持续扫描，选中一台连上就退回二级页。列表长了也不影响二级页。
// Third-level page: scanning starts on entry and runs continuously; picking a device connects
// and returns to level two. A long list never affects the page above it.
typedef struct {
    int devices_top;
    int device_rows;
    int status_top;
    int content_bottom;
} ble_scan_layout_t;

static ble_scan_layout_t ble_scan_layout(void) {
    ble_scan_layout_t l;
    memset(&l, 0, sizeof(l));
    l.status_top = 200;
    l.devices_top = 300;
    l.device_rows = (int)ble_pt_device_count();
    l.content_bottom = l.devices_top + (l.device_rows ? l.device_rows * BLE_ROW_H : 68) + 40;
    return l;
}
static void back_header(uint8_t *fb, const char *title) {
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, title, EPD_DRAW_ALIGN_CENTER, false);
    settings_divider(fb, 174, 36, 612);
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
    if (index == 8) {
        // TF 卡配置：两张重叠的页面。/ Two overlapping sheets for TF configuration.
        ui_draw_round_rect(fb, (EpdRect){cx - 12, cy - 15, 26, 30}, 3, 0x58);
        epd_draw_line(cx - 17, cy - 9, cx - 17, cy + 17, 0x58, fb);
        epd_draw_line(cx - 17, cy + 17, cx + 8, cy + 17, 0x58, fb);
        epd_draw_line(cx - 6, cy - 5, cx + 8, cy - 5, 0x58, fb);
        epd_draw_line(cx - 6, cy + 3, cx + 8, cy + 3, 0x58, fb);
        return;
    }
    if (index == 9) {
        epd_draw_circle(cx, cy + 3, 13, 0x58, fb);
        epd_draw_circle(cx, cy + 3, 12, 0x58, fb);
        epd_fill_rect((EpdRect){cx - 5, cy - 15, 10, 15}, UI_GRAY_WHITE, fb);
        epd_fill_rect((EpdRect){cx - 2, cy - 17, 4, 17}, 0x58, fb);
        return;
    }
    if (index == 10) {
        epd_draw_line(cx - 15, cy - 12, cx + 15, cy - 12, 0x58, fb);
        epd_draw_line(cx - 15, cy - 3, cx + 15, cy - 3, 0x58, fb);
        epd_draw_line(cx - 7, cy + 8, cx + 7, cy + 8, 0x58, fb);
        return;
    }
    if (index == 11) {
        epd_draw_circle(cx, cy, 14, 0x58, fb);
        epd_draw_line(cx - 1, cy - 8, cx - 1, cy + 1, 0x58, fb);
        epd_draw_line(cx - 1, cy + 1, cx + 7, cy + 4, 0x58, fb);
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
    const int row_height = SETTINGS_ROW_H;
    title_y -= s_main_scroll;
    card_y -= s_main_scroll;
    ui_text(fb, 42, title_y, 20, title, EPD_DRAW_ALIGN_LEFT, false);
    settings_card(fb, (EpdRect){36, card_y, 612, count * row_height}, 22,
                  UI_GRAY_WHITE, 0x70);
    for (int i = 0; i < count; ++i) {
        int y = card_y + i * row_height;
        if (i) settings_divider(fb, y, 88, 544);
        setting_icon(fb, icons[i], 67, y + row_height / 2);
        ui_text(fb, 100, y + 19, 25, labels[i], EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 619, y + 22, 20, values[i], EPD_DRAW_ALIGN_RIGHT, false);
    }
}

static void setting_toggle(uint8_t *fb, int y, const char *title, const char *detail, bool active) {
    settings_card(fb, (EpdRect){36, y, 612, 126}, 22, UI_GRAY_WHITE, 0x70);
    ui_text(fb, 59, y + 24, 27, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 59, y + 75, 19, detail, EPD_DRAW_ALIGN_LEFT, false);
    EpdRect track = {555, y + 43, 69, 39};
    ui_fill_round_rect(fb, track, 19, active ? 0x58 : 0xc4);
    int cx = active ? track.x + 49 : track.x + 20;
    epd_fill_circle(cx, track.y + 19, 16, UI_GRAY_WHITE, fb);
    epd_draw_circle(cx, track.y + 19, 16, 0x78, fb);
}

static void draw_style_thumbnail(uint8_t *fb, int style, int top) {
    if (top < 242 || top + 170 >= UI_NAV_TOP) return;
    const int left = 99;
    if (style == 4) {
        for (int i = 0; i < 2; ++i) {
            EpdRect cover = {left + 20 + i * 128, top + 3, 92, 135};
            epd_fill_rect(cover, i ? 0x78 : 0x48, fb);
            ui_draw_round_rect(fb, cover, 0, 0x28);
            epd_fill_rect((EpdRect){cover.x + cover.width + 1, cover.y + 5, 6, 130}, 0xc8, fb);
        }
        for (int i = 0; i < 7; ++i) {
            EpdRect spine = {left + 286 + i * 28, top + 29, 24, 109};
            epd_fill_rect(spine, i % 2 ? 0x78 : 0x48, fb);
            ui_draw_round_rect(fb, spine, 0, 0x28);
        }
        epd_fill_rect((EpdRect){left, top + 140, 486, 12}, 0x40, fb);
        return;
    }
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
    if (s_page == SETTINGS_PROFILE) {
        back_header(fb, "个人资料");
        ui_text(fb, 42, 214, 21, "自定义名称与头像", EPD_DRAW_ALIGN_LEFT, false);
        row(fb, 266, "设备名称", app_settings_device_name());
        row(fb, 366, "更换头像", app_settings_avatar_path()[0] ? "已选择图片  ›" : "默认图标  ›");
        ui_text(fb, 44, 612, 21, "Pico reader by Kiiko", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_AVATAR) {
        back_header(fb, "选择头像");
        section(fb, 232, "TF 卡 pictures 文件夹中的 JPG / PNG 图片");
        row(fb, 270, "使用默认图标", app_settings_avatar_path()[0] ? "选择  ›" : "当前  ✓");
        if (!s_wallpaper_count) ui_text(fb, 52, 384, 23, "未找到图片，请放入 pictures 文件夹", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < 7; ++i) {
            int index = s_wallpaper_page * 7 + i;
            if (index >= s_wallpaper_count) break;
            EpdRect box = {36, 374 + i * 96, 612, 80};
            bool active = !strcmp(s_wallpapers[index].path, app_settings_avatar_path());
            settings_card(fb, box, 17, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[index].name);
            fit_value(name, 490);
            ui_text_vc(fb, 60, box.y + 40, 24, name, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(611, box.y + 40, 7, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_TEXT_EDIT) {
        back_header(fb, s_editor_signature ? "状态栏签名" : "设备名称");
        ui_text(fb, 642, 95, 24, "完成", EPD_DRAW_ALIGN_RIGHT, false);
        ui_text(fb, 36, 201, 21, s_editor_signature ? "状态栏中间显示，留空则隐藏" : "显示在设置页的 Pico 资料卡", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_round_rect(fb, (EpdRect){36, 243, 612, 82}, 10, UI_GRAY_BLACK);
        char shown[96]; snprintf(shown, sizeof(shown), "%s", s_editor);
        fit_value(shown, 552);
        ui_text_vc(fb, 55, 284, 28, shown[0] ? shown : " ", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 36, 354, 21, s_editor_chinese ? "拼音输入" :
                s_editor_uppercase ? "英文大写" : "英文小写", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 36, 385, 25, s_editor_pinyin[0] ? s_editor_pinyin : " ", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < 5; ++i) {
            EpdRect box = {36 + i * 112, 426, 106, 57};
            ui_draw_round_rect(fb, box, 5, 0x78);
            if (i < (int)s_editor_candidate_count) {
                uint32_t cp = s_editor_candidates[i];
                char glyph[4] = {(char)(0xe0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 63)),
                                 (char)(0x80 | (cp & 63)), 0};
                ui_text_vc(fb, box.x + 53, box.y + 28, 30, glyph, EPD_DRAW_ALIGN_CENTER, false);
            }
        }
        ui_text_vc(fb, 631, 455, 26, "›", EPD_DRAW_ALIGN_CENTER, false);
        static const char *keys[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        for (int r = 0; r < 3; ++r) {
            int left = r == 0 ? 36 : r == 1 ? 67 : 123;
            for (int c = 0; c < (int)strlen(keys[r]); ++c) {
                EpdRect box = {left + c * 62, 519 + r * 74, 58, 61};
                ui_draw_round_rect(fb, box, 5, 0x78);
                char letter[2] = {s_editor_chinese || s_editor_uppercase ? keys[r][c] :
                                  (char)(keys[r][c] + ('a' - 'A')), 0};
                ui_text_vc(fb, box.x + 29, box.y + 30, 25, letter, EPD_DRAW_ALIGN_CENTER, false);
            }
        }
        const char *actions[] = {s_editor_chinese ? "中 / a" : s_editor_uppercase ? "A / 中" : "a / A",
                                 "空格", "删除", "确定"};
        static const EpdRect buttons[] = {{36, 752, 102, 70}, {148, 752, 298, 70},
                                          {456, 752, 98, 70}, {564, 752, 84, 70}};
        for (int i = 0; i < 4; ++i) ui_draw_button(fb, buttons[i], actions[i], i == 3);
        static const char *punct[] = {"，", "。", "！", "？", "-", "0", "1", "2", "3", "4",
                                      "5", "6", "7", "8", "9"};
        for (int i = 0; i < 15; ++i) {
            EpdRect box = {36 + (i % 10) * 62, 844 + (i / 10) * 70, 58, 58};
            ui_draw_round_rect(fb, box, 5, 0x78);
            ui_text_vc(fb, box.x + 29, box.y + 29, 23, punct[i], EPD_DRAW_ALIGN_CENTER, false);
        }
        if (s_editor_notice[0]) ui_text(fb, 36, 1001, 21, s_editor_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_SHELF_STYLE) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "书架样式", EPD_DRAW_ALIGN_CENTER, false);
        ui_text(fb, 36, 207, 23, "常规每页 9 本 · 书脊模式为测试版", EPD_DRAW_ALIGN_LEFT, false);
        static const char *const styles[] = {"深色书轨", "亚克力书架", "半透明书袋", "封面与书脊 · 测试版"};
        static const char *const descriptions[] = {"封面落在书轨上", "透明亚克力挡板", "每本独立透明书袋", "非正式版本"};
        for (int i = 0; i < 4; ++i) {
            int y = 263 + i * 253 - s_style_scroll;
            if (y + 230 < 242 || y > 1095) continue;
            EpdRect card = {36, y, 612, 230};
            int style = i + 1;
            bool active = app_settings_shelf_style() == style;
            settings_card(fb, card, 24, active ? 0xd0 : UI_GRAY_WHITE,
                          active ? 0x58 : 0x70);
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
            settings_card(fb, box, 18, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_draw_round_rect(fb, box, 18, active ? 0x90 : 0xd0);
            char label[TTF_FONT_NAME_MAX];
            snprintf(label, sizeof(label), "%s", item ? system_font_label(item->path) : "思源黑体（内建）");
            while (label[0] && ttf_text_width_px(ui_text_effective_px(26), label) > box.width - 102) {
                size_t len = strlen(label) - 1;
                while (len && ((unsigned char)label[len] & 0xc0) == 0x80) --len;
                label[len] = 0;
            }
            ui_text_vc(fb, 60, box.y + 39, 26, label, EPD_DRAW_ALIGN_LEFT, false);
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
            settings_card(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
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
            settings_card(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
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
            settings_card(fb, box, 22, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
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
            settings_card(fb, box, 18, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
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
        ui_draw_round_rect(fb, image, 8, 0x70);
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
    if (s_page == SETTINGS_READING) {
        back_header(fb, "阅读操作");
        section(fb, 240, "阅读正文");
        setting_toggle(fb, 281, "电源键翻页", "短按下一页，长按锁屏", app_settings_reader_power_turn());
        setting_toggle(fb, 431, "全屏沉浸", "全屏时隐藏状态栏，让正文延伸至顶部", app_settings_reader_immersive());
        setting_toggle(fb, 581, "关闭书内图片", "阅读时跳过插图，保留原书文件", app_settings_reader_hide_images());
        ui_text(fb, 50, 765, 20, "轻点正文中央切换全屏；中间触控键打开阅读设置。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 50, 806, 19, "关闭按键翻页时，电源键仍按原有锁屏逻辑工作。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_POWER_SLEEP) {
        back_header(fb, "关机睡眠");
        section(fb, 248, "选择电源菜单中“关机”的方式");
        const char *labels[] = {"彻底关机", "先浅睡，10 分钟后深睡"};
        const char *details[] = {"完全断电，长按电源键开机",
                                 "锁屏画面保留；深睡后短按电源键开机"};
        for (int i = 0; i < 2; ++i) {
            EpdRect box = {36, 300 + i * 154, 612, 132};
            bool active = app_settings_staged_shutdown() == (i == 1);
            settings_card(fb, box, 22, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_text(fb, 64, box.y + 23, 29, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 64, box.y + 79, 20, details[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 66, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 54, 664, 21, "浅睡时按键直接返回，深睡时会重新开机。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 708, 21, "锁屏样式同时适用于壁纸与阅读票根。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_CONFIG) {
        back_header(fb, "保存与恢复配置");
        section(fb, 248, "换机或刷机后，快速恢复个性化设置");
        settings_card(fb, (EpdRect){36, 299, 612, 197}, 22, UI_GRAY_WHITE, 0x70);
        ui_text(fb, 60, 326, 26, "TF 卡根目录", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 377, 23, "Pico-settings.backup", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 435, 19, "文字排版、设备资料、阅读记录与书签", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){36, 561, 612, 83}, "保存当前配置到 TF 卡", false);
        ui_draw_button(fb, (EpdRect){36, 681, 612, 83},
                       s_config_confirm ? "再次点按，确认恢复配置" : "从 TF 卡恢复配置", false);
        if (s_notice[0]) ui_text(fb, 48, 819, 21, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 899, 19, "包含阅读记录、设备资料和已存 WiFi。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 939, 19, "书籍、字体和图片仍需留在 TF 卡。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 979, 19, "备份含 WiFi 密码，请妥善保管 TF 卡。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    const int profile_y = 164 - s_main_scroll;
    if (profile_y + 106 > 160) {
        settings_card(fb, (EpdRect){36, profile_y, 612, 106}, 24, UI_GRAY_WHITE, 0x70);
        bool avatar_ok = app_settings_avatar_path()[0] &&
            ui_wallpaper_draw_rounded(fb, app_settings_avatar_path(),
                                      (EpdRect){57, profile_y + 11, 82, 82}, 20);
        if (!avatar_ok) {
            ui_fill_round_rect(fb, (EpdRect){57, profile_y + 11, 82, 82}, 20, 0x30);
            ui_text(fb, 98, profile_y + 25, 49, "P", EPD_DRAW_ALIGN_CENTER, true);
        }
        char profile_name[64]; snprintf(profile_name, sizeof(profile_name), "%s", app_settings_device_name());
        while (profile_name[0] && ttf_text_width_px(ui_text_effective_px(30), profile_name) > 345) {
            size_t n = strlen(profile_name) - 1;
            while (n && ((unsigned char)profile_name[n] & 0xc0) == 0x80) --n;
            profile_name[n] = 0;
        }
        ui_text(fb, 164, profile_y + 19, 30, profile_name, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 164, profile_y + 62, 19, "Pico reader by Kiiko", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 618, profile_y + 62, 19, "编辑  ›", EPD_DRAW_ALIGN_RIGHT, false);
        const pmu_snapshot_t *pmu = read_pico_pmu_get();
        if (pmu && pmu->soc_permille <= 1000) {
            char battery[12]; snprintf(battery, sizeof(battery), "%u%%", (unsigned)pmu->soc_permille / 10);
            // 电量和“编辑”从同一左边界起排，避免数字较短时看起来偏右。
            // Start the percentage at the edit label's left edge so short numbers do not appear offset.
            int edit_left = 618 - ui_text_fixed_width_px(ui_text_effective_px(19), "编辑  ›");
            ui_text(fb, edit_left, profile_y + 26, 22, battery, EPD_DRAW_ALIGN_LEFT, false);
        }
    }
    char wifi_ssid[33] = {0}; bool wifi_saved = false;
    (void)read_pico_transfer_get_saved_wifi(wifi_ssid, &wifi_saved);
    if (s_page == SETTINGS_BLE_SCAN) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "扫描设备", EPD_DRAW_ALIGN_CENTER, false);
        const ble_scan_layout_t l = ble_scan_layout();
        const int scroll = s_ble_scroll;
        // 持续扫描中：告诉用户正在找，以及找到几台。
        // Continuous scan: say that it is running and how many turned up.
        char head[64];
        if (ble_pt_scanning()) snprintf(head, sizeof(head), "正在搜索…已找到 %u 台", (unsigned)ble_pt_device_count());
        else snprintf(head, sizeof(head), "扫描已停止");
        ui_text(fb, 44, l.status_top - scroll, 23, head, EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < l.device_rows; ++i) {
            const ble_pt_device_t *dev = ble_pt_device((uint8_t)i);
            if (!dev) continue;
            char label[48];
            snprintf(label, sizeof(label), "%s", dev->name);
            fit_value(label, 360);
            char value[44];
            snprintf(value, sizeof(value), "%d dBm%s%s", dev->rssi, dev->hid ? " · HID" : "",
                     dev->bonded ? " · 已配对" : "");
            ble_row(fb, l.devices_top - scroll + i * BLE_ROW_H, label, value);
        }
        if (!l.device_rows)
            ui_text(fb, 44, l.devices_top - scroll + 18, 23, "附近还没有发现设备",
                    EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_BLUETOOTH) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "蓝牙翻页器", EPD_DRAW_ALIGN_CENTER, false);
        const ble_layout_t l = ble_layout();

        // 学习按键时把提示顶到最上面，用户不用滚回去看。
        // While learning, the prompt is pinned to the top so the user need not scroll back.
        if (s_ble_learning) {
            settings_card(fb, (EpdRect){36, 156, 612, 74}, 14, 0xd0, 0x58);
            ui_text_vc(fb, 342, 193, 24,
                       s_ble_learning == 1 ? "请按翻页器上「上一页」要用的键"
                                           : "请按翻页器上「下一页」要用的键",
                       EPD_DRAW_ALIGN_CENTER, false);
        } else {
            setting_toggle(fb, l.toggle_top, "启用蓝牙", "开启后可配对蓝牙翻页器；关闭会释放蓝牙内存",
                           app_settings_ble_turner());
        }

        section(fb, l.status_top - 38, "状态");
        char ble_status[80];
        if (!app_settings_ble_turner()) snprintf(ble_status, sizeof(ble_status), "已关闭");
        else if (!ble_pt_running()) snprintf(ble_status, sizeof(ble_status), "启动中…");
        else if (ble_pt_connected()) snprintf(ble_status, sizeof(ble_status), "已连接 %s", ble_pt_connected_name());
        else if (ble_pt_connecting()) snprintf(ble_status, sizeof(ble_status), "连接中…");
        else snprintf(ble_status, sizeof(ble_status), "未连接");
        ble_row(fb, l.status_top, "当前", ble_status);
        if (l.notice_top >= 0) ui_text(fb, 44, l.notice_top, 21, s_ble_notice, EPD_DRAW_ALIGN_LEFT, false);

        // 已配对：点行连接，右侧按钮删除。/ Bonded peers: the row connects, the button forgets.
        section(fb, l.bonds_title_top, "已配对");
        if (!l.bond_rows)
            ui_text(fb, 44, l.bonds_title_top + 56, 23, "还没有配对过设备", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < l.bond_rows && i < BLE_PT_MAX_BONDS; ++i) {
            const ble_pt_bond_t *bond = ble_pt_bond((uint8_t)i);
            if (!bond) continue;
            char label[40];
            snprintf(label, sizeof(label), "%s", bond->name[0] ? bond->name : bond->addr);
            fit_value(label, 300);
            const bool live = ble_pt_connected() && !strcmp(ble_pt_connected_name(), bond->name);
            ble_row(fb, l.bond_top[i], label, live ? "已连接" : "连接  ›");
            ui_draw_button(fb, ble_button_rect(l.bond_top[i]), "删除", false);
        }

        section(fb, l.scan_entry_top - 38, "设备");
        ble_row(fb, l.scan_entry_top, "扫描设备", "搜索并连接  ›");

        section(fb, l.learn_title_top, "按键映射");
        static const char *const learn_names[] = {"上一页", "下一页"};
        const uint32_t bound[] = {ble_pt_binding(BLE_PT_ACTION_PREV), ble_pt_binding(BLE_PT_ACTION_NEXT)};
        for (int i = 0; i < 2; ++i) {
            ble_row(fb, l.learn_top[i], learn_names[i], bound[i] ? "自定义按键" : "内置按键");
            ui_draw_button(fb, ble_button_rect(l.learn_top[i]), "学习", false);
        }
        ui_text(fb, 36, l.footer_top, 21, "内置：上下、左右、PageUp/PageDown 直接翻页",
                EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    const char *wireless_labels[] = {"WiFi"};
    char wireless_value[56]; snprintf(wireless_value, sizeof(wireless_value), "%s  ›", wifi_saved ? wifi_ssid : "未配置");
    fit_value(wireless_value, 235);
    const char *wireless_values[] = {wireless_value};
    static const int wireless_icons[] = {0};
    setting_group(fb, 285, "无线连接", SETTINGS_WIRELESS_Y,
                  wireless_icons, wireless_labels, wireless_values, 1);
    const char *reading_labels[] = {"系统字体", "系统字号", "系统对比度", "书架样式", "状态栏签名", "首页强刷"};
    char font[96];
    const char *chosen_font = app_settings_system_font_path();
    snprintf(font, sizeof(font), "%s  ›", system_font_label(chosen_font));
    fit_value(font, 235);
    char size[32]; snprintf(size, sizeof(size), "%u%%  ›", app_settings_system_font_size());
    char contrast[32]; snprintf(contrast, sizeof(contrast), "%u%%  ›", app_settings_system_contrast());
    static const char *const styles[] = {"深色书轨  ›", "深色书轨  ›", "亚克力书架  ›", "半透明书袋  ›", "书脊测试版  ›"};
    char signature_value[96];
    snprintf(signature_value, sizeof(signature_value), "%s  ›",
             app_settings_status_signature()[0] ? app_settings_status_signature() : "未设置");
    fit_value(signature_value, 235);
    const char *reading_values[] = {font, size, contrast, styles[app_settings_shelf_style()],
                                    signature_value, app_settings_home_full_refresh() ? "开启  ›" : "关闭  ›"};
    static const int reading_icons[] = {2, 3, 7, 4, 10, 11};
    setting_group(fb, 397, "显示", SETTINGS_DISPLAY_Y,
                  reading_icons, reading_labels, reading_values, 6);
    const char *display_labels[] = {"锁屏样式", "关机睡眠", "阅读操作", "日期与时间", "保存与恢复", "蓝牙翻页器"};
    const char *display_values[] = {app_settings_lock_style() ? "壁纸  ›" : "阅读票根  ›",
                                    app_settings_staged_shutdown() ? "先浅后深  ›" : "彻底断电  ›",
                                    "设置  ›", "设置  ›", "配置  ›",
                                    app_settings_ble_turner() ? "已开启  ›" : "已关闭  ›"};
    static const int display_icons[] = {5, 9, 2, 6, 8, 0};
    setting_group(fb, 842, "阅读与设备", SETTINGS_DEVICE_Y,
                  display_icons, display_labels, display_values, SETTINGS_DEVICE_ROWS);
    epd_fill_rect((EpdRect){0, 0, UI_LOCK_WIDTH, 160}, 0xe0, fb);
    ui_nav_status(fb);
    ui_text(fb, 36, 91, 52, "设置", EPD_DRAW_ALIGN_LEFT, false);
    ui_nav_draw(fb, 3);
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_notice[0] = 0;
    s_page = SETTINGS_MAIN;
    s_style_scroll = s_main_scroll = s_font_page = s_wallpaper_page = 0;
    s_wallpaper_selected = -1;
    s_wallpaper_confirm = s_wallpaper_preview_ok = false;
    s_sync_pending = false;
    s_config_confirm = false;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    // 蓝牙启动失败的原因转成提示。放在这里而不是 render()：render 必须是纯绘制，
    // 而 take_* 会清空状态。
    // Fold a Bluetooth start failure into the notice here rather than in render(), which must
    // stay pure, and because take_* clears the state.
    // 蓝牙页要跟着栈的状态重绘：开关拨开后栈是异步起来的，配对表、扫描结果和连接状态
    // 都在页面画完以后才变。不重绘的话用户看到的永远是进页那一刻的快照，只能退出去再进来。
    // The Bluetooth pages redraw with the stack: enabling it starts it asynchronously, and the
    // bond list, scan results and link state all change after the page was painted. Without a
    // poll the user only ever sees the snapshot from when they entered, and has to leave and
    // come back.
    if (s_page == SETTINGS_BLUETOOTH || s_page == SETTINGS_BLE_SCAN) {
        static uint32_t ble_seen;
        const uint32_t sig = (app_settings_ble_turner() ? 1u : 0u) | (ble_pt_running() ? 2u : 0u) |
                             (ble_pt_connected() ? 4u : 0u) | (ble_pt_connecting() ? 8u : 0u) |
                             (ble_pt_scanning() ? 16u : 0u) |
                             ((uint32_t)ble_pt_bond_count() << 8) |
                             ((uint32_t)ble_pt_device_count() << 16);
        if (sig != ble_seen) {
            ble_seen = sig;
            return APP_REDRAW_PAGE;
        }
    }
    if (s_page == SETTINGS_BLUETOOTH && !s_ble_notice[0]) {
        char why[64];
        if (ble_pt_take_start_failure(why, sizeof(why))) {
            snprintf(s_ble_notice, sizeof(s_ble_notice), "%s", why);
            return APP_REDRAW_PAGE;
        }
    }

    (void)ctx;
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

static app_redraw_t profile_editor_gesture(const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_SWIPE_L && s_editor_pinyin[0]) {
        ++s_editor_candidate_page; profile_editor_refresh(); return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_SWIPE_R && s_editor_candidate_page) {
        --s_editor_candidate_page; profile_editor_refresh(); return APP_REDRAW_PAGE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (y < 160) {
        if (x < 160) { s_page = s_editor_signature ? SETTINGS_MAIN : SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (x > 510) goto save_text;
    }
    if (y >= 426 && y < 483) {
        if (x >= 604) { ++s_editor_candidate_page; profile_editor_refresh(); return APP_REDRAW_PAGE; }
        if (x >= 36) profile_editor_candidate((x - 36) / 112);
        return APP_REDRAW_PAGE;
    }
    static const char *keys[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int r = 0; r < 3; ++r) {
        int left = r == 0 ? 36 : r == 1 ? 67 : 123, top = 519 + r * 74;
        if (y < top || y >= top + 61 || x < left) continue;
        int c = (x - left) / 62;
        if (c < 0 || c >= (int)strlen(keys[r]) || x >= left + c * 62 + 58) continue;
        char letter = keys[r][c];
        if (s_editor_chinese) {
            size_t n = strlen(s_editor_pinyin);
            if (n + 1 < sizeof(s_editor_pinyin)) {
                s_editor_pinyin[n] = (char)(letter + ('a' - 'A'));
                s_editor_pinyin[n + 1] = 0;
                s_editor_candidate_page = 0;
                profile_editor_refresh();
            }
        } else { char value[2] = {s_editor_uppercase ? letter : (char)(letter + ('a' - 'A')), 0};
                 profile_editor_append(value); }
        return APP_REDRAW_PAGE;
    }
    if (y >= 752 && y < 822) {
        if (x >= 36 && x < 138) {
            if (!s_editor_pinyin[0]) {
                if (s_editor_chinese) { s_editor_chinese = false; s_editor_uppercase = false; }
                else if (!s_editor_uppercase) s_editor_uppercase = true;
                else s_editor_chinese = true;
            }
            else snprintf(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字");
        } else if (x >= 148 && x < 446) {
            if (s_editor_pinyin[0] && s_editor_candidate_count) profile_editor_candidate(0);
            else if (s_editor_pinyin[0]) {
                profile_editor_append(s_editor_pinyin);
                s_editor_pinyin[0] = 0;
                profile_editor_refresh();
            } else profile_editor_append(" ");
        } else if (x >= 456 && x < 554) profile_editor_backspace();
        else if (x >= 564) goto save_text;
        return APP_REDRAW_PAGE;
    }
    if (y >= 844 && y < 972 && x >= 36 && x < 648) {
        static const char *punct[] = {"，", "。", "！", "？", "-", "0", "1", "2", "3", "4",
                                      "5", "6", "7", "8", "9"};
        int row = (y - 844) / 70, col = (x - 36) / 62;
        int i = row * 10 + col;
        if (row < 2 && col < 10 && i < 15 && (y - 844) % 70 < 58 &&
            x < 36 + col * 62 + 58) profile_editor_append(punct[i]);
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
save_text:
    if (s_editor_pinyin[0]) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字");
        return APP_REDRAW_PAGE;
    }
    if (s_editor_signature) app_settings_set_status_signature(s_editor);
    else if (s_editor[0]) app_settings_set_device_name(s_editor);
    else { snprintf(s_editor_notice, sizeof(s_editor_notice), "设备名称不能为空"); return APP_REDRAW_PAGE; }
    const char *saved = s_editor_signature ? app_settings_status_signature() : app_settings_device_name();
    if (strcmp(saved, s_editor)) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "保存失败，请检查设置存储空间");
        return APP_REDRAW_PAGE;
    }
    s_page = s_editor_signature ? SETTINGS_MAIN : SETTINGS_PROFILE;
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_page == SETTINGS_TEXT_EDIT) return profile_editor_gesture(ev);
    if (s_page == SETTINGS_MAIN &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int next = s_main_scroll + (ev->type == UI_GESTURE_SWIPE_U ? 80 : -80);
        if (next < 0) next = 0;
        if (next > SETTINGS_SCROLL_MAX) next = SETTINGS_SCROLL_MAX;
        if (next == s_main_scroll) return APP_REDRAW_NONE;
        s_main_scroll = next;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_AVATAR && (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int pages = (s_wallpaper_count + 6) / 7;
        if (ev->type == UI_GESTURE_SWIPE_U && s_wallpaper_page + 1 < pages) ++s_wallpaper_page;
        if (ev->type == UI_GESTURE_SWIPE_D && s_wallpaper_page > 0) --s_wallpaper_page;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW && !s_wallpaper_confirm &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int next = s_wallpaper_selected + (ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
        if (next < 0 || next >= s_wallpaper_count) return APP_REDRAW_NONE;
        s_wallpaper_selected = next;
        s_wallpaper_page = next / 8;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_SHELF_STYLE && (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int next = s_style_scroll + (ev->type == UI_GESTURE_SWIPE_U ? 253 : -253);
        if (next < 0) next = 0;
        if (next > 253) next = 253;
        if (next == s_style_scroll) return APP_REDRAW_NONE;
        s_style_scroll = next;
        return APP_REDRAW_PAGE;
    }
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
    if (s_page == SETTINGS_PROFILE) {
        if (y < 190) { s_page = SETTINGS_MAIN; return APP_REDRAW_PAGE; }
        if (y >= 266 && y < 350) { profile_editor_open(false); return APP_REDRAW_PAGE; }
        if (y >= 366 && y < 450) { wallpaper_scan(); s_page = SETTINGS_AVATAR; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_AVATAR) {
        if (y < 190) { s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (y >= 270 && y < 354) { app_settings_set_avatar_path(""); s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (y >= 374 && y < 1046) {
            int i = (y - 374) / 96, index = s_wallpaper_page * 7 + i;
            if (index >= 0 && index < s_wallpaper_count && (y - 374) % 96 < 80) {
                app_settings_set_avatar_path(s_wallpapers[index].path);
                s_page = SETTINGS_PROFILE;
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
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
        for (int i = 0; i < 4; ++i) {
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
    if (s_page == SETTINGS_READING) {
        if (y >= 281 && y < 407) {
            app_settings_set_reader_power_turn(!app_settings_reader_power_turn());
            return APP_REDRAW_PAGE;
        }
        if (y >= 431 && y < 557) {
            app_settings_set_reader_immersive(!app_settings_reader_immersive());
            return APP_REDRAW_PAGE;
        }
        if (y >= 581 && y < 707) {
            app_settings_set_reader_hide_images(!app_settings_reader_hide_images());
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_POWER_SLEEP) {
        if (y >= 300 && y < 432) {
            app_settings_set_staged_shutdown(false);
            return APP_REDRAW_PAGE;
        }
        if (y >= 454 && y < 586) {
            app_settings_set_staged_shutdown(true);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_CONFIG) {
        if (y >= 561 && y < 644) {
            s_config_confirm = false;
            esp_err_t err = app_settings_backup_save();
            snprintf(s_notice, sizeof(s_notice), "%s",
                     err == ESP_OK ? "已保存到 TF 卡根目录" :
                     err == ESP_ERR_INVALID_STATE ? "未识别到 TF 卡，请插卡后重试" :
                     "保存失败，请检查 TF 卡剩余空间");
            return APP_REDRAW_PAGE;
        }
        if (y >= 681 && y < 764) {
            if (!s_config_confirm) {
                s_config_confirm = true;
                snprintf(s_notice, sizeof(s_notice), "恢复将覆盖当前设置，请再点按一次确认");
                return APP_REDRAW_PAGE;
            }
            s_config_confirm = false;
            esp_err_t err = app_settings_backup_restore();
            if (err == ESP_OK) {
                ttf_font_scan();
                app_font_activate_system();
                ui_text_set_system_scale(true);
                // 阅读记录恢复后令首页和书架重新取进度、时长与排序。
                // Rebuild home and shelf caches after restoring reading records.
                book_store_notify_changed();
            }
            char saved_ssid[33] = {0};
            bool wifi_saved = false;
            if (err == ESP_OK)
                (void)read_pico_transfer_get_saved_wifi(saved_ssid, &wifi_saved);
            snprintf(s_notice, sizeof(s_notice), "%s",
                     err == ESP_OK ? (wifi_saved ? "配置与阅读记录已恢复；WiFi 请重新连接" :
                                                   "配置与阅读记录已恢复") :
                     err == ESP_ERR_INVALID_STATE ? "未识别到 TF 卡，请插卡后重试" :
                     err == ESP_ERR_NOT_FOUND ? "未找到 Pico-settings.backup" :
                     err == ESP_ERR_INVALID_RESPONSE ? "配置文件损坏或版本不兼容" :
                     "恢复未完成，请重试");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLUETOOTH) {
        const ble_layout_t l = ble_layout();
        // 上下滑滚动；上限由内容底推导，内容再变长也不会被卡住。
        // Swipe to scroll; the limit derives from the content bottom, so a longer page never sticks.
        if (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D) {
            const int step = ev->type == UI_GESTURE_SWIPE_U ? 80 : -80;
            int next = s_ble_scroll + step;
            const int limit = l.content_bottom + s_ble_scroll - UI_NAV_TOP + 8;
            if (next > limit) next = limit;
            if (next < 0) next = 0;
            if (next == s_ble_scroll) return APP_REDRAW_NONE;
            s_ble_scroll = next;
            return APP_REDRAW_PAGE;
        }
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        // 版式给的是屏幕坐标（已减去滚动量），所以这里直接用 y 比，不再加 scroll。
        // The layout yields screen coordinates (scroll already subtracted), so compare y directly.
        const int ty = y;

        // 学习模式：下一个按下的原始边沿就是绑定值；点别处取消。
        // Learn mode: the next raw edge becomes the binding; a tap elsewhere cancels.
        if (s_ble_learning) {
            ble_pt_raw_t raw;
            while (ble_pt_pop_raw(&raw)) {
                if (!raw.pressed || raw.was_rest) continue;
                const ble_pt_action_t action = s_ble_learning == 1 ? BLE_PT_ACTION_PREV
                                                                  : BLE_PT_ACTION_NEXT;
                snprintf(s_ble_notice, sizeof(s_ble_notice),
                         ble_pt_bind(action, ble_pt_raw_code(&raw)) == ESP_OK
                             ? "已绑定自定义按键" : "绑定失败：请先连接设备");
                s_ble_learning = 0;
                return APP_REDRAW_PAGE;
            }
            s_ble_learning = 0;
            snprintf(s_ble_notice, sizeof(s_ble_notice), "已取消学习");
            return APP_REDRAW_PAGE;
        }
        if (ty < 160 || ty >= UI_NAV_TOP) return APP_REDRAW_NONE;

        if (l.toggle_top >= 0 && ty >= l.toggle_top && ty < l.toggle_top + 126) {
            app_settings_set_ble_turner(!app_settings_ble_turner());
            // 明确关掉时把失败计数和原因一起清掉：否则自锁之后用户没有重试的途径。
            // Switching it off explicitly clears the failure count and reason; without this a
            // self-locked stack leaves the user no way to try again.
            if (!app_settings_ble_turner()) ble_pt_reset_failure();
            snprintf(s_ble_notice, sizeof(s_ble_notice), "%s",
                     app_settings_ble_turner() ? "已开启，正在等待设备" : "已关闭，蓝牙内存已释放");
            return APP_REDRAW_PAGE;
        }
        if (ty >= l.scan_entry_top && ty < l.scan_entry_top + BLE_ROW_H) {
            // 进扫描页就开持续扫描，离页时停掉。/ Scan continuously while the page is open.
            ble_pt_scan_start(BLE_PT_SCAN_FOREVER);
            s_ble_scroll = 0;
            s_ble_notice[0] = 0;
            s_page = SETTINGS_BLE_SCAN;
            return APP_REDRAW_PAGE;
        }
        for (int i = 0; i < 2; ++i) {
            const EpdRect btn = ble_button_rect(l.learn_top[i]);
            // 命中判定必须同时比 x：按钮的纵向范围几乎覆盖整行，只比 y 会让整行都算按钮。
            // The hit test needs the horizontal test too: the button's vertical span covers almost
            // the whole row, so comparing y alone makes the entire row count as the button.
            if (ev->x0 >= btn.x && ev->x0 < btn.x + btn.width && ty >= btn.y && ty < btn.y + btn.height) {
                if (!ble_pt_connected()) {
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "请先连接翻页器再学习按键");
                    return APP_REDRAW_PAGE;
                }
                s_ble_learning = i + 1;
                return APP_REDRAW_PAGE;
            }
        }
        // 已配对行：右侧按钮忘记，行本身连接。/ Bonded rows: the button forgets, the row connects.
        for (int i = 0; i < l.bond_rows && i < BLE_PT_MAX_BONDS; ++i) {
            const ble_pt_bond_t *bond = ble_pt_bond((uint8_t)i);
            if (!bond) continue;
            const EpdRect btn = ble_button_rect(l.bond_top[i]);
            if (ev->x0 >= btn.x && ev->x0 < btn.x + btn.width && ty >= btn.y && ty < btn.y + btn.height) {
                if (ble_pt_connected()) ble_pt_disconnect();
                ble_pt_forget(bond->addr);
                snprintf(s_ble_notice, sizeof(s_ble_notice), "已删除该配对");
                return APP_REDRAW_PAGE;
            }
            if (ty >= l.bond_top[i] && ty < l.bond_top[i] + BLE_ROW_H) {
                if (ble_pt_connected() || ble_pt_connecting()) ble_pt_disconnect();
                snprintf(s_ble_notice, sizeof(s_ble_notice),
                         ble_pt_connect(bond->addr) == ESP_OK ? "正在连接…" : "连接失败，请重试");
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLE_SCAN) {
        const ble_scan_layout_t l = ble_scan_layout();
        if (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D) {
            const int step = ev->type == UI_GESTURE_SWIPE_U ? 80 : -80;
            int next = s_ble_scroll + step;
            const int limit = l.content_bottom - UI_NAV_TOP + 8;
            if (next > limit) next = limit;
            if (next < 0) next = 0;
            if (next == s_ble_scroll) return APP_REDRAW_NONE;
            s_ble_scroll = next;
            return APP_REDRAW_PAGE;
        }
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (y < 160 || y >= UI_NAV_TOP) return APP_REDRAW_NONE;
        const int ty = y + s_ble_scroll;
        // 选一台就连，连上后退回二级页。扫描在离页时停。
        // Picking one connects, then returns to level two; scanning stops on the way out.
        for (int i = 0; i < l.device_rows; ++i) {
            const ble_pt_device_t *dev = ble_pt_device((uint8_t)i);
            if (!dev) continue;
            const int top = l.devices_top + i * BLE_ROW_H;
            if (ty < top || ty >= top + BLE_ROW_H) continue;
            ble_pt_scan_stop();
            if (ble_pt_connected() || ble_pt_connecting()) ble_pt_disconnect();
            snprintf(s_ble_notice, sizeof(s_ble_notice),
                     ble_pt_connect(dev->addr) == ESP_OK ? "正在连接…" : "连接失败，请重试");
            s_page = SETTINGS_BLUETOOTH;
            s_ble_scroll = 0;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (y < 160 || y >= UI_NAV_TOP) return APP_REDRAW_NONE;
    y += s_main_scroll;
    if (y >= 164 && y < 270) {
        s_page = SETTINGS_PROFILE;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_WIRELESS_Y && y < SETTINGS_WIRELESS_Y + SETTINGS_ROW_H) {
        extern const app_desc_t app_transfer;
        app_transfer_request_wifi_setup();
        ctx->request_app = &app_transfer;
        return APP_REDRAW_NONE;
    }
    if (y >= SETTINGS_DISPLAY_Y && y < SETTINGS_DISPLAY_Y + SETTINGS_ROW_H) {
        ttf_font_scan();
        s_font_page = 0;
        s_page = SETTINGS_SYSTEM_FONT;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DISPLAY_Y + SETTINGS_ROW_H && y < SETTINGS_DISPLAY_Y + 2 * SETTINGS_ROW_H) {
        s_page = SETTINGS_SYSTEM_SIZE;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DISPLAY_Y + 2 * SETTINGS_ROW_H && y < SETTINGS_DISPLAY_Y + 3 * SETTINGS_ROW_H) {
        s_page = SETTINGS_SYSTEM_CONTRAST;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DISPLAY_Y + 3 * SETTINGS_ROW_H && y < SETTINGS_DISPLAY_Y + 4 * SETTINGS_ROW_H) {
        s_page = SETTINGS_SHELF_STYLE;
        s_style_scroll = 0;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DISPLAY_Y + 4 * SETTINGS_ROW_H && y < SETTINGS_DISPLAY_Y + 5 * SETTINGS_ROW_H) {
        profile_editor_open(true);
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DISPLAY_Y + 5 * SETTINGS_ROW_H && y < SETTINGS_DISPLAY_Y + 6 * SETTINGS_ROW_H) {
        app_settings_set_home_full_refresh(!app_settings_home_full_refresh());
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DEVICE_Y && y < SETTINGS_DEVICE_Y + SETTINGS_ROW_H) {
        s_page = SETTINGS_LOCK_STYLE;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DEVICE_Y + SETTINGS_ROW_H && y < SETTINGS_DEVICE_Y + 2 * SETTINGS_ROW_H) {
        s_page = SETTINGS_POWER_SLEEP;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DEVICE_Y + 2 * SETTINGS_ROW_H && y < SETTINGS_DEVICE_Y + 3 * SETTINGS_ROW_H) {
        s_page = SETTINGS_READING;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DEVICE_Y + 3 * SETTINGS_ROW_H && y < SETTINGS_DEVICE_Y + 4 * SETTINGS_ROW_H) {
        s_page = SETTINGS_TIME;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DEVICE_Y + 4 * SETTINGS_ROW_H && y < SETTINGS_DEVICE_Y + 5 * SETTINGS_ROW_H) {
        s_page = SETTINGS_CONFIG;
        s_config_confirm = false;
        s_notice[0] = 0;
        return APP_REDRAW_PAGE;
    }
    if (y >= SETTINGS_DEVICE_Y + (SETTINGS_DEVICE_ROWS - 1) * SETTINGS_ROW_H &&
        y < SETTINGS_DEVICE_Y + SETTINGS_DEVICE_ROWS * SETTINGS_ROW_H) {
        s_page = SETTINGS_BLUETOOTH;
        s_ble_scroll = 0;
        s_ble_learning = 0;
        s_ble_notice[0] = 0;
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
    if (s_page == SETTINGS_AVATAR) { s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_TEXT_EDIT) { s_page = s_editor_signature ? SETTINGS_MAIN : SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_PROFILE) { s_page = SETTINGS_MAIN; return APP_REDRAW_PAGE; }
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
