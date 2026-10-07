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
#include "ui_wallpaper_crop.h"

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

// 仅头像自动裁掉明显的白色留边，并保持主体周围少量呼吸空间。
// Trim obvious white avatar margins while keeping a small amount of space around the subject.
static bool avatar_content_crop(const uint8_t *gray, unsigned width, unsigned height,
                                EpdRect area, int *crop_x, int *crop_y, int *crop_w, int *crop_h) {
    if (width < 24 || height < 24) return false;
    unsigned min_x = width, min_y = height, max_x = 0, max_y = 0, ink = 0;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            if (gray[(size_t)y * width + x] >= 245) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
            ++ink;
        }
    }
    if (ink < (uint64_t)width * height / 50) return false;
    int pad = (int)(width < height ? width : height) / 32;
    if (pad < 2) pad = 2;
    int box_w = (int)(max_x - min_x + 1) + pad * 2;
    int box_h = (int)(max_y - min_y + 1) + pad * 2;
    int w = box_w, h = box_h;
    if ((int64_t)w * area.height < (int64_t)h * area.width)
        w = (int)(((int64_t)h * area.width + area.height - 1) / area.height);
    else
        h = (int)(((int64_t)w * area.height + area.width - 1) / area.width);
    if (w > (int)width || h > (int)height ||
        ((int64_t)w * 10 > (int64_t)width * 9 &&
         (int64_t)h * 10 > (int64_t)height * 9)) return false;
    int x = ((int)min_x + (int)max_x + 1 - w) / 2;
    int y = ((int)min_y + (int)max_y + 1 - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > (int)width) x = (int)width - w;
    if (y + h > (int)height) y = (int)height - h;
    *crop_x = x; *crop_y = y; *crop_w = w; *crop_h = h;
    return true;
}

static bool draw_image(uint8_t *fb, const char *path, EpdRect area, int radius) {
    if (!fb || !path || strncmp(path, "/sdcard/", 8) ||
        area.width <= 0 || area.height <= 0 ||
        area.x < 0 || area.y < 0 ||
        area.x + area.width > UI_LOCK_WIDTH || area.y + area.height > UI_LOCK_HEIGHT)
        return false;
    const char *ext = strrchr(path, '.');
    bool png = ext && !strcasecmp(ext, ".png");
    if (!ext || (!png && strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) return false;
    struct stat st;
    if (stat(path, &st) || st.st_size <= 0 || (uint64_t)st.st_size > BOOK_IMAGE_FILE_MAX) return false;
    unsigned source_w = 0, source_h = 0;
    bool ok = book_image_file_dimensions(path, png, &source_w, &source_h);
    unsigned width = (unsigned)area.width, height = (unsigned)area.height;
    bool source_wider = ok && (uint64_t)source_w * height > (uint64_t)source_h * width;
    if (source_wider) width = (unsigned)(((uint64_t)height * source_w + source_h - 1) / source_h);
    else if (ok) height = (unsigned)(((uint64_t)width * source_h + source_w - 1) / source_w);
    if (!width) width = 1;
    if (!height) height = 1;
    // 保持原始宽高比，并受解码器边长限制；宽图只缩一次，避免过度降采样。
    // Preserve source aspect ratio within decoder limits; avoid repeatedly downsampling wide photos.
    if (width > 1216u) {
        width = 1216u;
        height = (unsigned)(((uint64_t)width * source_h + source_w / 2u) / source_w);
    } else if (height > 1216u) {
        height = 1216u;
        width = (unsigned)(((uint64_t)height * source_w + source_h / 2u) / source_h);
    }
    if (!width) width = 1;
    if (!height) height = 1;
    // 缓冲区最多一屏像素；最终从原比例灰阶图居中裁切到目标区域。
    // Cap the buffer to one screen and center-crop the aspect-preserved gray image to the target.
    while (ok && (width > 1216u || height > 1216u ||
                  (uint64_t)width * height > 684u * 1216u)) {
        width = (width * 3u + 3u) / 4u;
        height = (height * 3u + 3u) / 4u;
    }
    uint8_t *gray = ok ? heap_caps_malloc((size_t)width * height, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!gray) ok = false;
    if (ok) ok = book_image_file_grayscale(path, png, width, height, gray);
    if (!ok) { free(gray); return false; }
    ui_wallpaper_crop_t center = ui_wallpaper_center_crop(width, height,
                                                           (unsigned)area.width, (unsigned)area.height);
    int crop_x = (int)center.x, crop_y = (int)center.y;
    int crop_w = (int)center.width, crop_h = (int)center.height;
    if (radius > 0) avatar_content_crop(gray, width, height, area,
                                        &crop_x, &crop_y, &crop_w, &crop_h);

    for (int y = area.y; y < area.y + area.height; ++y)
        for (int x = area.x; x < area.x + area.width; ++x) {
            if (!inside_rounded(area, radius, x, y)) continue;
            int source_x = crop_x + (int)((int64_t)(x - area.x) * crop_w / area.width);
            int source_y = crop_y + (int)((int64_t)(y - area.y) * crop_h / area.height);
            epd_draw_pixel(x, y,
                ui_image_dither_gray(gray[(size_t)source_y * width + source_x],
                                     x, y), fb);
        }
    free(gray);
    return true;
}

bool ui_wallpaper_draw(uint8_t *fb, const char *path, EpdRect area) {
    return draw_image(fb, path, area, 0);
}

bool ui_wallpaper_draw_rounded(uint8_t *fb, const char *path, EpdRect area, int radius) {
    if (radius < 0 || radius > area.width / 2 || radius > area.height / 2) return false;
    return draw_image(fb, path, area, radius);
}
