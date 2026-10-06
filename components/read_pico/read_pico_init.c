/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 板级开机装配：墨水屏、SD 探测、加速度计、PMU、触摸。
 *
 * Board bring-up: EPD, SD probe, accelerometer, PMU, touch.
 */

#include "read_pico_init.h"

#if defined(PICO_BOARD_METALIO_EINK4_PLUS)
// 厂商 epdiy：显示定义、波形与应用侧初始化都用例程那一套。
// Vendor epdiy: display definition, waveform and app-side init all come from the demo.
#include "epdiy_extras.h"
#include "my_waveform.h"
#endif

#include "epdiy.h"
#include "esp_log.h"
#include "read_pico_board.h"
#include "read_pico_epd_timing.h"
#include "e0470_epaper_waveform.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"

static const char* TAG = "read_pico_init";

#define READ_PICO_PCLK_MHZ 18
#define READ_PICO_VCOM_MV 1290
// 加速度计 INT1。Read Pico 接 GPIO1；Metalio 上 GPIO1 是 I2C 的 SCL，而且加速度计的
// 中断是经 TCA9555 线与到 GPIO2 的，所以本板不占用任何引脚。
// Accelerometer INT1. Read Pico wires it to GPIO1; on the Metalio board GPIO1 is the I2C
// SCL and the interrupt reaches the host through the TCA9555 wired-OR onto GPIO2, so this
// board claims no pin.
#if defined(PICO_BOARD_METALIO_EINK4_PLUS)
#define READ_PICO_ACCEL_INT1 GPIO_NUM_NC
#else
#define READ_PICO_ACCEL_INT1 GPIO_NUM_1
#endif

