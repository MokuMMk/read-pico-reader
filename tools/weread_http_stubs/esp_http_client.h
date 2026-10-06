#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_HTTP_EAGAIN 0x7007
typedef struct FakeClient* esp_http_client_handle_t;
enum { HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_EVENT_ON_HEADER };
typedef struct { int event_id; void* user_data; char* header_key; char* header_value; } esp_http_client_event_t;
typedef struct { const char* url; int method; int timeout_ms; int buffer_size; int buffer_size_tx; esp_err_t (*crt_bundle_attach)(void*); bool disable_auto_redirect; bool keep_alive_enable; esp_err_t (*event_handler)(esp_http_client_event_t*); void* user_data; } esp_http_client_config_t;
extern "C" {
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char*,const char*);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t,const char*);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t,int);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t,void*);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t,const char*);
esp_err_t esp_http_client_open(esp_http_client_handle_t,int);
int esp_http_client_write(esp_http_client_handle_t,const char*,int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t,int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char*,int);
}
