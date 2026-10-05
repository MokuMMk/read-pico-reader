/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 验证 CrossMux 协议层脱离 Arduino 后的主机编译与边界行为。
 * Host compilation and boundary checks for the CrossMux protocol without Arduino.
 *
 * 冻结：只用公开测试向量，不联网，不接触账号或设备。
 * Frozen: public fixtures only; no network, account or device access.
 */
#include "WeReadProtocol.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static bool append(void* ctx, const uint8_t* bytes, size_t size) {
    static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(bytes), size);
    return true;
}

int main() {
    char out[128] = {};
    assert(WeReadProtocol::signQuery("a=1&b=hello%20world", out, sizeof(out)));
    assert(strcmp(out, "2a2e5d8e") == 0);
    assert(WeReadProtocol::urlEncode("中文 a&", out, sizeof(out)));
    assert(strcmp(out, "%E4%B8%AD%E6%96%87%20a%26") == 0);
    char bounded[5];
    assert(WeReadProtocol::decodeJsonString("\\u4E2D\\u6587", 12, bounded, sizeof(bounded)) == 3);
    assert(strcmp(bounded, "中") == 0);
    assert(WeReadProtocol::decodeJsonString("A\\u4E2D\\uD83D\\uDE00", 19, out, sizeof(out)) == 8);
    assert(strcmp(out, "A中😀") == 0);

    char cookie[128] = {};
    assert(WeReadProtocol::mergeRuntimeCookie(cookie, sizeof(cookie), "wr_vid", 6, "1", 1));
    assert(WeReadProtocol::mergeRuntimeCookie(cookie, sizeof(cookie), "wr_skey", 7, "two", 3));
    assert(WeReadProtocol::mergeRuntimeCookie(cookie, sizeof(cookie), "wr_skey", 7, "rotated", 7));
    assert(strcmp(cookie, "wr_vid=1; wr_skey=rotated") == 0);
    std::string saved = cookie;
    assert(!WeReadProtocol::mergeRuntimeCookie(cookie, sizeof(cookie), "wr_skey", 7, "bad\r\nheader", 11));
    assert(saved == cookie);
    assert(WeReadProtocol::mergeRuntimeCookie(cookie, sizeof(cookie), "wr_skey", 7, "", 0));
    assert(strcmp(cookie, "wr_vid=1") == 0);

    constexpr char html[] = R"(<script>window.__INITIAL_STATE__={"other":1,"psvts" : "abc_DEF-123"};</script>)";
    for (size_t split = 0; split < sizeof(html); ++split) {
        WeReadProtocol::PsvtsExtractor parser(out, sizeof(out));
        assert(parser.reset());
        assert(parser.feed(reinterpret_cast<const uint8_t*>(html), split));
        assert(parser.feed(reinterpret_cast<const uint8_t*>(html) + split, sizeof(html) - 1 - split));
        assert(parser.complete());
        assert(strcmp(out, "abc_DEF-123") == 0);
    }
    WeReadProtocol::PsvtsExtractor too_small(bounded, sizeof(bounded));
    assert(too_small.reset());
    assert(too_small.feed(reinterpret_cast<const uint8_t*>(html), sizeof(html) - 1));
    assert(!too_small.complete());

    constexpr char json[] = R"({"ignored":{"bookId":"other-book","progress":99},"payload":{"bookId":"book-1","progress":"40.5","chapterUid":"chapter-2","chapterOffset":150,"updateTime":1785301234}})";
    for (size_t split = 0; split < sizeof(json); ++split) {
        WeReadProtocol::RemoteProgressParser parser("book-1");
        assert(parser.feed(reinterpret_cast<const uint8_t*>(json), split));
        assert(parser.feed(reinterpret_cast<const uint8_t*>(json) + split, sizeof(json) - 1 - split));
        assert(parser.complete());
        assert(parser.progress().percent == 40.5f);
        assert(strcmp(parser.progress().chapterUid, "chapter-2") == 0);
        assert(parser.progress().chapterOffset == 150);
    }
    WeReadProtocol::RemoteProgressParser truncated("book-1");
    constexpr char fragment[] = R"({"progress":12)";
    assert(truncated.feed(reinterpret_cast<const uint8_t*>(fragment), sizeof(fragment) - 1));
    assert(!truncated.complete());

    constexpr char encoded[] = "SGVsbG8sIOS4lueVjCE";
    for (size_t chunk = 1; chunk < sizeof(encoded); ++chunk) {
        std::string decoded;
        WeReadProtocol::Base64UrlDecoder decoder(append, &decoded);
        for (size_t i = 0; i < sizeof(encoded) - 1; i += chunk) {
            assert(decoder.feed(reinterpret_cast<const uint8_t*>(encoded) + i,
                                std::min(chunk, sizeof(encoded) - 1 - i)));
        }
        assert(decoder.finish());
        assert(decoded == "Hello, 世界!");
    }
    std::string invalid_output;
    WeReadProtocol::Base64UrlDecoder invalid(append, &invalid_output);
    assert(invalid.feed(reinterpret_cast<const uint8_t*>("A"), 1));
    assert(!invalid.finish());
    const char* crc_vector = "123456789";
    assert((WeReadProtocol::crc32Update(0xFFFFFFFF,
                reinterpret_cast<const uint8_t*>(crc_vector), strlen(crc_vector)) ^ 0xFFFFFFFF) == 0xCBF43926);
    puts("PASS: portable protocol, signatures, UTF-8, cookies, reader tokens, progress parsing, Base64 and CRC fixtures");
}
