/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：HTTPS 分块下载、完整性校验和可取消升级任务。/ English: streamed HTTPS updates, integrity checks and cancellable workers.
 * 冻结：禁止覆盖当前槽；离页或锁屏先等待任务结束。/ Frozen: never overwrite the running slot; join before leaving or locking.
 */
#include "ota_online.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "read_pico_transfer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static SemaphoreHandle_t s_mutex, s_done;
static pico_update_status_t *s_status;
static atomic_bool s_cancel;
static bool s_joinable, s_download;
static const esp_partition_t *s_verified;

static bool initialize(void) {
    if (!s_status) s_status = heap_caps_calloc(1, sizeof(*s_status), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    return s_status && s_mutex && s_done;
}
static void state(pico_update_state_t value, const char *message) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status->state = value;
    snprintf(s_status->message, sizeof(s_status->message), "%s", message);
    ++s_status->revision;
    xSemaphoreGive(s_mutex);
}
bool pico_online_busy(void) {
    if (!s_status || !s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = s_status->busy;
    xSemaphoreGive(s_mutex); return busy;
}
void pico_online_get_status(pico_update_status_t *status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!s_status || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(status, s_status, sizeof(*status));
    xSemaphoreGive(s_mutex);
}
static bool layout_valid(void) {
    const esp_partition_t *a = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *b = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    const esp_partition_t *data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    return a && b && data && a->address == 0x10000 && b->address == 0x910000 &&
        a->size == 0x400000 && b->size == 0x400000 && data->address == 0xd10000 && data->size == 0x2000;
}
static bool online(bool *owned) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (!(status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready)) {
        char ssid[33]; bool saved = false;
        if (read_pico_transfer_get_saved_wifi(ssid, &saved) != ESP_OK || !saved) {
            state(PICO_UPDATE_FAILED, "请先在 WiFi 设置中连接网络"); return false;
        }
        read_pico_transfer_stop();
        read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
        *owned = true;
        if (read_pico_transfer_start(&cfg) != ESP_OK) {
            state(PICO_UPDATE_FAILED, "WiFi 连接失败，请重试"); return false;
        }
    }
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (!atomic_load(&s_cancel)) {
        read_pico_transfer_service_poll();
        read_pico_transfer_get_status(&status);
        if (status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready) {
            if (time(NULL) < 1704067200) {
                uint32_t utc;
                if (read_pico_transfer_sync_time_online(&utc) != ESP_OK) {
                    state(PICO_UPDATE_FAILED, "网络对时失败，请对时后重试"); return false;
                }
            }
            return !atomic_load(&s_cancel);
        }
        if (status.state == READ_PICO_TRANSFER_ERROR || esp_timer_get_time() >= deadline) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!atomic_load(&s_cancel)) state(PICO_UPDATE_FAILED, "网络连接超时，请重试");
    return false;
}
static esp_http_client_handle_t open_http(const char *url, int *status, int64_t *length) {
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 8000, .buffer_size = 2048,
        .buffer_size_tx = 1024, .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true, .keep_alive_enable = true};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    esp_http_client_set_header(client, "User-Agent", PICO_OTA_CAPABILITY);
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    if (esp_http_client_open(client, 0) != ESP_OK || atomic_load(&s_cancel)) goto fail;
    *length = esp_http_client_fetch_headers(client);
    if (*length < 0 || atomic_load(&s_cancel)) goto fail;
    *status = esp_http_client_get_status_code(client);
    esp_http_client_set_timeout_ms(client, 1000);
    return client;
