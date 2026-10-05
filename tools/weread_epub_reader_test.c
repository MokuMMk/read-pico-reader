/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 验证微信读书 ZIP 写入器产物能由 Pico 原有书源打开。/ Open a WeRead ZIP-writer fixture with Pico's reader.
 * 冻结：临时公开文本，不联网。/ Frozen: temporary public text, no networking.
 */
#include "book_epub.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(int argc, char** argv) {
    assert(argc == 2);
    book_epub_t* book = NULL;
    assert(book_epub_open(argv[1], &book) == ESP_OK && book);
    assert(book_epub_chapter_count(book) == 1);
    html_text_t text = {0};
    assert(book_epub_load(book, 0, &text) == ESP_OK);
    assert(text.utf8 && strstr(text.utf8, "中文正文"));
    assert(text.image_count == 1 && strstr(text.images[0], "images/test.png"));
    html_text_free(&text);
    book_epub_close(book);
    char title[192], author[96];
    assert(book_epub_metadata(argv[1], title, sizeof(title), author, sizeof(author)) == ESP_OK);
    assert(!strcmp(title, "中文测试") && !strcmp(author, "测试作者"));
    puts("PASS: Pico EPUB reader opened ZIP-writer fixture, Chinese metadata and offline chapter");
}
