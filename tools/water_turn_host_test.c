/*
 * SPDX-License-Identifier: Apache-2.0
 * 中文：验证真实水波纹组件的16/24带、37相、四向映射、裁剪边界和失败后旧帧基准。
 * English: Exercise the real water-turn engine's 16/24 bands, 37 phases, four directions, crop bounds and failure baseline.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "e0470_page_turn.h"
#include "e0470_epaper_waveform.h"

#define WIDTH 1216
#define HEIGHT 684
#define FB_BYTES ((WIDTH * HEIGHT) / 2)
static uint8_t front[FB_BYTES], back[FB_BYTES], diff[WIDTH * HEIGHT];
static bool dirty_lines[HEIGHT];
static uint8_t dirty_columns[WIDTH / 2];
static const EpdWaveformPhases gl = {.phases = 37, .luts = (const uint8_t*)"x"};
static const EpdWaveformPhases soft_gl = {.phases = 37, .luts = (const uint8_t*)"s"};
const EpdWaveform E0470_WAVEFORM = {.id = 1};
const EpdWaveform E0470_APPLY_WAVEFORM = {.id = 2};
static const EpdWaveform soft_wave = {.id = 3};
static enum EpdRotation rotation;
static e0470_turn_dir_t direction;
static bool check_direction = true;
static const uint8_t* const* staged_luts;
static const int8_t* staged_lines;
static const int *staged_x0, *staged_x1;
static const int8_t* staged_bands;
static int staged_count;
static int scans, fail_at, differences, powerons;
static int phase_hits[37];
static int test_bands=16, test_launch_step=1;
static int lut_builds;
static int64_t now_us;
static int scan_time_us=7000;
static uint64_t trace_hash;
int water_test_alloc_fail;

static void hash_byte(uint8_t b) { trace_hash=(trace_hash^b)*UINT64_C(1099511628211); }

int epd_width(void) { return WIDTH; }
int epd_height(void) { return HEIGHT; }
enum EpdRotation epd_get_rotation(void) { return rotation; }
EpdRect epd_full_screen(void) { return (EpdRect){0, 0, WIDTH, HEIGHT}; }
void epd_poweron(void) { ++powerons; }
int64_t esp_timer_get_time(void) { return now_us; }
void esp_rom_delay_us(uint32_t us) { now_us += us; }
const EpdWaveformPhases* e0470_waveform_phases(const EpdWaveform* waveform, int mode) {
    assert(mode == MODE_GL16);
    if(!waveform)return NULL;
    assert(waveform == &E0470_WAVEFORM || waveform == &soft_wave);
    return waveform == &soft_wave ? &soft_gl : &gl;
}
void epd_build_1ppB_lut_1k(uint8_t* lut, const EpdWaveformPhases* phases, int frame) {
    assert((phases == &gl || phases == &soft_gl) && frame >= 0 && frame < 37);
    ++lut_builds;
    memset(lut, frame, 1024);
}
EpdRect epd_difference_image_cropped(const uint8_t* to, const uint8_t* from, EpdRect area,
                                      uint8_t* difference, bool* lines, uint8_t* columns) {
    assert(to == front && from == back && difference == diff && lines == dirty_lines && columns == dirty_columns);
    assert(area.x >= 0 && area.y >= 0 && area.x + area.width <= WIDTH &&
           area.y + area.height <= HEIGHT && area.width > 0 && area.height > 0);
    ++differences;
    return area;
}
void epd_clear_phase_luts(void) {
    staged_luts = NULL;
    staged_lines = NULL;
    staged_x0 = staged_x1 = NULL;
    staged_bands = NULL;
    staged_count = 0;
}
void epd_set_line_phase_luts(const uint8_t* const* luts, const int8_t* lines) {
    epd_clear_phase_luts();
    staged_luts = luts;
    staged_lines = lines;
}
void epd_set_col_phase_luts(const uint8_t* const* luts, const int* x0, const int* x1,
                            const int8_t* phases, int count) {
    epd_clear_phase_luts();
    staged_luts = luts;
    staged_x0 = x0;
    staged_x1 = x1;
    staged_bands = phases;
    staged_count = count;
}
static void physical_to_logical(int px, int py, int* lx, int* ly) {
    switch (rotation) {
        case EPD_ROT_LANDSCAPE: *lx = px; *ly = py; break;
        case EPD_ROT_PORTRAIT: *lx = py; *ly = WIDTH - px - 1; break;
        case EPD_ROT_INVERTED_LANDSCAPE: *lx = WIDTH - px - 1; *ly = HEIGHT - py - 1; break;
        case EPD_ROT_INVERTED_PORTRAIT: *lx = HEIGHT - py - 1; *ly = px; break;
    }
}
enum EpdDrawError epd_draw_base(EpdRect area, const uint8_t* data, EpdRect crop,
                                 enum EpdDrawMode mode, int temperature, const bool* lines,
                                 const uint8_t* columns, const EpdWaveform* waveform) {
    assert(area.width == WIDTH && crop.height == HEIGHT && data == diff);
    assert(mode == (MODE_PACKING_1PPB_DIFFERENCE | MODE_DU) && temperature == 25);
    assert(lines == dirty_lines && columns == dirty_columns && waveform == &E0470_APPLY_WAVEFORM);
    assert(staged_luts && (staged_lines != NULL) != (staged_bands != NULL));
    for(int y=0;y<HEIGHT;++y){hash_byte(lines[y]);if(staged_lines)hash_byte((uint8_t)staged_lines[y]);}
    for(int x=0;x<WIDTH/2;++x)hash_byte(columns[x]);
    if(staged_bands)for(int b=0;b<staged_count;++b)hash_byte((uint8_t)staged_bands[b]);
    int tick = scans++;
    int active = 0;
    bool phase_seen[37] = {0};
    int px = -1, py = -1;
    if (staged_lines) {
        for (int y = 0; y < HEIGHT; ++y) {
            int p = staged_lines[y];
            if (p < 0) continue;
            assert(p < 37 && staged_luts[p][0] == p);
            if (!phase_seen[p]) { phase_seen[p] = true; ++active; }
            if (px < 0) { px = WIDTH / 2; py = y; }
        }
    } else {
        assert(staged_count == test_bands);
        for (int b = 0; b < staged_count; ++b) {
            int p = staged_bands[b];
            if (p < 0) continue;
            assert(p < 37 && staged_luts[p][0] == p);
            assert(staged_x0[b] >= 0 && staged_x1[b] <= WIDTH && staged_x1[b] > staged_x0[b]);
            if (!phase_seen[p]) { phase_seen[p] = true; ++active; }
            if (px < 0) { px = (staged_x0[b] + staged_x1[b]) / 2; py = HEIGHT / 2; }
        }
    }
    int expected = 0;
    for (int b = 0; b < test_bands; ++b)
        if (tick - b*test_launch_step >= 0 && tick - b*test_launch_step < 37) ++expected;
    assert(active == expected);
    for(int p=0;p<37;++p)phase_hits[p]+=phase_seen[p];
    if(tick==(test_bands-1)*test_launch_step) {
        assert(phase_seen[0]);
        if(test_launch_step==1)assert(expected==test_bands && tick<37);
        else assert(tick>=37 && expected<test_bands);
    }
    if (tick == 0 && check_direction) {
        int lx, ly;
        physical_to_logical(px, py, &lx, &ly);
        int coord = direction <= E0470_TURN_RTL ? lx : ly;
        int span = direction <= E0470_TURN_RTL ?
            ((rotation & 1) ? HEIGHT : WIDTH) : ((rotation & 1) ? WIDTH : HEIGHT);
        if (direction == E0470_TURN_LTR || direction == E0470_TURN_TTB) assert(coord < span / 4);
        else assert(coord > span * 3 / 4);
    }
    now_us += scan_time_us;
    return fail_at == scans ? EPD_DRAW_OTHER_ERROR : EPD_DRAW_SUCCESS;
}

static void check_crop_baseline(EpdRect logical) {
    for(int py=0;py<HEIGHT;++py)for(int px=0;px<WIDTH;++px) {
        int lx,ly;
        physical_to_logical(px,py,&lx,&ly);
        bool inside=lx>=logical.x && lx<logical.x+logical.width &&
                    ly>=logical.y && ly<logical.y+logical.height;
        int shift=(px&1)*4;
        size_t i=(size_t)py*WIDTH/2+px/2;
        assert(((back[i]>>shift)&15)==(inside?((front[i]>>shift)&15):15));
    }
}

int main(void) {
    EpdiyHighlevelState hl = {front, back, diff, dirty_lines, dirty_columns};
    for (int rot = 0; rot < 4; ++rot) for (int dir = 0; dir < 4; ++dir) {
      uint64_t normal_trace=0;
      // PR17的21ms/14ms只改变等待；相位、驱动区域与最终帧必须逐拍相同。
      // PR17's 21ms/14ms pacing changes only padding; every phase, driven region and final frame must match.
      for(int fast=0;fast<2;++fast){
        e0470_page_turn_set_tick_us(fast?E0470_TURN_FAST_TICK_US:21000);
        rotation = (enum EpdRotation)rot;
        direction = (e0470_turn_dir_t)dir;
        memset(front, 0x24, sizeof(front));
        memset(back, 0xFF, sizeof(back));
        scans = differences = powerons = fail_at = 0;
        now_us = 0; trace_hash=UINT64_C(14695981039346656037); memset(phase_hits,0,sizeof(phase_hits));
        EpdRect logical = (rot & 1) ? (EpdRect){0, 0, HEIGHT, WIDTH} :
                                      (EpdRect){0, 0, WIDTH, HEIGHT};
        assert(e0470_page_turn(&hl, logical, direction) == EPD_DRAW_SUCCESS);
        assert(scans == 52 && differences == 1 && powerons == 1);
        assert(!memcmp(back, front, sizeof(back)) && now_us == (fast?728000:1092000));
        if(fast)assert(trace_hash==normal_trace);else normal_trace=trace_hash;
        assert(!staged_luts);
        for(int p=0;p<37;++p)assert(phase_hits[p]==16);
        // 真实阅读页保留页眉和底栏；裁剪区同样必须覆盖全部 16 带。
        // The actual reader leaves header and footer outside the crop; all 16 bands must still run.
        EpdRect reader = {0, 160, logical.width, logical.height - 288};
        memset(back, 0xFF, sizeof(back));
        scans = differences = 0;
        check_direction = false;
        assert(e0470_page_turn(&hl, reader, direction) == EPD_DRAW_SUCCESS);
        assert(scans == 52 && differences == 1);
        check_crop_baseline(reader);
        // 全屏阅读保留状态栏时从第 80 行开始，水波纹仍要覆盖整个正文。
        // Full-screen reading keeps the status row at y=80 while animating the remaining body.
        EpdRect fullscreen_reader = {0, 80, logical.width, logical.height - 80};
        memset(back, 0xFF, sizeof(back));
        scans = differences = 0;
        assert(e0470_page_turn(&hl, fullscreen_reader, direction) == EPD_DRAW_SUCCESS);
        assert(scans == 52 && differences == 1);
        check_crop_baseline(fullscreen_reader);
        check_direction = true;
        memset(back, 0xFF, sizeof(back));
        scans = differences = 0;
        fail_at = 13;
        assert(e0470_page_turn(&hl, logical, direction) == EPD_DRAW_OTHER_ERROR);
        assert(scans == 13 && differences == 1);
        assert(!staged_luts);
        for (size_t i = 0; i < sizeof(back); ++i) assert(back[i] == 0xFF);
      }
    }
    // 扫描慢于节拍也不能截相；每一个失败位置都不提交半成品基准或保留LUT指针。
    // Never truncate phases when scanning is slower than pacing; no failure position may commit partial history or retain LUT pointers.
    rotation=EPD_ROT_PORTRAIT;direction=E0470_TURN_LTR;
    e0470_page_turn_set_tick_us(E0470_TURN_FAST_TICK_US);
    EpdRect full={0,0,HEIGHT,WIDTH};
    scans=differences=fail_at=0;now_us=0;scan_time_us=18000;
    memset(back,0xFF,sizeof(back));memset(phase_hits,0,sizeof(phase_hits));
    assert(e0470_page_turn(&hl,full,direction)==EPD_DRAW_SUCCESS);
    assert(scans==52 && now_us==936000 && !memcmp(back,front,sizeof(back)));
    for(int p=0;p<37;++p)assert(phase_hits[p]==16);
    scan_time_us=7000;
    for(int failure=1;failure<=52;++failure){
        scans=differences=0;fail_at=failure;
        memset(back,0xFF,sizeof(back));
        assert(e0470_page_turn(&hl,full,direction)==EPD_DRAW_OTHER_ERROR);
        assert(scans==failure && !staged_luts);
        for(size_t i=0;i<sizeof(back);++i)assert(back[i]==0xFF);
    }
    // 主页24窄带仍覆盖37相且只回写正文；横条、图标与裁剪外邻接半字节保留旧基准。
    // Main-page 24 narrow bands cover all 37 phases and commit only the body; markers, icons and neighboring nibbles retain the old baseline.
    test_bands=24; test_launch_step=2;
    e0470_page_turn_set_tick_us(E0470_TURN_DEFAULT_TICK_US);
    for(int rot=0;rot<4;++rot)for(int dir=0;dir<4;++dir) {
        rotation=(enum EpdRotation)rot; direction=(e0470_turn_dir_t)dir;
        EpdRect logical=(rot&1)?(EpdRect){0,0,HEIGHT,WIDTH}:(EpdRect){0,0,WIDTH,HEIGHT};
        EpdRect body={0,0,logical.width,logical.height-128};
        memset(front,0x24,sizeof(front)); memset(back,0xFF,sizeof(back));
        scans=differences=powerons=fail_at=0; now_us=0;
        memset(phase_hits,0,sizeof(phase_hits)); check_direction=false;
        assert(e0470_page_turn_with_waveform(&hl,body,direction,&soft_wave,24)==EPD_DRAW_SUCCESS);
        assert(scans==83 && differences==1 && powerons==1 && now_us==996000);
        for(int p=0;p<37;++p)assert(phase_hits[p]==24);
        check_crop_baseline(body);
        memset(back,0xFF,sizeof(back)); scans=differences=0; fail_at=13;
        assert(e0470_page_turn_with_waveform(&hl,body,direction,&soft_wave,24)==EPD_DRAW_OTHER_ERROR);
        assert(scans==13 && differences==1);
        for(size_t i=0;i<sizeof(back);++i)assert(back[i]==0xFF);
    }
    // 不同序列切换必须重建LUT，非法条带/缺波形在输出前拒绝。
    // Rebuild LUTs when changing sequences; reject invalid bands or missing waveforms before output.
    int before_luts=lut_builds;
    test_bands=16; test_launch_step=1;
    e0470_page_turn_set_tick_us(E0470_TURN_FAST_TICK_US);
    scans=differences=fail_at=0;
    assert(e0470_page_turn(&hl,(EpdRect){0,0,HEIGHT,WIDTH},E0470_TURN_LTR)==EPD_DRAW_SUCCESS);
    assert(lut_builds==before_luts+37 && scans==52);
    scans=differences=0;
    const unsigned invalid_bands[]={0,1,33};
    for(unsigned i=0;i<sizeof(invalid_bands)/sizeof(invalid_bands[0]);++i)
        assert(e0470_page_turn_with_waveform(&hl,(EpdRect){0,0,HEIGHT,WIDTH},E0470_TURN_LTR,
                                             &soft_wave,invalid_bands[i])==EPD_DRAW_INVALID_CROP);
    assert(e0470_page_turn_with_waveform(&hl,(EpdRect){0,0,HEIGHT,WIDTH},E0470_TURN_LTR,NULL,24)==EPD_DRAW_NO_PHASES_AVAILABLE);
    assert(scans==0 && differences==0);
    e0470_page_turn_release();
    memset(back, 0xFF, sizeof(back));
    scans = differences = 0;
    fail_at = 0;
    water_test_alloc_fail = 1;
    assert(e0470_page_turn(&hl, (EpdRect){0, 0, HEIGHT, WIDTH}, E0470_TURN_LTR) ==
           EPD_DRAW_NO_PHASES_AVAILABLE);
    assert(scans == 0 && differences == 0);
    for (size_t i = 0; i < sizeof(back); ++i) assert(back[i] == 0xFF);
    puts("water turn: 4 rotations × 4 directions; PR17 reader 16 bands / 52 ticks, 14ms vs 21ms identical phase/region traces; slow scans retain all 37 phases; all 52 failures retain history and clear LUTs; main 24 bands / 83 ticks / 12ms, crop bounds, profile cache, invalid inputs and OOM passed");
}
