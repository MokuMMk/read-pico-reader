/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：借鉴阅读演示的逐码点折行，建立 PSRAM 页表并绘制章节。
 * English: Adapt the reading demo's codepoint wrapping into PSRAM chapter pagination.
 *
 * 冻结：原文由调用方持有；字体测量和绘制必须由调用方串行化。
 * Frozen: Caller owns source text and serializes all font measurement and drawing.
 */
#include "book_layout.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "ttf_font.h"

#define PAGE_MAX 4096u
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const char* s_text;
static size_t s_len;
static size_t* s_pages;
static size_t s_count;
static size_t s_capacity;
static char* s_line;
static EpdRect s_rect;
static int s_px;
static unsigned s_line_percent = 150, s_paragraph_percent = 50;
static int s_tracking_px;
static unsigned s_first_line_indent_em = 2;
static unsigned s_reading_line;
static int s_reading_line_offset;
static size_t s_lead_skip;
static unsigned s_lead_height;

static const blk_t* s_blocks;
static size_t s_block_count;

// 块表是有序字节区间，二分查找当前行样式。/ Blocks are ordered byte ranges; binary-search the line style.
static const blk_t* block_at(size_t off) {
    if (!s_block_count) return NULL;
    size_t lo = 0, hi = s_block_count;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_blocks[mid].offset <= off) lo = mid;
        else hi = mid;
    }
    return &s_blocks[lo];
}
// 拒绝截断、过长编码、代理项和嵌入零字节。/ Reject truncation, overlong encodings, surrogates and embedded NUL.
static size_t codepoint_size(const char* text, size_t remaining) {
    if (!remaining) return 0;
    const unsigned char* p = (const unsigned char*)text;
    if (p[0] > 0 && p[0] < 0x80) return 1;
    size_t n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 :
               p[0] >= 0xe0 && p[0] <= 0xef ? 3 :
               p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || n > remaining) return 0;
    for (size_t i = 1; i < n; i++) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
        (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90)) return 0;
    return n;
}

static uint32_t codepoint_value(const char* text, size_t n) {
    const unsigned char* p = (const unsigned char*)text;
    if (n == 1) return p[0];
    uint32_t cp = p[0] & (0x7f >> n);
    for (size_t i = 1; i < n; ++i) cp = (cp << 6) | (p[i] & 63);
    return cp;
}

// 中文排版禁则：右标点不得出现在行首，左标点不得停在行尾。
// CJK kinsoku: closing punctuation may not start a line; opening punctuation may not end one.
static bool prohibited_line_start(uint32_t cp) {
    switch (cp) {
        case 0x0021: case 0x0025: case 0x0029: case 0x002c: case 0x002e: case 0x003a:
        case 0x003b: case 0x003f: case 0x005d: case 0x007d:
        case 0x2019: case 0x201d: case 0x2026: case 0x3001: case 0x3002: case 0x3009:
        case 0x300b: case 0x300d: case 0x300f: case 0x3011: case 0x3015: case 0x3017:
        case 0x3019: case 0x301b: case 0xff01: case 0xff05: case 0xff09: case 0xff0c:
        case 0xff0e: case 0xff1a: case 0xff1b: case 0xff1f: case 0xff3d: case 0xff5d:
            return true;
        default: return false;
    }
}

static bool prohibited_line_end(uint32_t cp) {
    switch (cp) {
        case 0x0028: case 0x005b: case 0x007b: case 0x2018: case 0x201c: case 0x3008:
        case 0x300a: case 0x300c: case 0x300e: case 0x3010: case 0x3014: case 0x3016:
        case 0x3018: case 0x301a: case 0xff08: case 0xff3b: case 0xff5b:
            return true;
        default: return false;
    }
}

void book_layout_free(void) {
    free(s_pages);
    free(s_line);
    s_pages = NULL;
    s_line = NULL;
    s_text = NULL;
    s_count = s_capacity = s_len = 0;
    s_px = 0;
    s_blocks = NULL;
    s_block_count = 0;
}

