/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：read_pico_pmu.h 那套接口在 Metalio E-Ink4-Plus 上的实现。这块板没有 CW32，
 * 所以电量来自 BQ27220、按键来自 TCA9555、时间来自 PCF8563、VCOM 来自 TPS65185。
 * 接口名字沿用 read_pico_pmu 是为了让 main/ 一行不改，但内部没有一条 CW32 协议。
 *
 * English: the read_pico_pmu.h interface on the Metalio E-Ink4-Plus. That board has no CW32, so
 * the battery comes from a BQ27220, the keys from the TCA9555, the clock from a PCF8563 and VCOM
 * from the TPS65185. The interface keeps the read_pico_pmu names so main/ does not change, but
 * none of the CW32 protocol survives underneath.
 *
 * 移植自 Metalio E-Ink4-Plus 示例固件（MIT，Copyright (c) 2025 Shenzhen Xinzhi Future
 * Technology Co., Ltd. 与 Project Contributors），见 licenses/METALIO-MIT.txt。
 * Portions ported from the Metalio E-Ink4-Plus demo firmware (MIT); see
 * licenses/METALIO-MIT.txt.
 *
 * 冻结：CW32 相关的 read_pico_pmu_cmd / action / UID / 事件队列在这里一律**接受并忽略**，
 * 不返回错误——main/ 里二十多处调用按"命令已送达"处理，报错会让界面走异常分支。
 * Frozen: the CW32-side calls (cmd, action, uid, event queue) accept and ignore here and never
 * return an error. The twenty-odd call sites in main/ treat them as delivered, and an error would
 * push the UI down its failure paths.
 */

#include "read_pico_pmu.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "fca9555.h"
#include "read_pico_pmu_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "pmu_metalio"

/// 面板 VCOM 标称值（绝对值 mV）。/ Nominal panel VCOM in absolute mV.
#define PMU_METALIO_VCOM_MV 1200

/* ---- 地址与寄存器 / Addresses and registers ---- */
#define BQ27220_ADDR 0x55
#define BQ27220_REG_VOLTAGE 0x08  ///< u16，mV，小端 / u16 little-endian millivolts
#define BQ27220_REG_CURRENT 0x0C  ///< i16，mA，小端；正=充电 / i16 milliamps, positive is charging

#define PCF8563_ADDR 0x51
#define PCF8563_REG_CTRL1 0x00
#define PCF8563_REG_SECONDS 0x02  ///< BCD；bit7 是 VL（时钟有效位），1 表示走时不可信
#define PCF8563_REG_MINUTES 0x03
#define PCF8563_REG_HOURS 0x04
#define PCF8563_REG_DAYS 0x05
#define PCF8563_REG_MONTHS 0x07  ///< bit7 是世纪位 / bit7 is the century flag
#define PCF8563_REG_YEARS 0x08
#define PCF8563_VL_BIT 0x80

/* ---- TCA9555 上的按键位 / Key bits on the TCA9555 ---- */
#define IOE_KEY_VOL_UP 8    ///< P10
#define IOE_KEY_VOL_DOWN 9  ///< P11
#define IOE_KEY_POWER 15    ///< P17

/// 关机脉冲：高低各保持 100ms，共 15 次，与例程一致。
/// / Power-off pulse train: 100 ms high and low, 15 times, matching the demo firmware.
#define PWR_PULSE_INTERVAL_MS 100
#define PWR_PULSE_COUNT 15

/// 长按判定与去抖，与例程对齐。
/// / Long-press threshold and debounce, matching the demo firmware.
#define KEY_LONG_MS 500

/// 电源键按住多久关整机。与例程的 POWER_LONG_PRESS_MS 一致。
/// / How long the power key must be held to cut the power, matching the demo firmware.
#define POWER_LONG_PRESS_MS 3000

/// 消抖：按下后至少经过这么久才认，然后立刻关机。取值略大于一次扫描间隔。
/// / Debounce: the press must last this long before it counts, then shutdown follows at once.
/// Slightly longer than one scan interval.
#define POWER_DEBOUNCE_MS 150

