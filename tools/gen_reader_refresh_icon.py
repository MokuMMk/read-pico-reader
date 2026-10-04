"""从用户参考图生成阅读刷新图标。/ Build the reader refresh glyph from the supplied reference."""
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "previews/ux-v22/refresh-icon-reference.png"
PREVIEW = ROOT / "previews/ux-v22/refresh-icon.png"
HEADER = ROOT / "main/assets/reader_refresh_icon.h"
SIZE = 36


def main() -> None:
    source = Image.open(SOURCE).convert("RGB")
    # 参考图的 24px 图形位于 (18..41, 12..35)；四周留白后放大到工具栏尺寸。
    # The 24px mark occupies (18..41, 12..35); retain its padding at toolbar size.
    mark = source.crop((16, 10, 44, 38))
    bg = (252 + 248 + 250) / 3
    ink = (69 + 71 + 78) / 3
    alpha = Image.new("L", mark.size)
    alpha.putdata([
        round(max(0, min(255, (bg - sum(pixel) / 3) * 255 / (bg - ink))))
        for pixel in mark.get_flattened_data()
    ])
    alpha = alpha.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    nibbles = [round(value * 15 / 255) for value in alpha.get_flattened_data()]
    packed = bytes((nibbles[i] << 4) | nibbles[i + 1]
                   for i in range(0, len(nibbles), 2))
    rows = ["    " + ", ".join(f"0x{byte:02x}" for byte in packed[i:i + 18]) + ","
            for i in range(0, len(packed), 18)]
    HEADER.write_text(
        "/* 中文：由用户参考图生成的阅读设置刷新图标。/ English: Reader refresh glyph from the supplied reference. */\n"
        "#pragma once\n#include <stdint.h>\n"
        f"#define READER_REFRESH_ICON_SIZE {SIZE}\n"
        f"static const uint8_t reader_refresh_icon_alpha[] = {{\n" + "\n".join(rows) + "\n};\n",
        encoding="utf-8",
    )
    icon = Image.new("RGBA", (SIZE, SIZE), (56, 56, 56, 0))
    icon.putalpha(Image.frombytes("L", (SIZE, SIZE),
                                  bytes(value * 17 for value in nibbles)))
    icon.save(PREVIEW)
    print(PREVIEW)
    print(HEADER)


if __name__ == "__main__":
    main()
