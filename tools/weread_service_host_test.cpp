// SPDX-License-Identifier: Apache-2.0
// 实际后台代码的串行队列、重排序、错误、取消与内存回归。
// Real worker regressions for serialization, reordering, errors, cancellation and memory.
#include <cassert>
#include "../components/pico_weread/weread_service.cpp"
static void run(){assert(fake_worker);auto fn=fake_worker;fake_worker=nullptr;fn(nullptr);}
int main(){
 assert(weread_configure("/sdcard/cache","/sdcard/books"));
 weread_selection_t queue[3]={};
 for(unsigned i=0;i<3;++i){queue[i].index=i;snprintf(queue[i].id,64,"%s",fake_ids[i].c_str());}
 assert(weread_start_batch(0,queue,3));assert(!weread_start(WEREAD_SYNC,0,0));
 // 派发后更改调用方数据和书架顺序，后台仍按原书号下载。/ Caller mutations and shelf reordering preserve IDs.
 fake_ids={"c","a","b"};strcpy(queue[0].id,"changed");run();
 assert((fake_started==std::vector<std::string>{"a","b","c"}));
 assert(fake_peak_operations==1&&!fake_active_operations&&!s_queue&&!s_queue_count&&!s_status.active);
 assert(s_status.batch_current==3&&s_status.batch_success==2&&s_status.batch_failed==1&&s_status.error==12&&s_status.state==WEREAD_FAILED);
 assert(s_status.changed==2&&fake_network_stops==0);
 strcpy(queue[0].id,"a");strcpy(queue[1].id,"a");assert(!weread_start_batch(0,queue,2));
 strcpy(queue[1].id,"b");fake_started.clear();fake_reset_cancel=true;fake_after_reset=[](){s_cancel.store(true);};
 assert(weread_start_batch(0,queue,3));run();
 assert(fake_started.size()==1&&s_status.state==WEREAD_CANCELLED&&!s_status.active&&!s_queue);
 fake_cancel=false;fake_task_failure=true;assert(!weread_start_batch(0,queue,3));assert(!s_queue&&!s_status.active);
 fake_task_failure=false;fake_alloc_failure=true;assert(!weread_start_batch(0,queue,3));assert(!s_status.active);
 fake_alloc_failure=false;assert(!weread_start_batch(0,nullptr,3));assert(!weread_start_batch(0,queue,WEREAD_BATCH_MAX+1));
 fake_started.clear();assert(weread_start(WEREAD_DOWNLOAD,0,1));run();assert(fake_started.size()==1);
 heap_caps_free(s_status_storage);s_status_storage=nullptr;
 puts("PASS: real worker sequential batch, owned queue, ID resolution, per-book failure, cancellation, allocation failure and STA preservation");
}
