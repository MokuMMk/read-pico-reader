/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：用主机像素缓冲验证按压背景和刷新区域。
 * English: Verify pressed backgrounds and refresh regions using a host pixel buffer.
 * 冻结：仅用于主机测试。/ Frozen: Host tests only.
 */
#include "ui_kit.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define W 100
#define H 100
static enum EpdRotation rotation = EPD_ROT_LANDSCAPE;
int epd_width(void) { return W; }
int epd_height(void) { return H; }
enum EpdRotation epd_get_rotation(void) { return rotation; }
static void physical(int *x, int *y) {
    int ox = *x, oy = *y;
    if (rotation == EPD_ROT_PORTRAIT) { *x = W - oy - 1; *y = ox; }
    else if (rotation == EPD_ROT_INVERTED_LANDSCAPE) { *x = W - ox - 1; *y = H - oy - 1; }
    else if (rotation == EPD_ROT_INVERTED_PORTRAIT) { *x = oy; *y = H - ox - 1; }
}
uint8_t epd_get_pixel(int x, int y, int width, int height, const uint8_t *fb) {
    assert(width == W && height == H && x >= 0 && x < W && y >= 0 && y < H);
    return fb[y * W + x];
}
uint8_t app_settings_system_contrast(void) { return 100; }
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t *fb) {
    physical(&x, &y);
    assert(x >= 0 && x < W && y >= 0 && y < H); fb[y * W + x] = color;
}
void epd_fill_rect(EpdRect r, uint8_t color, uint8_t *fb) {
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x) epd_draw_pixel(x, y, color, fb);
}
void epd_fill_circle_helper(int x, int y, int radius, int corners, int delta, uint8_t color, uint8_t *fb) {
    (void)x; (void)y; (void)radius; (void)corners; (void)delta; (void)color; (void)fb;
}
static void rect_eq(EpdRect r, int x, int y, int w, int h) {
    assert(r.x == x && r.y == y && r.width == w && r.height == h);
}
// 检查实际尺寸、底色合成与四种旋转下的像素一致性。/ Check draw sizes, blending and all rotations.
static void test_icons(void) {
    uint8_t fb[W * H], reference[W * H];
    const int sizes[] = {26, 30, 32, 34, 38, 40, 44, 46, 48, 72};
    for (int icon = 0; icon < UI_ICON_COUNT; ++icon) {
        for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
            int size = sizes[s], left = 50 - size / 2;
            for (int light = 0; light < 2; ++light) {
                uint8_t background = light ? 0x60 : 0xe0;
                rotation = EPD_ROT_LANDSCAPE;
                memset(reference, background, sizeof(reference));
                ui_draw_icon(reference, 50, 50, size, (ui_icon_t)icon, light ? 255 : 0);
                unsigned changes = 0;
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    uint8_t value = reference[y * W + x];
                    changes += value != background;
                    assert(light ? value >= background : value <= background);
                    if (x < left || y < left || x >= left + size || y >= left + size)
                        assert(value == background);
                }
                assert(changes > 0);
                for (int r = EPD_ROT_PORTRAIT; r <= EPD_ROT_INVERTED_PORTRAIT; ++r) {
                    rotation = (enum EpdRotation)r;
                    memset(fb, background, sizeof(fb));
                    ui_draw_icon(fb, 50, 50, size, (ui_icon_t)icon, light ? 255 : 0);
                    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                        int px = x, py = y; physical(&px, &py);
                        assert(fb[py * W + px] == reference[y * W + x]);
                    }
                }
            }
        }
    }
    rotation = EPD_ROT_LANDSCAPE;
    memset(fb, 0xe0, sizeof(fb)); memcpy(reference, fb, sizeof(fb));
    ui_draw_icon(fb, 50, 50, 40, UI_ICON_COUNT, 0);
    ui_draw_icon(fb, 50, 50, 0, UI_ICON_WIFI, 0);
    ui_draw_icon(NULL, 50, 50, 40, UI_ICON_WIFI, 0);
    assert(!memcmp(fb, reference, sizeof(fb)));
    ui_draw_icon(fb, 50, 50, 40, UI_ICON_BOOKMARK, 0);
    ui_draw_icon(reference, 50, 50, 40, UI_ICON_BOOKMARK_CHECK, 0);
    assert(memcmp(fb, reference, sizeof(fb)) != 0);
}
int main(void) {
    rect_eq(ui_rect_union((EpdRect){0}, (EpdRect){10,20,30,40}), 10,20,30,40);
    rect_eq(ui_rect_union((EpdRect){10,20,30,40}, (EpdRect){5,30,50,10}), 5,20,50,40);
    rect_eq(ui_rect_union((EpdRect){-10,-20,30,40}, (EpdRect){0}), 0,0,20,20);
    rect_eq(ui_rect_union((EpdRect){680,1210,INT_MAX,INT_MAX}, (EpdRect){0}), 680,1210,4,6);
    rect_eq(ui_rect_union((EpdRect){INT_MIN,INT_MIN,10,10}, (EpdRect){0}), 0,0,0,0);
    uint8_t fb[W * H]; memset(fb, UI_GRAY_WHITE, sizeof(fb));
    ui_draw_pressed_round_rect(fb, (EpdRect){10,10,60,40}, 0);
    assert(fb[30 * W + 40] == UI_GRAY_LIGHT);
    assert(fb[30 * W + 14] == UI_GRAY_BLACK && fb[30 * W + 16] == UI_GRAY_BLACK);
    assert(fb[30 * W + 9] == UI_GRAY_WHITE);
    // 按压底图之后绘制黑字像素，文字不反色。/ Draw black text pixels after the pressed background, without inversion.
    epd_draw_pixel(40,30,UI_GRAY_BLACK,fb); assert(fb[30 * W + 40] == UI_GRAY_BLACK);
    test_icons();
    puts("ui kit host tests passed");
}
