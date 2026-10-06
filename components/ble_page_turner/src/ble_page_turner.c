/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BLE HID 主机的 IDF NimBLE 实现。扫描、连接、配对（Just Works）与 HID 输入报告
 * 解析。报告解析、原始边沿、自动重复与 bond 持久化的逻辑移植自 CrossMux 的
 * BleKeyboardHost；BLE 胶水层按 ESP-IDF 的 NimBLE 回调模型重写（IDF 把 GATT client
 * 的 API 放在 host/ble_gatt.h，没有单独的 ble_gattc.h）。
 *
 * English: IDF NimBLE implementation of the BLE HID host: scan, connect, Just Works pairing
 * and HID input-report parsing. Report parsing, raw-button edges, auto-repeat and bond
 * persistence are ported from CrossMux's BleKeyboardHost; the BLE plumbing is rewritten for
 * ESP-IDF's NimBLE (which keeps the GATT client API in host/ble_gatt.h, with no ble_gattc.h).
 *
 * 冻结：不做刷屏、不决定翻页语义、不碰本模块 NVS 命名空间以外的存储。
 * Frozen: no drawing, no page-turn policy, no storage outside this module's NVS namespace.
 */

#include "ble_page_turner.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "store/config/ble_store_config.h"

static const char *TAG = "ble_pt";

/* ---- HID 常量 / HID constants ---- */

#define HID_SVC_UUID 0x1812
#define HID_CHR_REPORT_MAP 0x2A4B
#define HID_CHR_REPORT 0x2A4D
#define HID_CHR_PROTOCOL_MODE 0x2A4E
#define HID_CHR_BOOT_KBD_INPUT 0x2A22

// 静态存储期的 UUID：这些调用是异步的，且不拷贝 UUID，只留指针。用 BLE_UUID16_DECLARE
// 的复合字面量会在块结束时失效，procedure 真正执行时就是野指针——表现为调用返回 0、
// 回调永远不来。
// UUIDs with static storage duration. These calls are asynchronous and do NOT copy the UUID,
// only the pointer. A BLE_UUID16_DECLARE compound literal dies at the end of its block, so the
// procedure later runs against a dangling pointer: the call returns 0 and the callback never
// arrives.
#define HID_CHR_CCCD 0x2902

// 启动前的内存下限。内部 RAM 才是瓶颈：控制器与协议栈任务只能放这里（硬件路径读不到
// PSRAM）。阈值取 CrossMux 在同一块板子上验证过的值；本仓库把文件列表、图片列表、壁纸
// 列表、字形缓存和字体目录搬进 PSRAM 之后，DIRAM 空闲约 148KB，这里过得绰绰有余。
// 定得太低会半途失败，失败由退避和自锁兜住。
// Memory floors checked before starting. Internal RAM is the constraint, since the controller and
// the stack task can only live there (the hardware path cannot read PSRAM). The values are the
// ones CrossMux verified on this same board; after this tree moved its file, image, wallpaper,
// glyph-cache and font-catalogue arrays into PSRAM, DIRAM has about 148 KB free, so this passes
// with room to spare. Too low and the start fails halfway, which the back-off and self-lock
// contain.
#define BLE_PT_MIN_FREE_INTERNAL (56 * 1024)
#define BLE_PT_MIN_LARGEST_INTERNAL (20 * 1024)
#define BLE_PT_MIN_FREE_PSRAM (256 * 1024)

// 启动失败后的退避。/ Back-off after a failed start.
#define BLE_PT_RETRY_BACKOFF_MS 30000
// 连续失败这么多次就自锁，避免「开机即崩 → 重启 → 再崩」的死循环。
// Self-lock after this many consecutive failures, so "crash on boot, reboot, crash again" cannot
// loop forever.
#define BLE_PT_MAX_BOOT_TRIES 3
// 栈稳定跑这么久才把尝试计数清零。/ Tries are only cleared once the stack has run this long.
#define BLE_PT_STABLE_MS 10000
// 自动重连间隔。取值同 CrossMux 的 kReconnectBackoffMs。
// Auto-reconnect interval, matching CrossMux's kReconnectBackoffMs.
#define BLE_PT_RECONNECT_MS 4000
#define NVS_NS_BLE_STATE "rp_blestate"

// 报告帧与键环的容量。报告一般 8-9 字节，留 16 足够覆盖带 report id 的复合设备。
// Report frames are typically 8-9 bytes; 16 covers composite devices that prefix a report id.
#define FRAME_MAX 16
#define KEY_RING_LEN 16
// 八次按下连同各自的释放，再加一格用来区分满与空。/ Eight presses with releases, plus one to tell full from empty.
#define RAW_RING_LEN 17
#define MAX_INPUT_CHRS 4

/* ---- 环形缓冲 / Rings ---- */

typedef struct {
    ble_pt_event_t items[KEY_RING_LEN];
    volatile uint8_t head, tail;
} key_ring_t;

typedef struct {
    ble_pt_raw_t items[RAW_RING_LEN];
    volatile uint8_t head, tail;
} raw_ring_t;

static key_ring_t s_keys;
static raw_ring_t s_raws;
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- 模块状态 / Module state ---- */

static ble_pt_device_t s_devices[BLE_PT_MAX_DEVICES];
static uint8_t s_device_count;
static ble_pt_bond_t s_bonds[BLE_PT_MAX_BONDS];
static uint8_t s_bond_count;
static char s_conn_addr[18];
static char s_conn_name[32];
static char s_failure[128];
// 启动策略状态：退避截止、稳定计时起点、上一次同步是否已发生过、启动失败原因。
// Start-policy state: back-off deadline, stability timer, whether a sync has happened yet, and
// the start-failure text.
static uint32_t s_retry_after_ms;
static uint32_t s_stable_since;
static char s_start_failure[160];
// 自动重连：链路曾经建立过才记这里；用户主动断开时清空，免得"关掉"之后又被自己连回来。
// Auto-reconnect: recorded once a link has been up. Cleared on an explicit disconnect, so
// switching it off is not immediately undone by the retry.
static char s_reconnect_addr[18];
static uint32_t s_reconnect_at;
// 待连目标：已有链路在建立或已建立时点了另一台，就记在这里，等断开事件落地再由 poll() 发起。
// Pending target: picking another peer while a link exists or is coming up records it here, and
// poll() starts it once the disconnect has landed.
static char s_pending_addr[18];
static volatile bool s_connected, s_connecting, s_scanning, s_running;
// 链路是否已经建立（GAP 层）。与 s_connected 分开：后者还要求服务发现做完。重连必须以
// 这个为准——否则发现没走完时会把一条好链路反复拆掉重建，表现就是"连上一会儿又掉"。
// Whether the GAP link exists. Separate from s_connected, which also requires discovery to have
// finished. Reconnect must key off this: keyed off s_connected it tears down a perfectly good
// link whenever discovery has not completed, which reads as a connection that keeps dropping.
static volatile bool s_link_up;
static uint32_t s_connect_ms;
static volatile uint32_t s_passkey;
static volatile bool s_passkey_ready;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;


