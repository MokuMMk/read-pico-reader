/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TF 卡文件入口、目录浏览及 USB/Wi-Fi 传输入口。
 * English: SD file entry, directory browser and USB/Wi-Fi transfer routes.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "app_content_open.h"
#include "app_transfer_mode.h"
#include "book_store.h"
#include "book_progress.h"
#include "book_title.h"
#include "display.h"
#include "file_tree.h"
#include "ota_update.h"
#include "read_pico_sd.h"
#include "read_pico_search.h"
#include "settings.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "pmu_selftest.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "soc/rtc_cntl_reg.h"

#define FILE_MAX 96
#define FILE_ROWS 9
typedef struct { char path[288]; char name[128]; off_t size; bool is_dir; } file_item_t;
static file_item_t s_files[FILE_MAX];
static int s_count, s_folder, s_page;
static int s_requested_folder = -1;
static int s_counts[3];
static bool s_scan_pending;
static char s_message[96];
static char s_dir[288] = "/sdcard";
static read_pico_sd_info_t s_sd;
typedef enum { FILE_VIEW_LIST, FILE_VIEW_ACTIONS, FILE_VIEW_RENAME, FILE_VIEW_MOVE,
               FILE_VIEW_BOOT } file_view_t;
static file_view_t s_view;
static bool s_boot_pending;
typedef enum { OTA_VIEW_IDLE, OTA_VIEW_CONFIRM, OTA_VIEW_INSTALLING,
               OTA_VIEW_SUCCESS, OTA_VIEW_FAILED } ota_view_t;
static ota_view_t s_ota_view;
static pico_ota_info_t s_ota_info;
static bool s_ota_work_pending, s_ota_restart_pending;
static char s_ota_result[96];
static file_item_t s_selected;
static bool s_delete_confirm;
static int s_move_origin_folder, s_move_origin_page;
static char s_move_origin_dir[288];
static char s_editor[121], s_editor_ext[16], s_editor_pinyin[9], s_editor_notice[96];
static bool s_editor_chinese;
static size_t s_editor_candidate_page, s_editor_candidate_count;
static uint32_t s_editor_candidates[5];
static void scan_folder(int folder);

static void ota_refresh_info(void) {
    memset(&s_ota_info, 0, sizeof(s_ota_info));
    (void)pico_ota_inspect(PICO_OTA_UPDATE_PATH, &s_ota_info);
    s_ota_view = OTA_VIEW_IDLE;
    s_ota_result[0] = 0;
}

static const char *const names[] = {"书籍", "图片", "字体", "TF 卡目录"};
static const char *folder_root(int folder) {
    if (folder == 0) return app_settings_books_dir();
    if (folder == 1) return "/sdcard/pictures";
    if (folder == 2) return app_settings_fonts_dir();
    return s_dir;
}
static int visible_rows(void) { return s_folder == 3 ? 7 : FILE_ROWS; }
static int root_rows(void) { return 5; }
static EpdRect home_transfer_rect(int index) {
    return (EpdRect){36 + index * 208, 346, 196, 100};
}

static void copy_utf8(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
    }
    memcpy(dst, src, n); dst[n] = 0;
}

static bool is_book_path(const char *path) {
    const char *ext = strrchr(path, '.');
    return ext && (!strcasecmp(ext, ".epub") || !strcasecmp(ext, ".txt"));
}

static bool sibling_path(const char *name, char out[288]) {
    const char *slash = strrchr(s_selected.path, '/');
    if (!slash || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) return false;
    size_t parent = (size_t)(slash - s_selected.path);
    if (!name[0] || name[0] == '.' || parent + 1 + strlen(name) >= 288) return false;
    memcpy(out, s_selected.path, parent);
    out[parent] = '/';
    strcpy(out + parent + 1, name);
    return true;
}

static void migrate_book_state(const char *old_path, const char *new_path, uint32_t size) {
    if (!is_book_path(old_path)) return;
    book_progress_t progress = {0};
    bool had_progress = book_progress_load(old_path, size, &progress);
    char last[BOOK_STORE_PATH_MAX] = {0};
    bool was_last = book_progress_last_path(last, sizeof(last)) && !strcmp(last, old_path);
    if (had_progress) (void)book_progress_save(new_path, &progress);
    (void)book_progress_forget(old_path);
    if (was_last) (void)book_progress_set_last_path(new_path);
    (void)book_title_clear(old_path);
}

static esp_err_t copy_selected_item(char out_name[128]) {
    const char *dot = s_selected.is_dir ? NULL : strrchr(s_selected.name, '.');
    size_t stem_len = dot && dot != s_selected.name ? (size_t)(dot - s_selected.name) : strlen(s_selected.name);
    char stem[100], candidate[128], destination[288];
    if (stem_len >= sizeof(stem)) stem_len = sizeof(stem) - 1;
    memcpy(stem, s_selected.name, stem_len); stem[stem_len] = 0;
    const char *ext = dot && dot != s_selected.name ? dot : "";
    for (int number = 1; number < 100; ++number) {
        int used = number == 1 ? snprintf(candidate, sizeof(candidate), "%s 副本%s", stem, ext) :
            snprintf(candidate, sizeof(candidate), "%s 副本 %d%s", stem, number, ext);
        if (used < 0 || used >= (int)sizeof(candidate)) return ESP_ERR_INVALID_SIZE;
        if (!sibling_path(candidate, destination)) return ESP_ERR_INVALID_ARG;
        struct stat exists;
        if (stat(destination, &exists) != 0) {
            if (errno == ENOENT) break;
            return ESP_FAIL;
        }
        if (number == 99) return ESP_ERR_INVALID_SIZE;
    }
    read_pico_sd_get_info(&s_sd);
    if (!s_sd.mounted) return ESP_ERR_INVALID_STATE;
    esp_err_t err = file_tree_copy(s_selected.path, destination, s_sd.free_bytes);
    if (err != ESP_OK) return err;
    copy_utf8(out_name, 128, candidate);
    book_store_notify_changed();
    return ESP_OK;
}

static bool rebased_path(const char *path, const char *old_path, const char *new_path,
                         char *out, size_t cap) {
    if (!file_tree_same_or_below(path, old_path)) return false;
    int n = snprintf(out, cap, "%s%s", new_path, path + strlen(old_path));
    return n >= 0 && (size_t)n < cap;
}

static bool moved_settings_fit(const char *old_path, const char *new_path) {
    char updated[288];
    const char *paths[] = {app_settings_books_dir(), app_settings_fonts_dir(),
        app_settings_system_font_path(), app_settings_font_path(), app_settings_wallpaper_path()};
    const size_t limits[] = {96, 96, 160, 160, 288};
    for (int i = 0; i < 5; ++i) {
        if (!file_tree_same_or_below(paths[i], old_path)) continue;
        if (!rebased_path(paths[i], old_path, new_path, updated, sizeof(updated)) ||
            strlen(updated) >= limits[i]) return false;
    }
    return true;
}

