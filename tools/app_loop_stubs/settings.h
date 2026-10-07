/* 中文：调度测试替身。/ English: Scheduler test shim. */
#pragma once
#include "common.h"

static inline bool app_settings_ble_turner(void) { return false; }

uint8_t app_settings_auto_lock_minutes(void);
