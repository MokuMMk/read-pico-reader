/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：联网更新的版本清单与可取消后台任务。/ English: release metadata and cancellable online update jobs.
 * 冻结：只写空闲应用槽，完整校验后由 UI 确认启动。/ Frozen: only write the inactive app slot; UI commits boot after verification.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PICO_OTA_LAYOUT "pico-dual-4m-v1"
#define PICO_OTA_BOARD "RDP-G01-W"
#define PICO_OTA_CAPABILITY "PICO_HTTPS_OTA_V1"
#define PICO_OTA_FEED_URL "https://wegooo-cell.github.io/read-pico-reader/update.json"

typedef enum { PICO_UPDATE_IDLE, PICO_UPDATE_CHECKING, PICO_UPDATE_AVAILABLE,
               PICO_UPDATE_LATEST, PICO_UPDATE_DOWNLOADING, PICO_UPDATE_READY,
               PICO_UPDATE_FAILED, PICO_UPDATE_CANCELLED } pico_update_state_t;
typedef struct {
    char version[33], project[33], board[24], layout[32], minimum_base_version[33];
    char url[256], sha256[65], notes[384];
    uint32_t size;
} pico_release_t;
typedef struct {
    pico_update_state_t state;
    pico_release_t release;
    uint32_t revision, received;
    char message[96];
    bool busy;
} pico_update_status_t;

/// 有界解析、型号与版本验证；不接触网络。/ Bounded parsing, board and version validation without networking.
esp_err_t pico_release_parse(const char *json, size_t length, pico_release_t *release);
int pico_version_compare(const char *a, const char *b);
bool pico_release_url_valid(const char *url);
/// 检查或下载唯一后台任务；离页、锁屏先取消并等待。/ Start one worker; cancel and join on exit or lock.
esp_err_t pico_online_check(void);
esp_err_t pico_online_download(void);
bool pico_online_busy(void);
void pico_online_get_status(pico_update_status_t *status);
void pico_online_cancel_join(void);
/// 已验证镜像切换启动槽；失败不切换。/ Select a verified image; failures keep the existing boot slot.
esp_err_t pico_online_commit(void);
