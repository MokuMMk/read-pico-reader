// SPDX-License-Identifier: Apache-2.0
// 微读页面的宿主依赖；不联网。/ WeRead page host dependencies; no network.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "weread_service.h"
typedef struct { int x,y,width,height; } EpdRect;
typedef enum { APP_REDRAW_NONE,APP_REDRAW_AREA,APP_REDRAW_PAGE,APP_REDRAW_FULL,APP_REDRAW_DONE } app_redraw_t;
typedef enum { UI_GESTURE_TAP,UI_GESTURE_SWIPE_L,UI_GESTURE_SWIPE_R } ui_gesture_type_t;
typedef struct { ui_gesture_type_t type; int x0,y0; } ui_gesture_event_t;
struct app_desc;
typedef struct { int64_t now_ms; bool request_return; const struct app_desc *request_app; } app_ctx_t;
typedef struct app_desc { const char *title,*detail; bool enter_full,owns_keys; bool (*menu_handle_enabled)(app_ctx_t*);
 void (*on_enter)(app_ctx_t*),(*on_exit)(app_ctx_t*),(*on_media_lost)(app_ctx_t*),(*on_media_ready)(app_ctx_t*),(*on_before_lock)(app_ctx_t*);
 void (*render)(app_ctx_t*,uint8_t*); app_redraw_t (*on_gesture)(app_ctx_t*,const ui_gesture_event_t*),(*on_key)(app_ctx_t*,int),(*on_key_long)(app_ctx_t*,int),(*on_tick)(app_ctx_t*); } app_desc_t;
#define UI_LOCK_WIDTH 684
#define UI_LOCK_HEIGHT 1216
#define UI_NAV_TOP 1096
#define UI_GRAY_WHITE 255
#define EPD_DRAW_ALIGN_LEFT 2
#define EPD_DRAW_ALIGN_RIGHT 4
#define EPD_DRAW_ALIGN_CENTER 8
#define UI_KEY_2 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
static bool test_sd = true, test_alloc_fail, test_bulk;
static int test_start_count,test_stop_count,test_setup_count,test_book_open,test_notify_count,test_nav=-1;
static weread_action_t test_action;
static unsigned test_page,test_index;
static weread_snapshot_t test_snapshot;
static bool test_images;
static inline void *heap_caps_calloc(size_t a,size_t b,int flags){(void)flags;return test_alloc_fail?NULL:calloc(a,b);}
static inline void heap_caps_free(void *p){free(p);}
static inline void display_set_bulk_io(bool v){test_bulk=v;}
static inline void ttf_font_cache_clear(void){}
typedef struct {bool mounted;} read_pico_sd_info_t;
static inline void read_pico_sd_get_info(read_pico_sd_info_t *sd){sd->mounted=test_sd;}
typedef struct {bool is_flash;char path[160];} book_store_root_t;
static inline int book_store_upload_root(book_store_root_t *r){r->is_flash=false;strcpy(r->path,"/sdcard/books");return 0;}
static inline void book_store_notify_changed(void){test_notify_count++;}
static inline bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.width&&y<r.y+r.height;}
static inline void ui_clear_page(uint8_t *fb){(void)fb;}
static inline void ui_fill_round_rect(uint8_t *fb,...){(void)fb;}
static inline void ui_draw_round_rect(uint8_t *fb,...){(void)fb;}
static inline void ui_text(uint8_t *fb,...){(void)fb;}
static inline void ui_text_fixed(uint8_t *fb,...){(void)fb;}
static inline void ui_text_vc(uint8_t *fb,...){(void)fb;}
static inline void ui_hairline(uint8_t *fb,...){(void)fb;}
static inline void epd_fill_rect(EpdRect r,...){(void)r;}
static inline int ui_text_fixed_width_px(int px,const char *s){int n=0;for(;*s;s++)if(((unsigned char)*s&0xc0)!=0x80)n++;return n*px;}
static inline void ui_nav_status(uint8_t *fb){(void)fb;}
static inline void ui_nav_back(uint8_t *fb,...){(void)fb;}
static inline void ui_nav_draw(uint8_t *fb,...){(void)fb;}
static inline int ui_nav_hit(int x,int y){return y>=UI_NAV_TOP?x/171:-1;}
static inline void ui_nav_request(app_ctx_t *ctx,int i){(void)ctx;test_nav=i;}
static inline void ui_wifi_qr_clear(void){}
static inline bool ui_wifi_qr_prepare_weread(const char *s){return s&&*s;}
static inline void ui_wifi_qr_draw(uint8_t *fb,...){(void)fb;}