static void sync_moved_settings(const char *old_path, const char *new_path) {
    char updated[288];
    if (rebased_path(app_settings_books_dir(), old_path, new_path, updated, sizeof(updated)))
        (void)app_settings_set_books_dir(updated);
    if (rebased_path(app_settings_fonts_dir(), old_path, new_path, updated, sizeof(updated))) {
        (void)app_settings_set_fonts_dir(updated);
        ttf_font_scan();
    }
    if (rebased_path(app_settings_system_font_path(), old_path, new_path, updated, sizeof(updated))) {
        app_settings_set_system_font_path(updated);
        app_font_activate_system();
    }
    if (rebased_path(app_settings_font_path(), old_path, new_path, updated, sizeof(updated)))
        app_settings_set_font_path(updated);
    if (rebased_path(app_settings_wallpaper_path(), old_path, new_path, updated, sizeof(updated)))
        app_settings_set_wallpaper_path(updated);
}

static void notify_moved_tree(const char *old_path, const char *new_path, unsigned depth) {
    if (depth > 8) return;
    struct stat st;
    if (stat(new_path, &st)) return;
    if (S_ISREG(st.st_mode)) {
        migrate_book_state(old_path, new_path,
            st.st_size >= 0 && (uint64_t)st.st_size <= UINT32_MAX ? (uint32_t)st.st_size : 0);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    DIR *dir = opendir(new_path);
    if (!dir) return;
    char *paths = heap_caps_malloc(576, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!paths) { closedir(dir); return; }
    char *old_child = paths, *new_child = paths + 288;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (snprintf(old_child, 288, "%s/%s", old_path, entry->d_name) >= 288 ||
            snprintf(new_child, 288, "%s/%s", new_path, entry->d_name) >= 288) continue;
        notify_moved_tree(old_child, new_child, depth + 1);
    }
    free(paths);
    closedir(dir);
}

static void file_deleted(const char *path, bool directory, void *ctx) {
    (void)ctx;
    if (directory) return;
    if (is_book_path(path)) (void)book_progress_forget(path);
    (void)book_title_clear(path);
    if (!strcmp(app_settings_system_font_path(), path)) {
        app_settings_set_system_font_path("");
        app_font_activate_system();
    }
    if (!strcmp(app_settings_font_path(), path)) app_settings_set_font_path("");
    if (!strcmp(app_settings_wallpaper_path(), path)) {
        app_settings_set_wallpaper_path("");
        app_settings_set_lock_style(0);
    }
}

static void repair_deleted_directories(void) {
    struct stat st;
    if (stat(app_settings_books_dir(), &st) || !S_ISDIR(st.st_mode)) {
        (void)mkdir("/sdcard/books", 0775);
        (void)app_settings_set_books_dir("/sdcard/books");
    }
    if (stat(app_settings_fonts_dir(), &st) || !S_ISDIR(st.st_mode)) {
        (void)mkdir("/sdcard/fonts", 0775);
        (void)app_settings_set_fonts_dir("/sdcard/fonts");
        ttf_font_scan();
    }
}

static void start_move(void) {
    s_move_origin_folder = s_folder;
    s_move_origin_page = s_page;
    copy_utf8(s_move_origin_dir, sizeof(s_move_origin_dir), s_dir);
    copy_utf8(s_dir, sizeof(s_dir), "/sdcard");
    s_folder = 3;
    s_view = FILE_VIEW_MOVE;
    scan_folder(3);
}

static void leave_move(bool changed) {
    s_folder = s_move_origin_folder;
    copy_utf8(s_dir, sizeof(s_dir), s_move_origin_dir);
    s_view = FILE_VIEW_LIST;
    scan_folder(s_folder >= 0 ? s_folder : 3);
    if (!changed) s_page = s_move_origin_page;
}

static app_redraw_t move_selected(void) {
    read_pico_sd_get_info(&s_sd);
    if (!s_sd.mounted) {
        snprintf(s_message, sizeof(s_message), "TF 卡不可用"); return APP_REDRAW_PAGE;
    }
    char destination[288];
    if (snprintf(destination, sizeof(destination), "%s/%s", s_dir, s_selected.name) >= sizeof(destination)) {
        snprintf(s_message, sizeof(s_message), "目标路径过长"); return APP_REDRAW_PAGE;
    }
    if (!strcmp(destination, s_selected.path)) {
        snprintf(s_message, sizeof(s_message), "已在此文件夹"); return APP_REDRAW_PAGE;
    }
    if (!moved_settings_fit(s_selected.path, destination)) {
        snprintf(s_message, sizeof(s_message), "设置路径过长，无法移动"); return APP_REDRAW_PAGE;
    }
    esp_err_t err = file_tree_move(s_selected.path, destination);
    if (err != ESP_OK) {
        snprintf(s_message, sizeof(s_message), err == ESP_ERR_INVALID_STATE ? "目标已有同名项目" :
                 err == ESP_ERR_INVALID_ARG ? "不能移入自身文件夹" : "移动失败，请检查 TF 卡");
        return APP_REDRAW_PAGE;
    }
    sync_moved_settings(s_selected.path, destination);
    notify_moved_tree(s_selected.path, destination, 0);
    book_store_notify_changed();
    leave_move(true);
    snprintf(s_message, sizeof(s_message), "移动完成");
    return APP_REDRAW_PAGE;
}

void app_files_request_folder(int folder) {
    if (folder >= 0 && folder < 3) s_requested_folder = folder;
}

static bool supported(int folder, const char *name) {
    if (!name[0] || name[0] == '.' || !strncmp(name, "._", 2)) return false;
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (folder == 0) return !strcasecmp(ext, ".epub") || !strcasecmp(ext, ".txt");
    if (folder == 1) return !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png");
    return !strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf");
}

static int compare_files(const void *left, const void *right) {
    const file_item_t *a = left, *b = right;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static void scan_folder(int folder) {
    s_count = s_page = 0;
    s_message[0] = 0;
    read_pico_sd_get_info(&s_sd);
    if (!s_sd.mounted) { snprintf(s_message, sizeof(s_message), "TF 卡尚未挂载"); return; }
    const char *root = folder_root(folder);
    DIR *dir = opendir(root);
    if (!dir) { snprintf(s_message, sizeof(s_message), "目录不存在：%.60s", root); return; }
    struct dirent *entry;
    while (s_count < FILE_MAX && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.' || !strncmp(entry->d_name, "._", 2)) continue;
        file_item_t *item = &s_files[s_count];
        if (snprintf(item->path, sizeof(item->path), "%s/%s", root, entry->d_name) >= sizeof(item->path)) continue;
        struct stat st;
        bool is_dir = false, is_file = false;
        if (!stat(item->path, &st)) {
            is_dir = S_ISDIR(st.st_mode);
            is_file = S_ISREG(st.st_mode);
            item->size = st.st_size;
        } else {
            DIR *probe = opendir(item->path);
            if (probe) { is_dir = true; item->size = 0; closedir(probe); }
        }
        if (!is_dir && !is_file) continue;
        if (s_view == FILE_VIEW_MOVE && !is_dir) continue;
        if (folder != 3 && (!is_file || !supported(folder, entry->d_name))) continue;
        if (strnlen(entry->d_name, sizeof(item->name)) >= sizeof(item->name)) continue;
        strcpy(item->name, entry->d_name);
        item->is_dir = is_dir;
        ++s_count;
    }
    closedir(dir);
    qsort(s_files, s_count, sizeof(s_files[0]), compare_files);
    if (folder >= 0 && folder < 3) s_counts[folder] = s_count;
}

static void rescan_current(void) {
    scan_folder(s_folder >= 0 ? s_folder : 3);
}

static int count_root(int folder) {
    if (!s_sd.mounted) return 0;
    DIR *dir = opendir(folder_root(folder));
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) if (supported(folder, entry->d_name)) ++count;
    closedir(dir);
    return count;
}

