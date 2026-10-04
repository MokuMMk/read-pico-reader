# Pico Reader

[简体中文](README.zh-CN.md) · [日本語](README.ja-JP.md) · [Third-party notices](THIRD_PARTY_NOTICES.md)

An independent, open-source reading firmware for the **MindReset Read Pico (RDP-G01-W)** ESP32-S3 e-paper board. It is based on [MindReset's official demo firmware](https://github.com/MindReset/read_pico_firmware), but is **not an official MindReset release**.

The current interface has Home, Bookshelf, Files, and Settings. It reads EPUB and TXT books from a TF card, remembers reading progress, and supports Wi-Fi, hotspot, or USB file transfer. The online flasher installs the same firmware image as the local `flash/` bundle.

## Online installation

Visit the [HTTPS web flasher](https://wegooo-cell.github.io/read-pico-reader/). Use a desktop Chrome or Edge browser with a USB data cable. Select the Read Pico serial device and follow the prompts. **Check the board model before flashing.** Ordinary installation preserves the device's settings and reading records, as well as TF-card contents. It does not include books or sample reading history.

If automatic entry to download mode fails on a device already running this firmware, open **File Manager → BOOT** on the device, wait for the computer to detect its serial port again, then retry the web flasher.

The release manifest is [`flash/manifest.json`](flash/manifest.json); it flashes only the bootloader, partition table, and application. The site is published from an explicit allowlist in [`.github/workflows/pages.yml`](.github/workflows/pages.yml), so local books, backups, and extra font packages are not uploaded.

## Build from source

Use ESP-IDF **v6.1** for ESP32-S3:

```sh
idf.py set-target esp32s3
idf.py build
```

The board-specific flash and PSRAM timing is in `sdkconfig.defaults`. `sdkconfig.ci` is for compile checks only. Follow the [MindReset hardware documentation](https://dot.mindreset.tech/docs/read_0) for the board. A prebuilt firmware image is provided for the RDP-G01-W only.

## Books and fonts

On first mount, the firmware creates `books`, `fonts`, and `pictures` folders on the TF card if absent. No books are preloaded. The firmware embeds a subset of **Noto Sans SC Medium** for the system UI and distributes no additional font package. Users may place their own compatible fonts in `fonts` for reading. The embedded subset remains under the [SIL Open Font License](main/assets/OFL-Noto.txt).

TXT books and EPUB books without a valid embedded cover receive a deterministic grayscale cover shared by the shelf, home, and ticket lock screen. A valid EPUB cover takes priority. Generated covers are cached under `.readpico/covers` on the TF card and rebuilt when the file or title changes.

In Reading settings → Typography, first-line indent can be set to 0, 1, 2, or 3 characters (2 by default). Body text is centered by whole-character columns to balance the side margins, and common Chinese punctuation is kept away from prohibited line starts and ends.

To back up personal settings, open **Settings → Save & restore → Save to TF card**. The device writes `Pico-settings.backup` to the TF-card root. Put that file back at the root and choose **Restore from TF card** to recover system and reading fonts, sizes, typography, contrast, shelf, lock-screen, and reading controls. Books, reading progress, Wi-Fi credentials, font files, and wallpaper images are not copied; missing external assets fall back to built-in options.

EPUB metadata is allocated for the actual book size. ZIP entries and chapters each have an 8,192-item limit; covers, images, and navigation also consume ZIP entries. A book may exceed 32 MB overall, while each XHTML resource remains limited to 4 MB and each decompressed image to 8 MB; available device memory and standard ZIP limits also apply.

## Licenses and credit

The fork retains the upstream **Apache-2.0** license and notices. CrossPoint Reader icon artwork is credited under its **MIT** license; Lucide's **ISC** notice is retained. The modified epdiy driver uses **LGPL-3.0-or-later**, and pypinyin dictionary data uses **MIT**. See [Third-party notices](THIRD_PARTY_NOTICES.md) and component directories for the exact scope. The CrossPoint MIT notice does not change the license of the entire firmware.

Please report firmware bugs in this repository, not in the MindReset upstream issue tracker. Hardware purchasing and repair remain matters for the [official support channels](https://dot.mindreset.tech/docs/contact).
