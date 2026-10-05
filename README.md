# Pico Reader

[简体中文](README.zh-CN.md) · [日本語](README.ja-JP.md) · [Third-party notices](THIRD_PARTY_NOTICES.md)

An independent, open-source reading firmware for the **MindReset Read Pico (RDP-G01-W)** ESP32-S3 e-paper board. It is based on [MindReset's official demo firmware](https://github.com/MindReset/read_pico_firmware), but is **not an official MindReset release**.

The current interface has Home, Bookshelf, Files, and Settings. It reads EPUB and TXT books from a TF card, remembers reading progress, and supports Wi-Fi, hotspot, or USB file transfer. The online flasher installs the same firmware image as the local `flash/` bundle.

## Online installation

Visit the [HTTPS web flasher](https://wegooo-cell.github.io/read-pico-reader/). Use a desktop Chrome or Edge browser with a USB data cable. Select the Read Pico serial device and follow the prompts. **Check the board model before flashing.** Ordinary installation preserves the device's settings and reading records, as well as TF-card contents. It does not include books or sample reading history.

If automatic entry to download mode fails on a device already running this firmware, open **File Manager → BOOT** on the device, wait for the computer to detect its serial port again, then retry the web flasher.

The release manifest is [`flash/manifest.json`](flash/manifest.json); it flashes the bootloader, partition table, application, and the OTA data partition. Existing NVS settings and the internal book partition are preserved. The site is published from an explicit allowlist in [`.github/workflows/pages.yml`](.github/workflows/pages.yml), so local books, backups, and extra font packages are not uploaded. See the [detailed rc68 release notes](docs/RELEASE_NOTES_0.3.3-rc68.md).

## Local TF-card updates

Local updates require one complete computer installation of an OTA base build so the bootloader and dual-slot partition table are present. That migration keeps the existing settings, reading records, internal-storage addresses, and TF-card contents. Later, copy the application image to the TF-card root as `Pico-update.bin`, then open **Files → Update → Install update**.

Pico checks the image project, version, size, and headers before writing the inactive firmware slot. It selects the new slot only after full image validation. If the new image resets before its first hardware and UI startup check succeeds, the bootloader returns to the previous slot. Keep power connected and the TF card inserted during installation. An application-only update cannot replace the initial OTA base installation and cannot change the bootloader or partition table.

## Build from source

Use ESP-IDF **v6.1** for ESP32-S3:

```sh
idf.py set-target esp32s3
idf.py build
```

The board-specific flash and PSRAM timing is in `sdkconfig.defaults`. `sdkconfig.ci` is for compile checks only. Follow the [MindReset hardware documentation](https://dot.mindreset.tech/docs/read_0) for the board. A prebuilt firmware image is provided for the RDP-G01-W only.

## Books and fonts

On first mount, the firmware creates `books`, `fonts`, and `pictures` folders on the TF card if absent. No books are preloaded. The firmware embeds a subset of **Noto Sans SC Medium** for the system UI and distributes no additional font package. Users may place their own compatible fonts in `fonts` for reading. The embedded subset remains under the [SIL Open Font License](main/assets/OFL-Noto.txt).

The “Covers and spines” bookshelf mode is labeled **experimental, not a formal release**. Other shelf styles remain available.

TXT books and EPUB books without a valid embedded cover receive a deterministic grayscale cover shared by the shelf, home, and ticket lock screen. A valid EPUB cover takes priority. Generated covers are cached under `.readpico/covers` on the TF card and rebuilt when the file or title changes.

In Reading settings → Typography, first-line indent can be set to 0, 1, 2, or 3 characters (2 by default). Body text is centered by whole-character columns to balance the side margins, and common Chinese punctuation is kept away from prohibited line starts and ends.

To back up personal settings, open **Settings → Save & restore → Save to TF card**. The device writes `Pico-settings.backup` to the TF-card root. Put that file back at the root and choose **Restore from TF card** to recover fonts, typography, display and lock settings, profile and status signature, saved Wi-Fi name and password, book progress, reading time, bookmarks, favorites, and custom book names. Book, font, avatar, and wallpaper files remain on the TF card. The backup contains the Wi-Fi password in plaintext, so keep the TF card private. Missing external fonts and images fall back to built-in options. Older backups remain readable and leave the current network configuration unchanged.

EPUB metadata is allocated for the actual book size. ZIP entries and chapters each have an 8,192-item limit; covers, images, and navigation also consume ZIP entries. A book may exceed 32 MB overall, while each XHTML resource remains limited to 4 MB and each decompressed image to 8 MB; available device memory and standard ZIP limits also apply.

## Licenses and credit

The fork retains the upstream **Apache-2.0** license and notices. CrossPoint Reader icon artwork is credited under its **MIT** license; Lucide's **ISC** notice is retained. The modified epdiy driver uses **LGPL-3.0-or-later**, and pypinyin dictionary data uses **MIT**. See [Third-party notices](THIRD_PARTY_NOTICES.md) and component directories for the exact scope. The CrossPoint MIT notice does not change the license of the entire firmware.

Please report firmware bugs in this repository, not in the MindReset upstream issue tracker. Hardware purchasing and repair remain matters for the [official support channels](https://dot.mindreset.tech/docs/contact).
