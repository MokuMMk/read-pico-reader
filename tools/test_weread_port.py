#!/usr/bin/env python3
"""公开向量与临时文件的原生端口测试。 / Native port tests with public fixtures and temporary files."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import zipfile
ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "components/pico_weread"
with tempfile.TemporaryDirectory(prefix="pico-weread-") as directory:
    work = Path(directory).resolve()
    compiler = os.environ.get("CXX", shutil.which("clang++") or shutil.which("g++"))
    assert compiler, "A C++20 host compiler is required"
    common = [compiler, "-std=c++20", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra",
              "-I", str(ROOT / "tools/weread_test_stubs"), "-I", str(PORT / "platform"),
              "-I", str(PORT / "vendor")]
    protocol = work / "protocol-test"
    subprocess.run(common + ["-Werror", str(ROOT / "tools/weread_protocol_test.cpp"),
                   str(PORT / "vendor/WeReadProtocol.cpp"), str(PORT / "vendor/StreamingJsonParser.cpp"),
                   "-o", str(protocol)], check=True, timeout=60)
    subprocess.run([str(protocol)], check=True, timeout=20)
    sources = [PORT / "platform/HalStorage.cpp", PORT / "vendor/WeReadStore.cpp",
               PORT / "vendor/WeReadProtocol.cpp", PORT / "vendor/StreamingJsonParser.cpp",
               PORT / "vendor/WeReadXhtmlCodec.cpp"]
    binary = work / "storage-test"
    subprocess.run(common + [str(ROOT / "tools/weread_storage_test.cpp")] +
                   [str(s) for s in sources] + ["-o", str(binary)], check=True, timeout=60)
    subprocess.run([str(binary), str(work)], check=True, timeout=20)
    with zipfile.ZipFile(work / "books/test.epub") as archive:
        assert archive.testzip() is None
        assert archive.infolist()[0].filename == "mimetype"
        assert archive.infolist()[0].compress_type == zipfile.ZIP_STORED
        assert archive.read("mimetype") == b"application/epub+zip"
        assert "中文正文" in archive.read("OEBPS/ch0.xhtml").decode()
        assert archive.read("OEBPS/images/test.png").startswith(bytes([137, 80, 78, 71]))
    print("PASS: Python ZIP parser independently validated directory, CRC and UTF-8 content")
    reader = work / "reader-test"
    subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-I" + str(ROOT / "tools/book_epub_stubs"), "-I" + str(ROOT / "tools/zip_host_stubs"),
                   "-I" + str(ROOT / "main/book"), str(ROOT / "tools/weread_epub_reader_test.c"),
                   *[str(ROOT / "main/book" / name) for name in
                     ("book_epub.c", "book_image_header.c", "zip_reader.c", "html_text.c", "book_index_cache.c")],
                   "-lz", "-o", str(reader)], check=True, timeout=60)
    subprocess.run([str(reader), str(work / "books/test.epub")], check=True, timeout=20)

    http = work / "http-test"
    subprocess.run([compiler, "-std=c++20", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tools/weread_http_stubs"), "-I", str(PORT / "platform"), "-I", str(PORT / "vendor"),
                    str(ROOT / "tools/weread_http_test.cpp"), str(PORT / "platform/WeReadHttpClient.cpp"),
                    "-o", str(http)], check=True, timeout=60)
    subprocess.run([str(http)], check=True, timeout=20)

    client = work / "client-test"
    subprocess.run([common[0], "-I" + str(ROOT / "tools/weread_client_stubs"), *common[1:],
                    "-DENABLE_CHINESE_VERSION=1", "-DSIMULATOR=1",
                    "-I" + str(ROOT / "tools/weread_http_stubs"),
                    str(ROOT / "tools/weread_client_host_test.cpp"),
                    *[str(x) for x in sources], str(PORT / "vendor/WeReadBrowse.cpp"),
                    "-o", str(client)], check=True, timeout=60)
    subprocess.run([str(client), str(work / "cover-fixture")], check=True, timeout=20)
    with zipfile.ZipFile(work / "cover-fixture/books/测试原名书籍.epub") as archive:
        assert archive.testzip() is None
        assert archive.read("OEBPS/cover.png").startswith(bytes([137, 80, 78, 71]))
        assert b'cover-image' in archive.read("OEBPS/content.opf")
        assert "OEBPS/images/test.png" not in archive.namelist()
    print("PASS: independent ZIP check confirms a cover even with inline images excluded")
