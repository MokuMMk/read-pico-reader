"""票根字体上下文与浅睡恢复，使用真实切换函数。/ Actual ticket font switching and light-wake restoration.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import subprocess
import tempfile
from home_cover_cache_host_test import function
ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / 'main/font/app_font_context.c'
unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#define ESP_OK 0
#define TTF_FONT_PATH_MAX 288
#define TTF_FONT_BUILTIN "builtin"
typedef struct {char path[288];bool reading;} app_lock_font_t;
typedef struct {char path[288];} ttf_font_item_t;
static bool s_reading,system_face,system_scale,recovery;
static char chosen[288]="/fonts/system.ttf",reader[288]="/fonts/reader.ttf",active[288]="builtin",missing[288];
static unsigned opens;
static bool pico_boot_recovery(void){return recovery;}
static const char *app_settings_system_font_path(void){return chosen;}
static const char *app_settings_font_path(void){return reader;}
static const char *app_settings_fonts_dir(void){return "/absent/fonts";}
static bool ttf_font_ready(void){return true;}
static const char *ttf_font_path(void){return active;}
static bool ttf_font_is_builtin(void){return !strcmp(active,"builtin");}
static bool ttf_font_path_is_builtin(const char *path){return !*path||!strcmp(path,"builtin");}
static int ttf_font_count(void){return 0;}
static void ttf_font_scan(void){}
static const ttf_font_item_t *ttf_font_item(int index){(void)index;return NULL;}
static int ttf_font_open(const char *path){++opens;if(!strcmp(path,missing))return -1;snprintf(active,sizeof(active),"%s",path);return ESP_OK;}
static int ttf_font_open_builtin(void){return ttf_font_open("builtin");}
static void ui_text_set_system_font(bool on){system_face=on;}
static void ui_text_set_system_scale(bool on){system_scale=on;}
'''
for name in ('system_path', 'activate', 'app_font_activate_system', 'app_font_activate_reading',
             'app_font_retry_active', 'app_font_begin_lock', 'app_font_end_lock'):
    unit += function(name, FONT) + '\n'
unit += r'''
int main(void){
 app_lock_font_t saved;
 app_font_activate_reading();assert(s_reading&&!system_face&&!system_scale&&!strcmp(active,reader));
 app_font_begin_lock(&saved);assert(!s_reading&&system_face&&system_scale&&!strcmp(active,chosen));
 assert(saved.reading&&!strcmp(saved.path,reader));
 app_font_end_lock(&saved);assert(s_reading&&!system_face&&!system_scale&&!strcmp(active,reader));
 app_font_activate_system();unsigned before=opens;
 app_font_begin_lock(&saved);assert(!s_reading&&system_face&&system_scale&&!strcmp(active,chosen));
 app_font_end_lock(&saved);assert(!s_reading&&system_face&&system_scale&&!strcmp(active,chosen)&&opens==before);
 app_font_activate_reading();app_font_begin_lock(&saved);strcpy(missing,reader);
 app_font_end_lock(&saved);assert(s_reading&&!system_face&&!system_scale&&ttf_font_is_builtin());
 strcpy(missing,chosen);app_font_begin_lock(&saved);assert(ttf_font_is_builtin()&&!system_face&&system_scale);
 assert(!strcmp(chosen,"/fonts/system.ttf")&&!strcmp(reader,"/fonts/reader.ttf"));
 app_font_end_lock(&saved);assert(s_reading&&!system_face&&!system_scale);
 missing[0]=chosen[0]=0;app_font_begin_lock(&saved);assert(ttf_font_is_builtin()&&!system_face&&system_scale);
 app_font_end_lock(&saved);assert(s_reading&&!system_scale);
 strcpy(chosen,"/fonts/system.ttf");recovery=true;
 app_font_begin_lock(&saved);assert(ttf_font_is_builtin()&&!system_face);app_font_end_lock(&saved);
 app_font_begin_lock(NULL);app_font_end_lock(NULL);
 puts("PASS: tickets from reader/system use the same system face/scale; wake restores reader context; missing card/font falls back safely without overwriting saved choices");
}
'''
with tempfile.TemporaryDirectory(dir=ROOT / 'build') as folder:
    c, exe = Path(folder) / 'test.c', Path(folder) / 'test'
    c.write_text(unit)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
