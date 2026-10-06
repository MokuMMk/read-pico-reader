/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BLE HID 主机（central），配对市面上的蓝牙翻页器/键盘。同一时刻只连一个外设。
 * 本模块只负责「连上与收键」，翻页语义由调用方决定；键事件进环形缓冲，主循环用
 * ble_pt_pop_key() 取。
 *
 * English: BLE HID host (central) for off-the-shelf Bluetooth page-turners and keyboards.
 * One peripheral at a time. This module only connects and receives; page semantics belong to
 * the caller. Key events land in a ring the main loop drains with ble_pt_pop_key().
 *
 * 冻结：本模块不做刷屏、不碰设置存储以外的 NVS、不决定翻页行为。
 * Frozen: no drawing, no NVS beyond its own settings namespace, no page-turn policy.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 扫描结果上限；满了就不再收新的，覆盖会打乱列表。/ Scan capacity; later results are dropped.
#define BLE_PT_MAX_DEVICES 24
// 已配对设备上限。/ Maximum bonded peers.
#define BLE_PT_MAX_BONDS 4
// 一个外设最多学多少个按键。/ Learned buttons per peripheral.
#define BLE_PT_MAX_BINDINGS 4
// 扫描时长：一直扫到被停掉为止。三级扫描页用它。
// Scan duration: keep going until stopped. The scan page uses this.
#define BLE_PT_SCAN_FOREVER 0x7FFFFFFFu

/// HID 特殊键。可打印字符走 `ch`。/ HID special keys; printable keys arrive as `ch`.
typedef enum {
    BLE_PT_KEY_NONE = 0,
    BLE_PT_KEY_ENTER,
    BLE_PT_KEY_BACKSPACE,
    BLE_PT_KEY_TAB,
    BLE_PT_KEY_ESCAPE,
    BLE_PT_KEY_DELETE,
    BLE_PT_KEY_LEFT,
    BLE_PT_KEY_RIGHT,
    BLE_PT_KEY_UP,
    BLE_PT_KEY_DOWN,
    BLE_PT_KEY_HOME,
    BLE_PT_KEY_END,
    BLE_PT_KEY_PAGE_UP,
    BLE_PT_KEY_PAGE_DOWN,
} ble_pt_key_t;

/// 一次按键（含按住自动重复）。/ One key press, including held-key auto-repeat.
typedef struct {
    char ch;              ///< 可打印 ASCII，特殊键为 0 / printable ASCII, 0 for a special key
    uint8_t usage;        ///< 原始 HID usage id / raw HID usage id
    uint8_t mods;         ///< HID 修饰键位掩码 / HID modifier bitmask
    ble_pt_key_t special; ///< 特殊键，否则 BLE_PT_KEY_NONE / special key, else BLE_PT_KEY_NONE
} ble_pt_event_t;

/// 扫描到的设备。/ A device seen while scanning.
typedef struct {
    char addr[18];      ///< "AA:BB:CC:DD:EE:FF"
    char name[32];      ///< 没有广播名时回落为地址 / falls back to the address
    int8_t rssi;
    uint8_t addr_type;  ///< 重连需要 / needed to reconnect
    bool has_name;      ///< 广播名确实收到了 / the advertised name was actually received
    bool hid;           ///< 广播了 HID 服务 0x1812 / advertises the HID service 0x1812
    bool bonded;        ///< 已在已配对列表里 / already in the bond list
} ble_pt_device_t;

/// 已配对（bonded）的设备，持久化在 NVS。/ A bonded peer, persisted in NVS.
typedef struct {
    char addr[18];
    char name[32];
    uint8_t addr_type;
} ble_pt_bond_t;

/// 一次原始按键边沿。身份是「报告相对静止帧第一次出现差异」的位置：报告 id、字节下标、
/// 字节值。用来区分键解码会合并或丢弃的按键——解码器没读的字节上的按钮、两个共用同一
/// 码值的报告。手动映射就绑在这个身份上。
/// One raw button edge. Its identity is where the report first differs from its rest frame:
/// the report id, the payload byte index and the byte value. That keeps apart what the key
/// decode folds together or drops - a button on a byte the decoder never reads, two reports
/// sharing one code. Manual bindings attach to this identity.
typedef struct {
    uint8_t report_id;
    uint8_t byte_index;
    uint8_t value;
    bool pressed;
    /// 仅释放事件：按下那次读到时还不知道静止帧，后来学到的静止帧恰好就是这一字节，
    /// 说明那次「按下」只是外设停在非零状态字节上。学习界面应当丢掉这种。
    /// Release only: the press was read before the rest frame was known and the rest learned
    /// since holds this very byte, so the "press" was the remote idling on a non-zero status
    /// byte. A learning screen drops it.
    bool was_rest;
    uint32_t at_ms;  ///< 帧到达时刻，用于长按计时 / frame arrival time, for hold timing
} ble_pt_raw_t;

/// 把原始边沿折算成一个稳定的键值，供手动映射当索引用。
/// Fold a raw edge into a stable code, for use as a manual-binding key.
uint32_t ble_pt_raw_code(const ble_pt_raw_t *raw);

/// 手动映射的动作。只做翻页，所以只有两个。
/// Manual-binding actions. Page turning only, hence just two.
typedef enum {
    BLE_PT_ACTION_NONE = 0,
    BLE_PT_ACTION_PREV,
    BLE_PT_ACTION_NEXT,
} ble_pt_action_t;

/* ---- 生命周期 / Lifecycle ---- */

/// 起 BLE 栈、载入 bond 表。可重复调用。内存不够时返回 ESP_ERR_NO_MEM 且不做任何初始化。
/// Bring the stack up and load bonds. Idempotent. Returns ESP_ERR_NO_MEM without initialising
/// anything when there is not enough memory.
esp_err_t ble_pt_start(void);

