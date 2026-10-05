/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 只做开机装配：拉起板级硬件、读设置、定 VCOM、放开机图、开字体；
 * 未标定则先拦住进设定页。然后把控制权交给 app_loop。
 *
 * Boot wiring only: board init, settings, PMU RTC restore, VCOM, splash, fonts. If VCOM
 * is unset, the factory page blocks first. Then control goes to app_loop.
 */

#include <stdint.h>
#include <sys/time.h>

#include "app_loop.h"
#include "app_registry.h"
#include "display.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "esp_log.h"
#include "ota_update.h"
#include "pmu_selftest.h"
#include "read_pico_board.h"
#include "read_pico_init.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "settings.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "ui_kit.h"
#include "vcom_setup.h"

static const char* TAG = "read_pico";

// 主机上电会丢失系统时间；PMU 在主机断电后继续走时。先取实时值再恢复，
// 不使用启动时缓存的快照，以免将已经经过的开机耗时丢掉。
static void restore_time_from_pmu(void) {
    if (read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) != ESP_OK) return;
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (!pmu || !pmu->time_synced || pmu->unix_sec < 1704067200 || pmu->unix_sec > 4102444799U) return;
    struct timeval tv = {.tv_sec = (time_t)pmu->unix_sec, .tv_usec = 0};
    if (settimeofday(&tv, NULL) == 0) ESP_LOGI(TAG, "system clock restored from PMU");
}

extern const uint8_t lock_4bpp_bin_start[] asm("_binary_lock_4bpp_bin_start");
extern const uint8_t lock_4bpp_bin_end[] asm("_binary_lock_4bpp_bin_end");
extern const uint8_t loading_4bpp_bin_start[] asm("_binary_loading_4bpp_bin_start");
extern const uint8_t loading_4bpp_bin_end[] asm("_binary_loading_4bpp_bin_end");

// 已标定则开机读一次喂给 epdiy。未标定先用板级默认出开机图，随后拦住进标定页。
// Load VCOM once when set. Otherwise keep the board default for the splash,
// then gate into the setup page.
static bool resolve_vcom_at_boot(void) {
    int mv = 0;
    for (int i = 0; i < 3; i++) {
        if (read_pico_pmu_vcom_get(&mv) == ESP_OK) {
            epd_set_vcom((uint16_t)mv);
            ESP_LOGI(TAG, "panel VCOM loaded from PMU");
            return true;
        }
    }
    ESP_LOGW(TAG, "panel VCOM unset, keeping board default until setup");
    return false;
}

// 开机图和锁屏图是预先转好的 4bpp 全屏位图，尺寸对不上说明资源和面板不匹配。
// Splash and lock images are pre-baked 4bpp full-frames. A size mismatch
// means the assets do not match the panel.
static bool images_match_panel(void) {
    size_t lock_size = (size_t)(lock_4bpp_bin_end - lock_4bpp_bin_start);
    size_t loading_size = (size_t)(loading_4bpp_bin_end - loading_4bpp_bin_start);
    size_t expected = (size_t)epd_width() * epd_height() / 2;
    if (lock_size == expected && loading_size == expected) return true;
    ESP_LOGE(
        TAG, "Image size mismatch: lock %u loading %u expected %u",
        (unsigned)lock_size, (unsigned)loading_size, (unsigned)expected
    );
    return false;
}

void app_main(void) {
    read_pico_handle_t hw;
    if (read_pico_init(&hw) != ESP_OK) return;
    if (hw.pmu_ready) restore_time_from_pmu();
    app_settings_init();
    const bool vcom_ok = resolve_vcom_at_boot();
    pmu_selftest_bind(hw.sensor);

    if (!images_match_panel()) return;

    EpdiyHighlevelState hl = hw.hl;
    uint8_t* framebuffer = hw.framebuffer;

    epd_poweron();
    read_pico_i2c_census_take();
    epd_clear();
    epd_hl_set_all_white(&hl);
    ui_draw_full_image(framebuffer, loading_4bpp_bin_start);
    update_display_from_white(&hl);

    app_font_activate_system();

    if (!hw.touch_ready) {
        ESP_LOGE(TAG, "No touch controller, UI cannot run");
        return;
    }

    if (hw.pmu_ready && !vcom_ok) {
        vcom_setup_run(&hl, framebuffer, hw.touch);
    }

    // 硬件、显示、触摸与必要的出厂设置均成功后确认新槽。
    // Confirm a new slot only after hardware, display, touch, and required setup succeed.
    pico_ota_confirm_running();

    app_loop_run(&(app_loop_config_t){
        .hl = &hl,
        .fb = framebuffer,
        .acc = hw.sensor,
        .tp = hw.touch,
        .sensor_ready = hw.sensor_ready,
        // 正式固件始终进入产品首页；工厂自检不再进入用户导航。
        // Production firmware always enters the product home; factory diagnostics stay outside user navigation.
        .first_app = app_home_page(),
    });
}
