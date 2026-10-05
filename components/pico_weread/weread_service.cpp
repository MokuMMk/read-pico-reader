/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 串行微信读书任务，后台连接、扫码、同步与下载，UI 只读取快照。
 * Serialized background connection, login, shelf and download; UI reads snapshots only.
 * 冻结：退出与介质失效先取消并等待；只读云端进度。
 * Frozen: cancel and join before exit/media loss; read-only cloud progress.
 */
#include "weread_service.h"
#include "WeReadClient.h"
#include "HalStorage.h"
#include "TimeUtils.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
extern "C" {
#include "read_pico_transfer.h"
}
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <new>
#include <sys/stat.h>

// C 页面使用这组协议错误编号，防止上游枚举漂移。/ Guard protocol error numbers used by the C page.
static_assert(static_cast<int>(WeReadClient::Error::SdCard) == 6 &&
              static_cast<int>(WeReadClient::Error::Integrity) == 7 &&
              static_cast<int>(WeReadClient::Error::Unavailable) == 8 &&
              static_cast<int>(WeReadClient::Error::Clock) == 9 &&
              static_cast<int>(WeReadClient::Error::OutOfMemory) == 10 &&
              static_cast<int>(WeReadClient::Error::WholeBookOnly) == 11);

static SemaphoreHandle_t s_mutex, s_finished;
// 快照放扩展内存，内部内存留给 WiFi 与任务栈。/ Keep snapshots in PSRAM for WiFi/task-stack headroom.
static weread_snapshot_t* s_status_storage;
#define s_status (*s_status_storage)
static std::atomic<bool> s_cancel{false};
static bool s_configured;
static bool s_include_images = true;
static weread_action_t s_action;
static unsigned s_page, s_index;
bool pico_weread_cancelled() { return s_cancel.load(); }

