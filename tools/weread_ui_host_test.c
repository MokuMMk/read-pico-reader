// SPDX-License-Identifier: Apache-2.0
// 真正页面的导航、取消、分页与下载回归。/ Real page navigation, cancel, paging and download regression.
#include <assert.h>
#include <stdio.h>
#include "weread_ui_stubs/test_env.h"
bool weread_configure(const char *c,const char *b){return !strcmp(c,"/sdcard/.readpico/weread")&&!strcmp(b,"/sdcard/books");}
bool weread_start(weread_action_t a,unsigned p,unsigned i){test_start_count++;test_action=a;test_page=p;test_index=i;test_snapshot.action=a;test_snapshot.active=true;test_snapshot.state=WEREAD_WORKING;test_snapshot.revision++;return true;}
bool weread_start_batch(unsigned page,const weread_selection_t *q,unsigned n){
 assert(n&&n<=WEREAD_BATCH_MAX);memcpy(test_selection,q,n*sizeof(*q));test_selection_count=n;
 return weread_start(WEREAD_BATCH,page,0);
}
void weread_snapshot(weread_snapshot_t *v){*v=test_snapshot;}
bool weread_set_include_images(bool enabled){test_images=enabled;return true;}
void weread_stop(void){test_stop_count++;test_snapshot.active=false;test_snapshot.state=WEREAD_CANCELLED;test_snapshot.revision++;}
bool app_book_request_open(const char *path){assert(!strcmp(path,"/sdcard/books/test.epub"));test_book_open++;return true;}
void app_transfer_request_wifi_setup(void){test_setup_count++;}
const app_desc_t app_book={.title="阅读"},app_transfer={.title="传书"},app_files={.title="文件"};
#include "../main/apps/app_weread.c"
static void tap(app_ctx_t *ctx,EpdRect r){ui_gesture_event_t e={.type=UI_GESTURE_TAP,.x0=r.x+r.width/2,.y0=r.y+r.height/2};on_gesture(ctx,&e);}
int main(void){
 app_ctx_t ctx={0}; on_enter(&ctx); assert(s_ready&&test_bulk&&test_action==WEREAD_LOAD);
 test_snapshot.active=false;test_snapshot.state=WEREAD_IDLE;test_snapshot.count=7;test_snapshot.total=10;
 for(unsigned i=0;i<7;i++){snprintf(test_snapshot.books[i].id,64,"id-%u",i);strcpy(test_snapshot.books[i].title,"长字体名称与中文书籍标题");strcpy(test_snapshot.books[i].author,"作者");}
 refresh_snapshot(); tap(&ctx,row_rect(3)); assert(s_selected==3);
 test_text[0]=0;render(&ctx,NULL);assert(strstr(test_text,"书籍下载")&&!strstr(test_text,"同步书架")&&!strstr(test_text,"退出登录"));
 tap(&ctx,image_rect()); assert(!s_images); tap(&ctx,download_rect());
 assert(test_action==WEREAD_DOWNLOAD&&test_index==3&&!test_images);
 int starts=test_start_count;tap(&ctx,download_rect());tap(&ctx,next_rect());assert(test_start_count==starts);
 tap(&ctx,cancel_rect());assert(!s_view.active&&test_stop_count==1);
 s_selected=-1;tap(&ctx,next_rect());assert(test_page==1&&test_action==WEREAD_LOAD);
 test_snapshot.active=false;test_snapshot.page=1;test_snapshot.count=3;test_snapshot.state=WEREAD_IDLE;test_snapshot.revision++;refresh_snapshot();
 tap(&ctx,row_rect(0));tap(&ctx,download_rect());assert(test_index==7);
 weread_stop();strcpy(test_snapshot.books[0].local_path,"/sdcard/books/test.epub");refresh_snapshot();
 tap(&ctx,download_rect());assert(test_book_open==1&&ctx.request_app==&app_book);
 ctx.request_app=NULL;s_selected=-1;test_snapshot.logged_in=true;refresh_snapshot();tap(&ctx,logout_rect());assert(s_logout_confirm);
 tap(&ctx,(EpdRect){82,604,240,74});assert(!s_logout_confirm&&test_action==WEREAD_DOWNLOAD);
 tap(&ctx,logout_rect());tap(&ctx,(EpdRect){362,604,240,74});assert(test_action==WEREAD_LOGOUT);
 weread_stop();test_snapshot.count=0;test_snapshot.logged_in=false;refresh_snapshot();tap(&ctx,(EpdRect){36,703,612,78});assert(test_setup_count==1);
 ctx.request_app=NULL;tap(&ctx,(EpdRect){36,79,56,56});assert(ctx.request_app==&app_files&&!ctx.request_return);ctx.request_app=NULL;
 // 跨页多选不丢失，确认页只启动一个串行批次。/ Cross-page selections dispatch one serial batch.
 test_snapshot.count=7;test_snapshot.total=10;test_snapshot.page=0;test_snapshot.state=WEREAD_IDLE;refresh_snapshot();
 tap(&ctx,select_rect());assert(s_multiselect);tap(&ctx,row_rect(0));tap(&ctx,row_rect(3));assert(s_selection_count==2);
 tap(&ctx,next_rect());assert(test_page==1);test_snapshot.active=false;test_snapshot.page=1;test_snapshot.count=3;
 for(unsigned i=0;i<3;i++)snprintf(test_snapshot.books[i].id,64,"id-%u",i+7);refresh_snapshot();
 tap(&ctx,row_rect(2));assert(s_selection_count==3);tap(&ctx,batch_rect());assert(s_batch_panel);
 test_text[0]=0;render(&ctx,NULL);assert(strstr(test_text,"批量下载")&&!strstr(test_text,"同步书架")&&!strstr(test_text,"退出登录"));
 tap(&ctx,download_rect());assert(test_action==WEREAD_BATCH&&test_selection_count==3);
 assert(!strcmp(test_selection[0].id,"id-0")&&!strcmp(test_selection[1].id,"id-3")&&!strcmp(test_selection[2].id,"id-9"));
 starts=test_start_count;tap(&ctx,download_rect());assert(test_start_count==starts);
 tap(&ctx,(EpdRect){36,79,56,56});assert(!s_view.active&&!s_batch_panel&&s_multiselect);
 tap(&ctx,prev_rect());test_snapshot.active=false;test_snapshot.page=0;test_snapshot.count=7;
 for(unsigned i=0;i<7;i++)snprintf(test_snapshot.books[i].id,64,"id-%u",i);refresh_snapshot();
 assert(selection_index("id-0")==0&&selection_index("id-3")==1);tap(&ctx,row_rect(0));assert(s_selection_count==2);
 tap(&ctx,select_rect());assert(!s_selection_count);tap(&ctx,sync_rect());assert(s_selection_count==7);
 tap(&ctx,logout_rect());assert(!s_multiselect);tap(&ctx,(EpdRect){36,79,56,56});assert(ctx.request_app==&app_files&&!ctx.request_return);
 test_snapshot.changed=1;refresh_snapshot();refresh_snapshot();assert(test_notify_count==1);
 test_snapshot.state=WEREAD_QR;strcpy(test_snapshot.qr,"https://weread.qq.com/web/confirm?uid=public");refresh_snapshot();assert(s_qr_ok);render(&ctx,NULL);
 on_before_lock(&ctx);assert(!s_view.active);
 weread_on_exit(&ctx);assert(!s_view_storage&&!s_selection&&!test_bulk);
 test_sd=false;on_enter(&ctx);assert(!s_ready);starts=test_start_count;tap(&ctx,sync_rect());assert(test_start_count==starts);render(&ctx,NULL);weread_on_exit(&ctx);
 test_sd=true;test_alloc_fail=true;on_enter(&ctx);assert(!s_ready&&!s_view_storage);render(&ctx,NULL);weread_on_exit(&ctx);
 puts("PASS: WeRead real-page download policy, pagination, busy guards, cancellation, logout, open, lock, missing SD and allocation failure");
}
