/* SPDX-License-Identifier: Apache-2.0
 * 读取响应头已缓存全部正文的SDK状态。/ Exercise SDK header-prefetched complete bodies. */
#include "WeReadHttpClient.h"
#include "read_pico_transfer.h"
#include <cassert>
#include <cstring>
#include <string>
#include <algorithm>
#include <cstdio>
struct FakeClient {};
static FakeClient client;
static std::string body;
static size_t position;
static bool headerComplete, truncated, cancelled, again;
static unsigned reads, cleanups, initializations, headerDeletes;
static bool failOpen;
static void *userData;
bool pico_weread_cancelled() { return cancelled; }
extern "C" void read_pico_transfer_get_status(read_pico_transfer_status_t* s) { s->mode=READ_PICO_TRANSFER_MODE_STA;s->network_ready=true; }
extern "C" int64_t esp_timer_get_time() { static int64_t now; return now+=100; }
extern "C" esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }
extern "C" esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* c) { assert(c->crt_bundle_attach==esp_crt_bundle_attach && c->disable_auto_redirect);++initializations;return &client; }
extern "C" esp_err_t esp_http_client_cleanup(esp_http_client_handle_t) { ++cleanups;return ESP_OK; }
extern "C" esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char*,const char*) { return ESP_OK; }
extern "C" esp_err_t esp_http_client_set_url(esp_http_client_handle_t,const char*) { return ESP_OK; }
extern "C" esp_err_t esp_http_client_set_method(esp_http_client_handle_t,int) { return ESP_OK; }
extern "C" esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t,void* p) { userData=p;return ESP_OK; }
extern "C" esp_err_t esp_http_client_delete_header(esp_http_client_handle_t,const char*) { ++headerDeletes;return ESP_OK; }
extern "C" esp_err_t esp_http_client_open(esp_http_client_handle_t,int) { return failOpen ? 1 : ESP_OK; }
extern "C" int esp_http_client_write(esp_http_client_handle_t,const char*,int n) { return n; }
extern "C" int64_t esp_http_client_fetch_headers(esp_http_client_handle_t) { return body.size(); }
extern "C" int esp_http_client_get_status_code(esp_http_client_handle_t) { return 200; }
extern "C" esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t,int) { return ESP_OK; }
extern "C" bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) { return !truncated && (headerComplete || position==body.size()); }
extern "C" int esp_http_client_read(esp_http_client_handle_t,char* out,int cap) { ++reads;if(again){again=false;return -ESP_ERR_HTTP_EAGAIN;}size_t n=std::min(body.size()-position,static_cast<size_t>(cap));memcpy(out,body.data()+position,n);position+=n;return n; }
static void fixture(const char* text,bool complete) { body=text;position=0;headerComplete=complete;truncated=cancelled=again=false;reads=cleanups=0; }
static WeReadHttpClient::Result run(std::string& output,bool cancel=false) {
 uint8_t buffer[7];int status=0;WeReadHttpClient::RequestOptions options;options.readBuffer=buffer;options.readBufferSize=sizeof(buffer);
 auto result=WeReadHttpClient::request("https://example.com/fixture",options,[&](const uint8_t* p,size_t n){output.append(reinterpret_cast<const char*>(p),n);if(cancel)cancelled=true;return true;},{},status);
 assert(status==200 && cleanups==1);return result;
}
int main() {
 std::string out;
 fixture("cached short chapter",true);assert(run(out)==WeReadHttpClient::Result::Ok);assert(out==body && reads>=2);
 out.clear();fixture("streamed long fixture across read calls",false);assert(run(out)==WeReadHttpClient::Result::Ok && out==body);
 out.clear();fixture("cached body after timeout",true);again=true;assert(run(out)==WeReadHttpClient::Result::Ok && out==body);
 out.clear();fixture("cancelled chapter stream",false);assert(run(out,true)==WeReadHttpClient::Result::Aborted && out.size()==7);
 out.clear();fixture("partial body",false);truncated=true;assert(run(out)==WeReadHttpClient::Result::NetworkError);
 out.clear();fixture("",true);assert(run(out)==WeReadHttpClient::Result::Ok && out.empty());
 {
  WeReadHttpClient::Session session; uint8_t buffer[16]; int status;
  WeReadHttpClient::Header h={"Cookie","private-test"};
  WeReadHttpClient::RequestOptions o;o.readBuffer=buffer;o.readBufferSize=sizeof(buffer);o.headers=&h;o.headerCount=1;
  fixture("one",false);
  assert(WeReadHttpClient::request(session,"https://example.com/one",o,{},{},status)==WeReadHttpClient::Result::Ok);
  assert(session.reusable() && session.newConnections()==1 && !userData);
  fixture("two",false);
  assert(WeReadHttpClient::request(session,"https://example.com/two",o,{},{},status)==WeReadHttpClient::Result::Ok);
  assert(session.reusedRequests()==1 && session.newConnections()==1 && headerDeletes>=2 && !userData);
  fixture("three",false);
  assert(WeReadHttpClient::request(session,"https://cdn.example.com/three",o,{},{},status)==WeReadHttpClient::Result::Ok);
  assert(session.newConnections()==2);
  failOpen=true;
  assert(WeReadHttpClient::request(session,"https://cdn.example.com/fail",o,{},{},status)==WeReadHttpClient::Result::NetworkError);
  assert(!session.reusable());failOpen=false;
 }
 puts("PASS: native HTTP drains header-prefetched bodies, streamed data, EAGAIN, cancellation and truncation");
}