static void log_memory(const char* stage) {
    ESP_LOGI("weread", "%s internal=%u largest=%u psram=%u stack_free=%u", stage,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}
static bool initialize() {
    if (!s_status_storage) s_status_storage = static_cast<weread_snapshot_t*>(
        heap_caps_calloc(1, sizeof(weread_snapshot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_finished) s_finished = xSemaphoreCreateBinary();
    return s_status_storage && s_mutex && s_finished;
}
static void lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(s_mutex); }
static void set_state(weread_state_t state, int error = 0) {
    lock();
    if (s_status.state != state || s_status.error != error) {
        s_status.state = state; s_status.error = error; ++s_status.revision;
    }
    if (state != WEREAD_QR) s_status.qr[0] = 0;
    if (state == WEREAD_FAILED) ESP_LOGE("weread", "failed error=%d", error);
    unlock();
}
static bool load_page(unsigned page) {
    auto* visible = static_cast<weread_book_t*>(heap_caps_calloc(
        WEREAD_ROWS, sizeof(weread_book_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!visible) { set_state(WEREAD_FAILED, 10); return false; }
    HalFile file;
    uint32_t total = 0;
    if (!WeReadStore::openShelf(file, total)) total = 0;
    const unsigned pages = total ? (total + WEREAD_ROWS - 1) / WEREAD_ROWS : 1;
    if (page >= pages) page = pages - 1;
    unsigned count = 0;
    for (unsigned i = 0; i < WEREAD_ROWS && page * WEREAD_ROWS + i < total; ++i) {
        WeReadStore::ShelfRecord record;
        if (!WeReadStore::readShelfRecord(file, page * WEREAD_ROWS + i, record)) break;
        if (!memchr(record.bookId, 0, sizeof(record.bookId)) || !memchr(record.title, 0, sizeof(record.title)) ||
            !memchr(record.author, 0, sizeof(record.author))) break;
        memcpy(visible[i].id, record.bookId, sizeof(record.bookId));
        memcpy(visible[i].title, record.title, sizeof(record.title));
        memcpy(visible[i].author, record.author, sizeof(record.author));
        const auto path = WeReadStore::finalBookPath(record);
        if (Storage.exists(path)) snprintf(visible[i].local_path, sizeof(visible[i].local_path), "%s", Storage.map(path).c_str());
        ++count;
    }
    WeReadStore::Session session;
    const bool logged_in = WeReadStore::loadSession(session);
    session.clear();
    lock();
    s_status.logged_in = logged_in;
    s_status.total = total; s_status.page = page; s_status.count = count;
    memcpy(s_status.books, visible, sizeof(s_status.books));
    ++s_status.revision;
    unlock();
    heap_caps_free(visible);
    return true;
}
static bool connect_online(bool& owned_network) {
    char ssid[33]; bool configured = false;
    if (read_pico_transfer_get_saved_wifi(ssid, &configured) != ESP_OK || !configured) {
        set_state(WEREAD_FAILED, 100); return false;
    }
    read_pico_transfer_cfg_t cfg = {};
    cfg.mode = READ_PICO_TRANSFER_MODE_STA; cfg.network_only = true;
    // 复用已有 STA 并保持连接；只清理本任务创建的网络。
    // Reuse an existing STA without disconnecting it on completion. Only tear down a session this worker starts.
    read_pico_transfer_status_t existing;
    read_pico_transfer_get_status(&existing);
    if (!(existing.mode == READ_PICO_TRANSFER_MODE_STA && existing.network_ready)) {
        read_pico_transfer_stop();
        owned_network = true;
        const esp_err_t error = read_pico_transfer_start(&cfg);
        if (error != ESP_OK) {
            ESP_LOGE("weread", "network start failed: %s", esp_err_to_name(error));
            set_state(WEREAD_FAILED, error == ESP_ERR_NO_MEM ? 10 : 101); return false;
        }
    }
    log_memory("network started");
    const TickType_t began = xTaskGetTickCount();
    while (!s_cancel.load()) {
        read_pico_transfer_service_poll();
        read_pico_transfer_status_t status;
        read_pico_transfer_get_status(&status);
        if (status.network_ready) {
            uint32_t epoch;
            if (!TimeUtils::isClockValid() && read_pico_transfer_sync_time_online(&epoch) != ESP_OK) {
                set_state(WEREAD_FAILED, 102); return false;
            }
            return true;
        }
        if (status.state == READ_PICO_TRANSFER_ERROR || xTaskGetTickCount() - began > pdMS_TO_TICKS(20000)) {
            set_state(WEREAD_FAILED, 101); return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}
static void progress_callback(void* raw) {
    auto* op = static_cast<WeReadClient::Operation*>(raw);
    if (s_cancel.load()) op->cancel();
    // 长打包循环定期让 UI 处理输入。/ Let the UI process input during long packaging loops.
    vTaskDelay(pdMS_TO_TICKS(1));
}
static void worker(void*) {
    log_memory("worker started");
    bool online = false, owned_network = false;
    WeReadClient::Operation* op = nullptr;
    if (s_action == WEREAD_LOAD) {
        if (load_page(s_page)) set_state(WEREAD_IDLE);
    } else if (s_action == WEREAD_LOGOUT) {
        const bool session = WeReadStore::clearSession();
        const bool shelf = WeReadStore::clearShelf() && WeReadBrowse::clearAllCaches();
        if (load_page(0)) set_state(session && shelf ? WEREAD_COMPLETE : WEREAD_FAILED, session && shelf ? 0 : 103);
    } else {
        WeReadStore::ShelfRecord selected;
        bool selection_ok = true;
        if (s_action == WEREAD_DOWNLOAD) {
            HalFile shelf; uint32_t total;
            selection_ok = WeReadStore::openShelf(shelf, total) && s_index < total &&
                WeReadStore::readShelfRecord(shelf, s_index, selected);
        }
        if (!selection_ok) set_state(WEREAD_FAILED, 103);
        else {
            set_state(WEREAD_CONNECTING);
            online = connect_online(owned_network);
            if (online && !s_cancel.load()) {
                log_memory("network ready");
                // 操作对象约 8 KiB 放 PSRAM，保留内部内存用于任务栈和 TLS。
                // Place the roughly 8 KiB operation in PSRAM, reserving internal RAM for task stack and TLS.
                void* memory = heap_caps_malloc(sizeof(WeReadClient::Operation), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (memory) op = new (memory) WeReadClient::Operation();
                if (!op) { ESP_LOGE("weread", "operation allocation failed bytes=%u", (unsigned)sizeof(WeReadClient::Operation));
                    set_state(WEREAD_FAILED, static_cast<int>(WeReadClient::Error::OutOfMemory)); }
                else {
                    WeReadClient::DownloadOptions options;
                    options.imagePolicy = s_include_images ? WeReadStore::ImagePolicy::Embed : WeReadStore::ImagePolicy::Exclude;
                    const auto kind = s_action == WEREAD_SYNC ? WeReadClient::Operation::Kind::Sync : WeReadClient::Operation::Kind::Download;
                    if (!op->begin(kind, s_action == WEREAD_SYNC ? nullptr : &selected, options))
                        set_state(WEREAD_FAILED, static_cast<int>(op->error()));
                    else {
                        set_state(WEREAD_WORKING);
                        while (op->active()) {
                            if (s_cancel.load()) op->cancel();
                            auto event = op->step(progress_callback, op);
                            if (event == WeReadClient::Operation::Event::QrReady) {
                                lock();
                                snprintf(s_status.qr, sizeof(s_status.qr), "%.319s", op->qrUrl());
                                s_status.state = WEREAD_QR; ++s_status.revision;
                                unlock();
                            } else if (event == WeReadClient::Operation::Event::Authenticated) set_state(WEREAD_WORKING);
                            lock();
                            if (s_status.done != op->progressCompleted() || s_status.target != op->progressTotal() ||
                                (int)s_status.stage != (int)op->progressStage() || s_status.skipped_images != op->skippedImageCount()) {
                                s_status.done = op->progressCompleted(); s_status.target = op->progressTotal();
                                s_status.stage = static_cast<weread_stage_t>(op->progressStage());
                                s_status.skipped_images = op->skippedImageCount(); ++s_status.revision;
                            }
                            unlock();
                            if (event == WeReadClient::Operation::Event::Complete) {
                                if (s_action == WEREAD_DOWNLOAD) {
                                    lock();
                                    snprintf(s_status.output, sizeof(s_status.output), "%s", Storage.map(op->finalPath()).c_str());
                                    ++s_status.changed;
                                    unlock();
                                }
                                if (load_page(s_page)) set_state(WEREAD_COMPLETE);
                                break;
                            }
                            if (event == WeReadClient::Operation::Event::Failed) { set_state(WEREAD_FAILED, static_cast<int>(op->error())); break; }
                            if (event == WeReadClient::Operation::Event::Cancelled) { set_state(WEREAD_CANCELLED); break; }
                            vTaskDelay(pdMS_TO_TICKS(30));
                        }
                    }
                }
            }
        }
    }
    log_memory("worker finished");
    if (op) { op->reset(); op->~Operation(); heap_caps_free(op); }
    // 仅清理自己创建的网络，保留配网建立的连接。
    // Cleanup owned network only; provisioning-owned STA remains available.
    if (owned_network) read_pico_transfer_stop();
    if (s_cancel.load()) set_state(WEREAD_CANCELLED);
    lock();
    s_status.active = false; ++s_status.revision;
    xSemaphoreGive(s_finished);
    unlock();
    vTaskDelete(nullptr);
}
extern "C" bool weread_configure(const char* cache, const char* books) {
    if (!initialize() || !cache || !books || strncmp(cache, "/sdcard/", 8) || strncmp(books, "/sdcard/", 8) ||
        strlen(cache) >= 160 || strlen(books) >= 160) return false;
    lock();
    if (s_status.active) { unlock(); return false; }
    Storage.configure(cache, books); s_configured = true;
    unlock();
    return true;
}
extern "C" bool weread_start(weread_action_t action, unsigned page, unsigned index) {
    if (!initialize() || !s_configured || action < WEREAD_LOAD || action > WEREAD_LOGOUT) return false;
    lock();
    if (s_status.active) { unlock(); return false; }
    while (xSemaphoreTake(s_finished, 0) == pdTRUE) {}
    s_cancel.store(false);
    s_action = action; s_page = page; s_index = index;
    s_status.action = action;
    s_status.active = true; s_status.qr[0] = 0; s_status.output[0] = 0;
    s_status.done = s_status.target = s_status.skipped_images = 0; s_status.error = 0;
    s_status.stage = WEREAD_CHAPTERS;
    s_status.state = WEREAD_WORKING; ++s_status.revision;
    unlock();
    log_memory("dispatch");
    if (xTaskCreate(worker, "weread", 16384, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE("weread", "worker stack allocation failed bytes=16384");
        lock(); s_status.active = false; s_status.state = WEREAD_FAILED;
        s_status.error = static_cast<int>(WeReadClient::Error::OutOfMemory); ++s_status.revision; unlock();
        return false;
    }
    return true;
}
extern "C" void weread_snapshot(weread_snapshot_t* out) {
    if (!out) return;
    if (!initialize()) { memset(out, 0, sizeof(*out)); out->state = WEREAD_FAILED; out->error = 10; return; }
    lock(); *out = s_status; unlock();
}
extern "C" void weread_stop() {
    if (!initialize()) return;
    s_cancel.store(true);
    lock(); const bool active = s_status.active; unlock();
    if (active) xSemaphoreTake(s_finished, portMAX_DELAY);
}

extern "C" bool weread_set_include_images(bool enabled) {
    if (!initialize()) return false;
    lock();
    if (s_status.active) { unlock(); return false; }
    s_include_images = enabled;
    unlock();
    return true;
}
