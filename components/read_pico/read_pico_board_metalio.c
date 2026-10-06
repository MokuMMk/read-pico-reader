/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：Metalio E-Ink4-Plus（ESP32-S31）板级。实现与 read_pico_board.c 相同的对外接口，
 * 但硬件不同：EPD 电源是 TPS65185（经 TCA9555 控 VCOM/PWRUP/WAKEUP/PWR_GOOD），
 * 面板是 8 位并行的 ED047TC2 1216x684。
 *
 * English: Metalio E-Ink4-Plus (ESP32-S31) board layer. Implements the same public interface as
 * read_pico_board.c on different hardware: the EPD rails come from a TPS65185 (VCOM/PWRUP/
 * WAKEUP/PWR_GOOD driven through a TCA9555), and the panel is an 8-bit parallel ED047TC2 at
 * 1216x684.
 *
 * 寄存器值与上电时序参考 xiaozhi 体系的 Metalio E-Ink4-Plus 演示固件
 * （display/epd_board_metalio_eink4_plus.c 与 bus/tps65185.c，同芯片同面板），
 * I2C 由旧版 i2c_port_t 换成 IDF v6 的 i2c_master。
 * Register values and the power sequence follow the Metalio E-Ink4-Plus demo firmware
 * (display/epd_board_metalio_eink4_plus.c and bus/tps65185.c, same chip and panel), with the old
 * i2c_port_t interface replaced by IDF v6's i2c_master.
 *
 * 冻结：TPS65185 的三条控制线（VCOM/PWRUP/WAKEUP）只能经 TCA9555 改，顺序不得调整——
 * 手册 8.4 要求 PWRUP/VCOM 先降、WAKEUP 后降，否则升压会在关机时反冲。
 * Frozen: the three TPS65185 control lines (VCOM/PWRUP/WAKEUP) are only reachable through the
 * TCA9555 and their order must not change; datasheet 8.4 requires PWRUP/VCOM to fall before
 * WAKEUP, or the shutdown kicks back into the boost.
 *
 * 移植自 Metalio E-Ink4-Plus 示例固件（MIT，Copyright (c) 2025 Shenzhen Xinzhi Future
 * Technology Co., Ltd. 与 Project Contributors），见 licenses/METALIO-MIT.txt。
 * Portions ported from the Metalio E-Ink4-Plus demo firmware (MIT); see
 * licenses/METALIO-MIT.txt.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "epd_board.h"
#include "epd_lcd.h"
#include "epdiy.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "fca9555.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_board.h"
#include "read_pico_epd_timing.h"
#include "sdkconfig.h"
#include "tps65185.h"

/// 面板 VCOM（绝对值 mV）。取自 Metalio config.h 的 EPD_VCOM_MV；本板 VCOM 由 TPS65185
/// 在轨就绪后写入，与 Read Pico 存在 PMU 里的做法不同。
/// Panel VCOM in absolute mV, from Metalio's config.h EPD_VCOM_MV. The TPS65185 takes it
/// once the rails are up, unlike Read Pico where the PMU stores it.
#define BOARD_EPD_VCOM_MV 1200

#define TAG "metalio"

/* ---- 引脚：照 Metalio config.h / 原理图 V1.2 ---- */
/* Pins, from Metalio's config.h and schematic V1.2. */

#define EPD_D0 GPIO_NUM_8
#define EPD_D1 GPIO_NUM_9
#define EPD_D2 GPIO_NUM_10
#define EPD_D3 GPIO_NUM_11
#define EPD_D4 GPIO_NUM_12
#define EPD_D5 GPIO_NUM_13
#define EPD_D6 GPIO_NUM_14
#define EPD_D7 GPIO_NUM_15

#define EPD_CKH GPIO_NUM_16
#define EPD_LEH GPIO_NUM_17
#define EPD_OE GPIO_NUM_18
#define EPD_STH GPIO_NUM_4   /* 1.3/1.4 版硬件 / rev 1.3 and 1.4 boards */
#define EPD_STV GPIO_NUM_35
#define EPD_MODE GPIO_NUM_19 /* 1.4 版硬件；1.2 版是 36 / rev 1.4; 36 on rev 1.2 */
#define EPD_BORDER GPIO_NUM_37
#define EPD_CKV GPIO_NUM_40

