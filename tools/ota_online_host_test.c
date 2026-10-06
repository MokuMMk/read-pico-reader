/* SPDX-License-Identifier: Apache-2.0
 * 中文：真实 OTA 任务的断流、校验、取消和启动提交。/ English: Real OTA interruption, verification, cancel and boot commit. */
#include "common.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../main/ota_online.c"
static const esp_partition_t running={0x10000,0x400000},inactive={0x910000,0x400000},metadata={0xd10000,0x2000};
static const esp_app_desc_t current={.version="0.3.3-rc78",.project_name="Read_Pico"};
static uint8_t firmware[4096],written[4096];static char feed[1600];
static size_t written_size,limit;static int boot_calls,abort_calls,begin_calls,end_error,fail_task;
static bool bad_layout,same_slot,cancel_read,again;
static size_t drop_at;
static int drops,open_failures,range_requests,http_opens;
static bool wrong_range,ignored_range;
static wifi_ps_type_t wifi_ps;
static int ps_disabled,ps_restored;
static int offline_polls;
static bool cancel_reconnect;
static void (*pending)(void*);
static int64_t now;
struct FakeHttp {const uint8_t *data;size_t size,at,offset;bool firmware;int (*event_handler)(esp_http_client_event_t*);void *user_data;};
void *heap_caps_malloc(size_t n,int c){(void)c;return malloc(n);}
void *heap_caps_calloc(size_t n,size_t s,int c){(void)c;return calloc(n,s);}
void heap_caps_free(void *p){free(p);}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
SemaphoreHandle_t xSemaphoreCreateBinary(void){return (void*)2;}
static void run(void){assert(pending);void(*f)(void*)=pending;pending=NULL;f(NULL);}
int xSemaphoreTake(SemaphoreHandle_t s,uint32_t wait){(void)wait;if(s==(void*)2&&pending)run();return 1;}
int xSemaphoreGive(SemaphoreHandle_t s){(void)s;return 1;}
int xTaskCreate(void(*f)(void*),const char*n,unsigned stack,void*a,unsigned p,void*t){(void)n;(void)a;(void)p;(void)t;assert(stack==12288);if(fail_task)return 0;pending=f;return pdPASS;}
void vTaskDelete(void *p){(void)p;}
void vTaskDelay(unsigned ms){now+=(int64_t)ms*1000;}
int64_t esp_timer_get_time(void){return now+=1000;}
const esp_app_desc_t *esp_app_get_description(void){return &current;}
const esp_partition_t *esp_partition_find_first(int type,int sub,const char *label){(void)label;if(bad_layout)return NULL;if(type==1)return &metadata;return sub==16?&running:&inactive;}
const esp_partition_t *esp_ota_get_running_partition(void){return &running;}
const esp_partition_t *esp_ota_get_next_update_partition(void*p){(void)p;return same_slot?&running:&inactive;}
esp_err_t esp_ota_begin(const esp_partition_t*p,size_t n,esp_ota_handle_t*h){assert(p==&inactive&&n==4096);++begin_calls;written_size=0;*h=7;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *p,size_t n){assert(h==7&&written_size+n<=sizeof(written));memcpy(written+written_size,p,n);written_size+=n;return ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t h){assert(h==7);++abort_calls;return ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t h){assert(h==7);return end_error;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p){assert(p==&inactive);++boot_calls;return ESP_OK;}
void read_pico_transfer_get_status(read_pico_transfer_status_t*s){*s=(read_pico_transfer_status_t){.mode=1,.network_ready=offline_polls==0};}
esp_err_t read_pico_transfer_get_saved_wifi(char*s,bool*b){strcpy(s,"test");*b=true;return ESP_OK;}
void read_pico_transfer_stop(void){}
esp_err_t read_pico_transfer_start(const read_pico_transfer_cfg_t*c){(void)c;return ESP_OK;}
void read_pico_transfer_service_poll(void){if(offline_polls){--offline_polls;if(cancel_reconnect)atomic_store(&s_cancel,true);}}
esp_err_t esp_wifi_get_ps(wifi_ps_type_t*p){*p=wifi_ps;return ESP_OK;}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t p){wifi_ps=p;if(p==WIFI_PS_NONE)++ps_disabled;else ++ps_restored;return ESP_OK;}
esp_err_t read_pico_transfer_sync_time_online(uint32_t*p){*p=1791200000;return ESP_OK;}
int esp_crt_bundle_attach(void*p){(void)p;return ESP_OK;}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*c){assert(c->disable_auto_redirect&&c->crt_bundle_attach&&c->timeout_ms==15000);struct FakeHttp*h=calloc(1,sizeof(*h));h->firmware=!strstr(c->url,"update.json");h->data=h->firmware?firmware:(const uint8_t*)feed;h->size=h->firmware?sizeof(firmware):strlen(feed);h->event_handler=c->event_handler;h->user_data=c->user_data;return h;}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h,const char*k,const char*v){if(!strcmp(k,"Range")){unsigned long at=0;assert(sscanf(v,"bytes=%lu-",&at)==1);h->offset=at;h->at=ignored_range?0:at;++range_requests;}return ESP_OK;}
esp_err_t esp_http_client_open(esp_http_client_handle_t h,int n){(void)n;++http_opens;assert(wifi_ps==WIFI_PS_NONE);if(h->firmware&&open_failures){--open_failures;return ESP_FAIL;}return ESP_OK;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h){if(h->offset&&!ignored_range){char value[96];snprintf(value,sizeof(value),"bytes %lu-%lu/%lu",(unsigned long)(h->offset+(wrong_range?1:0)),(unsigned long)h->size-1,(unsigned long)h->size);esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_HEADER,.user_data=h->user_data,.header_key="Content-Range",.header_value=value};assert(h->event_handler(&event)==ESP_OK);}return h->size-(ignored_range?0:h->offset);}
int esp_http_client_get_status_code(esp_http_client_handle_t h){return h->offset&&!ignored_range?206:200;}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t h,int ms){(void)h;(void)ms;return ESP_OK;}
int esp_http_client_read(esp_http_client_handle_t h,char*p,int cap){if(again){again=false;return -ESP_ERR_HTTP_EAGAIN;}if(h->firmware&&drops&&h->at>=drop_at){--drops;offline_polls=5;return -1;}size_t end=h->firmware?limit:h->size;if(h->at>=end)return 0;size_t n=end-h->at;if(n>37)n=37;if(n>(size_t)cap)n=cap;memcpy(p,h->data+h->at,n);h->at+=n;if(cancel_read&&h->firmware&&h->at>500)atomic_store(&s_cancel,true);return (int)n;}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h){return h->at==h->size;}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h){free(h);return ESP_OK;}
int psa_crypto_init(void){return PSA_SUCCESS;}
int psa_hash_setup(psa_hash_operation_t*h,int alg){assert(alg==PSA_ALG_SHA_256);h->bytes=0;return PSA_SUCCESS;}
int psa_hash_update(psa_hash_operation_t*h,const void*p,size_t n){(void)p;h->bytes+=n;return PSA_SUCCESS;}
int psa_hash_finish(psa_hash_operation_t*h,uint8_t*p,size_t cap,size_t*n){assert(cap==32&&h->bytes==4096);memset(p,0,32);*n=32;return PSA_SUCCESS;}
int psa_hash_abort(psa_hash_operation_t*h){(void)h;return PSA_SUCCESS;}
static void fixture(void){pico_online_cancel_join();memset(firmware,0,sizeof(firmware));esp_image_header_t header={.magic=0xe9,.chip_id=9};memcpy(firmware,&header,sizeof(header));esp_app_desc_t desc={.magic_word=ESP_APP_DESC_MAGIC_WORD,.version="0.3.3-rc79",.project_name="Read_Pico"};memcpy(firmware+32,&desc,sizeof(desc));snprintf(feed,sizeof(feed),"{\"schema\":1,\"version\":\"0.3.3-rc79\",\"project\":\"Read_Pico\",\"board\":\"RDP-G01-W\",\"layout\":\"pico-dual-4m-v1\",\"minimum_base_version\":\"0.3.3-rc72\",\"url\":\"https://wegooo-cell.github.io/read-pico-reader/Pico-update-0.3.3-rc79.bin\",\"sha256\":\"%064d\",\"notes\":\"test\",\"size\":4096}",0);limit=4096;bad_layout=same_slot=cancel_read=again=false;written_size=0;boot_calls=abort_calls=begin_calls=end_error=fail_task=0;}
static void check_download(void){wifi_ps=WIFI_PS_MIN_MODEM;ps_disabled=ps_restored=0;assert(pico_online_check()==ESP_OK);assert(pico_online_busy());run();assert(s_status->state==PICO_UPDATE_AVAILABLE&&!pico_online_busy());assert(wifi_ps==WIFI_PS_MIN_MODEM&&ps_disabled==1&&ps_restored==1);assert(pico_online_download()==ESP_OK);run();assert(wifi_ps==WIFI_PS_MIN_MODEM&&ps_disabled==2&&ps_restored==2);}
int main(void){
 _Static_assert(sizeof(esp_image_header_t)==24&&sizeof(esp_app_desc_t)==256,"real header sizes");
 fixture();again=true;check_download();assert(s_status->state==PICO_UPDATE_READY&&written_size==4096&&!memcmp(firmware,written,4096)&&!boot_calls);assert(pico_online_commit()==ESP_OK&&boot_calls==1);
 fixture();limit=1024;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&abort_calls==1&&!boot_calls&&range_requests==3);assert(pico_online_commit()!=ESP_OK);range_requests=0;
 fixture();drop_at=100;drops=1;check_download();assert(s_status->state==PICO_UPDATE_READY&&written_size==4096&&!memcmp(firmware,written,4096)&&begin_calls==1&&range_requests==1);range_requests=0;
 fixture();drop_at=1000;drops=2;check_download();assert(s_status->state==PICO_UPDATE_READY&&written_size==4096&&!memcmp(firmware,written,4096)&&begin_calls==1&&range_requests==2);range_requests=0;
 fixture();drop_at=1000;drops=1;cancel_reconnect=true;check_download();assert(s_status->state==PICO_UPDATE_CANCELLED&&abort_calls==1&&!boot_calls);cancel_reconnect=false;offline_polls=0;
 fixture();open_failures=2;check_download();assert(s_status->state==PICO_UPDATE_READY&&written_size==4096);
 fixture();drop_at=1000;drops=1;wrong_range=true;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&abort_calls==1&&!boot_calls);wrong_range=false;range_requests=0;
 fixture();drop_at=1000;drops=1;ignored_range=true;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&abort_calls==1&&!boot_calls);ignored_range=false;range_requests=0;
 fixture();drop_at=1000;drops=20;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&abort_calls==1&&!boot_calls&&range_requests==3);drops=range_requests=0;
 fixture();cancel_read=true;check_download();assert(s_status->state==PICO_UPDATE_CANCELLED&&!boot_calls&&abort_calls==1);
 fixture();check_download();pico_online_cancel_join();assert(pico_online_commit()!=ESP_OK&&!boot_calls);
 fixture();char *sha=strstr(feed,"0000000000");assert(sha);sha[0]='1';check_download();assert(s_status->state==PICO_UPDATE_FAILED&&abort_calls==1&&!boot_calls);
 fixture();firmware[80]='X';check_download();assert(s_status->state==PICO_UPDATE_FAILED&&!begin_calls&&!boot_calls);
 fixture();same_slot=true;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&!begin_calls);
 fixture();end_error=ESP_FAIL;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&!boot_calls);
 fixture();bad_layout=true;assert(pico_online_check()==ESP_OK);run();assert(s_status->state==PICO_UPDATE_FAILED&&!begin_calls&&wifi_ps==WIFI_PS_MIN_MODEM);
 fixture();assert(pico_online_check()==ESP_OK);pico_online_cancel_join();assert(!pico_online_busy()&&s_status->state==PICO_UPDATE_CANCELLED);
 fixture();fail_task=1;assert(pico_online_check()==ESP_ERR_NO_MEM&&!pico_online_busy());
 free(s_status);s_status=NULL;
 puts("PASS: OTA split headers, EAGAIN, bounded Range resume, rejected wrong/ignored ranges, radio power-save restoration, hash/project checks, cancellation, inactive slot and explicit boot commit");
}
