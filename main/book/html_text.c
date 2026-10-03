/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：单遍提取章节文字，折叠空白并保留非空块和标题标记。
 * English: Extract chapter text in one pass, collapsing whitespace and preserving nonempty blocks and headings.
 *
 * 冻结：不执行脚本、不加载资源；输出有界，失败释放全部临时分配。
 * Frozen: Never execute scripts or load resources; bound output and release temporary allocations on failure.
 */
#include "html_text.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define HTML_IMAGE_MAX 256u
#define HTML_IMAGE_PATH_MAX 511u
#define CSS_RULE_MAX 64u

enum {
    CSS_ALIGN = 1u << 0,
    CSS_INDENT = 1u << 1,
    CSS_BEFORE = 1u << 2,
    CSS_AFTER = 1u << 3,
};

typedef struct {
    uint8_t mask, align, indent, before, after;
} css_style_t;

typedef struct {
    char selector[32];
    css_style_t style;
} css_rule_t;

typedef struct {
    html_text_t text;
    size_t text_cap, block_cap, start;
    bool active, heading, block_heading, space;
    css_style_t current_style, block_style;
    css_rule_t rules[CSS_RULE_MAX];
    size_t rule_count;
} writer_t;

static unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool name_char(unsigned char c) {
    c = lower(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '-' || c == '_';
}

static bool name_equal(const char* name, const char* expected) {
    return strcmp(name, expected) == 0;
}

static const char* bounded_case_find(const char* at, const char* end, const char* needle) {
    size_t n = strlen(needle);
    if (!n || (size_t)(end - at) < n) return NULL;
    for (; at + n <= end; ++at) if (!strncasecmp(at, needle, n)) return at;
    return NULL;
}

static void css_apply(css_style_t* dst, const css_style_t* src) {
    if (src->mask & CSS_ALIGN) dst->align = src->align;
    if (src->mask & CSS_INDENT) dst->indent = src->indent;
    if (src->mask & CSS_BEFORE) dst->before = src->before;
    if (src->mask & CSS_AFTER) dst->after = src->after;
    dst->mask |= src->mask;
}

static uint8_t css_length_percent(const char* value, size_t len) {
    while (len && ascii_space((unsigned char)*value)) { ++value; --len; }
    char number[20];
    size_t n = 0;
    while (n < len && n + 1 < sizeof(number) &&
           ((value[n] >= '0' && value[n] <= '9') || value[n] == '.')) {
        number[n] = value[n]; ++n;
    }
    number[n] = 0;
    if (!n) return 0;
    double amount = strtod(number, NULL), percent = amount * 100.0;
    const char* unit = value + n;
    size_t units = len - n;
    while (units && ascii_space((unsigned char)*unit)) { ++unit; --units; }
    if (units >= 2 && !strncasecmp(unit, "px", 2)) percent = amount * 100.0 / 16.0;
    else if (units >= 1 && *unit == '%') percent = amount;
    if (percent < 0) percent = 0;
    if (percent > 250) percent = 250;
    return (uint8_t)(percent + 0.5);
}

static void css_declarations(const char* at, const char* end, css_style_t* style) {
    while (at < end) {
        while (at < end && (ascii_space((unsigned char)*at) || *at == ';')) ++at;
        const char* key = at;
        while (at < end && *at != ':' && *at != ';') ++at;
        if (at == end || *at != ':') { while (at < end && *at++ != ';') {} continue; }
        const char* key_end = at++;
        while (key_end > key && ascii_space((unsigned char)key_end[-1])) --key_end;
        while (at < end && ascii_space((unsigned char)*at)) ++at;
        const char* value = at;
        while (at < end && *at != ';') ++at;
        const char* value_end = at;
        while (value_end > value && ascii_space((unsigned char)value_end[-1])) --value_end;
        size_t kn = (size_t)(key_end - key), vn = (size_t)(value_end - value);
        if (kn == 10 && !strncasecmp(key, "text-align", 10)) {
            style->align = vn == 6 && !strncasecmp(value, "center", 6) ? 1 :
                           vn == 5 && !strncasecmp(value, "right", 5) ? 2 : 0;
            style->mask |= CSS_ALIGN;
        } else if (kn == 11 && !strncasecmp(key, "text-indent", 11)) {
            style->indent = css_length_percent(value, vn); style->mask |= CSS_INDENT;
        } else if (kn == 10 && !strncasecmp(key, "margin-top", 10)) {
            style->before = css_length_percent(value, vn); style->mask |= CSS_BEFORE;
        } else if (kn == 13 && !strncasecmp(key, "margin-bottom", 13)) {
            style->after = css_length_percent(value, vn); style->mask |= CSS_AFTER;
        }
    }
}

static void css_add_rule(writer_t* w, const char* selector, size_t len, const css_style_t* style) {
    while (len && ascii_space((unsigned char)*selector)) { ++selector; --len; }
    while (len && ascii_space((unsigned char)selector[len - 1])) --len;
    const char* last = selector;
    for (size_t i = 0; i < len; ++i) if (ascii_space((unsigned char)selector[i]) || selector[i] == '>') last = selector + i + 1;
    len -= (size_t)(last - selector); selector = last;
    const char* pseudo = memchr(selector, ':', len);
    if (pseudo) len = (size_t)(pseudo - selector);
    if (!len || len >= sizeof(w->rules[0].selector) || w->rule_count == CSS_RULE_MAX || !style->mask) return;
    css_rule_t* rule = &w->rules[w->rule_count++];
    for (size_t i = 0; i < len; ++i) rule->selector[i] = (char)lower((unsigned char)selector[i]);
    rule->selector[len] = 0;
    rule->style = *style;
}

static void css_parse_rules(writer_t* w, const char* at, const char* end) {
    while (at < end) {
        while (at < end && ascii_space((unsigned char)*at)) ++at;
        if (end - at >= 2 && at[0] == '/' && at[1] == '*') {
            const char* close = bounded_case_find(at + 2, end, "*/");
            at = close ? close + 2 : end;
            continue;
        }
        if (at < end && *at == '@') {
            const char* semi = memchr(at, ';', (size_t)(end - at));
            const char* brace = memchr(at, '{', (size_t)(end - at));
            if (semi && (!brace || semi < brace)) { at = semi + 1; continue; }
        }
        const char* open = at;
        while (open < end && *open != '{') ++open;
        if (open == end) break;
        const char* shut = memchr(open + 1, '}', (size_t)(end - open - 1));
        if (!shut) break;
        css_style_t style = {0};
        css_declarations(open + 1, shut, &style);
        const char* selector = at;
        while (selector < open) {
            const char* comma = memchr(selector, ',', (size_t)(open - selector));
            const char* selector_end = comma ? comma : open;
            css_add_rule(w, selector, (size_t)(selector_end - selector), &style);
            selector = comma ? comma + 1 : open;
        }
        at = shut + 1;
    }
}

static void css_parse_styles(writer_t* w, const char* html, size_t len) {
    const char* at = html;
    const char* end = html + len;
    while ((at = bounded_case_find(at, end, "<style"))) {
        const char* body = memchr(at, '>', (size_t)(end - at));
        if (!body) break;
        ++body;
        const char* close = bounded_case_find(body, end, "</style");
        if (!close) break;
        css_parse_rules(w, body, close);
        at = close + 7;
    }
}

static bool attr_value(const char* at, const char* end, const char* wanted,
                       const char** value_out, size_t* length_out) {
    size_t wanted_len = strlen(wanted);
    while (at < end) {
        while (at < end && (ascii_space((unsigned char)*at) || *at == '/')) ++at;
        const char* name = at;
        while (at < end && name_char((unsigned char)*at)) ++at;
        size_t n = (size_t)(at - name);
        while (at < end && ascii_space((unsigned char)*at)) ++at;
        if (at >= end || *at++ != '=') continue;
        while (at < end && ascii_space((unsigned char)*at)) ++at;
        if (at >= end) break;
        char quote = (*at == '\'' || *at == '"') ? *at++ : 0;
        const char* value = at;
        while (at < end && (quote ? *at != quote : !ascii_space((unsigned char)*at) && *at != '>')) ++at;
        size_t length = (size_t)(at - value);
        if (quote && at < end) ++at;
        if (n == wanted_len && !strncasecmp(name, wanted, n)) {
            *value_out = value; *length_out = length; return true;
        }
    }
    return false;
}

static bool class_has(const char* classes, size_t len, const char* wanted) {
    size_t n = strlen(wanted);
    for (size_t i = 0; i < len;) {
        while (i < len && ascii_space((unsigned char)classes[i])) ++i;
        size_t start = i;
        while (i < len && !ascii_space((unsigned char)classes[i])) ++i;
        if (i - start == n && !strncasecmp(classes + start, wanted, n)) return true;
    }
    return false;
}

static css_style_t style_for(writer_t* w, const char* tag, const char* attrs, const char* attrs_end) {
    css_style_t out = {0};
    const char *classes = NULL, *inline_css = NULL;
    size_t class_len = 0, inline_len = 0;
    (void)attr_value(attrs, attrs_end, "class", &classes, &class_len);
    (void)attr_value(attrs, attrs_end, "style", &inline_css, &inline_len);
    for (size_t i = 0; i < w->rule_count; ++i) {
        const char* selector = w->rules[i].selector;
        const char* dot = strchr(selector, '.');
        bool matches = !strcmp(selector, "body") || (!dot && !strcmp(selector, tag));
        if (dot) {
            size_t tag_len = (size_t)(dot - selector);
            matches = (!tag_len || (strlen(tag) == tag_len && !strncasecmp(selector, tag, tag_len))) &&
                      classes && class_has(classes, class_len, dot + 1);
        } else if (selector[0] == '.' && classes) matches = class_has(classes, class_len, selector + 1);
        if (matches) css_apply(&out, &w->rules[i].style);
    }
    if (inline_css) {
        css_style_t inline_style = {0};
        css_declarations(inline_css, inline_css + inline_len, &inline_style);
        css_apply(&out, &inline_style);
    }
    return out;
}

void html_text_free(html_text_t* text) {
    if (!text) return;
    free(text->utf8);
    free(text->blocks);
    for (size_t i = 0; i < text->image_count; ++i) free(text->images[i]);
    free(text->images);
    *text = (html_text_t){0};
}

static esp_err_t reserve_text(writer_t* w, size_t extra) {
    if (extra > HTML_TEXT_MAX_BYTES - w->text.len) return ESP_ERR_INVALID_SIZE;
    size_t need = w->text.len + extra + 1;
    if (need <= w->text_cap) return ESP_OK;
    size_t cap = w->text_cap ? w->text_cap : 256;
    while (cap < need) {
        if (cap > (HTML_TEXT_MAX_BYTES + 1) / 2) { cap = HTML_TEXT_MAX_BYTES + 1; break; }
        cap *= 2;
    }
    char* text = heap_caps_realloc(w->text.utf8, cap, PSRAM_CAPS);
    if (!text) return ESP_ERR_NO_MEM;
    w->text.utf8 = text;
    w->text_cap = cap;
    return ESP_OK;
}

static esp_err_t finish_block(writer_t* w) {
    w->space = false;
    if (!w->active) return ESP_OK;
    if (w->text.count == HTML_TEXT_MAX_BLOCKS) return ESP_ERR_INVALID_SIZE;
    if (w->text.count == w->block_cap) {
        size_t cap = w->block_cap ? w->block_cap * 2 : 32;
        if (cap > HTML_TEXT_MAX_BLOCKS) cap = HTML_TEXT_MAX_BLOCKS;
        blk_t* blocks = heap_caps_realloc(w->text.blocks, cap * sizeof(*blocks), PSRAM_CAPS);
        if (!blocks) return ESP_ERR_NO_MEM;
        w->text.blocks = blocks;
        w->block_cap = cap;
    }
    w->text.blocks[w->text.count++] = (blk_t){
        .offset = w->start, .len = w->text.len - w->start, .heading = w->block_heading, .image = -1,
        .align = w->block_style.align,
        .indent_percent = w->block_style.indent,
        .margin_before_percent = w->block_style.before,
        .margin_after_percent = w->block_style.after,
    };
    w->active = false;
    return ESP_OK;
}

static esp_err_t emit(writer_t* w, uint32_t cp) {
    if (cp == 0xa0 || (cp < 128 && ascii_space((unsigned char)cp))) {
        if (w->active) w->space = true;
        return ESP_OK;
    }
    char bytes[4];
    size_t n;
    if (cp < 0x80) { bytes[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        bytes[0] = (char)(0xc0 | (cp >> 6)); bytes[1] = (char)(0x80 | (cp & 63)); n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (char)(0xe0 | (cp >> 12)); bytes[1] = (char)(0x80 | ((cp >> 6) & 63));
        bytes[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        bytes[0] = (char)(0xf0 | (cp >> 18)); bytes[1] = (char)(0x80 | ((cp >> 12) & 63));
        bytes[2] = (char)(0x80 | ((cp >> 6) & 63)); bytes[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    bool separator = !w->active && w->text.count > 0;
    esp_err_t err = reserve_text(w, n + (separator || w->space ? 1 : 0));
    if (err != ESP_OK) return err;
    if (!w->active) {
        if (separator) w->text.utf8[w->text.len++] = '\n';
        w->start = w->text.len;
        w->active = true;
        w->block_heading = w->heading;
        w->block_style = w->current_style;
    } else if (w->space) w->text.utf8[w->text.len++] = ' ';
    w->space = false;
    memcpy(w->text.utf8 + w->text.len, bytes, n);
    w->text.len += n;
    return ESP_OK;
}

// 实体查找有固定上限；未知名称原样保留，非法数值替换为 U+FFFD。
// Bound entity lookahead; preserve unknown names and replace invalid numeric values with U+FFFD.
static size_t entity(const char* s, size_t len, uint32_t* cp) {
    size_t end = 1;
    while (end < len && end <= 32 && s[end] != ';' && !ascii_space((unsigned char)s[end]) && s[end] != '&' && s[end] != '<') end++;
    if (end >= len || end > 32 || s[end] != ';') return 0;
    static const struct { const char* name; uint32_t cp; } names[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
    };
    if (end > 2 && s[1] == '#') {
        size_t i = 2;
        unsigned base = 10;
        if (i < end && (s[i] == 'x' || s[i] == 'X')) { base = 16; i++; }
        if (i == end) return 0;
        uint32_t value = 0;
        bool overflow = false;
        for (; i < end; i++) {
            unsigned char c = lower((unsigned char)s[i]);
            unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 255;
            if (digit >= base) return 0;
            if (value > (0x10ffffu - digit) / base) overflow = true;
            else if (!overflow) value = value * base + digit;
        }
        *cp = overflow || !value || (value >= 0xd800 && value <= 0xdfff) ? 0xfffd : value;
        return end + 1;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i].name) == end - 1 && memcmp(s + 1, names[i].name, end - 1) == 0) {
            *cp = names[i].cp;
            return end + 1;
        }
    }
    return 0;
}

static size_t utf8(const char* s, size_t len, uint32_t* cp) {
    const unsigned char* p = (const unsigned char*)s;
    if (p[0] && p[0] < 0x80) { *cp = p[0]; return 1; }
    size_t n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 : p[0] >= 0xe0 && p[0] <= 0xef ? 3 : p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || n > len) return 0;
    uint32_t value = p[0] & (0x7f >> n);
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) return 0;
        value = (value << 6) | (p[i] & 63);
    }
    if ((n == 2 && value < 0x80) || (n == 3 && value < 0x800) || (n == 4 && value < 0x10000) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    *cp = value;
    return n;
}