// 定义在扫描那一节，但 note_peer() 也要用。/ Defined in the discovery section, also used by note_peer().
static const ble_pt_device_t *device_find(const char *addr);

// 已发现的可订阅输入报告。/ Subscribable input reports found on the peer.
static uint16_t s_input_chr_vals[MAX_INPUT_CHRS];
static uint16_t s_input_chr_cccds[MAX_INPUT_CHRS];
static uint8_t s_input_chr_count;
static uint8_t s_subscribed_count;
// HID 服务的结束句柄。ble_gatt_chr 只有 def_handle/val_handle，没有 end_handle，
// 所以发现描述符时用服务自己的范围。
// The HID service's end handle. ble_gatt_chr carries def_handle/val_handle but no end_handle,
// so descriptor discovery uses the service's own range.
static uint16_t s_hid_svc_end = 0xFFFF;
// 发现是否已经发起过。/ Whether discovery has been kicked off for this link.
static bool s_discovery_started;
static bool s_protocol_mode_written;

// 报告映射给出的提示：哪个字节更可能是键码。/ Report-map hint for the byte that holds the code.
static bool s_has_keyboard_page, s_has_consumer_page;
static uint8_t s_preferred_byte = 0xFF;

// 静止帧：外设什么都没按时发的报告。第一帧如果就是全松开的，用它做基准；之后每一帧
// 都与它比较来判定边沿。边沿必须同时看上一帧，否则「等于静止值」会在每次报告里重复触发。
// The rest frame: the report the remote sends with nothing pressed. The first all-released frame
// becomes the baseline and later frames are compared against it. Edge detection also needs the
// previous frame, or "equals rest" would fire a release on every report.
static uint8_t s_rest[FRAME_MAX];
static uint8_t s_last[FRAME_MAX];
static size_t s_frame_len;
static bool s_rest_known;

// 自动重复：HID 只在状态变化时发报告，按住不放只会来一次，所以自己合成重复。
// Auto-repeat: HID only reports on state change, so a held key arrives once and repeats are
// synthesized here.
static volatile uint8_t s_held_usage, s_held_mods;
static volatile uint32_t s_held_since, s_last_repeat;

/* ---- 手动映射 / Manual bindings ---- */

#define NVS_NS_BINDINGS "rp_blebind"
#define NVS_NS_BONDS "rp_blebond"

typedef struct {
    char addr[18];
    uint32_t codes[BLE_PT_MAX_BINDINGS];  // 槽位 0 = PREV，1 = NEXT / slot 0 = PREV, 1 = NEXT
} binding_set_t;

static binding_set_t s_bindings;  // 当前连接外设的绑定 / bindings of the connected peer

/* ---- NVS ---- */

// 地址不能直接当 NVS 键名（含冒号且可能超长），所以用 FNV-1a 折算。
// An address cannot be an NVS key (colons, length), so it is folded with FNV-1a.
static uint32_t addr_hash(const char *addr) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)addr; p && *p; ++p) h = (h ^ *p) * 16777619u;
    return h;
}

static const char *bond_key(char *buf, size_t cap, const char *addr) {
    snprintf(buf, cap, "b_%08x", (unsigned)addr_hash(addr));
    return buf;
}

static void bonds_load(void) {
    s_bond_count = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BONDS, NVS_READONLY, &h) != ESP_OK) return;
    for (uint8_t i = 0; i < BLE_PT_MAX_BONDS; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "d_%u", (unsigned)i);
        ble_pt_bond_t bond = {0};
        size_t len = sizeof(bond);
        if (nvs_get_blob(h, key, &bond, &len) == ESP_OK && bond.addr[0]) s_bonds[s_bond_count++] = bond;
    }
    nvs_close(h);
}

static void bonds_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BONDS, NVS_READWRITE, &h) != ESP_OK) return;
    for (uint8_t i = 0; i < BLE_PT_MAX_BONDS; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "d_%u", (unsigned)i);
        if (i < s_bond_count) nvs_set_blob(h, key, &s_bonds[i], sizeof(s_bonds[i]));
        else nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

static ble_pt_bond_t *bond_find(const char *addr) {
    for (uint8_t i = 0; i < s_bond_count; ++i)
        if (!strcmp(s_bonds[i].addr, addr)) return &s_bonds[i];
    return NULL;
}

static void bond_add(const char *addr, const char *name, uint8_t type) {
    if (!addr || !addr[0]) return;
    ble_pt_bond_t *existing = bond_find(addr);
    if (existing) {
        if (name && name[0]) snprintf(existing->name, sizeof(existing->name), "%s", name);
        bonds_save();
        return;
    }
    if (s_bond_count >= BLE_PT_MAX_BONDS) {
        // 满了就顶掉最老的一个，否则新外设永远配不上。
        // Evict the oldest, or a new peripheral could never be paired.
        memmove(&s_bonds[0], &s_bonds[1], sizeof(s_bonds[0]) * (BLE_PT_MAX_BONDS - 1));
        s_bond_count = BLE_PT_MAX_BONDS - 1;
    }
    ble_pt_bond_t *slot = &s_bonds[s_bond_count++];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->addr, sizeof(slot->addr), "%s", addr);
    if (name) snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->addr_type = type;
    bonds_save();
}

static void bindings_load(const char *addr) {
    memset(&s_bindings, 0, sizeof(s_bindings));
    if (!addr || !addr[0]) return;
    snprintf(s_bindings.addr, sizeof(s_bindings.addr), "%s", addr);
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BINDINGS, NVS_READONLY, &h) != ESP_OK) return;
    char key[16];
    size_t len = sizeof(s_bindings.codes);
    nvs_get_blob(h, bond_key(key, sizeof(key), addr), s_bindings.codes, &len);
    nvs_close(h);
}

/* ---- 环形缓冲操作 / Ring operations ---- */

static void key_push(const ble_pt_event_t *ev) {
    portENTER_CRITICAL(&s_ring_lock);
    const uint8_t next = (uint8_t)((s_keys.head + 1) % KEY_RING_LEN);
    if (next != s_keys.tail) {
        s_keys.items[s_keys.head] = *ev;
        s_keys.head = next;
    }
    portEXIT_CRITICAL(&s_ring_lock);
}