/// 两轮脉冲之间的间隔，与例程 PwrOffTask 里的 200ms 一致。
/// / Gap between pulse trains, matching the 200 ms in the demo's PwrOffTask.
#define POWER_OFF_RETRY_MS 200

/// 关机任务：栈要放得下 fca9555 的一次 I2C 事务，优先级高于主循环（例程也是提高优先级）。
/// / Shutdown task: the stack must hold one fca9555 I2C transaction, and it runs above
/// the main loop, as the demo's does.
#define POWER_OFF_TASK_STACK 3072
#define POWER_OFF_TASK_PRIO (tskIDLE_PRIORITY + 5)
#define KEY_DEBOUNCE_MS 30

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_gauge;
static i2c_master_dev_handle_t s_rtc;
static fca9555_handle_t s_ioe;
static bool s_ready;

static pmu_snapshot_t s_snapshot;
static bool s_key_short;
static bool s_key_wakeup;

// 按键边沿跟踪：记录上一次采样电平与按下时刻。
// Key edge tracking: last sampled level and the moment it went down.
static bool s_key_down[3];
static TickType_t s_key_down_tick[3];

static int s_layout_ok = -1;
// 关机脉冲只发一次，避免长按期间反复触发。
// The shutdown pulse fires once; a held key must not retrigger it.
static bool s_power_off_started;
// 上一次的按键掩码，只在变化时打日志。
// Last key mask; logged only on change.
static uint8_t s_last_mask;
// 关机任务句柄；非空表示已经在关机路上。
// Shutdown task handle; non-NULL means the shutdown is already in flight.
static TaskHandle_t s_power_off_task;

/* ---- I2C 小工具 / I2C helpers ---- */

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* buf, size_t n) {
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, 100);
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

// 句柄来自 fca9555 的登记处（板级层建好后登记），不在这里自建：同一地址上第二个句柄会失败，
// 而 read_pico 已经 require 了本组件，直接引用 read_pico 的访问函数又会成环。
// The handle comes from the fca9555 registry, filled in by the board layer. It is not created
// here: a second handle on the same address fails, and calling read_pico's accessor directly
// would be a cycle since read_pico already requires this component.

static bool ioe_ready(void) {
    if (s_layout_ok >= 0) return s_layout_ok != 0;
    s_layout_ok = s_ioe != NULL ? 1 : 0;
    return s_layout_ok != 0;
}

static bool ioe_key_level(int bit) {
    if (s_ioe == NULL) return true;
    uint8_t value = 0xFF;
    if (fca9555_read_input(s_ioe, bit < 8 ? 0 : 1, &value) != ESP_OK) return true;
    return (value & (1u << (bit & 7))) != 0;
}

/* ---- BCD / 时间 / BCD and time ---- */

static uint8_t bcd2bin(uint8_t v) {
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}

static uint8_t bin2bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/// 电压 → 电量百分比。BQ27220 在本板上没配电池模型，所以按锂电单节线性估算。
/// / Voltage to state of charge. The BQ27220 has no battery model configured on this board, so
/// this is a single-cell linear estimate.
static uint16_t voltage_to_permille(uint16_t mv) {
    struct {
        uint16_t mv;
        uint16_t pm;
    } static const kCurve[] = {
        { 4200, 1000 }, { 4000, 800 }, { 3850, 600 }, { 3750, 400 },
        { 3650, 200 },  { 3500, 100 }, { 3300, 30 },  { 3200, 0 },
    };
    const size_t n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (mv >= kCurve[0].mv) return 1000;
    if (mv <= kCurve[n - 1].mv) return 0;
    for (size_t i = 1; i < n; ++i) {
        if (mv >= kCurve[i].mv) {
            const uint16_t hi = kCurve[i - 1].mv;
            const uint16_t lo = kCurve[i].mv;
            const uint16_t span = (uint16_t)(hi - lo);
            const uint32_t frac = span ? (uint32_t)(mv - lo) * (kCurve[i - 1].pm - kCurve[i].pm) / span
                                       : 0;
            return (uint16_t)(kCurve[i].pm + frac);
        }
    }
    return 0;
}