static void file_icon(uint8_t *fb, int x, int y, bool folder) {
    if (folder) {
        epd_draw_line(x, y + 7, x + 15, y + 7, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 15, y + 7, x + 22, y + 13, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 22, y + 13, x + 42, y + 13, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 42, y + 13, x + 42, y + 37, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 42, y + 37, x, y + 37, UI_GRAY_BLACK, fb);
        epd_draw_line(x, y + 37, x, y + 7, UI_GRAY_BLACK, fb);
    } else {
        epd_draw_line(x + 5, y + 2, x + 31, y + 2, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 31, y + 2, x + 40, y + 11, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 40, y + 11, x + 40, y + 40, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 40, y + 40, x + 5, y + 40, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 5, y + 40, x + 5, y + 2, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 31, y + 2, x + 31, y + 11, UI_GRAY_BLACK, fb);
        epd_draw_line(x + 31, y + 11, x + 40, y + 11, UI_GRAY_BLACK, fb);
    }
}

static void file_detail(char *dst, size_t cap, const file_item_t *item) {
    if (item->is_dir) snprintf(dst, cap, "文件夹");
    else if (item->size >= 1024 * 1024) snprintf(dst, cap, "%.1f MB", item->size / 1048576.0);
    else if (item->size >= 1024) snprintf(dst, cap, "%.1f KB", item->size / 1024.0);
    else snprintf(dst, cap, "%lld B", (long long)item->size);
}

// 文件列表的分隔线保持两像素，墨水屏上比浅色发丝线更容易辨认。
// Two-pixel file dividers stay legible on the panel without changing the layout.
static void file_rule(uint8_t *fb, int x, int y, int width) {
    epd_fill_rect((EpdRect){x, y, width, 2}, 0x68, fb);
}

// 大卡片用双层描边，避免单像素浅灰线在低对比度屏上消失。
// Double-stroke large cards so their outlines survive low-contrast E-ink rendering.
static void file_card_border(uint8_t *fb, EpdRect rect, int radius) {
    ui_draw_round_rect(fb, rect, radius, 0x70);
    ui_draw_round_rect(fb, (EpdRect){rect.x + 1, rect.y + 1,
                                     rect.width - 2, rect.height - 2}, radius - 1, 0x70);
}

static void fit_name(char *name, int px, int width) {
    while (*name && ttf_text_width_px(ui_text_effective_px(px), name) > width) {
        size_t n = strlen(name) - 1;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
        name[n] = 0;
    }
}

static void editor_refresh(void) {
    s_editor_candidate_count = s_editor_chinese && s_editor_pinyin[0]
        ? read_pico_search_candidates(s_editor_pinyin, s_editor_candidates, 5,
                                      s_editor_candidate_page * 5) : 0;
}

static void editor_start(void) {
    if (strlen(s_selected.name) >= sizeof(s_editor)) {
        snprintf(s_message, sizeof(s_message), "名称过长，无法在设备上编辑");
        return;
    }
    copy_utf8(s_editor, sizeof(s_editor), s_selected.name);
    s_editor_ext[0] = 0;
    char *dot = strrchr(s_editor, '.');
    if (!s_selected.is_dir && dot && dot != s_editor) {
        copy_utf8(s_editor_ext, sizeof(s_editor_ext), dot);
        *dot = 0;
    }
    s_editor_pinyin[0] = s_editor_notice[0] = 0;
    s_editor_chinese = true;
    s_editor_candidate_page = s_editor_candidate_count = 0;
    s_view = FILE_VIEW_RENAME;
}

static bool editor_append(const char *text) {
    size_t used = strlen(s_editor), more = strlen(text);
    if (used + more >= sizeof(s_editor)) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "文件名过长");
        return false;
    }
    memcpy(s_editor + used, text, more + 1);
    s_editor_notice[0] = 0;
    return true;
}

static void editor_backspace(void) {
    if (s_editor_pinyin[0]) {
        s_editor_pinyin[strlen(s_editor_pinyin) - 1] = 0;
        s_editor_candidate_page = 0; editor_refresh(); return;
    }
    size_t n = strlen(s_editor);
    if (!n) return;
    do { --n; } while (n && ((unsigned char)s_editor[n] & 0xc0) == 0x80);
    s_editor[n] = 0;
}

static void editor_commit_candidate(size_t index) {
    if (index >= s_editor_candidate_count) return;
    uint32_t cp = s_editor_candidates[index];
    if (cp < 0x800 || cp > 0xffff) return;
    char glyph[4] = {(char)(0xe0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 63)),
                     (char)(0x80 | (cp & 63)), 0};
    if (editor_append(glyph)) {
        s_editor_pinyin[0] = 0; s_editor_candidate_page = 0; editor_refresh();
    }
}

static void render_actions(uint8_t *fb) {
    // S03：保留当前目录作背景，只叠加文件操作底部面板。/ S03: keep the directory visible and overlay a file-action sheet.
    EpdRect sheet = {20, 430, 644, 626};
    ui_fill_round_rect(fb, sheet, 28, UI_GRAY_WHITE);
    file_card_border(fb, sheet, 28);
    ui_fill_round_rect(fb, (EpdRect){604, 454, 38, 38}, 19, 0xe0);
    ui_text_vc(fb, 623, 473, 24, "×", EPD_DRAW_ALIGN_CENTER, false);
    char name[128]; copy_utf8(name, sizeof(name), s_selected.name); fit_name(name, 28, 540);
    file_icon(fb, 48, 467, s_selected.is_dir);
    ui_text(fb, 102, 462, 28, name, EPD_DRAW_ALIGN_LEFT, false);
    char detail[48]; file_detail(detail, sizeof(detail), &s_selected);
    ui_text(fb, 102, 507, 20, detail, EPD_DRAW_ALIGN_LEFT, false);
    char path[128]; copy_utf8(path, sizeof(path), s_selected.path); fit_name(path, 19, 585);
    ui_text(fb, 50, 552, 19, path, EPD_DRAW_ALIGN_LEFT, false);
    file_rule(fb, 42, 590, 600);
    if (s_message[0]) ui_text(fb, 50, 608, 20, s_message, EPD_DRAW_ALIGN_LEFT, false);
    if (s_delete_confirm) {
        ui_text_vc(fb, 342, 712, s_selected.is_dir ? 23 : 28,
                   s_selected.is_dir ? "将删除文件夹内所有内容，无法恢复" : "删除后无法恢复，是否继续？",
                   EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, (EpdRect){50, 788, 278, 76}, "取消", false);
        ui_draw_button(fb, (EpdRect){356, 788, 278, 76}, "确认删除", true);
    } else {
        static const char *labels[] = {"重命名", "复制", "移动", "删除"};
        for (int i = 0; i < 4; ++i) {
            EpdRect button = {40 + (i % 2) * 312, 648 + (i / 2) * 122, 292, 100};
            ui_fill_round_rect(fb, button, 20, 0xe8);
            ui_draw_round_rect(fb, button, 20, 0x60);
            ui_text_vc(fb, button.x + button.width / 2, button.y + 50, 25,
                       labels[i], EPD_DRAW_ALIGN_CENTER, false);
        }
        ui_text_vc(fb, 342, 900, 20, "长按文件或文件夹打开操作", EPD_DRAW_ALIGN_CENTER, false);
        ui_fill_round_rect(fb, (EpdRect){40, 930, 604, 76}, 26, 0xe0);
        ui_text_vc(fb, 342, 968, 23, "取消", EPD_DRAW_ALIGN_CENTER, false);
    }
}

