/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS 里的用户设置：睡眠档、字体路径、上次唤醒/开机原因、拿起唤醒开关。
 *
 * User settings in NVS: sleep mode, font path, last wake/boot reason,
 * pickup-wake switch.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/// 默认深睡。浅睡：按键回原页；拿起唤醒默认关。软睡：SOFT_SLEEP 拉低 EN，再短按开机。关机：EN=0，长按开机。
/// Default is deep. Light: key returns to the page; pickup-wake defaults off. Soft sleep: SOFT_SLEEP drops EN, then a short press boots. Off: EN=0, long-press to boot.
typedef enum {
    APP_SLEEP_LIGHT = 0,
    APP_SLEEP_DEEP = 1,
    APP_SLEEP_OFF = 2,
} app_sleep_mode_t;

void app_settings_init(void);
app_sleep_mode_t app_settings_sleep_mode(void);
void app_settings_set_sleep_mode(app_sleep_mode_t mode);
const char* app_sleep_mode_name(app_sleep_mode_t mode);
/// 空路径表示固件内建字体；非空为 SD 上的 TTF。/ Empty path is the built-in font; non-empty is a TTF on the SD card.
const char* app_settings_font_path(void);
void app_settings_set_font_path(const char* path);
/// 系统界面字体；空值使用内建思源黑体并从 TF 卡补字。/ UI font; empty uses built-in Source Han Sans with SD fallback.
const char* app_settings_system_font_path(void);
void app_settings_set_system_font_path(const char* path);
uint8_t app_settings_system_font_size(void);
void app_settings_set_system_font_size(uint8_t percent);
/// 系统界面灰阶对比度，100..140%。/ System UI grayscale contrast, 100..140%.
uint8_t app_settings_system_contrast(void);
void app_settings_set_system_contrast(uint8_t percent);
/// 锁屏样式和壁纸路径。/ Lock style and wallpaper image path.
uint8_t app_settings_lock_style(void);
void app_settings_set_lock_style(uint8_t style);
const char* app_settings_wallpaper_path(void);
void app_settings_set_wallpaper_path(const char* path);
/// 上次浅睡唤醒源（app_wake_source_t），掉电也保留。/ Last light-sleep wake source (app_wake_source_t); kept across power loss.
uint8_t app_settings_last_wake(void);
void app_settings_set_last_wake(uint8_t src);
/// 最近一次非 0 的 PMU wake_reason。STATUS 报 0 时用这个回显。/ Last non-zero PMU wake_reason. Used when STATUS reports 0.
uint8_t app_settings_last_boot(void);
void app_settings_set_last_boot(uint8_t reason);
/// 浅睡拿起唤醒。默认关；有加速度计也不会自动开。/ Light-sleep pickup wake. Defaults off; an accelerometer does not turn it on.
bool app_settings_pickup_wake(void);
void app_settings_set_pickup_wake(bool on);
/// 阅读默认字号，36..72，默认 48。/ Default reading size, 36..72, initially 48.
uint8_t app_settings_book_px(void);
/// 无效字号恢复 48。/ Invalid sizes fall back to 48.
void app_settings_set_book_px(uint8_t px);
/// 实验性晃动翻页，默认关闭。/ Experimental shake page turn, off by default.
bool app_settings_book_shake(void);
/// 保存实验性晃动翻页开关。/ Persist the experimental shake page-turn switch.
void app_settings_set_book_shake(bool on);
/// 阅读时每 5、10 或 15 页整屏全刷一次。/ Full-screen reader cleanup every 5, 10 or 15 page turns.
uint8_t app_settings_reader_full_pages(void);
void app_settings_set_reader_full_pages(uint8_t pages);
/// 阅读翻页效果：0 默认，1 水波纹；初始为默认。/ Reader turn effect: 0 default, 1 water ripple; initially default.
uint8_t app_settings_reader_turn_effect(void);
void app_settings_set_reader_turn_effect(uint8_t effect);
/// 阅读行高百分比，110..150。/ Reader line-height percentage: 110..150.
uint8_t app_settings_book_line_spacing(void);
void app_settings_set_book_line_spacing(uint8_t percent);
/// 阅读页左右边距，24..60 像素。/ Reader horizontal margin: 24..60 pixels.
uint8_t app_settings_book_margin(void);
void app_settings_set_book_margin(uint8_t px);
/// 段后距离百分比，0/25/50/75。/ Paragraph-gap percentage: 0/25/50/75.
uint8_t app_settings_book_paragraph_spacing(void);
void app_settings_set_book_paragraph_spacing(uint8_t percent);
/// 书架样式：1 深色书轨、2 亚克力挡板、3 半透明书袋。/ Shelf style: 1 dark rail, 2 acrylic guard, 3 frosted pocket.
uint8_t app_settings_shelf_style(void);
void app_settings_set_shelf_style(uint8_t style);
/// 当前 TF 卡书籍目录和字体目录；默认分别为 /sdcard/books、/sdcard/fonts。
const char* app_settings_books_dir(void);
const char* app_settings_fonts_dir(void);
/// 仅允许已挂载 TF 卡内的有界绝对目录；调用者负责确认目录存在。
bool app_settings_set_books_dir(const char* path);
bool app_settings_set_fonts_dir(const char* path);
