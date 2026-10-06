/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：板级层建好的 9555 句柄的登记处。
 *
 * 板级层（components/read_pico）持有 9555 句柄，电源层（components/read_pico_pmu）也要用它
 * 发关机脉冲、读按键。但 read_pico 已经 require 了 read_pico_pmu，反过来再 require 会成环。
 * 两边都依赖本组件，所以把句柄放在这里转一手。
 *
 * English: a registry for the 9555 handle the board layer created.
 *
 * The board layer (components/read_pico) owns an fca9555 handle, and the power layer
 * (components/read_pico_pmu) needs it too, to pulse the shutdown line and read the keys. But
 * read_pico already requires read_pico_pmu, so depending back would be a cycle. Both depend on
 * this component, so the handle is parked here.
 *
 * 冻结：只放一个句柄。多实例的板子要么换成数组，要么由板级层自己管。
 * Frozen: one handle only. A board with more expanders needs an array here or its own lookup.
 */

#include "fca9555.h"

static fca9555_handle_t s_default;

/// 由创建句柄的一方登记（板级层初始化后调用一次）。
/// / Registered by whoever created the handle (the board layer calls this once after init).
void fca9555_set_default(fca9555_handle_t handle) {
    s_default = handle;
}

/// 没有登记过就返回 NULL，调用方必须容忍。
/// / Returns NULL when nothing was registered; callers must tolerate that.
fca9555_handle_t fca9555_default(void) {
    return s_default;
}
