#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""验证固件 WiFi 像素来自预览图同一图形。/ Verify firmware WiFi pixels come from the preview glyph."""
from __future__ import annotations

import importlib.util
import re
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PREVIEW = ROOT / "previews/ux-v15/render.py"
HEADER = ROOT / "main/assets/wifi_glyph.h"


def main() -> None:
    spec = importlib.util.spec_from_file_location("pico_wifi_reference", PREVIEW)
    preview = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(preview)
    image = Image.new("RGB", (32, 32), "white")
    preview.setting_icon(image, 16, 16, "wifi")
    alpha = Image.new("L", (32, 32))
    alpha.putdata([max(0, min(255, round((255 - pixel[0]) * 255 / 149)))
                   for pixel in image.get_flattened_data()])
    content = HEADER.read_text()
    for size, expected in ((32, alpha), (24, alpha.resize((24, 24), Image.Resampling.LANCZOS))):
        match = re.search(rf"wifi_glyph_{size}\[\d+\] = \{{(.*?)\}};", content, re.S)
        assert match, f"missing {size}px WiFi glyph"
        values = [int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", match.group(1))]
        pixels = [item for byte in values for item in (byte >> 4, byte & 15)]
        assert len(pixels) == size * size
        for actual, source in zip(pixels, expected.get_flattened_data()):
            assert actual == round(source * 15 / 255)
    print("WiFi 32px and 24px masks match the UX v17 preview glyph")


if __name__ == "__main__":
    main()
