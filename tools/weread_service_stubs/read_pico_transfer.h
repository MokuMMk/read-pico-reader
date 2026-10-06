// SPDX-License-Identifier: Apache-2.0
// 已联网的 STA 替身。/ Connected STA fake.
#pragma once
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define READ_PICO_TRANSFER_MODE_STA 1
#define READ_PICO_TRANSFER_ERROR 2
typedef int esp_err_t;
typedef struct{int mode;bool network_only;}read_pico_transfer_cfg_t;
typedef struct{int mode,state;bool network_ready;}read_pico_transfer_status_t;
inline unsigned fake_network_stops;
inline esp_err_t read_pico_transfer_get_saved_wifi(char*,bool* configured){*configured=true;return 0;}
inline void read_pico_transfer_stop(){++fake_network_stops;}
inline esp_err_t read_pico_transfer_start(read_pico_transfer_cfg_t*){return 0;}
inline void read_pico_transfer_get_status(read_pico_transfer_status_t* s){s->mode=1;s->network_ready=true;}
inline void read_pico_transfer_service_poll(){}
inline esp_err_t read_pico_transfer_sync_time_online(uint32_t*){return 0;}
inline const char* esp_err_to_name(int){return "test";}