static void read_battery(void) {
    uint8_t buf[2] = { 0 };
    if (s_gauge == NULL || reg_read(s_gauge, BQ27220_REG_VOLTAGE, buf, 2) != ESP_OK) return;
    const uint16_t mv = (uint16_t)(buf[0] | (buf[1] << 8));
    if (mv < 2000 || mv > 5000) return;  // 明显无效就不覆盖上一次 / implausible, keep the last
    s_snapshot.battery_mv = mv;
    s_snapshot.soc_permille = voltage_to_permille(mv);

    int16_t ma = 0;
    if (reg_read(s_gauge, BQ27220_REG_CURRENT, buf, 2) == ESP_OK) {
        ma = (int16_t)(buf[0] | (buf[1] << 8));
    }
    // 电流符号决定在充还是在放；界面按 charge_state 分档显示。
    // The current sign says charging or discharging; the UI maps charge_state to its labels.
    if (ma > 20) {
        s_snapshot.charge_state = 1;
    } else if (ma < -20) {
        s_snapshot.charge_state = 0;
    } else {
        s_snapshot.charge_state = s_snapshot.battery_mv > 4150 ? 2 : 0;
    }
}

static void read_rtc(void) {
    uint8_t r[7] = { 0 };
    if (s_rtc == NULL) return;
    if (reg_read(s_rtc, PCF8563_REG_SECONDS, r, 7) != ESP_OK) return;
    if (r[0] & PCF8563_VL_BIT) {
        // VL 置位说明掉过电，走时不可信；此时不要用它覆盖系统时间。
        // VL set means the clock lost power and cannot be trusted; do not overwrite system time.
        s_snapshot.time_synced = 0;
        return;
    }
    struct tm t = { 0 };
    t.tm_sec = bcd2bin(r[0] & 0x7F);
    t.tm_min = bcd2bin(r[1] & 0x7F);
    t.tm_hour = bcd2bin(r[2] & 0x3F);
    t.tm_mday = bcd2bin(r[3] & 0x3F);
    t.tm_mon = bcd2bin(r[5] & 0x1F) - 1;
    t.tm_year = bcd2bin(r[6]) + 100;  // 2000 起算 / years since 2000
    t.tm_isdst = 0;
    const time_t utc = mktime(&t);
    if (utc < 1704067200) return;
    s_snapshot.unix_sec = (uint32_t)utc;
    s_snapshot.time_synced = 1;
}