#define BOARD_I2C_SDA GPIO_NUM_0
#define BOARD_I2C_SCL GPIO_NUM_1
#define BOARD_IOE_INT GPIO_NUM_2

#define BOARD_TP_RST_GPIO GPIO_NUM_5 /* FT6336U INT；RST 在 TCA9555 P12 */

/// USB 通路选择：YU1 模拟开关的 SEL。低=USB_DM/DP 那一路，高=ESP_DP/DN（S31 自己）。
/// 例程在初始化时拉高（注释：GPIO36 默认高，非虚拟 U 盘）。本固件以前完全不碰它，
/// 引脚悬空时开关状态不定，表现就是"不按按键看不到 USB 串口"。
/// / USB path select, the SEL input of the YU1 analog switch. Low picks the USB_DM/DP path, high
/// picks ESP_DP/ESP_DN (the S31 itself). The demo drives it high at init, noting that GPIO36
/// defaults high. This firmware never touched it, so a floating pin left the switch undecided,
/// which is why the serial port only appeared after forcing download mode by hand.
#define BOARD_USB_SEL_GPIO GPIO_NUM_36

/* TCA9555 位号：编号即手册的 P0x/P1x。 / TCA9555 bit numbers, straight from the datasheet P0x/P1x. */
#define IOE_VCOM_CTRL 0  /* P00 */
#define IOE_PWRUP 1      /* P01 */
#define IOE_WAKEUP 2     /* P02 */
#define IOE_MOTOR 3      /* P03 */
#define IOE_PA_EN 4      /* P04 */
#define IOE_CAM_SCR 5    /* P05 */
#define IOE_PWR_GOOD 7   /* P07 */
#define IOE_TP_RST 10    /* P12 */
#define IOE_BT_PA_PWR 12 /* P14 */

/// Port0 输出位：TPS 三条控制线加马达/功放/摄像屏电。
/// / Port0 outputs: the three TPS control lines plus motor, amplifier and camera/screen power.
#define IOE_P0_OUT (FCA9555_P00 | FCA9555_P01 | FCA9555_P02)
/// Port1 输出位：触摸复位、蓝牙/功放电源、关机脉冲。
/// P13 必须在这里，否则它会被配成输入，关机脉冲拉不动——表现就是"脉冲串跑了但设备不关"。
/// / Port1 outputs: touch reset, the Bluetooth/amplifier rail, and the shutdown pulse.
/// P13 has to be listed: left out, it gets configured as an input and the shutdown pulse cannot
/// drive it, which looks exactly like "the pulse train ran but the device stayed on".
#define IOE_P1_OUT (FCA9555_P12 | FCA9555_P13 | FCA9555_P14)

/// TCA9555 地址按 A2A1A0 试这几个；板上常见 0x20，深睡后锁存不变。
/// / Candidate TCA9555 addresses by A2A1A0; 0x20 is the usual strap and does not move after sleep.
static const uint8_t k_ioe_addrs[] = { 0x20, 0x24, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27 };

static i2c_master_bus_handle_t s_bus;
static fca9555_handle_t s_ioe;
static tps65185_handle_t s_tps;

static uint8_t s_p0_out; /* Port0 输出锁存 / Port0 output latch */
static uint8_t s_p1_out; /* Port1 输出锁存 / Port1 output latch */
static bool s_ioe_ok;
static bool s_tps_ok;
static bool s_rails_on;

static lcd_bus_config_t lcd_config = {
    .clock = EPD_CKH,
    .ckv = EPD_CKV,
    .leh = EPD_LEH,
    .start_pulse = EPD_STH,
    .stv = EPD_STV,
    .data = { EPD_D0, EPD_D1, EPD_D2, EPD_D3, EPD_D4, EPD_D5, EPD_D6, EPD_D7,
              /* 面板是 8 位，高 8 项只是结构体占位，不参与输出。 */
              /* The panel is 8-bit; the upper eight entries only fill the struct. */
              EPD_D7, EPD_D7, EPD_D7, EPD_D7, EPD_D7, EPD_D7, EPD_D7, EPD_D7 },
};

