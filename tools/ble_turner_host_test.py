#!/usr/bin/env python3
"""Exercise production HID input, learning feedback and BLE/network lifecycle under faults.

SPDX-FileCopyrightText: 2026 mindreset
SPDX-License-Identifier: Apache-2.0
中文：抽取真实函数，模拟报告、内存与任务失败。/ English: extract real functions and inject report, memory and task faults.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BLE = ROOT / 'components/ble_page_turner/src/ble_page_turner.c'
SETTINGS = ROOT / 'main/apps/app_device_settings.c'

def function(path, name):
    source = path.read_text()
    match = re.search(r'^(?:static )?[^\n]+\b' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    start, pos, depth = match.start(), match.end(), 1
    # Remove comments while keeping byte positions, then count braces outside strings.
    cleaned = re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: ' ' * len(m[0]), source, flags=re.S)
    quote, escaped = None, False
    while depth:
        c = cleaned[pos]
        if quote:
            if escaped: escaped = False
            elif c == '\\': escaped = True
            elif c == quote: quote = None
        elif c in '\"\'': quote = c
        elif c == '{': depth += 1
        elif c == '}': depth -= 1
        pos += 1
    return source[start:pos]

unit = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ble_page_turner.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define configMAX_PRIORITIES 25
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 4096
#define CONFIG_BT_NIMBLE_PINNED_TO_CORE 0
#define BLE_SM_IO_CAP_NO_IO 0
#define BLE_HS_CONN_HANDLE_NONE 65535
#define KEY_RING_LEN 16
#define RAW_RING_LEN 17
#define FRAME_MAX 16
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define BLE_PT_MIN_FREE_INTERNAL (56*1024)
#define BLE_PT_MIN_LARGEST_INTERNAL (20*1024)
#define BLE_PT_MIN_FREE_PSRAM (256*1024)
static atomic_flag s_lifecycle=ATOMIC_FLAG_INIT;
static atomic_uint s_network_users;
static atomic_bool s_running;
static bool s_connected=true,s_connecting,s_scanning,s_link_up;
static uint16_t s_conn_handle;
static uint32_t s_stable_since;
static void *s_host_task;
static char s_failure[128];
static bool s_has_keyboard_page=true,s_rest_known;
static uint8_t s_preferred_byte=2,s_held_usage,s_held_mods;
static uint32_t s_held_since,s_last_repeat,s_input_serial;
static uint8_t s_rest[FRAME_MAX],s_last[FRAME_MAX];
static size_t s_frame_len;
static int s_ring_lock;
static struct {ble_pt_event_t items[KEY_RING_LEN];uint8_t head,tail;}s_keys;
static struct {ble_pt_raw_t items[RAW_RING_LEN];uint8_t head,tail;}s_raws;
static int64_t clock_us;
static int64_t esp_timer_get_time(void){return clock_us;}
static void vTaskDelay(unsigned ms){clock_us+=(int64_t)ms*1000;}
static size_t free_internal=150*1024,largest_internal=60*1024,free_psram=2*1024*1024;
static size_t heap_caps_get_free_size(int c){return c==MALLOC_CAP_SPIRAM?free_psram:free_internal;}
static size_t heap_caps_get_largest_free_block(int c){(void)c;return largest_internal;}
static int init_count,deinit_count,stop_count,task_count,init_error,task_error,stop_error;
static esp_err_t nimble_port_init(void){++init_count;return init_error;}
static esp_err_t nimble_port_deinit(void){++deinit_count;return ESP_OK;}
static int nimble_port_stop(void){++stop_count;return stop_error;}
static void host_task(void*p){(void)p;}
static void on_sync(void){}
static void bonds_load(void){}
static struct {void(*sync_cb)(void);int sm_io_cap,sm_bonding,sm_mitm,sm_sc,sm_our_key_dist,sm_their_key_dist;}ble_hs_cfg;
static int xTaskCreatePinnedToCore(void(*fn)(void*),const char*name,int stack,void*param,int priority,void**handle,int core){
 assert(fn==host_task && !strcmp(name,"nimble_host") && stack==4096 && !param && priority==21 && core==0);
 ++task_count;if(task_error)return 0;*handle=(void*)1;return pdPASS;
}
void ble_pt_scan_stop(void){s_scanning=false;}
void ble_pt_disconnect(void){s_connected=false;}
static int s_ble_scroll,s_ble_learning;
static char s_ble_feedback[64];
bool ble_pt_connected(void){return s_connected;}
static int binding_error;
static uint32_t saved_code;
static ble_pt_action_t saved_action;
static struct {char addr[18];uint32_t codes[BLE_PT_MAX_BINDINGS];} s_bindings;
static uint32_t persisted[BLE_PT_MAX_BINDINGS];
static int binding_handles;
typedef int nvs_handle_t;
#define NVS_NS_BINDINGS "bindings"
#define NVS_READWRITE 1
static esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h){assert(!strcmp(ns,NVS_NS_BINDINGS)&&mode==1);if(binding_error==1)return ESP_FAIL;*h=1;binding_handles++;return ESP_OK;}
static const char *bond_key(char*k,size_t cap,const char*addr){assert(addr[0]);snprintf(k,cap,"test");return k;}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*in,size_t len){assert(h==1&&!strcmp(k,"test")&&len==sizeof(persisted));if(binding_error==2)return ESP_FAIL;memcpy(persisted,in,len);return ESP_OK;}
static esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);return binding_error==3?ESP_FAIL:ESP_OK;}
static void nvs_close(nvs_handle_t h){assert(h==1);binding_handles--;}

'''
for name in ('lifecycle_take','lifecycle_give','ble_pt_input_serial','key_push','raw_push','extract_primary_code',
             'usage_to_special','emit_usage','ingest_report','ble_pt_pop_key','ble_pt_pop_raw',
             'ble_pt_raw_code','ble_pt_bind','start_locked','stop_locked','ble_pt_start','ble_pt_stop',
             'ble_pt_network_acquire','ble_pt_network_release'):
    unit += '\n' + function(BLE, name)
unit += '\n' + function(SETTINGS, 'ble_receive_feedback')
unit += '\n' + function(SETTINGS, 'format_bound_key')
unit += r'''
static void reset_input(void){
 memset(&s_keys,0,sizeof(s_keys));memset(&s_raws,0,sizeof(s_raws));
 memset(s_rest,0,sizeof(s_rest));memset(s_last,0,sizeof(s_last));
 s_rest_known=false;s_held_usage=s_held_mods=0;s_frame_len=0;s_connected=true;
 s_ble_feedback[0]=0;s_ble_learning=0;binding_error=0;s_ble_scroll=200;
 snprintf(s_bindings.addr,sizeof(s_bindings.addr),"AA:BB:CC:DD:EE:FF");memset(s_bindings.codes,0,sizeof(s_bindings.codes));
}
static void report(int len,int id,int key){uint8_t data[9]={0};data[len==9?0:8]=(uint8_t)id;data[len==9?3:2]=(uint8_t)key;ingest_report(data,(size_t)len);}
int main(void){
 char label[48];
 format_bound_key(0x88,label,sizeof(label));assert(!strcmp(label,"0x88"));
 format_bound_key(0x288,label,sizeof(label));assert(!strcmp(label,"0x88 b2"));
 format_bound_key(0x10288,label,sizeof(label));assert(!strcmp(label,"0x88 r1 b2"));
 format_bound_key(0xffffff,label,sizeof(label));assert(!strcmp(label,"0xFF r255 b255"));
 char tiny[3];format_bound_key(0x10288,tiny,sizeof(tiny));assert(tiny[2]==0);
 char sentinel='x';format_bound_key(0x88,&sentinel,0);assert(sentinel=='x');
 // 首次按下、重复帧、释放和报告 ID 不需要先点屏幕。/ First presses, duplicate frames, releases and report IDs need no touch.
 for(int len=8;len<=9;len++){
  reset_input();s_ble_learning=1;report(len,len==9?7:0,0x4b);
  assert(s_rest_known);assert(ble_receive_feedback());
  saved_code=s_bindings.codes[0];saved_action=BLE_PT_ACTION_PREV;
  assert(!s_ble_learning && s_ble_scroll==0 && saved_action==BLE_PT_ACTION_PREV);
  assert(saved_code==((uint32_t)0x4b | (uint32_t)(len==9?3:2)<<8 | (uint32_t)(len==9?7:0)<<16));
  format_bound_key(saved_code,label,sizeof(label));assert(!strcmp(label,len==9?"0x4B r7 b3":"0x4B b2"));
  assert(strstr(s_ble_feedback,"识别成功")&&strstr(s_ble_feedback,"上一页"));
  report(len,len==9?7:0,0x4b);assert(!ble_receive_feedback());
  report(len,len==9?7:0,0);assert(!s_held_usage && !ble_receive_feedback());
  s_ble_feedback[0]=0;s_ble_learning=2;report(len,len==9?7:0,0x4e);
  assert(ble_receive_feedback() && s_bindings.codes[1] && strstr(s_ble_feedback,"下一页"));
 }
 reset_input();s_ble_learning=2;binding_error=3;report(8,0,0x4e);
 assert(ble_receive_feedback()&&strstr(s_ble_feedback,"保存失败")&&!s_ble_learning);
 reset_input();report(8,0,0x4b);assert(ble_receive_feedback()&&strstr(s_ble_feedback,"已收到"));
 // 有界队列、断连和释放不造成误绑定。/ Bound queues and ignore releases or disconnected input.
 reset_input();for(int n=0;n<100;n++){report(8,0,n%2?0:0x4b);}assert(ble_receive_feedback());
 reset_input();report(8,0,0x4b);s_connected=false;s_ble_learning=1;
 assert(!ble_receive_feedback()&&s_ble_learning==1);
 // 内存不够时不进入 NimBLE；任务失败完整回收。/ Reject before NimBLE on low memory; unwind a failed host task.
 for(int kind=0;kind<3;kind++){
  free_internal=150*1024;largest_internal=60*1024;free_psram=2*1024*1024;
  if(kind==0)free_internal=55*1024;if(kind==1)largest_internal=19*1024;if(kind==2)free_psram=255*1024;
  int before=init_count;assert(ble_pt_start()==ESP_ERR_NO_MEM && !s_running && init_count==before);
 }
 free_internal=150*1024;largest_internal=60*1024;free_psram=2*1024*1024;
 task_error=1;assert(ble_pt_start()==ESP_ERR_NO_MEM && !s_running && task_count==1 && deinit_count==1);
 task_error=0;assert(ble_pt_start()==ESP_OK && s_running);
 stop_error=1;assert(ble_pt_network_acquire()==ESP_FAIL && atomic_load(&s_network_users)==0 && s_running);
 stop_error=0;assert(ble_pt_network_acquire()==ESP_OK && !s_running && atomic_load(&s_network_users)==1);
 int before=init_count;assert(ble_pt_start()==ESP_ERR_INVALID_STATE && init_count==before);
 assert(ble_pt_network_acquire()==ESP_OK && atomic_load(&s_network_users)==2);
 ble_pt_network_release();assert(ble_pt_start()==ESP_ERR_INVALID_STATE);
 ble_pt_network_release();ble_pt_network_release();assert(!atomic_load(&s_network_users));
 assert(ble_pt_start()==ESP_OK);assert(ble_pt_stop(2000)==ESP_OK && !s_running);
 atomic_flag_test_and_set(&s_lifecycle);assert(ble_pt_start()==ESP_ERR_TIMEOUT && !s_running);lifecycle_give();
 for(int n=0;n<100;n++){assert(ble_pt_start()==ESP_OK);assert(ble_pt_network_acquire()==ESP_OK);ble_pt_network_release();}
 assert(!s_running && !atomic_load(&s_network_users));
 puts("PASS: BLE first key, report ID/release, learning feedback, bounded queues, memory/task failures and network resource leases");
}
'''
# Active tick dispatch must consume reports before a state-only redraw can return.
assert 'ble_receive_feedback()' in function(SETTINGS, 'on_tick')
assert 'while (ble_pt_pop_raw(&stale))' in function(SETTINGS, 'on_gesture')
transfer = (ROOT / 'components/read_pico_transfer/read_pico_transfer.c').read_text()
for name in ('read_pico_transfer_start', 'read_pico_transfer_scan_wifi', 'read_pico_transfer_sync_time'):
    assert 'ble_pt_network_acquire()' in function(ROOT / 'components/read_pico_transfer/read_pico_transfer.c', name)
assert transfer.count('ble_pt_network_acquire()') == 3
with tempfile.TemporaryDirectory(prefix='pico-ble-test-') as directory:
    work = Path(directory)
    (work/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\nenum {ESP_OK,ESP_FAIL,ESP_ERR_NO_MEM,ESP_ERR_TIMEOUT,ESP_ERR_INVALID_STATE,ESP_ERR_INVALID_ARG};\n')
    (work/'test.c').write_text(unit)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',
                    '-I'+str(work),'-I'+str(ROOT/'components/ble_page_turner/include'),str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
