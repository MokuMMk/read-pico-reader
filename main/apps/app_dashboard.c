/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：首页显示继续阅读与近七天阅读时长；封面跨页面缓存。
 * English: Home shows the current read and seven days of reading time; covers survive tab changes.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "app_content_open.h"
#include "app_registry.h"
#include "book_cover.h"
#include "ui_image_dither.h"
#include "book_epub.h"
#include "book_progress.h"
#include "book_store.h"
#include "book_title.h"
#include "book_ticket.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_heap_caps.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_nav.h"

typedef struct {
    char path[BOOK_STORE_PATH_MAX];
    char title[128];
    char author[128];
    uint8_t *cover;
    time_t modified;
    book_progress_t progress;
    bool has_progress;
} home_book_t;

static home_book_t s_current, s_recent[6];
typedef struct {
    char path[BOOK_STORE_PATH_MAX];
    char title[128];
    char author[128];
    off_t size;
    time_t modified;
    uint8_t *pixels;
} home_cover_cache_t;
static home_cover_cache_t s_cover_cache[7];
static int s_cover_next;
static bool s_scan_pending;
static bool s_books_cache_valid;
static bool s_cached_sd_present;
static bool s_cached_sd_mounted;
static unsigned s_cached_store_revision;
static char s_cached_books_dir[128];
static char s_cached_last_path[BOOK_STORE_PATH_MAX];
static EpdRect s_area;
static uint32_t s_reading_days[30];
static bool s_reading_days_valid;
static bool s_home_force_full_once;

static home_book_t *book_at(int index);

static bool is_book(const char *name) {
    if (!name[0] || name[0] == '.' || !strncmp(name, "._", 2)) return false;
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".epub") || !strcasecmp(dot, ".txt"));
}
static void copy_title(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void title_for(home_book_t *item) {
    if (!book_title_from_path(item->path, item->title, sizeof(item->title)))
        copy_title(item->title, sizeof(item->title), "未命名");
}

static void consider_book(const char *path, const struct stat *st) {
    if (s_current.path[0] && !strcmp(path, s_current.path)) return;
    book_progress_t progress = {0};
    bool has_progress = st->st_size >= 0 && (uint64_t)st->st_size <= UINT32_MAX &&
                        book_progress_load(path, (uint32_t)st->st_size, &progress);
    uint64_t score = has_progress ? (UINT64_C(1) << 32) + progress.last_open_s : (uint64_t)st->st_mtime;
    int slot = -1;
    for (int i = 0; i < 6; ++i) {
        uint64_t other = s_recent[i].has_progress ?
            (UINT64_C(1) << 32) + s_recent[i].progress.last_open_s : (uint64_t)s_recent[i].modified;
        if (!s_recent[i].path[0] || score > other) { slot = i; break; }
    }
    if (slot < 0) return;
    for (int i = 5; i > slot; --i) s_recent[i] = s_recent[i - 1];
    memset(&s_recent[slot], 0, sizeof(s_recent[slot]));
    snprintf(s_recent[slot].path, sizeof(s_recent[slot].path), "%s", path);
    s_recent[slot].modified = st->st_mtime;
    s_recent[slot].has_progress = has_progress;
    if (has_progress) s_recent[slot].progress = progress;
}

static void scan_root(const char *root) {
    DIR *dir = opendir(root);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!is_book(entry->d_name)) continue;
        char path[BOOK_STORE_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >= sizeof(path)) continue;
        struct stat st;
        if (!stat(path, &st) && S_ISREG(st.st_mode)) consider_book(path, &st);
    }
    closedir(dir);
}