void book_layout_set_spacing(unsigned line_percent, unsigned paragraph_percent) {
    s_line_percent = line_percent >= 110 && line_percent <= 200 ? line_percent : 150;
    s_paragraph_percent = paragraph_percent <= 100 ? paragraph_percent : 50;
}
void book_layout_set_typography(int tracking_px) {
    s_tracking_px = tracking_px >= -4 && tracking_px <= 4 && tracking_px % 2 == 0 ? tracking_px : 0;
}
void book_layout_set_first_line_indent(unsigned em) {
    s_first_line_indent_em = em <= 3 ? em : 2;
}
EpdRect book_layout_balanced_rect(EpdRect outer, int px, int tracking_px) {
    if (px <= 0 || outer.width < px || tracking_px < -4 || tracking_px > 4) return outer;
    int step = px + tracking_px;
    if (step <= 0) return outer;
    int columns = 1 + (outer.width - px) / step;
    int used = px + (columns - 1) * step;
    outer.x += (outer.width - used) / 2;
    outer.width = used;
    return outer;
}
void book_layout_set_reading_line(unsigned style) {
    s_reading_line = style <= 2 ? style : 0;
}
void book_layout_set_reading_line_offset(int offset_px) {
    s_reading_line_offset = offset_px < -8 ? -8 : offset_px > 8 ? 8 : offset_px;
}
void book_layout_set_chapter_lead(size_t skip_bytes, unsigned height_px) {
    s_lead_skip = skip_bytes;
    s_lead_height = height_px;
}

int book_layout_page_image(size_t page) {
    if (page >= s_count || !s_block_count) return -1;
    const blk_t* block = block_at(s_pages[page]);
    return block && block->offset == s_pages[page] ? block->image : -1;
}

static int line_height_for(int px) { return (int)((unsigned)px * s_line_percent / 100); }
static int gap_for(int height, bool heading) {
    return (int)((unsigned)height * s_paragraph_percent / (heading ? 150 : 100));
}

static bool append_page(size_t off) {
    if (s_count == PAGE_MAX) return false;
    if (s_count == s_capacity) {
        size_t cap = s_capacity ? s_capacity * 2 : 16;
        size_t* pages = heap_caps_realloc(s_pages, cap * sizeof(*pages), PSRAM_CAPS);
        if (!pages) return false;
        s_pages = pages;
        s_capacity = cap;
    }
    s_pages[s_count++] = off;
    return true;
}

