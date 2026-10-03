/* SPDX-License-Identifier: Apache-2.0 */
/* Validate bounded opening, navigation, and distant chapters in a long EPUB. */
#include "book_epub.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    book_epub_t *book = NULL;
    assert(book_epub_open(argv[1], &book) == ESP_OK && book);
    assert(book_epub_chapter_count(book) == 5000);
    assert(book_epub_navigation_count(book) == 5000);
    assert(book_epub_navigation_chapter(book, 4999) == 4999);
    char title[160];
    assert(book_epub_navigation_title(book, 4999, title, sizeof(title)) == ESP_OK);
    assert(!strcmp(title, "第5000章 终点"));
    html_text_t page = {0};
    assert(book_epub_load(book, 4999, &page) == ESP_OK);
    assert(strstr(page.utf8, "终点") != NULL);
    html_text_free(&page);
    book_epub_close(book);
    puts("large EPUB host test passed");
    return 0;
}