static void read_keys(void) {
    static const int kBits[3] = { IOE_KEY_POWER, IOE_KEY_VOL_UP, IOE_KEY_VOL_DOWN };
    // 用 FreeRTOS 的 tick 计时：只需要比较按住时长，不需要真实时间。
    // Timing comes from the FreeRTOS tick: only the held duration matters, not wall time.
    const TickType_t now = xTaskGetTickCount();
    uint8_t mask = 0;

    for (int i = 0; i < 3; ++i) {
        // 按键是低有效，且扩展器读失败时返回高（未按下），不会误报按下。
        // The keys are active low, and a failed expander read reports high, so a failure never
        // looks like a press.
        const bool down = !ioe_key_level(kBits[i]);
        if (down) mask |= (uint8_t)(1u << i);
        if (down && !s_key_down[i]) s_key_down_tick[i] = now;
        if (i == 0 && down && !s_key_down[i]) {
            // 只上报短按，不在这里发关机脉冲：本板要关的是整机，而关机必须停掉面板升压
            // （epd_poweroff）再发 P13，否则电源芯片不肯锁存关断。那条流程在应用里
            // （run_power_action），所以这里只把事件交上去。
            // 在按下沿就锁存，是因为扫描每 APP_LOCK_POLL_MS 才一次，快按可能整个落在两次
            // 扫描之间。
            //
            // Report the short press only; the pulse is not sent here. Powering this board off
            // requires stopping the panel boost (epd_poweroff) before pulsing P13, or the supply
            // will not latch off. That sequence lives in the app (run_power_action), so this just
            // hands the event up. It is latched on the press edge because the scan runs only once
            // per APP_LOCK_POLL_MS and a quick tap could fall between two scans.
            s_key_short = true;
            ESP_LOGI(TAG, "power key pressed: reporting short press");
        }
        if (!down && s_key_down[i]) {
            const uint32_t held_ms = (uint32_t)((now - s_key_down_tick[i]) * portTICK_PERIOD_MS);
            if (i == 0) {
                // 短按已经在按下沿上报过了，松开这里不再补发。
                // The short press was already reported on the press edge; nothing to add here.
                ESP_LOGD(TAG, "power key released after %ums", (unsigned)held_ms);
            }
        }
        s_key_down[i] = down;
    }

    // 端口原始值一变就打一行：这样即使关机脉冲没生效，串口日志也能证明按键到底有没有被读到，
    // 从而把"扫描没跑"和"扫描跑了但脉冲没关掉电源"分开。
    // Log whenever the raw port value changes: even if the pulse fails, the log then proves
    // whether the key was read at all, separating "the scan never ran" from "the scan ran and the
    // pulse did not cut the power".
    if (mask != s_last_mask) {
        uint8_t p1 = 0xFF;
        if (s_ioe != NULL) (void)fca9555_read_input(s_ioe, 1, &p1);
        ESP_LOGW(TAG, "keys P1=0x%02X mask=0x%02X", (unsigned)p1, (unsigned)mask);
        s_last_mask = mask;
    }

    s_snapshot.key_state = mask;
}

/* ---- 公开接口 / Public interface ---- */

esp_err_t read_pico_pmu_init(i2c_master_bus_handle_t bus_handle,
                             const read_pico_pmu_config_t* config) {
    (void)config;
    if (bus_handle == NULL) return ESP_ERR_INVALID_ARG;
    s_bus = bus_handle;

    i2c_device_config_t gauge_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ27220_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_device_config_t rtc_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8563_ADDR,
        .scl_speed_hz = 100000,
    };

    // 两个器件各自独立：缺一个不影响另一个，ready 只要求电量计在。
    // The two parts are independent: one missing does not break the other, and ready only needs
    // the gauge.
    s_ready = i2c_master_bus_add_device(s_bus, &gauge_cfg, &s_gauge) == ESP_OK;
    if (!s_ready) {
        ESP_LOGW(TAG, "BQ27220 not present");
        s_gauge = NULL;
    }
    if (i2c_master_bus_add_device(s_bus, &rtc_cfg, &s_rtc) != ESP_OK) {
        ESP_LOGW(TAG, "PCF8563 not present");
        s_rtc = NULL;
    }

    // 用板级层建好的句柄：同一条总线上再建一个同地址的句柄会失败，之前就是这里没拿到句柄，
    // 于是关机脉冲发不出去。
    // Use the handle the board layer created. A second handle on the same address does not work,
    // and failing to get one here is exactly why the shutdown pulse never went out.
    s_ioe = fca9555_default();
    if (s_ioe == NULL) ESP_LOGW(TAG, "no TCA9555 handle: keys and power-off unavailable");

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.present = s_ready;
    s_snapshot.identity_ok = s_ready;
    s_snapshot.status_ok = s_ready;
    s_snapshot.time_ok = s_rtc != NULL;
    s_snapshot.config_ok = s_ready;
    s_snapshot.charge_state = 0;
    ESP_LOGI(TAG, "gauge=%s rtc=%s", s_gauge ? "ok" : "none", s_rtc ? "ok" : "none");
    return s_ready ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t read_pico_pmu_deinit(void) {
    if (s_gauge) {
        i2c_master_bus_rm_device(s_gauge);
        s_gauge = NULL;
    }
    if (s_rtc) {
        i2c_master_bus_rm_device(s_rtc);
        s_rtc = NULL;
    }
    if (s_ioe) {
        fca9555_deinit(s_ioe);
        s_ioe = NULL;
    }
    s_layout_ok = -1;
    s_ready = false;
    return ESP_OK;
}

