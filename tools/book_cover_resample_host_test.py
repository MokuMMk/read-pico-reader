#!/usr/bin/env python3
"""中文：实际双线性映射与流式 PNG 的边界、损坏文件和内存失败。
English: Actual bilinear mapping and streamed PNG bounds, corrupt input and allocation failures.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import re,struct,subprocess,tempfile,zlib
root=Path(__file__).resolve().parents[1]
source=(root/'main/book/book_cover.c').read_text()
def fn(name):
    start=re.search(r'^(?:static )?(?:inline )?[^\n]+\b'+name+r'\([^;]*?\)\s*\{',source,re.M).start()
    brace=source.index('{',start);depth=1;at=brace+1
    # 已选函数无代码中的花括号字符串。/ These helpers have no braces inside string literals.
    while depth:
        depth+=(source[at]=='{')-(source[at]=='}');at+=1
    return source[start:at]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "png.h"
static int fail_after=-1,live;
static void *cover_alloc(size_t n){if(fail_after==0)return NULL;if(fail_after>0)--fail_after;void *p=malloc(n);if(p)++live;return p;}
static void tracked_free(void *p){if(p)--live;free(p);}
#define free tracked_free
static void decode_yield(unsigned at){(void)at;}
typedef struct {unsigned x,y,width,height;} book_crop_t;
'''
for name in ('luminance','scale_axis'):unit+=fn(name)+'\n'
unit+='typedef struct {uint16_t at;uint8_t frac;} scale_step_t;\n'
for name in ('scale_map','scale_next','bilinear4','book_cover_crop'):unit+=fn(name)+'\n'
unit+='typedef struct {const uint8_t *data;size_t size,pos;FILE *file;} png_input_t;\n'
for name in ('png_read_mem','png_gray_input'):unit+=fn(name)+'\n'
unit+=r'''
#undef free
int main(int argc,char **argv){
 assert(argc==2);
 for(unsigned src=1;src<90;src++)for(unsigned dst=1;dst<130;dst++)for(unsigned i=0;i<dst;i++){
  int at,f;scale_axis(i,dst,src,&at,&f);assert(at>=0&&(unsigned)at<src&&f>=0&&f<256);
 }
 FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 uint8_t *data=malloc(size);assert(fread(data,1,size,f)==(size_t)size);fclose(f);
 uint8_t pixels[80];memset(pixels,0xaa,sizeof(pixels));assert(png_gray_input(data,size,NULL,8,8,pixels));assert(!live);
 assert(pixels[0]==0&&pixels[63]==240&&pixels[27]>80&&pixels[27]<140);
 for(int i=64;i<80;i++)assert(pixels[i]==0xaa);
 assert(png_gray_input(data,size,NULL,2,2,pixels));assert(!live&&pixels[0]==40&&pixels[3]==200);
 assert(png_gray_input(data,size,NULL,1,1,pixels));assert(!live);
 for(int i=0;i<5;i++){fail_after=i;assert(!png_gray_input(data,size,NULL,8,8,pixels));assert(!live);}
 fail_after=-1;
 // IDAT 截断发生在分配映射及两行缓存之后，验证 longjmp 释放路径。
 // IDAT truncation occurs after map/row allocation, exercising longjmp cleanup.
 assert(!png_gray_input(data,45,NULL,8,8,pixels));assert(!live);
 f=fopen(argv[1],"rb");assert(png_gray_input(NULL,size,f,8,8,pixels));fclose(f);assert(!live);
 free(data);puts("book_cover_resample: PASS (up/down/degenerate scaling, streamed PNG, bounds, truncation and OOM)");
}
'''
def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
raw=b''.join(b'\0'+b''.join(bytes([x*32+y*48]*3+[255]) for x in range(4)) for y in range(4))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',4,4,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b'')
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp);(tmp/'test.c').write_text(unit);(tmp/'fixture.png').write_bytes(png)
    vendor=root/'managed_components/espressif__libpng/libpng';assert vendor.exists(),'Build IDF once to resolve libpng'
    (tmp/'pnglibconf.h').write_bytes((vendor/'scripts/pnglibconf.h.prebuilt').read_bytes())
    names='png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwrite pngwio pngwtran pngwutil'.split()
    subprocess.run(['cc','-std=gnu11','-O2','-w','-fsanitize=address,undefined','-DPNG_ARM_NEON_OPT=0','-DPNG_INTEL_SSE_OPT=0','-I'+str(tmp),'-I'+str(vendor),str(tmp/'test.c')]+[str(vendor/(n+'.c')) for n in names]+['-lz','-lm','-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test'),str(tmp/'fixture.png')],check=True)