fail:
    esp_http_client_cleanup(client);
    return NULL;
}
// 读超时可重试；每次收到数据续期，避免慢速网络被总时长截断。
// Retry temporary read timeouts and renew the idle deadline after each chunk.
static int read_chunk(esp_http_client_handle_t client, uint8_t *buffer, int cap, int64_t *idle) {
    while (!atomic_load(&s_cancel) && esp_timer_get_time() < *idle) {
        read_pico_transfer_service_poll();
        int n = esp_http_client_read(client, (char *)buffer, cap);
        if (n > 0) { *idle = esp_timer_get_time() + 30000000; return n; }
        if (!n && esp_http_client_is_complete_data_received(client)) return 0;
        if (n < 0 && n != -ESP_ERR_HTTP_EAGAIN) return -1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return -1;
}
static bool check_feed(void) {
    char *json = heap_caps_malloc(8193, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pico_release_t *release = heap_caps_calloc(1, sizeof(*release), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json || !release) { state(PICO_UPDATE_FAILED, "内存不足，请退出传书后重试"); goto failed; }
    char url[192]; snprintf(url, sizeof(url), "%s?t=%lld", PICO_OTA_FEED_URL, (long long)time(NULL));
    int code = -1; int64_t length = -1;
    esp_http_client_handle_t client = NULL;
    for (int i = 0; i < 3 && !atomic_load(&s_cancel); ++i) {
        client = open_http(url, &code, &length);
        if (client) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!client) { state(PICO_UPDATE_FAILED, "更新服务器连接失败，请稍后重试"); goto failed; }
    if (code != 200 || length > 8192) {
        esp_http_client_cleanup(client);
        state(PICO_UPDATE_FAILED, code == 404 ? "联网更新暂未发布，请使用 TF 卡升级" : "更新清单读取失败，请重试");
        goto failed;
    }
    size_t received = 0;
    int64_t idle = esp_timer_get_time() + 30000000;
    int n = -1;
    while (received < 8192 && (n = read_chunk(client, (uint8_t *)json + received, (int)(8192 - received), &idle)) > 0)
        received += n;
    bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    json[received] = 0;
    if (n < 0 || !complete || pico_release_parse(json, received, release) != ESP_OK) {
        state(PICO_UPDATE_FAILED, "更新清单无效或下载中断，请重试"); goto failed;
    }
    const esp_app_desc_t *running = esp_app_get_description();
    if (strcmp(release->project, running->project_name) || !layout_valid() ||
        pico_version_compare(running->version, release->minimum_base_version) < 0) {
        state(PICO_UPDATE_FAILED, "基础包不兼容，请先通过官网完整刷机"); goto failed;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_status->release, release, sizeof(*release));
    xSemaphoreGive(s_mutex);
    state(pico_version_compare(release->version, running->version) > 0 ? PICO_UPDATE_AVAILABLE : PICO_UPDATE_LATEST,
          pico_version_compare(release->version, running->version) > 0 ? "发现新版本，可下载安装" : "当前已是最新版本");
    heap_caps_free(json); heap_caps_free(release);
    return true;
failed:
    heap_caps_free(json); heap_caps_free(release);
    return false;
}
static bool header_valid(const uint8_t *data, const pico_release_t *release) {
    const esp_image_header_t *header = (const esp_image_header_t *)data;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(data + sizeof(*header) + sizeof(esp_image_segment_header_t));
    const esp_app_desc_t *current = esp_app_get_description();
    return header->magic == ESP_IMAGE_HEADER_MAGIC && header->chip_id == ESP_CHIP_ID_ESP32S3 &&
        desc->magic_word == ESP_APP_DESC_MAGIC_WORD &&
        memchr(desc->project_name, 0, sizeof(desc->project_name)) && memchr(desc->version, 0, sizeof(desc->version)) &&
        !strcmp(desc->project_name, current->project_name) && !strcmp(desc->version, release->version);
}
static bool download(void) {
    pico_release_t *release = heap_caps_malloc(sizeof(*release), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *buffer = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *header = heap_caps_malloc(288, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!release || !buffer || !header) { state(PICO_UPDATE_FAILED, "内存不足，无法下载升级包"); goto failed; }
    xSemaphoreTake(s_mutex, portMAX_DELAY); *release = s_status->release; xSemaphoreGive(s_mutex);
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!layout_valid() || !target || target == esp_ota_get_running_partition() || release->size > target->size) {
        state(PICO_UPDATE_FAILED, "基础包不兼容，请通过官网完整刷机"); goto failed;
    }
    int code = -1; int64_t length = -1;
    esp_http_client_handle_t client = open_http(release->url, &code, &length);
    if (!client) { state(PICO_UPDATE_FAILED, "下载连接失败，请重试"); goto failed; }
    if (code != 200 || (length > 0 && length != release->size)) {
        esp_http_client_cleanup(client); state(PICO_UPDATE_FAILED, "升级包不存在或大小不符，请重新检查更新"); goto failed;
    }
    esp_ota_handle_t handle = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool begun = false;
    esp_err_t error = ESP_OK;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) error = ESP_FAIL;
    size_t total = 0, header_size = 0;
    int64_t idle = esp_timer_get_time() + 30000000;
    int n = -1;
    while (error == ESP_OK && (n = read_chunk(client, buffer, 4096, &idle)) > 0) {
        if (total + n > release->size) { error = ESP_ERR_INVALID_SIZE; break; }
        size_t used = 0;
        if (!begun) {
            size_t needed = 288 - header_size;
            used = (size_t)n < needed ? (size_t)n : needed;
            memcpy(header + header_size, buffer, used); header_size += used;
            if (header_size == 288) {
                if (!header_valid(header, release)) { error = ESP_ERR_INVALID_RESPONSE; break; }
                error = esp_ota_begin(target, release->size, &handle);
                begun = error == ESP_OK;
                if (begun) error = esp_ota_write(handle, header, header_size);
            }
        }
        if (error == ESP_OK && begun && (size_t)n > used) error = esp_ota_write(handle, buffer + used, n - used);
        if (error != ESP_OK || psa_hash_update(&hash, buffer, n) != PSA_SUCCESS) { error = ESP_FAIL; break; }
        total += n;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status->received = total; ++s_status->revision;
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    uint8_t digest[32]; size_t digest_size = 0; char hex[65];
    if (error == ESP_OK && (!begun || n < 0 || !complete || total != release->size || atomic_load(&s_cancel))) error = ESP_FAIL;
    if (error == ESP_OK && psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS) error = ESP_FAIL;
    psa_hash_abort(&hash);
    if (error == ESP_OK) {
        for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        if (digest_size != 32 || strcasecmp(hex, release->sha256)) error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) {
        if (begun) esp_ota_abort(handle);
        state(PICO_UPDATE_FAILED, "下载中断或校验失败，当前版本保持不变"); goto failed;
    }
    error = esp_ota_end(handle);
    if (error != ESP_OK) { state(PICO_UPDATE_FAILED, "固件校验失败，当前版本保持不变"); goto failed; }
    // 后台仅验证镜像；主线程处理最终启动切换，取消时不影响当前槽。
    // The worker verifies only; the main task commits boot, so cancellation cannot change the running slot.
    xSemaphoreTake(s_mutex, portMAX_DELAY); s_verified = target; xSemaphoreGive(s_mutex);
    state(PICO_UPDATE_READY, "校验通过，即将重启安装");
    heap_caps_free(release); heap_caps_free(buffer); heap_caps_free(header);
    return true;
failed:
    heap_caps_free(release); heap_caps_free(buffer); heap_caps_free(header);
    return false;
}
static void worker(void *unused) {
    (void)unused;
    bool owned = false;
    if (online(&owned) && !atomic_load(&s_cancel)) {
        if (s_download) (void)download(); else (void)check_feed();
    }
    if (owned) read_pico_transfer_stop();
    if (atomic_load(&s_cancel)) state(PICO_UPDATE_CANCELLED, "已取消，当前版本保持不变");
    xSemaphoreTake(s_mutex, portMAX_DELAY); s_status->busy = false; ++s_status->revision; xSemaphoreGive(s_mutex);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}
static esp_err_t start(bool downloading) {
    if (!initialize()) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = s_status->busy;
    bool available = s_status->state == PICO_UPDATE_AVAILABLE;
    xSemaphoreGive(s_mutex);
    if (busy || (downloading && !available)) return ESP_ERR_INVALID_STATE;
    if (s_joinable) { xSemaphoreTake(s_done, portMAX_DELAY); s_joinable = false; }
    atomic_store(&s_cancel, false);
    s_download = downloading; s_verified = NULL;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status->busy = true; s_status->received = 0;
    xSemaphoreGive(s_mutex);
    state(downloading ? PICO_UPDATE_DOWNLOADING : PICO_UPDATE_CHECKING,
          downloading ? "正在下载，请保持供电" : "正在连接并检查更新");
    if (xTaskCreate(worker, "pico_ota", 12288, NULL, 4, NULL) != pdPASS) {
        xSemaphoreTake(s_mutex, portMAX_DELAY); s_status->busy = false; xSemaphoreGive(s_mutex);
        state(PICO_UPDATE_FAILED, "内存不足，无法开始升级"); return ESP_ERR_NO_MEM;
    }
    s_joinable = true;
    return ESP_OK;
}
esp_err_t pico_online_check(void) { return start(false); }
esp_err_t pico_online_download(void) { return start(true); }
void pico_online_cancel_join(void) {
    if (s_joinable) {
        atomic_store(&s_cancel, true);
        xSemaphoreTake(s_done, portMAX_DELAY); s_joinable = false;
    }
    s_verified = NULL;
    if (s_status && s_status->state == PICO_UPDATE_READY) state(PICO_UPDATE_CANCELLED, "已取消，当前版本保持不变");
}
esp_err_t pico_online_commit(void) {
    if (s_joinable) { xSemaphoreTake(s_done, portMAX_DELAY); s_joinable = false; }
    if (!s_verified || atomic_load(&s_cancel)) return ESP_ERR_INVALID_STATE;
    esp_err_t error = esp_ota_set_boot_partition(s_verified);
    s_verified = NULL;
    if (error != ESP_OK) state(PICO_UPDATE_FAILED, "启动切换失败，当前版本保持不变");
    return error;
}