bool read_pico_pmu_ready(void) {
    return s_ready;
}

esp_err_t read_pico_pmu_refresh(void) {
    if (!s_ready && s_rtc == NULL) return ESP_ERR_INVALID_STATE;
    read_battery();
    read_rtc();
    return ESP_OK;
}

esp_err_t read_pico_pmu_poll(void) {
    if (!ioe_ready()) return ESP_ERR_INVALID_STATE;
    read_keys();
    read_battery();
    read_rtc();
    return ESP_OK;
}

const pmu_snapshot_t* read_pico_pmu_get(void) {
    return &s_snapshot;
}

esp_err_t read_pico_pmu_cmd(uint16_t code, const uint8_t* payload, uint8_t plen) {
    (void)payload;
    // 没有 CW32 可以收命令。静默接受，避免 main/ 的二十多处调用走失败分支。
    // There is no CW32 to receive commands. Accept silently so the twenty-odd call sites in main/
    // do not take their failure paths.
    ESP_LOGD(TAG, "cmd 0x%04X (%u bytes) ignored: no host MCU on this board", code, (unsigned)plen);
    return ESP_OK;
}

esp_err_t read_pico_pmu_vcom_get(int* out_mv) {
    if (out_mv == NULL) return ESP_ERR_INVALID_ARG;
    // VCOM 由 TPS65185 持有，写法在板级上电流程里。这里只报标称值：本组件不能反过来依赖
    // read_pico，那会成环。
    // VCOM lives in the TPS65185 and is written by the board power-on sequence. This reports the
    // nominal value only: this component cannot depend on read_pico, which would be a cycle.
    *out_mv = PMU_METALIO_VCOM_MV;
    return ESP_OK;
}

esp_err_t read_pico_pmu_vcom_set(int mv) {
    (void)mv;
    // VCOM 由 TPS65185 持有，改它要走板级；这里不直接写，避免绕过轨状态检查。
    // VCOM lives in the TPS65185 and changing it goes through the board layer, so this does not
    // write directly and bypass the rail checks.
    ESP_LOGW(TAG, "VCOM set ignored: the TPS65185 owns it, use the board layer");
    return ESP_OK;
}