/// 让蓝牙栈跟随设置：需要时启动，不需要时拆掉，启动失败后按退避重试。
/// 主循环只调这一个函数——不要自己写「设置与运行状态不一致就 start()」，启动失败会让那句
/// 话每轮都成立，变成每秒几十次的重复初始化，把内存耗光并导致反复重启。
/// Follow the setting: start when wanted, tear down when not, and back off after a failed start.
/// The main loop calls only this - never roll your own "setting differs from state, so start()",
/// because a failed start keeps that condition true and re-initialises dozens of times a second.
void ble_pt_sync(bool wanted);

/// 启动失败时的原因（取走即清空），供给界面显示。
/// Reason for a failed start, cleared once taken, for the UI to show.
bool ble_pt_take_start_failure(char *out, size_t cap);

/// 用户明确把蓝牙关掉时调用：清掉失败计数与原因，下次开启重新给机会。
/// 与 sync(false) 不同——sync(false) 也会被「WiFi 抢占射频」触发，那种情况下不能清，
/// 否则崩溃自锁会被 WiFi 的开关悄悄解开。
/// Call this when the user explicitly switches Bluetooth off: it clears the failure count and
/// reason so the next enable gets a fresh chance. Unlike sync(false), which WiFi taking over the
/// radio also triggers and where clearing would let a WiFi toggle quietly disarm the self-lock.
void ble_pt_reset_failure(void);

/// 完全拆栈，把 NimBLE 占的内存（host 在 PSRAM）还给堆。用户关掉蓝牙时用这个，
/// 而不是 ble_pt_disconnect()，好让 EPUB 解压之类吃内存的活儿能重新分配。
/// 等最多 timeout_ms（上限 2s）让连接任务退出。
/// Tear the stack down and return NimBLE's memory (the host lives in PSRAM). Use this rather
/// than disconnect() when the user turns Bluetooth off, so memory-hungry work such as EPUB
/// inflate can allocate again. Waits at most timeout_ms (capped at 2s).
esp_err_t ble_pt_stop(uint32_t timeout_ms);
bool ble_pt_running(void);

/// 主循环每次迭代泵一次：驱动自动重连与按键自动重复。很轻，不阻塞。
/// Pump once per main-loop iteration: drives auto-reconnect and key auto-repeat. Cheap.
void ble_pt_poll(void);

/* ---- 扫描 / Discovery ---- */

void ble_pt_scan_start(uint32_t ms);
void ble_pt_scan_stop(void);
bool ble_pt_scanning(void);
uint8_t ble_pt_device_count(void);
const ble_pt_device_t *ble_pt_device(uint8_t index);

/* ---- 连接 / Connection ---- */

/// 异步发起连接；链路加密且订阅到 HID 输入报告后 ble_pt_connected() 才转真。
/// Start an async connect; ble_pt_connected() flips once the link is encrypted and the HID
/// input report is subscribed.
esp_err_t ble_pt_connect(const char *addr);
void ble_pt_disconnect(void);
bool ble_pt_connected(void);
bool ble_pt_connecting(void);
const char *ble_pt_connected_name(void);

/// 取走一次连接失败原因（取走后清空）。/ Take a pending connect-failure reason, clearing it.
bool ble_pt_take_failure(char *out, size_t cap);

/// 取走一次配对码（取走后清空）。非 0 时界面应显示它。
/// Take a pending pairing passkey, clearing it. A non-zero value should be shown.
bool ble_pt_take_passkey(uint32_t *out);

/* ---- 已配对 / Pairings ---- */

uint8_t ble_pt_bond_count(void);
const ble_pt_bond_t *ble_pt_bond(uint8_t index);
void ble_pt_forget(const char *addr);

/* ---- 输入 / Input ---- */

/// 取下一个键事件；队列空返回 false。/ Pop the next key event; false when empty.
bool ble_pt_pop_key(ble_pt_event_t *out);

/// 取下一个原始按键边沿。与 ble_pt_pop_key() 同源但各自一个环，不取就完全没影响。
/// 一次按下只有在它的释放还有位置时才入环；环满时整对丢弃，绝不只丢释放。
/// Pop the next raw button edge. Same reports as ble_pt_pop_key() but a ring of its own, so an
/// app that never calls it sees no change. A press only enters with room left for its release;
/// when the ring is full the whole press is dropped, never a release alone.
bool ble_pt_pop_raw(ble_pt_raw_t *out);

/* ---- 手动映射 / Manual bindings ---- */

/// 把当前连接外设的一个原始键值绑到动作上；code 为 0 表示清除该动作的绑定。
/// 绑定按外设地址分别保存。/ Bind a raw code of the connected peripheral to an action; a
/// code of 0 clears that action. Bindings are stored per peripheral address.
esp_err_t ble_pt_bind(ble_pt_action_t action, uint32_t code);

/// 读回某个动作当前绑定的键值；未绑定时为 0。
/// Read back an action's bound code; 0 when unbound.
uint32_t ble_pt_binding(ble_pt_action_t action);

/// 把原始键值折算成动作。内置映射优先，其次是手动绑定。
/// Resolve a raw code to an action: built-in keys first, then manual bindings.
ble_pt_action_t ble_pt_action_for_raw(uint32_t code);

/// 把 HID usage 折算成动作（内置映射，不查手动绑定）。真机不带码值所以单列。
/// Resolve a HID usage to an action via the built-in map only; separate because a usage has
/// no raw code.
ble_pt_action_t ble_pt_action_for_usage(uint8_t usage, uint8_t mods);
