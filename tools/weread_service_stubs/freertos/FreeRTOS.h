// SPDX-License-Identifier: Apache-2.0
// 单线程调度替身，任务显式运行。/ Deterministic scheduler; tasks run explicitly.
#pragma once
#define portMAX_DELAY 0xffffffff
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
typedef unsigned TickType_t;
