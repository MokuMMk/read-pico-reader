#!/usr/bin/env python3
"""Stage only the files intended for the public HTTPS flashing site."""

from __future__ import annotations

from html.parser import HTMLParser
from pathlib import Path
import shutil
import sys
from urllib.parse import urlparse

from verify_flash_bundle import check


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
    for name in ("index.html", "notices.html", "manifest.json", "firmware.bin",
                 "bootloader.bin", "partitions.bin", "ota_data_initial.bin"):
        shutil.copy2(FLASH / name, output / name)
    shutil.copytree(FLASH / "vendor/web", output / "vendor/web", dirs_exist_ok=True)
    shutil.copy2(FLASH / "vendor/LICENSE", output / "vendor/LICENSE")
    licenses = {
        FLASH / "licenses/CROSSPOINT-MIT.txt": "CROSSPOINT-MIT.txt",
        FLASH / "licenses/LUCIDE-ISC.txt": "LUCIDE-ISC.txt",
        ROOT / "licenses/LGPL-3.0.txt": "LGPL-3.0.txt",
        ROOT / "licenses/GPL-3.0.txt": "GPL-3.0.txt",
        ROOT / "components/read_pico_search/LICENSE.pypinyin": "PYPINYIN-MIT.txt",
        ROOT / "main/assets/OFL-Noto.txt": "OFL-Noto.txt",
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
