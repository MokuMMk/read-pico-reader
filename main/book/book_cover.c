/* SPDX-License-Identifier: Apache-2.0 */
#include "book_cover.h"
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "png.h"

static void *cover_alloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static uint8_t luminance(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)(((unsigned)r * 77 + (unsigned)g * 150 + (unsigned)b * 29) >> 8);
}

static bool jpeg_gray(const uint8_t *data, size_t size, unsigned out_width,
                      unsigned out_height, uint8_t *out) {
    if (size > UINT32_MAX) return false;
    esp_jpeg_image_cfg_t cfg = { .indata = (uint8_t *)data, .indata_size = (uint32_t)size,
                                 .out_format = JPEG_IMAGE_FORMAT_RGB565 };
    esp_jpeg_image_output_t info = {0};
    size_t limit = out_width <= BOOK_COVER_W && out_height <= BOOK_COVER_H
        ? 3u * 1024u * 1024u : 6u * 1024u * 1024u;
    bool decoded = false;
    for (int scale = JPEG_IMAGE_SCALE_0; scale <= JPEG_IMAGE_SCALE_1_8; ++scale) {
        cfg.out_scale = scale;
        if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return false;
        if (!info.width || !info.height || !info.output_len || info.output_len > limit) continue;
        uint8_t *pixels = cover_alloc(info.output_len);
        if (!pixels) continue;
        cfg.outbuf = pixels;
        cfg.outbuf_size = info.output_len;
        if (esp_jpeg_decode(&cfg, &info) == ESP_OK) {
            for (unsigned y = 0; y < out_height; ++y) {
                unsigned sy = (uint64_t)y * info.height / out_height;
                for (unsigned x = 0; x < out_width; ++x) {
                    unsigned sx = (uint64_t)x * info.width / out_width;
                    const uint8_t *p = pixels + ((size_t)sy * info.width + sx) * 2;
                    uint16_t color = (uint16_t)p[0] | (uint16_t)p[1] << 8;
                    uint8_t r = (uint8_t)((color >> 11) & 31);
                    uint8_t g = (uint8_t)((color >> 5) & 63);
                    uint8_t b = (uint8_t)(color & 31);
                    out[y * out_width + x] = luminance((uint8_t)((r << 3) | (r >> 2)),
                                                        (uint8_t)((g << 2) | (g >> 4)),
                                                        (uint8_t)((b << 3) | (b >> 2)));
                }
            }
            decoded = true;
        }
        free(pixels);
        if (decoded) break;
    }
    return decoded;
}

typedef struct { const uint8_t *data; size_t size, pos; } png_input_t;
static void png_read_mem(png_structp png, png_bytep dst, png_size_t count) {
    png_input_t *input = png_get_io_ptr(png);
    if (count > input->size - input->pos) png_error(png, "truncated PNG");
    memcpy(dst, input->data + input->pos, count);
    input->pos += count;
}
static bool png_gray(const uint8_t *data, size_t size, unsigned out_width,
                     unsigned out_height, uint8_t *out) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return false; }
    uint8_t *volatile row = NULL;
    png_input_t input = {data, size, 0};
    bool ok = false;
    if (setjmp(png_jmpbuf(png))) goto done;
    png_set_read_fn(png, &input, png_read_mem);
    png_read_info(png, info);
    png_uint_32 width = png_get_image_width(png, info), height = png_get_image_height(png, info);
    if (!width || !height || width > 8192 || height > 8192) goto done;
    int color = png_get_color_type(png, info), depth = png_get_bit_depth(png, info);
    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    png_read_update_info(png, info);
    png_size_t stride = png_get_rowbytes(png, info);
    if (stride < width * 4 || stride > 65536) goto done;
    row = cover_alloc(stride);
    if (!row) goto done;
    unsigned next_y = 0;
    for (png_uint_32 y = 0; y < height; ++y) {
        png_read_row(png, row, NULL);
        while (next_y < out_height && (uint64_t)next_y * height / out_height == y) {
            for (unsigned x = 0; x < out_width; ++x) {
                unsigned sx = (uint64_t)x * width / out_width;
                const uint8_t *p = row + sx * 4;
                uint8_t gray = luminance(p[0], p[1], p[2]);
                out[next_y * out_width + x] = (uint8_t)(((unsigned)gray * p[3] + 255u * (255u - p[3])) / 255u);
            }
            ++next_y;
        }
    }
    ok = next_y == out_height;
done:
    free((void *)row);
    png_destroy_read_struct(&png, &info, NULL);
    return ok;
}

bool book_cover_thumbnail(const uint8_t *data, size_t size, bool png,
                          uint8_t out[BOOK_COVER_W * BOOK_COVER_H]) {
    return book_image_grayscale(data, size, png, BOOK_COVER_W, BOOK_COVER_H, out);
}

bool book_image_grayscale(const uint8_t *data, size_t size, bool png,
                          unsigned out_width, unsigned out_height, uint8_t *out) {
    if (!data || !size || !out || !out_width || !out_height ||
        out_width > 1216 || out_height > 1216 || (size_t)out_width * out_height > 684u * 1216u)
        return false;
    return png ? png_gray(data, size, out_width, out_height, out)
               : jpeg_gray(data, size, out_width, out_height, out);
}

bool book_image_dimensions(const uint8_t *data, size_t size, bool png,
                           unsigned *width, unsigned *height) {
    if (!data || !width || !height) return false;
    *width = *height = 0;
    if (png) {
        static const uint8_t signature[8] = {137,80,78,71,13,10,26,10};
        if (size < 24 || memcmp(data, signature, 8) || memcmp(data + 12, "IHDR", 4)) return false;
        *width = (uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 | (uint32_t)data[18] << 8 | data[19];
        *height = (uint32_t)data[20] << 24 | (uint32_t)data[21] << 16 | (uint32_t)data[22] << 8 | data[23];
    } else {
        if (size > UINT32_MAX) return false;
        esp_jpeg_image_cfg_t cfg = {.indata = (uint8_t *)data, .indata_size = (uint32_t)size,
                                    .out_format = JPEG_IMAGE_FORMAT_RGB888};
        esp_jpeg_image_output_t info = {0};
        if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return false;
        *width = info.width; *height = info.height;
    }
    return *width > 0 && *height > 0 && *width <= 8192 && *height <= 8192;
}