/* ---- I2C ---- */

static void i2c_bus_recover(void) {
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_I2C_SDA) | (1ULL << BOARD_I2C_SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_I2C_SDA, 1);
    gpio_set_level(BOARD_I2C_SCL, 1);
    esp_rom_delay_us(20);

    // 从机半掉电时会钳住 SDA，只有手工打时钟能解开。
    // A half-powered slave can hold SDA down; only clocking it by hand clears that.
    for (int i = 0; i < 9 && gpio_get_level(BOARD_I2C_SDA) == 0; ++i) {
        gpio_set_level(BOARD_I2C_SCL, 0);
        esp_rom_delay_us(5);
        gpio_set_level(BOARD_I2C_SCL, 1);
        esp_rom_delay_us(5);
    }
    gpio_set_level(BOARD_I2C_SDA, 0);
    esp_rom_delay_us(5);
    gpio_set_level(BOARD_I2C_SCL, 1);
    esp_rom_delay_us(5);
    gpio_set_level(BOARD_I2C_SDA, 1);
    esp_rom_delay_us(5);
}

/* ---- TCA9555 ---- */

static void ioe_write(void) {
    if (!s_ioe_ok) return;
    esp_err_t err = fca9555_set_output(s_ioe, 0, s_p0_out);
    if (err != ESP_OK) ESP_LOGW(TAG, "P0 write: %s", esp_err_to_name(err));
    err = fca9555_set_output(s_ioe, 1, s_p1_out);
    if (err != ESP_OK) ESP_LOGW(TAG, "P1 write: %s", esp_err_to_name(err));
}

static void ioe_set_bit(int bit, bool high) {
    const uint16_t mask = (uint16_t)(1u << bit);
    uint8_t* latch = bit < 8 ? &s_p0_out : &s_p1_out;
    if (high) {
        *latch |= (uint8_t)(mask & 0xFF);
    } else {
        *latch &= (uint8_t)~(mask & 0xFF);
    }
    ioe_write();
}

/// 读扩展器一位。读失败时返回 on_failure：按键要按"未按下"（高）算，PWR_GOOD 要按
/// "未就绪"（低）算，两者相反，不能共用一个默认值。
/// / Read one expander bit. A failed read returns on_failure: a key must look released (high)
/// while PWR_GOOD must look not-ready (low), so they cannot share one default.
static bool ioe_read_bit(int bit, bool on_failure) {
    if (!s_ioe_ok) return on_failure;
    uint8_t value = 0xFF;
    if (fca9555_read_input(s_ioe, bit < 8 ? 0 : 1, &value) != ESP_OK) return on_failure;
    return (value & (1u << (bit & 7))) != 0;
}

static bool ioe_probe(void) {
    for (size_t i = 0; i < sizeof(k_ioe_addrs) / sizeof(k_ioe_addrs[0]); ++i) {
        fca9555_config_t cfg = FCA9555_CONFIG_DEFAULT();
        cfg.i2c_addr = k_ioe_addrs[i];
        cfg.scl_speed_hz = 100000;
        if (fca9555_init(s_bus, &cfg, &s_ioe) != ESP_OK) continue;
        // 探测靠读一次输入寄存器：ACK 但读不回来说明不是 9555。
        // Probing reads an input register: an ACK that will not read back is not a 9555.
        uint8_t probe = 0;
        if (fca9555_read_reg(s_ioe, FCA9555_REG_IN0, &probe) != ESP_OK) {
            fca9555_deinit(s_ioe);
            s_ioe = NULL;
            continue;
        }
        // 登记给别的层用（电源层要读按键、发关机脉冲）。
        // Register it for other layers: the power layer reads the keys and pulses shutdown.
        fca9555_set_default(s_ioe);
        ESP_LOGI(TAG, "TCA9555 at 0x%02X", k_ioe_addrs[i]);
        return true;
    }
    return false;
}

/* ---- EPD 电源 / Rails ---- */

// board_set_ctrl 定义在本节末尾，上电与掉电流程都要用它，所以先声明。
// board_set_ctrl is defined at the end of this section and both the power-on and power-off paths
// call it, so declare it up here.
static void board_set_ctrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask);

