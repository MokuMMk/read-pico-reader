"""Exercise actual Home cache permutation and mode ownership with checked frees.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]

def function(name, path):
    text = path.read_text()
    scan = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"', lambda m: ' ' * len(m[0]), text)
    start = re.search(r'^(?:static )?[^\n]+\b' + name + r'\([^;{}]*?\)\s*\{', scan, re.M)
    assert start, name
    at, depth = start.end(), 1
    while depth:
        depth += (scan[at] == '{') - (scan[at] == '}')
        at += 1
    return text[start.start():at]

if __name__ == '__main__':
    home = ROOT / 'main/apps/app_dashboard.c'
    source = home.read_text()
    types = source[source.index('typedef struct {'):source.index('static int s_cover_next;')]
    unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "ui_image_dither.h"
#define BOOK_STORE_PATH_MAX 288
#define BOOK_COVER_W 176
#define BOOK_COVER_H 240
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {unsigned x,y,width,height;} book_crop_t;
typedef struct {int unused;} book_progress_t;
static bool fast, alloc_fail, asset_allowed=true;
static unsigned loads;
static size_t live_bytes;
static struct {void *ptr;size_t bytes;} live[24];
static void *remember(void *p,size_t bytes){if(!p)return NULL;for(int i=0;i<24;++i)if(!live[i].ptr){live[i].ptr=p;live[i].bytes=bytes;live_bytes+=bytes;return p;}assert(0);return NULL;}
static void tracked_free(void *p){if(!p)return;for(int i=0;i<24;++i)if(live[i].ptr==p){live_bytes-=live[i].bytes;live[i].ptr=NULL;free(p);return;}assert(0);}
#define free tracked_free
static void *heap_caps_malloc(size_t n,int caps){assert(caps==3);return alloc_fail?NULL:remember(malloc(n),n);}
static void *heap_caps_calloc(size_t n,size_t bytes,int caps){assert(caps==3);return alloc_fail?NULL:remember(calloc(n,bytes),n*bytes);}
static bool pico_boot_asset_allowed(const char *path){(void)path;return asset_allowed;}
static bool app_settings_main_fast_refresh(void){return fast;}
static uint8_t app_settings_system_contrast(void){return 100;}
static uint8_t sample(int p){return (uint8_t)(p*19+31);}
static bool book_cover_load_gray(const char *path,const char *title,const char *author,uint8_t *out,bool decode,bool *pending){(void)path;(void)title;(void)author;assert(decode&&!pending);++loads;for(int i=0;i<176*240;++i)out[i]=sample(i);return true;}
static int missing=-1, modified=-1;
static int test_stat(const char *path,struct stat *st){int id=-1;assert(sscanf(path,"/book%d.epub",&id)==1);if(id==missing)return -1;memset(st,0,sizeof(*st));st->st_mode=S_IFREG;st->st_size=100+id;st->st_mtime=200+id+(id==modified);return 0;}
#define stat(path,out) test_stat(path,out)
'''+types+'\nstatic int s_cover_next;\n'
    unit += function('ui_contrast_gray', ROOT/'main/ui/ui_kit.c')+'\n'
    unit += function('book_cover_crop', ROOT/'main/book/book_cover.c')+'\n'
    for name in ('book_at','clear_cover_cache','app_home_cover_mode_changed','restore_cover_cache','load_cover'):
        unit += function(name, home)+'\n'
    unit += r'''
static void book(int slot,int id){home_book_t *b=book_at(slot);memset(b,0,sizeof(*b));if(id<0)return;snprintf(b->path,sizeof(b->path),"/book%d.epub",id);snprintf(b->title,sizeof(b->title),"title%d",id);strcpy(b->author,"author");}
static void seed(void){clear_cover_cache();for(int i=0;i<7;++i){book(i,i);assert(load_cover(book_at(i),i));}assert(live_bytes==7u*176*240);}
int main(void){
    unsigned rng=1;
    for(int attempt=0;attempt<300;++attempt){
        seed();int ids[]={0,1,2,3,4,5,6};void *original[7];for(int i=0;i<7;++i)original[i]=s_cover_cache[i].pixels;
        for(int i=6;i>0;--i){rng=rng*1664525u+1013904223u;unsigned j=rng%(i+1);int t=ids[i];ids[i]=ids[j];ids[j]=t;}
        for(int i=0;i<7;++i)book(i,ids[i]);restore_cover_cache();
        for(int i=0;i<7;++i){assert(book_at(i)->cover==original[ids[i]]);assert(s_cover_cache[i].pixels==book_at(i)->cover);assert(!strcmp(book_at(i)->path,s_cover_cache[i].path));}
        assert(live_bytes==7u*176*240);
    }
    seed();book(0,5);book(1,5);book(2,-1);book(3,2);book(4,0);book(5,-1);book(6,8);restore_cover_cache();
    assert(book_at(0)->cover&&!book_at(1)->cover&&!book_at(2)->cover&&book_at(3)->cover&&book_at(4)->cover&&!book_at(5)->cover&&!book_at(6)->cover);
    assert(live_bytes==3u*176*240);
    seed();missing=2;modified=3;strcpy(book_at(4)->author,"changed");restore_cover_cache();assert(!book_at(2)->cover&&!book_at(3)->cover&&!book_at(4)->cover&&live_bytes==4u*176*240);
    missing=modified=-1;app_home_cover_mode_changed();assert(!live_bytes);
    fast=true;book(0,0);assert(load_cover(book_at(0),0));assert(live_bytes==5500&&book_at(0)->cover_fast);
    book_crop_t crop=book_cover_crop(176,240,172,250);
    for(int y=0;y<250;++y)for(int x=0;x<172;++x){unsigned sx=crop.x+(uint64_t)(unsigned)x*crop.width/172,sy=crop.y+(uint64_t)(unsigned)y*crop.height/250;assert(!!(book_at(0)->cover[y*22+x/8]&(1u<<(x&7)))==!!ui_image_dither_bw(ui_contrast_gray(sample(sy*176+sx)),36+x,292+y));}
    restore_cover_cache();assert(live_bytes==5500&&book_at(0)->cover_fast);
    fast=false;app_home_cover_mode_changed();assert(!live_bytes&&!book_at(0)->cover);assert(load_cover(book_at(0),0)&&live_bytes==176u*240&&!book_at(0)->cover_fast);
    app_home_cover_mode_changed();alloc_fail=true;assert(!load_cover(book_at(0),0)&&!live_bytes);alloc_fail=false;asset_allowed=false;unsigned before=loads;assert(!load_cover(book_at(0),0)&&loads==before);
    puts("PASS: 300 actual in-place Home reorderings; duplicate/missing/modified identities; no stale pointer or double free; fast 5,500-byte BW only; gray-only ordinary/ripple; checked OOM and recovery");
}
'''
    with tempfile.TemporaryDirectory(dir=ROOT/'build') as folder:
        c, exe = Path(folder)/'test.c',Path(folder)/'test'
        c.write_text(unit)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'main/ui'),str(c),str(ROOT/'main/ui/ui_image_dither.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
