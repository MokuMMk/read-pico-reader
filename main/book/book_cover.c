/* SPDX-License-Identifier: Apache-2.0 */
#include "book_cover.h"
#include "book_cover_auto.h"
#include "book_epub.h"
#include "book_title.h"
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "JPEGDEC.h"
#include "png.h"

static void *cover_alloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static uint8_t luminance(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)(((unsigned)r * 77 + (unsigned)g * 150 + (unsigned)b * 29) >> 8);
}

/* ---- JPEG 帧头 / JPEG frame header ---- */

// 读 SOF 标记与帧尺寸，只走标记段、不解码。
// 0xC0 是基线，0xC2 是渐进式；ROM TJpgDec 只解基线，所以渐进式要交给 JPEGDEC。
// Walk the marker segments and read the SOF marker plus the frame size. 0xC0 is baseline and
// 0xC2 is progressive; the ROM TJpgDec decodes baseline only, so progressive frames go to
// JPEGDEC.
static bool jpeg_frame(const uint8_t *data, size_t size, uint8_t *sof,
                       unsigned *width, unsigned *height) {
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    size_t at = 2;
    while (at + 1 < size) {
        if (data[at] != 0xFF) { ++at; continue; }   // 段间的填充字节 / fill bytes between segments
        uint8_t marker = data[at + 1];
        if (marker == 0xFF) { ++at; continue; }
        // 无长度字段的标记。/ Markers without a length field.
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) { at += 2; continue; }
        if (at + 4 > size) return false;
        size_t length = ((size_t)data[at + 2] << 8) | data[at + 3];
        if (length < 2) return false;
        // SOF0..SOF15，跳过 DHT(0xC4)、JPG(0xC8)、DAC(0xCC)。
        // SOF0..SOF15, skipping DHT (0xC4), JPG (0xC8) and DAC (0xCC).
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (at + 9 > size) return false;
            if (height) *height = ((unsigned)data[at + 5] << 8) | data[at + 6];
            if (width) *width = ((unsigned)data[at + 7] << 8) | data[at + 8];
            if (sof) *sof = marker;
            return true;
        }
        at += 2 + length;
    }
    return false;
}

/* ---- 渐进式 JPEG / Progressive JPEG ---- */

// EIGHT_BIT_GRAYSCALE 下 pPixels 每像素一个字节。JPEGDEC 按 MCU 块回调：块内 stride 是
// iWidth，只有 iWidthUsed 列有效，坐标落在 JPEGDEC 缩放后的空间里。
// Under EIGHT_BIT_GRAYSCALE pPixels holds one byte per pixel. JPEGDEC calls back per MCU
// block: the stride inside a block is iWidth, only iWidthUsed columns are valid, and the
// coordinates live in JPEGDEC's reduced space.
typedef struct {
    uint8_t *plane;
    unsigned stride, height;
} jpegdec_plane_t;

static int jpegdec_collect(JPEGDRAW *draw) {
    jpegdec_plane_t *plane = draw->pUser;
    if (!plane || !plane->plane || draw->iBpp != 8 || !draw->pPixels || draw->iWidth < 1) return 0;
    const uint8_t *src = (const uint8_t *)draw->pPixels;
    const int columns = draw->iWidthUsed < draw->iWidth ? draw->iWidthUsed : draw->iWidth;
    for (int row = 0; row < draw->iHeight; ++row) {
        size_t y = (size_t)draw->y + row;
        if (draw->y + row < 0 || y >= plane->height) break;
        for (int column = 0; column < columns; ++column) {
            if (draw->x + column < 0) continue;
            size_t x = (size_t)draw->x + column;
            if (x >= plane->stride) continue;
            plane->plane[y * plane->stride + x] = src[(size_t)row * draw->iWidth + column];
        }
    }
    return 1;
}