/// 上电：PWRUP 保持低、WAKEUP 拉高进 STANDBY；写 UPSEQ；拉高 PWRUP 起升压；
/// 等扩展器的 PWR_GOOD；再写 ENABLE 与 VCOM。
/// Power on: hold PWRUP low and raise WAKEUP to reach STANDBY, write UPSEQ, raise PWRUP to
/// start the boost, wait for the expander's PWR_GOOD, then write ENABLE and VCOM.
static void board_poweron(epd_ctrl_state_t* state) {
    if (s_rails_on) return;
    if (!s_ioe_ok) {
        ESP_LOGE(TAG, "poweron without IO expander: panel power unavailable");
        return;
    }

    // 先把"输出关、模式低、STV 高"推给引脚；只改结构体不推引脚等于没做。
    // Push "output off, mode low, STV high" to the pins first; changing the struct without
    // calling set_ctrl does nothing at all.
    state->ep_output_enable = false;
    state->ep_mode = false;
    state->ep_stv = true;
    const epd_ctrl_state_t ctrl_mask = {
        .ep_output_enable = true,
        .ep_mode = true,
        .ep_stv = true,
    };
    board_set_ctrl(state, &ctrl_mask);

    ioe_set_bit(IOE_VCOM_CTRL, false);
    ioe_set_bit(IOE_PWRUP, false);
    ioe_set_bit(IOE_WAKEUP, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t probe = ESP_FAIL;
    for (int i = 0; i < 3 && probe != ESP_OK; ++i) {
        probe = tps65185_probe(s_tps);
        if (probe != ESP_OK) vTaskDelay(pdMS_TO_TICKS(10));
    }

    uint8_t revid = 0;
    if (probe == ESP_OK) (void)tps65185_read(s_tps, TPS_REG_REVID, &revid);
    ESP_LOGI(TAG, "STANDBY REVID=0x%02X probe=%s", revid, esp_err_to_name(probe));

    // UPSEQ 决定各路升压顺序；写失败也继续，后面靠 PWR_GOOD 判断成败。
    // UPSEQ sets the rail bring-up order; failures are tolerated and PWR_GOOD decides.
    (void)tps65185_write(s_tps, TPS_REG_UPSEQ0, 0xE4);
    (void)tps65185_write(s_tps, TPS_REG_UPSEQ1, 0x55);

    ioe_set_bit(IOE_VCOM_CTRL, true);
    ioe_set_bit(IOE_PWRUP, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    bool good = false;
    for (int retry = 0; retry < 3 && !good; ++retry) {
        if (retry > 0) {
            ESP_LOGW(TAG, "TPS retry %d/3", retry + 1);
            ioe_set_bit(IOE_PWRUP, false);
            vTaskDelay(pdMS_TO_TICKS(100));
            ioe_set_bit(IOE_PWRUP, true);
        }
        for (int ms = 0; ms < 200; ++ms) {
            if (ioe_read_bit(IOE_PWR_GOOD, false)) {
                ESP_LOGI(TAG, "PWR_GOOD @%dms", ms);
                good = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (!good) {
        ESP_LOGE(TAG, "TPS power-on failed; PWR_GOOD never asserted");
        // 保持三线为高，方便用表量 VN；下一次上电会重新走一遍。
        // Leave the three lines high so VN can be probed; the next power-on retries.
        s_tps_ok = false;
        return;
    }

    if (probe == ESP_OK) {
        esp_err_t err = tps65185_write(s_tps, TPS_REG_ENABLE, TPS_ENABLE_ALL_RAILS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ENABLE write: %s", esp_err_to_name(err));
            s_tps_ok = false;
            return;
        }
        (void)tps65185_set_vcom(s_tps, BOARD_EPD_VCOM_MV);
    }

    // 轨就绪后才允许输出：这一步之前 OE 一直是低的。
    // Output is only allowed once the rails are up; until here OE has been held low.
    state->ep_output_enable = true;
    state->ep_stv = true;
    const epd_ctrl_state_t out_mask = { .ep_output_enable = true };
    board_set_ctrl(state, &out_mask);

    s_rails_on = true;
    s_tps_ok = (probe == ESP_OK);

    // 读回关键寄存器：面板"通电但无显示"最常见的原因就是高压没真起来——ENABLE 没写进、
    // PG 没全绿、或 VCOM 还是 0。没有这三项，波形再对也画不出东西。
    // Read the key registers back. "Powered but blank" is most often the high voltage never
    // coming up: ENABLE not latched, PG not all good, or VCOM still zero. Without those three the
    // waveform cannot put anything on the glass no matter how correct it is.
    if (s_tps_ok) {
        uint8_t pg = 0;
        uint8_t en = 0;
        uint8_t v1 = 0;
        uint8_t v2 = 0;
        uint8_t i1 = 0;
        uint8_t i2 = 0;
        (void)tps65185_read(s_tps, TPS_REG_PG, &pg);
        (void)tps65185_read(s_tps, TPS_REG_ENABLE, &en);
        (void)tps65185_read(s_tps, TPS_REG_VCOM1, &v1);
        (void)tps65185_read(s_tps, TPS_REG_VCOM2, &v2);
        (void)tps65185_read(s_tps, TPS_REG_INT1, &i1);
        (void)tps65185_read(s_tps, TPS_REG_INT2, &i2);
        const unsigned vcom = ((unsigned)(v2 & 0x01) << 8) | v1;
        ESP_LOGI(
            TAG,
            "TPS: ENABLE=0x%02X (want 0x%02X) PG=0x%02X (want 0x%02X ready=%d) "
            "VCOM=%umV INT1=0x%02X INT2=0x%02X",
            en, TPS_ENABLE_ALL_RAILS, pg, TPS_PG_ALL_READY_MASK,
            (pg & TPS_PG_ALL_READY_MASK) == TPS_PG_ALL_READY_MASK, vcom * 10, i1, i2
        );
    }

    ESP_LOGI(TAG, "rails up (tps=%s)", s_tps_ok ? "ok" : "unreachable");
}

static void board_poweroff(epd_ctrl_state_t* state) {
    state->ep_output_enable = false;
    state->ep_mode = false;
    state->ep_stv = false;
    const epd_ctrl_state_t mask = {
        .ep_output_enable = true,
        .ep_mode = true,
        .ep_stv = true,
    };
    board_set_ctrl(state, &mask);

    // 手册 8.4：先落 PWRUP/VCOM，再落 WAKEUP。
    // Datasheet 8.4: PWRUP/VCOM fall first, WAKEUP last.
    ioe_set_bit(IOE_VCOM_CTRL, false);
    ioe_set_bit(IOE_PWRUP, false);
    vTaskDelay(pdMS_TO_TICKS(50));
    ioe_set_bit(IOE_WAKEUP, false);
    s_rails_on = false;
}

static void board_set_ctrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
    if (mask->ep_output_enable) gpio_set_level(EPD_OE, state->ep_output_enable ? 1 : 0);
    if (mask->ep_mode) gpio_set_level(EPD_MODE, state->ep_mode ? 1 : 0);
    if (mask->ep_stv) gpio_set_level(EPD_STV, state->ep_stv ? 1 : 0);
}

static void board_init(uint32_t epd_row_width) {
    (void)epd_row_width;

    // 第一件事：把 USB 通路指到 S31 自己，保证串口从开机起就在。
    // First thing: point the USB path at the S31 itself so the serial port is there from boot.
    {
        gpio_reset_pin(BOARD_USB_SEL_GPIO);
        gpio_set_direction(BOARD_USB_SEL_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(BOARD_USB_SEL_GPIO, 1);
        ESP_LOGI(TAG, "USB path select (GPIO%d) -> ESP", (int)BOARD_USB_SEL_GPIO);
    }

    i2c_bus_recover();
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = READ_PICO_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    s_ioe_ok = ioe_probe();
    if (!s_ioe_ok) {
        ESP_LOGE(TAG, "no TCA9555: panel power and touch reset are unavailable");
    } else {
        // P07 PWR_GOOD 与 P16 是输入，其余按输出配置。
        // P07 PWR_GOOD and P16 are inputs; everything else is an output.
        const uint8_t p0_input = (uint8_t)~IOE_P0_OUT;
        const uint8_t p1_input = (uint8_t)~IOE_P1_OUT;
        ESP_ERROR_CHECK(fca9555_set_config(s_ioe, 0, p0_input));
        ESP_ERROR_CHECK(fca9555_set_config(s_ioe, 1, p1_input));
        s_p0_out = 0;
        s_p1_out = (uint8_t)(FCA9555_P12 >> 8 | FCA9555_P14 >> 8);
        ioe_write();
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    ESP_ERROR_CHECK(tps65185_init(s_bus, TPS65185_ADDR_DEFAULT, &s_tps));

    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << BOARD_IOE_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&int_cfg);

    const gpio_num_t ctrl_pins[] = { EPD_OE, EPD_MODE, EPD_BORDER, EPD_STV };
    for (size_t i = 0; i < sizeof(ctrl_pins) / sizeof(ctrl_pins[0]); ++i) {
        gpio_reset_pin(ctrl_pins[i]);
        gpio_set_direction(ctrl_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(ctrl_pins[i], 0);
    }

    const EpdDisplay_t* display = epd_get_display();
    const int init_pclk_mhz = display->bus_speed * 8 / display->bus_width;
    read_pico_epd_scan_t scan;
    read_pico_epd_scan(init_pclk_mhz, READ_PICO_EPD_SCAN_FULL, &scan);
    read_pico_epd_init_lcd(&lcd_config, display->bus_width, display->width, display->height, &scan);
    ESP_LOGI(
        TAG,
        "init %dx%d bus=%d pclk=%dMHz ioexp=%s",
        display->width,
        display->height,
        display->bus_width,
        display->bus_speed,
        s_ioe_ok ? "ok" : "FAIL"
    );
}

static void board_deinit(void) {
    if (s_rails_on) board_poweroff(epd_ctrl_state());
    epd_lcd_deinit();
    if (s_tps) {
        tps65185_deinit(s_tps);
        s_tps = NULL;
    }
    if (s_ioe) {
        fca9555_deinit(s_ioe);
        s_ioe = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

static void board_measure_vcom(epd_ctrl_state_t* state) {
    board_poweron(state);
}

static float board_temperature(void) {
    // 面板侧温度取 TPS 的热敏电阻；轨没开时芯片不应答。
    // Panel-side temperature comes from the TPS thermistor; the chip will not answer with the
    // rails down.
    if (!s_rails_on || s_tps == NULL) return 25.0f;
    const int8_t t = tps65185_temperature(s_tps);
    return t == -128 ? 25.0f : (float)t;
}

static void board_set_vcom(int value) {
    if (s_tps) (void)tps65185_set_vcom(s_tps, (unsigned)value);
}

const EpdBoardDefinition epd_board_metalio_eink4_plus = {
    .init = board_init,
    .deinit = board_deinit,
    .set_ctrl = board_set_ctrl,
    .poweron = board_poweron,
    .poweroff = board_poweroff,
    .measure_vcom = board_measure_vcom,
    .get_temperature = board_temperature,
    .set_vcom = board_set_vcom,
    .gpio_set_direction = NULL,
    .gpio_read = NULL,
    .gpio_write = NULL,
};

/* ---- 对外接口：与 read_pico_board.c 同名同义 ---- */
/* Public interface: same names and meanings as read_pico_board.c. */

i2c_master_bus_handle_t read_pico_i2c_bus(void) {
    return s_bus;
}

sy7636a_handle_t read_pico_sy7636a(void) {
    // 本板没有 SY7636A；调用方必须容忍 NULL。
    // This board has no SY7636A; callers must tolerate NULL.
    return NULL;
}

fca9555_handle_t read_pico_fca9555(void) {
    return s_ioe;
}

bool read_pico_rails_on(void) {
    return s_rails_on;
}

int read_pico_ioe_int_level(void) {
    return gpio_get_level(BOARD_IOE_INT);
}

bool read_pico_sd_present(void) {
    // 本板的卡座没有 CD 线，按"总是有卡"处理，由挂载失败去判定。
    // This board's slot has no card-detect line, so report present and let the mount decide.
    return true;
}

esp_err_t read_pico_tp_rst(bool high) {
    if (!s_ioe_ok) return ESP_ERR_INVALID_STATE;
    ioe_set_bit(IOE_TP_RST, high);
    return ESP_OK;
}

esp_err_t read_pico_touch_reset(void) {
    // FT6336U：Trst ≥ 5ms，放开后 Trsi ≥ 300ms。
    // FT6336U: Trst is at least 5 ms, and Trsi at least 300 ms after release.
    esp_err_t err = read_pico_tp_rst(false);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    err = read_pico_tp_rst(true);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(300));
    return ESP_OK;
}

void read_pico_clear_ioe_int(void) {
    if (s_ioe) (void)fca9555_clear_int(s_ioe);
}

esp_err_t read_pico_toggle_vcomctl(void) {
    if (!s_ioe_ok) return ESP_ERR_INVALID_STATE;
    // TPS65185 没有 SY7636A 那样的自动/外部 VCOM 选择位，这里只翻转 P00 并回报当前值。
    // The TPS65185 has no auto/external VCOM select like the SY7636A, so this just flips P00.
    const bool next = (s_p0_out & (uint8_t)FCA9555_P00) == 0;
    ioe_set_bit(IOE_VCOM_CTRL, next);
    return ESP_OK;
}

esp_err_t read_pico_get_status(read_pico_status_t* status) {
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));

    uint8_t p0 = 0;
    uint8_t p1 = 0;
    if (s_ioe_ok) {
        (void)fca9555_read_input(s_ioe, 0, &p0);
        (void)fca9555_read_input(s_ioe, 1, &p1);
    }
    status->ioe_input = (uint16_t)(p0 | (p1 << 8));
    status->ioe_output = (uint16_t)(s_p0_out | (s_p1_out << 8));
    status->ioe_int_level = read_pico_ioe_int_level();
    status->rails_on = s_rails_on;

    // 界面读的是 sy.* 这几个字段；本板用 TPS65185 填同一组语义（VCOM 与温度）。
    // The UI reads these sy.* fields; on this board the TPS65185 fills the same meanings.
    status->sy_live = s_rails_on && s_tps_ok;
    status->sy.on = s_rails_on;
    status->sy.vcom_mv = (int)BOARD_EPD_VCOM_MV;
    const int8_t t = (s_rails_on && s_tps) ? tps65185_temperature(s_tps) : -128;
    status->sy.temperature_c = t == -128 ? 25 : t;
    return ESP_OK;
}

esp_err_t read_pico_get_ioe_status(read_pico_status_t* status) {
    return read_pico_get_status(status);
}

esp_err_t read_pico_get_ioe_status_full(read_pico_status_t* status) {
    // 9555 没有需要连读缓存的方向寄存器语义差异，读全 8 个寄存器也只是多几次传输。
    // The 9555 has no cached-direction semantics to refresh, so a full read is just more traffic.
    if (s_ioe) {
        fca9555_map_t map = { 0 };
        (void)fca9555_read_all(s_ioe, &map);
    }
    return read_pico_get_status(status);
}

static read_pico_i2c_census_t s_census;

void read_pico_i2c_census_take(void) {
    // 普查项按本板实际器件给出；地址取自 Metalio 的 config.h。
    // The census lists this board's actual parts, with addresses from Metalio's config.h.
    static const struct {
        uint8_t addr;
        const char* name;
    } map[READ_PICO_I2C_DEV_N] = {
        { TPS65185_ADDR_DEFAULT, "TPS65185" },
        { 0x51, "PCF8563" },
        { 0x55, "BQ27220" },
        { 0x19, "SC7A20H" },
        { 0x38, "FT6336U" },
    };
    static read_pico_i2c_census_t census;
    memset(&census, 0, sizeof(census));
    census.all_online = true;
    for (int i = 0; i < READ_PICO_I2C_DEV_N; ++i) {
        census.dev[i].name = map[i].name;
        census.dev[i].addr = map[i].addr;
        const bool online = s_bus != NULL && i2c_master_probe(s_bus, map[i].addr, 50) == ESP_OK;
        census.dev[i].online = online;
        if (!online) {
            census.offline_n++;
            census.all_online = false;
        }
    }
    s_census = census;
}

const read_pico_i2c_census_t* read_pico_i2c_census(void) {
    return &s_census;
}