static void render_rename(uint8_t *fb) {
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, "重命名", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 646, 107, 25, "完成", EPD_DRAW_ALIGN_RIGHT, false);
    file_rule(fb, 36, 157, 612);
    ui_text(fb, 36, 202, 22, "文件名", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect field = {36, 242, 612, 82};
    ui_draw_round_rect(fb, field, 8, UI_GRAY_BLACK);
    char visible[128]; copy_utf8(visible, sizeof(visible), s_editor); fit_name(visible, 29, 470);
    ui_text_vc(fb, 53, 283, 29, visible[0] ? visible : " ", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(fb, 628, 283, 22, s_editor_ext, EPD_DRAW_ALIGN_RIGHT, false);
    ui_text(fb, 36, 348, 20, s_editor_chinese ? "拼音输入" : "英文输入", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 36, 380, 25, s_editor_pinyin[0] ? s_editor_pinyin : " ", EPD_DRAW_ALIGN_LEFT, false);
    for (int i = 0; i < 5; ++i) {
        EpdRect r = {36 + i * 112, 424, 106, 57};
        if (i == 0 && s_editor_candidate_count) ui_fill_round_rect(fb, r, 4, UI_GRAY_LIGHT);
        else ui_draw_round_rect(fb, r, 4, 0x70);
        if (i < (int)s_editor_candidate_count) {
            uint32_t cp = s_editor_candidates[i];
            char glyph[4] = {(char)(0xe0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 63)),
                             (char)(0x80 | (cp & 63)), 0};
            ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 30, glyph, EPD_DRAW_ALIGN_CENTER, false);
        }
    }
    ui_text_vc(fb, 634, 452, 27, "›", EPD_DRAW_ALIGN_CENTER, false);
    static const char *keys[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int row = 0; row < 3; ++row) {
        int len = strlen(keys[row]), left = row == 0 ? 36 : row == 1 ? 67 : 123;
        for (int col = 0; col < len; ++col) {
            EpdRect r = {left + col * 62, 517 + row * 74, 58, 61};
            ui_draw_round_rect(fb, r, 5, 0x70);
            char label[2] = {keys[row][col], 0};
            ui_text_vc(fb, r.x + 29, r.y + 30, 25, label, EPD_DRAW_ALIGN_CENTER, false);
        }
    }
    static const char *actions[] = {"中 / EN", "空格", "删除", "确定"};
    static const EpdRect buttons[] = {{36, 751, 102, 70}, {148, 751, 298, 70}, {456, 751, 98, 70}, {564, 751, 84, 70}};
    for (int i = 0; i < 4; ++i) ui_draw_button(fb, buttons[i], actions[i], i == 3);
    ui_text(fb, 36, 857, 21, s_editor_notice[0] ? s_editor_notice : "扩展名保持不变", EPD_DRAW_ALIGN_LEFT, false);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    if (s_view == FILE_VIEW_ACTIONS) {
        file_view_t restore = s_view;
        s_view = FILE_VIEW_LIST;
        render(ctx, fb);
        s_view = restore;
        render_actions(fb);
        return;
    }
    (void)ctx;
    ui_clear_page(fb);
    ui_nav_status(fb);
    if (s_view == FILE_VIEW_BOOT) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "固件升级", EPD_DRAW_ALIGN_CENTER, false);
        if (s_ota_view == OTA_VIEW_CONFIRM) {
            ui_fill_round_rect(fb, (EpdRect){36, 214, 612, 616}, 24, UI_GRAY_WHITE);
            file_card_border(fb, (EpdRect){36, 214, 612, 616}, 24);
            ui_text_vc(fb, 342, 275, 30, "确认安装本地升级包？", EPD_DRAW_ALIGN_CENTER, false);
            ui_text(fb, 70, 354, 20, "当前版本", EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 614, 350, 24, s_ota_info.current_version, EPD_DRAW_ALIGN_RIGHT, false);
            ui_text(fb, 70, 415, 20, "升级到", EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 614, 411, 24, s_ota_info.candidate_version, EPD_DRAW_ALIGN_RIGHT, false);
            file_rule(fb, 70, 472, 544);
            ui_text(fb, 70, 515, 21, "安装过程中请保持供电并保留 TF 卡。", EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 70, 558, 21, "校验失败时不会切换当前固件。", EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_button(fb, (EpdRect){60, 672, 265, 82}, "取消", false);
            ui_draw_button(fb, (EpdRect){359, 672, 265, 82}, "开始安装", true);
            ui_nav_draw(fb, 2);
            return;
        }
        if (s_ota_view == OTA_VIEW_INSTALLING || s_ota_view == OTA_VIEW_SUCCESS ||
            s_ota_view == OTA_VIEW_FAILED) {
            ui_fill_round_rect(fb, (EpdRect){36, 260, 612, 430}, 24, UI_GRAY_WHITE);
            file_card_border(fb, (EpdRect){36, 260, 612, 430}, 24);
            const char *title = s_ota_view == OTA_VIEW_INSTALLING ? "正在安装" :
                                s_ota_view == OTA_VIEW_SUCCESS ? "安装完成" : "安装失败";
            ui_text_vc(fb, 342, 336, 34, title, EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 342, 421, 23,
                       s_ota_view == OTA_VIEW_INSTALLING ? "正在写入空闲固件分区" : s_ota_result,
                       EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 342, 468, 21,
                       s_ota_view == OTA_VIEW_INSTALLING ? "请勿断电或取出 TF 卡" :
                       s_ota_view == OTA_VIEW_SUCCESS ? "Pico 将自动重新启动" : "当前版本仍可继续使用",
                       EPD_DRAW_ALIGN_CENTER, false);
            if (s_ota_view == OTA_VIEW_FAILED)
                ui_draw_button(fb, (EpdRect){60, 560, 564, 82}, "返回升级页", false);
            ui_nav_draw(fb, 2);
            return;
        }

        ui_fill_round_rect(fb, (EpdRect){36, 187, 612, 143}, 22, UI_GRAY_WHITE);
        file_card_border(fb, (EpdRect){36, 187, 612, 143}, 22);
        ui_text(fb, 60, 214, 20, "当前版本", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 254, 27, s_ota_info.current_version[0] ? s_ota_info.current_version : "未知",
                EPD_DRAW_ALIGN_LEFT, false);

        ui_fill_round_rect(fb, (EpdRect){36, 353, 612, 271}, 22, UI_GRAY_WHITE);
        file_card_border(fb, (EpdRect){36, 353, 612, 271}, 22);
        ui_text(fb, 60, 380, 27, s_ota_info.ready ? "发现本地升级包" : "TF 卡本地升级", EPD_DRAW_ALIGN_LEFT, false);
        if (s_ota_info.ready) {
            char detail[80];
            snprintf(detail, sizeof(detail), "%s · %.1f MB", s_ota_info.candidate_version,
                     s_ota_info.image_size / 1048576.0);
            ui_text(fb, 60, 426, 21, detail, EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 60, 466, 19, "Pico-update.bin", EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_button(fb, (EpdRect){60, 518, 564, 78}, "安装升级", true);
        } else {
            ui_text(fb, 60, 426, 20, s_ota_info.message, EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 60, 470, 19, "文件名必须为 Pico-update.bin", EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_button(fb, (EpdRect){60, 518, 564, 78}, "重新检查 TF 卡", false);
        }

        ui_text(fb, 42, 672, 21, "电脑刷机", EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_round_rect(fb, (EpdRect){36, 710, 612, 202}, 22, UI_GRAY_WHITE);
        file_card_border(fb, (EpdRect){36, 710, 612, 202}, 22);
        ui_text(fb, 60, 742, 26, "进入 BOOT 模式", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 786, 19, "用于首次安装 OTA 基础版或故障恢复。", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){60, 821, 564, 66},
                       s_boot_pending ? "正在进入 BOOT 模式" : "进入 BOOT 模式", false);
        ui_nav_draw(fb, 2);
        return;
    }
    if (s_view == FILE_VIEW_RENAME) {
        render_rename(fb);
        ui_nav_draw(fb, 2);
        return;
    }
    if (s_folder >= 0) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, s_view == FILE_VIEW_MOVE ? "选择目标文件夹" : names[s_folder], EPD_DRAW_ALIGN_CENTER, false);
        char path[144];
        snprintf(path, sizeof(path), "TF卡%s", folder_root(s_folder) + 7);
        ui_text(fb, 36, 172, 20, path, EPD_DRAW_ALIGN_LEFT, false);
        int folders = 0, files = 0;
        for (int i = 0; i < s_count; ++i) s_files[i].is_dir ? ++folders : ++files;
        char summary[48]; snprintf(summary, sizeof(summary), "%d个文件夹 · %d个文件", folders, files);
        ui_text(fb, 648, 172, 20, summary, EPD_DRAW_ALIGN_RIGHT, false);
        file_rule(fb, 36, 202, 612);
        if (s_message[0]) ui_text(fb, 36, 238, 27, s_message, EPD_DRAW_ALIGN_LEFT, false);
        else if (!s_count) ui_text(fb, 36, 238, 27, "此目录还没有文件", EPD_DRAW_ALIGN_LEFT, false);
        for (int row = 0; row < visible_rows(); ++row) {
            int index = s_page * visible_rows() + row;
            if (index >= s_count) break;
            int y = 214 + row * 91;
            char name[128]; snprintf(name, sizeof(name), "%s", s_files[index].name);
            while (*name && ttf_text_width_px(ui_text_effective_px(23), name) > 450) {
                size_t n = strlen(name) - 1;
                while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
                name[n] = 0;
            }
            file_icon(fb, 48, y + 20, s_files[index].is_dir);
            ui_text(fb, 101, y + 11, 23, name, EPD_DRAW_ALIGN_LEFT, false);
            char detail[32]; file_detail(detail, sizeof(detail), &s_files[index]);
            ui_text(fb, 101, y + 49, 19, detail, EPD_DRAW_ALIGN_LEFT, false);
            const char *tag = !strcmp(s_files[index].path, app_settings_books_dir()) ? "书籍目录" :
                              !strcmp(s_files[index].path, app_settings_fonts_dir()) ? "字体目录" : NULL;
            if (tag) ui_text(fb, 588, y + 35, 18, tag, EPD_DRAW_ALIGN_RIGHT, false);
            ui_text(fb, 640, y + 30, 22, "›", EPD_DRAW_ALIGN_RIGHT, false);
            file_rule(fb, 101, y + 85, 547);
        }
        if (s_folder == 3) {
            ui_draw_round_rect(fb, (EpdRect){36, 875, 293, 76}, 8, UI_GRAY_BLACK);
            ui_draw_round_rect(fb, (EpdRect){355, 875, 293, 76}, 8, UI_GRAY_BLACK);
            ui_text(fb, 182, 897, 24, s_view == FILE_VIEW_MOVE ? "取消移动" : "设为书籍目录", EPD_DRAW_ALIGN_CENTER, false);
            ui_text(fb, 501, 897, 24, s_view == FILE_VIEW_MOVE ? "移动到此处" : "设为字体目录", EPD_DRAW_ALIGN_CENTER, false);
            if (s_message[0]) ui_text(fb, 36, 995, 22, s_message, EPD_DRAW_ALIGN_LEFT, false);
        }
        char page[32]; snprintf(page, sizeof(page), "%d / %d", s_page + 1, s_count ? (s_count + visible_rows() - 1) / visible_rows() : 1);
        ui_text(fb, 648, 1057, 22, page, EPD_DRAW_ALIGN_RIGHT, false);
    } else {
        ui_text(fb, 36, 91, 52, "文件", EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_round_rect(fb, (EpdRect){530, 91, 118, 59}, 27, UI_GRAY_WHITE);
        ui_draw_round_rect(fb, (EpdRect){530, 91, 118, 59}, 27, 0x68);
        ui_text_vc(fb, 589, 120, 21, "升级", EPD_DRAW_ALIGN_CENTER, false);
        EpdRect storage = {36, 171, 612, 112};
        ui_fill_round_rect(fb, storage, 24, UI_GRAY_WHITE);
        file_card_border(fb, storage, 24);
        ui_text(fb, 58, 188, 27, "存储卡", EPD_DRAW_ALIGN_LEFT, false);
        char total[32], usage[80];
        if (s_sd.mounted) {
            snprintf(total, sizeof(total), "%.1f GB", s_sd.capacity_bytes / 1073741824.0);
            snprintf(usage, sizeof(usage), "已用 %.1f GB · 可用 %.1f GB",
                     (s_sd.capacity_bytes - s_sd.free_bytes) / 1073741824.0,
                     s_sd.free_bytes / 1073741824.0);
        } else {
            snprintf(total, sizeof(total), "未挂载");
            snprintf(usage, sizeof(usage), "请检查 TF 卡");
        }
        ui_text(fb, 624, 191, 21, total, EPD_DRAW_ALIGN_RIGHT, false);
        ui_text(fb, 58, 231, 19, usage, EPD_DRAW_ALIGN_LEFT, false);
        EpdRect capacity = {58, 263, 566, 6};
        ui_fill_round_rect(fb, capacity, 3, 0xa0);
        if (s_sd.mounted && s_sd.capacity_bytes) {
            capacity.width = (int)((s_sd.capacity_bytes - s_sd.free_bytes) * 566 / s_sd.capacity_bytes);
            if (capacity.width > 0) ui_fill_round_rect(fb, capacity, 3, 0x68);
        }
        ui_text(fb, 42, 308, 21, "传输文件", EPD_DRAW_ALIGN_LEFT, false);
        static const char *const methods[] = {"WiFi 传书", "热点传书", "USB 读卡"};
        static const char *const details[] = {"同一网络", "Pico 热点", "连接电脑"};
        for (int i = 0; i < 3; ++i) {
            EpdRect card = home_transfer_rect(i);
            ui_fill_round_rect(fb, card, 20, UI_GRAY_WHITE);
            ui_draw_round_rect(fb, card, 20, 0x60);
            ui_text_vc(fb, card.x + card.width / 2, card.y + 35, 23, methods[i],
                       EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, card.x + card.width / 2, card.y + 75, 19, details[i],
                       EPD_DRAW_ALIGN_CENTER, false);
        }
        char summary[48]; snprintf(summary, sizeof(summary), "根目录 · %d 项", s_count);
        ui_text(fb, 36, 482, 21, summary, EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_round_rect(fb, (EpdRect){537, 458, 111, 50}, 23, UI_GRAY_WHITE);
        ui_draw_round_rect(fb, (EpdRect){537, 458, 111, 50}, 23, 0x70);
        ui_text_vc(fb, 592, 483, 19, "名称 ↑", EPD_DRAW_ALIGN_CENTER, false);
        int first = s_page * root_rows();
        int rows = s_count - first;
        if (rows > root_rows()) rows = root_rows();
        if (rows < 0) rows = 0;
        EpdRect list = {36, 520, 612, root_rows() * 91};
        ui_fill_round_rect(fb, list, 24, UI_GRAY_WHITE);
        file_card_border(fb, list, 24);
        if (!s_count) ui_text_vc(fb, 342, 565, 22, s_message[0] ? s_message : "根目录为空", EPD_DRAW_ALIGN_CENTER, false);
        for (int row = 0; row < root_rows(); ++row) {
            int index = first + row;
            if (index >= s_count) break;
            int y = 520 + row * 91;
            if (row) file_rule(fb, 96, y, 532);
            char name[128]; copy_utf8(name, sizeof(name), s_files[index].name); fit_name(name, 23, 430);
            file_icon(fb, 52, y + 24, s_files[index].is_dir);
            if (s_files[index].is_dir)
                ui_text_vc(fb, 101, y + 46, 23, name, EPD_DRAW_ALIGN_LEFT, false);
            else
                ui_text(fb, 101, y + 20, 23, name, EPD_DRAW_ALIGN_LEFT, false);
            char detail[32]; file_detail(detail, sizeof(detail), &s_files[index]);
            if (s_files[index].is_dir) ui_text_vc(fb, 628, y + 46, 24, "›", EPD_DRAW_ALIGN_RIGHT, false);
            else {
                ui_text(fb, 101, y + 55, 19, detail, EPD_DRAW_ALIGN_LEFT, false);
                ui_text_vc(fb, 624, y + 44, 22, "›", EPD_DRAW_ALIGN_RIGHT, false);
            }
        }
        if (s_count > root_rows()) {
            char page[24]; snprintf(page, sizeof(page), "%d / %d", s_page + 1,
                                     (s_count + root_rows() - 1) / root_rows());
            ui_text_vc(fb, 342, 1030, 20, page, EPD_DRAW_ALIGN_CENTER, false);
        }
    }
    ui_nav_draw(fb, 2);
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_folder = s_requested_folder; s_requested_folder = -1;
    s_view = FILE_VIEW_LIST;
    s_boot_pending = false;
    s_ota_view = OTA_VIEW_IDLE;
    s_ota_work_pending = false;
    s_ota_restart_pending = false;
    memset(&s_ota_info, 0, sizeof(s_ota_info));
    s_ota_result[0] = 0;
    s_delete_confirm = false;
    snprintf(s_dir, sizeof(s_dir), "/sdcard");
    s_page = 0; s_scan_pending = true;
    read_pico_sd_start_probe();
    read_pico_sd_get_info(&s_sd);
    for (int i = 0; i < 3; ++i) s_counts[i] = count_root(i);
    if (s_folder >= 0) scan_folder(s_folder);
    else scan_folder(3);
}
static void on_media_lost(app_ctx_t *ctx) {
    (void)ctx; s_folder = -1; s_count = 0; s_scan_pending = false;
    s_view = FILE_VIEW_LIST; s_delete_confirm = false;
    s_ota_view = OTA_VIEW_IDLE; s_ota_work_pending = false; s_ota_restart_pending = false;
    memset(&s_ota_info, 0, sizeof(s_ota_info));
    memset(s_counts, 0, sizeof(s_counts)); read_pico_sd_get_info(&s_sd);
}
static void on_media_ready(app_ctx_t *ctx) {
    (void)ctx;
    s_scan_pending = true;
}
static app_redraw_t on_tick(app_ctx_t *ctx) {
    (void)ctx;
    if (s_ota_restart_pending) {
        s_ota_restart_pending = false;
        esp_restart();
        return APP_REDRAW_NONE;
    }
    if (s_ota_work_pending) {
        s_ota_work_pending = false;
        esp_err_t err = pico_ota_install(PICO_OTA_UPDATE_PATH, s_ota_result, sizeof(s_ota_result));
        if (err == ESP_OK) {
            s_ota_view = OTA_VIEW_SUCCESS;
            s_ota_restart_pending = true;
        } else {
            ESP_LOGE("files", "local OTA failed: %s", esp_err_to_name(err));
            s_ota_view = OTA_VIEW_FAILED;
        }
        return APP_REDRAW_PAGE;
    }
    if (s_boot_pending) {
        s_boot_pending = false;
        pmu_selftest_prepare_powerdown();
        uint8_t req[2] = {0, 0};
        esp_err_t err = read_pico_pmu_cmd(PMU_CMD_HOST_REQUEST_RESET, req, sizeof(req));
        if (err != ESP_OK) ESP_LOGW("files", "BOOT PMU notice: %s", esp_err_to_name(err));
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
        return APP_REDRAW_NONE;
    }
    if (!s_scan_pending) return APP_REDRAW_NONE;
    if (read_pico_sd_get_info(&s_sd) == ESP_ERR_NOT_FINISHED) return APP_REDRAW_NONE;
    s_scan_pending = false;
    for (int i = 0; i < 3; ++i) s_counts[i] = count_root(i);
    if (s_folder >= 0) scan_folder(s_folder);
    else scan_folder(3);
    return APP_REDRAW_PAGE;
}

static void open_selected(app_ctx_t *ctx) {
    const char *ext = strrchr(s_selected.name, '.');
    if (!ext) { snprintf(s_message, sizeof(s_message), "没有可用的打开方式"); return; }
    if (!strcasecmp(ext, ".epub") || !strcasecmp(ext, ".txt")) {
        extern const app_desc_t app_book;
        if (app_book_request_open(s_selected.path)) ctx->request_app = &app_book;
    } else if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png")) {
        extern const app_desc_t app_image;
        if (app_image_request_open(s_selected.path)) ctx->request_app = &app_image;
    } else if (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf")) {
        if (ttf_font_open(s_selected.path) == ESP_OK) {
            app_settings_set_system_font_path(s_selected.path);
            app_font_activate_system();
            snprintf(s_message, sizeof(s_message), "已设为系统字体");
        } else snprintf(s_message, sizeof(s_message), "字体无法加载");
    } else snprintf(s_message, sizeof(s_message), "没有可用的打开方式");
}

