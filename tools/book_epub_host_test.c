/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：EPUB 容器、spine、目录与正文集成测试。
 * English: EPUB container, spine, navigation and text integration tests.
 * 冻结：仅用于主机测试。/ Frozen: Host tests only.
 */
#include "book_epub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    assert(argc > 1);
    for (int a = 1; a < argc; ++a) {
        book_epub_t *book = NULL;
        if (strstr(argv[a], "/bad_")) {
            assert(book_epub_open(argv[a], &book) != ESP_OK && !book);
            printf("epub rejection passed: %s\n", argv[a]); continue;
        }
        assert(book_epub_open(argv[a], &book) == ESP_OK && book);
        if (strstr(argv[a], "good_body_")) {
            bool samefile = strstr(argv[a], "samefile") != NULL;
            bool partial = strstr(argv[a], "partial") != NULL;
            bool false_positive = strstr(argv[a], "false_positive") != NULL;
            assert(book_epub_chapter_count(book) == (samefile ? 1 :
                strstr(argv[a], "priority") || strstr(argv[a], "without_nav") ||
                strstr(argv[a], "ideographic") ? 5 : 4));
            assert(book_epub_navigation_count(book) == (samefile ? 2 : 4));
            char title[160];
            assert(book_epub_navigation_title(book, 0, title, sizeof(title)) == ESP_OK);
            if (false_positive) assert(!strcmp(title, "Parent & One"));
            else assert(!strcmp(title, "第一章 起点"));
            if (!partial && !false_positive) {
                assert(book_epub_navigation_title(book, 1, title, sizeof(title)) == ESP_OK);
                assert(!strcmp(title, strstr(argv[a], "ideographic") ? "第2章　第二站" : "第2章 第二站"));
                for (size_t i = 0; i < (samefile ? 2 : 4); ++i) {
                    size_t chapter = book_epub_navigation_chapter(book, i);
                    size_t source = book_epub_navigation_source_offset(book, i);
                    html_text_t text = {0}; size_t offset = 0;
                    assert(source != SIZE_MAX);
                    assert(book_epub_navigation_anchor(book, i) == NULL);
                    assert(book_epub_load_target(book, chapter, NULL, source, &offset, &text) == ESP_OK);
                    assert(offset < text.len);
                    assert(text.utf8[offset] == (char)0xe7);
                    html_text_free(&text);
                }
            } else if (partial) {
                assert(book_epub_navigation_title(book, 1, title, sizeof(title)) == ESP_OK);
                assert(!strcmp(title, "Child Two"));
            }
            book_epub_close(book); printf("epub fixture passed: %s\n", argv[a]); continue;
        }
        if (strstr(argv[a], "good_multianchor")) {
            assert(book_epub_chapter_count(book) == 1);
            assert(book_epub_navigation_count(book) == 2);
            char first_title[160];
            assert(book_epub_chapter_title(book, 0, first_title, sizeof(first_title)) == ESP_OK);
            assert(!strcmp(first_title, "第一章 起点"));
            for (size_t i = 0; i < 2; ++i) {
                char title[160]; html_text_t text = {0}; size_t offset = 0;
                assert(book_epub_navigation_chapter(book, i) == 0);
                assert(book_epub_navigation_title(book, i, title, sizeof(title)) == ESP_OK);
                assert(!strncmp(title, i ? "第二章" : "第一章", strlen("第一章")));
                const char *anchor = book_epub_navigation_anchor(book, i);
                assert(anchor && !strcmp(anchor, i ? "two" : "one"));
                assert(book_epub_load_anchor(book, 0, anchor, &offset, &text) == ESP_OK);
                assert(offset > 0 && offset < text.len);
                assert(strstr(text.utf8 + offset, i ? "第二章" : "第一章") == text.utf8 + offset);
                html_text_free(&text);
            }
            book_epub_close(book); printf("epub fixture passed: %s\n", argv[a]); continue;
        }
        bool frontmatter = strstr(argv[a], "good_frontmatter") != NULL;
        size_t chapters = frontmatter ? 5 : 4;
        assert(book_epub_chapter_count(book) == chapters);
        assert(book_epub_navigation_count(book) == 4);
        for (size_t i = 0; i < 4; ++i)
            assert(book_epub_navigation_chapter(book, i) == i + (frontmatter ? 1 : 0));
        assert(book_epub_navigation_chapter(book, 4) == SIZE_MAX);
        uint32_t previous = 0;
        for (size_t i = 0; i < chapters; ++i) {
            char title[160]; html_text_t text = {0};
            assert(book_epub_chapter_title(book, i, title, sizeof(title)) == ESP_OK);
            assert(title[0]);
            if (frontmatter && i == 0) assert(!strcmp(title, "作者信息"));
            else if (strstr(argv[a], "good_defaults") || (frontmatter && i > 0 && strstr(argv[a], "fallback")))
                assert(!strncmp(title, "Chapter ", strlen("Chapter ")));
            else assert(strncmp(title, "第 ", strlen("第 ")));
            if (strstr(argv[a], "good_paths") && i < 2) assert(!strcmp(title, i ? "Child Two" : "Parent & One"));
            if (strstr(argv[a], "good_navfallback") || strstr(argv[a], "good_navrole") ||
                strstr(argv[a], "good_navtype") || strstr(argv[a], "good_navunmarked"))
                assert(!strncmp(title, "NAV ", 4));
            uint32_t offset = book_epub_chapter_byte_offset(book, i);
            assert(i ? offset > previous : offset == 0); previous = offset;
            assert(book_epub_load(book, i, &text) == ESP_OK);
            assert(text.utf8 && text.len && text.blocks && text.count);
            if (strstr(argv[a], "good_resources") && i == 0) {
                assert(text.image_count == 1 && !strcmp(text.images[0], "../images/pic&one.png"));
                uint8_t *image = NULL; size_t size = 0; bool png = false;
                assert(book_epub_image(book, i, text.images[0], &image, &size, &png) == ESP_OK);
                assert(image && size == 12 && png); free(image);
                assert(book_epub_image(book, i, "../images/wrapper.svg", &image, &size, &png) == ESP_OK);
                assert(image && size == 12 && png); free(image);
                assert(text.blocks[0].align == 1);
            }
            html_text_free(&text);
        }
        assert(book_epub_total_bytes(book) > previous);
        book_epub_close(book); printf("epub fixture passed: %s\n", argv[a]);
    }
    puts("epub host tests passed");
}
