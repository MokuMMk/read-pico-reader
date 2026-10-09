/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书架拼贴锁屏，使用真实封面和当前系统字体；缓存放在 TF 卡。
 * English: Library collage lock with real covers and the active system face; cache on SD.
 * 冻结：只由锁屏/预生成入口调用，字体上下文由调用者管理，不更改阅读书源。
 * Frozen: Call only at lock/preparation boundaries; the caller owns font context, never change the reader source.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// 画到完整画布；低内存、无卡或封面错误时安全退化，临时像素均在返回前释放。
/// Draw a complete frame; degrade safely on OOM, missing media or covers and free all temporary pixels.
bool book_lock_collage_draw(uint8_t *framebuffer);