esp_err_t read_pico_init(read_pico_handle_t* hw) {
    if (hw == NULL) return ESP_ERR_INVALID_ARG;
    *hw = (read_pico_handle_t){ 0 };

    ESP_LOGI(TAG, "%s", READ_PICO_PRODUCT_NAME);
    ESP_LOGI(TAG, "Initializing 684x1216 EPD");
    // LUT 用 1K：LCD 路径下 epdiy 走 S3 向量实现，64K 里实际只用到 1K。
    // / 1K LUT: the LCD path uses the S3 vector impl; only 1K of the 64K is used.
#if defined(PICO_BOARD_METALIO_EINK4_PLUS)
    // 与例程 metalio_epd.c 一致：ED047TC2_1216（8 位/20MHz）+ 例程波形 + 横向原生。
    // Matches the demo's metalio_epd.c: ED047TC2_1216 (8-bit/20 MHz), its waveform, landscape.
    epd_init(&epd_board_read_pico, &ED047TC2_1216, EPD_LUT_1K);
#else
    epd_init(&epd_board_read_pico, &E0470_DISPLAY, EPD_LUT_1K);
#endif
    // CD 走 FCA9555，必须等板级 I2C 起来再探测。
    // / CD is on the FCA9555; probe after board I2C is up.
    read_pico_sd_start_probe();
#if defined(PICO_BOARD_METALIO_EINK4_PLUS)
    // 例程用 LVGL，按原生横屏排版；本仓库的界面是竖屏 684x1216，所以旋转必须和 Read Pico
    // 一致，不能照抄例程的 LANDSCAPE——那会让整个界面横过来。
    // The demo drives LVGL in native landscape; this repo's UI is portrait 684x1216, so the
    // rotation has to match Read Pico instead of copying the demo's LANDSCAPE, which lays the
    // whole interface on its side.
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
#else
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
#endif
    // 板级默认 12MHz，锁到实测稳定的 18MHz；消隐和 CKV 一起按新频率重解。
    // / Board default is 12 MHz; lock to the bench-stable 18 MHz and re-solve blanking/CKV.
    read_pico_epd_set_pclk(READ_PICO_PCLK_MHZ);
    esp_log_level_set("epdiy", ESP_LOG_WARN);
    epd_set_vcom(READ_PICO_VCOM_MV);

    // 默认灰阶表是开机从原厂表裁出来的，必须先算好再挂给 highlevel；
    // 差分刷新跳过整屏都在保持的前导相位。
    // / Default gray table is trimmed from the vendor table at boot; build it
    // before highlevel. Diff refresh skips leading all-hold phases.
#if defined(PICO_BOARD_METALIO_EINK4_PLUS)
    // 例程的波形表：DU 完备，GC/GL 完整 48 相。
    // The demo's waveform table: complete DU, full 48-phase GC/GL.
    my_waveform_init();
    hw->hl = epd_hl_init(&MY_WAVEFORM);
#else
    e0470_waveform_init();
    epd_set_leading_skip(true);
    hw->hl = epd_hl_init(&E0470_WAVEFORM);
#endif
    hw->framebuffer = epd_hl_get_framebuffer(&hw->hl);
    if (hw->framebuffer == NULL) {
        ESP_LOGE(TAG, "EPD framebuffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    sc7a20h_config_t acc = {
        .i2c_addr = SC7A20H_ADDR_DEFAULT,
        .int1_gpio = READ_PICO_ACCEL_INT1,
        .sensor = SC7A20H_CONFIG_IDLE,
    };
    if (sc7a20h_init(read_pico_i2c_bus(), &acc, &hw->sensor) == ESP_OK) {
        sc7a20h_sample_t sample;
        if (read_pico_accel_read(hw->sensor, &sample) == ESP_OK) {
            ESP_LOGI(
                TAG, "SC7A20H: X=%dmg Y=%dmg Z=%dmg",
                sample.x_mg, sample.y_mg, sample.z_mg
            );
        }
        sc7a20h_version(hw->sensor, &hw->sensor_whoami, &hw->sensor_version);
        sc7a20h_power_down(hw->sensor);
        hw->sensor_ready = true;
    } else {
        ESP_LOGW(TAG, "SC7A20H not ready");
    }

    read_pico_pmu_config_t pmu = READ_PICO_PMU_CONFIG_DEFAULT();
    hw->pmu_ready = read_pico_pmu_init(read_pico_i2c_bus(), &pmu) == ESP_OK;
    if (!hw->pmu_ready) {
        ESP_LOGW(TAG, "PMU / CW32 0x2A not ready");
    }

    cst836u_config_t tp = {
        .i2c_addr = READ_PICO_TOUCH_ADDR,
        .scl_speed_hz = CST836U_I2C_HZ_DEFAULT,
        .int_gpio = (gpio_num_t)READ_PICO_TOUCH_INT_GPIO,
        .rst_gpio = GPIO_NUM_NC,
        .reset_fn = read_pico_touch_reset,
    };
    if (cst836u_init(read_pico_i2c_bus(), &tp, &hw->touch) == ESP_OK) {
        hw->touch_ready = true;
    } else {
        ESP_LOGW(TAG, "CST836U not ready");
    }

    return ESP_OK;
}

void read_pico_deinit(read_pico_handle_t* hw) {
    if (hw == NULL) return;
    if (hw->touch != NULL) {
        (void)cst836u_deinit(hw->touch);
        hw->touch = NULL;
    }
    if (hw->pmu_ready) {
        (void)read_pico_pmu_deinit();
        hw->pmu_ready = false;
    }
    if (hw->sensor != NULL) {
        (void)sc7a20h_deinit(hw->sensor);
        hw->sensor = NULL;
    }
    epd_deinit();
    *hw = (read_pico_handle_t){ 0 };
}

void read_pico_sensor_wake(sc7a20h_handle_t h) {
    if (h && !sc7a20h_powered(h)) {
        sc7a20h_power_up(h);
    }
}

void read_pico_sensor_sleep(sc7a20h_handle_t h) {
    if (h && sc7a20h_powered(h)) {
        sc7a20h_power_down(h);
    }
}

void read_pico_accel_to_device(sc7a20h_sample_t* sample) {
    if (sample == NULL) return;
    int16_t xc = sample->x_mg;
    int16_t yc = sample->y_mg;
    int16_t zc = sample->z_mg;
    sample->x_mg = (int16_t)(-yc);
    sample->y_mg = (int16_t)(-xc);
    sample->z_mg = (int16_t)(-zc);
    xc = sample->x_raw;
    yc = sample->y_raw;
    zc = sample->z_raw;
    sample->x_raw = (int16_t)(-yc);
    sample->y_raw = (int16_t)(-xc);
    sample->z_raw = (int16_t)(-zc);
}

uint8_t read_pico_accel_map_aoi(uint8_t src) {
    uint8_t out = (uint8_t)(src & 0x40);
    if (src & 0x01) out |= 0x08;
    if (src & 0x02) out |= 0x04;
    if (src & 0x04) out |= 0x02;
    if (src & 0x08) out |= 0x01;
    if (src & 0x10) out |= 0x20;
    if (src & 0x20) out |= 0x10;
    return out;
}

esp_err_t read_pico_accel_read(sc7a20h_handle_t h, sc7a20h_sample_t* sample) {
    esp_err_t err = sc7a20h_read(h, sample);
    if (err == ESP_OK) read_pico_accel_to_device(sample);
    return err;
}