esp_err_t read_pico_pmu_uid_get(uint8_t out[PMU_CHIP_UID_LEN]) {
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    // 本板没有芯片 UID；用全 0 表示"未知"而不是伪造一个。
    // This board has no chip UID; all zeros means unknown rather than inventing one.
    memset(out, 0, PMU_CHIP_UID_LEN);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t read_pico_pmu_event_ack(uint16_t event_id) {
    (void)event_id;
    return ESP_OK;
}

esp_err_t read_pico_pmu_action(uint8_t action, uint16_t delay_ms, uint16_t reason) {
    (void)delay_ms;
    (void)reason;
    if (action == PMU_ACTION_HOST_LOGICAL_OFF) return read_pico_pmu_power_off();
    return ESP_OK;
}

void read_pico_pmu_drain_events(void) {
    // 事件队列是 CW32 的概念，本板没有。
    // The event queue is a CW32 concept and does not exist here.
}

bool read_pico_pmu_take_key_short(void) {
    const bool v = s_key_short;
    s_key_short = false;
    return v;
}

bool read_pico_pmu_take_key_wakeup(void) {
    const bool v = s_key_wakeup;
    s_key_wakeup = false;
    return v;
}

read_pico_pmu_key_action_t read_pico_pmu_take_key_action(void) {
    // 长按优先于短按，与协议里的排队规则一致；本板没有事件队列，用两个标志位代替。
    // Long press wins over short, matching the protocol's queue rule; this board has no event
    // queue, so two flags stand in for it.
    if (s_key_wakeup) {
        s_key_wakeup = false;
        return READ_PICO_PMU_KEY_LONG;
    }
    if (s_key_short) {
        s_key_short = false;
        return READ_PICO_PMU_KEY_SHORT;
    }
    return READ_PICO_PMU_KEY_NONE;
}

esp_err_t read_pico_pmu_report_sleep(void) {
    return ESP_OK;
}

esp_err_t read_pico_pmu_report_ready(void) {
    return ESP_OK;
}

/// 关机任务：反复发 P13 脉冲串，直到电源真的断开。
/// 例程就是死循环重发——发一次只会让整机掉一下电又马上起来（实测：日志里 TPS 重新上电、
/// 应用继续跑），因为脉冲只是打断供电，并没有把电源锁存关掉。
/// / Shutdown task: keep sending the P13 pulse train until the supply actually latches off.
/// The demo loops forever for a reason: a single train only browns the board out and it comes
/// straight back (measured -- the TPS power-on log reappears and the app keeps running), because
/// the pulse interrupts the supply instead of latching it off.
static void power_off_task(void* arg) {
    (void)arg;
    fca9555_handle_t ioe = s_ioe;
    if (ioe == NULL) {
        vTaskDelete(NULL);
        return;
    }

    // Port1 常态：P12 触摸复位与 P14 蓝牙/功放电源保持高，P13 关机脉冲默认低。
    // Port1 idle: P12 touch reset and P14 Bluetooth/amplifier power high, P13 pulse low.
    const uint8_t idle = (uint8_t)((FCA9555_P12 | FCA9555_P14) >> 8);
    const uint8_t pulse = (uint8_t)(idle | (FCA9555_P13 >> 8));

    ESP_LOGW(TAG, "power-off: P13 pulse train, repeating until the supply cuts");
    for (;;) {
        for (int i = 0; i < PWR_PULSE_COUNT; ++i) {
            const esp_err_t wr = fca9555_set_output(ioe, 1, pulse);
            vTaskDelay(pdMS_TO_TICKS(PWR_PULSE_INTERVAL_MS));

            // 读回引脚实际电平：TCA9555 的输入寄存器反映引脚状态，即使该脚是输出。
            // 这样能把"写没生效"和"电平对了但芯片不认"彻底分开。
            // Read the actual pin level back: the TCA9555 input register reflects the pin even
            // when it is configured as an output. That separates "the write did not take effect"
            // from "the level is right and the chip ignores it".
            uint8_t rb = 0xFF;
            const esp_err_t rd = fca9555_read_input(ioe, 1, &rb);
            if (i == 0) {
                ESP_LOGW(TAG, "P13 drive: write=%s read=%s P1=0x%02X P13=%d", esp_err_to_name(wr),
                         esp_err_to_name(rd), (unsigned)rb, (int)((rb >> 3) & 1));
            }

            (void)fca9555_set_output(ioe, 1, idle);
            vTaskDelay(pdMS_TO_TICKS(PWR_PULSE_INTERVAL_MS));
        }
        // 每 15 次之间歇一下，与例程的 PwrOffTask 一致。
        // Pause between trains, matching the demo's PwrOffTask.
        vTaskDelay(pdMS_TO_TICKS(POWER_OFF_RETRY_MS));
    }
}

esp_err_t read_pico_pmu_power_off(void) {
    fca9555_handle_t ioe = s_ioe;
    if (ioe == NULL) return ESP_ERR_INVALID_STATE;

    // 已经在关机的路上就别再起一个任务。
    // Do not start a second task if the shutdown is already in flight.
    if (s_power_off_task != NULL) return ESP_OK;

    if (xTaskCreate(power_off_task, "pwr_off", POWER_OFF_TASK_STACK, NULL, POWER_OFF_TASK_PRIO,
                    &s_power_off_task) != pdPASS) {
        s_power_off_task = NULL;
        ESP_LOGE(TAG, "power-off task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
