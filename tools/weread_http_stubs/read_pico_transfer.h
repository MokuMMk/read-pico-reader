#pragma once
#include <stdbool.h>
typedef struct { int mode; bool network_ready; } read_pico_transfer_status_t;
#define READ_PICO_TRANSFER_MODE_STA 1
extern "C" void read_pico_transfer_get_status(read_pico_transfer_status_t*);