// 折行时保留原文字节位置；CRLF 算一个段落边界。/ Preserve source offsets while wrapping; CRLF is one paragraph boundary.
static bool take_line(size_t off, size_t* next, bool* paragraph_end, int* px, bool* heading,
                      int* line_width, int* indent, uint8_t* align,
                      int* margin_before, int* margin_after) {
    const blk_t* block = block_at(off);
    *heading = block && block->heading;
    *px = s_px + (*heading ? 8 : 0);
    bool first_line = block ? off == block->offset :
        off == 0 || s_text[off - 1] == '\n' || s_text[off - 1] == '\r';
    *align = block ? block->align : 0;
    // 首行缩进由阅读设置统一控制；书内标题和对齐块仍保持原本的位置。
    // The reader setting controls paragraph indent; headings and aligned blocks keep their placement.
    *indent = first_line && block ? (int)((unsigned)*px * block->indent_percent / 100) : 0;
    if (first_line && !*heading && !*align) *indent = *px * (int)s_first_line_indent_em;
    if (*indent >= s_rect.width) *indent = s_rect.width > 1 ? s_rect.width - 1 : 0;
    if (*indent + *px > s_rect.width) *indent = 0;
    *margin_before = first_line && block ? (int)((unsigned)*px * block->margin_before_percent / 100) : 0;
    *margin_after = block ? (int)((unsigned)*px * block->margin_after_percent / 100) : 0;
    int available = s_rect.width - *indent;
    size_t limit = block ? block->offset + block->len : s_len;
    size_t end = off;
    size_t last_start = off;
    uint32_t last_cp = 0;
    int64_t width = 0;
    int64_t last_width = 0;
    s_line[0] = 0;
    *paragraph_end = false;
    while (end < limit && s_text[end] != '\r' && s_text[end] != '\n') {
        size_t n = codepoint_size(s_text + end, s_len - end);
        if (!n) return false;
        uint32_t cp = codepoint_value(s_text + end, n);
        // 字体逐字取整后累加 advance；单字测量避免反复扫描整行前缀。
        // Font advances are rounded per glyph and summed; measure each glyph once instead of every prefix.
        char glyph[5];
        memcpy(glyph, s_text + end, n);
        glyph[n] = 0;
        int64_t candidate = width + ttf_text_width_px(*px, glyph) +
                            (end > off && !*heading ? s_tracking_px : 0);
        if (candidate < 0) return false;
        if (candidate > available) {
            if (end == off) return false;
            // 右标点与前一字一起移到下一行，避免把标点悬挂到右侧留白。
            // Move a closing mark with the preceding glyph rather than hanging it into the right margin.
            if (prohibited_line_start(cp)) {
                if (last_start > off) {
                    end = last_start;
                    s_line[end - off] = 0;
                    width = last_width;
                    break;
                }
                // 极窄行容不下两个字时才保留悬挂，避免出现以标点开头的死循环。
                // Only a one-glyph-wide line may hang punctuation to avoid a non-progressing wrap.
                if (candidate <= (int64_t)available + *px * 2) {
                    memcpy(s_line + end - off, glyph, n);
                    s_line[end - off + n] = 0;
                    width = candidate;
                    end += n;
                    break;
                }
            }
            // 若最后一个字是左括号，将它回退到下一行；极窄行则让括号和首字成组悬挂。
            // Move a trailing opener to the next line; on a one-glyph line keep the pair together.
            if (prohibited_line_end(last_cp)) {
                if (last_start > off) {
                    end = last_start;
                    s_line[end - off] = 0;
                    width = last_width;
                } else if (candidate <= (int64_t)available + *px * 2) {
                    memcpy(s_line + end - off, glyph, n);
                    s_line[end - off + n] = 0;
                    width = candidate;
                    end += n;
                }
            }
            break;
        }
        last_width = width;
        width = candidate;
        last_start = end;
        last_cp = cp;
        memcpy(s_line + end - off, glyph, n);
        s_line[end - off + n] = 0;
        end += n;
    }
    *line_width = (int)width;
    *next = end;
    if (end < s_len && (s_text[end] == '\r' || s_text[end] == '\n')) {
        *next = end + 1;
        if (s_text[end] == '\r' && *next < s_len && s_text[*next] == '\n') (*next)++;
        *paragraph_end = true;
    }
    return *next > off;
}

bool book_layout_build(const char* utf8, size_t len, EpdRect rect, int px) {
    return book_layout_build_blocks(utf8, len, NULL, 0, rect, px);
}

bool book_layout_build_blocks(const char* utf8, size_t len, const blk_t* blocks, size_t count, EpdRect rect, int px) {
    book_layout_free();
    if ((!utf8 && len) || len == SIZE_MAX || s_lead_skip > len || s_lead_height >= (unsigned)rect.height ||
        px <= 0 || px > INT_MAX / 3 ||
        rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0 ||
        rect.x > INT_MAX - rect.width || rect.y > INT_MAX - rect.height) return false;
    int line_height = line_height_for(px);
    if (line_height > rect.height) return false;
    for (size_t off = 0; off < len;) {
        size_t n = codepoint_size(utf8 + off, len - off);
        if (!n) return false;
        off += n;
    }
    if (count) {
        if (!blocks || count > HTML_TEXT_MAX_BLOCKS) return false;
        size_t expected = 0;
        for (size_t i = 0; i < count; ++i) {
            const blk_t* b = &blocks[i];
            if (b->offset != expected || b->offset >= len || !b->len || b->len > len - b->offset ||
                ((unsigned char)utf8[b->offset] & 0xc0) == 0x80) return false;
            size_t end = b->offset + b->len;
            if (i + 1 < count) {
                if (end >= len || utf8[end] != '\n') return false;
                expected = end + 1;
            } else if (end != len) return false;
        }
    }
    s_blocks = blocks;
    s_block_count = count;
    s_text = utf8;
    s_len = len;
    s_px = px;
    s_rect = rect;
    s_line = heap_caps_malloc(len + 1, PSRAM_CAPS);
    if (!s_line || !append_page(s_lead_skip)) goto fail;
    size_t off = s_lead_skip;
    int64_t used = s_lead_height;
    while (off < len) {
        const blk_t* block = block_at(off);
        if (block && block->image >= 0 && off == block->offset) {
            if (used && !append_page(off)) goto fail;
            used = rect.height;
            off = block->offset + block->len;
            if (off < len && utf8[off] == '\n') ++off;
            continue;
        }
        size_t next;
        bool paragraph_end, heading;
        int line_px, line_width, indent, margin_before, margin_after;
        uint8_t align;
        if (!take_line(off, &next, &paragraph_end, &line_px, &heading,
                       &line_width, &indent, &align, &margin_before, &margin_after)) goto fail;
        (void)line_width; (void)indent; (void)align;
        line_height = line_height_for(line_px);
        if (line_height > rect.height) goto fail;
        int leading = used ? margin_before : 0;
        if (used + leading + line_height > rect.height) {
            if (!append_page(off)) goto fail;
            used = 0;
            leading = 0;
        }
        used += leading + line_height;
        if (paragraph_end) used += gap_for(line_height, heading) + margin_after;
        off = next;
    }
    return true;
fail:
    book_layout_free();
    return false;
}

