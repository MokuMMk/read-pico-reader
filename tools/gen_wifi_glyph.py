#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""从 UX v17 实际引用的设置图标生成固件掩模。/ Generate firmware masks from the UX v17 Settings glyph."""
from __future__ import annotations

import importlib.util
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PREVIEW = ROOT / "previews/ux-v15/render.py"
OUTPUT = ROOT / "main/assets/wifi_glyph.h"


def main() -> None:
    spec = importlib.util.spec_from_file_location("pico_wifi_reference", PREVIEW)
    preview = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(preview)
    canvas = Image.new("RGB", (32, 32), (255, 255, 255))
    preview.setting_icon(canvas, 16, 16, "wifi")
    # The preview paints the mark in RGB(106,108,105). Recover its alpha,
    # then use the same LANCZOS resize as ux-v17's 24 px status-bar glyph.
    alpha = Image.new("L", (32, 32))
    alpha.putdata([max(0, min(255, round((255 - pixel[0]) * 255 / 149)))
                   for pixel in canvas.get_flattened_data()])
    masks = {32: alpha, 24: alpha.resize((24, 24), Image.Resampling.LANCZOS)}
    lines = [
        "/* SPDX-FileCopyrightText: 2026 mindreset",
        " * SPDX-License-Identifier: Apache-2.0",
        " * 中文：由 tools/gen_wifi_glyph.py 从 UX v17 预览原图生成，勿手改。",
        " * English: Generated from the UX v17 reference glyph; do not edit by hand.",
        " */",
        "#pragma once",
        "#include <stdint.h>",
    ]
    for size, mask in masks.items():
        values = [round(value * 15 / 255) for value in mask.get_flattened_data()]
        packed = [(values[i] << 4) | values[i + 1] for i in range(0, len(values), 2)]
        lines.append(f"static const uint8_t wifi_glyph_{size}[{len(packed)}] = {{")
        for start in range(0, len(packed), 20):
            lines.append("    " + ", ".join(f"0x{value:02x}" for value in packed[start:start + 20]) + ",")
        lines.append("};")
    OUTPUT.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