static void delete_selected(void) {
    char old_path[288]; copy_utf8(old_path, sizeof(old_path), s_selected.path);
    esp_err_t err = file_tree_delete(old_path, file_deleted, NULL);
    if (err != ESP_OK) {
        s_delete_confirm = false;
        repair_deleted_directories();
        book_store_notify_changed();
        rescan_current();
        snprintf(s_message, sizeof(s_message), "删除未完成，请检查 TF 卡");
        return;
    }
    repair_deleted_directories();
    book_store_notify_changed();
    s_view = FILE_VIEW_LIST; s_delete_confirm = false;
    rescan_current();
    snprintf(s_message, sizeof(s_message), s_selected.is_dir ? "文件夹已删除" : "文件已删除");
}

static app_redraw_t save_rename(void) {
    if (s_editor_pinyin[0]) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字");
        return APP_REDRAW_PAGE;
    }
    char *start = s_editor;
    while (*start == ' ') ++start;
    if (start != s_editor) memmove(s_editor, start, strlen(start) + 1);
    size_t len = strlen(s_editor);
    while (len && (s_editor[len - 1] == ' ' || s_editor[len - 1] == '.')) s_editor[--len] = 0;
    if (!len || strchr(s_editor, '/') || strchr(s_editor, '\\') || strchr(s_editor, ':')) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "请输入有效文件名");
        return APP_REDRAW_PAGE;
    }
    char name[128], destination[288];
    if (snprintf(name, sizeof(name), "%s%s", s_editor, s_editor_ext) >= sizeof(name) ||
        !sibling_path(name, destination)) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "文件名过长");
        return APP_REDRAW_PAGE;
    }
    if (!strcmp(destination, s_selected.path)) { s_view = FILE_VIEW_ACTIONS; return APP_REDRAW_PAGE; }
    struct stat existing;
    if (stat(destination, &existing) == 0) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "同名文件已经存在");
        return APP_REDRAW_PAGE;
    }
    char old_path[288]; copy_utf8(old_path, sizeof(old_path), s_selected.path);
    if (!moved_settings_fit(old_path, destination)) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "设置路径过长");
        return APP_REDRAW_PAGE;
    }
    if (file_tree_move(old_path, destination) != ESP_OK) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "重命名失败，请检查 TF 卡");
        return APP_REDRAW_PAGE;
    }
    sync_moved_settings(old_path, destination);
    notify_moved_tree(old_path, destination, 0);
    book_store_notify_changed();
    copy_utf8(s_selected.path, sizeof(s_selected.path), destination);
    copy_utf8(s_selected.name, sizeof(s_selected.name), name);
    s_view = FILE_VIEW_ACTIONS;
    snprintf(s_message, sizeof(s_message), "重命名完成");
    return APP_REDRAW_PAGE;
}