// 一次按下只有在它的释放还有位置时才入环，环满就整对丢弃，绝不只丢释放。
// A press only enters with room left for its release; a full ring drops the whole pair, never a
// release alone.
static void raw_push(bool pressed, uint8_t report_id, uint8_t byte_index, uint8_t value,
                     bool was_rest) {
    portENTER_CRITICAL(&s_ring_lock);
    const uint8_t used = (uint8_t)((s_raws.head - s_raws.tail + RAW_RING_LEN) % RAW_RING_LEN);
    const uint8_t keep = pressed ? 1 : 0;  // 按下要给自己的释放留位 / a press reserves room for its release
    if ((uint8_t)(RAW_RING_LEN - 1 - used) > keep) {
        ble_pt_raw_t *slot = &s_raws.items[s_raws.head];
        slot->report_id = report_id;
        slot->byte_index = byte_index;
        slot->value = value;
        slot->pressed = pressed;
        slot->was_rest = was_rest;
        slot->at_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_raws.head = (uint8_t)((s_raws.head + 1) % RAW_RING_LEN);
    }
    portEXIT_CRITICAL(&s_ring_lock);
}

/* ---- HID 报告解析（移植自 CrossMux）/ Report parsing (ported) ---- */

// 报告页提示：报告映射里 0x05 0x07 是键盘页，0x05 0x0C 是消费者页。键盘页的键码在
// 第 2 字节（修饰键之后），消费者页的用法码在第 1 字节。
// Report-map hints: 0x05 0x07 marks the keyboard page, 0x05 0x0C the consumer page. On the
// keyboard page the code sits in byte 2 (after the modifier byte), on the consumer page in byte 1.
static void report_map_hints(const uint8_t *map, size_t len) {
    if (!map || len < 2) return;
    for (size_t i = 0; i + 1 < len; ++i) {
        if (map[i] != 0x05) continue;  // Usage Page
        if (map[i + 1] == 0x07) s_has_keyboard_page = true;
        else if (map[i + 1] == 0x0C) s_has_consumer_page = true;
    }
    if (s_has_keyboard_page) s_preferred_byte = 2;
    else if (s_has_consumer_page) s_preferred_byte = 1;
}

// 取报告里的主码值：优先用报告映射指出的字节，否则扫前几字节取第一个非零。
// Extract the primary code: prefer the byte the report map pointed at, else the first non-zero
// byte within the first few.
static uint8_t extract_primary_code(const uint8_t *p, size_t n) {
    if (s_preferred_byte != 0xFF && s_preferred_byte < n && p[s_preferred_byte] != 0)
        return p[s_preferred_byte];
    const size_t limit = n < 8 ? n : 8;
    for (size_t i = 0; i < limit; ++i)
        if (p[i] != 0) return p[i];
    return 0;
}

// HID usage → 特殊键（US QWERTY）。只保留翻页用得到的部分。
// HID usage to special key (US QWERTY), trimmed to what page turning needs.
static ble_pt_key_t usage_to_special(uint8_t usage) {
    switch (usage) {
        case 0x28: case 0x58: return BLE_PT_KEY_ENTER;
        case 0x29: return BLE_PT_KEY_ESCAPE;
        case 0x2A: return BLE_PT_KEY_BACKSPACE;
        case 0x2B: return BLE_PT_KEY_TAB;
        case 0x4C: return BLE_PT_KEY_DELETE;
        case 0x4A: return BLE_PT_KEY_HOME;
        case 0x4B: return BLE_PT_KEY_PAGE_UP;
        case 0x4D: return BLE_PT_KEY_END;
        case 0x4E: return BLE_PT_KEY_PAGE_DOWN;
        case 0x4F: return BLE_PT_KEY_RIGHT;
        case 0x50: return BLE_PT_KEY_LEFT;
        case 0x51: return BLE_PT_KEY_DOWN;
        case 0x52: return BLE_PT_KEY_UP;
        default: return BLE_PT_KEY_NONE;
    }
}

static void emit_usage(uint8_t usage, uint8_t mods) {
    if (!usage) return;
    const ble_pt_event_t ev = {.usage = usage, .mods = mods, .special = usage_to_special(usage)};
    s_held_usage = usage;
    s_held_mods = mods;
    s_held_since = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_repeat = 0;
    key_push(&ev);
}

// 一帧报告 → 键事件 + 原始边沿。/ One report frame to key events plus raw edges.
static void ingest_report(const uint8_t *data, size_t len) {
    if (!data || !len || len > FRAME_MAX) return;

    const uint8_t code = extract_primary_code(data, len);
    const uint8_t mods = (s_has_keyboard_page && len > 0) ? data[0] : 0;

    // 静止帧取第一帧「什么都没按」的报告。/ The rest frame is the first report with nothing pressed.
    if (!s_rest_known) {
        if (!code) {
            memcpy(s_rest, data, len);
            memcpy(s_last, data, len);
            s_frame_len = len;
            s_rest_known = true;
        }
        if (code) emit_usage(code, mods);
        return;
    }
    if (len != s_frame_len) return;  // 长度变了，之前学的基准不再适用 / frame shape changed

    // 报告 id 是 9 字节帧的首字节，其余按 0。/ The report id is the first byte of a 9-byte frame, else 0.
    const uint8_t report_id = (len == 9) ? data[0] : 0;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t rest = s_rest[i];
        const bool now_pressed = data[i] != rest;
        const bool was_pressed = s_last[i] != rest;
        if (now_pressed && !was_pressed) raw_push(true, report_id, (uint8_t)i, data[i], false);
        else if (!now_pressed && was_pressed) raw_push(false, report_id, (uint8_t)i, rest, rest != 0);
    }
    memcpy(s_last, data, len);

    if (code) emit_usage(code, mods);
    else s_held_usage = 0;  // 全零报告 = 所有键都松开了 / an all-released report
}

/* ---- 扫描 / Discovery ---- */

static int scan_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    const struct ble_gap_disc_desc *d = &event->disc;

    char addr[18];
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x", d->addr.val[5], d->addr.val[4],
             d->addr.val[3], d->addr.val[2], d->addr.val[1], d->addr.val[0]);

    // 同一地址只收一次，否则重复广播会把列表刷满。
    // Keep one entry per address, or repeated adverts flood the list.
    for (uint8_t i = 0; i < s_device_count; ++i)
        if (!strcasecmp(s_devices[i].addr, addr)) return 0;
    if (s_device_count >= BLE_PT_MAX_DEVICES) return 0;

    ble_pt_device_t *slot = &s_devices[s_device_count];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->addr, sizeof(slot->addr), "%s", addr);
    slot->rssi = d->rssi;
    slot->addr_type = d->addr.type;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) == 0) {
        if (fields.name && fields.name_len) {
            const size_t copy = fields.name_len < sizeof(slot->name) - 1 ? fields.name_len
                                                                        : sizeof(slot->name) - 1;
            memcpy(slot->name, fields.name, copy);
            slot->has_name = true;
        }
        for (int i = 0; i < fields.num_uuids16; ++i)
            if (ble_uuid_u16(&fields.uuids16[i].u) == HID_SVC_UUID) slot->hid = true;
    }
    if (!slot->name[0]) snprintf(slot->name, sizeof(slot->name), "%s", addr);
    slot->bonded = bond_find(slot->addr) != NULL;
    s_device_count++;
    return 0;
}

