/*
 * 首次无卡启动后插卡应可挂载；拔出已挂载卡仍须显式重挂。
 * A first insertion after empty boot mounts; removal of a mounted card still requires explicit remount.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_vfs_fat.h"
#include "freertos/task.h"

static bool test_present;
static int mount_calls, unmount_calls;
static sdmmc_card_t test_card = {
    .cid.name = "TEST64G", .csd.capacity = 1024, .csd.sector_size = 512,
};

#define mkdir(path, mode) (0)
#include "../components/read_pico/read_pico_sd.c"
#undef mkdir

bool read_pico_sd_present(void) { return test_present; }
const char* esp_err_to_name(esp_err_t err) { (void)err; return "test"; }
BaseType_t xTaskCreate(void (*fn)(void*), const char* name, int stack,
                       void* arg, int priority, void* handle) {
    (void)name; (void)stack; (void)priority; (void)handle;
    fn(arg);
    return pdPASS;
}
void vTaskDelay(int ticks) { (void)ticks; }
void vTaskDelete(void* handle) { (void)handle; }
esp_err_t esp_vfs_fat_sdmmc_mount(const char* path, const sdmmc_host_t* host,
                                  const sdmmc_slot_config_t* slot,
                                  const esp_vfs_fat_sdmmc_mount_config_t* config,
                                  sdmmc_card_t** out) {
    (void)path; (void)host; (void)slot; (void)config;
    ++mount_calls;
    assert(test_present);
    *out = &test_card;
    return ESP_OK;
}
esp_err_t esp_vfs_fat_sdcard_unmount(const char* path, sdmmc_card_t* mounted) {
    (void)path;
    assert(mounted == &test_card);
    ++unmount_calls;
    return ESP_OK;
}
esp_err_t esp_vfs_fat_sdcard_format(const char* path, sdmmc_card_t* mounted) {
    (void)path; (void)mounted;
    return ESP_OK;
}
esp_err_t esp_vfs_fat_info(const char* path, uint64_t* total, uint64_t* free_bytes) {
    (void)path;
    *total = 1024 * 512;
    *free_bytes = 768 * 512;
    return ESP_OK;
}

int main(void) {
    read_pico_sd_info_t info = {0};
    assert(read_pico_sd_start_probe() == ESP_ERR_NOT_FOUND);
    assert(read_pico_sd_get_info(&info) == ESP_ERR_NOT_FOUND);
    assert(!info.present && !info.mounted && mount_calls == 0);

    test_present = true;
    assert(read_pico_sd_get_info(&info) == ESP_ERR_INVALID_STATE);
    assert(info.present && !info.mounted);
    assert(read_pico_sd_start_probe() == ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info) == ESP_OK);
    assert(info.present && info.mounted && mount_calls == 1);

    test_present = false;
    assert(read_pico_sd_get_info(&info) == ESP_ERR_NOT_FOUND);
    test_present = true;
    assert(read_pico_sd_start_probe() == ESP_ERR_INVALID_STATE);
    assert(read_pico_sd_remount() == ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info) == ESP_OK);
    assert(info.mounted && mount_calls == 2 && unmount_calls == 1);
    puts("sd_probe: empty boot, insertion, removal and explicit remount passed");
    return 0;
}
