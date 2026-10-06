#!/usr/bin/env python3
"""Validate the small, public Read Pico web-flashing bundle."""

from __future__ import annotations

import hashlib
import json
import sys
import struct
import re
from pathlib import Path


EXPECTED = {
    "bootloader.bin": (0x0, 0x8000),
    "partitions.bin": (0x8000, 0x10000),
    "firmware.bin": (0x10000, 0x410000),
    "ota_data_initial.bin": (0xD10000, 0xD12000),
}


def check(folder: Path) -> None:
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    assert manifest.get("new_install_prompt_erase") is False, "flasher must preserve user data"
    builds = manifest.get("builds")
    assert isinstance(builds, list) and len(builds) == 1
    assert builds[0].get("chipFamily") == "ESP32-S3"
    parts = builds[0].get("parts")
    assert isinstance(parts, list) and len(parts) == len(EXPECTED)
    seen: set[str] = set()
    for part in parts:
        name = part.get("path")
        assert name in EXPECTED and name not in seen, f"unexpected flash part: {name}"
        seen.add(name)
        start, end = EXPECTED[name]
        assert part.get("offset") == start, f"wrong offset for {name}"
        data = (folder / name).read_bytes()
        assert data and len(data) <= end - start, f"invalid size for {name}"
        print(f"{name}: {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()}")
    assert seen == set(EXPECTED)
    # 升级兼容性门禁：实际双槽分区、应用描述、基础包能力及升级 SHA 必须一致。
    # Upgrade gate: real slots, app description, base capability and upgrade SHA must agree.
    entries = {}
    table = (folder / "partitions.bin").read_bytes()
    for offset in range(0, len(table) - 31, 32):
        magic, kind, subtype, address, size, label, _ = struct.unpack_from("<HBBII16sI", table, offset)
        if magic != 0x50aa: break
        entries[label.split(b"\0")[0].decode()] = (kind, subtype, address, size)
    assert entries.get("ota_0") == (0, 16, 0x10000, 0x400000), "base OTA slot changed"
    assert entries.get("ota_1") == (0, 17, 0x910000, 0x400000), "inactive OTA slot changed"
    assert entries.get("otadata") == (1, 0, 0xd10000, 0x2000), "OTA selection metadata changed"
    assert entries.get("nvs") == (1, 2, 0x9000, 0x5000), "user settings partition changed"
    assert entries.get("storage") == (1, 129, 0x410000, 0x500000), "user storage partition changed"
    image = (folder / "firmware.bin").read_bytes()
    assert image[0] == 0xe9 and struct.unpack_from("<H", image, 12)[0] == 9, "wrong target chip"
    assert struct.unpack_from("<I", image, 32)[0] == 0xabcd5432, "missing app description"
    version = image[48:80].split(b"\0")[0].decode("ascii")
    project = image[80:112].split(b"\0")[0].decode("ascii")
    assert version == manifest["version"] and project == "Read_Pico", "base version mismatch"
    assert re.fullmatch(r"\d+\.\d+\.\d+(?:-rc\d+)?", version), "invalid release version"
    capability = manifest.get("pico_ota", {})
    assert capability.get("layout") == "pico-dual-4m-v1"
    assert capability.get("base_version") == version and capability.get("rollback") is True
    assert capability.get("minimum_base_version") == "0.3.3-rc72"
    assert capability.get("online_ota_base_version") == "0.3.3-rc79"
    assert b"PICO_HTTPS_OTA_V1" in image, "base package cannot perform online upgrades"
    feed = json.loads((folder / "update.json").read_text())
    upgrade_name = f"Pico-update-{version}.bin"
    assert feed["schema"] == 1 and feed["version"] == version and feed["project"] == project
    assert feed["board"] == "RDP-G01-W" and feed["layout"] == capability["layout"]
    assert feed["minimum_base_version"] == capability["minimum_base_version"]
    assert feed["url"] == f"https://wegooo-cell.github.io/read-pico-reader/{upgrade_name}"
    assert feed["size"] == len(image) and feed["sha256"] == hashlib.sha256(image).hexdigest()
    assert (folder / "Pico-update.bin").read_bytes() == image
    assert (folder / upgrade_name).read_bytes() == image
    print(f"OTA base/TF/feed compatible: {version}, {capability['layout']}")
    assert (folder / "index.html").is_file()
    assert (folder / "vendor/web/install-button.js").is_file()
    assert (folder / "licenses/LUCIDE-ISC.txt").is_file()


if __name__ == "__main__":
    check(Path(sys.argv[1]) if len(sys.argv) > 1 else Path("flash"))