void ble_pt_scan_start(uint32_t ms) {
    if (!s_running || s_scanning || s_connected || s_connecting) return;
    s_device_count = 0;  // 重扫时清空，免得新旧混在一起 / clear on rescan so old and new do not mix
    const struct ble_gap_disc_params params = {
        .itvl = 0, .window = 0, .filter_duplicates = 0, .passive = 0,
    };
    if (ble_gap_disc(s_own_addr_type, (int32_t)ms, &params, scan_cb, NULL) == 0) s_scanning = true;
}

void ble_pt_scan_stop(void) {
    if (!s_scanning) return;
    ble_gap_disc_cancel();
    s_scanning = false;
}

bool ble_pt_scanning(void) { return s_scanning; }
uint8_t ble_pt_device_count(void) { return s_device_count; }

const ble_pt_device_t *ble_pt_device(uint8_t index) {
    return index < s_device_count ? &s_devices[index] : NULL;
}

// 按地址找扫描结果。地址比较不区分大小写：扫描回调写的是小写，已配对列表里的可能来自
// 别处。/ Look up a scan result by address, case-insensitively: the scan callback writes lower
// case while the bond list may hold another spelling.
static const ble_pt_device_t *device_find(const char *addr) {
    if (!addr) return NULL;
    for (uint8_t i = 0; i < s_device_count; ++i)
        if (!strcasecmp(s_devices[i].addr, addr)) return &s_devices[i];
    return NULL;
}

/* ---- 连接与发现 / Connection and discovery ---- */

static void note_peer(const char *addr, const char *name) {
    if (addr && addr[0]) snprintf(s_conn_addr, sizeof(s_conn_addr), "%s", addr);
    if (name && name[0]) {
        snprintf(s_conn_name, sizeof(s_conn_name), "%s", name);
    } else if (addr) {
        // 名字按可信度依次回退：扫描到的广播名 → 已配对时存下的名字 → 地址。
        // 扫描结果里本来就有广播名，之前没接上，所以连上以后只显示 MAC。
        // Fall back by confidence: the name seen while scanning, then the one stored when it was
        // bonded, then the address. The scan result already carries the advertised name; not
        // wiring it through is why a connected peer showed only its MAC.
        const ble_pt_device_t *dev = device_find(addr);
        const ble_pt_bond_t *bond = bond_find(addr);
        if (dev && dev->has_name) snprintf(s_conn_name, sizeof(s_conn_name), "%s", dev->name);
        else if (bond && bond->name[0]) snprintf(s_conn_name, sizeof(s_conn_name), "%s", bond->name);
        else snprintf(s_conn_name, sizeof(s_conn_name), "%s", addr);
    }
    bindings_load(s_conn_addr);
}

static void peer_addr_from_desc(const struct ble_gap_conn_desc *desc, char *out, size_t cap) {
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", desc->peer_id_addr.val[5],
             desc->peer_id_addr.val[4], desc->peer_id_addr.val[3], desc->peer_id_addr.val[2],
             desc->peer_id_addr.val[1], desc->peer_id_addr.val[0]);
}

// 订阅一个 HID 输入报告：在 CCCD 上写 0x0001（通知）。第一个订阅成功就算链路可用。
// Subscribe to one HID input report by writing 0x0001 (notify) to its CCCD. The link counts as
// usable once the first one succeeds.
static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle; (void)attr; (void)arg;
    if (error && error->status != 0) {
        ESP_LOGW(TAG, "CCCD write failed: %d", error->status);
        return 0;
    }
    s_subscribed_count++;
    if (!s_connected) {
        s_connected = true;
        s_connecting = false;
        ESP_LOGI(TAG, "connected to %s", s_conn_name);
    }
    return 0;
}

// 串行化：绝不从一个 ATT procedure 的回调里启动另一个 procedure。
// NimBLE 同一时刻只允许一个 procedure，嵌着发起的那个要么被丢弃，要么把正在跑的枚举
// 搅乱。上一版 chr_cb 里嵌 disc_all_dscs 和协议模式写、dsc_cb 里嵌 CCCD 写，都是这个毛病。
// Serialised: no ATT procedure is ever started from another's callback. NimBLE allows one at a
// time, so a nested call is dropped or derails the enumeration in flight. The previous version
// nested descriptor discovery and a protocol-mode write inside chr_cb, and a CCCD write inside
// dsc_cb.
static uint16_t s_pmode_handle;
static uint8_t s_dsc_index;

// 找完所有输入报告的描述符之后才订阅。/ Subscribe only once every descriptor pass is done.
static void subscribe_ready(void) {
    for (uint8_t i = 0; i < s_input_chr_count; ++i) {
        if (!s_input_chr_cccds[i]) continue;
        const uint8_t value[2] = {0x01, 0x00};  // 通知 / notify
        const int rc = ble_gattc_write_flat(s_conn_handle, s_input_chr_cccds[i], value,
                                            sizeof(value), cccd_write_cb, NULL);
    }
    if (!s_input_chr_count) {
        // 没有可订阅的输入报告：链路是好的，只是没有键。
        // No subscribable input report: the link is fine, there are just no keys.
        if (!s_connected) {
            s_connected = true;
            s_connecting = false;
        }
    }
}

static void dsc_next(void);

static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
    (void)conn_handle; (void)chr_val_handle;
    const uint8_t index = (uint8_t)(uintptr_t)arg;
    switch (error->status) {
        case 0: {
            const ble_uuid16_t cccd = BLE_UUID16_INIT(HID_CHR_CCCD);
            const bool is_cccd = ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0;
            if (is_cccd) s_input_chr_cccds[index] = dsc->handle;  // 只记录，不在这里订阅
            return 0;
        }
        case BLE_HS_EDONE:
            dsc_next();
            return 0;
        default:
            dsc_next();
            return 0;
    }
}

// 一个报告接一个报告地找它的 CCCD。/ Walk the report characteristics one at a time.
static void dsc_next(void) {
    if (s_dsc_index >= s_input_chr_count) {
        subscribe_ready();
        return;
    }
    const uint8_t i = s_dsc_index++;
    const int rc = ble_gattc_disc_all_dscs(s_conn_handle, s_input_chr_vals[i], s_hid_svc_end,
                                           dsc_cb, (void *)(uintptr_t)i);
    if (rc != 0) dsc_next();  // 起不来就跳过这一个，别把整条链卡死
}

static void chr_enum_done(void);

// 协议模式写完再去找描述符。/ Descriptor passes start once the protocol-mode write is done.
static int pmode_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle; (void)attr; (void)arg;
    chr_enum_done();
    return 0;
}