// ESP-IDF 下 JPEGDEC 只提供 C++ 类，组件里有一层 extern "C" 包装。
// Under ESP-IDF JPEGDEC only offers the C++ class; the component wraps it with C linkage.
extern int jpegdec_gray_progressive(const uint8_t *data, int size, JPEG_DRAW_CALLBACK *draw,
                                    void *user, int *drawn_width, int *drawn_height);

// 渐进式只取 DC 扫描，JPEGDEC 因此固定输出 1/8 分辨率；平面尺寸由帧头算出，
// 与小平面一起缩到目标尺寸。对封面（176×240）来说已经足够，也避开了整幅系数数组。
// Progressive decoding reads the DC scan only, so JPEGDEC always emits 1/8 resolution. The plane
// size comes from the frame header and is resampled to the requested size: plenty for a 176x240
// cover, and it never holds the full coefficient array.
static bool jpegdec_gray(const uint8_t *data, size_t size, unsigned frame_width,
                         unsigned frame_height, unsigned out_width, unsigned out_height,
                         uint8_t *out) {
    if (!data || size < 4 || size > INT32_MAX || !frame_width || !frame_height) return false;
    jpegdec_plane_t plane = {
        .stride = (frame_width + 7) / 8,
        .height = (frame_height + 7) / 8,
    };
    if (plane.stride > 4096 || plane.height > 4096) return false;
    plane.plane = cover_alloc((size_t)plane.stride * plane.height);
    if (!plane.plane) return false;
    memset(plane.plane, 0xFF, (size_t)plane.stride * plane.height);
    const bool decoded = jpegdec_gray_progressive(data, (int)size, jpegdec_collect, &plane,
                                                 NULL, NULL) != 0;
    bool ok = false;
    if (decoded) {
        for (unsigned y = 0; y < out_height; ++y) {
            unsigned sy = (uint64_t)y * plane.height / out_height;
            for (unsigned x = 0; x < out_width; ++x) {
                unsigned sx = (uint64_t)x * plane.stride / out_width;
                out[y * out_width + x] = plane.plane[(size_t)sy * plane.stride + sx];
            }
        }
        ok = true;
    }
    free(plane.plane);
    return ok;
}

static bool jpeg_gray(const uint8_t *data, size_t size, unsigned out_width,
                      unsigned out_height, uint8_t *out) {
    if (size > UINT32_MAX) return false;
    uint8_t sof = 0;
    unsigned frame_width = 0, frame_height = 0;
    if (jpeg_frame(data, size, &sof, &frame_width, &frame_height) && sof == 0xC2)
        return jpegdec_gray(data, size, frame_width, frame_height, out_width, out_height, out);
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
        unsigned frame_width = 0, frame_height = 0;
        // 帧头对基线、渐进式都能给出尺寸，而 esp_jpeg 只认基线。
        // The frame header yields the size for baseline and progressive alike, where esp_jpeg
        // only handles baseline.
        if (!jpeg_frame(data, size, NULL, &frame_width, &frame_height)) return false;
        *width = frame_width; *height = frame_height;
    }
    return *width > 0 && *height > 0 && *width <= 8192 && *height <= 8192;
}

/* ---- 共享封面缓存 / Shared cover cache ---- */

#define COVER_CACHE_MAGIC UINT32_C(0x52435041)
#define COVER_CACHE_SCHEMA 1u

typedef struct {
    uint32_t magic, schema, template_version, width, height;
    uint64_t file_size;
    int64_t modified;
    uint64_t path_hash, text_hash;
} cover_cache_header_t;

static uint64_t cover_hash(uint64_t hash, const char *text) {
    if (!text) text = "";
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return (hash ^ 0xffu) * UINT64_C(1099511628211);
}

