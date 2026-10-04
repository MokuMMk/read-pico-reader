/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：锁屏票根从持久进度与真实阅读计数生成；没有可靠时钟时不编造今日数据。
 * 票面上下以裁纸缺口绘制，最近阅读的 EPUB 封面作为灰阶背景。
 * English: Build the ticket from persisted progress and measured reading data; omit daily data without a valid clock.
 * Scalloped paper edges sit over a grayscale wallpaper made from the latest EPUB cover.
 */
#include "book_ticket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "book_cover.h"
#include "book_epub.h"
#include "book_progress.h"
#include "book_title.h"
#include "epdiy.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "read_pico_pmu.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_image_dither.h"

#define TICKET_MAGIC UINT32_C(0x52505431)
typedef struct {
    uint32_t magic, day;
    uint64_t total_seconds, today_seconds;
    uint32_t total_turns, today_turns;
} ticket_stats_t;
#define TICKET_HEAT_MAGIC UINT32_C(0x52504831)
typedef struct {
    uint32_t magic;
    uint32_t day[30];
    uint32_t seconds[30];
} ticket_heat_t;
static uint32_t local_day(void) {
    time_t now = time(NULL);
    if (now < 1704067200) {
        const pmu_snapshot_t *pmu = read_pico_pmu_get();
        if (pmu && pmu->time_synced && pmu->unix_sec >= 1704067200) now = pmu->unix_sec;
    }
    if (now < 1704067200) return 0;
    return (uint32_t)((now + 8 * 3600) / 86400);
}
static void stats_load(ticket_stats_t *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open("rp_ticket", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, "stats", out, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*out) || out->magic != TICKET_MAGIC) memset(out, 0, sizeof(*out));
}
esp_err_t book_ticket_record(uint32_t seconds, uint32_t turns) {
    if (!seconds && !turns) return ESP_OK;
    ticket_stats_t stats; stats_load(&stats);
    stats.magic = TICKET_MAGIC;
    uint32_t day = local_day();
    if (day && stats.day != day) { stats.day = day; stats.today_seconds = 0; stats.today_turns = 0; }
    stats.total_seconds += seconds;
    stats.total_turns += turns;
    if (day) { stats.today_seconds += seconds; stats.today_turns += turns; }
    nvs_handle_t h;
    esp_err_t err = nvs_open("rp_ticket", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "stats", &stats, sizeof(stats));
    if (err == ESP_OK && day && seconds) {
        ticket_heat_t heat = {0};
        size_t len = sizeof(heat);
        if (nvs_get_blob(h, "heatmap", &heat, &len) != ESP_OK ||
            len != sizeof(heat) || heat.magic != TICKET_HEAT_MAGIC) {
            memset(&heat, 0, sizeof(heat));
            heat.magic = TICKET_HEAT_MAGIC;
        }
        unsigned slot = day % 30;
        if (heat.day[slot] != day) { heat.day[slot] = day; heat.seconds[slot] = 0; }
        uint32_t old = heat.seconds[slot];
        heat.seconds[slot] = UINT32_MAX - old < seconds ? UINT32_MAX : old + seconds;
        err = nvs_set_blob(h, "heatmap", &heat, sizeof(heat));
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
bool book_ticket_recent_days(uint32_t seconds[30]) {
    if (!seconds) return false;
    memset(seconds, 0, 30 * sizeof(*seconds));
    uint32_t today = local_day();
    if (!today) return false;
    nvs_handle_t h;
    if (nvs_open("rp_ticket", NVS_READONLY, &h) != ESP_OK) return true;
    ticket_heat_t heat = {0};
    size_t len = sizeof(heat);
    esp_err_t err = nvs_get_blob(h, "heatmap", &heat, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(heat) || heat.magic != TICKET_HEAT_MAGIC) return true;
    for (unsigned i = 0; i < 30; ++i) {
        uint32_t day = today - (29 - i);
        unsigned slot = day % 30;
        if (heat.day[slot] == day) seconds[i] = heat.seconds[slot];
    }
    return true;
}
static void text_fit(char *text, int px, int width) {
    while (*text && ttf_text_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}
static uint8_t *cover_load(const char *path, const char *title, const char *author) {
    uint8_t *gray = heap_caps_malloc(BOOK_COVER_W * BOOK_COVER_H,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gray && !book_cover_load_gray(path, title, author, gray, true, NULL)) {
        free(gray);
        gray = NULL;
    }
    return gray;
}
static void cover_background_draw(uint8_t *fb, const uint8_t *gray) {
    if (!gray) {
        epd_fill_rect((EpdRect){0, 0, UI_LOCK_WIDTH, UI_LOCK_HEIGHT}, 0xb0, fb);
        return;
    }
    int crop_x = 0, crop_y = 0, crop_w = BOOK_COVER_W, crop_h = BOOK_COVER_H;
    if ((int64_t)BOOK_COVER_W * UI_LOCK_HEIGHT > (int64_t)BOOK_COVER_H * UI_LOCK_WIDTH) {
        crop_w = BOOK_COVER_H * UI_LOCK_WIDTH / UI_LOCK_HEIGHT;
        crop_x = (BOOK_COVER_W - crop_w) / 2;
    } else {
        crop_h = BOOK_COVER_W * UI_LOCK_HEIGHT / UI_LOCK_WIDTH;
        crop_y = (BOOK_COVER_H - crop_h) / 2;
    }
    for (int y = 0; y < UI_LOCK_HEIGHT; ++y) {
        const int sy = crop_y + y * crop_h / UI_LOCK_HEIGHT;
        for (int x = 0; x < UI_LOCK_WIDTH; ++x) {
            const int sx = crop_x + x * crop_w / UI_LOCK_WIDTH;
            const uint8_t tone = ui_contrast_gray(gray[sy * BOOK_COVER_W + sx]);
            epd_draw_pixel(x, y, ui_image_dither_gray(tone, x, y), fb);
        }
    }
}
static int notch_depth(int x) {
    const int radius = 18, first = 80, step = 64;
    int dx = (x - first) % step;
    if (dx < 0) dx += step;
    if (dx > step / 2) dx -= step;
    if (dx < -radius || dx > radius) return 0;
    return radius - dx * dx / radius;
}
static void ticket_paper_draw(uint8_t *fb) {
    const int left = 48, right = 636, top = 58, bottom = 1158;
    for (int x = left; x < right; ++x) {
        int cut = notch_depth(x);
        int y0 = top + cut, y1 = bottom - cut;
        epd_fill_rect((EpdRect){x, y0, 1, y1 - y0}, UI_GRAY_WHITE, fb);
        epd_draw_pixel(x, y0, UI_GRAY_BLACK, fb);
        epd_draw_pixel(x, y1 - 1, UI_GRAY_BLACK, fb);
    }
    epd_fill_rect((EpdRect){left, top + 20, 1, bottom - top - 40}, UI_GRAY_BLACK, fb);
    epd_fill_rect((EpdRect){right - 1, top + 20, 1, bottom - top - 40}, UI_GRAY_BLACK, fb);
}
static void ticket_shadow_draw(uint8_t *fb) {
    const int left = 48, right = 636, top = 58, bottom = 1158;
    for (int x = left; x < right; ++x) {
        int cut = notch_depth(x);
        int y0 = top + cut + 8, y1 = bottom - cut + 8;
        epd_fill_rect((EpdRect){x + 5, y0, 1, y1 - y0}, 0x80, fb);
    }
}
static void cover_draw(uint8_t *fb, const uint8_t *gray, const char *title) {
    EpdRect box = {216, 378, 252, 344};
    epd_fill_rect(box, UI_GRAY_LIGHT, fb);
    if (gray) {
        for (int y = 0; y < box.height; ++y)
            for (int x = 0; x < box.width; ++x) {
                int sx = x * BOOK_COVER_W / box.width;
                int sy = y * BOOK_COVER_H / box.height;
                const uint8_t tone = ui_contrast_gray(gray[sy * BOOK_COVER_W + sx]);
                epd_draw_pixel(box.x + x, box.y + y,
                               ui_image_dither_gray(tone, box.x + x, box.y + y), fb);
            }
    } else {
        char short_title[96]; snprintf(short_title, sizeof(short_title), "%s", title);
        text_fit(short_title, 32, box.width - 20);
        ui_text_vc(fb, box.x + box.width / 2, box.y + box.height / 2, 32,
                   short_title, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_draw_round_rect(fb, box, 0, UI_GRAY_BLACK);
}
bool book_ticket_draw(uint8_t *fb, bool reader_background) {
    (void)reader_background;
    char path[288];
    if (!book_progress_last_path(path, sizeof(path))) return false;
    struct stat st;
    bool file_valid = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
                      (uint64_t)st.st_size <= UINT32_MAX;
    book_progress_t progress = {0};
    bool progress_valid = file_valid && book_progress_load(path, st.st_size, &progress);
    char title[256] = {0}, author[128] = {0};
    if (!book_title_from_path(path, title, sizeof(title))) snprintf(title, sizeof(title), "未命名");
    const char *ext = strrchr(path, '.');
    if (ext && !strcasecmp(ext, ".epub")) {
        char meta_title[256] = {0};
        (void)book_epub_metadata(path, meta_title, sizeof(meta_title), author, sizeof(author));
    }
    ticket_stats_t stats; stats_load(&stats);
    uint32_t day = local_day();
    uint8_t *gray = cover_load(path, title, author);
    cover_background_draw(fb, gray);
    ticket_shadow_draw(fb);
    ticket_paper_draw(fb);
    ui_text(fb, 82, 98, 28, "Pico  /  阅读票根", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 156, 82, 520, UI_GRAY_BLACK);
    text_fit(title, 50, 520);
    ui_text(fb, 82, 202, 50, title, EPD_DRAW_ALIGN_LEFT, false);
    if (author[0]) { text_fit(author, 28, 520); ui_text(fb, 82, 274, 28, author, EPD_DRAW_ALIGN_LEFT, false); }
    cover_draw(fb, gray, title);
    free(gray);
    ui_hairline(fb, 768, 82, 520, UI_GRAY_BLACK);
    char line[96];
    if (progress_valid) snprintf(line, sizeof(line), "已读 %u%%  ·  第 %u 章", progress.pct,
                                 (unsigned)progress.chapter + 1);
    else snprintf(line, sizeof(line), "已读 —  ·  章节 —");
    ui_text(fb, 82, 802, 34, line, EPD_DRAW_ALIGN_LEFT, false);
    EpdRect track = {82, 868, 520, 10};
    epd_fill_rect(track, UI_GRAY_LIGHT, fb);
    track.width = progress_valid ? track.width * progress.pct / 100 : 0;
    if (track.width) epd_fill_rect(track, UI_GRAY_BLACK, fb);
    if (day && stats.day == day) snprintf(line, sizeof(line), "今日阅读 %llu 分钟  ·  翻页 %u 次",
        (unsigned long long)(stats.today_seconds / 60), (unsigned)stats.today_turns);
    else snprintf(line, sizeof(line), "今日阅读 —  ·  日期未同步");
    ui_text(fb, 82, 914, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "累计阅读 %llu 小时 %llu 分钟  ·  翻页 %u 次",
             (unsigned long long)(stats.total_seconds / 3600),
             (unsigned long long)((stats.total_seconds / 60) % 60), (unsigned)stats.total_turns);
    ui_text(fb, 82, 969, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 1050, 82, 520, UI_GRAY_BLACK);
    ui_text(fb, 82, 1080, 26, "在每一页里，留下时间的凭证", EPD_DRAW_ALIGN_LEFT, false);
    return true;
}