static void chr_enum_done(void) {
    s_dsc_index = 0;
    if (s_pmode_handle) {
        const uint8_t report_protocol = 1;
        const int rc = ble_gattc_write_flat(s_conn_handle, s_pmode_handle, &report_protocol,
                                            sizeof(report_protocol), pmode_cb, NULL);
        if (rc == 0) return;  // 回调里接着走 / continue from the callback
    }
    dsc_next();
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg) {
    (void)conn_handle; (void)arg;
    switch (error->status) {
        case 0: {
            // Protocol Mode(0x2A4E)：只记下手柄，写操作等特征枚举结束后再做。
            // Protocol Mode (0x2A4E): record the handle only; the write waits until characteristic
            // enumeration has finished.
            const ble_uuid16_t pmode = BLE_UUID16_INIT(HID_CHR_PROTOCOL_MODE);
            if (ble_uuid_cmp(&chr->uuid.u, &pmode.u) == 0 &&
                (chr->properties & BLE_GATT_CHR_PROP_WRITE)) {
                s_pmode_handle = chr->val_handle;
                return 0;
            }
            const ble_uuid16_t report = BLE_UUID16_INIT(HID_CHR_REPORT);
            if (ble_uuid_cmp(&chr->uuid.u, &report.u) == 0 &&
                (chr->properties & BLE_GATT_CHR_PROP_NOTIFY) &&
                s_input_chr_count < MAX_INPUT_CHRS) {
                const uint8_t index = s_input_chr_count++;
                s_input_chr_vals[index] = chr->val_handle;
                s_input_chr_cccds[index] = 0;
            }
            return 0;
        }
        case BLE_HS_EDONE:
            chr_enum_done();
            return 0;
        default:
            return 0;
    }
}


// 照 CrossMux 的顺序：连上先做「全量服务发现」，再从结果里挑出 HID 服务。
// NimBLE-Arduino 的 connect() 内部就是先发现全部服务，而按 UUID 定向查服务在这台翻页器上
// 回调根本不来（日志里 disc_svc_by_uuid rc=0 之后一片空白）。这里也不从别的 procedure 的
// 回调里启动新 procedure —— 那是之前另一种失败方式。
// CrossMux's order: discover every service first, then pick the HID one out of the result.
// NimBLE-Arduino's connect() discovers all services up front, whereas a UUID-filtered discovery
// never called back on this peripheral. No procedure is started from another's callback either.
static bool s_hid_found;
static uint16_t s_hid_svc_start = 0xFFFF;

static int svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg) {
    (void)arg;
    switch (error->status) {
        case 0: {
            const ble_uuid16_t hid = BLE_UUID16_INIT(HID_SVC_UUID);
            if (ble_uuid_cmp(&svc->uuid.u, &hid.u) == 0) {
                s_hid_found = true;
                s_hid_svc_start = svc->start_handle;
                s_hid_svc_end = svc->end_handle;
            } else {
            }
            return 0;
        }
        case BLE_HS_EDONE: {
            if (!s_hid_found) {
                // 链路是通的，只是对方不是 HID 设备：照样算连上，免得界面永远转圈。
                // The link is fine, the peer just is not a HID device: mark it connected anyway so
                // the page does not spin forever.
                snprintf(s_failure, sizeof(s_failure), "该设备不提供 HID 服务");
                if (!s_connected) {
                    s_connected = true;
                    s_connecting = false;
                }
                return 0;
            }
            const int rc = ble_gattc_disc_all_chrs(conn_handle, s_hid_svc_start, s_hid_svc_end,
                                                   chr_cb, NULL);
            return rc;
        }
        default:
            return 0;
    }
}

static void start_discovery(void) {
    s_discovery_started = true;
    s_hid_found = false;
    s_hid_svc_start = 0xFFFF;
    const int rc = ble_gattc_disc_all_svcs(s_conn_handle, svc_cb, NULL);
}

static int gap_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC_COMPLETE:
            s_scanning = false;
            return 0;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                s_connecting = false;
                snprintf(s_failure, sizeof(s_failure), "连接失败 (%d)", event->connect.status);
                ESP_LOGW(TAG, "%s", s_failure);
                return 0;
            }
            s_conn_handle = event->connect.conn_handle;
            s_link_up = true;
            s_connecting = false;  // 链路已建立，"连接中"到此为止 / the link exists, so stop saying "connecting"
            struct ble_gap_conn_desc cd;
            if (ble_gap_conn_find(event->connect.conn_handle, &cd) == 0) {
                // 协商后的参数：监督超时太短就会在干扰下频繁掉线，这是"不稳定"的另一种成因。
                // Negotiated parameters: too short a supervision timeout drops the link under
                // interference, which is another way "unstable" happens.
            }
            s_discovery_started = false;
            s_input_chr_count = 0;
            s_subscribed_count = 0;
            const int src_ = ble_gap_security_initiate(s_conn_handle);
            // 这里不做 GATT 发现：加密还在进行，ATT 操作会被拒，而发现一旦发起过就不再重试，
            // 于是整条流程死掉。照 CrossMux 的顺序（connect → 确认服务 → secure → setup），
            // 发现放到 ENC_CHANGE；已配对的链路可能已经加密、不会再发那个事件，poll() 里有兜底。
            // No GATT discovery here: encryption is still in flight, the ATT operations are
            // refused, and discovery never retries once started, so the whole bring-up died here.
            // CrossMux's order is connect, confirm the service, secure, then set up, so discovery
            // runs on ENC_CHANGE; poll() covers the already-encrypted case.
            s_connect_ms = (uint32_t)(esp_timer_get_time() / 1000);
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnected: %d", event->disconnect.reason);
            s_connected = false;
            s_connecting = false;
            s_link_up = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_held_usage = 0;
            s_rest_known = false;
            s_subscribed_count = 0;
            s_input_chr_count = 0;
            s_discovery_started = false;
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE: {
            if (event->enc_change.status != 0) {
                ESP_LOGW(TAG, "encryption failed: %d", event->enc_change.status);
                return 0;
            }
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                char addr[18];
                peer_addr_from_desc(&desc, addr, sizeof(addr));
                note_peer(addr, NULL);
                // 链路真的通了才记下自动重连目标。/ Only arm auto-reconnect on a live link.
                snprintf(s_reconnect_addr, sizeof(s_reconnect_addr), "%s", addr);
                // 配对完成才落到已配对列表里，这样列表只反映真正可自动重连的设备。
                // Only a completed pairing enters the bond list, so it reflects peers that can
                // actually auto-reconnect.
                if (desc.sec_state.bonded) bond_add(addr, s_conn_name, desc.peer_id_addr.type);
            }
            // 加密完成才做 GATT 发现，顺序同 CrossMux：connect → 确认服务 → secure → setup。
            // GATT discovery only after encryption completes, matching CrossMux's order.
            if (!s_discovery_started) start_discovery();
            return 0;
        }

        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            // 外设要显示配对码：存下来给界面显示，并回一个「已确认」。
            // The peer wants a passkey shown: stash it for the UI and acknowledge.
            if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
                const uint32_t passkey = (uint32_t)(esp_timer_get_time() % 1000000u);
                struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = passkey};
                if (ble_sm_inject_io(event->passkey.conn_handle, &io) == 0) {
                    s_passkey = passkey;
                    s_passkey_ready = true;
                }
            }
            return 0;
        }

        case BLE_GAP_EVENT_NOTIFY_RX: {
            const uint8_t *body = OS_MBUF_DATA(event->notify_rx.om, const uint8_t *);
            const uint16_t len = (uint16_t)OS_MBUF_PKTLEN(event->notify_rx.om);
            // 收到报告本身就是"链路可用"最可靠的证据：CCCD 订阅可能因为链路未加密而写失败，
            // 但已配对设备的订阅状态由对端保留，翻页器照样会发。之前拿"订阅写成功"当判据，
            // 于是能翻页却一直显示「连接中」，「学习」也因此说你没连。
            // A report arriving is the most reliable proof the link is usable: the CCCD subscribe
            // can fail while the link is unencrypted, yet a bonded peer keeps the subscription on
            // its side and notifies anyway. Keying "connected" off a successful subscribe is why
            // pages turned while the screen said "connecting", and why 学习 claimed no connection.
            if (!s_connected) {
                s_connected = true;
                s_connecting = false;
            }
            ingest_report(body, len);
            return 0;
        }

        default:
            return 0;
    }
}

