/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：壁纸预览与锁屏共用的 TF 图片缩放和灰阶绘制。
 * English: Shared SD image scaling and grayscale rendering for wallpaper preview and lock.
 */

#include "ui_wallpaper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "book_cover.h"
#include "esp_heap_caps.h"
#include "ui_image_dither.h"
#include "ui_kit.h"

bool ui_wallpaper_draw(uint8_t *fb, const char *path, EpdRect area) {
    if (!fb || !path || strncmp(path, "/sdcard/", 8) ||
        area.width <= 0 || area.height <= 0 ||
        area.x < 0 || area.y < 0 ||
        area.x + area.width > UI_LOCK_WIDTH || area.y + area.height > UI_LOCK_HEIGHT)
        return false;
    const char *ext = strrchr(path, '.');
    bool png = ext && !strcasecmp(ext, ".png");
    if (!ext || (!png && strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) return false;
    struct stat st;
    if (stat(path, &st) || st.st_size <= 0 || st.st_size > 2 * 1024 * 1024) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    uint8_t *encoded = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = encoded && fread(encoded, 1, (size_t)st.st_size, file) == (size_t)st.st_size;
    fclose(file);
    unsigned source_w = 0, source_h = 0;
    if (ok) ok = book_image_dimensions(encoded, (size_t)st.st_size, png, &source_w, &source_h);
    unsigned width = (unsigned)area.width, height = (unsigned)area.height;
    if (ok && (uint64_t)source_w * height > (uint64_t)source_h * width)
        height = (unsigned)((uint64_t)width * source_h / source_w);
    else if (ok)
        width = (unsigned)((uint64_t)height * source_w / source_h);
    if (!width) width = 1;
    if (!height) height = 1;
    uint8_t *gray = ok ? heap_caps_malloc((size_t)width * height, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!gray) ok = false;
    if (ok) ok = book_image_grayscale(encoded, (size_t)st.st_size, png, width, height, gray);
    free(encoded);
    if (!ok) { free(gray); return false; }
    int left = area.x + (area.width - (int)width) / 2;
    int top = area.y + (area.height - (int)height) / 2;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            epd_draw_pixel(left + (int)x, top + (int)y,
                ui_image_dither_gray(ui_contrast_gray(gray[y * width + x]),
                                     left + (int)x, top + (int)y), fb);
    free(gray);
    return true;
}
