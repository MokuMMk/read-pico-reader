/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：用伪字体验证布局完整性、偏移和测宽复杂度。
 * English: Verify layout completeness, offsets and measurement complexity using a fake font.
 * 冻结：只用于宿主测试。/ Frozen: Host testing only.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "book_layout.h"
#include "ttf_font.h"
static char drawn[20000];
static size_t measured_codepoints;
static size_t measure_calls;
static int first_draw_px, last_draw_px, first_draw_x, last_draw_x;
static int last_tracking_px;
int test_guide_segments;
int test_guide_first_y;
uint8_t test_guide_gray;
int ttf_text_width_px(int px, const char* text) {
    int n = 0, width = 0;
    for (; *text; text++) if (((unsigned char)*text & 0xc0) != 0x80) {
        n++;
        width += *text == 'i' ? px / 2 : *text == 'W' ? px + px / 2 : px;
    }
    measured_codepoints += (size_t)n;
    measure_calls++;
    return width;
}
int ttf_ascender_px(int px) { return px; }
void ttf_draw_text_px(uint8_t* fb, int x, int y, int px, const char* text,
                      enum EpdFontFlags align, uint8_t fg, uint8_t bg) {
    (void)fb; (void)y; (void)px; (void)align;
    assert(fg <= 15 && bg <= 15);
    if (!drawn[0]) { first_draw_px = px; first_draw_x = x; }
    last_draw_px = px;
    last_draw_x = x;
    assert(strlen(drawn) + strlen(text) < sizeof(drawn));
    strcat(drawn, text);
}
void ttf_draw_text_px_spaced(uint8_t* fb, int x, int y, int px, const char* text,
                             int tracking_px, uint8_t fg, uint8_t bg) {
    last_tracking_px = tracking_px;
    ttf_draw_text_px(fb, x, y, px, text, EPD_DRAW_ALIGN_LEFT, fg, bg);
}
int main(void) {
    EpdRect r = {0, 0, 20, 30};
    const char text[] = "甲乙丙丁戊己庚辛壬癸";
    assert(book_layout_build(text, strlen(text), r, 10));
    assert(book_layout_page_count() == 3);
    assert(book_layout_page_start_offset(1) == 12);
    assert(book_layout_page_for_offset(11) == 0);
    assert(book_layout_page_for_offset(12) == 1);
    assert(book_layout_page_for_offset(999) == 2);
    uint8_t fb = 0;
    for (size_t i = 0; i < book_layout_page_count(); i++) {
        assert(book_layout_page_start_offset(i) % 3 == 0);
        book_layout_draw_page(&fb, i, r, 10);
    }
    assert(strcmp(drawn, text) == 0);
    const char mixed[] = "Wi甲iiW";
    assert(book_layout_build(mixed, strlen(mixed), r, 10));
    assert(book_layout_page_count() == 2);
    assert(book_layout_page_start_offset(1) == 7);
    drawn[0] = 0;
    book_layout_draw_page(&fb, 0, r, 10);
    book_layout_draw_page(&fb, 1, r, 10);
    assert(strcmp(drawn, mixed) == 0);
    assert(book_layout_build("", 0, r, 10));
    assert(book_layout_page_count() == 1);
    assert(book_layout_build(NULL, 0, r, 10));
    assert(!book_layout_build(NULL, 1, r, 10));
    assert(book_layout_page_count() == 0);
    assert(!book_layout_build("\xe7\x94", 2, r, 10));
    assert(!book_layout_build("x\0y", 3, r, 10));
    assert(!book_layout_build("x", 1, (EpdRect){0,0,5,30}, 10));
    assert(!book_layout_build("x", 1, (EpdRect){0,0,20,5}, 10));
    char* many = malloc(4097);
    memset(many, 'x', 4097);
    r = (EpdRect){0,0,10,15};
    assert(book_layout_build(many, 4096, r, 10));
    assert(book_layout_page_count() == 4096);
    assert(!book_layout_build(many, 4097, r, 10));
    assert(book_layout_page_count() == 0);
    free(many);
    assert(book_layout_build("a\r\n\r\nb", 6, (EpdRect){0,0,20,100}, 10));
    drawn[0] = 0;
    book_layout_draw_page(&fb, 0, (EpdRect){0,0,20,100}, 10);
    assert(strcmp(drawn, "ab") == 0);
    char long_line[1024];
    memset(long_line, 'z', sizeof(long_line));
    r = (EpdRect){0,0,10000,30};
    measured_codepoints = measure_calls = 0;
    assert(book_layout_build(long_line, sizeof(long_line), r, 10));
    printf("long-line build: %zu calls, %zu measured codepoints\n", measure_calls, measured_codepoints);
    fflush(stdout);
    assert(measured_codepoints <= sizeof(long_line) * 2);
    assert(book_layout_page_count() == 1);
    drawn[0] = 0;
    book_layout_draw_page(&fb, 0, r, 11);
    assert(drawn[0] == 0);
    book_layout_draw_page(&fb, 99, r, 10);
    assert(drawn[0] == 0);
    measured_codepoints = measure_calls = 0;
    book_layout_draw_page(&fb, 0, r, 10);
    assert(measured_codepoints <= sizeof(long_line) * 2);
    assert(strlen(drawn) == sizeof(long_line));
    assert(memcmp(drawn, long_line, sizeof(long_line)) == 0);
    assert(book_layout_page_start_offset(99) == sizeof(long_line));
    assert(!book_layout_build("\xed\xa0\x80", 3, r, 10));
    assert(!book_layout_build("\xf4\x90\x80\x80", 4, r, 10));
    assert(!book_layout_build("\xc0\xaf", 2, r, 10));
    book_layout_free();
    assert(book_layout_page_count() == 0);
    const char styled[] = "Title\nbody";
    blk_t blocks[] = {{.offset = 0, .len = 5, .heading = true, .image = -1},
                      {.offset = 6, .len = 4, .heading = false, .image = -1}};
    r = (EpdRect){0, 0, 100, 45};
    assert(book_layout_build_blocks(styled, strlen(styled), blocks, 2, r, 10));
    assert(book_layout_page_count() == 2);
    assert(book_layout_page_start_offset(1) == 6);
    drawn[0] = 0;
    book_layout_draw_page(&fb, 0, r, 10);
    book_layout_draw_page(&fb, 1, r, 10);
    assert(!strcmp(drawn, "Titlebody") && first_draw_px == 18 && last_draw_px == 10);
    const char punct[] = "甲乙，丙";
    r = (EpdRect){0, 0, 20, 15};
    assert(book_layout_build(punct, strlen(punct), r, 10));
    assert(book_layout_page_count() == 2 && book_layout_page_start_offset(1) == 9);
    const char opener[] = "甲（乙丙";
    assert(book_layout_build(opener, strlen(opener), r, 10));
    assert(book_layout_page_count() == 3 && book_layout_page_start_offset(1) == 3);
    const char aligned[] = "甲乙";
    blk_t aligned_block = {.offset = 0, .len = strlen(aligned), .image = -1,
                           .align = 1, .indent_percent = 100};
    r = (EpdRect){10, 0, 100, 30};
    assert(book_layout_build_blocks(aligned, strlen(aligned), &aligned_block, 1, r, 10));
    drawn[0] = 0;
    book_layout_draw_page(&fb, 0, r, 10);
    assert(first_draw_x == 55 && last_draw_x == 55);
    r = (EpdRect){0, 0, 100, 45};
    book_layout_set_chapter_lead(6, 20);
    assert(book_layout_build_blocks(styled, strlen(styled), blocks, 2, r, 10));
    assert(book_layout_page_count() == 1);
    assert(book_layout_page_start_offset(0) == 6);
    drawn[0] = 0;
    book_layout_draw_page(&fb, 0, r, 10);
    assert(!strcmp(drawn, "body"));
    book_layout_set_chapter_lead(0, 0);
    r.width = 20;
    assert(book_layout_build_blocks(styled, strlen(styled), blocks, 2, r, 10));
    assert(book_layout_page_count() == 6);
    assert(book_layout_page_start_offset(5) == 6);
    drawn[0] = 0;
    for (size_t i = 0; i < book_layout_page_count(); ++i) book_layout_draw_page(&fb, i, r, 10);
    assert(!strcmp(drawn, "Titlebody"));
    assert(!book_layout_build_blocks(styled, strlen(styled), blocks, 2, (EpdRect){0,0,100,20}, 10));
    blocks[1].offset = 5;
    assert(!book_layout_build_blocks(styled, strlen(styled), blocks, 2, r, 10));
    assert(book_layout_page_count() == 0);
    blocks[1].offset = 6;
    blocks[1].len = SIZE_MAX;
    assert(!book_layout_build_blocks(styled, strlen(styled), blocks, 2, r, 10));
    assert(!book_layout_build_blocks(styled, strlen(styled), NULL, 2, r, 10));
    blk_t split_utf8[] = {{.offset = 0, .len = 1, .heading = true, .image = -1},
                          {.offset = 2, .len = 2, .heading = false, .image = -1}};
    assert(!book_layout_build_blocks("甲\nx", 5, split_utf8, 2, r, 10));
    assert(book_layout_build("body", 4, r, 10));
    drawn[0] = 0;
    book_layout_draw_page(&fb, 0, r, 10);
    assert(first_draw_px == 10);
    book_layout_set_typography(0);
    r = (EpdRect){0, 0, 35, 100};
    assert(book_layout_build("abcd", 4, r, 10));
    drawn[0] = 0;
    book_layout_draw_page(&fb, 0, r, 10);
    assert(!strcmp(drawn, "abcd") && first_draw_x == 20 && last_draw_x == 0);
    book_layout_set_typography(2);
    r = (EpdRect){0, 0, 25, 15};
    assert(book_layout_build("abcd", 4, r, 10));
    assert(book_layout_page_count() == 2 && book_layout_page_start_offset(1) == 2);
    drawn[0] = 0; last_tracking_px = 0;
    book_layout_draw_page(&fb, 0, r, 10);
    assert(!strcmp(drawn, "ab") && last_tracking_px == 2);
    book_layout_set_typography(-2);
    r.width = 28;
    assert(book_layout_build("abcd", 4, r, 10));
    assert(book_layout_page_count() == 2 && book_layout_page_start_offset(1) == 3);
    drawn[0] = 0; last_tracking_px = 0;
    book_layout_draw_page(&fb, 0, r, 10);
    assert(!strcmp(drawn, "abc") && last_tracking_px == -2);
    book_layout_set_typography(0);
    r = (EpdRect){10, 20, 400, 120};
    assert(book_layout_build("甲乙丙丁", strlen("甲乙丙丁"), r, 40));
    book_layout_set_reading_line(0);
    test_guide_segments = 0;
    book_layout_draw_page(&fb, 0, r, 40);
    assert(test_guide_segments == 0);
    book_layout_set_reading_line(1);
    book_layout_draw_page(&fb, 0, r, 40);
    int dashed_segments = test_guide_segments;
    assert(dashed_segments > 0);
    assert(test_guide_first_y == 70 && test_guide_gray == 0x80);
    book_layout_set_reading_line(2);
    test_guide_segments = 0;
    book_layout_draw_page(&fb, 0, r, 40);
    assert(test_guide_segments > dashed_segments);
    assert(test_guide_first_y == 70 && test_guide_gray == 0x80);
    book_layout_set_spacing(110, 50);
    assert(book_layout_build("甲乙丙丁", strlen("甲乙丙丁"), r, 40));
    test_guide_segments = 0;
    book_layout_draw_page(&fb, 0, r, 40);
    assert(test_guide_first_y == 62);
    book_layout_set_spacing(200, 50);
    assert(book_layout_build("甲乙丙丁", strlen("甲乙丙丁"), r, 40));
    test_guide_segments = 0;
    book_layout_draw_page(&fb, 0, r, 40);
    assert(test_guide_first_y == 80);
    book_layout_set_spacing(150, 50);
    book_layout_set_reading_line(0);
    book_layout_free();
    puts("book_layout_host_test: PASS");
}