esp_err_t ble_pt_connect(const char *addr) {
    if (!s_running || !addr || !addr[0]) return ESP_ERR_INVALID_STATE;
    if (s_link_up || s_connecting) {
        // 想换一台：先记下目标、断掉现有的，等 DISCONNECT 事件到了 poll() 再连。
        // 直接在这里发起会撞上还没落地的旧链路，返回 ESP_ERR_INVALID_STATE —— 表现就是
        // 点了没反应或者连不上。
        // Switching peer: record the target, drop the current link, and let poll() connect once
        // the DISCONNECT event lands. Starting here would race the closing link and fail with
        // ESP_ERR_INVALID_STATE, which shows up as "tapping it does nothing".
        snprintf(s_pending_addr, sizeof(s_pending_addr), "%s", addr);
        ble_pt_disconnect();
        return ESP_OK;
    }

    ble_pt_scan_stop();

    unsigned v[6];
    if (sscanf(addr, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return ESP_ERR_INVALID_ARG;
    ble_addr_t peer = {0};
    for (int i = 0; i < 6; ++i) peer.val[5 - i] = (uint8_t)v[i];
    // 已配对过的用它的地址类型重连，公开地址只是兜底。
    // Reconnect with the address type it was bonded with; public is only a fallback.
    const ble_pt_bond_t *bond = bond_find(addr);
    peer.type = bond ? (uint8_t)bond->addr_type : BLE_ADDR_PUBLIC;

    s_connecting = true;
    s_rest_known = false;
    s_subscribed_count = 0;
    note_peer(addr, NULL);

    const int rc = ble_gap_connect(s_own_addr_type, &peer, 10000, NULL, gap_cb, NULL);
    if (rc != 0) {
        s_connecting = false;
        snprintf(s_failure, sizeof(s_failure), "发起连接失败 (%d)", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void ble_pt_disconnect(void) {
    // 记下是谁在断链路：日志里 reason=534 看着像"本地断开"，但调用点不明会误导排查。
    // Record who terminates: reason=534 looks like a local termination, and not knowing the
    // caller would send the next round of debugging the wrong way.
    s_reconnect_addr[0] = 0;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn_handle, 0x13);
}

// 界面上的"已连接"就是"链路建立了"，与我们的 HID 订阅是否成功无关。以前这里报的是
// "订阅可用"，而订阅可能失败、翻页器却照旧发通知，于是页面永远停在「连接中」。
// "Connected" in the UI means the link exists, regardless of whether our HID subscription
// succeeded. This used to report "subscription usable", and since a subscription can fail while
// the remote keeps notifying, the page sat on "connecting" forever.
bool ble_pt_connected(void) { return s_link_up; }
bool ble_pt_connecting(void) { return s_connecting && !s_link_up; }
const char *ble_pt_connected_name(void) { return s_conn_name; }

bool ble_pt_take_failure(char *out, size_t cap) {
    if (!s_failure[0]) return false;
    if (out && cap) snprintf(out, cap, "%s", s_failure);
    s_failure[0] = 0;
    return true;
}

bool ble_pt_take_passkey(uint32_t *out) {
    if (!s_passkey_ready) return false;
    if (out) *out = s_passkey;
    s_passkey_ready = false;
    return true;
}

/* ---- 生命周期 / Lifecycle ---- */

static void on_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "no usable address");
        return;
    }
    ble_hs_id_infer_auto(0, &s_own_addr_type);
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_pt_start(void) {
    if (s_running) return ESP_OK;

    // 内存闸门：NimBLE 的 host 动态分配走 PSRAM（CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL），
    // 但控制器与协议栈任务必须落在内部 RAM——它跑在硬件路径上，读不到 PSRAM。所以卡住的
    // 是内部 RAM，不是 PSRAM。不够就当场拒绝，绝不进到一半再崩。
    // Memory gate. NimBLE's host allocations go to PSRAM
    // (CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL), but the controller and the stack task must live
    // in internal RAM - they sit on the hardware path and cannot read PSRAM. So the binding
    // constraint is internal RAM, not PSRAM. Refuse outright rather than half-initialise.
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_internal < BLE_PT_MIN_FREE_INTERNAL ||
        largest_internal < BLE_PT_MIN_LARGEST_INTERNAL || free_psram < BLE_PT_MIN_FREE_PSRAM) {
        ESP_LOGW(TAG, "not enough memory: internal %u B free / %u B largest, psram %u B free",
                 (unsigned)free_internal, (unsigned)largest_internal, (unsigned)free_psram);
        snprintf(s_failure, sizeof(s_failure), "内存不足（内部 %uK，最大块 %uK）",
                 (unsigned)(free_internal / 1024), (unsigned)(largest_internal / 1024));
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;  // Just Works / Just Works
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;
    // store 由 IDF 在 nimble_port_init() 内部初始化（ble_store_config_init 是内部符号，
    // 公开头里没有声明），bond 的持久化由 CONFIG_BT_NIMBLE_NVS_PERSIST 决定。
    // The store is initialised by IDF inside nimble_port_init() (ble_store_config_init is an
    // internal symbol with no public declaration); bond persistence is decided by
    // CONFIG_BT_NIMBLE_NVS_PERSIST.

    bonds_load();
    nimble_port_freertos_init(host_task);
    s_running = true;
    ESP_LOGI(TAG, "started, %u bond(s)", (unsigned)s_bond_count);
    return ESP_OK;
}

esp_err_t ble_pt_stop(uint32_t timeout_ms) {
    if (!s_running) return ESP_OK;
    (void)timeout_ms;
    ble_pt_scan_stop();
    ble_pt_disconnect();
    // 让断开走完再拆 host；bond 留在 NVS，下次 start() 重新载入。
    // Let the disconnect settle before tearing the host down; bonds stay in NVS for the next start().
    vTaskDelay(pdMS_TO_TICKS(200));
    nimble_port_stop();
    nimble_port_deinit();
    s_running = false;
    s_connected = s_connecting = s_scanning = s_link_up = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_stable_since = 0;
    return ESP_OK;
}

bool ble_pt_running(void) { return s_running; }

/* ---- 启动策略 / Start policy ---- */

// 连续失败计数落在 NVS：硬崩（panic 重启）时计数已经先写下去了，所以死循环能被自锁打断。
// The consecutive-failure count lives in NVS: it is written before the risky call, so a hard
// crash still leaves the evidence that breaks the loop.
static uint8_t boot_tries_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BLE_STATE, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t tries = 0;
    nvs_get_u8(h, "tries", &tries);
    nvs_close(h);
    return tries;
}

static void boot_tries_store(uint8_t tries) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BLE_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "tries", tries);
    nvs_commit(h);
    nvs_close(h);
}

