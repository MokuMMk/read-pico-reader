/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 图书书架、阅读、目录与页内进度；文件解析和排版由book模块负责。
 * Book shelf, reader, TOC and progress; book modules own parsing and pagination.
 *
 * 冻结：Phase4b统一手势入口并接管三键为上页/工具条/下页；工具条保留强刷。屏幕翻页在抬起提交，不画按下态。
 * 晃动实验默认关，只翻下一页；离页关闭AOI2并休眠。render只绘图。
 * 预渲染回调返回前收齐，避免菜单/锁屏绕过页内TTF锁。
 * 普通翻页只刷新正文与页脚；手动或周期清残影整屏全刷。
 * 用户修订：中键短按打开/关闭阅读设置，轻触正文中间切换全屏；长按中键返回来源，阅读页不画右下角菜单图标。
 * 用户授权基础管理：长按书架先看完整详情，清进度与删文件分别确认；失败保留待重试记录，不自动回收其他书进度。
 * 用户修订：单本管理为书架弹窗；管理页用于批量操作。分页和排序保留勾选，筛选/应用搜索及重扫清除勾选。
 * 失败进度仅按变更路径失效；删除后的清理重试保留到本次开机结束，不随切页释放。
 * 卡失效时先保存进度并关闭阅读资源，再由主循环回退字体；禁止自动续读失效挂载。
 * 用户最新修订：书架每页九本，仅显示导入书籍；底栏保留首页/书架/文件/设置，设置直接进入设置页。
 * 用户修订：长按图书可用本机拼音输入编辑书名；阅读时长与翻页真实记录，供票根锁屏使用。
 * 用户最新修订：书名编辑可点选插入位置并用左右键微调，支持在文字中间插入和删除。
 * 用户修订：首页、书架、文件、设置四栏导航；切换界面采用 GL16，章节首页单独排标题。
 * 用户修订：EPUB 章节首页以书内目录标题为准，正文题头仅在相同时折叠，避免引言误判。
 * 用户修订：阅读进度条无外伸刻度并使用圆角；继续阅读区显示最近书籍封面。
 * 用户修订：书架只在封面下显示书名；首次打开时先显示书架，再逐本生成封面缓存。
 * 用户修订：字号和间距重排只保存最终进度，字号使用现有闲置灰阶整理。
 * 用户修订：阅读设置滑杆可拖动并在松手后重排；字体选择可纵向翻页；统计入口显示真实明细与近30天数据。
 * 用户修订：默认翻页保持原刷新规则；刷新设置可选真实错相 GL16 水波纹，阅读字体面板保持紧凑。
 * 用户修订：目录由独立模块整页绘制与命中；目录标题清理换行并限制为单行，翻页不再沿用书架的局部刷新。
 * Frozen: Phase4b uses the shared gesture entry and owns previous/tools/next keys; the toolbar keeps full refresh. Screen turns commit on release without pressed decoration.
 * Shake is experimental, off by default, forward only; exit disables AOI2 and sleeps it. Render only paints.
 * Join preparation before returning callbacks so menus/lock cannot race the page-local TTF lock.
 * Ordinary turns refresh only body and footer; manual and periodic ghost cleanup refresh the entire screen.
 * User revision: the middle key toggles page settings on short release; a center body tap toggles full screen; a middle-key hold returns to the opening source. The reader has no bottom-right menu icon.
 * User-authorized management shows full details before separate clear/delete confirmations; retain failed saves for retry without pruning other books.
 * User revision: single-book actions use a shelf dialog; full management is for batches. Paging/sorting preserve selection; filtering/applied search and rescanning clear it.
 * Invalidate failed progress only for changed paths; retain deletion cleanup retries across page exits for this boot.
 * Lost media saves progress and closes reader resources before global font fallback; never auto-resume an invalid mount.
 * Latest user revision: shelf pages contain at most nine imported books; the fourth tab opens Settings directly.
 * User revision: book details lead to an on-device Pinyin title editor; measured reading time and turns feed the ticket lock face.
 * Latest user revision: the title editor can place and move an insertion caret for edits in the middle of text.
 * User revision: home, shelf, files and settings have four-tab navigation; view changes use GL16 and chapter starts have a title lead.
 * User revision: EPUB chapter leads use navigation titles; body headings are folded only when matching, preventing front matter from being mislabeled.
 * User revision: the reader bar is rounded without protruding ticks; continue reading displays the latest cover.
 * User revision: the shelf shows titles without author rows; first visits paint the shelf before filling cached covers.
 * User revision: size and spacing reflow saves final progress only; size uses the existing idle grayscale settle.
 * User revision: reader sliders drag and reflow on release; font selection pages vertically; statistics entries show real details and recent-30-day data.
 * User revision: default turns keep their refresh policy; Refresh Settings may enable staggered GL16 water turns, while the font sheet stays compact.
 * User revision: a standalone module owns full-page TOC rendering and hit testing; normalized single-line titles cannot leak into another row.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "app_content_open.h"
#include "app_registry.h"
#include "book_layout.h"
#include "book_cover.h"
#include "book_epub.h"
#include "book_policy.h"
#include "book_progress.h"
#include "book_title.h"
#include "book_ticket.h"
#include "book_toc.h"
#include "app_font_context.h"
#include "book_source.h"
#include "book_store.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "read_pico_init.h"
#include "read_pico_sd.h"
#include "read_pico_search.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_image_dither.h"
#include "ui_gesture.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "assets/reader_refresh_icon.h"

#define BOOK_ROWS 9
#define BOOK_BULK_ROWS 6
#define BOOK_PX_MIN 36
#define BOOK_PX_MAX 72
#define BOOK_PX_STEP 4
#define BOOK_SHAKE_THS_MG 192
#define BOOK_SHAKE_DUR 2
#define BOOK_TOOL_COUNT 5
#define BOOK_FONT_PAGE 9
#define BOOKMARK_MAX 24
#define BOOKMARK_ROWS 6

typedef enum { SHELF, READING, TOC, MANAGE, BULK, SEARCH, EDIT } book_view_t;
typedef enum {
    READER_PANEL_NONE,
    READER_PANEL_TOOLS,
    READER_PANEL_FONT_SETTINGS,
    READER_PANEL_LAYOUT_SETTINGS,
    READER_PANEL_FONT_PICKER,
    READER_PANEL_STATS,
    READER_PANEL_BOOKMARKS,
    READER_PANEL_STATS_RECENT,
    READER_PANEL_REFRESH_SETTINGS,
} reader_panel_t;
typedef struct {
    char name[256];
    char author[128];
    char path[BOOK_STORE_PATH_MAX];
    uint32_t size;
    uint16_t chapter;
    bool is_flash;
    bool has_progress;
    uint8_t pct;
    uint32_t recent;
    bool selected, removed, search_match;
} shelf_entry_t;

static const char* TAG = "book";
static book_view_t s_view;
static int s_presented_view = -1;
static char s_requested_open[BOOK_STORE_PATH_MAX];
static bool s_requested_open_home;
static bool s_reader_return_home;
static bool s_requested_manage;
static shelf_entry_t* s_shelf;
static struct { int index; uint8_t *gray; } s_covers[BOOK_ROWS];
static unsigned s_cover_pending_mask;
static size_t s_shelf_capacity;
static int s_count, s_visible_count, s_filter;
static bool s_recent_sort;
static shelf_entry_t s_managed;
static bool s_delete_confirm, s_file_removed;
static char s_shelf_warning[128], s_manage_message[128];
static char s_query[65], s_search_draft[65], s_batch_message[128];
static book_view_t s_search_parent;
static bool s_batch_confirm, s_batch_delete;
typedef struct pending_progress {
    char path[BOOK_STORE_PATH_MAX];
    book_progress_t value;
    bool dirty, progress_saved;
    book_progress_watch_t* watch;
    struct pending_progress* next;
} pending_progress_t;
static pending_progress_t* s_pending;
typedef struct delete_retry {
    shelf_entry_t entry;
    struct delete_retry* next;
} delete_retry_t;
static delete_retry_t* s_delete_retries;
static char s_latest_path[BOOK_STORE_PATH_MAX];
static char s_editor_title[121], s_editor_pinyin[9], s_editor_notice[96];
static size_t s_editor_cursor;
static bool s_editor_chinese;
static size_t s_editor_candidate_page;
static uint32_t s_editor_candidates[5];
static size_t s_editor_candidate_count;
static uint8_t* s_editor_cover;
static bool s_save_failed;
static bool s_pending_invalidated;
static int64_t s_save_retry_ms;
static unsigned s_store_revision;
static bool s_shelf_cache_valid, s_cache_sd_present, s_cache_sd_mounted;
static bool s_scan_pending, s_clear_confirm;
static reader_panel_t s_reader_panel;
static bool s_reader_fullscreen;
static char s_message[128], s_storage[128], s_path[BOOK_STORE_PATH_MAX], s_title[128];
static char s_book_title[128];
static char s_chapter_heading_title[128], s_chapter_heading_label[32];
static size_t s_chapter_lead_skip;
static unsigned s_chapter_lead_height;
static char s_font_path[192];
static char* s_text;
bool app_book_reader_body_visible(void) {
    return s_view == READING && s_text && s_reader_panel == READER_PANEL_NONE && !s_clear_confirm;
}
static blk_t* s_blocks;
static char** s_images;
static size_t s_image_count;
static uint8_t* s_inline_gray;
static unsigned s_inline_w, s_inline_h;
static int s_inline_index = -1;
static size_t s_block_count;
static size_t s_text_len, s_chapter, s_page;
static size_t s_selected_toc = SIZE_MAX;
static size_t s_jump_offset = SIZE_MAX, s_jump_page;
static uint32_t s_file_size;
static int s_px, s_margin, s_line_spacing, s_turns, s_unsaved;
static int64_t s_poll_ms, s_last_turn_ms, s_sensor_ms;
static int64_t s_stats_last_ms, s_stats_activity_ms;
static uint32_t s_stats_pending_ms, s_stats_pending_turns;
static uint32_t s_session_read_ms, s_session_turns;
static int s_font_page;
static int s_reader_slider = -1;
static bool s_reader_slider_endpoint;
static int s_reader_preview_px, s_reader_preview_margin, s_reader_preview_line, s_reader_preview_para;
static int s_reader_preview_tracking;
static uint32_t s_recent_days[30];
static bool s_recent_days_valid;
static char s_reader_notice[64];
static int64_t s_reader_notice_until;
static int s_bookmark_page;
static bool s_shake_enabled, s_sensor_on;
static bool s_sensor_saved;
static sc7a20h_sensor_config_t s_sensor_config;
static book_shake_gate_t s_shake;
static EpdRect s_area;
static enum EpdDrawMode s_mode = MODE_GL16;
static bool s_reader_cleanup;
static bool s_reader_split;
static bool s_water_turn_pending;
static e0470_turn_dir_t s_water_turn_dir;
static int s_pressed_control = -1;
static int64_t s_du_ms;
static unsigned s_du_count;
static EpdRect s_du_area;
static SemaphoreHandle_t s_draw_lock, s_prep_done;
static TaskHandle_t s_prep_task;
static uint8_t* s_next_fb;
static int s_next_page = -1, s_prep_page = -1;

#define s_toolbar (s_reader_panel != READER_PANEL_NONE)

