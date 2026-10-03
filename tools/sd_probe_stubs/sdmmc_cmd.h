/* SPDX-License-Identifier: Apache-2.0
 * SD 探测主机测试卡片结构。/ Host SD-probe card structure stub.
 */
#pragma once

#include <stdint.h>

typedef struct {
    struct { char name[8]; } cid;
    struct { uint32_t capacity, sector_size; } csd;
} sdmmc_card_t;