// 失败原因也要落 NVS：它在 RAM 里的话，自锁后重启就只剩「失败 3 次」而看不到到底为什么，
// 那样这个自锁除了挡住用户什么用都没有。
// The failure reason is persisted too. Kept only in RAM it is gone after a reboot, leaving the
// self-lock showing "failed 3 times" with no cause, which blocks the user and explains nothing.
static void why_store(const char *why) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BLE_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    if (why && why[0]) nvs_set_str(h, "why", why);
    else nvs_erase_key(h, "why");
    nvs_commit(h);
    nvs_close(h);
}

static bool why_load(char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BLE_STATE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    const bool ok = nvs_get_str(h, "why", out, &len) == ESP_OK && out[0];
    nvs_close(h);
    return ok;
}

// wanted 要稳定这么久才照做。/ The requested value must hold this long before acting on it.
#define BLE_PT_WANT_DEBOUNCE_MS 2000

void ble_pt_sync(bool wanted) {
    // 先防抖。主循环每轮都会问一次，而"要不要蓝牙"这个值来自 WiFi 状态；那个状态在
    // 启动/停止的过渡里会抖，照单全收就会把蓝牙栈反复拆了又建——既表现为连接不稳定，
    // 也会在链路还活着的时候把"已连接"标志清掉（通知照旧到达，于是翻页器还能翻页）。
    // Debounce first. The main loop asks every iteration and the answer depends on WiFi state,
    // which flickers while the transfer service starts or stops. Acting on every flicker tore the
    // stack down and rebuilt it repeatedly: that reads as an unstable link, and it cleared the
    // connected flag while a live link was still delivering notifications, so the remote kept
    // turning pages while the screen said it was not connected.
    static bool have_want;
    static bool steady_want;
    static uint32_t want_changed_at;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (!have_want || wanted != steady_want) {
        if (wanted != steady_want) {
            steady_want = wanted;
            want_changed_at = now_ms;
            if (!have_want) {
                have_want = true;
            } else {
                return;  // 等它稳定下来 / wait for it to settle
            }
        }
        have_want = true;
    } else if (now_ms - want_changed_at < BLE_PT_WANT_DEBOUNCE_MS) {
        return;
    }

    if (!wanted) {
        if (s_running) ble_pt_stop(1000);
        s_retry_after_ms = 0;
        // 这里不清崩溃计数：WiFi 抢占射频时也会走到这个分支，清掉就等于把自锁削弱了。
        // 计数只在栈稳定跑满 BLE_PT_STABLE_MS 之后才清零。
        // The failure count is deliberately not cleared here: WiFi taking over the radio also
        // lands in this branch, and clearing would weaken the self-lock. It is only cleared once
        // the stack has run stably for BLE_PT_STABLE_MS.
        return;
    }
    if (s_running) {
        // 稳定跑够时间才认为这次启动是好的。/ Only a stack that stayed up counts as a good start.
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (!s_stable_since) s_stable_since = now;
        else if (now - s_stable_since >= BLE_PT_STABLE_MS && boot_tries_load()) {
            boot_tries_store(0);
        }
        return;
    }

    const uint8_t tries = boot_tries_load();
    char why[120];
    const bool have_why = why_load(why, sizeof(why));
    // 锁着、并且确实记下了原因，才真的停手并把原因显示出来。
    // 如果计数到了上限却没有原因（旧固件留下的锁），这里放行一次诊断尝试去把原因抓出来；
    // 否则用户面对的是一把既不说明原因、又解不开的锁。
    // Stop only when the count is exhausted *and* a cause was recorded. When the count is
    // exhausted with no cause (a lock left behind by an older build), let one diagnostic attempt
    // through to capture it; otherwise the user faces a lock that neither explains itself nor
    // can be released.
    if (tries >= BLE_PT_MAX_BOOT_TRIES && have_why) {
        snprintf(s_start_failure, sizeof(s_start_failure),
                 "启动失败 %u 次：%s（关掉再开启可重试）", (unsigned)tries, why);
        return;
    }

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_retry_after_ms && now < s_retry_after_ms) return;  // 退避中 / backing off

    // 先记账再动手：这一步之后硬崩，重启时计数已经加过了。
    // Record first, then act: a hard crash after this point still leaves the count raised.
    boot_tries_store((uint8_t)(tries + 1));
    const esp_err_t err = ble_pt_start();
    if (err != ESP_OK) {
        s_retry_after_ms = now + BLE_PT_RETRY_BACKOFF_MS;
        // 优先用 start() 写下的详细原因（里面有内存数字），只有它没写时才退回错误名。
        // Prefer the detailed reason start() recorded (it carries the memory figures) and fall
        // back to the bare error name only when there is none.
        if (s_failure[0]) {
            snprintf(s_start_failure, sizeof(s_start_failure), "%s", s_failure);
            s_failure[0] = 0;
        } else {
            snprintf(s_start_failure, sizeof(s_start_failure), "启动失败：%s", esp_err_to_name(err));
        }
        why_store(s_start_failure);
        ESP_LOGW(TAG, "start failed (%s), backing off %u ms", esp_err_to_name(err),
                 (unsigned)BLE_PT_RETRY_BACKOFF_MS);
    } else {
        s_retry_after_ms = 0;
        s_start_failure[0] = 0;
        why_store(NULL);
    }
}

bool ble_pt_take_start_failure(char *out, size_t cap) {
    if (!s_start_failure[0]) return false;
    if (out && cap) snprintf(out, cap, "%s", s_start_failure);
    s_start_failure[0] = 0;
    return true;
}

void ble_pt_reset_failure(void) {
    boot_tries_store(0);
    why_store(NULL);
    s_retry_after_ms = 0;
    s_start_failure[0] = 0;
    s_failure[0] = 0;
}

// 自动重复：按住 500ms 后开始，之后每 120ms 一次。
// Auto-repeat: begins 500 ms after the press, then one repeat every 120 ms.
#define REPEAT_DELAY_MS 500
#define REPEAT_PERIOD_MS 120

