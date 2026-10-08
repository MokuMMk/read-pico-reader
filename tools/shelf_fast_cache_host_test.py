"""中文：运行真实封面缓存，验证内存上限、失效与重复切页复用。
English: Exercise actual cover caching, its memory bound, invalidation and repeated-tab reuse.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(name, path):
    text = path.read_text()
    scan = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"',
                  lambda m: ' ' * len(m[0]), text)
    start = re.search(r'^(?:static )?[^\n]+\b' + name + r'\([^;{}]*?\)\s*\{', scan, re.M)
    assert start, name
    at, depth = start.end(), 1
    while depth:
        depth += (scan[at] == '{') - (scan[at] == '}')
        at += 1
    return text[start.start():at]


book = ROOT / 'main/apps/app_book.c'
unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_image_dither.h"
#define BOOK_ROWS 13
#define BOOK_GRID_ROWS 9
#define BOOK_STORE_PATH_MAX 288
#define BOOK_COVER_W 176
#define BOOK_COVER_H 240
#define SHELF_FAST_COVER_WIDTH 164
#define SHELF_FAST_COVER_HEIGHT 214
#define SHELF_FAST_COVER_STRIDE ((SHELF_FAST_COVER_WIDTH+7)/8)
#define SHELF_FAST_COVER_BYTES (SHELF_FAST_COVER_STRIDE*SHELF_FAST_COVER_HEIGHT)
#define SHELF_FAST_CACHE_RESERVE (512u*1024u)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {unsigned x,y,width,height;} book_crop_t;
enum {SHELF,MANAGE};
static int s_view=SHELF,rows=9,contrast=100,phase_shift;
static bool fast=true,alloc_fail;
static size_t free_psram=4*1024*1024,allocated_bytes;
static unsigned allocations,s_cover_pending_mask;
static struct {int index;uint8_t *gray,*fast_bits;uint8_t fast_contrast,fast_phase;} s_covers[BOOK_ROWS];
static int shelf_rows(void){return rows;}
static bool app_settings_main_fast_refresh(void){return fast;}
static uint8_t app_settings_system_contrast(void){return contrast;}
static size_t heap_caps_get_free_size(unsigned caps){assert(caps==3);return free_psram;}
static void* heap_caps_calloc(size_t count,size_t bytes,unsigned caps){
    assert(caps==3 && count==1 && bytes==4494);++allocations;
    if(alloc_fail)return NULL;
    allocated_bytes+=bytes;return calloc(count,bytes);
}
static EpdRect row_rect(int row){return (EpdRect){36+(row%3)*210+phase_shift,224+(row/3)*282,192,260};}
'''
unit += function('ui_contrast_gray', ROOT / 'main/ui/ui_kit.c') + '\n'
unit += function('book_cover_crop', ROOT / 'main/book/book_cover.c') + '\n'
for name in ('invalidate_covers', 'release_fast_covers', 'fast_cover_matches', 'prepare_fast_covers'):
    unit += function(name, book) + '\n'
unit += r'''
typedef struct {int leaf;} app_ctx_t;
typedef struct {uint8_t pct;uint32_t chapter,last_open_s;} book_progress_t;
typedef struct {char path[288];bool removed,has_progress;uint32_t size,chapter,recent;uint8_t pct;} shelf_entry_t;
static shelf_entry_t s_shelf[1];
static int s_count=1,sorts;
static bool s_recent_sort=true,added,last_valid=true,progress_valid=true;
static char s_latest_path[288];
static book_progress_t test_progress={37,9,42};
static bool shelf_backfill_read_books(bool* trunc){assert(!trunc);return added;}
static bool book_progress_last_path(char* out,size_t cap){assert(cap>=10);strcpy(out,"/test.epub");return last_valid;}
static bool book_progress_load(const char* path,uint32_t size,book_progress_t* p){assert(!strcmp(path,"/test.epub") && size==100);*p=test_progress;return progress_valid;}
static void copy_text(char* dst,size_t cap,const char* src){assert(strlen(src)<cap);strcpy(dst,src);}
static void sort_shelf(app_ctx_t* ctx){(void)ctx;++sorts;}
'''
unit += function('refresh_cached_progress', book) + '\n'
unit += r'''
static void verify_bitmap(int row) {
    EpdRect card=row_rect(row);
    int image_x=card.x+(card.width-164)/2,image_y=card.y;
    book_crop_t crop=book_cover_crop(176,240,164,214);
    assert(fast_cover_matches(row,(EpdRect){image_x,image_y,164,214}));
    for(int y=0;y<214;++y)for(int x=0;x<164;++x) {
        unsigned sx=crop.x+(uint64_t)(unsigned)x*crop.width/164;
        unsigned sy=crop.y+(uint64_t)(unsigned)y*crop.height/214;
        uint8_t tone=(ui_contrast_gray(s_covers[row].gray[sy*176+sx])>>4)*17u;
        int expected=ui_image_dither_cover_bw(tone,image_x+x,image_y+y)!=0;
        assert(!!(s_covers[row].fast_bits[y*21+x/8]&(1u<<(x&7)))==expected);
    }
    assert(fast_cover_matches(row,(EpdRect){image_x,image_y-16,164,214}));
}
int main(void) {
    for(int i=0;i<BOOK_ROWS;++i) {
        s_covers[i].index=i;s_covers[i].gray=malloc(176*240);assert(s_covers[i].gray);
        for(int p=0;p<176*240;++p)s_covers[i].gray[p]=(uint8_t)(p*19+i*31);
    }
    rows=13;prepare_fast_covers();
    assert(allocations==9 && allocated_bytes==40446);
    for(int row=0;row<9;++row)verify_bitmap(row);
    for(int row=9;row<13;++row)assert(!s_covers[row].fast_bits);
    unsigned before=allocations;
    for(int i=0;i<80;++i)prepare_fast_covers();
    assert(allocations==before);
    contrast=200;prepare_fast_covers();assert(allocations==before+9);
    for(int row=0;row<9;++row)verify_bitmap(row);
    phase_shift=1;before=allocations;prepare_fast_covers();assert(allocations==before+9);
    for(int row=0;row<9;++row)verify_bitmap(row);
    free_psram=512*1024;prepare_fast_covers();
    for(int row=0;row<13;++row)assert(!s_covers[row].fast_bits && s_covers[row].gray);
    free_psram=4*1024*1024;alloc_fail=true;prepare_fast_covers();
    for(int row=0;row<13;++row)assert(!s_covers[row].fast_bits && s_covers[row].gray);
    alloc_fail=false;rows=9;prepare_fast_covers();
    fast=false;prepare_fast_covers();
    for(int row=0;row<13;++row)assert(!s_covers[row].fast_bits);
    fast=true;prepare_fast_covers();s_view=MANAGE;prepare_fast_covers();
    for(int row=0;row<13;++row)assert(!s_covers[row].fast_bits);
    s_view=SHELF;prepare_fast_covers();invalidate_covers();
    for(int row=0;row<13;++row)assert(!s_covers[row].gray && !s_covers[row].fast_bits && s_covers[row].index==-1);
    // 快刷封面白底纯白，黑区少量白点；字形与收藏在缓存之外绘制。
    // Pure white cover backgrounds remain white, while solid blacks contain sparse white dots; glyphs and favorites are outside the cache.
    int black=0,midgray=0;
    for(int y=0;y<4;++y)for(int x=0;x<4;++x){
        assert(ui_image_dither_cover_bw((ui_contrast_gray(255)>>4)*17u,x,y)==255);
        black+=ui_image_dither_cover_bw(0,x,y)==0;
        midgray+=ui_image_dither_cover_bw(128,x,y)==0;
    }
    assert(black==15 && midgray==8);
    app_ctx_t ctx={0};strcpy(s_shelf[0].path,"/test.epub");s_shelf[0].size=100;s_shelf[0].recent=42;
    for(int i=0;i<80;++i)refresh_cached_progress(&ctx);
    assert(sorts==0 && s_shelf[0].pct==37 && s_shelf[0].chapter==9);
    ++test_progress.last_open_s;refresh_cached_progress(&ctx);assert(sorts==1);
    refresh_cached_progress(&ctx);assert(sorts==1);
    progress_valid=false;refresh_cached_progress(&ctx);assert(sorts==2 && s_shelf[0].recent==0);
    refresh_cached_progress(&ctx);assert(sorts==2);
    added=true;s_recent_sort=false;refresh_cached_progress(&ctx);assert(sorts==3);
    added=false;last_valid=false;refresh_cached_progress(&ctx);assert(sorts==3);
    puts("shelf fast cache: 40,446-byte PSRAM bound; 80 hot reuses without allocation; exact cover pixels, contrast/phase invalidation, low-memory/OOM fallback, reader/media release and unchanged-progress sort avoidance passed");
}
'''
with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
    src, exe = Path(directory) / 'test.c', Path(directory) / 'test'
    src.write_text(unit)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                    '-I' + str(ROOT / 'main/ui'), str(src), str(ROOT / 'main/ui/ui_image_dither.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