static void render(app_ctx_t* ctx, uint8_t* fb);
static void draw_reader_panel(uint8_t* fb);
static EpdRect reader_slider_rect(int slider);
static void scan_shelf(app_ctx_t* ctx);
static void copy_text(char* dst, size_t cap, const char* src);
static void fit_text(char* text, int px, int width);
static void free_book(void);
static void save_progress(void);
static void invalidate_prep(void);
static void prepare_inline_image(void);
static void sort_shelf(app_ctx_t* ctx);
static void prepare_covers(app_ctx_t* ctx);
bool app_book_request_open(const char* path) {
    if (!path || !path[0] || strnlen(path, sizeof(s_requested_open)) >= sizeof(s_requested_open)) return false;
    copy_text(s_requested_open, sizeof(s_requested_open), path);
    s_requested_open_home = false;
    return true;
}
bool app_book_request_open_from_home(const char* path) {
    if (!app_book_request_open(path)) return false;
    s_requested_open_home = true;
    return true;
}
void app_book_request_manage(void) { s_requested_manage = true; }
static void draw_control(uint8_t* fb, EpdRect rect, const char* label, int id) {
    if (id == 112 && s_query[0]) {
        ui_fill_round_rect(fb, rect, UI_BTN_RADIUS, UI_GRAY_BLACK);
        ui_text_vc(fb, rect.x + rect.width / 2, rect.y + rect.height / 2,
                   UI_PX_BTN, label, EPD_DRAW_ALIGN_CENTER, true);
        return;
    }
    if (s_pressed_control == id) ui_draw_pressed_round_rect(fb, rect, UI_BTN_RADIUS);
    ui_draw_button(fb, rect, label, false);
}
static void lock_draw(void) { if (s_draw_lock) xSemaphoreTake(s_draw_lock, portMAX_DELAY); }
static void unlock_draw(void) { if (s_draw_lock) xSemaphoreGive(s_draw_lock); }
static size_t fb_bytes(void) { return (size_t)epd_width() * epd_height() / 2; }
// 全屏时正文扩展到状态栏下方和屏幕底部；普通模式保留页眉与页脚。
// Full screen extends the body below the status row and near the bottom; standard mode keeps header and footer.
static EpdRect reader_area(void) {
    const int top = s_reader_fullscreen ? (app_settings_reader_immersive() ? 0 : 80) : 160;
    const int bottom = s_reader_fullscreen ? UI_LOCK_HEIGHT : (UI_BAR_TOP / 32) * 32;
    return (EpdRect){0, top, UI_LOCK_WIDTH, bottom - top};
}
static EpdRect body_rect(void) {
    int top = s_reader_fullscreen ? (app_settings_reader_immersive() ? 28 : 92) : 174;
    int margin = s_margin ? s_margin : 36;
    int bottom = s_reader_fullscreen ? UI_LOCK_HEIGHT - 24 : UI_BAR_TOP - 8;
    return (EpdRect){margin, top, UI_LOCK_WIDTH - 2 * margin, bottom - top};
}
static EpdRect progress_rect(void) {
    // 阅读页没有右侧菜单按钮；进度信息应和页眉分隔线一样铺满内容宽度。
    // The reader has no right-hand menu button; the footer spans the full content width.
    return (EpdRect){36, UI_BAR_TOP, UI_LOCK_WIDTH - 72, UI_BAR_H};
}
static EpdRect row_rect(int row) {
    if (s_view != BULK) {
        int col = row % 3, line = row / 3;
        int cell = (ui_content_width() - 2 * UI_GAP) / 3;
        return (EpdRect){UI_MARGIN + col * (cell + UI_GAP), 224 + line * 282, cell, 260};
    }
    return (EpdRect){UI_MARGIN, 308 + row * (UI_BTN_H + UI_GAP), ui_content_width(), UI_BTN_H};
}
static void invalidate_covers(void) {
    s_cover_pending_mask = 0;
    for (int i = 0; i < BOOK_ROWS; ++i) {
        free(s_covers[i].gray);
        s_covers[i].gray = NULL;
        s_covers[i].index = -1;
    }
}
typedef struct {
    uint32_t magic;
    uint32_t size;
    int64_t modified;
    uint64_t path_hash;
} cover_cache_header_t;
static uint64_t cover_path_hash(const char* path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}
static bool cover_cache_path(const char* source, char* out, size_t cap, cover_cache_header_t* header) {
    read_pico_sd_info_t sd = {0};
    read_pico_sd_get_info(&sd);
    if (!sd.present || !sd.mounted) return false;
    struct stat st;
    if (stat(source, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > UINT32_MAX) return false;
    *header = (cover_cache_header_t){.magic = UINT32_C(0x52435032), .size = (uint32_t)st.st_size,
        .modified = (int64_t)st.st_mtime, .path_hash = cover_path_hash(source)};
    return snprintf(out, cap, "/sdcard/.readpico/covers/%016llx.bin",
        (unsigned long long)header->path_hash) < (int)cap;
}
static bool cover_cache_read(const char* path, const cover_cache_header_t* expected, uint8_t* gray, bool* missing) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    cover_cache_header_t found;
    bool ok = fread(&found, 1, sizeof(found), file) == sizeof(found) &&
        (found.magic == expected->magic || found.magic == UINT32_C(0x5243504e)) &&
        found.size == expected->size && found.modified == expected->modified &&
        found.path_hash == expected->path_hash;
    *missing = ok && found.magic == UINT32_C(0x5243504e);
    if (ok && !*missing) ok = fread(gray, 1, BOOK_COVER_W * BOOK_COVER_H, file) == BOOK_COVER_W * BOOK_COVER_H;
    if (ok) ok = fgetc(file) == EOF;
    fclose(file);
    return ok;
}
static void cover_cache_write(const char* path, const cover_cache_header_t* header, const uint8_t* gray) {
    if (mkdir("/sdcard/.readpico", 0777) && errno != EEXIST) return;
    if (mkdir("/sdcard/.readpico/covers", 0777) && errno != EEXIST) return;
    char temp[100];
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return;
    FILE* file = fopen(temp, "wb");
    if (!file) return;
    cover_cache_header_t saved = *header;
    if (!gray) saved.magic = UINT32_C(0x5243504e);
    bool ok = fwrite(&saved, 1, sizeof(saved), file) == sizeof(saved) &&
        (!gray || fwrite(gray, 1, BOOK_COVER_W * BOOK_COVER_H, file) == BOOK_COVER_W * BOOK_COVER_H);
    if (fclose(file) != 0) ok = false;
    if (ok) {
        remove(path);
        if (rename(temp, path) == 0) return;
    }
    remove(temp);
}
static uint8_t* load_cover_gray(const char* source, bool decode, bool* pending) {
    *pending = false;
    const char* ext = strrchr(source, '.');
    if (!ext || strcasecmp(ext, ".epub")) return NULL;
    uint8_t* gray = heap_caps_malloc(BOOK_COVER_W * BOOK_COVER_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gray) return NULL;
    char path[96];
    cover_cache_header_t header;
    bool cache = cover_cache_path(source, path, sizeof(path), &header);
    bool missing = false;
    if (cache && cover_cache_read(path, &header, gray, &missing)) {
        if (missing) { free(gray); return NULL; }
        return gray;
    }
    if (cache && !decode) { *pending = true; free(gray); return NULL; }
    uint8_t* data = NULL; size_t size = 0; bool png = false;
    esp_err_t err = book_epub_cover(source, &data, &size, &png);
    bool ok = err == ESP_OK && book_cover_thumbnail(data, size, png, gray);
    free(data);
    if (!ok) {
        if (cache && (err == ESP_OK || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED ||
                      err == ESP_ERR_INVALID_SIZE)) cover_cache_write(path, &header, NULL);
        free(gray); return NULL;
    }
    if (cache) cover_cache_write(path, &header, gray);
    return gray;
}
static void prepare_covers(app_ctx_t* ctx) {
    if (s_view != SHELF && s_view != MANAGE) return;
    if (s_covers[0].index != ctx->leaf * BOOK_ROWS) s_cover_pending_mask = 0;
    for (int row = 0; row < BOOK_ROWS; ++row) {
        int index = ctx->leaf * BOOK_ROWS + row;
        if (s_covers[row].index == index) continue;
        free(s_covers[row].gray);
        s_covers[row].gray = NULL;
        s_covers[row].index = index;
        if (index >= s_visible_count || index < 0 || s_shelf[index].removed) continue;
        bool pending = false;
        s_covers[row].gray = load_cover_gray(s_shelf[index].path, false, &pending);
        if (pending) s_cover_pending_mask |= 1u << row;
    }
}
static void draw_shelf_cover(uint8_t* fb, EpdRect card, int row, const char* name) {
    EpdRect image = {card.x + (card.width - 164) / 2, card.y, 164, 214};
    epd_fill_rect(image, UI_GRAY_LIGHT, fb);
    if (s_covers[row].gray) {
        const uint8_t *gray = s_covers[row].gray;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const uint8_t tone = ui_contrast_gray(gray[(y * BOOK_COVER_H / image.height) * BOOK_COVER_W +
                                                            x * BOOK_COVER_W / image.width]);
                epd_draw_pixel(image.x + x, image.y + y,
                               ui_image_dither_gray(tone, image.x + x, image.y + y), fb);
            }
        }
    } else {
        char title[80]; copy_text(title, sizeof(title), name);
        char *dot = strrchr(title, '.'); if (dot) *dot = 0;
        fit_text(title, 26, image.width - 22);
        ui_text_vc(fb, image.x + image.width / 2, image.y + image.height / 2, 26,
                   title, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_draw_round_rect(fb, image, 0, UI_GRAY_BLACK);
}
static void draw_shelf_furniture(uint8_t* fb) {
    uint8_t style = app_settings_shelf_style();
    if (!style) return;
    for (int row = 0; row < 3; ++row) {
        int top = 224 + row * 282;
        if (style == 1) {
            epd_fill_rect((EpdRect){36, top + 214, 612, 12}, 0x30, fb);
            ui_hairline(fb, top + 214, 36, 612, 0x80);
            ui_hairline(fb, top + 225, 36, 612, 0x10);
        } else if (style == 2) {
            ui_draw_acrylic_guard(fb, (EpdRect){20, top + 136, 644, 90});
        } else if (style == 3) {
            for (int col = 0; col < 3; ++col) {
                int center = 130 + col * 205;
                ui_draw_frosted_pocket(fb, (EpdRect){center - 90, top + 92, 180, 128});
            }
        }
    }
}
static EpdRect tool_rect(int i) {
    int x0 = i * UI_LOCK_WIDTH / BOOK_TOOL_COUNT;
    int x1 = (i + 1) * UI_LOCK_WIDTH / BOOK_TOOL_COUNT;
    return (EpdRect){x0, 1096, x1 - x0, 120};
}
static int leaves(void) {
    if (s_view == TOC) return book_toc_pages(book_navigation_count());
    int count = s_visible_count;
    int rows = s_view == BULK ? BOOK_BULK_ROWS : BOOK_ROWS;
    return count ? 1 + (count - 1) / rows : 1;
}
static EpdRect shelf_manage_rect(void) { return (EpdRect){442, 94, 97, 54}; }
static EpdRect shelf_import_rect(void) { return (EpdRect){551, 94, 97, 54}; }
static void clean_filename(char *dst, size_t cap, const char *filename) {
    copy_text(dst, cap, filename);
    char *ext = strrchr(dst, '.');
    if (ext) *ext = 0;
    char *start = dst;
    while (*start == ' ' || *start == '\t') ++start;
    if (start != dst) memmove(dst, start, strlen(start) + 1);
    size_t len = strlen(dst);
    while (len && (dst[len - 1] == ' ' || dst[len - 1] == '\t')) dst[--len] = 0;
}

// 截断必须停在UTF8字符边界。/ Truncation must stop at a UTF8 character boundary.
static void copy_text(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}
static void fit_text(char* text, int px, int width) {
    while (*text && ttf_text_width_px(ui_text_effective_px(px), text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}

static void fit_fixed_text(char* text, int px, int width) {
    while (*text && ui_text_fixed_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}

static void reader_footer_strip_number(char *dst, size_t cap, const char *source) {
    char title[128];
    copy_text(title, sizeof(title), source ? source : "");
    char *name = title;
    while (*name == ' ' || *name == '\t') ++name;
    char *chapter_mark = !strncmp(name, "第", strlen("第")) ? strstr(name, "章") : NULL;
    if (chapter_mark && chapter_mark - name < 24) {
        char *after = chapter_mark + strlen("章");
        while (*after == ' ' || *after == '\t' || *after == ':' || *after == '-' ||
               !strncmp(after, "：", strlen("：")) || !strncmp(after, "、", strlen("、")) ||
               !strncmp(after, "·", strlen("·"))) {
            if (*after == ' ' || *after == '\t' || *after == ':' || *after == '-') ++after;
            else after += strlen("：");
        }
        name = after;
    }
    if (!*name || !strcmp(name, "本章") || !strcmp(name, "章节")) name = "未命名章节";
    copy_text(dst, cap, name);
}
static void reader_footer_chapter_name(char *dst, size_t cap) {
    char source[128] = {0};
    bool selected = s_selected_toc < book_navigation_count() &&
        book_navigation_chapter(s_selected_toc) == s_chapter &&
        book_navigation_title(s_selected_toc, source, sizeof(source)) == ESP_OK && source[0];
    if (!selected) {
        if (s_chapter_heading_title[0]) copy_text(source, sizeof(source), s_chapter_heading_title);
        else if (book_chapter_title(s_chapter, source, sizeof(source)) != ESP_OK) source[0] = 0;
    }
    reader_footer_strip_number(dst, cap, source);
}
static void draw_shelf_title(uint8_t* fb, EpdRect card, const char* full) {
    char line[128];
    copy_text(line, sizeof(line), full);
    if (ttf_text_width_px(ui_text_effective_px(22), line) <= card.width - 8) {
        ui_text_vc(fb, card.x + card.width / 2, card.y + 238, 22, line, EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    const char* next = full;
    char rows[2][128] = {{0}};
    for (int row = 0; row < 2; ++row) {
        size_t used = 0;
        while (*next && used + 5 < sizeof(rows[row])) {
            size_t bytes = ((unsigned char)*next & 0x80) == 0 ? 1 :
                ((unsigned char)*next & 0xe0) == 0xc0 ? 2 :
                ((unsigned char)*next & 0xf0) == 0xe0 ? 3 : 4;
            if (strlen(next) < bytes) break;
            memcpy(rows[row] + used, next, bytes);
            rows[row][used + bytes] = 0;
            if (ttf_text_width_px(ui_text_effective_px(19), rows[row]) > card.width - 8) {
                rows[row][used] = 0;
                break;
            }
            used += bytes;
            next += bytes;
        }
    }
    if (*next) {
        size_t n = strlen(rows[1]);
        while (n && ttf_text_width_px(ui_text_effective_px(19), rows[1]) > card.width - 35) {
            do { --n; } while (n && ((unsigned char)rows[1][n] & 0xc0) == 0x80);
            rows[1][n] = 0;
        }
        strlcat(rows[1], "…", sizeof(rows[1]));
    }
    ui_text_vc(fb, card.x + card.width / 2, card.y + 227, 19, rows[0], EPD_DRAW_ALIGN_CENTER, false);
    if (rows[1][0]) ui_text_vc(fb, card.x + card.width / 2, card.y + 252, 19, rows[1], EPD_DRAW_ALIGN_CENTER, false);
}
static uint32_t chapter_end(void) {
    return s_chapter + 1 < book_chapter_count()
        ? book_chapter_byte_offset(s_chapter + 1) : book_total_bytes();
}
static size_t reader_page_offset(size_t page) {
    size_t off = book_layout_page_start_offset(page);
    if (s_jump_offset != SIZE_MAX && page == s_jump_page && s_jump_offset > off)
        off = s_jump_offset;
    return off;
}
static unsigned percent(size_t page) {
    uint32_t total = book_total_bytes();
    if (!total) return 0;
    if (s_chapter + 1 == book_chapter_count() && page + 1 == book_layout_page_count()) return 100;
    uint32_t off = book_position_bytes(book_chapter_byte_offset(s_chapter), chapter_end(),
                                       reader_page_offset(page), s_text_len);
    return (unsigned)((uint64_t)off * 100 / total);
}
static pending_progress_t* pending_find(const char* path) {
    for (pending_progress_t* p = s_pending; p; p = p->next) if (!strcmp(p->path, path)) return p;
    return NULL;
}
static int layout_name(uint8_t* fb, const char* name, int y, bool draw);
static EpdRect manage_panel(void) {
    int height = layout_name(NULL, s_managed.name, 0, false) + 495;
    return (EpdRect){UI_MARGIN - 16, 190 + (876 - height) / 2, ui_content_width() + 32, height};
}
static EpdRect manage_rect(int index, int count) {
    EpdRect panel = manage_panel();
    return ui_row_rect(index, count, panel.y + panel.height - 94, 76);
}
static EpdRect batch_rect(int id) {
    if (id < 3) return ui_row_rect(id, 3, 978, 48);
    return ui_row_rect(id - 3, 2, 1038, 48);
}
static EpdRect search_rect(int id) {
    if (id < 40) {
        int width = (ui_content_width() - 54) / 10;
        return (EpdRect){UI_MARGIN + id % 10 * (width + 6), 388 + id / 10 * 100, width, 88};
    }
    if (id < 43) return ui_row_rect(id - 40, 3, 808, 80);
    return ui_bar_rect(id - 43, 2);
}
static size_t selected_count(void) {
    size_t selected = 0;
    for (int i = 0; i < s_count; ++i) if (s_shelf[i].selected) ++selected;
    return selected;
}
static void clear_selection(void) {
    for (int i = 0; i < s_count; ++i) s_shelf[i].selected = false;
}
static void toggle_selection(int index) {
    if (index >= 0 && index < s_visible_count) s_shelf[index].selected = !s_shelf[index].selected;
}
static void select_page(int page) {
    for (int i = page * BOOK_BULK_ROWS; i < s_visible_count && i < (page + 1) * BOOK_BULK_ROWS; ++i) s_shelf[i].selected = true;
}
static const char* search_keys(void) {
    return "1234567890" "qwertyuiop" "asdfghjkl-" "zxcvbnm._'";
}
static void search_begin(void) {
    s_search_parent = s_view;
    memcpy(s_search_draft, s_query, sizeof(s_query));
    s_view = SEARCH;
}
static void refresh_search_matches(void) {
    for (int i = 0; i < s_count; ++i)
        s_shelf[i].search_match = read_pico_search_match(s_shelf[i].name, s_query);
}
static void search_finish(app_ctx_t* ctx, bool apply) {
    s_view = s_search_parent;
    if (apply) {
        memcpy(s_query, s_search_draft, sizeof(s_query));
        refresh_search_matches();
        clear_selection();
        sort_shelf(ctx);
        s_batch_message[0] = 0;
    }
    memset(s_search_draft, 0, sizeof(s_search_draft));
}
static app_redraw_t search_action(app_ctx_t* ctx, int id) {
    size_t len = strlen(s_search_draft);
    if ((id >= 0 && id < 40) || id == 40) {
        if (len < sizeof(s_search_draft) - 1) {
            s_search_draft[len] = id == 40 ? ' ' : search_keys()[id];
            s_search_draft[len + 1] = 0;
        }
    } else if (id == 41 && len) s_search_draft[len - 1] = 0;
    else if (id == 42) s_search_draft[0] = 0;
    else if (id == 43 || id == 44) { search_finish(ctx, id == 44); return APP_REDRAW_PAGE; }
    return APP_REDRAW_AREA;
}
static bool pending_reserve(const char* path) {
    if (pending_find(path)) return true;
    pending_progress_t* p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(*p));
    if (!p) return false;
    memset(p, 0, sizeof(*p));
    copy_text(p->path, sizeof(p->path), path);
    // 阅读页内注册；传书 HTTP 尚未启动，退出后也不注销失败项。
    // Register in reading before transfer HTTP starts; failed records survive page exit.
    p->watch = book_progress_watch_create(path);
    if (!p->watch) { free(p); return false; }
    pending_progress_t** tail = &s_pending;
    while (*tail) tail = &(*tail)->next;
    *tail = p;
    return true;
}
static bool pending_restore(const char* path, uint32_t size, book_progress_t* out) {
    pending_progress_t* pending = pending_find(path);
    if (!pending || !pending->dirty || pending->value.file_size != size) return false;
    *out = pending->value;
    return true;
}
static void pending_discard(const char* path) {
    pending_progress_t** p = &s_pending;
    while (*p) {
        if (!strcmp((*p)->path, path)) {
            pending_progress_t* old = *p;
            *p = old->next;
            book_progress_watch_destroy(old->watch);
            free(old);
            break;
        }
        p = &(*p)->next;
    }
    s_save_failed = false;
    for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
}
static void pending_mark_latest(const char* path) {
    pending_progress_t** item = &s_pending;
    while (*item && strcmp((*item)->path, path)) item = &(*item)->next;
    if (*item) {
        pending_progress_t* current = *item;
        *item = current->next;
        current->next = NULL;
        pending_progress_t** tail = &s_pending;
        while (*tail) tail = &(*tail)->next;
        *tail = current;
    }
    copy_text(s_latest_path, sizeof(s_latest_path), path);
}
static void pending_drop_invalidated(void) {
    pending_progress_t* p = s_pending;
    while (p) {
        pending_progress_t* next = p->next;
        if (book_progress_watch_invalidated(p->watch)) {
            if (p->dirty) s_pending_invalidated = true;
            pending_discard(p->path);
        }
        p = next;
    }
}
static delete_retry_t* delete_retry_find(const char* path) {
    for (delete_retry_t* p = s_delete_retries; p; p = p->next)
        if (!strcmp(p->entry.path, path)) return p;
    return NULL;
}
static bool pending_flush(pending_progress_t* p) {
    if (!p->dirty) return true;
    if (!p->progress_saved) {
        if (book_progress_save(p->path, &p->value) != ESP_OK) return false;
        p->progress_saved = true;
    }
    if (!strcmp(p->path, s_latest_path) && book_progress_set_last_path(p->path) != ESP_OK) return false;
    p->dirty = false;
    return true;
}
static void retry_progress(void) {
    s_save_failed = false;
    bool had_dirty = false;
    pending_progress_t* p = s_pending;
    while (p) {
        pending_progress_t* next = p->next;
        bool dirty = p->dirty;
        had_dirty |= dirty;
        if (s_text && !strcmp(p->path, s_path)) { p = next; continue; }
        if (!pending_flush(p)) s_save_failed = true;
        else if (strcmp(p->path, s_path)) pending_discard(p->path);
        else if (dirty) s_unsaved = 0;
        p = next;
    }
    if (s_text && (s_unsaved || had_dirty)) save_progress();
    s_save_failed = false;
    for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
}

/* ---- 管理详情 / Management details ---- */
static int layout_name(uint8_t* fb, const char* name, int y, bool draw) {
    const char* at = name;
    while (*at) {
        char line[256];
        size_t used = 0;
        while (at[used]) {
            unsigned char first = (unsigned char)at[used];
            size_t n = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            size_t remain = strlen(at + used);
            if (n > remain) n = 1;
            if (used + n >= sizeof(line)) break;
            memcpy(line + used, at + used, n);
            line[used + n] = 0;
            if (ttf_text_width_px(UI_PX_CAPTION, line) > ui_content_width() && used) break;
            used += n;
        }
        if (!used) break;
        line[used] = 0;
        if (draw) ui_text(fb, UI_MARGIN, y, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
        y += 36;
        at += used;
    }
    return y;
}
static int draw_wrapped_name(uint8_t* fb, const char* name, int y) {
    return layout_name(fb, name, y, true);
}
static void draw_manage(uint8_t* fb) {
    EpdRect panel = manage_panel();
    ui_fill_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text(fb, UI_MARGIN, panel.y + 18, UI_PX_BODY,
            s_clear_confirm ? (s_delete_confirm ? "确认删除文件？" : "确认清除进度？") : "图书详情", EPD_DRAW_ALIGN_LEFT, false);
    int y = draw_wrapped_name(fb, s_managed.name, panel.y + 74) + 12;
    char info[96];
    snprintf(info, sizeof(info), "%s · %s · %.2f MB", s_managed.is_flash ? "内置存储" : "TF 卡",
             strrchr(s_managed.name, '.') ? strrchr(s_managed.name, '.') + 1 : "", s_managed.size / 1048576.0);
    ui_text(fb, UI_MARGIN, y, UI_PX_CAPTION, info, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(info, sizeof(info), s_managed.has_progress ? "阅读进度 %u%%" : "尚无阅读进度", s_managed.pct);
    ui_text(fb, UI_MARGIN, y + 44, UI_PX_CAPTION, info, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 88, UI_PX_CAPTION,
            s_managed.is_flash ? "/flash/books" : !strncmp(s_managed.path, "/sdcard/book/", 13) ? "/sdcard/book" : app_settings_books_dir(),
            EPD_DRAW_ALIGN_LEFT, false);
    if (s_manage_message[0]) ui_text(fb, UI_MARGIN, panel.y + panel.height - 270, UI_PX_CAPTION, s_manage_message, EPD_DRAW_ALIGN_LEFT, false);
    if (s_clear_confirm) {
        ui_text(fb, UI_MARGIN, panel.y + panel.height - 142, UI_PX_CAPTION, s_delete_confirm ? "删除后文件无法恢复" : "仅清阅读进度，保留图书文件", EPD_DRAW_ALIGN_LEFT, false);
        draw_control(fb, manage_rect(0, 2), "取消", 300);
        draw_control(fb, manage_rect(1, 2), s_delete_confirm ? "确认删除" : "确认清除", 301);
    } else if (s_file_removed) {
        draw_control(fb, manage_rect(0, 2), "关闭", 400);
        draw_control(fb, manage_rect(1, 2), "重试清理", 403);
    } else {
        draw_control(fb, (EpdRect){UI_MARGIN, panel.y + panel.height - 190, ui_content_width(), 68}, "前往文件管理重命名", 404);
        draw_control(fb, manage_rect(0, 3), "关闭", 400);
        draw_control(fb, manage_rect(1, 3), "清进度", 401);
        draw_control(fb, manage_rect(2, 3), "删除文件", 402);
    }
}
static void editor_refresh_candidates(void) {
    s_editor_candidate_count = s_editor_pinyin[0] && s_editor_chinese
        ? read_pico_search_candidates(s_editor_pinyin, s_editor_candidates, 5,
                                      s_editor_candidate_page * 5) : 0;
}
static void editor_start(void) {
    free(s_editor_cover);
    bool pending = false;
    s_editor_cover = load_cover_gray(s_managed.path, false, &pending);
    copy_text(s_editor_title, sizeof(s_editor_title), s_managed.name);
    s_editor_cursor = strlen(s_editor_title);
    s_editor_pinyin[0] = s_editor_notice[0] = 0;
    s_editor_chinese = true;
    s_editor_candidate_page = 0;
    s_editor_candidate_count = 0;
    s_view = EDIT;
}
static bool editor_append(const char *utf8) {
    size_t a = strlen(s_editor_title), b = strlen(utf8);
    if (a + b > 120) { copy_text(s_editor_notice, sizeof(s_editor_notice), "书名最多 120 字节"); return false; }
    memmove(s_editor_title + s_editor_cursor + b, s_editor_title + s_editor_cursor, a - s_editor_cursor + 1);
    memcpy(s_editor_title + s_editor_cursor, utf8, b);
    s_editor_cursor += b;
    s_editor_notice[0] = 0;
    return true;
}
static void editor_backspace(void) {
    if (s_editor_pinyin[0]) {
        size_t n = strlen(s_editor_pinyin);
        s_editor_pinyin[n - 1] = 0;
    } else if (s_editor_cursor) {
        size_t prev = s_editor_cursor - 1;
        while (prev && ((unsigned char)s_editor_title[prev] & 0xc0) == 0x80) --prev;
        memmove(s_editor_title + prev, s_editor_title + s_editor_cursor,
                strlen(s_editor_title + s_editor_cursor) + 1);
        s_editor_cursor = prev;
    } else return;
    s_editor_candidate_page = 0;
    editor_refresh_candidates();
}
static size_t editor_next(size_t at) {
    if (!s_editor_title[at]) return at;
    ++at;
    while (s_editor_title[at] && ((unsigned char)s_editor_title[at] & 0xc0) == 0x80) ++at;
    return at;
}
static void editor_move(int delta) {
    if (delta > 0) s_editor_cursor = editor_next(s_editor_cursor);
    else if (delta < 0 && s_editor_cursor) {
        --s_editor_cursor;
        while (s_editor_cursor && ((unsigned char)s_editor_title[s_editor_cursor] & 0xc0) == 0x80)
            --s_editor_cursor;
    }
}
static size_t editor_visible_start(void) {
    size_t start = 0;
    while (start < s_editor_cursor) {
        char prefix[121];
        size_t bytes = s_editor_cursor - start;
        memcpy(prefix, s_editor_title + start, bytes); prefix[bytes] = 0;
        if (ttf_text_width_px(31, prefix) <= 461) break;
        start = editor_next(start);
    }
    return start;
}
static void editor_place_cursor(int x) {
    size_t start = editor_visible_start(), at = start, best = start;
    int nearest = 10000;
    while (at <= strlen(s_editor_title)) {
        char prefix[121];
        size_t bytes = at - start;
        memcpy(prefix, s_editor_title + start, bytes); prefix[bytes] = 0;
        int width = ttf_text_width_px(31, prefix);
        if (width > 461) break;
        int dist = abs(x - (53 + width));
        if (dist < nearest) { nearest = dist; best = at; }
        size_t next = editor_next(at);
        if (next == at) break;
        at = next;
    }
    s_editor_cursor = best;
}
static void editor_commit_candidate(size_t index) {
    if (index >= s_editor_candidate_count) return;
    uint32_t cp = s_editor_candidates[index];
    char utf8[5];
    int len = cp <= 0x7f ? 1 : cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;
    if (len == 3) {
        utf8[0] = 0xe0 | (cp >> 12);
        utf8[1] = 0x80 | ((cp >> 6) & 63);
        utf8[2] = 0x80 | (cp & 63);
    } else return;
    utf8[len] = 0;
    if (editor_append(utf8)) {
        s_editor_pinyin[0] = 0;
        s_editor_candidate_page = 0;
        editor_refresh_candidates();
    }
}
static void draw_editor(uint8_t* fb) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, UI_LOCK_WIDTH / 2, 107, 31, "编辑书籍", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 646, 107, 26, "完成", EPD_DRAW_ALIGN_RIGHT, false);
    ui_hairline(fb, 133, 36, 612, UI_GRAY_LIGHT);
    EpdRect cover = {36, 175, 112, 152};
    epd_fill_rect(cover, UI_GRAY_LIGHT, fb);
    if (s_editor_cover) {
        for (int y = 0; y < cover.height; ++y) {
            for (int x = 0; x < cover.width; ++x) {
                const uint8_t tone = ui_contrast_gray(
                    s_editor_cover[(y * BOOK_COVER_H / cover.height) * BOOK_COVER_W +
                                   x * BOOK_COVER_W / cover.width]);
                epd_draw_pixel(cover.x + x, cover.y + y,
                    ui_image_dither_gray(tone, cover.x + x, cover.y + y), fb);
            }
        }
    } else {
        char title[80]; copy_text(title, sizeof(title), s_managed.name);
        fit_text(title, 22, cover.width - 16);
        ui_text_vc(fb, cover.x + cover.width / 2, cover.y + cover.height / 2, 22, title, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_draw_round_rect(fb, cover, 0, UI_GRAY_BLACK);
    char title[128]; copy_text(title, sizeof(title), s_managed.name);
    fit_text(title, 30, 460);
    ui_text(fb, 174, 192, 30, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 174, 244, 22, "只修改书架显示名称", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 356, 36, 612, UI_GRAY_LIGHT);
    ui_text(fb, 36, 378, 24, "书名", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect field = {36, 424, 612, 83};
    ui_draw_round_rect(fb, field, 8, UI_GRAY_BLACK);
    size_t start = editor_visible_start(), end = start;
    while (s_editor_title[end]) {
        size_t next = editor_next(end);
        char candidate[121];
        memcpy(candidate, s_editor_title + start, next - start); candidate[next - start] = 0;
        if (ttf_text_width_px(31, candidate) > 461) break;
        end = next;
    }
    char visible[121];
    memcpy(visible, s_editor_title + start, end - start); visible[end - start] = 0;
    ui_text_vc(fb, 53, 465, 31, visible, EPD_DRAW_ALIGN_LEFT, false);
    char before[121];
    memcpy(before, s_editor_title + start, s_editor_cursor - start); before[s_editor_cursor - start] = 0;
    int caret_x = 53 + ttf_text_width_px(31, before);
    for (int i = 0; i < 2; ++i) epd_draw_line(caret_x + i, 441, caret_x + i, 489, UI_GRAY_BLACK, fb);
    ui_hairline(fb, 431, 534, 1, UI_GRAY_LIGHT);
    ui_text_vc(fb, 562, 465, 32, "‹", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 618, 465, 32, "›", EPD_DRAW_ALIGN_CENTER, false);
    ui_text(fb, 36, 530, 22, s_editor_chinese ? "拼音输入 · 点书名定位光标" : "英文输入 · 点书名定位光标", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 36, 560, 25, s_editor_pinyin[0] ? s_editor_pinyin : " ", EPD_DRAW_ALIGN_LEFT, false);
    for (int i = 0; i < 5; ++i) {
        EpdRect r = {36 + i * 112, 606, 106, 57};
        if (i == 0 && s_editor_candidate_count) ui_fill_round_rect(fb, r, 4, UI_GRAY_LIGHT);
        else ui_draw_round_rect(fb, r, 4, UI_GRAY_LIGHT);
        if (i < (int)s_editor_candidate_count) {
            uint32_t cp = s_editor_candidates[i];
            char glyph[4] = {(char)(0xe0 | (cp >> 12)),
                (char)(0x80 | ((cp >> 6) & 63)), (char)(0x80 | (cp & 63)), 0};
            ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 30, glyph, EPD_DRAW_ALIGN_CENTER, false);
        }
    }
    ui_text_vc(fb, 634, 634, 27, "›", EPD_DRAW_ALIGN_CENTER, false);
    static const char *keys[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int row = 0; row < 3; ++row) {
        int len = strlen(keys[row]);
        int left = row == 0 ? 36 : row == 1 ? 67 : 123;
        for (int col = 0; col < len; ++col) {
            EpdRect r = {left + col * 62, 695 + row * 74, 58, 61};
            ui_draw_round_rect(fb, r, 5, UI_GRAY_LIGHT);
            char label[2] = {keys[row][col], 0};
            ui_text_vc(fb, r.x + 29, r.y + 30, 25, label, EPD_DRAW_ALIGN_CENTER, false);
        }
    }
    static const char *actions[] = {"中 / EN", "空格", "删除", "确定"};
    static const EpdRect buttons[] = {{36, 929, 102, 70}, {148, 929, 298, 70}, {456, 929, 98, 70}, {564, 929, 84, 70}};
    for (int i = 0; i < 4; ++i) ui_draw_button(fb, buttons[i], actions[i], i == 3);
    if (s_editor_notice[0]) ui_text(fb, 36, 1031, 22, s_editor_notice, EPD_DRAW_ALIGN_LEFT, false);
    else ui_text(fb, 36, 1031, 22, "选择候选字；左右翻候选页", EPD_DRAW_ALIGN_LEFT, false);
}
static void draw_search(uint8_t* fb) {
    ui_clear_page(fb);
    ui_draw_header(fb, "搜索图书", "输入拼音首字母、完整拼音或英文");
    EpdRect field = {UI_MARGIN, 200, ui_content_width(), 88};
    ui_draw_round_rect(fb, field, UI_BTN_RADIUS, UI_GRAY_BLACK);
    const char* tail = s_search_draft;
    while (*tail && ttf_text_width_px(UI_PX_BODY, tail) > field.width - 2 * UI_PAD) ++tail;
    ui_text_vc(fb, field.x + UI_PAD, field.y + field.height / 2, UI_PX_BODY, tail, EPD_DRAW_ALIGN_LEFT, false);
    char count[48];
    snprintf(count, sizeof(count), "%u/64 · 清空后应用可显示全部", (unsigned)strlen(s_search_draft));
    ui_text(fb, UI_MARGIN, 310, UI_PX_CAPTION, count, EPD_DRAW_ALIGN_LEFT, false);
    const char* keys = search_keys();
    for (int i = 0; i < 40; ++i) {
        char label[2] = {keys[i], 0};
        draw_control(fb, search_rect(i), label, 500 + i);
    }
    draw_control(fb, search_rect(40), "空格", 540);
    draw_control(fb, search_rect(41), "退格", 541);
    draw_control(fb, search_rect(42), "清空", 542);
    ui_text(fb, UI_MARGIN, 938, UI_PX_CAPTION, "应用清除勾选 · KEY1 取消", EPD_DRAW_ALIGN_LEFT, false);
    draw_control(fb, search_rect(43), "取消", 543);
    draw_control(fb, search_rect(44), "应用", 544);
    ui_draw_menu_handle(fb, false);
}
static void draw_batch_confirmation(uint8_t* fb) {
    if (!s_batch_confirm) return;
    EpdRect panel = {UI_MARGIN - 12, 410, ui_content_width() + 24, 330};
    ui_fill_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_BLACK);
    char title[96];
    snprintf(title, sizeof(title), "%s %u 本图书？", s_batch_delete ? "删除" : "清除进度：", (unsigned)selected_count());
    ui_text(fb, UI_MARGIN, 438, UI_PX_BODY, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, 508, UI_PX_CAPTION, s_batch_delete ? "删除文件不可撤销，失败项可重试" : "仅清阅读进度，所有文件保留", EPD_DRAW_ALIGN_LEFT, false);
    draw_control(fb, ui_row_rect(0, 2, 620, UI_BTN_H), "取消", 600);
    draw_control(fb, ui_row_rect(1, 2, 620, UI_BTN_H), "确认", 601);
}
static void save_progress(void) {
    if (!s_text || !s_path[0] || !book_layout_page_count()) return;
    bool was_failed = s_save_failed;
    pending_progress_t* pending = pending_find(s_path);
    if (!pending) { s_save_failed = true; return; }
    book_progress_t p = {
        .file_size = s_file_size, .chapter = (uint16_t)s_chapter,
        .byte_off = (uint32_t)reader_page_offset(s_page),
        .px = (uint8_t)s_px, .pct = (uint8_t)percent(s_page),
        .last_open_s = 0,
    };
    if (!pending->dirty || pending->value.file_size != p.file_size || pending->value.chapter != p.chapter ||
        pending->value.byte_off != p.byte_off || pending->value.px != p.px || pending->value.pct != p.pct) {
        pending->value = p;
        pending->progress_saved = false;
    }
    pending->dirty = true;
    if (pending_flush(pending)) {
        s_unsaved = 0;
        s_save_failed = false;
        for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
    }
    else { if (!s_unsaved) s_unsaved = 1; s_save_failed = true; }
    if (was_failed != s_save_failed) invalidate_prep();
}
static void invalidate_prep(void) { s_next_page = s_prep_page = -1; }

/* ---- 阅读工具层 / Reader overlays ---- */
typedef struct {
    uint32_t magic, file_size, byte_off;
    uint16_t chapter;
    char path[BOOK_STORE_PATH_MAX];
} legacy_reader_bookmark_t;

typedef struct {
    uint16_t chapter;
    uint16_t reserved;
    uint32_t byte_off;
    uint32_t saved_s;
} reader_bookmark_entry_t;

typedef struct {
    uint32_t magic, file_size;
    uint16_t count, reserved;
    char path[BOOK_STORE_PATH_MAX];
    reader_bookmark_entry_t entries[BOOKMARK_MAX];
} reader_bookmarks_t;

static void bookmark_key(const char* path, char key[11]) {
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
        hash = (hash ^ *p) * UINT32_C(16777619);
    snprintf(key, 11, "m_%08lx", (unsigned long)hash);
}
static bool bookmark_read_handle(nvs_handle_t h, reader_bookmarks_t* marks) {
    if (!marks || !s_path[0]) return false;
    memset(marks, 0, sizeof(*marks));
    char key[11]; bookmark_key(s_path, key);
    size_t size = 0;
    if (nvs_get_blob(h, key, NULL, &size) != ESP_OK) return false;
    if (size == sizeof(legacy_reader_bookmark_t)) {
        legacy_reader_bookmark_t old;
        size = sizeof(old);
        if (nvs_get_blob(h, key, &old, &size) != ESP_OK ||
            old.magic != UINT32_C(0x52504d31) || old.file_size != s_file_size ||
            strcmp(old.path, s_path)) return false;
        marks->magic = UINT32_C(0x52504d32);
        marks->file_size = old.file_size;
        marks->count = 1;
        copy_text(marks->path, sizeof(marks->path), old.path);
        marks->entries[0] = (reader_bookmark_entry_t){
            .chapter = old.chapter, .byte_off = old.byte_off,
        };
        return true;
    }
    if (size != sizeof(*marks) || nvs_get_blob(h, key, marks, &size) != ESP_OK) return false;
    return marks->magic == UINT32_C(0x52504d32) && marks->file_size == s_file_size &&
        marks->count <= BOOKMARK_MAX && !strcmp(marks->path, s_path);
}

static bool bookmark_load(reader_bookmarks_t* marks) {
    if (!marks || !s_path[0]) return false;
    nvs_handle_t h;
    if (nvs_open("rp_marks", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = bookmark_read_handle(h, marks);
    nvs_close(h);
    return ok;
}
static bool bookmark_is_current(void) {
    reader_bookmarks_t marks;
    if (!bookmark_load(&marks)) return false;
    size_t offset = book_layout_page_start_offset(s_page);
    for (size_t i = 0; i < marks.count; ++i)
        if (marks.entries[i].chapter == s_chapter && marks.entries[i].byte_off == offset) return true;
    return false;
}
static size_t bookmark_count(void) {
    reader_bookmarks_t marks;
    return bookmark_load(&marks) ? marks.count : 0;
}
static bool bookmark_toggle(void) {
    if (!s_path[0]) return false;
    nvs_handle_t h;
    if (nvs_open("rp_marks", NVS_READWRITE, &h) != ESP_OK) return false;
    char key[11]; bookmark_key(s_path, key);
    reader_bookmarks_t marks;
    bool valid = bookmark_read_handle(h, &marks);
    if (!valid) {
        memset(&marks, 0, sizeof(marks));
        marks.magic = UINT32_C(0x52504d32);
        marks.file_size = s_file_size;
        copy_text(marks.path, sizeof(marks.path), s_path);
    }
    uint32_t offset = (uint32_t)book_layout_page_start_offset(s_page);
    size_t found = marks.count;
    for (size_t i = 0; i < marks.count; ++i)
        if (marks.entries[i].chapter == s_chapter && marks.entries[i].byte_off == offset) { found = i; break; }
    bool remove = found < marks.count;
    if (remove) {
        memmove(&marks.entries[found], &marks.entries[found + 1],
                (marks.count - found - 1) * sizeof(marks.entries[0]));
        --marks.count;
    } else {
        if (marks.count == BOOKMARK_MAX) {
            memmove(&marks.entries[0], &marks.entries[1],
                    (BOOKMARK_MAX - 1) * sizeof(marks.entries[0]));
            --marks.count;
        }
        time_t now = time(NULL);
        marks.entries[marks.count++] = (reader_bookmark_entry_t){
            .chapter = (uint16_t)s_chapter, .byte_off = offset,
            .saved_s = now >= 1704067200 ? (uint32_t)now : 0,
        };
    }
    esp_err_t err = marks.count ? nvs_set_blob(h, key, &marks, sizeof(marks)) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND && !marks.count) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        copy_text(s_reader_notice, sizeof(s_reader_notice), remove ? "书签已移除" : "书签已添加");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 1600;
    } else {
        copy_text(s_reader_notice, sizeof(s_reader_notice), "书签保存失败");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
    }
    return err == ESP_OK;
}

static void draw_reader_tool_icon(uint8_t* fb, int cx, int cy, int kind, bool selected) {
    uint8_t ink = 0x38;
    if (kind == 0) {
        const int ys[] = {-11, 0, 11}, widths[] = {21, 27, 18};
        for (int i = 0; i < 3; ++i) {
            epd_fill_circle(cx - 16, cy + ys[i], 2, ink, fb);
            epd_fill_rect((EpdRect){cx - 9, cy + ys[i] - 1, widths[i], 3}, ink, fb);
        }
    } else if (kind == 1) {
        int x[] = {cx - 12, cx - 12, cx - 9, cx + 9, cx + 12, cx + 12, cx, cx - 12};
        int y[] = {cy + 17, cy - 13, cy - 17, cy - 17, cy - 13, cy + 17, cy + 6, cy + 17};
        if (selected) {
            for (int py = cy - 16; py <= cy + 15; ++py) {
                int half = py <= cy + 5 ? 11 : 11 - (py - cy - 5);
                if (half > 0) epd_fill_rect((EpdRect){cx - half, py, half * 2 + 1, 1}, ink, fb);
            }
        } else for (int i = 0; i < 7; ++i) {
            epd_draw_line(x[i], y[i], x[i + 1], y[i + 1], ink, fb);
            epd_draw_line(x[i] + 1, y[i], x[i + 1] + 1, y[i + 1], ink, fb);
        }
    } else if (kind == 2) {
        const int xs[] = {cx - 12, cx, cx + 12}, tops[] = {cy + 4, cy - 4, cy - 14};
        for (int i = 0; i < 3; ++i) {
            epd_fill_rect((EpdRect){xs[i] - 1, tops[i], 3, cy + 18 - tops[i]}, ink, fb);
            epd_fill_circle(xs[i], tops[i], 2, ink, fb);
            epd_fill_circle(xs[i], cy + 17, 2, ink, fb);
        }
    } else if (kind == 3) {
        // Keep the supplied refresh shape; use the same 0x38 ink and center as its neighbors.
        const int size = READER_REFRESH_ICON_SIZE;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                int index = y * size + x;
                uint8_t packed = reader_refresh_icon_alpha[index / 2];
                int alpha = index & 1 ? packed & 15 : packed >> 4;
                if (!alpha) continue;
                uint8_t gray = (uint8_t)((255 * (15 - alpha) + ink * alpha + 7) / 15);
                epd_draw_pixel(cx - size / 2 + x, cy - size / 2 + y, gray, fb);
            }
        }
    } else ui_text_vc(fb, cx, cy, 33, "A", EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_sheet(uint8_t* fb, int top, const char* title) {
    EpdRect sheet = {0, top, UI_LOCK_WIDTH, UI_LOCK_HEIGHT - top + 28};
    ui_fill_round_rect(fb, sheet, 28, UI_GRAY_WHITE);
    epd_fill_rect((EpdRect){0, top + 28, UI_LOCK_WIDTH, UI_LOCK_HEIGHT - top - 28}, UI_GRAY_WHITE, fb);
    ui_draw_round_rect(fb, sheet, 28, 0x58);
    ui_fill_round_rect(fb, (EpdRect){292, top + 14, 100, 7}, 3, 0x48);
    ui_text_vc(fb, 342, top + 57, 30, title, EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_pill_slider(uint8_t* fb, EpdRect r, const char* left, const char* right,
                             const char* value, int index, int count, int left_px, int right_px) {
    ui_fill_round_rect(fb, r, r.height / 2, 0xb8);
    ui_draw_round_rect(fb, r, r.height / 2, 0x50);
    int cy = r.y + r.height / 2;
    ui_text_vc(fb, r.x + 28, cy, left_px, left, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(fb, r.x + r.width - 28, cy, right_px, right, EPD_DRAW_ALIGN_RIGHT, false);
    int span = r.width - 128;
    int cx = r.x + 64 + (count > 1 ? span * index / (count - 1) : 0);
    EpdRect track = {r.x + 59, cy - 3, r.width - 118, 6};
    ui_fill_round_rect(fb, track, 3, 0x78);
    track.width = cx - track.x;
    if (track.width > 0) ui_fill_round_rect(fb, track, 3, 0x40);
    epd_fill_circle(cx, cy, 37, UI_GRAY_WHITE, fb);
    epd_draw_circle(cx, cy, 37, 0x48, fb);
    ui_text_vc(fb, cx, cy, 22, value, EPD_DRAW_ALIGN_CENTER, false);
}

static EpdRect reader_slider_refresh_rect(int slider) {
    EpdRect r = reader_slider_rect(slider);
    // The knob extends four pixels outside the pill. Include that margin so
    // moving it restores the old white footprint and the rail beneath it.
    r.x -= 8; r.y -= 8; r.width += 16; r.height += 16;
    return r;
}

static const char* font_friendly_name(const ttf_font_item_t* item) {
    return item ? ttf_font_localized_name(item->name) : "";
}

static void draw_font_name_with_current_face(uint8_t* fb, int x, int y, const char* name) {
    int above = 0, below = 0;
    ttf_measure_line_px(25, name, &above, &below);
    ttf_draw_text_px(fb, x, y + (above - below) / 2, 25, name,
                     EPD_DRAW_ALIGN_CENTER, UI_INK_BLACK, UI_INK_WHITE);
}

static void draw_font_settings(uint8_t* fb) {
    const int top = 640;
    draw_sheet(fb, top, "字体设置");
    int shown_px = s_reader_slider >= 0 ? s_reader_preview_px : s_px;
    char value[16]; snprintf(value, sizeof(value), "%d", shown_px);
    draw_pill_slider(fb, reader_slider_rect(0), "A", "A", value,
                     shown_px - BOOK_PX_MIN, BOOK_PX_MAX - BOOK_PX_MIN + 1, 20, 31);
    EpdRect font_card = {36, 840, 294, 82};
    EpdRect shake_card = {354, 840, 294, 82};
    ui_fill_round_rect(fb, font_card, 20, 0xd8); ui_draw_round_rect(fb, font_card, 20, 0x70);
    ui_fill_round_rect(fb, shake_card, 20, 0xd8); ui_draw_round_rect(fb, shake_card, 20, 0x70);
    ui_text(fb, 58, font_card.y + 13, 17, "阅读字体", EPD_DRAW_ALIGN_LEFT, false);
    char font_name[64]; copy_text(font_name, sizeof(font_name), ttf_font_display_name());
    fit_text(font_name, 21, 210);
    ui_text(fb, 58, font_card.y + 49, 21, font_name, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(fb, 305, font_card.y + 42, 27, "›", EPD_DRAW_ALIGN_CENTER, false);
    ui_text(fb, 375, shake_card.y + 13, 17, "晃动翻页", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 375, shake_card.y + 49, 20, s_shake_enabled ? "开启" : "关闭", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect toggle = {565, shake_card.y + 25, 62, 34};
    ui_fill_round_rect(fb, toggle, 17, s_shake_enabled ? 0x50 : 0xd0);
    int knob = s_shake_enabled ? toggle.x + 45 : toggle.x + 17;
    epd_fill_circle(knob, toggle.y + 17, 13, UI_GRAY_WHITE, fb);
    epd_draw_circle(knob, toggle.y + 17, 13, 0x90, fb);
    EpdRect layout_card = {36, 944, 612, 92};
    ui_fill_round_rect(fb, layout_card, 20, 0xd8); ui_draw_round_rect(fb, layout_card, 20, 0x70);
    ui_text(fb, 58, 960, 23, "排版设置", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 58, 1000, 18, "边距 · 行距 · 段距 · 字间距", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(fb, 620, 990, 28, "›", EPD_DRAW_ALIGN_CENTER, false);
    EpdRect rule_card = {36, 1058, 612, 109};
    ui_fill_round_rect(fb, rule_card, 20, 0xd8); ui_draw_round_rect(fb, rule_card, 20, 0x70);
    ui_text(fb, 58, 1081, 21, "阅读线", EPD_DRAW_ALIGN_LEFT, false);
    static const char* rule_names[] = {"无", "虚线", "点线"};
    int selected_rule = app_settings_book_reading_line();
    ui_text(fb, 615, 1083, 18, rule_names[selected_rule], EPD_DRAW_ALIGN_RIGHT, false);
    ui_text(fb, 630, 1083, 22, "›", EPD_DRAW_ALIGN_RIGHT, false);
    if (!selected_rule) ui_text_vc(fb, 420, 1134, 18, "无阅读线", EPD_DRAW_ALIGN_CENTER, false);
    else {
        int span = selected_rule == 1 ? 20 : 3;
        int step = selected_rule == 1 ? 32 : 15;
        for (int x = 198; x < 610; x += step) {
            int width = x + span <= 610 ? span : 610 - x;
            epd_fill_rect((EpdRect){x, 1131, width, 2}, 0x58, fb);
        }
    }
}

static void draw_layout_settings(uint8_t* fb) {
    const int top = 575;
    draw_sheet(fb, top, "排版设置");
    epd_draw_circle(64, top + 57, 24, 0x78, fb);
    epd_draw_line(68, top + 47, 58, top + 57, 0x38, fb);
    epd_draw_line(58, top + 57, 68, top + 67, 0x38, fb);
    int shown_margin = s_reader_slider >= 0 ? s_reader_preview_margin : s_margin;
    int shown_line = s_reader_slider >= 0 ? s_reader_preview_line : app_settings_book_line_spacing();
    int shown_para = s_reader_slider >= 0 ? s_reader_preview_para : app_settings_book_paragraph_spacing();
    int shown_tracking = s_reader_slider >= 0 ? s_reader_preview_tracking : app_settings_book_tracking();
    draw_pill_slider(fb, reader_slider_rect(1), "小", "大", "边距", shown_margin - 24, 37, 21, 21);
    int line_i = shown_line - 110;
    if (line_i < 0) line_i = 0;
    if (line_i > 40) line_i = 40;
    draw_pill_slider(fb, reader_slider_rect(2), "紧", "松", "行距", line_i, 41, 21, 21);
    ui_text(fb, 42, 790, 19, "段距", EPD_DRAW_ALIGN_LEFT, false);
    char value[16]; snprintf(value, sizeof(value), "%d%%", shown_para);
    draw_pill_slider(fb, reader_slider_rect(3), "紧", "松", value, shown_para / 25, 4, 21, 21);
    ui_text(fb, 42, 909, 19, "字间距", EPD_DRAW_ALIGN_LEFT, false);
    static const char* track_names[] = {"-4", "-2", "默认", "+2", "+4"};
    draw_pill_slider(fb, reader_slider_rect(4), "紧", "松", track_names[shown_tracking], shown_tracking, 5, 21, 21);
    ui_text(fb, 42, 1034, 18, "首行默认缩进两字", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect back = {36, 1080, 612, 72};
    ui_fill_round_rect(fb, back, 20, 0xd8); ui_draw_round_rect(fb, back, 20, 0x70);
    ui_text_vc(fb, 342, 1116, 22, "返回字体设置", EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_refresh_settings(uint8_t* fb) {
    const int top = 560;
    draw_sheet(fb, top, "刷新设置");
    ui_text(fb, 42, 646, 22, "手动全刷", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect manual = {36, 680, 612, 112};
    ui_fill_round_rect(fb, manual, 22, 0xf0);
    ui_draw_round_rect(fb, manual, 22, 0x98);
    ui_text(fb, 58, 696, 26, "立即刷新整块屏幕", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 58, 748, 18, "包含状态栏、书名与底部进度", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect action = {510, 704, 117, 63};
    ui_fill_round_rect(fb, action, 21, UI_GRAY_BLACK);
    ui_text_vc(fb, 568, 735, 22, "全刷", EPD_DRAW_ALIGN_CENTER, true);

    ui_text(fb, 42, 816, 22, "自动全刷 · 阅读翻页", EPD_DRAW_ALIGN_LEFT, false);
    const int options[] = {5, 10, 15};
    for (int i = 0; i < 3; ++i) {
        EpdRect rect = {36 + i * 207, 850, 194, 78};
        bool selected = app_settings_reader_full_pages() == options[i];
        ui_fill_round_rect(fb, rect, 20, selected ? 0xd8 : UI_GRAY_WHITE);
        ui_draw_round_rect(fb, rect, 20, selected ? 0x48 : 0x98);
        char label[16]; snprintf(label, sizeof(label), "%d 页", options[i]);
        ui_text_vc(fb, rect.x + rect.width / 2, 889, 27, label,
                   EPD_DRAW_ALIGN_CENTER, false);
        if (selected) epd_fill_circle(rect.x + rect.width - 23, 870, 7, UI_GRAY_BLACK, fb);
    }
    ui_text(fb, 42, 955, 22, "翻页效果", EPD_DRAW_ALIGN_LEFT, false);
    static const char* effects[] = {"默认效果", "水波纹效果"};
    for (int i = 0; i < 2; ++i) {
        EpdRect rect = {36 + i * 312, 993, 300, 94};
        bool selected = app_settings_reader_turn_effect() == i;
        ui_fill_round_rect(fb, rect, 20, selected ? 0xd8 : UI_GRAY_WHITE);
        ui_draw_round_rect(fb, rect, 20, selected ? 0x48 : 0x98);
        ui_text_vc(fb, rect.x + rect.width / 2, 1040, 25, effects[i], EPD_DRAW_ALIGN_CENTER, false);
        if (selected) epd_fill_circle(rect.x + rect.width - 25, 1016, 7, UI_GRAY_BLACK, fb);
    }
    ui_text(fb, 42, 1101, 17, "默认保持现有刷新；水波纹仅用于阅读翻页。", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect done = {36, 1141, 612, 58};
    ui_fill_round_rect(fb, done, 20, UI_GRAY_BLACK);
    ui_text_vc(fb, 342, 1170, 22, "完成", EPD_DRAW_ALIGN_CENTER, true);
}

static void draw_font_picker(uint8_t* fb) {
    const int top = 584;
    draw_sheet(fb, top, "字体");
    epd_draw_line(55, top + 49, 64, top + 58, UI_GRAY_BLACK, fb);
    epd_draw_line(64, top + 58, 73, top + 49, UI_GRAY_BLACK, fb);
    int count = ttf_font_count();
    int pages = count > 0 ? (count + BOOK_FONT_PAGE - 1) / BOOK_FONT_PAGE : 1;
    char current[96];
    snprintf(current, sizeof(current), "当前选择 · %s  %d/%d", ttf_font_display_name(), s_font_page + 1, pages);
    fit_text(current, 20, 540);
    ui_text_vc(fb, 342, top + 91, 20, current, EPD_DRAW_ALIGN_CENTER, false);
    int start = s_font_page * BOOK_FONT_PAGE;
    char active[TTF_FONT_PATH_MAX]; copy_text(active, sizeof(active), ttf_font_path());
    bool active_builtin = ttf_font_is_builtin();
    for (int slot = 0; slot < BOOK_FONT_PAGE && start + slot < count; ++slot) {
        const ttf_font_item_t* item = ttf_font_item(start + slot);
        int row = slot / 3, col = slot % 3;
        EpdRect card = {36 + col * 204, top + 119 + row * 146, 180, 126};
        bool selected = !strcmp(active, item->path);
        ui_fill_round_rect(fb, card, 18, selected ? 0xc2 : 0xe4);
        ui_draw_round_rect(fb, card, 18, selected ? 0x48 : 0x98);
        const char* label = font_friendly_name(item);
        if (ttf_font_open(item->path) == ESP_OK) draw_font_name_with_current_face(fb, card.x + 90, card.y + 63, label);
        else ui_text_vc(fb, card.x + 90, card.y + 63, 25, label, EPD_DRAW_ALIGN_CENTER, false);
        if (selected) epd_fill_circle(card.x + 158, card.y + 22, 7, UI_GRAY_BLACK, fb);
    }
    if (active_builtin) (void)ttf_font_open_builtin();
    else if (active[0]) (void)ttf_font_open(active);
    if (!count) ui_text_vc(fb, 342, 780, 24, "所选字体目录中没有可用字体", EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_reader_stats(uint8_t* fb) {
    const int top = 768;
    draw_sheet(fb, top, "阅读统计");
    char values[3][32];
    snprintf(values[0], sizeof(values[0]), "%u%%", percent(s_page));
    snprintf(values[1], sizeof(values[1]), "%lu 分钟", (unsigned long)(s_session_read_ms / 60000));
    snprintf(values[2], sizeof(values[2]), "%lu 次", (unsigned long)s_session_turns);
    const char* labels[] = {"阅读进度", "本次阅读", "翻页"};
    for (int i = 0; i < 3; ++i) {
        int cx = 114 + i * 228;
        ui_text_vc(fb, cx, top + 126, 33, values[i], EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, cx, top + 169, 20, labels[i], EPD_DRAW_ALIGN_CENTER, false);
        if (i < 2) epd_fill_rect((EpdRect){228 + i * 228, top + 99, 1, 74}, 0x98, fb);
    }
    ui_text(fb, 54, top + 207, 21, "本书进度", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect track = {54, top + 245, 576, 12};
    ui_fill_round_rect(fb, track, 6, 0xd0);
    EpdRect fill = track; fill.width = fill.width * (int)percent(s_page) / 100;
    if (fill.width) ui_fill_round_rect(fb, fill, 6, 0x40);
    if (book_chapter_count() == 1) {
        char left[32], right[32];
        snprintf(left, sizeof(left), "第 %u 页", (unsigned)s_page + 1);
        snprintf(right, sizeof(right), "共 %u 页", (unsigned)book_layout_page_count());
        ui_text(fb, 54, top + 272, 19, left, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 630, top + 272, 19, right, EPD_DRAW_ALIGN_RIGHT, false);
    } else {
        ui_text(fb, 54, top + 272, 19, "当前页 —", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 630, top + 272, 19, "总页数 —", EPD_DRAW_ALIGN_RIGHT, false);
    }
    EpdRect details = {36, top + 319, 294, 78}, recent = {354, top + 319, 294, 78};
    ui_fill_round_rect(fb, details, 20, 0xd8); ui_draw_round_rect(fb, details, 20, 0x68);
    ui_fill_round_rect(fb, recent, 20, 0xd8); ui_draw_round_rect(fb, recent, 20, 0x68);
    char marks[32]; snprintf(marks, sizeof(marks), "查看书签 · %u", (unsigned)bookmark_count());
    ui_text_vc(fb, 183, top + 358, 24, marks, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 501, top + 358, 24, "最近 30 天", EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_reader_bookmarks(uint8_t* fb) {
    const int top = 560;
    draw_sheet(fb, top, "书签");
    epd_draw_line(55, top + 49, 64, top + 58, UI_GRAY_BLACK, fb);
    epd_draw_line(64, top + 58, 73, top + 49, UI_GRAY_BLACK, fb);
    reader_bookmarks_t marks = {0};
    bool valid = bookmark_load(&marks);
    int pages = valid && marks.count ? ((int)marks.count + BOOKMARK_ROWS - 1) / BOOKMARK_ROWS : 1;
    if (s_bookmark_page >= pages) s_bookmark_page = pages - 1;
    if (!valid || !marks.count) {
        ui_text_vc(fb, 342, top + 260, 26, "还没有保存书签", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, top + 309, 18, "阅读时点按“书签”即可保存当前位置", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    int first = s_bookmark_page * BOOKMARK_ROWS;
    for (int row = 0; row < BOOKMARK_ROWS && first + row < marks.count; ++row) {
        int newest = (int)marks.count - 1 - (first + row);
        const reader_bookmark_entry_t* mark = &marks.entries[newest];
        EpdRect item = {36, top + 96 + row * 82, 612, 72};
        bool current = mark->chapter == s_chapter && mark->byte_off == book_layout_page_start_offset(s_page);
        ui_fill_round_rect(fb, item, 14, current ? 0xc8 : 0xe8);
        ui_draw_round_rect(fb, item, 14, current ? 0x50 : 0x98);
        char chapter[96];
        if (book_chapter_title(mark->chapter, chapter, sizeof(chapter)) != ESP_OK)
            snprintf(chapter, sizeof(chapter), "第 %u 节", (unsigned)mark->chapter + 1);
        fit_text(chapter, 24, 410);
        ui_text_vc(fb, item.x + 18, item.y + 27, 24, chapter, EPD_DRAW_ALIGN_LEFT, false);
        char where[52];
        if (mark->chapter == s_chapter)
            snprintf(where, sizeof(where), "第 %u 页", (unsigned)book_layout_page_for_offset(mark->byte_off) + 1);
        else snprintf(where, sizeof(where), "章节 %u", (unsigned)mark->chapter + 1);
        ui_text_vc(fb, item.x + item.width - 20, item.y + 27, 20, where, EPD_DRAW_ALIGN_RIGHT, false);
        ui_text_vc(fb, item.x + 18, item.y + 55, 18,
                   current ? "当前阅读位置" : "点按跳转到此处", EPD_DRAW_ALIGN_LEFT, false);
    }
    char page[32]; snprintf(page, sizeof(page), "‹   %d / %d   ›", s_bookmark_page + 1, pages);
    ui_text_vc(fb, 342, top + 606, 22, page, EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_reader_stats_recent(uint8_t* fb) {
    const int top = 584;
    draw_sheet(fb, top, "最近 30 天");
    epd_draw_line(55, top + 49, 64, top + 58, UI_GRAY_BLACK, fb);
    epd_draw_line(64, top + 58, 73, top + 49, UI_GRAY_BLACK, fb);
    if (!s_recent_days_valid) {
        ui_text_vc(fb, 342, top + 280, 23, "请先完成日期与时间设置", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, top + 326, 18, "完成对时后开始记录每天的阅读时长", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    uint64_t sum = 0;
    uint32_t maximum = 0;
    for (int i = 0; i < 30; ++i) {
        sum += s_recent_days[i];
        if (s_recent_days[i] > maximum) maximum = s_recent_days[i];
    }
    char total[64];
    snprintf(total, sizeof(total), "累计 %llu 小时 %llu 分钟",
             (unsigned long long)(sum / 3600), (unsigned long long)((sum / 60) % 60));
    ui_text_vc(fb, 342, top + 112, 28, total, EPD_DRAW_ALIGN_CENTER, false);
    const int base_y = top + 438, max_h = 250;
    ui_hairline(fb, base_y, 42, 600, 0xb0);
    for (int i = 0; i < 30; ++i) {
        int h = maximum ? (int)((uint64_t)s_recent_days[i] * max_h / maximum) : 0;
        if (s_recent_days[i] && h < 5) h = 5;
        if (h) ui_fill_round_rect(fb, (EpdRect){47 + i * 20, base_y - h, 10, h}, 5, 0x38);
    }
    ui_text(fb, 42, base_y + 22, 19, "30 天前", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 642, base_y + 22, 19, "今天", EPD_DRAW_ALIGN_RIGHT, false);
}

static void draw_reader_panel(uint8_t* fb) {
    if (s_reader_panel == READER_PANEL_TOOLS) {
        epd_fill_rect((EpdRect){0, 1096, UI_LOCK_WIDTH, 120}, UI_GRAY_WHITE, fb);
        ui_hairline(fb, 1096, 0, UI_LOCK_WIDTH, 0x98);
        const char* labels[] = {"目录", "书签", "阅读统计", "刷新设置", "字体设置"};
        for (int i = 0; i < BOOK_TOOL_COUNT; ++i) {
            EpdRect rect = tool_rect(i);
            int cx = rect.x + rect.width / 2;
            draw_reader_tool_icon(fb, cx, 1138, i, i == 1 && bookmark_is_current());
            ui_text_vc(fb, cx, 1179, 20, labels[i], EPD_DRAW_ALIGN_CENTER, false);
        }
    } else if (s_reader_panel == READER_PANEL_FONT_SETTINGS) draw_font_settings(fb);
    else if (s_reader_panel == READER_PANEL_LAYOUT_SETTINGS) draw_layout_settings(fb);
    else if (s_reader_panel == READER_PANEL_FONT_PICKER) draw_font_picker(fb);
    else if (s_reader_panel == READER_PANEL_STATS) draw_reader_stats(fb);
    else if (s_reader_panel == READER_PANEL_BOOKMARKS) draw_reader_bookmarks(fb);
    else if (s_reader_panel == READER_PANEL_STATS_RECENT) draw_reader_stats_recent(fb);
    else if (s_reader_panel == READER_PANEL_REFRESH_SETTINGS) draw_refresh_settings(fb);
}

/* ---- 绘制与预渲染 / Drawing and preparation ---- */
static uint8_t inline_ink_gray(uint8_t gray) {
    if (gray >= 252) return 0xff;
    unsigned ink = (255u - gray) * 7u / 4u + 7u;
    if (ink > 255u) ink = 255u;
    return ui_contrast_gray((uint8_t)(255u - ink));
}

static void draw_reader(uint8_t* fb, size_t page) {
    ui_clear_page(fb);
    if (!(s_reader_fullscreen && app_settings_reader_immersive())) ui_nav_status(fb);
    if (!s_reader_fullscreen) {
        char header[128];
        copy_text(header, sizeof(header), s_book_title[0] ? s_book_title : s_title);
        fit_text(header, 30, 574);
        ui_text(fb, UI_LOCK_WIDTH / 2, 94, 30, header, EPD_DRAW_ALIGN_CENTER, false);
        ui_hairline(fb, 151, 36, 612, UI_GRAY_LIGHT);
    }
    EpdRect body = body_rect();
    book_layout_draw_page(fb, page, body, s_px);
    if (page == 0 && s_chapter_lead_height && s_chapter_heading_title[0]) {
        char heading[128]; copy_text(heading, sizeof(heading), s_chapter_heading_title);
        fit_text(heading, 49, ui_content_width());
        ui_text(fb, UI_LOCK_WIDTH / 2, body.y + 15, 27, s_chapter_heading_label, EPD_DRAW_ALIGN_CENTER, false);
        ui_text(fb, UI_LOCK_WIDTH / 2, body.y + 87, 49, heading, EPD_DRAW_ALIGN_CENTER, false);
        ui_hairline(fb, body.y + 174, 210, 264, UI_GRAY_LIGHT);
    }
    if (book_layout_page_image(page) == s_inline_index && s_inline_gray) {
        int left = body.x + (body.width - (int)s_inline_w) / 2;
        int top = body.y + (body.height - (int)s_inline_h) / 2;
        for (unsigned y = 0; y < s_inline_h; ++y)
            for (unsigned x = 0; x < s_inline_w; ++x)
                epd_draw_pixel(left + x, top + y,
                    ui_image_dither_gray(inline_ink_gray(s_inline_gray[y * s_inline_w + x]),
                                         left + (int)x, top + (int)y), fb);
    } else if (book_layout_page_image(page) >= 0) {
        ui_text_vc(fb, body.x + body.width / 2, body.y + body.height / 2,
                   UI_PX_CAPTION, "此插图暂无法显示", EPD_DRAW_ALIGN_CENTER, false);
    }
    if (!s_reader_fullscreen) {
        EpdRect track = progress_rect();
        EpdRect bar = {track.x, track.y + 16, track.width, 12};
        ui_fill_round_rect(fb, bar, 5, UI_GRAY_LIGHT);
        bar.width = bar.width * (int)percent(page) / 100;
        if (bar.width) ui_fill_round_rect(fb, bar, 5, UI_GRAY_BLACK);
        char chapter_name[72] = {0};
        reader_footer_chapter_name(chapter_name, sizeof(chapter_name));
        const int footer_px = 24;
        char progress[48];
        snprintf(progress, sizeof(progress), "%u/%u · 全书 %u%%",
                 (unsigned)page + 1, (unsigned)book_layout_page_count(), percent(page));
        char chapter_line[192];
        snprintf(chapter_line, sizeof(chapter_line), "%s%s",
                 s_save_failed ? "未保存 · " : "", chapter_name);
        fit_fixed_text(chapter_line, footer_px,
                       track.width - ui_text_fixed_width_px(footer_px, progress) - 16);
        // 页码和全书进度靠右固定，长章节名仅截短左侧；字号不跟随正文设置。
        // Keep page and book progress right-aligned; trim only long chapter titles and ignore body font settings.
        ui_text_fixed(fb, track.x, track.y + 35, footer_px, chapter_line, EPD_DRAW_ALIGN_LEFT, false);
        ui_text_fixed(fb, track.x + track.width, track.y + 35, footer_px,
                      progress, EPD_DRAW_ALIGN_RIGHT, false);
    }
    if (s_reader_notice[0]) {
        EpdRect notice = {218, 160, 248, 48};
        ui_fill_round_rect(fb, notice, 24, 0xd0);
        ui_text_vc(fb, 342, 184, 19, s_reader_notice, EPD_DRAW_ALIGN_CENTER, false);
    }
    draw_reader_panel(fb);
}
static void prep_task(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        lock_draw();
        if (s_prep_page >= 0 && s_text && s_next_fb) {
            draw_reader(s_next_fb, (size_t)s_prep_page);
            s_next_page = s_prep_page;
        }
        unlock_draw();
        xSemaphoreGive(s_prep_done);
    }
}
static void ensure_prep(void) {
    if (!s_draw_lock) s_draw_lock = xSemaphoreCreateMutex();
    if (!s_prep_done) s_prep_done = xSemaphoreCreateBinary();
    if (!s_next_fb) s_next_fb = heap_caps_aligned_alloc(16, fb_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prep_task && s_draw_lock && s_prep_done && s_next_fb) {
        if (xTaskCreatePinnedToCore(prep_task, "book_prep", 12 * 1024, NULL, 3, &s_prep_task, 1) != pdPASS)
            s_prep_task = NULL;
    }
}
static bool kick_prep(void) {
    if (!s_prep_task || s_view != READING || s_toolbar || s_clear_confirm ||
        s_page + 1 >= book_layout_page_count() || s_next_page == (int)s_page + 1 ||
        book_layout_page_image(s_page + 1) >= 0) return false;
    s_prep_page = (int)s_page + 1;
    xSemaphoreTake(s_prep_done, 0);
    xTaskNotifyGive(s_prep_task);
    return true;
}
static size_t current_toc_position(void) {
    size_t count = book_navigation_count();
    if (s_selected_toc < count && book_navigation_chapter(s_selected_toc) == s_chapter)
        return s_selected_toc;
    size_t selected = SIZE_MAX;
    size_t best_chapter = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t chapter = book_navigation_chapter(i);
        if (chapter > s_chapter) break;
        if (selected == SIZE_MAX || chapter > best_chapter) { selected = i; best_chapter = chapter; }
    }
    return selected;
}
static void render(app_ctx_t* ctx, uint8_t* fb) {
    ui_text_set_system_scale(s_view != READING && s_view != TOC);
    lock_draw();
    if (s_view == EDIT) { draw_editor(fb); unlock_draw(); return; }
    if (s_view == SEARCH) { draw_search(fb); unlock_draw(); return; }
    if (s_view == TOC) {
        book_toc_render(fb, s_book_title[0] ? s_book_title : s_title,
                        book_navigation_count(), current_toc_position(), ctx->leaf, s_message);
        unlock_draw();
        return;
    }
    if (s_view == READING && s_text) {
        draw_reader(fb, s_page);
    } else {
        ui_clear_page(fb);
        if (s_view == SHELF) {
            epd_fill_rect((EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_TOP}, 0xe0, fb);
            ui_nav_status(fb);
            ui_text(fb, 36, 90, 52, "书架", EPD_DRAW_ALIGN_LEFT, false);
        } else {
            ui_nav_status(fb);
            ui_nav_back(fb, 36, 79);
            ui_text_vc(fb, 342, 107, 34,
                       s_view == BULK ? "批量管理" : "图书详情",
                       EPD_DRAW_ALIGN_CENTER, false);
            ui_text(fb, 342, 151, 20, s_storage,
                    EPD_DRAW_ALIGN_CENTER, false);
            ui_hairline(fb, 176, 36, 612, UI_GRAY_LIGHT);
        }
        if (s_view == SHELF) {
            EpdRect manage = shelf_manage_rect();
            ui_fill_round_rect(fb, manage, 18, UI_GRAY_WHITE);
            ui_draw_round_rect(fb, manage, 18, 0xd0);
            ui_text_vc(fb, manage.x + manage.width / 2, manage.y + manage.height / 2,
                       20, "管理", EPD_DRAW_ALIGN_CENTER, false);
            EpdRect import = shelf_import_rect();
            ui_fill_round_rect(fb, import, 18, UI_GRAY_WHITE);
            ui_draw_round_rect(fb, import, 18, 0xd0);
            ui_text_vc(fb, import.x + import.width / 2, import.y + import.height / 2,
                       20, "+ 导入", EPD_DRAW_ALIGN_CENTER, false);
            ui_hairline(fb, 195, 36, 612, UI_GRAY_LIGHT);
            if (s_shelf_warning[0]) ui_text(fb, UI_MARGIN, 1023, 17, s_shelf_warning, EPD_DRAW_ALIGN_LEFT, false);
        } else {
            draw_control(fb, ui_row_rect(0, 3, 176, 72), s_filter == 0 ? "全部来源" : s_filter == 1 ? "TF 卡" : "内置", 110);
            draw_control(fb, ui_row_rect(1, 3, 176, 72), s_recent_sort ? "按最近" : "按名称", 111);
            draw_control(fb, ui_row_rect(2, 3, 176, 72), s_query[0] ? "搜索中" : "搜索", 112);
            char shelf_hint[128];
            snprintf(shelf_hint, sizeof(shelf_hint), "已选 %u 本 · 筛选/重扫清勾选", (unsigned)selected_count());
            ui_text(fb, UI_MARGIN, 264, UI_PX_CAPTION, s_view == BULK && s_batch_message[0] ? s_batch_message : s_message[0] ? s_message : s_view == BULK ? shelf_hint : s_shelf_warning, EPD_DRAW_ALIGN_LEFT, false);
            if (s_view != BULK) ui_text(fb, UI_MARGIN, UI_CONTENT_BOTTOM - UI_PX_CAPTION, UI_PX_CAPTION,
                    s_save_failed ? "进度未保存：点此重试" : "点击打开 · 长按查看详情和管理", EPD_DRAW_ALIGN_LEFT, false);
        }
        if (!s_visible_count && s_count)
            ui_text(fb, UI_MARGIN, 308, UI_PX_CAPTION, "当前筛选没有图书", EPD_DRAW_ALIGN_LEFT, false);
        if (!s_message[0] || s_visible_count) {
            int rows = s_view == BULK ? BOOK_BULK_ROWS : BOOK_ROWS;
            int count = s_visible_count;
            for (int row = 0; row < rows; ++row) {
                int i = ctx->leaf * rows + row;
                if (i >= count) break;
                EpdRect r = row_rect(row);
                char name[128], mark[32] = "";
                copy_text(name, sizeof(name), s_shelf[i].name);
                if (s_shelf[i].has_progress) snprintf(mark, sizeof(mark), "%s %u%%", s_shelf[i].is_flash ? "内置" : "", s_shelf[i].pct);
                else if (s_shelf[i].is_flash) copy_text(mark, sizeof(mark), "内置");
                if (s_view == BULK) snprintf(mark, sizeof(mark), "%s", s_shelf[i].selected ? "已选" : "未选");
                if (s_shelf[i].removed) snprintf(mark, sizeof(mark), "%s待清理", s_view == BULK && s_shelf[i].selected ? "已选·" : "已删·");
                if (s_view == SHELF || s_view == MANAGE) {
                    draw_shelf_cover(fb, r, row, name);
                    if (s_view != SHELF) draw_shelf_title(fb, r, name);
                    if (s_view == BULK && s_shelf[i].selected) ui_draw_selected_round_rect(fb, r, 0);
                } else {
                    fit_text(name, UI_PX_BTN, r.width - 2 * UI_PAD - ttf_text_width_px(UI_PX_CAPTION, mark) - UI_GAP);
                    if (s_pressed_control == row) ui_draw_pressed_round_rect(fb, r, UI_BTN_RADIUS);
                    ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
                    if (i == (int)s_chapter) ui_draw_selected_round_rect(fb, r, UI_BTN_RADIUS);
                    ui_text_vc(fb, r.x + UI_PAD, r.y + r.height / 2, UI_PX_BTN, name, EPD_DRAW_ALIGN_LEFT, false);
                    ui_text_vc(fb, r.x + r.width - UI_PAD, r.y + r.height / 2, UI_PX_CAPTION, mark, EPD_DRAW_ALIGN_RIGHT, false);
                }
            }
        }
        if (s_view == SHELF || s_view == MANAGE) draw_shelf_furniture(fb);
        if (s_view == SHELF) {
            char count[64];
            snprintf(count, sizeof(count), "%d本书 · %02d/%02d", s_visible_count, ctx->leaf + 1, leaves());
            ui_text(fb, 342, 1053, 17, count, EPD_DRAW_ALIGN_CENTER, false);
            ui_nav_draw(fb, 1);
        }
        if (s_view != SHELF) {
            draw_control(fb, ui_bar_rect(0, 3), "上一页", 100);
            draw_control(fb, ui_bar_rect(1, 3), "返回书架", 101);
            draw_control(fb, ui_bar_rect(2, 3), "下一页", 102);
        }
        if (s_view == BULK) {
            const char* labels[] = {"本页全选", "清除勾选", "重新扫描", "删除所选", "清进度"};
            for (int i = 0; i < 5; ++i) draw_control(fb, batch_rect(i), labels[i], 610 + i);
            draw_batch_confirmation(fb);
        }
        if (s_view != SHELF) ui_draw_menu_handle(fb, false);
        if (s_view == MANAGE) draw_manage(fb);
    }
    unlock_draw();
}
static bool present(app_ctx_t* ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_NONE || redraw == APP_REDRAW_DONE) return true;
    if (s_presented_view != (int)s_view && redraw == APP_REDRAW_AREA) redraw = APP_REDRAW_PAGE;
    if (s_view == SHELF || s_view == MANAGE) prepare_covers(ctx);
    int64_t start = esp_timer_get_time();
    if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL) render(ctx, ctx->fb);
    bool prep = kick_prep();
    int64_t drawn = esp_timer_get_time();
    enum EpdDrawError err;
    if (redraw == APP_REDRAW_FULL || s_reader_cleanup) err = update_display_full(ctx->hl);
    else if (redraw == APP_REDRAW_AREA) {
        err = s_reader_split && s_water_turn_pending
            ? update_display_water_turn(ctx->hl, s_area, s_water_turn_dir)
            : update_display_area_with(ctx->hl, &E0470_WAVEFORM, s_mode, s_area);
        if (s_reader_split) {
            // 普通翻页页脚只驱动变化像素；全刷由上方整屏分支一次完成。
            // Ordinary turns drive only changed footer pixels; the full-screen branch handles cleanup at once.
            err = (enum EpdDrawError)(err | update_display_area_with(ctx->hl, &E0470_FOLLOW_WAVEFORM,
                                                                     MODE_DU, progress_rect()));
            ESP_LOGI(TAG, "reader regions body_h=%d footer=diff pct=%u", s_area.height, percent(s_page));
        }
    }
    else err = s_view == READING
        ? update_display_mode_diff(ctx->hl, APP_PAGE_REFRESH_MODE)
        : update_display_fast_page(ctx->hl);
    int64_t displayed = esp_timer_get_time();
    if (prep) xSemaphoreTake(s_prep_done, portMAX_DELAY);
    if (redraw == APP_REDRAW_AREA && s_mode == MODE_DU && !s_reader_cleanup) {
        s_du_area = s_du_count ? ui_rect_union(s_du_area, s_area) : s_area;
        ++s_du_count;
        s_du_ms = esp_timer_get_time() / 1000;
    } else s_du_count = 0;
    ESP_LOGI(TAG, "present draw=%lld display=%lld join=%lld ms", (drawn - start) / 1000,
             (displayed - drawn) / 1000, (esp_timer_get_time() - displayed) / 1000);
    guard_draw_result(ctx->hl, err);
    s_reader_cleanup = false;
    s_presented_view = (int)s_view;
    s_reader_split = false;
    s_water_turn_pending = false;
    s_mode = MODE_GL16;
    return true;
}
static app_redraw_t paint_reading(app_ctx_t* ctx, enum EpdDrawMode mode) {
    s_water_turn_pending = false;
    int64_t started = esp_timer_get_time();
    prepare_inline_image();
    lock_draw();
    bool cached = !s_toolbar && !s_clear_confirm && s_next_fb && s_next_page == (int)s_page;
    if (cached)
        memcpy(ctx->fb, s_next_fb, fb_bytes());
    else draw_reader(ctx->fb, s_page);
    unlock_draw();
    ESP_LOGI(TAG, "paint cached=%d ms=%lld", cached, (esp_timer_get_time() - started) / 1000);
    s_area = reader_area();
    s_reader_split = !s_reader_fullscreen;
    s_mode = mode;
    return APP_REDRAW_AREA;
}

/* ---- 文件与进度 / Files and progress ---- */
static void flush_ticket_stats(void) {
    uint32_t seconds = s_stats_pending_ms / 1000;
    if (!seconds && !s_stats_pending_turns) return;
    if (book_ticket_record(seconds, s_stats_pending_turns) == ESP_OK) {
        s_stats_pending_ms -= seconds * 1000;
        s_stats_pending_turns = 0;
    }
}
static void track_ticket_stats(app_ctx_t* ctx) {
    if (s_view != READING || !s_text) { s_stats_last_ms = ctx->now_ms; return; }
    if (s_stats_last_ms > 0 && ctx->now_ms >= s_stats_last_ms &&
        ctx->now_ms - s_stats_last_ms <= 5000 &&
        ctx->now_ms - s_stats_activity_ms <= 300000 && !s_toolbar) {
        uint32_t elapsed = (uint32_t)(ctx->now_ms - s_stats_last_ms);
        s_stats_pending_ms += elapsed;
        s_session_read_ms += elapsed;
    }
    s_stats_last_ms = ctx->now_ms;
    if (s_stats_pending_ms >= 30000) flush_ticket_stats();
}
static void free_book(void) {
    flush_ticket_stats();
    e0470_page_turn_release();
    s_water_turn_pending = false;
    pending_progress_t* pending = pending_find(s_path);
    if (pending && !pending->dirty) pending_discard(s_path);
    invalidate_prep();
    book_layout_free();
    book_layout_set_chapter_lead(0, 0);
    s_chapter_lead_skip = s_chapter_lead_height = 0;
    s_chapter_heading_title[0] = s_chapter_heading_label[0] = 0;
    free(s_text);
    free(s_blocks);
    for (size_t i = 0; i < s_image_count; ++i) free(s_images[i]);
    free(s_images);
    s_images = NULL; s_image_count = 0;
    free(s_inline_gray); s_inline_gray = NULL; s_inline_index = -1;
    s_blocks = NULL;
    s_block_count = 0;
    s_text = NULL;
    s_text_len = 0;
    s_selected_toc = SIZE_MAX;
    s_jump_offset = SIZE_MAX;
    book_close();
    s_path[0] = 0;
    s_book_title[0] = 0;
    app_font_activate_system();
}
static void prepare_inline_image(void) {
    int index = book_layout_page_image(s_page);
    if (index < 0 || index == s_inline_index || (size_t)index >= s_image_count) return;
    invalidate_prep();
    free(s_inline_gray); s_inline_gray = NULL;
    s_inline_index = index;
    uint8_t *encoded = NULL; size_t size = 0; bool png = false;
    esp_err_t image_err = book_chapter_image(s_chapter, s_images[index], &encoded, &size, &png);
    if (image_err != ESP_OK) {
        ESP_LOGW(TAG, "inline image load chapter=%u index=%d: %s", (unsigned)s_chapter,
                 index, esp_err_to_name(image_err));
        return;
    }
    unsigned width = 0, height = 0;
    EpdRect body = body_rect();
    if (book_image_dimensions(encoded, size, png, &width, &height)) {
        double scale_x = (double)body.width / width, scale_y = (double)body.height / height;
        double scale = scale_x < scale_y ? scale_x : scale_y;
        if (scale > 1.0) scale = 1.0;
        unsigned out_w = (unsigned)(width * scale), out_h = (unsigned)(height * scale);
        if (!out_w) out_w = 1;
        if (!out_h) out_h = 1;
        uint8_t *gray = heap_caps_malloc((size_t)out_w * out_h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (gray && book_image_grayscale(encoded, size, png, out_w, out_h, gray)) {
            s_inline_gray = gray; s_inline_w = out_w; s_inline_h = out_h;
        } else { free(gray); ESP_LOGW(TAG, "inline image decode chapter=%u index=%d", (unsigned)s_chapter, index); }
    } else ESP_LOGW(TAG, "inline image dimensions chapter=%u index=%d", (unsigned)s_chapter, index);
    free(encoded);
}
static bool load_chapter_at(app_ctx_t* ctx, size_t chapter, size_t offset,
                            bool last_page, const char *anchor, size_t source_offset) {
    if (chapter != s_chapter || (!anchor && source_offset == SIZE_MAX)) s_selected_toc = SIZE_MAX;
    html_text_t loaded = {0};
    size_t anchor_offset = 0;
    esp_err_t err = book_chapter_load_blocks_target(chapter, anchor, source_offset,
                                                    &anchor_offset, &loaded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "chapter load failed: %s", esp_err_to_name(err));
        copy_text(s_message, sizeof(s_message), "加载失败，请检查存储卡后重试");
        return false;
    }
    char heading[128] = {0}, label[32] = {0}, toc_title[128] = {0};
    size_t lead_skip = 0;
    unsigned lead_height = 0;
    if (book_kind() == BOOK_KIND_EPUB && loaded.len) {
        // 已识别的正文编号题头优先；没有时沿用书内导航或备用标题。
        // Prefer a recognized numbered body heading, then authored navigation or a fallback title.
        if (book_chapter_title(chapter, toc_title, sizeof(toc_title)) != ESP_OK)
            snprintf(toc_title, sizeof(toc_title), "第 %u 节", (unsigned)chapter + 1);
        copy_text(heading, sizeof(heading), toc_title);
        char *chapter_mark = strstr(heading, "章");
        if (chapter_mark && chapter_mark - heading < 24) {
            char *after = chapter_mark + strlen("章");
            while (*after == ' ' || *after == '\t' || *after == ':' || *after == '-' ||
                   !strncmp(after, "：", strlen("：")) || !strncmp(after, "、", strlen("、"))) {
                if (*after == ' ' || *after == '\t' || *after == ':' || *after == '-') ++after;
                else after += strlen("：");
            }
            if (*after && (size_t)(chapter_mark + strlen("章") - heading) < sizeof(label)) {
                size_t label_len = (size_t)(chapter_mark + strlen("章") - heading);
                memcpy(label, heading, label_len); label[label_len] = 0;
                memmove(heading, after, strlen(after) + 1);
            }
        }
        size_t skip_blocks = 0;
        while (skip_blocks < loaded.count && skip_blocks < 2 &&
               loaded.blocks[skip_blocks].heading && loaded.blocks[skip_blocks].image < 0) {
            char body_heading[128] = {0};
            size_t len = loaded.blocks[skip_blocks].len;
            if (len >= sizeof(body_heading)) break;
            memcpy(body_heading, loaded.utf8 + loaded.blocks[skip_blocks].offset, len);
            while (len && (body_heading[len - 1] == ' ' || body_heading[len - 1] == '\t')) body_heading[--len] = 0;
            const char *trimmed = body_heading;
            while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
            if (strcmp(trimmed, toc_title) && strcmp(trimmed, heading) &&
                (!label[0] || strcmp(trimmed, label))) break;
            ++skip_blocks;
        }
        if (skip_blocks) lead_skip = skip_blocks < loaded.count ? loaded.blocks[skip_blocks].offset : loaded.len;
        if (skip_blocks < loaded.count && loaded.blocks[skip_blocks].image >= 0) lead_skip = 0;
        else
            lead_height = body_rect().height > 214 ? 214 : 0;
    }
    size_t old_lead_skip = s_chapter_lead_skip;
    unsigned old_lead_height = s_chapter_lead_height;
    lock_draw();
    invalidate_prep();
    book_layout_set_chapter_lead(lead_skip, lead_height);
    bool ok = book_layout_build_blocks(loaded.utf8, loaded.len, loaded.blocks, loaded.count, body_rect(), s_px);
    if (!ok) {
        html_text_free(&loaded);
        book_layout_set_chapter_lead(old_lead_skip, old_lead_height);
        bool restored = s_text && book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
        unlock_draw();
        if (!restored) {
            free_book();
            s_view = SHELF;
            ctx->leaf = 0;
        }
        copy_text(s_message, sizeof(s_message), "排版失败：内存不足或章节过长");
        return false;
    }
    free(s_text);
    free(s_blocks);
    s_text = loaded.utf8;
    s_text_len = loaded.len;
    s_blocks = loaded.blocks;
    s_block_count = loaded.count;
    for (size_t i = 0; i < s_image_count; ++i) free(s_images[i]);
    free(s_images);
    s_images = loaded.images; s_image_count = loaded.image_count;
    free(s_inline_gray); s_inline_gray = NULL; s_inline_index = -1;
    s_chapter = chapter;
    s_chapter_lead_skip = lead_skip;
    s_chapter_lead_height = lead_height;
    copy_text(s_chapter_heading_title, sizeof(s_chapter_heading_title), heading);
    copy_text(s_chapter_heading_label, sizeof(s_chapter_heading_label), label);
    s_page = last_page ? book_layout_page_count() - 1 :
             book_layout_page_for_offset(anchor || source_offset != SIZE_MAX ? anchor_offset : offset);
    s_jump_offset = anchor || source_offset != SIZE_MAX ? anchor_offset : SIZE_MAX;
    s_jump_page = s_page;
    if (book_chapter_title(chapter, s_title, sizeof(s_title)) != ESP_OK)
        snprintf(s_title, sizeof(s_title), "第 %u 节", (unsigned)chapter + 1);
    copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
    s_message[0] = 0;
    unlock_draw();
    prepare_inline_image();
    return true;
}
static bool load_chapter(app_ctx_t* ctx, size_t chapter, size_t offset, bool last_page) {
    return load_chapter_at(ctx, chapter, offset, last_page, NULL, SIZE_MAX);
}
static bool open_book(app_ctx_t* ctx, const char* path) {
    s_reader_return_home = false;
    if (!strncmp(path, "/sdcard/", 8)) {
        read_pico_sd_info_t sd = {0};
        read_pico_sd_get_info(&sd);
        if (!sd.present || !sd.mounted) {
            copy_text(s_message, sizeof(s_message), "TF 卡不可用，请重新挂载后打开");
            return false;
        }
    }
    // 自动续读也不能绕过同路径旧文件的清理重试。
    // Automatic resume must also finish cleanup of the old file at this path.
    if (delete_retry_find(path)) {
        copy_text(s_message, sizeof(s_message), "文件已删除，进度清理失败，请重试");
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) return false;
    if (strncmp(path, "/flash/", 7) == 0 && st.st_size > BOOK_STORE_FLASH_FILE_MAX) {
        copy_text(s_message, sizeof(s_message), "内置存储单本限 1 MB");
        return false;
    }
    if (!pending_reserve(path)) {
        copy_text(s_message, sizeof(s_message), "内存不足，无法保留待保存进度");
        return false;
    }
    save_progress();
    free_book();
    s_reader_fullscreen = false;
    app_font_activate_reading();
    if (!pending_reserve(path)) {
        copy_text(s_message, sizeof(s_message), "内存不足，无法打开图书");
        app_font_activate_system();
        return false;
    }
    esp_err_t err = book_open(path);
    if (err != ESP_OK) {
        pending_progress_t* pending = pending_find(path);
        if (pending && !pending->dirty) pending_discard(path);
        copy_text(s_message, sizeof(s_message), err == ESP_ERR_NOT_SUPPORTED ? "文件格式或压缩方式暂不支持" : "无法打开图书，请检查文件");
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        app_font_activate_system();
        return false;
    }
    copy_text(s_path, sizeof(s_path), path);
    const char* filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    if (!book_title_from_path(path, s_book_title, sizeof(s_book_title)))
        clean_filename(s_book_title, sizeof(s_book_title), filename);
    s_file_size = (uint32_t)st.st_size;
    book_progress_t p = {0};
    bool resume = pending_restore(path, s_file_size, &p) || book_progress_load(path, s_file_size, &p);
    s_px = resume ? p.px : app_settings_book_px();
    if (s_px < BOOK_PX_MIN || s_px > BOOK_PX_MAX) s_px = 48;
    size_t chapter = resume && p.chapter < book_chapter_count() ? p.chapter : 0;
    if (!load_chapter(ctx, chapter, resume && p.chapter == chapter ? p.byte_off : 0, false)) {
        free_book();
        return false;
    }
    s_view = READING;
    s_stats_last_ms = s_stats_activity_ms = ctx->now_ms;
    s_reader_panel = READER_PANEL_NONE;
    s_clear_confirm = s_batch_confirm = false;
    s_session_read_ms = s_session_turns = 0;
    s_reader_notice[0] = 0;
    s_turns = s_unsaved = 0;
    pending_mark_latest(s_path);
    save_progress();
    ESP_LOGI(TAG, "opened kind=%d chapters=%u pages=%u px=%d", book_kind(), (unsigned)book_chapter_count(), (unsigned)book_layout_page_count(), s_px);
    return true;
}
static bool open_requested_book(app_ctx_t* ctx) {
    if (!s_requested_open[0]) return false;
    char path[BOOK_STORE_PATH_MAX];
    copy_text(path, sizeof(path), s_requested_open);
    bool from_home = s_requested_open_home;
    s_requested_open[0] = 0;
    s_requested_open_home = false;
    if (!open_book(ctx, path)) return false;
    s_reader_return_home = from_home;
    return true;
}
/* ---- 书架数据 / Shelf data ---- */
static bool shelf_matches(const shelf_entry_t* item) {
    return item->search_match && (s_filter == 0 || (s_filter == 1 && !item->is_flash) || (s_filter == 2 && item->is_flash));
}
static int compare_books(const void* a, const void* b) {
    const shelf_entry_t* x = a;
    const shelf_entry_t* y = b;
    if (shelf_matches(x) != shelf_matches(y)) return shelf_matches(x) ? -1 : 1;
    if (s_recent_sort && x->recent != y->recent) return x->recent > y->recent ? -1 : 1;
    int name = strcasecmp(x->name, y->name);
    return name ? name : strcmp(x->path, y->path);
}
static void sort_shelf(app_ctx_t* ctx) {
    invalidate_covers();
    if (s_count > 1) qsort(s_shelf, s_count, sizeof(*s_shelf), compare_books);
    s_visible_count = 0;
    while (s_visible_count < s_count && shelf_matches(&s_shelf[s_visible_count])) ++s_visible_count;
    ctx->leaf = 0;
}
static bool shelf_reserve(void) {
    if ((size_t)s_count < s_shelf_capacity) return true;
    if (s_count == INT_MAX) return false;
    size_t cap = s_shelf_capacity ? s_shelf_capacity * 2 : 32;
    if (cap > INT_MAX || cap > SIZE_MAX / sizeof(*s_shelf)) return false;
    shelf_entry_t* entries = heap_caps_realloc(s_shelf, cap * sizeof(*entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) return false;
    s_shelf = entries;
    s_shelf_capacity = cap;
    return true;
}

// 在删除文件前预留记录，避免删除成功后才遇到内存不足。
// Reserve before unlink so allocation failure never loses a completed deletion's retry.
static delete_retry_t* delete_retry_reserve(const shelf_entry_t* entry) {
    delete_retry_t* existing = delete_retry_find(entry->path);
    if (existing) return existing;
    delete_retry_t* p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(*p));
    if (!p) return NULL;
    p->entry = *entry;
    p->next = s_delete_retries;
    s_delete_retries = p;
    return p;
}
static void delete_retry_discard(const char* path) {
    delete_retry_t** p = &s_delete_retries;
    while (*p) {
        if (!strcmp((*p)->entry.path, path)) {
            delete_retry_t* old = *p;
            *p = old->next;
            free(old);
            return;
        }
        p = &(*p)->next;
    }
}
static void scan_shelf(app_ctx_t* ctx) {
    s_count = 0;
    s_visible_count = 0;
    s_message[0] = 0;
    s_shelf_warning[0] = 0;
    // 内置分区每次启动都必须重新挂载；已有文件不会因此格式化。
    // Flash mounts on each boot; existing files are not formatted by that mount.
    book_store_root_t roots[BOOK_STORE_ROOT_MAX];
    int n = 0;
    esp_err_t root_err = book_store_roots(roots, &n);
    bool truncated = false, unreadable = root_err != ESP_OK || book_store_roots_degraded(), skipped = false;
    s_storage[0] = 0;
    for (int i = 0; i < n; ++i) {
        if (i == 0 || roots[i].is_flash || roots[i - 1].is_flash) {
            char capacity[64];
            snprintf(capacity, sizeof(capacity), "%s%s %.1f MB", s_storage[0] ? " · " : "",
                     roots[i].is_flash ? "内置余" : "TF余", book_store_free_bytes(&roots[i]) / 1048576.0);
            strncat(s_storage, capacity, sizeof(s_storage) - strlen(s_storage) - 1);
        }
        DIR* dir = opendir(roots[i].path);
        if (!dir) { unreadable = true; continue; }
        struct dirent* ent;
        for (;;) {
            errno = 0;
            ent = readdir(dir);
            if (!ent) { if (errno) unreadable = true; break; }
            // macOS AppleDouble sidecars (._book.epub) and other hidden files are not books.
            if (ent->d_name[0] == '.') continue;
            const char* ext = strrchr(ent->d_name, '.');
            if (!ext || (strcasecmp(ext, ".txt") && strcasecmp(ext, ".epub"))) continue;
            shelf_entry_t candidate = {0};
            shelf_entry_t* item = &candidate;
            int len = snprintf(item->path, sizeof(item->path), "%s/%s", roots[i].path, ent->d_name);
            if (len < 0 || (size_t)len >= sizeof(item->path) || strlen(ent->d_name) >= sizeof(item->name)) { skipped = true; continue; }
            struct stat st;
            if (stat(item->path, &st) != 0) { unreadable = true; continue; }
            if (!S_ISREG(st.st_mode)) continue;
            if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) { skipped = true; continue; }
            if (!shelf_reserve()) { truncated = true; break; }
            if (!book_title_from_path(item->path, item->name, sizeof(item->name)))
                clean_filename(item->name, sizeof(item->name), ent->d_name);
            if (!strcasecmp(ext, ".epub")) {
                char title[sizeof(item->name)] = {0};
                char author[sizeof(item->author)] = {0};
                if (book_epub_metadata(item->path, title, sizeof(title), author, sizeof(author)) == ESP_OK) {
                    copy_text(item->author, sizeof(item->author), author);
                }
            }
            item->search_match = read_pico_search_match(item->name, s_query);
            item->size = st.st_size;
            item->is_flash = roots[i].is_flash;
            book_progress_t p;
            item->has_progress = book_progress_load(item->path, item->size, &p);
            item->pct = item->has_progress ? p.pct : 0;
            item->chapter = item->has_progress ? p.chapter : 0;
            item->recent = item->has_progress ? p.last_open_s : 0;
            s_shelf[s_count++] = candidate;
        }
        closedir(dir);
    }
    // 同路径重新出现也先完成旧清理，避免新阅读进度被后续重试擦掉。
    // Finish old cleanup even if the path reappears, before new reading can create progress.
    for (delete_retry_t* p = s_delete_retries; p; p = p->next) {
        int i = 0;
        while (i < s_count && strcmp(s_shelf[i].path, p->entry.path)) ++i;
        if (i == s_count) {
            if (!shelf_reserve()) { truncated = true; continue; }
            ++s_count;
        }
        s_shelf[i] = p->entry;
        s_shelf[i].selected = false;
        s_shelf[i].search_match = read_pico_search_match(p->entry.name, s_query);
    }
    sort_shelf(ctx);
    s_latest_path[0] = 0;
    char last_path[BOOK_STORE_PATH_MAX];
    if (book_progress_last_path(last_path, sizeof(last_path))) {
        for (int i = 0; i < s_count; ++i) {
            if (!strcmp(s_shelf[i].path, last_path) && !s_shelf[i].removed) {
                copy_text(s_latest_path, sizeof(s_latest_path), last_path);
                break;
            }
        }
    }
    if (!n) copy_text(s_storage, sizeof(s_storage), "存储不可用，请检查 TF 卡");
    if (truncated) snprintf(s_shelf_warning, sizeof(s_shelf_warning), "内存不足，仅列 %d 本；释放后重扫", s_count);
    else if (unreadable) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "部分目录或文件不可读，请检查后重扫");
    else if (skipped) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "部分文件过大或名称过长，未列入");
    else if (s_pending_invalidated) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "图书已更新，旧待保存进度已作废");
    if (!s_count && !unreadable && !truncated) copy_text(s_message, sizeof(s_message), "暂无图书，请传入 TXT 或 EPUB");
    read_pico_sd_info_t cache_sd = {0};
    read_pico_sd_get_info(&cache_sd);
    s_cache_sd_present = cache_sd.present;
    s_cache_sd_mounted = cache_sd.mounted;
    s_store_revision = book_store_revision();
    s_shelf_cache_valid = !unreadable && !truncated;
    ESP_LOGI(TAG, "shelf books=%d roots=%d", s_count, n);
}

static void refresh_cached_progress(app_ctx_t* ctx) {
    char latest[BOOK_STORE_PATH_MAX];
    if (!book_progress_last_path(latest, sizeof(latest))) return;
    for (int i = 0; i < s_count; ++i) {
        shelf_entry_t *item = &s_shelf[i];
        if (strcmp(item->path, latest) || item->removed) continue;
        book_progress_t progress;
        item->has_progress = book_progress_load(item->path, item->size, &progress);
        item->pct = item->has_progress ? progress.pct : 0;
        item->chapter = item->has_progress ? progress.chapter : 0;
        item->recent = item->has_progress ? progress.last_open_s : 0;
        copy_text(s_latest_path, sizeof(s_latest_path), latest);
        if (s_recent_sort) sort_shelf(ctx);
        return;
    }
}
static void return_to_cached_shelf(app_ctx_t* ctx) {
    if (s_shelf_cache_valid && s_store_revision == book_store_revision()) refresh_cached_progress(ctx);
    else scan_shelf(ctx);
}

static void refresh_capacity(void) {
    book_store_root_t roots[BOOK_STORE_ROOT_MAX];
    int count = 0;
    s_storage[0] = 0;
    book_store_roots(roots, &count);
    for (int i = 0; i < count; ++i) {
        if (i && !roots[i].is_flash && !roots[i - 1].is_flash) continue;
        char line[64];
        snprintf(line, sizeof(line), "%s%s %.1f MB", s_storage[0] ? " · " : "", roots[i].is_flash ? "内置余" : "TF余", book_store_free_bytes(&roots[i]) / 1048576.0);
        strncat(s_storage, line, sizeof(s_storage) - strlen(s_storage) - 1);
    }
}
static void manage_apply(app_ctx_t* ctx) {
    if (!strcmp(s_path, s_managed.path)) free_book();
    esp_err_t err;
    if (s_delete_confirm && !s_file_removed) {
        delete_retry_t* retry = delete_retry_reserve(&s_managed);
        if (!retry) {
            copy_text(s_manage_message, sizeof(s_manage_message), "内存不足，无法保留删除重试记录");
            s_clear_confirm = false;
            return;
        }
        bool removed = false;
        err = book_store_delete(s_managed.path, &removed);
        if (removed) {
            retry->entry.removed = true;
            s_file_removed = true;
            pending_discard(s_managed.path);
            book_store_notify_changed();
            s_store_revision = book_store_revision();
            for (int i = 0; i < s_count; ++i) if (!strcmp(s_shelf[i].path, s_managed.path)) s_shelf[i].removed = true;
            refresh_capacity();
        } else delete_retry_discard(s_managed.path);
    } else {
        pending_discard(s_managed.path);
        err = book_progress_forget(s_managed.path);
    }
    s_clear_confirm = false;
    if (err != ESP_OK) {
        copy_text(s_manage_message, sizeof(s_manage_message), s_file_removed ?
                  "文件已删除，进度清理失败，请重试" : s_delete_confirm ?
                  "文件删除失败，请检查存储后重试" : "进度清理失败，请重新尝试");
        return;
    }
    delete_retry_discard(s_managed.path);
    s_view = SHELF;
    int write = 0, leaf = ctx->leaf;
    for (int i = 0; i < s_count; ++i) {
        if (!strcmp(s_shelf[i].path, s_managed.path)) {
            if (s_file_removed) continue;
            s_shelf[i].has_progress = false; s_shelf[i].pct = 0; s_shelf[i].chapter = 0; s_shelf[i].recent = 0;
        }
        s_shelf[write++] = s_shelf[i];
    }
    s_count = write;
    sort_shelf(ctx);
    ctx->leaf = leaf < leaves() ? leaf : leaves() - 1;
}

/* ---- 输入与生命周期 / Input and lifecycle ---- */
static void sensor_set(app_ctx_t* ctx, bool on) {
    memset(&s_shake, 0, sizeof(s_shake));
    if (!ctx->sensor_ready) return;
    if (on) {
        if (!s_sensor_saved) {
            s_sensor_config = *sc7a20h_get_config(ctx->acc);
            s_sensor_saved = true;
        }
        sc7a20h_sensor_config_t config = s_sensor_config;
        config.odr = SC7A20H_ODR_100;
        config.fs = SC7A20H_FS_2G;
        s_sensor_on = sc7a20h_apply_config(ctx->acc, &config) == ESP_OK &&
            sc7a20h_activity_config(ctx->acc, BOOK_SHAKE_THS_MG, BOOK_SHAKE_DUR) == ESP_OK;
        if (!s_sensor_on) {
            sc7a20h_aoi_cfg_t off = {0};
            sc7a20h_aoi_config(ctx->acc, SC7A20H_AOI2, &off);
            sc7a20h_apply_config(ctx->acc, &s_sensor_config);
            s_sensor_saved = false;
            read_pico_sensor_sleep(ctx->acc);
        }
    } else {
        sc7a20h_aoi_cfg_t off = {0};
        sc7a20h_aoi_config(ctx->acc, SC7A20H_AOI2, &off);
        if (s_sensor_saved) sc7a20h_apply_config(ctx->acc, &s_sensor_config);
        s_sensor_saved = false;
        read_pico_sensor_sleep(ctx->acc);
        s_sensor_on = false;
    }
}
static app_redraw_t turn_page(app_ctx_t* ctx, int dir) {
    if (!s_text || s_clear_confirm) return APP_REDRAW_NONE;
    bool changed = true;
    if (dir > 0 && s_page + 1 < book_layout_page_count()) ++s_page;
    else if (dir < 0 && s_page) --s_page;
    else if (dir > 0 && s_chapter + 1 < book_chapter_count()) { save_progress(); changed = load_chapter(ctx, s_chapter + 1, 0, false); }
    else if (dir < 0 && s_chapter) { save_progress(); changed = load_chapter(ctx, s_chapter - 1, 0, true); }
    else return APP_REDRAW_NONE;
    if (!changed) { s_view = s_text ? TOC : SHELF; ctx->leaf = s_text ? s_chapter / BOOK_TOC_ROWS : 0; return APP_REDRAW_PAGE; }
    s_jump_offset = SIZE_MAX;
    s_last_turn_ms = s_stats_activity_ms = ctx->now_ms;
    ++s_stats_pending_turns;
    ++s_session_turns;
    ++s_turns;
    s_reader_cleanup = s_turns >= app_settings_reader_full_pages();
    if (s_reader_cleanup) s_turns = 0;
    if (s_unsaved < 8) ++s_unsaved;
    if (s_unsaved >= 8 && !s_save_failed) save_progress();
    ESP_LOGI(TAG, "turn chapter=%u page=%u/%u pct=%u", (unsigned)s_chapter, (unsigned)s_page + 1, (unsigned)book_layout_page_count(), percent(s_page));
    // 预渲染未命中时也使用 GL16；到达设定页数后在本次翻页整屏全刷。
    // A cache miss still uses GL16; the selected turn count triggers a full-screen refresh on this turn.
    app_redraw_t redraw = paint_reading(ctx, MODE_GL16);
    s_water_turn_pending = redraw == APP_REDRAW_AREA && !s_reader_cleanup &&
                           app_settings_reader_turn_effect() == 1;
    s_water_turn_dir = dir > 0 ? E0470_TURN_RTL : E0470_TURN_LTR;
    return redraw;
}
static app_redraw_t resize_text(app_ctx_t* ctx, int dir) {
    int next = s_px + dir * BOOK_PX_STEP;
    if (next < BOOK_PX_MIN || next > BOOK_PX_MAX) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    int old = s_px;
    if (!book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), next)) {
        bool restored = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), old);
        unlock_draw();
        if (!restored) {
            free_book();
            s_view = SHELF;
            copy_text(s_message, sizeof(s_message), "排版失败，请重新扫描并打开图书");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    s_px = next;
    s_page = book_layout_page_for_offset(off);
    unlock_draw();
    app_settings_set_book_px(s_px);
    save_progress();
    return paint_reading(ctx, MODE_DU);
}

static app_redraw_t apply_reader_layout(app_ctx_t* ctx, int next_px, int next_margin,
                                        uint8_t next_line, uint8_t next_para) {
    if (next_px < BOOK_PX_MIN || next_px > BOOK_PX_MAX ||
        next_margin < 24 || next_margin > 60 ||
        next_line < 110 || next_line > 150 ||
        next_para > 75 || next_para % 25) return APP_REDRAW_NONE;
    if (next_px == s_px && next_margin == s_margin && next_line == app_settings_book_line_spacing() &&
        next_para == app_settings_book_paragraph_spacing()) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    int old_px = s_px, old_margin = s_margin;
    uint8_t old_line = app_settings_book_line_spacing();
    uint8_t old_para = app_settings_book_paragraph_spacing();
    lock_draw();
    invalidate_prep();
    s_px = next_px;
    s_margin = next_margin;
    s_line_spacing = next_line;
    book_layout_set_spacing(next_line, next_para);
    bool ok = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    if (ok) s_page = book_layout_page_for_offset(off);
    else {
        s_px = old_px;
        s_margin = old_margin;
        s_line_spacing = old_line;
        book_layout_set_spacing(old_line, old_para);
        (void)book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    app_settings_set_book_px((uint8_t)s_px);
    app_settings_set_book_margin((uint8_t)s_margin);
    app_settings_set_book_line_spacing(next_line);
    app_settings_set_book_paragraph_spacing(next_para);
    save_progress();
    return APP_REDRAW_PAGE;
}

static app_redraw_t apply_reader_typography(app_ctx_t* ctx, int tracking_index) {
    (void)ctx;
    if (!s_text || tracking_index < 0 || tracking_index > 4)
        return APP_REDRAW_NONE;
    int old_index = app_settings_book_tracking();
    if (tracking_index == old_index) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    book_layout_set_typography((tracking_index - 2) * 2);
    bool ok = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    if (ok) s_page = book_layout_page_for_offset(off);
    else {
        book_layout_set_typography((old_index - 2) * 2);
        (void)book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    app_settings_set_book_tracking((uint8_t)tracking_index);
    save_progress();
    return APP_REDRAW_PAGE;
}

static app_redraw_t toggle_reader_fullscreen(app_ctx_t* ctx) {
    if (!s_text) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    s_reader_fullscreen = !s_reader_fullscreen;
    bool ok = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    if (ok) s_page = book_layout_page_for_offset(off);
    else {
        s_reader_fullscreen = !s_reader_fullscreen;
        (void)book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    free(s_inline_gray); s_inline_gray = NULL; s_inline_index = -1;
    prepare_inline_image();
    s_jump_offset = SIZE_MAX;
    s_reader_panel = READER_PANEL_NONE;
    save_progress();
    return APP_REDRAW_PAGE;
}

static app_redraw_t select_reading_font_item(app_ctx_t* ctx, const ttf_font_item_t* item) {
    if (!item || !strcmp(ttf_font_path(), item->path)) return APP_REDRAW_NONE;
    char previous[TTF_FONT_PATH_MAX];
    copy_text(previous, sizeof(previous), ttf_font_path());
    if (ttf_font_open(item->path) != ESP_OK) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    bool ok = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    if (ok) s_page = book_layout_page_for_offset(off);
    else if (previous[0]) {
        (void)ttf_font_open(previous);
        (void)book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    app_settings_set_font_path(item->path);
    copy_text(s_font_path, sizeof(s_font_path), item->path);
    save_progress();
    return APP_REDRAW_PAGE;
}

static int slider_index(EpdRect rect, int x, int count) {
    int span = rect.width - 128;
    int pos = x - rect.x - 64;
    int index = span > 0 && count > 1 ? (pos * (count - 1) + span / 2) / span : 0;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    return index;
}

static EpdRect reader_slider_rect(int slider) {
    if (slider == 0) return (EpdRect){36, 746, 612, 66};
    if (slider == 1) return (EpdRect){36, 700, 294, 66};
    if (slider == 2) return (EpdRect){354, 700, 294, 66};
    if (slider == 3) return (EpdRect){36, 819, 612, 66};
    return (EpdRect){36, 938, 612, 66};
}

static int reader_slider_at(uint16_t x, uint16_t y) {
    int first = s_reader_panel == READER_PANEL_FONT_SETTINGS ? 0 : 1;
    int end = s_reader_panel == READER_PANEL_FONT_SETTINGS ? 1 : 5;
    for (int i = first; i < end; ++i) if (ui_rect_hit(reader_slider_rect(i), x, y)) return i;
    return -1;
}

static void reader_slider_map(int slider, int x) {
    if (slider == 0) s_reader_preview_px = BOOK_PX_MIN + slider_index(reader_slider_rect(0), x, 37);
    else if (slider == 1) s_reader_preview_margin = 24 + slider_index(reader_slider_rect(1), x, 37);
    else if (slider == 2) s_reader_preview_line = 110 + slider_index(reader_slider_rect(2), x, 41);
    else if (slider == 3) s_reader_preview_para = slider_index(reader_slider_rect(3), x, 4) * 25;
    else if (slider == 4) s_reader_preview_tracking = slider_index(reader_slider_rect(4), x, 5);
}

static void reader_slider_begin(int slider, int x) {
    s_reader_slider = slider;
    s_reader_preview_px = s_px;
    s_reader_preview_margin = s_margin;
    s_reader_preview_line = app_settings_book_line_spacing();
    s_reader_preview_para = app_settings_book_paragraph_spacing();
    s_reader_preview_tracking = app_settings_book_tracking();
    EpdRect rect = reader_slider_rect(slider);
    int *value = slider == 0 ? &s_reader_preview_px : slider == 1 ? &s_reader_preview_margin :
                 slider == 2 ? &s_reader_preview_line : slider == 3 ? &s_reader_preview_para :
                 &s_reader_preview_tracking;
    int min = slider == 0 ? BOOK_PX_MIN : slider == 1 ? 24 : slider == 2 ? 110 : 0;
    int max = slider == 0 ? BOOK_PX_MAX : slider == 1 ? 60 : slider == 2 ? 150 : slider == 3 ? 75 : 4;
    s_reader_slider_endpoint = x < rect.x + 64 || x >= rect.x + rect.width - 64;
    int step = slider == 3 ? 25 : 1;
    if (x < rect.x + 64) *value -= step;
    else if (x >= rect.x + rect.width - 64) *value += step;
    else reader_slider_map(slider, x);
    if (*value < min) *value = min;
    if (*value > max) *value = max;
}

static void reader_slider_move(int x) {
    int slider = s_reader_slider;
    if (slider < 0) return;
    s_reader_slider_endpoint = false;
    reader_slider_map(slider, x);
}

static app_redraw_t reader_slider_commit(app_ctx_t* ctx) {
    if (s_reader_slider < 0) return APP_REDRAW_NONE;
    int slider = s_reader_slider;
    int px = s_reader_preview_px, margin = s_reader_preview_margin, line = s_reader_preview_line;
    int para = s_reader_preview_para;
    int tracking = s_reader_preview_tracking;
    s_reader_slider = -1;
    if (slider == 4) return apply_reader_typography(ctx, tracking);
    return apply_reader_layout(ctx, px, margin, line, (uint8_t)para);
}

static app_redraw_t reader_panel_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_reader_panel == READER_PANEL_TOOLS) {
        for (int i = 0; i < BOOK_TOOL_COUNT; ++i) if (ui_rect_hit(tool_rect(i), x, y)) {
            invalidate_prep();
            if (i == 0) {
                save_progress();
                s_view = TOC;
                s_message[0] = 0;
                s_reader_panel = READER_PANEL_NONE;
                size_t toc = current_toc_position();
                ctx->leaf = toc == SIZE_MAX ? 0 : (int)(toc / BOOK_TOC_ROWS);
            } else if (i == 1) {
                (void)bookmark_toggle();
            } else if (i == 2) {
                s_reader_panel = READER_PANEL_STATS;
            } else if (i == 3) {
                s_reader_panel = READER_PANEL_REFRESH_SETTINGS;
            } else {
                s_line_spacing = app_settings_book_line_spacing();
                s_reader_slider = -1;
                s_reader_panel = READER_PANEL_FONT_SETTINGS;
            }
            return APP_REDRAW_PAGE;
        }
        s_reader_panel = READER_PANEL_NONE;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_STATS) {
        const int top = 768;
        EpdRect details = {36, top + 319, 294, 78}, recent = {354, top + 319, 294, 78};
        if (ui_rect_hit(details, x, y)) { s_bookmark_page = 0; s_reader_panel = READER_PANEL_BOOKMARKS; }
        else if (ui_rect_hit(recent, x, y)) {
            flush_ticket_stats();
            s_recent_days_valid = book_ticket_recent_days(s_recent_days);
            s_reader_panel = READER_PANEL_STATS_RECENT;
        } else s_reader_panel = y < top ? READER_PANEL_NONE : READER_PANEL_TOOLS;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_BOOKMARKS) {
        const int top = 560;
        if (y < top) s_reader_panel = READER_PANEL_NONE;
        else if (y < top + 86) s_reader_panel = READER_PANEL_STATS;
        else {
            reader_bookmarks_t marks = {0};
            bool valid = bookmark_load(&marks);
            int pages = valid && marks.count ? ((int)marks.count + BOOKMARK_ROWS - 1) / BOOKMARK_ROWS : 1;
            if (y >= top + 570) {
                if (x < UI_LOCK_WIDTH / 2 && s_bookmark_page > 0) --s_bookmark_page;
                else if (x >= UI_LOCK_WIDTH / 2 && s_bookmark_page + 1 < pages) ++s_bookmark_page;
            } else if (valid) {
                int row = ((int)y - (top + 96)) / 82;
                int position = s_bookmark_page * BOOKMARK_ROWS + row;
                if (row >= 0 && row < BOOKMARK_ROWS && position < marks.count) {
                    int newest = (int)marks.count - 1 - position;
                    reader_bookmark_entry_t mark = marks.entries[newest];
                    if (mark.chapter < book_chapter_count() && load_chapter(ctx, mark.chapter, mark.byte_off, false)) {
                        s_reader_panel = READER_PANEL_NONE;
                        save_progress();
                    }
                }
            }
        }
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_STATS_RECENT) {
        int top = 584;
        if (y < top) s_reader_panel = READER_PANEL_NONE;
        else if (y < top + 112) s_reader_panel = READER_PANEL_STATS;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_REFRESH_SETTINGS) {
        if (y < 560) s_reader_panel = READER_PANEL_NONE;
        else if (ui_rect_hit((EpdRect){36, 680, 612, 112}, x, y)) {
            // 手动全刷先收起面板，再把整张阅读页作为新目标帧提交。
            // Close the sheet before presenting the entire reader frame as the target.
            s_reader_panel = READER_PANEL_NONE;
            s_reader_split = false;
            s_reader_cleanup = false;
            s_turns = 0;
            invalidate_prep();
            return APP_REDRAW_FULL;
        } else if (y >= 850 && y < 928) {
            for (int i = 0; i < 3; ++i) {
                EpdRect choice = {36 + i * 207, 850, 194, 78};
                if (!ui_rect_hit(choice, x, y)) continue;
                app_settings_set_reader_full_pages((uint8_t)(5 + i * 5));
                s_turns = 0;
                return APP_REDRAW_PAGE;
            }
        } else if (y >= 993 && y < 1087) {
            for (int i = 0; i < 2; ++i) {
                EpdRect choice = {36 + i * 312, 993, 300, 94};
                if (!ui_rect_hit(choice, x, y)) continue;
                app_settings_set_reader_turn_effect((uint8_t)i);
                return APP_REDRAW_PAGE;
            }
        } else if (y >= 1141 && y < 1199) s_reader_panel = READER_PANEL_NONE;
        else return APP_REDRAW_NONE;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_FONT_SETTINGS) {
        const int top = 640;
        EpdRect size = reader_slider_rect(0);
        EpdRect font = {36, 840, 294, 82};
        EpdRect shake = {354, 840, 294, 82};
        EpdRect layout = {36, 944, 612, 92};
        EpdRect rule = {36, 1058, 612, 109};
        if (ui_rect_hit(size, x, y)) {
            int px = BOOK_PX_MIN + slider_index(size, x, BOOK_PX_MAX - BOOK_PX_MIN + 1);
            return apply_reader_layout(ctx, px, s_margin, app_settings_book_line_spacing(),
                                       app_settings_book_paragraph_spacing());
        }
        if (ui_rect_hit(font, x, y)) {
            ttf_font_scan();
            s_font_page = 0;
            int count = ttf_font_count();
            const char* active = ttf_font_path();
            for (int i = 0; i < count; ++i) {
                const ttf_font_item_t* item = ttf_font_item(i);
                if (item && !strcmp(active, item->path)) { s_font_page = i / BOOK_FONT_PAGE; break; }
            }
            s_reader_panel = READER_PANEL_FONT_PICKER;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(shake, x, y)) {
            s_shake_enabled = !s_shake_enabled;
            app_settings_set_book_shake(s_shake_enabled);
            sensor_set(ctx, s_shake_enabled);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(layout, x, y)) {
            s_reader_slider = -1;
            s_reader_panel = READER_PANEL_LAYOUT_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(rule, x, y)) {
            uint8_t next = (app_settings_book_reading_line() + 1) % 3;
            app_settings_set_book_reading_line(next);
            book_layout_set_reading_line(next);
            invalidate_prep();
            return APP_REDRAW_PAGE;
        }
        s_reader_panel = y < top ? READER_PANEL_NONE : READER_PANEL_TOOLS;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_LAYOUT_SETTINGS) {
        const int top = 575;
        EpdRect back_icon = {36, top + 29, 56, 56};
        EpdRect back_button = {36, 1080, 612, 72};
        if (ui_rect_hit(back_icon, x, y) || ui_rect_hit(back_button, x, y)) {
            s_reader_slider = -1;
            s_reader_panel = READER_PANEL_FONT_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        EpdRect margin = reader_slider_rect(1);
        EpdRect line = reader_slider_rect(2);
        EpdRect paragraph = reader_slider_rect(3);
        EpdRect tracking = reader_slider_rect(4);
        if (ui_rect_hit(margin, x, y)) {
            return apply_reader_layout(ctx, s_px, 24 + slider_index(margin, x, 37),
                                       app_settings_book_line_spacing(), app_settings_book_paragraph_spacing());
        }
        if (ui_rect_hit(line, x, y)) {
            uint8_t value = (uint8_t)(110 + slider_index(line, x, 41));
            return apply_reader_layout(ctx, s_px, s_margin, value,
                                       app_settings_book_paragraph_spacing());
        }
        if (ui_rect_hit(paragraph, x, y))
            return apply_reader_layout(ctx, s_px, s_margin, app_settings_book_line_spacing(),
                                       (uint8_t)(slider_index(paragraph, x, 4) * 25));
        if (ui_rect_hit(tracking, x, y))
            return apply_reader_typography(ctx, slider_index(tracking, x, 5));
        s_reader_panel = y < top ? READER_PANEL_NONE : READER_PANEL_FONT_SETTINGS;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_FONT_PICKER) {
        const int top = 584;
        if (y < top + 112) {
            s_reader_panel = READER_PANEL_FONT_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        int count = ttf_font_count(), start = s_font_page * BOOK_FONT_PAGE;
        for (int slot = 0; slot < BOOK_FONT_PAGE && start + slot < count; ++slot) {
            int row = slot / 3, col = slot % 3;
            EpdRect card = {36 + col * 204, top + 119 + row * 146, 180, 126};
            if (ui_rect_hit(card, x, y)) return select_reading_font_item(ctx, ttf_font_item(start + slot));
        }
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t adjust_spacing(app_ctx_t* ctx, int control) {
    uint8_t old_line = app_settings_book_line_spacing();
    uint8_t old_para = app_settings_book_paragraph_spacing();
    uint8_t line = old_line, para = old_para;
    if (control == 3 && line > 120) line -= 30;
    if (control == 4 && line < 180) line += 30;
    if (control == 5 && para > 0) para -= 25;
    if (control == 6 && para < 75) para += 25;
    if (control == 17) { line = 150; para = 50; }
    if (line == old_line && para == old_para) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    book_layout_set_spacing(line, para);
    bool ok = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    if (ok) s_page = book_layout_page_for_offset(off);
    else {
        book_layout_set_spacing(old_line, old_para);
        (void)book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    app_settings_set_book_line_spacing(line);
    app_settings_set_book_paragraph_spacing(para);
    save_progress();
    return paint_reading(ctx, MODE_DU);
}
static app_redraw_t select_reading_font(app_ctx_t* ctx, int style) {
    static const char* filenames[] = {
        "Song.ttf", "Hei.ttf", "Kai.ttf", "WenKai.ttf", "FangSong.ttf", "KingHwa.ttf",
        "CangErYunHei05.ttf", "ChillKai.ttf"
    };
    static const char* names[] = {"宋体", "黑体", "楷体", "文楷", "仿宋", "京华老宋", "仓耳云黑05", "寒蝉正楷"};
    char path[160];
    if (snprintf(path, sizeof(path), "%s/%s", app_settings_fonts_dir(), filenames[style]) >= sizeof(path)) return APP_REDRAW_NONE;
    if (!strcmp(ttf_font_path(), path)) return APP_REDRAW_NONE;
    struct stat font_stat;
    if (stat(path, &font_stat) != 0) {
        snprintf(s_message, sizeof(s_message), "请将 %s 字体放入所选字体目录", names[style]);
        return APP_REDRAW_PAGE;
    }
    if (ttf_font_open(path) != ESP_OK) {
        snprintf(s_message, sizeof(s_message), "请将 %s 字体放入所选字体目录", names[style]);
        return APP_REDRAW_PAGE;
    }
    app_settings_set_font_path(path);
    s_message[0] = 0;
    size_t off = book_layout_page_start_offset(s_page);
    save_progress();
    lock_draw();
    invalidate_prep();
    bool ok = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
    if (ok) s_page = book_layout_page_for_offset(off);
    copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
    unlock_draw();
    if (!ok) {
        free_book(); s_view = SHELF;
        copy_text(s_message, sizeof(s_message), "字体重排失败，请重新打开图书");
        return APP_REDRAW_PAGE;
    }
    save_progress();
    return paint_reading(ctx, MODE_GL16);
}
static app_redraw_t manage_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (!s_clear_confirm && !s_file_removed && ui_rect_hit((EpdRect){UI_MARGIN, manage_panel().y + manage_panel().height - 190, ui_content_width(), 68}, x, y)) {
        extern void app_files_request_folder(int folder);
        app_files_request_folder(0);
        ui_nav_request(ctx, 2);
        return APP_REDRAW_NONE;
    }
    int count = s_clear_confirm || s_file_removed ? 2 : 3;
    for (int i = 0; i < count; ++i) if (ui_rect_hit(manage_rect(i, count), x, y)) {
        if (s_clear_confirm) { if (!i) s_clear_confirm = false; else manage_apply(ctx); }
        else if (!i) s_view = SHELF;
        else if (s_file_removed) manage_apply(ctx);
        else { s_clear_confirm = true; s_delete_confirm = i == 2; s_manage_message[0] = 0; }
        break;
    }
    return APP_REDRAW_PAGE;
}
static app_redraw_t editor_save(app_ctx_t* ctx) {
    if (s_editor_pinyin[0]) {
        copy_text(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字");
        return APP_REDRAW_PAGE;
    }
    char *start = s_editor_title;
    while (*start == ' ') ++start;
    if (start != s_editor_title) memmove(s_editor_title, start, strlen(start) + 1);
    size_t len = strlen(s_editor_title);
    while (len && s_editor_title[len - 1] == ' ') s_editor_title[--len] = 0;
    if (!s_editor_title[0]) {
        copy_text(s_editor_notice, sizeof(s_editor_notice), "书名不能为空");
        return APP_REDRAW_PAGE;
    }
    esp_err_t err = book_title_set(s_managed.path, s_editor_title);
    if (err != ESP_OK) {
        copy_text(s_editor_notice, sizeof(s_editor_notice), "保存失败，请检查书名或存储");
        return APP_REDRAW_PAGE;
    }
    for (int i = 0; i < s_count; ++i)
        if (!strcmp(s_shelf[i].path, s_managed.path)) copy_text(s_shelf[i].name, sizeof(s_shelf[i].name), s_editor_title);
    free(s_editor_cover); s_editor_cover = NULL;
    s_view = SHELF;
    sort_shelf(ctx);
    return APP_REDRAW_PAGE;
}
static app_redraw_t editor_paint(app_ctx_t* ctx) {
    render(ctx, ctx->fb);
    s_area = (EpdRect){36, 422, 612, 655};
    s_mode = MODE_DU;
    return APP_REDRAW_AREA;
}
static app_redraw_t editor_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < 134) {
        if (x < 170) { free(s_editor_cover); s_editor_cover = NULL; s_view = MANAGE; return APP_REDRAW_PAGE; }
        if (x > 510) return editor_save(ctx);
        return APP_REDRAW_NONE;
    }
    if (y >= 424 && y < 507 && x >= 36 && x < 648) {
        if (x >= 590) editor_move(1);
        else if (x >= 534) editor_move(-1);
        else editor_place_cursor(x);
        return editor_paint(ctx);
    }
    if (y >= 606 && y < 663) {
        if (x >= 604) { ++s_editor_candidate_page; editor_refresh_candidates(); return editor_paint(ctx); }
        int index = (int)(x - 36) / 112;
        if (x >= 36 && index >= 0 && index < 5) { editor_commit_candidate(index); return editor_paint(ctx); }
    }
    static const char *keys[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int row = 0; row < 3; ++row) {
        int left = row == 0 ? 36 : row == 1 ? 67 : 123;
        int top = 695 + row * 74;
        if (y < top || y >= top + 61 || x < left) continue;
        int col = (x - left) / 62;
        if (col < 0 || col >= (int)strlen(keys[row]) || x >= left + col * 62 + 58) continue;
        char letter = keys[row][col];
        if (s_editor_chinese) {
            size_t n = strlen(s_editor_pinyin);
            if (n + 1 < sizeof(s_editor_pinyin)) {
                s_editor_pinyin[n] = letter >= 'A' && letter <= 'Z' ? letter + 32 : letter;
                s_editor_pinyin[n + 1] = 0;
                s_editor_candidate_page = 0;
                editor_refresh_candidates();
            }
        } else { char english[2] = {letter, 0}; editor_append(english); }
        return editor_paint(ctx);
    }
    if (y >= 929 && y < 999) {
        if (x >= 36 && x < 138) {
            if (s_editor_pinyin[0]) { copy_text(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字"); return editor_paint(ctx); }
            s_editor_chinese = !s_editor_chinese;
            return editor_paint(ctx);
        }
        if (x >= 148 && x < 446) {
            if (s_editor_pinyin[0] && s_editor_candidate_count) editor_commit_candidate(0);
            else if (s_editor_pinyin[0]) { editor_append(s_editor_pinyin); s_editor_pinyin[0] = 0; editor_refresh_candidates(); }
            else editor_append(" ");
            return editor_paint(ctx);
        }
        if (x >= 456 && x < 554) { editor_backspace(); return editor_paint(ctx); }
        if (x >= 564) return editor_save(ctx);
    }
    return APP_REDRAW_NONE;
}
// 成功项退出选择，已删文件的失败项仅重试元数据。/ Deselect successes; removed-file failures retry metadata only.
static void batch_apply(app_ctx_t* ctx) {
    unsigned done = 0, failed = 0;
    int write = 0;
    for (int i = 0; i < s_count; ++i) {
        shelf_entry_t item = s_shelf[i];
        bool discard = false;
        if (item.selected) {
            if (!strcmp(s_path, item.path)) free_book();
            esp_err_t err;
            if (s_batch_delete && !item.removed) {
                delete_retry_t* retry = delete_retry_reserve(&item);
                if (!retry) { ++failed; s_shelf[write++] = item; continue; }
                bool removed = false;
                err = book_store_delete(item.path, &removed);
                if (removed) {
                    retry->entry.removed = true;
                    item.removed = true; pending_discard(item.path);
                    book_store_notify_changed(); s_store_revision = book_store_revision();
                } else delete_retry_discard(item.path);
            } else { pending_discard(item.path); err = book_progress_forget(item.path); }
            if (err == ESP_OK) {
                delete_retry_discard(item.path);
                ++done; item.selected = false; item.has_progress = false; item.pct = 0;
                item.chapter = 0; item.recent = 0;
                discard = item.removed;
            } else ++failed;
        }
        if (!discard) s_shelf[write++] = item;
    }
    s_count = write;
    refresh_capacity();
    int leaf = ctx->leaf;
    sort_shelf(ctx);
    ctx->leaf = leaf < leaves() ? leaf : leaves() - 1;
    s_batch_confirm = false;
    snprintf(s_batch_message, sizeof(s_batch_message), "成功 %u 本，失败 %u 本%s", done, failed, failed ? "；所选可重试" : "");
}
static app_redraw_t batch_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_batch_confirm) {
        if (ui_rect_hit(ui_row_rect(0, 2, 620, UI_BTN_H), x, y)) s_batch_confirm = false;
        else if (ui_rect_hit(ui_row_rect(1, 2, 620, UI_BTN_H), x, y)) batch_apply(ctx);
        return APP_REDRAW_PAGE;
    }
    for (int i = 0; i < 5; ++i) if (ui_rect_hit(batch_rect(i), x, y)) {
        if (!i) select_page(ctx->leaf);
        else if (i == 1) clear_selection();
        else if (i == 2) { clear_selection(); s_batch_message[0] = 0; scan_shelf(ctx); }
        else if (selected_count()) { s_batch_delete = i == 3; s_batch_confirm = true; }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t action_at(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_view == TOC) {
        int target = book_toc_hit(x, y, ctx->leaf, book_navigation_count());
        if (target == BOOK_TOC_BACK) {
            s_message[0] = 0;
            s_view = READING;
            s_reader_panel = READER_PANEL_NONE;
            return APP_REDRAW_PAGE;
        }
        if (target == BOOK_TOC_PREV || target == BOOK_TOC_NEXT) {
            int next = ctx->leaf + (target == BOOK_TOC_PREV ? -1 : 1);
            if (next >= 0 && next < leaves()) { ctx->leaf = next; return APP_REDRAW_PAGE; }
            return APP_REDRAW_NONE;
        }
        if (target >= 0 && !s_message[0]) {
            size_t chapter = book_navigation_chapter((size_t)target);
            if (chapter >= book_chapter_count()) return APP_REDRAW_NONE;
            save_progress();
            if (load_chapter_at(ctx, chapter, 0, false, book_navigation_anchor((size_t)target),
                                book_navigation_source_offset((size_t)target))) {
                s_selected_toc = (size_t)target;
                char entry_title[sizeof(s_title)];
                if (book_navigation_title((size_t)target, entry_title, sizeof(entry_title)) == ESP_OK)
                    copy_text(s_title, sizeof(s_title), entry_title);
                s_view = READING;
                s_reader_panel = READER_PANEL_NONE;
                save_progress();
            }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == MANAGE) return manage_action(ctx, x, y);
    if (s_view == SEARCH) {
        for (int i = 0; i < 45; ++i) if (ui_rect_hit(search_rect(i), x, y)) {
            app_redraw_t result = search_action(ctx, i);
            if (result == APP_REDRAW_AREA) { render(ctx, ctx->fb); s_area = (EpdRect){UI_MARGIN, 190, ui_content_width(), 850}; s_mode = MODE_DU; }
            return result;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == BULK) { app_redraw_t result = batch_action(ctx, x, y); if (result != APP_REDRAW_NONE) return result; }
    if (s_view == READING) {
        if (s_toolbar) return reader_panel_action(ctx, x, y);
        if (x < UI_LOCK_WIDTH * 3 / 10) return turn_page(ctx, -1);
        if (x >= UI_LOCK_WIDTH * 7 / 10) return turn_page(ctx, 1);
        if (ui_rect_hit(body_rect(), x, y)) return toggle_reader_fullscreen(ctx);
        return APP_REDRAW_NONE;
    }
    if (s_view == SHELF || s_view == BULK) {
        if (s_view == SHELF) {
            int tab = ui_nav_hit(x, y);
            if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
        }
        if (s_view == SHELF && ui_rect_hit(shelf_import_rect(), x, y)) {
            extern const app_desc_t app_files;
            ctx->request_app = &app_files;
            return APP_REDRAW_NONE;
        }
        if (s_view == SHELF && ui_rect_hit(shelf_manage_rect(), x, y)) {
            clear_selection();
            s_batch_confirm = false;
            s_batch_message[0] = 0;
            s_view = BULK;
            ctx->leaf = 0;
            return APP_REDRAW_PAGE;
        }
        if (s_view == BULK) {
        if (ui_rect_hit(ui_row_rect(0, 3, 176, 72), x, y)) {
            s_filter = (s_filter + 1) % 3; clear_selection(); s_batch_message[0] = 0;
            sort_shelf(ctx);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_row_rect(1, 3, 176, 72), x, y)) {
            s_recent_sort = !s_recent_sort;
            sort_shelf(ctx);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_row_rect(2, 3, 176, 72), x, y)) { search_begin(); return APP_REDRAW_PAGE; }
        }
        if (s_view == SHELF && s_save_failed && y >= UI_CONTENT_BOTTOM - UI_PX_CAPTION && y < UI_CONTENT_BOTTOM) {
            retry_progress();
            return APP_REDRAW_PAGE;
        }
    }
    int nav = ui_bar_hit(x, y, 3);
    if (nav == 0 || nav == 2) {
        int next = ctx->leaf + (nav == 0 ? -1 : 1);
        if (next >= 0 && next < leaves()) ctx->leaf = next;
        return APP_REDRAW_PAGE;
    }
    if (nav == 1) {
        s_message[0] = 0;
        s_view = s_view == BULK ? SHELF : BULK; s_batch_confirm = false;
        return APP_REDRAW_PAGE;
    }
    if (s_message[0] && !((s_view == SHELF || s_view == BULK) && s_visible_count)) return APP_REDRAW_NONE;
    int rows = s_view == BULK ? BOOK_BULK_ROWS : BOOK_ROWS;
    for (int row = 0; row < rows; ++row) if (ui_rect_hit(row_rect(row), x, y)) {
        int i = ctx->leaf * rows + row;
        if (s_view == BULK && i < s_visible_count) toggle_selection(i);
        else if (s_view == SHELF && i < s_visible_count) {
            if (s_shelf[i].removed) {
                s_managed = s_shelf[i]; s_view = MANAGE; s_file_removed = s_delete_confirm = true; s_clear_confirm = false;
                copy_text(s_manage_message, sizeof(s_manage_message), "文件已删除，进度清理失败，请重试");
            } else open_book(ctx, s_shelf[i].path);
        }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}
static void on_enter(app_ctx_t* ctx) {
    ensure_prep();
    book_layout_set_spacing(app_settings_book_line_spacing(), app_settings_book_paragraph_spacing());
    book_layout_set_typography(((int)app_settings_book_tracking() - 2) * 2);
    book_layout_set_reading_line(app_settings_book_reading_line());
    s_reader_fullscreen = false;
    s_view = s_requested_manage ? BULK : SHELF;
    s_reader_return_home = false;
    s_requested_manage = false;
    s_presented_view = -1;
    ctx->leaf = 0;
    s_px = app_settings_book_px();
    s_margin = app_settings_book_margin();
    s_line_spacing = app_settings_book_line_spacing();
    // 传书页已停止并 join HTTP，安全注销且只丢弃对应路径的旧进度。
    // Transfer has stopped and joined HTTP; safely unregister only invalidated paths.
    pending_drop_invalidated();
    s_reader_panel = READER_PANEL_NONE;
    s_reader_slider = -1;
    s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    s_du_count = 0;
    s_reader_cleanup = false;
    s_poll_ms = 0;
    read_pico_sd_info_t sd = {0};
    esp_err_t media_err = read_pico_sd_get_info(&sd);
    bool reuse = s_shelf_cache_valid && media_err != ESP_ERR_NOT_FINISHED &&
                 sd.present == s_cache_sd_present && sd.mounted == s_cache_sd_mounted &&
                 s_store_revision == book_store_revision();
    s_scan_pending = !reuse;
    if (reuse) {
        refresh_cached_progress(ctx);
        (void)open_requested_book(ctx);
    } else {
        s_count = s_visible_count = 0;
        s_message[0] = 0;
        s_storage[0] = 0;
        invalidate_covers();
        // 已指定文件可直接打开；书架目录留到真正进入书架时再扫描。
        // Open a requested file directly; scan the shelf only when it is actually shown.
        if (open_requested_book(ctx)) {
            s_scan_pending = false;
            return;
        }
        read_pico_sd_start_probe();
    }
    s_shake_enabled = app_settings_book_shake();
    if (s_shake_enabled) sensor_set(ctx, true);
}
static void book_on_exit(app_ctx_t* ctx) {
    s_presented_view = -1;
    free(s_editor_cover); s_editor_cover = NULL;
    s_pressed_control = -1;
    s_reader_slider = -1;
    save_progress();
    s_reader_return_home = false;
    sensor_set(ctx, false);
    free_book();
    if (s_prep_task) { vTaskDelete(s_prep_task); s_prep_task = NULL; }
    free(s_next_fb);
    s_next_fb = NULL;
    if (s_prep_done) { vSemaphoreDelete(s_prep_done); s_prep_done = NULL; }
    if (s_draw_lock) { vSemaphoreDelete(s_draw_lock); s_draw_lock = NULL; }
}
// 先停止使用旧卡句柄与字体预渲染，主循环随后切换内置字体。
// Stop old-card handles and font preparation before the loop switches to the builtin font.
static void book_on_media_lost(app_ctx_t* ctx) {
    lock_draw();
    save_progress();
    free_book();
    unlock_draw();
    free(s_editor_cover); s_editor_cover = NULL;
    s_view = SHELF;
    s_count = s_visible_count = 0;
    s_shelf_cache_valid = false;
    free(s_shelf); s_shelf = NULL; s_shelf_capacity = 0;
    invalidate_covers();
    ctx->leaf = 0;
    s_reader_panel = READER_PANEL_NONE;
    s_reader_slider = -1;
    s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    s_scan_pending = true;
    s_du_count = 0;
    copy_text(s_storage, sizeof(s_storage), "TF 卡已移除");
    copy_text(s_message, sizeof(s_message), "TF 卡已移除");
}
// 控件编号仅用于保持按下与抬起命中同一个目标。/ IDs pair a press with release on the same control.
static int control_at(app_ctx_t* ctx, uint16_t x, uint16_t y, EpdRect* rect) {
    if (s_view == SEARCH) {
        for (int i = 0; i < 45; ++i) { *rect = search_rect(i); if (ui_rect_hit(*rect, x, y)) return 500 + i; }
        return -1;
    }
    if (s_view == BULK && s_batch_confirm) {
        for (int i = 0; i < 2; ++i) { *rect = ui_row_rect(i, 2, 620, UI_BTN_H); if (ui_rect_hit(*rect, x, y)) return 600 + i; }
        return -1;
    }
    if (s_view == BULK) for (int i = 0; i < 5; ++i) { *rect = batch_rect(i); if (ui_rect_hit(*rect, x, y)) return 610 + i; }
    if (s_view == MANAGE) {
        if (!s_clear_confirm && !s_file_removed) { *rect = (EpdRect){UI_MARGIN, manage_panel().y + manage_panel().height - 190, ui_content_width(), 68}; if (ui_rect_hit(*rect, x, y)) return 404; }
        int count = s_clear_confirm || s_file_removed ? 2 : 3;
        for (int i = 0; i < count; ++i) {
            *rect = manage_rect(i, count);
            if (ui_rect_hit(*rect, x, y)) return s_clear_confirm ? 300 + i : s_file_removed && i == 1 ? 403 : 400 + i;
        }
        return -1;
    }
    if (s_clear_confirm) {
        for (int i = 0; i < 2; ++i) {
            *rect = ui_row_rect(i, 2, 610, UI_BTN_H);
            if (ui_rect_hit(*rect, x, y)) return 300 + i;
        }
        return -1;
    }
    if (s_view == READING) {
        if (s_reader_panel == READER_PANEL_TOOLS) for (int i = 0; i < BOOK_TOOL_COUNT; ++i) {
            *rect = tool_rect(i);
            if (ui_rect_hit(*rect, x, y)) return 200 + i;
        }
        return -1;
    }
    if (s_view != SHELF) for (int i = 0; i < 3; ++i) {
        *rect = ui_bar_rect(i, 3);
        if (ui_rect_hit(*rect, x, y)) return 100 + i;
    }
    if (s_view == SHELF && ui_rect_hit(shelf_manage_rect(), x, y)) { *rect = shelf_manage_rect(); return 114; }
    if (s_view == SHELF && ui_rect_hit(shelf_import_rect(), x, y)) { *rect = shelf_import_rect(); return 115; }
    if (s_view == BULK) {
        for (int i = 0; i < 3; ++i) {
            *rect = ui_row_rect(i, 3, 176, 72);
            if (ui_rect_hit(*rect, x, y)) return 110 + i;
        }
    }
    *rect = (EpdRect){UI_MARGIN, UI_CONTENT_BOTTOM - UI_PX_CAPTION, ui_content_width(), UI_PX_CAPTION};
    if (s_view == SHELF && s_save_failed && ui_rect_hit(*rect, x, y)) return 113;
    if (s_message[0] && !((s_view == SHELF || s_view == BULK) && s_visible_count)) return -1;
    int rows = s_view == BULK ? BOOK_BULK_ROWS : BOOK_ROWS;
    int count = s_visible_count;
    for (int row = 0; row < rows && ctx->leaf * rows + row < count; ++row) {
        *rect = row_rect(row);
        if (ui_rect_hit(*rect, x, y)) return row;
    }
    return -1;
}
static app_redraw_t paint_control(app_ctx_t* ctx, EpdRect rect) {
    render(ctx, ctx->fb);
    s_area = rect;
    s_mode = MODE_DU;
    return APP_REDRAW_AREA;
}
static app_redraw_t gesture_event(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_view == TOC) {
        if (ev->type == UI_GESTURE_TAP) {
            int start = book_toc_hit(ev->x0, ev->y0, ctx->leaf, book_navigation_count());
            int end = book_toc_hit(ev->x, ev->y, ctx->leaf, book_navigation_count());
            if (start != -1 && start == end) return action_at(ctx, ev->x0, ev->y0);
        }
        if (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D) {
            int next = ctx->leaf + (ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
            if (next >= 0 && next < leaves()) { ctx->leaf = next; return APP_REDRAW_PAGE; }
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == EDIT) {
        if (ev->type == UI_GESTURE_TAP) return editor_action(ctx, ev->x0, ev->y0);
        if (ev->type == UI_GESTURE_SWIPE_L && s_editor_pinyin[0]) {
            ++s_editor_candidate_page; editor_refresh_candidates(); return editor_paint(ctx);
        }
        if (ev->type == UI_GESTURE_SWIPE_R && s_editor_candidate_page) {
            --s_editor_candidate_page; editor_refresh_candidates(); return editor_paint(ctx);
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == SHELF && ev->type == UI_GESTURE_TAP && ui_nav_hit(ev->x0, ev->y0) >= 0) {
        ui_nav_request(ctx, ui_nav_hit(ev->x0, ev->y0));
        return APP_REDRAW_NONE;
    }
    if (s_view == READING && ev->type != UI_GESTURE_PRESS) s_stats_activity_ms = ctx->now_ms;
    if (s_view == READING &&
        (s_reader_panel == READER_PANEL_FONT_SETTINGS || s_reader_panel == READER_PANEL_LAYOUT_SETTINGS)) {
        if (ev->type == UI_GESTURE_PRESS) {
            int slider = reader_slider_at(ev->x0, ev->y0);
            if (slider >= 0) {
                reader_slider_begin(slider, ev->x0);
                render(ctx, ctx->fb);
                s_area = reader_slider_refresh_rect(slider);
                s_mode = MODE_GL16;
                return APP_REDRAW_AREA;
            }
        } else if (s_reader_slider >= 0) {
            if (ev->type == UI_GESTURE_MOVE) {
                int slider = s_reader_slider;
                reader_slider_move(ev->x);
                render(ctx, ctx->fb);
                s_area = reader_slider_refresh_rect(slider);
                s_mode = MODE_GL16;
                return APP_REDRAW_AREA;
            }
            if (ev->type == UI_GESTURE_TAP || ev->type == UI_GESTURE_SWIPE_L ||
                ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_U ||
                ev->type == UI_GESTURE_SWIPE_D || ev->type == UI_GESTURE_LONG_PRESS) {
                if (!s_reader_slider_endpoint) reader_slider_move(ev->x);
                return reader_slider_commit(ctx);
            }
            if (ev->type == UI_GESTURE_CANCEL) {
                s_reader_slider = -1;
                return APP_REDRAW_PAGE;
            }
        }
    }
    if (s_view == READING && s_reader_panel == READER_PANEL_FONT_PICKER &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int count = ttf_font_count();
        int pages = count > 0 ? (count + BOOK_FONT_PAGE - 1) / BOOK_FONT_PAGE : 1;
        if (ev->type == UI_GESTURE_SWIPE_U && s_font_page + 1 < pages) ++s_font_page;
        else if (ev->type == UI_GESTURE_SWIPE_D && s_font_page > 0) --s_font_page;
        else return APP_REDRAW_NONE;
        return APP_REDRAW_PAGE;
    }
    EpdRect start_rect = {0}, end_rect = {0};
    int start = control_at(ctx, ev->x0, ev->y0, &start_rect);
    int end = control_at(ctx, ev->x, ev->y, &end_rect);
    if (ev->type == UI_GESTURE_PRESS) {
        s_pressed_control = start;
        return start >= 0 ? paint_control(ctx, start_rect) : APP_REDRAW_NONE;
    }
    bool decorated = s_pressed_control >= 0;
    s_pressed_control = -1;
    if (ev->type == UI_GESTURE_LONG_PRESS && start == end) {
        if (s_view == SHELF && start >= 0 && start < BOOK_ROWS && !s_clear_confirm) {
            s_managed = s_shelf[ctx->leaf * BOOK_ROWS + start];
            s_view = MANAGE;
            s_clear_confirm = false;
            s_delete_confirm = s_file_removed = s_managed.removed;
            copy_text(s_manage_message, sizeof(s_manage_message), s_file_removed ? "文件已删除，进度清理失败，请重试" : "");
            return APP_REDRAW_PAGE;
        }
        if (s_view == READING && !s_toolbar && !s_clear_confirm && ui_rect_hit(body_rect(), ev->x0, ev->y0)) {
            save_progress();
            s_view = TOC;
            s_message[0] = 0;
            size_t toc = current_toc_position();
            ctx->leaf = toc == SIZE_MAX ? 0 : (int)(toc / BOOK_TOC_ROWS);
            return APP_REDRAW_PAGE;
        }
    }
    if (ev->type == UI_GESTURE_TAP && !s_scan_pending) {
        if (start >= 0 && start == end) {
            app_redraw_t result = action_at(ctx, ev->x0, ev->y0);
            if (ctx->request_app || ctx->request_menu || ctx->request_return) return result;
            return result != APP_REDRAW_NONE ? result : paint_control(ctx, start_rect);
        }
        if (start < 0 && end < 0 && s_view == READING && !s_clear_confirm)
            return action_at(ctx, ev->x0, ev->y0);
    }
    if (!s_clear_confirm && !s_scan_pending) {
        if (s_view == READING && !s_toolbar && ui_rect_hit(body_rect(), ev->x0, ev->y0)) {
            if (ev->type == UI_GESTURE_SWIPE_L) return turn_page(ctx, 1);
            if (ev->type == UI_GESTURE_SWIPE_R) return turn_page(ctx, -1);
        } else if (s_view == SHELF && (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R)) {
            int next = ctx->leaf + (ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
            if (next >= 0 && next < leaves()) {
                ctx->leaf = next;
                prepare_covers(ctx);
                render(ctx, ctx->fb);
                s_area = (EpdRect){0, 196, UI_LOCK_WIDTH, 892};
                s_mode = MODE_GL16;
                return APP_REDRAW_AREA;
            }
        } else if ((s_view == BULK && !s_batch_confirm) &&
                   (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
            int next = ctx->leaf + (ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
            if (next >= 0 && next < leaves()) {
                ctx->leaf = next;
                return APP_REDRAW_PAGE;
            }
        }
    }
    return decorated ? paint_control(ctx, start_rect) : APP_REDRAW_NONE;
}
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    s_pressed_control = -1;
    if (s_view != READING && s_view != TOC) {
        if (key == UI_KEY_2) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
        if (s_view == EDIT) { free(s_editor_cover); s_editor_cover = NULL; s_view = MANAGE; return APP_REDRAW_PAGE; }
        if (s_view == SEARCH) { search_finish(ctx, false); return APP_REDRAW_PAGE; }
        if (s_batch_confirm) { s_batch_confirm = false; return APP_REDRAW_PAGE; }
        if (s_view == MANAGE || s_view == BULK) { s_view = SHELF; return APP_REDRAW_PAGE; }
        if (s_view == SHELF) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    }
    if (s_scan_pending || s_clear_confirm) return APP_REDRAW_NONE;
    if (key == UI_KEY_2) {
        if (s_view == BULK) s_view = SHELF;
        else if (s_view == SHELF) { ui_nav_request(ctx, 3); return APP_REDRAW_NONE; }
        else if (s_view == READING) {
            s_reader_slider = -1;
            s_reader_panel = s_reader_panel == READER_PANEL_TOOLS ? READER_PANEL_NONE : READER_PANEL_TOOLS;
            invalidate_prep();
        } else if (s_view == TOC) {
            s_view = s_text ? READING : SHELF;
            s_reader_panel = READER_PANEL_NONE;
        } else {
            ctx->request_menu = true;
            return APP_REDRAW_NONE;
        }
        return APP_REDRAW_PAGE;
    }
    if (key != UI_KEY_1 && key != UI_KEY_3) return APP_REDRAW_NONE;
    int dir = key == UI_KEY_1 ? -1 : 1;
    if (s_view == READING) { s_stats_activity_ms = ctx->now_ms; return turn_page(ctx, dir); }
    int next = ctx->leaf + dir;
    if (next < 0 || next >= leaves()) return APP_REDRAW_NONE;
    ctx->leaf = next;
    return APP_REDRAW_PAGE;
}
static app_redraw_t on_power_short(app_ctx_t* ctx) {
    if (s_view != READING || !s_text || s_toolbar || s_clear_confirm ||
        !app_settings_reader_power_turn()) return APP_REDRAW_NONE;
    s_stats_activity_ms = ctx->now_ms;
    app_redraw_t result = turn_page(ctx, 1);
    return result == APP_REDRAW_NONE ? APP_REDRAW_DONE : result;
}
static app_redraw_t on_key_long(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_2) return APP_REDRAW_NONE;
    if (s_view != READING && s_view != TOC) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_view == READING) {
        save_progress();
        if (s_reader_return_home) {
            ui_nav_request(ctx, 0);
            return APP_REDRAW_NONE;
        }
        free_book();
        s_view = SHELF;
        s_reader_panel = READER_PANEL_NONE;
        return_to_cached_shelf(ctx);
        return APP_REDRAW_PAGE;
    }
    s_message[0] = 0;
    s_view = s_text ? READING : SHELF;
    s_reader_panel = READER_PANEL_NONE;
    return APP_REDRAW_PAGE;
}
static bool menu_handle_enabled(app_ctx_t* ctx) {
    (void)ctx;
    return s_view != READING && s_view != SHELF && s_view != TOC && s_view != EDIT;
}
static app_redraw_t on_tick(app_ctx_t* ctx) {
    track_ticket_stats(ctx);
    if (ctx->consumed) return APP_REDRAW_NONE;
    if (s_reader_notice[0] && ctx->now_ms >= s_reader_notice_until) {
        s_reader_notice[0] = 0;
        if (s_view == READING) {
            // 区域刷新不会自动重绘帧缓冲；先移除提示，再只更新提示所在区域。
            // Area refreshes do not rerender the framebuffer; erase the toast before updating its region.
            render(ctx, ctx->fb);
            s_area = (EpdRect){208, 150, 268, 68};
            s_mode = MODE_GL16;
            return APP_REDRAW_AREA;
        }
    }
    if (s_save_failed && ctx->now_ms - s_save_retry_ms >= 15000) {
        s_save_retry_ms = ctx->now_ms;
        bool failed = s_save_failed;
        retry_progress();
        if (failed != s_save_failed) { invalidate_prep(); return APP_REDRAW_PAGE; }
    }
    if (s_scan_pending && ctx->now_ms - s_poll_ms >= 500) {
        s_poll_ms = ctx->now_ms;
        read_pico_sd_info_t info = {0};
        if (read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED) return APP_REDRAW_NONE;
        s_scan_pending = false;
        scan_shelf(ctx);
        (void)open_requested_book(ctx);
        return APP_REDRAW_PAGE;
    }
    if (s_view == SHELF && !(ctx->touch && ctx->touch->touched)) {
        for (int row = 0; row < BOOK_ROWS; ++row) {
            unsigned bit = 1u << row;
            if (!(s_cover_pending_mask & bit)) continue;
            s_cover_pending_mask &= ~bit;
            int index = ctx->leaf * BOOK_ROWS + row;
            if (index >= s_visible_count || s_covers[row].index != index) continue;
            bool pending = false;
            s_covers[row].gray = load_cover_gray(s_shelf[index].path, true, &pending);
            if (!s_covers[row].gray) continue;
            EpdRect area = row_rect(row);
            render(ctx, ctx->fb);
            s_area = area;
            s_mode = MODE_GL16;
            return APP_REDRAW_AREA;
        }
    }
    if (s_text && strcmp(s_font_path, ttf_font_path())) {
        size_t off = book_layout_page_start_offset(s_page);
        save_progress();
        lock_draw();
        invalidate_prep();
        bool ok = book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px);
        if (ok) s_page = book_layout_page_for_offset(off);
        copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
        unlock_draw();
        if (!ok) { free_book(); s_view = SHELF; copy_text(s_message, sizeof(s_message), "字体重排失败，请重新打开图书"); }
        return APP_REDRAW_PAGE;
    }
    // SC7A20H 的有效输出节奏为 12.5Hz；80ms 轮询避免重复 I2C 读取同一采样。
    // The effective SC7A20H cadence is 12.5Hz; 80ms avoids rereading the same sample over I2C.
    if (s_shake_enabled && s_sensor_on && ctx->now_ms - s_sensor_ms >= 80) {
        s_sensor_ms = ctx->now_ms;
        if (!sc7a20h_powered(ctx->acc)) sensor_set(ctx, true);
        sc7a20h_events_t ev;
        if (sc7a20h_read_events(ctx->acc, &ev) == ESP_OK) {
            bool suppressed = s_view != READING || s_toolbar || s_clear_confirm ||
                              (ctx->touch && ctx->touch->touched) || ctx->now_ms - s_last_turn_ms < 800;
            if (book_shake_feed(&s_shake, ev.aoi2_src & 0x40, suppressed, ctx->now_ms)) return turn_page(ctx, 1);
        }
    }
    return APP_REDRAW_NONE;
}
static void before_lock(app_ctx_t* ctx) {
    track_ticket_stats(ctx);
    save_progress();
    // 锁屏票根始终以当前正在阅读的书为准；不要依赖书架最近项是否已经同步。
    if (s_text && s_path[0]) book_progress_set_last_path(s_path);
    flush_ticket_stats();
    s_stats_last_ms = 0;
}
static EpdRect area_hint(app_ctx_t* ctx) { (void)ctx; return s_area; }

const app_desc_t app_book = {
    .title = "图书 Books", .detail = "TF 卡 txt / epub 阅读", .enter_full = false, .owns_keys = true,
    .defer_middle_short = true,
    .menu_handle_enabled = menu_handle_enabled,
    .render = render, .present = present, .on_enter = on_enter, .on_exit = book_on_exit,
    .on_media_lost = book_on_media_lost, .on_before_lock = before_lock,
    .on_gesture = gesture_event, .on_key = on_key, .on_key_long = on_key_long,
    .on_power_short = on_power_short,
    .on_tick = on_tick, .area_hint = area_hint,
};