static bool block_tag(const char* name) {
    static const char* const tags[] = {"p", "div", "li", "tr", "br", "hr", "blockquote"};
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) return true;
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) if (name_equal(name, tags[i])) return true;
    return false;
}

// 识别常见惰性图片属性及 SVG 引用，并解码路径中的 XML 实体。
// Recognize common lazy image attributes and SVG references, decoding XML entities in paths.
static bool image_attr(const char* at, const char* end, bool object,
                       char path[HTML_IMAGE_PATH_MAX + 1]) {
    static const char* const image_names[] = {
        "data-src", "data-original", "data-lazy-src", "src", "xlink:href", "href", "srcset", "data-srcset"
    };
    const char* value = NULL; size_t length = 0;
    if (object) {
        if (!attr_value(at, end, "data", &value, &length)) return false;
    } else {
        for (size_t i = 0; i < sizeof(image_names) / sizeof(image_names[0]); ++i) {
            if (attr_value(at, end, image_names[i], &value, &length) && length &&
                !(length >= 5 && !strncasecmp(value, "data:", 5))) {
                if (strstr(image_names[i], "srcset")) {
                    while (length && ascii_space((unsigned char)*value)) { ++value; --length; }
                    size_t n = 0;
                    while (n < length && !ascii_space((unsigned char)value[n]) && value[n] != ',') ++n;
                    length = n;
                }
                break;
            }
        }
    }
    if (!value || !length || length > HTML_IMAGE_PATH_MAX) return false;
    size_t used = 0;
    for (size_t i = 0; i < length;) {
        uint32_t cp = 0;
        size_t consumed = value[i] == '&' ? entity(value + i, length - i, &cp) : 0;
        if (consumed) {
            char bytes[4]; size_t count = 0;
            if (cp < 0x80) bytes[count++] = (char)cp;
            else if (cp < 0x800) {
                bytes[count++] = (char)(0xc0 | (cp >> 6));
                bytes[count++] = (char)(0x80 | (cp & 63));
            } else if (cp < 0x10000) {
                bytes[count++] = (char)(0xe0 | (cp >> 12));
                bytes[count++] = (char)(0x80 | ((cp >> 6) & 63));
                bytes[count++] = (char)(0x80 | (cp & 63));
            } else {
                bytes[count++] = (char)(0xf0 | (cp >> 18));
                bytes[count++] = (char)(0x80 | ((cp >> 12) & 63));
                bytes[count++] = (char)(0x80 | ((cp >> 6) & 63));
                bytes[count++] = (char)(0x80 | (cp & 63));
            }
            if (used + count > HTML_IMAGE_PATH_MAX) return false;
            memcpy(path + used, bytes, count); used += count; i += consumed;
        } else {
            if (used == HTML_IMAGE_PATH_MAX) return false;
            path[used++] = value[i++];
        }
    }
    path[used] = 0;
    return used != 0;
}