static void scan_books(void) {
    char last[BOOK_STORE_PATH_MAX];
    if (book_progress_last_path(last, sizeof(last))) {
        struct stat st;
        if (!stat(last, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 && (uint64_t)st.st_size <= UINT32_MAX) {
            snprintf(s_current.path, sizeof(s_current.path), "%s", last);
            s_current.has_progress = book_progress_load(last, (uint32_t)st.st_size, &s_current.progress);
            title_for(&s_current);
        }
    }
    read_pico_sd_info_t sd = {0};
    read_pico_sd_get_info(&sd);
    if (sd.mounted) {
        if (!strcmp(app_settings_books_dir(), "/sdcard/books")) scan_root("/sdcard/book");
        scan_root(app_settings_books_dir());
    }
    if (book_store_flash_ready()) scan_root("/flash/books");
    for (int i = 0; i < 6; ++i) if (s_recent[i].path[0]) title_for(&s_recent[i]);
    home_book_t *featured = s_current.path[0] ? &s_current : &s_recent[0];
    const char *ext = strrchr(featured->path, '.');
    if (ext && !strcasecmp(ext, ".epub")) {
        char metadata_title[256];
        if (book_epub_metadata(featured->path, metadata_title, sizeof(metadata_title),
                               featured->author, sizeof(featured->author)) != ESP_OK)
            featured->author[0] = 0;
    }
}

static void refresh_progress(void) {
    for (int i = 0; i < 7; ++i) {
        home_book_t *item = book_at(i);
        struct stat st;
        if (!item->path[0] || stat(item->path, &st) || !S_ISREG(st.st_mode) ||
            st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) continue;
        item->has_progress = book_progress_load(item->path, (uint32_t)st.st_size, &item->progress);
    }
}

static void remember_scan_state(const read_pico_sd_info_t *sd) {
    s_books_cache_valid = true;
    s_cached_store_revision = book_store_revision();
    s_cached_sd_present = sd->present;
    s_cached_sd_mounted = sd->mounted;
    snprintf(s_cached_books_dir, sizeof(s_cached_books_dir), "%s", app_settings_books_dir());
    s_cached_last_path[0] = 0;
    (void)book_progress_last_path(s_cached_last_path, sizeof(s_cached_last_path));
}

static home_book_t *book_at(int index) { return index ? &s_recent[index - 1] : &s_current; }
static void clear_cover_cache(void) {
    for (int i = 0; i < 7; ++i) {
        free(s_cover_cache[i].pixels);
        memset(&s_cover_cache[i], 0, sizeof(s_cover_cache[i]));
    }
    s_current.cover = NULL;
    for (int i = 0; i < 6; ++i) s_recent[i].cover = NULL;
}

static void restore_cover_cache(void) {
    home_cover_cache_t old[7];
    memcpy(old, s_cover_cache, sizeof(old));
    memset(s_cover_cache, 0, sizeof(s_cover_cache));
    for (int i = 0; i < 7; ++i) {
        home_book_t *book = book_at(i);
        struct stat st;
        book->cover = NULL;
        if (!book->path[0] || stat(book->path, &st) || !S_ISREG(st.st_mode)) continue;
        for (int j = 0; j < 7; ++j) {
            if (!old[j].pixels || strcmp(book->path, old[j].path) ||
                strcmp(book->title, old[j].title) || strcmp(book->author, old[j].author) ||
                st.st_size != old[j].size || st.st_mtime != old[j].modified) continue;
            s_cover_cache[i] = old[j];
            book->cover = old[j].pixels;
            old[j].pixels = NULL;
            break;
        }
    }
    for (int i = 0; i < 7; ++i) free(old[i].pixels);
}

static bool load_cover(home_book_t *item, int slot) {
    uint8_t *pixels = heap_caps_malloc(BOOK_COVER_W * BOOK_COVER_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = pixels && book_cover_load_gray(item->path, item->title, item->author,
                                              pixels, true, NULL);
    if (!ok) { free(pixels); return false; }
    struct stat st;
    if (stat(item->path, &st) || !S_ISREG(st.st_mode)) { free(pixels); return false; }
    home_cover_cache_t *cached = &s_cover_cache[slot];
    free(cached->pixels);
    snprintf(cached->path, sizeof(cached->path), "%s", item->path);
    snprintf(cached->title, sizeof(cached->title), "%s", item->title);
    snprintf(cached->author, sizeof(cached->author), "%s", item->author);
    cached->size = st.st_size;
    cached->modified = st.st_mtime;
    cached->pixels = pixels;
    item->cover = pixels;
    return ok;
}

static void fit(char *title, int px, int width) {
    while (*title && ui_text_fixed_width_px(ui_text_effective_px(px), title) > width) {
        size_t n = strlen(title) - 1;
        while (n && ((unsigned char)title[n] & 0xc0) == 0x80) --n;
        title[n] = 0;
    }
}

static int home_title_px(const char *title, int width) {
    for (int px = 39; px > 24; --px)
        if (ui_text_fixed_width_px(ui_text_effective_px(px), title) <= width) return px;
    return 24;
}

static void draw_cover(uint8_t *fb, const home_book_t *item, EpdRect box) {
    epd_fill_rect(box, UI_GRAY_LIGHT, fb);
    if (item->cover) {
        for (int y = 0; y < box.height; ++y)
            for (int x = 0; x < box.width; ++x) {
                int sx = x * BOOK_COVER_W / box.width;
                int sy = y * BOOK_COVER_H / box.height;
                const uint8_t gray = ui_contrast_gray(item->cover[sy * BOOK_COVER_W + sx]);
                epd_draw_pixel(box.x + x, box.y + y,
                               ui_image_dither_gray(gray, box.x + x, box.y + y), fb);
            }
    } else {
        char title[128]; snprintf(title, sizeof(title), "%s", item->title);
        fit(title, 24, box.width - 16);
        ui_text_vc(fb, box.x + box.width / 2, box.y + box.height / 2, 24, title, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_draw_round_rect(fb, box, 0, UI_GRAY_BLACK);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    // 题头与横线对齐书架，续读区域使用已确认预览的下移坐标。
    // Align the title and rule with the shelf and place the resume section at the approved lower coordinates.
    epd_fill_rect((EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_TOP}, 0xf0, fb);
    ui_nav_status(fb);
    ui_text(fb, 36, 90, 52, "首页", EPD_DRAW_ALIGN_LEFT, false);
    epd_fill_rect((EpdRect){36, 195, 612, 2}, 0x68, fb);
    ui_text_vc(fb, 36, 237, 29, "继续阅读", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(fb, 648, 237, 19, "查看详情 ›", EPD_DRAW_ALIGN_RIGHT, false);
    home_book_t *featured = s_current.path[0] ? &s_current : &s_recent[0];
    if (featured->path[0]) {
        draw_cover(fb, featured, (EpdRect){36, 292, 172, 250});
        char title[128]; snprintf(title, sizeof(title), "%s", featured->title);
        int title_px = home_title_px(title, 374);
        fit(title, title_px, 374);
        ui_text_vc(fb, 248, 322, title_px, title, EPD_DRAW_ALIGN_LEFT, false);
        if (featured->author[0]) {
            char author[sizeof(featured->author)];
            snprintf(author, sizeof(author), "%s", featured->author);
            fit(author, 22, 374);
            ui_text_vc(fb, 248, 378, 22, author, EPD_DRAW_ALIGN_LEFT, false);
        }
        char detail[80];
        snprintf(detail, sizeof(detail), "已读 %u%%", featured->has_progress ? featured->progress.pct : 0);
        ui_text_vc(fb, 248, 449, 22, detail, EPD_DRAW_ALIGN_LEFT, false);
        EpdRect track = {248, 470, 374, 9};
        ui_fill_round_rect(fb, track, 5, 0xb0);
        track.width = track.width * (featured->has_progress ? featured->progress.pct : 0) / 100;
        if (track.width > 0) ui_fill_round_rect(fb, track, 5, 0x38);
        ui_text_vc(fb, 248, 518, 24, "继续阅读 ›", EPD_DRAW_ALIGN_LEFT, false);
    } else {
        ui_text_vc(fb, 342, 418, 23, "打开一本书后，会在这里继续阅读", EPD_DRAW_ALIGN_CENTER, false);
    }

    epd_fill_rect((EpdRect){36, 619, 612, 2}, 0x68, fb);
    ui_text_vc(fb, 36, 643, 29, "近7天阅读", EPD_DRAW_ALIGN_LEFT, false);
    uint64_t sum = 0;
    uint32_t maximum = 0;
    for (int i = 23; i < 30; ++i) {
        sum += s_reading_days[i];
        if (s_reading_days[i] > maximum) maximum = s_reading_days[i];
    }
    char value[64];
    if (sum >= 3600)
        snprintf(value, sizeof(value), "累计 %llu 小时", (unsigned long long)(sum / 3600));
    else
        snprintf(value, sizeof(value), "累计 %llu 分钟", (unsigned long long)(sum / 60));
    ui_text_vc(fb, 648, 643, 20, value, EPD_DRAW_ALIGN_RIGHT, false);
    ui_fill_round_rect(fb, (EpdRect){36, 698, 612, 334}, 25, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, (EpdRect){36, 698, 612, 334}, 25, 0x70);
    ui_draw_round_rect(fb, (EpdRect){37, 699, 610, 332}, 24, 0x70);
    ui_text_vc(fb, 62, 743, 22, "每天阅读时长", EPD_DRAW_ALIGN_LEFT, false);
    uint64_t average_minutes = (sum + 210) / 420;
    if (average_minutes >= 60)
        snprintf(value, sizeof(value), "日均 %llu 小时%llu分",
                 (unsigned long long)(average_minutes / 60),
                 (unsigned long long)(average_minutes % 60));
    else
        snprintf(value, sizeof(value), "日均 %llu 分钟", (unsigned long long)average_minutes);
    ui_text_vc(fb, 628, 743, 19, value, EPD_DRAW_ALIGN_RIGHT, false);
    if (!s_reading_days_valid) {
        ui_text_vc(fb, 342, 867, 22, "完成日期与时间设置后显示阅读记录", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        unsigned top_hours = (maximum + 3599) / 3600;
        if (top_hours < 3) top_hours = 3;
        if (top_hours % 3) top_hours += 3 - top_hours % 3;
        const int grid_y[] = {791, 843, 895};
        for (int tick = 0; tick < 3; ++tick) {
            snprintf(value, sizeof(value), "%uh", top_hours * (3 - tick) / 3);
            ui_text_vc(fb, 61, grid_y[tick], 18, value, EPD_DRAW_ALIGN_LEFT, false);
            ui_hairline(fb, grid_y[tick], 102, 520, 0x90);
        }
        ui_hairline(fb, 947, 102, 520, 0x90);
        static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        time_t now = time(NULL);
        struct tm date = {0};
        gmtime_r(&(time_t){now + 8 * 3600}, &date);
        for (int i = 0; i < 7; ++i) {
            int cx = 127 + i * 80;
            uint32_t seconds = s_reading_days[23 + i];
            int height = (int)((uint64_t)seconds * 156 / ((uint64_t)top_hours * 3600));
            if (seconds && height < 5) height = 5;
            if (height > 156) height = 156;
            if (height) ui_fill_round_rect(fb, (EpdRect){cx - 18, 947 - height, 36, height},
                                           height < 36 ? height / 2 : 18, 0x38);
            ui_text_vc(fb, cx, 977, 19, weekdays[(date.tm_wday + 1 + i) % 7],
                       EPD_DRAW_ALIGN_CENTER, false);
        }
    }
    ui_nav_draw(fb, 0);
}

static bool present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_NONE) return true;
    render(ctx, ctx->fb);
    if (redraw == APP_REDRAW_AREA) guard_draw_result(ctx->hl, update_display_area_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_area));
    else if (redraw == APP_REDRAW_FULL || s_home_force_full_once) {
        s_home_force_full_once = false;
        guard_draw_result(ctx->hl, update_display_full(ctx->hl));
    }
    else guard_draw_result(ctx->hl, update_display_fast_page(ctx->hl));
    return true;
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_home_force_full_once = app_settings_home_full_refresh();
    s_reading_days_valid = book_ticket_recent_days(s_reading_days);
    read_pico_sd_info_t sd = {0};
    esp_err_t sd_err = read_pico_sd_get_info(&sd);
    bool cache_matches = s_books_cache_valid && sd_err != ESP_ERR_NOT_FINISHED &&
                         s_cached_store_revision == book_store_revision() &&
                         s_cached_sd_present == sd.present && s_cached_sd_mounted == sd.mounted &&
                         !strcmp(s_cached_books_dir, app_settings_books_dir());
    char last_path[BOOK_STORE_PATH_MAX] = {0};
    (void)book_progress_last_path(last_path, sizeof(last_path));
    bool last_changed = strcmp(last_path, s_cached_last_path) != 0;
    if (cache_matches && !last_changed) {
        refresh_progress();
        s_cover_next = 0;
        s_scan_pending = false;
        return;
    }
    memset(&s_current, 0, sizeof(s_current));
    memset(s_recent, 0, sizeof(s_recent));
    s_cover_next = 0;
    // 阅读记录换书时重建续读选择；TF 卡状态没变则无需再次探测。
    // Rebuild the featured book when the latest read changes without probing an unchanged card.
    s_scan_pending = !cache_matches;
    if (s_scan_pending) read_pico_sd_start_probe();
    scan_books();
    restore_cover_cache();
    if (cache_matches || sd_err != ESP_ERR_NOT_FINISHED) remember_scan_state(&sd);
}
static void on_media_lost(app_ctx_t *ctx) {
    (void)ctx;
    clear_cover_cache();
    memset(&s_current, 0, sizeof(s_current));
    memset(s_recent, 0, sizeof(s_recent));
    s_books_cache_valid = false;
}
static void on_media_ready(app_ctx_t *ctx) {
    (void)ctx;
    s_books_cache_valid = false;
    s_scan_pending = true;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (ctx->touch && ctx->touch->touched) return APP_REDRAW_NONE;
    if (s_scan_pending) {
        read_pico_sd_info_t info = {0};
        if (read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED) return APP_REDRAW_NONE;
        s_scan_pending = false;
        if (info.mounted) {
            memset(&s_current, 0, sizeof(s_current));
            memset(s_recent, 0, sizeof(s_recent));
            s_cover_next = 0;
            scan_books();
            restore_cover_cache();
            remember_scan_state(&info);
            return APP_REDRAW_PAGE;
        }
        remember_scan_state(&info);
    }
    bool cover_changed = false;
    if (!s_cover_next) {
        s_cover_next = 1;
        int slot = s_current.path[0] ? 0 : 1;
        home_book_t *item = book_at(slot);
        if (item->path[0] && !item->cover) cover_changed = load_cover(item, slot);
    }
    // 只更新续读卡片，不再逐本刷新首页。/ Update only the current-read card, never flash covers one by one.
    if (cover_changed) {
        render(ctx, ctx->fb);
        s_area = (EpdRect){36, 268, 612, 351};
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int tab = ui_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    if (ev->y0 >= 216 && ev->y0 < 619) {
        home_book_t *featured = s_current.path[0] ? &s_current : &s_recent[0];
        if (featured->path[0]) app_book_request_open_from_home(featured->path);
        ui_nav_request(ctx, 1);
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    // 首页是导航根页，返回停留在首页。/ Home is the navigation root; Back stays here.
    (void)ctx;
    (void)key;
    return APP_REDRAW_NONE;
}
static bool no_menu_handle(app_ctx_t *ctx) { (void)ctx; return false; }
static EpdRect area_hint(app_ctx_t *ctx) { (void)ctx; return s_area; }

const app_desc_t app_dashboard = {
    .title = "首页 Home", .detail = "继续阅读与近七天阅读", .enter_full = false,
    .owns_keys = true, .menu_handle_enabled = no_menu_handle,
    .on_enter = on_enter, .on_media_lost = on_media_lost, .on_media_ready = on_media_ready,
    .render = render, .present = present, .on_tick = on_tick,
    .on_gesture = on_gesture, .on_key = on_key, .area_hint = area_hint,
};
