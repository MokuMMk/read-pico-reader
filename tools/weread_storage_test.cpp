/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 主机检查路径隔离、会话、原子替换、书架分页记录和 EPUB ZIP。
 * Host checks for path confinement, session persistence, replacement, shelf records and EPUB ZIP.
 * 冻结：只用临时目录和虚构账号，不联网。/ Frozen: temporary directories and fake accounts only; no network.
 */
#include "WeReadStore.h"
#include "WeReadProtocol.h"
#include "nvs.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <unistd.h>
#include <dlfcn.h>
#include <cerrno>
#include <sys/stat.h>
// 模拟FAT挂载根目录：它存在，但mkdir(root)返回EINVAL。/ Simulate an existing FAT mount root rejecting mkdir(root).
static std::string mountRoot;
static unsigned mountMkdirCalls;
extern "C" int mkdir(const char* path, mode_t mode) {
    if (!mountRoot.empty() && mountRoot == path) { ++mountMkdirCalls; errno = EINVAL; return -1; }
    using NativeMkdir = int (*)(const char*, mode_t);
    static auto nativeMkdir = reinterpret_cast<NativeMkdir>(dlsym(RTLD_NEXT, "mkdir"));
    assert(nativeMkdir); return nativeMkdir(path, mode);
}
static bool cancelled;
bool pico_weread_cancelled() { return cancelled; }
static std::vector<unsigned char> blob;
esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t* out) { *out = 1; return 0; }
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void* out, size_t* n) {
    if (blob.empty() || *n < blob.size()) return 1;
    memcpy(out, blob.data(), blob.size()); *n = blob.size(); return 0;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void* data, size_t n) {
    blob.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + n); return 0;
}
esp_err_t nvs_commit(nvs_handle_t) { return 0; }
esp_err_t nvs_erase_key(nvs_handle_t, const char*) { blob.clear(); return 0; }
void nvs_close(nvs_handle_t) {}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1];
    mountRoot = root;
    Storage.configure((root + "/cache").c_str(), (root + "/books").c_str());
    // 不预建缓存目录，复现真机首次登录后的书架写入。/ Reproduce first shelf write without precreating the cache.
    assert(!Storage.exists(WeReadStore::kRoot));
    WeReadStore::IndexWriter firstShelf;
    assert(firstShelf.begin(WeReadStore::kShelfPath, WeReadStore::kShelfMagic, sizeof(WeReadStore::ShelfRecord)));
    assert(firstShelf.finish());
    assert(Storage.exists(WeReadStore::kShelfPath));
    assert(mountMkdirCalls == 0);
    assert(WeReadStore::ensureRoot());
    assert(Storage.ensureDirectoryExists("/WeRead"));
    assert(Storage.map("/WeRead/../secret").empty());
    assert(Storage.map("/WeRead/..\\secret").empty());
    assert(Storage.map("/WeRead2/secret").empty());
    assert(Storage.map("/unknown").empty());
    assert(symlink("/tmp", (root + "/books/escape").c_str()) == 0);
    assert(Storage.map("/WeRead/escape/secret").empty());

    WeReadStore::Session session, loaded;
    assert(session.setCookie("wr_vid", "123", 3));
    assert(session.setCookie("wr_skey", "fake-token", 10));
    assert(!session.setCookie("wr_skey", "bad\r\n", 5));
    assert(WeReadStore::saveSession(session));
    assert(WeReadStore::loadSession(loaded));
    assert(strcmp(loaded.vid, "123") == 0 && strcmp(loaded.skey, "fake-token") == 0);
    assert(!Storage.exists(WeReadStore::kSessionPath));
    blob.assign(sizeof(session), 255);
    assert(!WeReadStore::loadSession(loaded));
    assert(!loaded.valid());
    assert(WeReadStore::clearSession());

    WeReadStore::IndexWriter shelf;
    assert(shelf.begin(WeReadStore::kShelfPath, WeReadStore::kShelfMagic, sizeof(WeReadStore::ShelfRecord)));
    for (unsigned i = 0; i < 17; ++i) {
        WeReadStore::ShelfRecord book;
        snprintf(book.bookId, sizeof(book.bookId), "%u", i);
        snprintf(book.title, sizeof(book.title), "中文书籍 %u", i);
        book.readUpdateTime = i;
        assert(shelf.append(&book));
    }
    assert(shelf.finish());
    assert(WeReadStore::sortShelfByRecent() == WeReadStore::ShelfSortResult::Ok);
    HalFile input; uint32_t count;
    assert(WeReadStore::openShelf(input, count) && count == 17);
    WeReadStore::ShelfRecord record;
    assert(WeReadStore::readShelfRecord(input, 0, record) && strcmp(record.bookId, "16") == 0);
    assert(!WeReadStore::readShelfRecord(input, 17, record));
    input.close();
    assert(WeReadStore::finalBookPath(record) == "/WeRead/中文书籍 16.epub");
    strcpy(record.title,"夏天、烟火和我的尸体（完整版）");
    assert(WeReadStore::finalBookPath(record) == "/WeRead/夏天、烟火和我的尸体（完整版）.epub");
    strcpy(record.title,"unsafe/../title:1");
    assert(WeReadStore::finalBookPath(record).find("/../") == std::string::npos);

    HalFile source;
    assert(Storage.openFileForWrite("test", "/.crosspoint/weread/chapter.xhtml", source));
    const char body[] = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>中文正文</p><img src=\"images/test.png\"/></body></html>";
    assert(source.write(body, strlen(body)) == strlen(body)); source.close();
    uint8_t buffer[1024];
    WeReadStore::StoreOnlyZipWriter zip;
    assert(zip.begin("/WeRead/test.epub.part", "/.crosspoint/weread/central.part", buffer, sizeof(buffer)));
    const char mime[] = "application/epub+zip";
    const char container[] = "<container><rootfiles><rootfile full-path=\"OEBPS/content.opf\"/></rootfiles></container>";
    const char opf[] = "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>中文测试</dc:title><dc:creator>测试作者</dc:creator></metadata><manifest><item id=\"illustration\" href=\"images/test.png\" media-type=\"image/png\"/><item id=\"c0\" href=\"ch0.xhtml\" media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"c0\"/></spine></package>";
    assert(zip.addBuffer("mimetype", reinterpret_cast<const uint8_t*>(mime), strlen(mime)));
    assert(zip.addBuffer("META-INF/container.xml", reinterpret_cast<const uint8_t*>(container), strlen(container)));
    assert(zip.addBuffer("OEBPS/content.opf", reinterpret_cast<const uint8_t*>(opf), strlen(opf)));
    assert(zip.addFile("OEBPS/ch0.xhtml", "/.crosspoint/weread/chapter.xhtml"));
    // 公开的 1×1 PNG，检查插图可进入同一个成品 ZIP。/ Public 1×1 PNG in the completed ZIP.
    const uint8_t png[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,181,28,12,2,0,0,0,11,73,68,65,84,120,218,99,252,255,31,0,3,3,2,0,239,191,173,163,0,0,0,0,73,69,78,68,174,66,96,130};
    assert(zip.addBuffer("OEBPS/images/test.png", png, sizeof(png)));
    assert(zip.finish());
    assert(WeReadStore::looksLikeZip("/WeRead/test.epub.part"));
    assert(WeReadStore::atomicReplace("/WeRead/test.epub.part", "/WeRead/test.epub"));
    assert(!WeReadStore::atomicReplace("/WeRead/missing.part", "/WeRead/test.epub"));
    assert(WeReadStore::looksLikeZip("/WeRead/test.epub"));
    assert(!Storage.exists("/WeRead/test.epub.bak"));

    assert(Storage.openFileForWrite("test", "/WeRead/cancel.epub.part", source));
    cancelled = true;
    assert(source.write(body, strlen(body)) == 0);
    assert(source.read(buffer, sizeof(buffer)) < 0);
    source.close(); cancelled = false;
    assert(!WeReadStore::looksLikeZip("/WeRead/cancel.epub.part"));
    assert(Storage.remove("/WeRead/cancel.epub.part"));
    assert(WeReadStore::looksLikeZip("/WeRead/test.epub"));
    puts("PASS: confined paths, session validation, shelf sorting, cancellation and ZIP publication");
}