static app_redraw_t action_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (ev->y0 < 430 || (ev->x0 >= 596 && ev->y0 < 510) || ev->y0 >= 918) {
        s_view = FILE_VIEW_LIST; s_delete_confirm = false; rescan_current(); return APP_REDRAW_PAGE;
    }
    if (s_delete_confirm) {
        if (ev->y0 >= 788 && ev->y0 < 864) {
            if (ev->x0 < 342) s_delete_confirm = false;
            else delete_selected();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (ev->y0 >= 648 && ev->y0 < 870) {
        int action = (ev->y0 >= 770 ? 2 : 0) + (ev->x0 >= 352 ? 1 : 0);
        if (action == 0) editor_start();
        else if (action == 1) {
            char copied[128] = {0};
            esp_err_t err = copy_selected_item(copied);
            if (err == ESP_OK) snprintf(s_message, sizeof(s_message), "已复制：%.74s", copied);
            else snprintf(s_message, sizeof(s_message), err == ESP_ERR_NO_MEM ? "内存不足，复制失败" : "复制失败，请检查空间");
        } else if (action == 2) start_move();
        else { s_delete_confirm = true; s_message[0] = 0; }
        return ctx->request_app ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t rename_gesture(const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_SWIPE_L && s_editor_pinyin[0]) {
        ++s_editor_candidate_page; editor_refresh(); return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_SWIPE_R && s_editor_candidate_page) {
        --s_editor_candidate_page; editor_refresh(); return APP_REDRAW_PAGE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (y < 157) {
        if (x > 510) return save_rename();
        s_view = FILE_VIEW_ACTIONS; return APP_REDRAW_PAGE;
    }
    if (y >= 424 && y < 481) {
        if (x >= 604) { ++s_editor_candidate_page; editor_refresh(); return APP_REDRAW_PAGE; }
        int index = (x - 36) / 112;
        if (x >= 36 && index >= 0 && index < 5) editor_commit_candidate(index);
        return APP_REDRAW_PAGE;
    }
    static const char *keys[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int row = 0; row < 3; ++row) {
        int left = row == 0 ? 36 : row == 1 ? 67 : 123, top = 517 + row * 74;
        if (y < top || y >= top + 61 || x < left) continue;
        int col = (x - left) / 62;
        if (col < 0 || col >= (int)strlen(keys[row]) || x >= left + col * 62 + 58) continue;
        char letter = keys[row][col];
        if (s_editor_chinese) {
            size_t n = strlen(s_editor_pinyin);
            if (n + 1 < sizeof(s_editor_pinyin)) {
                s_editor_pinyin[n] = (char)(letter + ('a' - 'A'));
                s_editor_pinyin[n + 1] = 0; s_editor_candidate_page = 0; editor_refresh();
            }
        } else { char value[2] = {letter, 0}; editor_append(value); }
        return APP_REDRAW_PAGE;
    }
    if (y >= 751 && y < 821) {
        if (x >= 36 && x < 138) {
            if (s_editor_pinyin[0]) snprintf(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字");
            else s_editor_chinese = !s_editor_chinese;
        } else if (x >= 148 && x < 446) {
            if (s_editor_pinyin[0] && s_editor_candidate_count) editor_commit_candidate(0);
            else if (s_editor_pinyin[0]) { editor_append(s_editor_pinyin); s_editor_pinyin[0] = 0; editor_refresh(); }
            else editor_append(" ");
        } else if (x >= 456 && x < 554) editor_backspace();
        else if (x >= 564) return save_rename();
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_view == FILE_VIEW_BOOT) {
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (s_ota_view == OTA_VIEW_INSTALLING || s_ota_view == OTA_VIEW_SUCCESS) return APP_REDRAW_NONE;
        if (s_ota_view == OTA_VIEW_CONFIRM) {
            if (ev->y0 >= 672 && ev->y0 < 754) {
                if (ev->x0 < 342) s_ota_view = OTA_VIEW_IDLE;
                else {
                    s_ota_view = OTA_VIEW_INSTALLING;
                    s_ota_work_pending = true;
                }
                return APP_REDRAW_PAGE;
            }
            if (ev->y0 < 180) { s_ota_view = OTA_VIEW_IDLE; return APP_REDRAW_PAGE; }
            return APP_REDRAW_NONE;
        }
        if (s_ota_view == OTA_VIEW_FAILED) {
            if ((ev->y0 >= 560 && ev->y0 < 642) || ev->y0 < 180) {
                ota_refresh_info();
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        if (ev->y0 < 180) { s_view = FILE_VIEW_LIST; return APP_REDRAW_PAGE; }
        if (ev->y0 >= 518 && ev->y0 < 596) {
            if (s_ota_info.ready) s_ota_view = OTA_VIEW_CONFIRM;
            else ota_refresh_info();
            return APP_REDRAW_PAGE;
        }
        if (ev->y0 >= 821 && ev->y0 < 887) { s_boot_pending = true; return APP_REDRAW_PAGE; }
        int tab = ui_nav_hit(ev->x0, ev->y0);
        if (tab >= 0) ui_nav_request(ctx, tab);
        return APP_REDRAW_NONE;
    }
    if (s_view == FILE_VIEW_ACTIONS) return action_gesture(ctx, ev);
    if (s_view == FILE_VIEW_RENAME) return rename_gesture(ev);
    if (s_view == FILE_VIEW_MOVE) {
        if (ev->type == UI_GESTURE_SWIPE_L && (s_page + 1) * visible_rows() < s_count) {
            ++s_page; return APP_REDRAW_PAGE;
        }
        if (ev->type == UI_GESTURE_SWIPE_R && s_page > 0) { --s_page; return APP_REDRAW_PAGE; }
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (ev->y0 < 185) {
            if (strcmp(s_dir, "/sdcard")) {
                char *slash = strrchr(s_dir, '/'); if (slash) *slash = 0;
                scan_folder(3);
            } else leave_move(false);
            return APP_REDRAW_PAGE;
        }
        if (ev->y0 >= 875 && ev->y0 < 951) {
            if (ev->x0 < 342) leave_move(false);
            else return move_selected();
            return APP_REDRAW_PAGE;
        }
        int row = (ev->y0 - 214) / 91;
        int index = s_page * visible_rows() + row;
        if (ev->y0 >= 214 && row >= 0 && row < visible_rows() && index < s_count) {
            copy_utf8(s_dir, sizeof(s_dir), s_files[index].path);
            scan_folder(3);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_SWIPE_L && s_folder >= 0 && (s_page + 1) * visible_rows() < s_count) { ++s_page; return APP_REDRAW_PAGE; }
    if (ev->type == UI_GESTURE_SWIPE_R && s_folder >= 0 && s_page > 0) { --s_page; return APP_REDRAW_PAGE; }
    if (ev->type == UI_GESTURE_SWIPE_L && s_folder < 0 && (s_page + 1) * root_rows() < s_count) { ++s_page; return APP_REDRAW_PAGE; }
    if (ev->type == UI_GESTURE_SWIPE_R && s_folder < 0 && s_page > 0) { --s_page; return APP_REDRAW_PAGE; }
    if (ev->type != UI_GESTURE_TAP && ev->type != UI_GESTURE_LONG_PRESS) return APP_REDRAW_NONE;
    bool long_press = ev->type == UI_GESTURE_LONG_PRESS;
    if (long_press && ev->y0 < (s_folder >= 0 ? 214 : 520)) return APP_REDRAW_NONE;
    int tab = ui_nav_hit(ev->x0, ev->y0);
    if (!long_press && tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    if (s_folder >= 0) {
        if (!long_press && ev->y0 < 185) {
            if (s_folder == 3 && strcmp(s_dir, "/sdcard")) {
                char *slash = strrchr(s_dir, '/'); if (slash) *slash = 0;
                scan_folder(3);
            } else s_folder = -1;
            return APP_REDRAW_PAGE;
        }
        if (!long_press && s_folder == 3 && ev->y0 >= 875 && ev->y0 < 951) {
            struct stat selected;
            if (!strcmp(s_dir, "/sdcard") || stat(s_dir, &selected) || !S_ISDIR(selected.st_mode))
                snprintf(s_message, sizeof(s_message), "请先进入 TF 卡内的文件夹");
            else if (ev->x0 < 342) {
                if (app_settings_set_books_dir(s_dir)) {
                    book_store_notify_changed();
                    snprintf(s_message, sizeof(s_message), "已设为书籍目录");
                } else snprintf(s_message, sizeof(s_message), "目录路径过长，无法设置");
            } else {
                if (app_settings_set_fonts_dir(s_dir)) {
                    ttf_font_scan();
                    snprintf(s_message, sizeof(s_message), "已设为字体目录");
                } else snprintf(s_message, sizeof(s_message), "目录路径过长，无法设置");
            }
            s_counts[0] = count_root(0); s_counts[2] = count_root(2);
            return APP_REDRAW_PAGE;
        }
        int row = (int)(ev->y0 - 214) / 91;
        int index = s_page * visible_rows() + row;
        if (ev->y0 >= 214 && row >= 0 && row < visible_rows() && index < s_count) {
            if (long_press) {
                s_selected = s_files[index]; s_view = FILE_VIEW_ACTIONS;
                s_delete_confirm = false; s_message[0] = 0;
                return APP_REDRAW_PAGE;
            }
            if (s_folder == 3 && s_files[index].is_dir) {
                if (strlen(s_files[index].path) >= sizeof(s_dir)) {
                    snprintf(s_message, sizeof(s_message), "目录路径过长"); return APP_REDRAW_PAGE;
                }
                snprintf(s_dir, sizeof(s_dir), "%s", s_files[index].path);
                scan_folder(3);
                return APP_REDRAW_PAGE;
            }
            s_selected = s_files[index];
            open_selected(ctx);
            return ctx->request_app ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (!long_press && ev->y0 >= 91 && ev->y0 < 150 && ev->x0 >= 530) {
        s_view = FILE_VIEW_BOOT;
        ota_refresh_info();
        return APP_REDRAW_PAGE;
    }
    for (int i = 0; !long_press && i < 3; ++i) if (ui_rect_hit(home_transfer_rect(i), ev->x0, ev->y0)) {
        if (i == 0) app_transfer_request_wifi_upload();
        else if (i == 1) app_transfer_request_hotspot_start();
        else app_transfer_request_usb_start();
        extern const app_desc_t app_transfer;
        ctx->request_app = &app_transfer;
        return APP_REDRAW_NONE;
    }
    if (ev->y0 >= 520 && ev->y0 < 975) {
        int row = (ev->y0 - 520) / 91;
        int index = s_page * root_rows() + row;
        if (row >= 0 && row < root_rows() && index < s_count) {
            if (long_press) {
                s_selected = s_files[index]; s_view = FILE_VIEW_ACTIONS;
                s_delete_confirm = false; s_message[0] = 0;
                return APP_REDRAW_PAGE;
            }
            if (s_files[index].is_dir) {
                s_folder = 3;
                copy_utf8(s_dir, sizeof(s_dir), s_files[index].path);
                scan_folder(3);
                return APP_REDRAW_PAGE;
            }
            s_selected = s_files[index];
            open_selected(ctx);
            return ctx->request_app ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (s_view == FILE_VIEW_BOOT) {
        if (s_ota_view == OTA_VIEW_INSTALLING || s_ota_view == OTA_VIEW_SUCCESS) return APP_REDRAW_NONE;
        if (s_ota_view != OTA_VIEW_IDLE) {
            ota_refresh_info();
            return APP_REDRAW_PAGE;
        }
        s_view = FILE_VIEW_LIST;
        return APP_REDRAW_PAGE;
    }
    if (s_view == FILE_VIEW_MOVE) { leave_move(false); return APP_REDRAW_PAGE; }
    if (key == 1) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_view == FILE_VIEW_RENAME) { s_view = FILE_VIEW_ACTIONS; return APP_REDRAW_PAGE; }
    if (s_view == FILE_VIEW_ACTIONS) {
        s_view = FILE_VIEW_LIST; s_delete_confirm = false; rescan_current(); return APP_REDRAW_PAGE;
    }
    if (s_folder >= 0) {
        if (s_folder == 3 && strcmp(s_dir, "/sdcard")) {
            char *slash = strrchr(s_dir, '/'); if (slash) *slash = 0;
            scan_folder(3);
        } else s_folder = -1;
        return APP_REDRAW_PAGE;
    }
    ui_nav_request(ctx, 0);
    return APP_REDRAW_NONE;
}
static bool no_menu_handle(app_ctx_t *ctx) { (void)ctx; return false; }

const app_desc_t app_files = {
    .title = "文件管理 Files", .detail = "TF 卡文件与传输", .enter_full = false,
    .owns_keys = true, .menu_handle_enabled = no_menu_handle,
    .on_enter = on_enter, .on_media_lost = on_media_lost, .on_media_ready = on_media_ready,
    .render = render, .on_tick = on_tick,
    .on_gesture = on_gesture, .on_key = on_key,
};
