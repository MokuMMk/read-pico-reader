#!/usr/bin/env python3
"""Stage only the files intended for the public HTTPS flashing site."""

from __future__ import annotations

from html.parser import HTMLParser
from pathlib import Path
import shutil
import json
import re
import sys
from urllib.parse import urlparse

from verify_flash_bundle import check, check_archived_releases


ROOT = Path(__file__).resolve().parents[1]
FLASH = ROOT / "flash"


class LocalLinks(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.links: list[str] = []

    def handle_starttag(self, _tag: str, attrs: list[tuple[str, str | None]]) -> None:
        for key, value in attrs:
            if key in {"href", "src", "manifest"} and value:
                self.links.append(value)


def stage(output: Path) -> None:
    check(FLASH)
    output.mkdir(parents=True, exist_ok=True)
    for name in ("index.html", "notices.html", "manifest.json", "transfer-preview.png", "toc-preview.png", "firmware.bin",
                 "bootloader.bin", "partitions.bin", "ota_data_initial.bin", "Pico-update.bin", "update.json"):
        shutil.copy2(FLASH / name, output / name)
    version = json.loads((FLASH / "manifest.json").read_text())["version"]
    shutil.copy2(FLASH / f"Pico-update-{version}.bin", output / f"Pico-update-{version}.bin")
    # 保留已发布版本的固定下载地址，避免旧清单缓存或正在进行的更新突然遇到 404。
    # Keep immutable published URLs so cached manifests and in-flight updates do not suddenly get a 404.
    for upgrade in FLASH.glob("Pico-update-*.bin"):
        name = re.fullmatch(r"Pico-update-(\d+\.\d+\.\d+(?:-rc\d+)?)\.bin", upgrade.name)
        if not name: continue
        with upgrade.open("rb") as source:
            header = source.read(112)
        assert header[48:80].split(b"\0")[0].decode("ascii") == name[1], "historical upgrade version mismatch"
        assert header[80:112].split(b"\0")[0] == b"Read_Pico", "historical upgrade project mismatch"
        shutil.copy2(upgrade, output / upgrade.name)
    # 按验证后的正式目录发布历史清单与基础包，不递归上传本地测试文件。
    # Publish only verified archived manifests/base parts, excluding local test files.
    archives = check_archived_releases(FLASH)
    if (FLASH / "releases.json").is_file():
        shutil.copy2(FLASH / "releases.json", output / "releases.json")
        shutil.copy2(FLASH / "versions.js", output / "versions.js")
        for release in archives:
            destination = output / Path(release["manifest"]).parent
            destination.mkdir(parents=True, exist_ok=True)
            shutil.copy2(FLASH / release["manifest"], destination / "manifest.json")
            for name in ("bootloader.bin", "partitions.bin", "ota_data_initial.bin"):
                shutil.copy2(FLASH / Path(release["manifest"]).parent / name, destination / name)
    shutil.copytree(FLASH / "vendor/web", output / "vendor/web", dirs_exist_ok=True)
    shutil.copy2(FLASH / "vendor/LICENSE", output / "vendor/LICENSE")
    licenses = {
        FLASH / "licenses/LUCIDE-ISC.txt": "LUCIDE-ISC.txt",
        ROOT / "components/jpegdec/LICENSE": "JPEGDEC-APACHE-2.0.txt",
        ROOT / "licenses/LGPL-3.0.txt": "LGPL-3.0.txt",
        ROOT / "licenses/GPL-3.0.txt": "GPL-3.0.txt",
        ROOT / "components/read_pico_search/LICENSE.pypinyin": "PYPINYIN-MIT.txt",
        ROOT / "main/assets/OFL-Noto.txt": "OFL-Noto.txt",
        ROOT / "licenses/CrossMux-MIT.txt": "CROSSMUX-MIT.txt",
        ROOT / "licenses/FreeInk-SDK-MIT.txt": "FREEINK-SDK-MIT.txt",
    }
    (output / "licenses").mkdir(exist_ok=True)
    for source, name in licenses.items():
        shutil.copy2(source, output / "licenses" / name)
    shutil.copy2(ROOT / "LICENSE", output / "LICENSE")
    (output / ".nojekyll").touch()

    for page in ("index.html", "notices.html"):
        links = LocalLinks()
        links.feed((output / page).read_text(encoding="utf-8"))
        for link in links.links:
            parsed = urlparse(link)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            target = (output / parsed.path.removeprefix("./")).resolve()
            assert target.is_relative_to(output.resolve()) and target.is_file(), (
                f"broken local link in {page}: {link}")
    files = list(output.rglob("*"))
    count = sum(path.is_file() for path in files)
    size = sum(path.stat().st_size for path in files if path.is_file())
    print(f"Public site: {count} files, {size} bytes")


if __name__ == "__main__":
    stage(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "_site")
