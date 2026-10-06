#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOK_COVER_W 176
#define BOOK_COVER_H 240

// Output is one grayscale byte per pixel (0=black, 255=white).
bool book_cover_thumbnail(const uint8_t *data, size_t size, bool png,
                          uint8_t out[BOOK_COVER_W * BOOK_COVER_H]);
// Decode an image to a chosen grayscale size; output is one byte per pixel.
bool book_image_grayscale(const uint8_t *data, size_t size, bool png,
                          unsigned out_width, unsigned out_height, uint8_t *out);
bool book_image_dimensions(const uint8_t *data, size_t size, bool png,
                           unsigned *width, unsigned *height);

/// 源图取样窗口，按「填满目标再居中裁剪」计算。
/// 长宽比不同时按长边铺满、裁掉多余的一边，绝不拉伸。
/// Source sampling window computed as cover-then-centre-crop: the longer side fills the target
/// and the excess on the other axis is cropped, so a mismatched aspect ratio is never stretched.
typedef struct {
    unsigned x, y, width, height;
} book_crop_t;

book_crop_t book_cover_crop(unsigned src_width, unsigned src_height,
                            unsigned dst_width, unsigned dst_height);

/// 优先读取书内封面，缺少或无法解码时按书籍标识生成并缓存灰阶封面。
/// Prefer a valid embedded cover; otherwise generate and cache a deterministic grayscale cover.
bool book_cover_load_gray(const char *path, const char *title, const char *author,
                          uint8_t out[BOOK_COVER_W * BOOK_COVER_H],
                          bool allow_decode, bool *pending);
