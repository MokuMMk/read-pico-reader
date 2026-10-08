/* SPDX-License-Identifier: Apache-2.0
 * 中文：主页面灰阶保持未变像素；书架退出加强同向擦白，主页水波纹端点单向推动，中灰保持原序列。
 * English: Main-page grays hold unchanged pixels; shelf exit strengthens directional whitening and main water uses directional endpoints while retaining original midgray sequences.
 */
#pragma once
#include "epdiy.h"
const EpdWaveform* display_main_gray_waveform(void);
/// 离开书架单向变白并补四相，避免GC16反向压黑；其他迁移用原DU，等值保持。/ Shelf exit whitens directionally with four extra phases, avoiding GC16 reverse darkening; other transitions use original DU and equal values hold.
const EpdWaveform* display_main_shelf_exit_waveform(void);
/// 主页水波纹端点单向驱动；中灰变色完整保留原序列，等灰保持。/ Main water drives endpoints directionally, retains full original changed midgray sequences, and holds equal grays.
const EpdWaveform* display_main_water_waveform(void);
/// 快刷首帧保留原DU并保持所有等值像素，亚克力首帧不驱动。/ Fast first pass retains original DU while holding every equal pair, including skipped acrylic.
const EpdWaveform* display_main_bw_waveform(void);
