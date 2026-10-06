/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
static inline void read_pico_transfer_service_poll(void) {}

#include "common.h"
#define READ_PICO_TRANSFER_STOPPED 0
typedef struct {int state;} read_pico_transfer_status_t;
static inline void read_pico_transfer_get_status(read_pico_transfer_status_t *s) { s->state=0; }