void ble_pt_poll(void) {
    if (!s_running) return;

    // 自动重连：翻页器会自己省电断开，或走出范围。链路曾经建立过就记下目标，掉线后按
    // 退避重试。必须排除「正在扫描」——扫描页是持续扫描，两者同时抢控制器就会互相打架，
    // 表现就是连接不稳定（CrossMux 的同类判断里也有 !scanning_）。
    // Auto-reconnect: a remote drops the link to save power, or walks out of range. Once a link
    // has been up the target is remembered and retried with a back-off. Scanning must be excluded:
    // the scan page scans continuously, and letting both drive the controller at once makes them
    // fight, which shows up as an unstable connection (CrossMux guards with !scanning_ too).
    if (!s_link_up && !s_connecting && !s_scanning && s_reconnect_addr[0]) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - s_reconnect_at >= BLE_PT_RECONNECT_MS) {
            s_reconnect_at = now;
            char addr[18];
            snprintf(addr, sizeof(addr), "%s", s_reconnect_addr);
            if (ble_pt_connect(addr) == ESP_OK)
                ESP_LOGI(TAG, "reconnecting to %s", s_conn_name);
        }
    }

    // 切换目标：等现有链路断干净再连，别在 old link 还没落地时发起新的。
    // Switching targets waits for the old link to finish closing rather than racing it.
    // 已配对的链路连上时可能已加密，NimBLE 不再发 ENC_CHANGE；等一小会儿还没开始发现就
    // 直接进 setup，否则页面永远停在「连接中」。
    // A bonded link can already be encrypted and send no ENC_CHANGE; if setup has not begun
    // shortly after connecting, run it anyway.
    if (s_link_up && !s_discovery_started && s_connect_ms &&
        (uint32_t)(esp_timer_get_time() / 1000) - s_connect_ms > 1500) {
        start_discovery();
    }

    if (s_pending_addr[0] && !s_link_up && !s_connecting) {
        char addr[18];
        snprintf(addr, sizeof(addr), "%s", s_pending_addr);
        s_pending_addr[0] = 0;
        if (ble_pt_connect(addr) == ESP_OK) ESP_LOGI(TAG, "connecting to %s", s_conn_name);
    }

    const uint8_t usage = s_held_usage;
    if (!usage) return;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_last_repeat) {
        if (now - s_held_since >= REPEAT_DELAY_MS) s_last_repeat = now;
        return;
    }
    if (now - s_last_repeat >= REPEAT_PERIOD_MS) {
        s_last_repeat = now;
        emit_usage(usage, s_held_mods);
    }
}

/* ---- 输入 / Input ---- */

bool ble_pt_pop_key(ble_pt_event_t *out) {
    if (!out) return false;
    bool got = false;
    portENTER_CRITICAL(&s_ring_lock);
    if (s_keys.tail != s_keys.head) {
        *out = s_keys.items[s_keys.tail];
        s_keys.tail = (uint8_t)((s_keys.tail + 1) % KEY_RING_LEN);
        got = true;
    }
    portEXIT_CRITICAL(&s_ring_lock);
    return got;
}

bool ble_pt_pop_raw(ble_pt_raw_t *out) {
    if (!out) return false;
    bool got = false;
    portENTER_CRITICAL(&s_ring_lock);
    if (s_raws.tail != s_raws.head) {
        *out = s_raws.items[s_raws.tail];
        s_raws.tail = (uint8_t)((s_raws.tail + 1) % RAW_RING_LEN);
        got = true;
    }
    portEXIT_CRITICAL(&s_ring_lock);
    return got;
}

uint32_t ble_pt_raw_code(const ble_pt_raw_t *raw) {
    if (!raw) return 0;
    return (uint32_t)raw->value | ((uint32_t)raw->byte_index << 8) |
           ((uint32_t)raw->report_id << 16);
}

/* ---- 已配对 / Pairings ---- */

uint8_t ble_pt_bond_count(void) { return s_bond_count; }

const ble_pt_bond_t *ble_pt_bond(uint8_t index) {
    return index < s_bond_count ? &s_bonds[index] : NULL;
}

void ble_pt_forget(const char *addr) {
    if (!addr) return;
    for (uint8_t i = 0; i < s_bond_count; ++i) {
        if (strcmp(s_bonds[i].addr, addr)) continue;
        memmove(&s_bonds[i], &s_bonds[i + 1], sizeof(s_bonds[0]) * (s_bond_count - i - 1));
        s_bond_count--;
        bonds_save();
        break;
    }
    nvs_handle_t h;  // 绑定也跟着清掉 / drop the bindings with it
    if (nvs_open(NVS_NS_BINDINGS, NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        nvs_erase_key(h, bond_key(key, sizeof(key), addr));
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- 手动映射 / Manual bindings ---- */

esp_err_t ble_pt_bind(ble_pt_action_t action, uint32_t code) {
    if (action != BLE_PT_ACTION_PREV && action != BLE_PT_ACTION_NEXT) return ESP_ERR_INVALID_ARG;
    if (!s_bindings.addr[0]) return ESP_ERR_INVALID_STATE;  // 没连着外设 / no peer connected
    s_bindings.codes[action == BLE_PT_ACTION_PREV ? 0 : 1] = code;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_BINDINGS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char key[16];
    err = nvs_set_blob(h, bond_key(key, sizeof(key), s_bindings.addr), s_bindings.codes,
                       sizeof(s_bindings.codes));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint32_t ble_pt_binding(ble_pt_action_t action) {
    if (action != BLE_PT_ACTION_PREV && action != BLE_PT_ACTION_NEXT) return 0;
    return s_bindings.codes[action == BLE_PT_ACTION_PREV ? 0 : 1];
}

// 内置映射：上/下、左右、PageUp/PageDown 都算翻页。市面上的翻页器按键五花八门，
// 全认下来比让用户逐个配置省事得多。
// Built-in map: up/down, left/right and PageUp/PageDown all turn pages. Remotes differ wildly
// in which keys they send, so accepting them all beats configuring each one.
ble_pt_action_t ble_pt_action_for_usage(uint8_t usage, uint8_t mods) {
    (void)mods;
    switch (usage) {
        case 0x52:  // Up
        case 0x50:  // Left
        case 0x4B:  // PageUp
            return BLE_PT_ACTION_PREV;
        case 0x51:  // Down
        case 0x4F:  // Right
        case 0x4E:  // PageDown
        case 0x2C:  // Space
        case 0x28:  // Enter
            return BLE_PT_ACTION_NEXT;
        default:
            return BLE_PT_ACTION_NONE;
    }
}

ble_pt_action_t ble_pt_action_for_raw(uint32_t code) {
    if (!code) return BLE_PT_ACTION_NONE;
    if (code == s_bindings.codes[0]) return BLE_PT_ACTION_PREV;
    if (code == s_bindings.codes[1]) return BLE_PT_ACTION_NEXT;
    return BLE_PT_ACTION_NONE;
}
