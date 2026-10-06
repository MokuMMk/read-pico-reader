"""中文：验证真实更新 UI 的任务收尾、一次提示与局部刷新节流。
English: Test actual update UI worker completion, one-shot offers and local refresh throttling.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
settings = root / "main/apps/app_device_settings.c"

def function(name, source=settings):
    text = source.read_text()
    match = re.search(r"^static [^\n]+\b" + name + r"\([^;{}]*?\)\s*\{", text, re.M)
    assert match, name
    start, at, depth, quote, escape = match.start(), match.end(), 1, None, False
    while depth:
        c = text[at]
        if quote:
            if escape: escape = False
            elif c == "\\": escape = True
            elif c == quote: quote = None
        elif c in "\"'": quote = c
        elif c == "{": depth += 1
        elif c == "}": depth -= 1
        at += 1
    return text[start:at]

unit = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ota_online.h"
typedef enum {APP_REDRAW_NONE,APP_REDRAW_AREA,APP_REDRAW_PAGE} app_redraw_t;
typedef struct {int x,y,width,height;} EpdRect;
static pico_update_status_t s_update;
static uint32_t s_update_drawn_percent,s_update_drawn_at;
static pico_update_state_t s_update_drawn_state;
static bool s_update_drawn_busy,s_upgrade_offer_pending,s_upgrade_confirm,s_upgrade_online;
static EpdRect wiped;
#define UI_GRAY_WHITE 255
#define UI_GRAY_LIGHT 192
#define UI_LOCK_WIDTH 684
#define EPD_DRAW_ALIGN_RIGHT 1
#define EPD_DRAW_ALIGN_CENTER 2
static int ui_text_last_percent,track_width;
static void epd_fill_rect(EpdRect r,int color,uint8_t *fb){(void)color;(void)fb;wiped=r;}
static void ui_text(uint8_t *fb,int x,int y,int px,const char *text,int align,bool inverted){(void)fb;(void)x;(void)y;(void)px;(void)align;(void)inverted;ui_text_last_percent=atoi(text);}
static void ui_fill_round_rect(uint8_t *fb,EpdRect r,int radius,int color){(void)fb;(void)radius;(void)color;track_width=r.width;assert(r.x>=wiped.x&&r.y>=wiped.y&&r.x+r.width<=wiped.x+wiped.width&&r.y+r.height<=wiped.y+wiped.height);}
static char s_book_title[128]="海边的信",s_title[128]="chapter";
static bool s_reader_favorite;
static int back_center,title_center,favorite_center;
static void ui_nav_back(uint8_t *fb,int x,int y){(void)fb;(void)x;back_center=y+35;}
static void ui_text_vc(uint8_t *fb,int x,int y,int px,const char *text,int align,bool inverted){(void)fb;(void)x;(void)px;(void)text;(void)align;(void)inverted;title_center=y;}
static void draw_favorite_icon(uint8_t *fb,int x,int y,int w,int h,bool favorite,int gray){(void)fb;(void)x;(void)w;(void)favorite;(void)gray;favorite_center=y+h/2;}
static void ui_hairline(uint8_t *fb,int y,int x,int width,int gray){(void)fb;(void)y;(void)x;(void)width;(void)gray;}
static void copy_text(char *out,size_t cap,const char *text){snprintf(out,cap,"%s",text);}
static void fit_text(char *text,int px,int width){(void)text;(void)px;(void)width;}
'''
for name in ("upgrade_percent", "upgrade_remember", "upgrade_status_redraw", "upgrade_progress_area", "draw_upgrade_progress"):
    unit += function(name) + "\n"
unit += function("draw_reader_header", root / "main/apps/app_book.c") + "\n"
unit += r'''
int main(void){
 s_update=(pico_update_status_t){.state=PICO_UPDATE_CHECKING,.busy=true};upgrade_remember(0);s_upgrade_offer_pending=true;
 s_update.state=PICO_UPDATE_AVAILABLE;assert(upgrade_status_redraw(100)==APP_REDRAW_NONE&&!s_upgrade_confirm);
 s_update.busy=false;assert(upgrade_status_redraw(200)==APP_REDRAW_PAGE&&s_upgrade_confirm&&s_upgrade_online&&!s_upgrade_offer_pending);
 s_upgrade_confirm=false;assert(upgrade_status_redraw(6000)==APP_REDRAW_NONE&&!s_upgrade_confirm);
 s_update=(pico_update_status_t){.state=PICO_UPDATE_CHECKING,.busy=true};upgrade_remember(0);s_upgrade_offer_pending=true;
 s_update.state=PICO_UPDATE_FAILED;assert(upgrade_status_redraw(100)==APP_REDRAW_NONE);
 s_update.busy=false;assert(upgrade_status_redraw(200)==APP_REDRAW_PAGE&&!s_upgrade_offer_pending);
 s_update=(pico_update_status_t){.state=PICO_UPDATE_DOWNLOADING,.busy=true,.release={.size=1000}};upgrade_remember(100);
 s_update.received=40;assert(upgrade_status_redraw(10000)==APP_REDRAW_NONE);
 s_update.received=50;assert(upgrade_status_redraw(4999)==APP_REDRAW_NONE);assert(upgrade_status_redraw(5100)==APP_REDRAW_AREA);
 for(uint32_t now=5101;now<10100;++now){s_update.received=500;assert(upgrade_status_redraw(now)==APP_REDRAW_NONE);}
 assert(upgrade_status_redraw(10100)==APP_REDRAW_AREA);draw_upgrade_progress(NULL);assert(ui_text_last_percent==50&&track_width==282);
 s_update.received=1000;assert(upgrade_status_redraw(15100)==APP_REDRAW_AREA);draw_upgrade_progress(NULL);assert(ui_text_last_percent==100&&track_width==564);
 s_update.state=PICO_UPDATE_READY;assert(upgrade_status_redraw(15101)==APP_REDRAW_NONE);s_update.busy=false;assert(upgrade_status_redraw(15102)==APP_REDRAW_PAGE);
 s_update.state=PICO_UPDATE_DOWNLOADING;s_update.busy=true;s_update.received=0;upgrade_remember(UINT32_MAX-2000);s_update.received=60;assert(upgrade_status_redraw(2999)==APP_REDRAW_AREA);
 draw_reader_header(NULL);assert(back_center==title_center&&favorite_center==title_center);
 s_reader_favorite=true;draw_reader_header(NULL);assert(back_center==title_center&&favorite_center==title_center);
 puts("PASS: worker cleanup and busy transition, direct one-shot update offer, five-second/5-percent area-only progress, timer wrap and aligned reader header");
}
'''
with tempfile.TemporaryDirectory() as folder:
    source, binary = Path(folder)/"test.c", Path(folder)/"test"
    source.write_text(unit)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I"+str(root/"tools/ota_stubs"), "-I"+str(root/"main"), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