static bool cover_cache_key(const char *source, const char *title, const char *author,
                            char *path, size_t cap, cover_cache_header_t *header) {
    struct stat st;
    if (stat(source, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) return false;
    uint64_t path_hash = cover_hash(UINT64_C(14695981039346656037), source);
    uint64_t text_hash = cover_hash(cover_hash(UINT64_C(14695981039346656037), title), author);
    *header = (cover_cache_header_t){
        .magic = COVER_CACHE_MAGIC, .schema = COVER_CACHE_SCHEMA,
        .template_version = BOOK_AUTO_COVER_VERSION,
        .width = BOOK_COVER_W, .height = BOOK_COVER_H,
        .file_size = (uint64_t)st.st_size, .modified = (int64_t)st.st_mtime,
        .path_hash = path_hash, .text_hash = text_hash,
    };
    return snprintf(path, cap, "/sdcard/.readpico/covers/%016llx-%ux%u.bin",
                    (unsigned long long)path_hash, BOOK_COVER_W, BOOK_COVER_H) < (int)cap;
}

static bool cover_cache_read(const char *path, const cover_cache_header_t *expected,
                             uint8_t out[BOOK_COVER_W * BOOK_COVER_H]) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    cover_cache_header_t found;
    bool ok = fread(&found, 1, sizeof(found), file) == sizeof(found) &&
              found.magic == expected->magic && found.schema == expected->schema &&
              found.template_version == expected->template_version &&
              found.width == expected->width && found.height == expected->height &&
              found.file_size == expected->file_size && found.modified == expected->modified &&
              found.path_hash == expected->path_hash && found.text_hash == expected->text_hash &&
              fread(out, 1, BOOK_COVER_W * BOOK_COVER_H, file) == BOOK_COVER_W * BOOK_COVER_H &&
              fgetc(file) == EOF;
    fclose(file);
    return ok;
}

static void cover_cache_write(const char *path, const cover_cache_header_t *header,
                              const uint8_t out[BOOK_COVER_W * BOOK_COVER_H]) {
    if (mkdir("/sdcard/.readpico", 0777) && errno != EEXIST) return;
    if (mkdir("/sdcard/.readpico/covers", 0777) && errno != EEXIST) return;
    char temp[128];
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return;
    FILE *file = fopen(temp, "wb");
    if (!file) return;
    bool ok = fwrite(header, 1, sizeof(*header), file) == sizeof(*header) &&
              fwrite(out, 1, BOOK_COVER_W * BOOK_COVER_H, file) == BOOK_COVER_W * BOOK_COVER_H;
    if (fclose(file)) ok = false;
    if (ok) {
        remove(path);
        if (!rename(temp, path)) return;
    }
    remove(temp);
}

bool book_cover_load_gray(const char *path, const char *title, const char *author,
                          uint8_t out[BOOK_COVER_W * BOOK_COVER_H],
                          bool allow_decode, bool *pending) {
    if (pending) *pending = false;
    if (!path || !*path || !out) return false;
    char canonical_title[256];
    if (book_title_from_path(path, canonical_title, sizeof(canonical_title))) title = canonical_title;
    else if (!title || !*title) return false;
    char cache_path[120];
    cover_cache_header_t key;
    bool cacheable = cover_cache_key(path, title, author, cache_path, sizeof(cache_path), &key);
    if (!cacheable) return false;
    if (cover_cache_read(cache_path, &key, out)) return true;
    if (!allow_decode) { if (pending) *pending = true; return false; }

    bool good = false;
    bool cache_result = true;
    const char *ext = strrchr(path, '.');
    if (ext && !strcasecmp(ext, ".epub")) {
        uint8_t *data = NULL;
        size_t size = 0;
        bool png = false;
        esp_err_t err = book_epub_cover(path, &data, &size, &png);
        if (err == ESP_OK) good = book_cover_thumbnail(data, size, png, out);
        free(data);
        if (err == ESP_ERR_NO_MEM) cache_result = false;
    }
    if (!good) good = book_auto_cover_render(path, title, author, BOOK_COVER_W, BOOK_COVER_H, out);
    if (good && cache_result) cover_cache_write(cache_path, &key, out);
    return good;
}