size_t book_layout_page_count(void) { return s_count; }

size_t book_layout_page_start_offset(size_t page) {
    return page < s_count ? s_pages[page] : s_len;
}

size_t book_layout_page_for_offset(size_t off) {
    size_t lo = 0, hi = s_count;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_pages[mid] <= off) lo = mid;
        else hi = mid;
    }
    return lo;
}

void book_layout_draw_page(uint8_t* fb, size_t page, EpdRect rect, int px) {
    if (!fb || page >= s_count || px != s_px || rect.width != s_rect.width ||
        rect.height != s_rect.height || rect.x < 0 || rect.y < 0 ||
        rect.x > INT_MAX - rect.width || rect.y > INT_MAX - rect.height) return;
    size_t off = s_pages[page];
    if (book_layout_page_image(page) >= 0) return;
    size_t end = page + 1 < s_count ? s_pages[page + 1] : s_len;
    int64_t used = page == 0 ? s_lead_height : 0;
    while (off < end) {
        size_t next;
        bool paragraph_end, heading;
        int line_px, line_width, indent, margin_before, margin_after;
        uint8_t align;
        if (!take_line(off, &next, &paragraph_end, &line_px, &heading,
                       &line_width, &indent, &align, &margin_before, &margin_after)) return;
        int line_height = line_height_for(line_px);
        int leading = used ? margin_before : 0;
        if (used + leading + line_height > rect.height) return;
        used += leading;
        if (s_line[0]) {
            if (s_reading_line) {
                int guide_y = rect.y + (int)used + line_px + (line_height - line_px) / 2 +
                              s_reading_line_offset;
                int dash = s_reading_line == 1 ? 19 : 2;
                int period = s_reading_line == 1 ? 31 : 13;
                for (int dx = 0; dx < rect.width; dx += period) {
                    int width = dx + dash <= rect.width ? dash : rect.width - dx;
                    epd_fill_rect((EpdRect){rect.x + dx, guide_y, width, 2}, 0x50, fb);
                }
            }
            int x = rect.x + indent;
            int available = rect.width - indent;
            if (align == 1) x += (available - line_width) / 2;
            else if (align == 2) x += available - line_width;
            if (s_tracking_px && !heading)
                ttf_draw_text_px_spaced(fb, x, rect.y + (int)used + ttf_ascender_px(line_px),
                                        line_px, s_line, s_tracking_px, 0, 15);
            else
                ttf_draw_text_px(fb, x, rect.y + (int)used + ttf_ascender_px(line_px), line_px,
                                 s_line, EPD_DRAW_ALIGN_LEFT, 0, 15);
        }
        used += line_height;
        if (paragraph_end) used += gap_for(line_height, heading) + margin_after;
        off = next;
    }
}
