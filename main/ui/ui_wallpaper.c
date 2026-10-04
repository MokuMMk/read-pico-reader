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

static bool inside_rounded(EpdRect area, int radius, int x, int y) {
    if (radius <= 0) return true;
    int local_x = x - area.x, local_y = y - area.y;
    int center_x = local_x < radius ? radius - 1 :
                   local_x >= area.width - radius ? area.width - radius : local_x;
    int center_y = local_y < radius ? radius - 1 :
                   local_y >= area.height - radius ? area.height - radius : local_y;
    int dx = local_x - center_x, dy = local_y - center_y;
    return dx * dx + dy * dy <= radius * radius;
}

static bool draw_image(uint8_t *fb, const char *path, EpdRect area, bool crop, int radius) {
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
    bool source_wider = ok && (uint64_t)source_w * height > (uint64_t)source_h * width;
    if (crop) {
        if (source_wider) width = (unsigned)(((uint64_t)height * source_w + source_h - 1) / source_h);
        else if (ok) height = (unsigned)(((uint64_t)width * source_h + source_w - 1) / source_w);
    } else if (source_wider)
        height = (unsigned)((uint64_t)width * source_h / source_w);
    else if (ok)
        width = (unsigned)((uint64_t)height * source_w / source_h);
    if (!width) width = 1;
    if (!height) height = 1;
    if ((uint64_t)width * height > 1024u * 1024u) ok = false;
    uint8_t *gray = ok ? heap_caps_malloc((size_t)width * height, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!gray) ok = false;
    if (ok) ok = book_image_grayscale(encoded, (size_t)st.st_size, png, width, height, gray);
    free(encoded);
    if (!ok) { free(gray); return false; }
    int left = area.x + (area.width - (int)width) / 2;
    int top = area.y + (area.height - (int)height) / 2;
    for (int y = area.y; y < area.y + area.height; ++y)
        for (int x = area.x; x < area.x + area.width; ++x) {
            int source_x = x - left, source_y = y - top;
            if (source_x < 0 || source_y < 0 || source_x >= (int)width || source_y >= (int)height ||
                !inside_rounded(area, radius, x, y)) continue;
            epd_draw_pixel(x, y,
                ui_image_dither_gray(ui_contrast_gray(gray[(size_t)source_y * width + source_x]),
                                     x, y), fb);
        }
    free(gray);
    return true;
}

bool ui_wallpaper_draw(uint8_t *fb, const char *path, EpdRect area) {
    return draw_image(fb, path, area, false, 0);
}

bool ui_wallpaper_draw_rounded(uint8_t *fb, const char *path, EpdRect area, int radius) {
    if (radius < 0 || radius > area.width / 2 || radius > area.height / 2) return false;
    return draw_image(fb, path, area, true, radius);
}