static esp_err_t add_image(writer_t* w, const char* path) {
    if (w->text.image_count == HTML_IMAGE_MAX) return ESP_OK;
    esp_err_t err = finish_block(w);
    if (err != ESP_OK) return err;
    char** images = heap_caps_realloc(w->text.images, (w->text.image_count + 1) * sizeof(char*), PSRAM_CAPS);
    if (!images) return ESP_ERR_NO_MEM;
    w->text.images = images;
    size_t n = strlen(path) + 1;
    char* copy = heap_caps_malloc(n, PSRAM_CAPS);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, path, n);
    w->text.images[w->text.image_count] = copy;
    int index = (int)w->text.image_count++;
    err = emit(w, 0xfffc);
    if (err != ESP_OK) return err;
    err = finish_block(w);
    if (err == ESP_OK) w->text.blocks[w->text.count - 1].image = index;
    return err;
}

esp_err_t html_to_blocks_with_css_target(const char* html, size_t len,
                                          const char* css, size_t css_len,
                                          const char* anchor, size_t source_offset,
                                          size_t* anchor_offset, html_text_t* out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (html_text_t){0};
    if (anchor_offset) *anchor_offset = 0;
    if ((!html && len) || (!css && css_len)) return ESP_ERR_INVALID_ARG;
    if (len > HTML_TEXT_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    writer_t w = {0};
    if (css_len) css_parse_rules(&w, css, css + css_len);
    if (len) css_parse_styles(&w, html, len);
    esp_err_t err = ESP_OK;
    char skip[16] = "";
    bool resume_head = false;
    size_t pos = len >= 3 && memcmp(html, "\xef\xbb\xbf", 3) == 0 ? 3 : 0;
    while (pos < len) {
        if ((!skip[0] || name_equal(skip, "head")) && len - pos >= 4 && memcmp(html + pos, "<!--", 4) == 0) {
            pos += 4;
            while (len - pos >= 3 && memcmp(html + pos, "-->", 3)) pos++;
            pos = len - pos >= 3 ? pos + 3 : len;
            continue;
        }
        if (html[pos] == '<') {
            size_t tag_offset = pos;
            size_t at = pos + 1;
            bool closing = at < len && html[at] == '/';
            if (closing) at++;
            size_t start = at;
            while (at < len && name_char((unsigned char)html[at])) at++;
            char name[16] = "";
            size_t name_len = at - start;
            if (name_len < sizeof(name)) {
                for (size_t i = 0; i < name_len; i++) name[i] = (char)lower((unsigned char)html[start + i]);
            }
            bool head_child = name_equal(skip, "head") && !closing &&
                              (name_equal(name, "script") || name_equal(name, "style"));
            if (skip[0] && !head_child && (!closing || !name_equal(name, skip))) { pos++; continue; }
            bool declaration = start < len && (html[start] == '!' || html[start] == '?');
            unsigned char initial = start < len ? lower((unsigned char)html[start]) : 0;
            if ((name_len && initial >= 'a' && initial <= 'z') || declaration) {
                char quote = 0;
                size_t end = at;
                for (; end < len; end++) {
                    char c = html[end];
                    if (quote) { if (c == quote) quote = 0; }
                    else if (c == '\'' || c == '"') quote = c;
                    else if (c == '>') break;
                }
                if (end == len) { err = ESP_ERR_INVALID_RESPONSE; goto fail; }
                bool self_closing = end > at && html[end - 1] == '/';
                pos = end + 1;
                if (skip[0]) {
                    if (head_child) {
                        if (!self_closing) { memcpy(skip, name, sizeof(skip)); resume_head = true; }
                    } else if (resume_head) {
                        memcpy(skip, "head", 5);
                        resume_head = false;
                    } else skip[0] = 0;
                    continue;
                }
                if (!closing && (name_equal(name, "img") || name_equal(name, "image") ||
                                 name_equal(name, "svg:image") || name_equal(name, "object"))) {
                    char path[HTML_IMAGE_PATH_MAX + 1];
                    if (image_attr(html + at, html + end, name_equal(name, "object"), path)) {
                        err = add_image(&w, path);
                        if (err != ESP_OK) goto fail;
                    }
                    continue;
                }
                if (!closing && !self_closing && (name_equal(name, "head") || name_equal(name, "style") || name_equal(name, "script"))) {
                    memcpy(skip, name, sizeof(skip));
                } else if (block_tag(name)) {
                    err = finish_block(&w);
                    if (err != ESP_OK) goto fail;
                    if (!closing && !self_closing) w.current_style = style_for(&w, name, html + at, html + end);
                    else w.current_style = (css_style_t){0};
                    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '3' && !name[2]) w.heading = !closing && !self_closing;
                }
                if (!closing && !skip[0] && anchor_offset) {
                    const char *value = NULL; size_t length = 0;
                    bool matched = tag_offset == source_offset;
                    if (!matched && anchor && *anchor &&
                        (attr_value(html + at, html + end, "id", &value, &length) ||
                         attr_value(html + at, html + end, "xml:id", &value, &length) ||
                         attr_value(html + at, html + end, "name", &value, &length)) &&
                        length == strlen(anchor) && !memcmp(value, anchor, length)) matched = true;
                    if (matched)
                        *anchor_offset = w.text.len + (!w.active && w.text.count ? 1 : 0);
                }
                continue;
            }
        }
        if (skip[0]) { pos++; continue; }
        uint32_t cp;
        size_t n = html[pos] == '&' ? entity(html + pos, len - pos, &cp) : 0;
        if (!n) n = utf8(html + pos, len - pos, &cp);
        if (!n) { err = ESP_ERR_INVALID_RESPONSE; goto fail; }
        err = emit(&w, cp);
        if (err != ESP_OK) goto fail;
        pos += n;
    }
    err = finish_block(&w);
    if (err == ESP_OK) err = reserve_text(&w, 0);
    if (err != ESP_OK) goto fail;
    w.text.utf8[w.text.len] = 0;
    *out = w.text;
    return ESP_OK;
fail:
    html_text_free(&w.text);
    return err;
}

esp_err_t html_to_blocks_with_css_anchor(const char* html, size_t len,
                                          const char* css, size_t css_len,
                                          const char* anchor, size_t* anchor_offset,
                                          html_text_t* out) {
    return html_to_blocks_with_css_target(html, len, css, css_len,
                                           anchor, SIZE_MAX, anchor_offset, out);
}

esp_err_t html_to_blocks_with_css(const char* html, size_t len,
                                   const char* css, size_t css_len, html_text_t* out) {
    return html_to_blocks_with_css_anchor(html, len, css, css_len, NULL, NULL, out);
}

esp_err_t html_to_blocks(const char* html, size_t len, html_text_t* out) {
    return html_to_blocks_with_css(html, len, NULL, 0, out);
}
