# Third-party notices / 绗笁鏂硅鍙?

This is an independent reader firmware for MindReset's Read Pico board. It is
based on the [MindReset demo firmware](https://github.com/MindReset/read_pico_firmware),
which is licensed under Apache-2.0. The original copyright and license notices
remain in the source. This project is not an official MindReset release.

鏈」鐩槸鍩轰簬 MindReset Read Pico 瀹樻柟绀轰緥鍥轰欢寮€鍙戠殑鐙珛闃呰鍥轰欢锛屽苟闈?MindReset
瀹樻柟鍙戝竷銆傚師椤圭洰鐨勭増鏉冨０鏄庡拰 Apache-2.0 璁稿彲鍧囦簣淇濈暀銆?

| Material / 缁勪欢 | License / 璁稿彲 | Notice / 澹版槑 |
| --- | --- | --- |
| MindReset Read Pico firmware and board components | Apache-2.0 | [LICENSE](LICENSE); source file headers |
| [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) icon artwork used as a design source | MIT | [CrossPoint notice](flash/licenses/CROSSPOINT-MIT.txt) |
| [Lucide](https://lucide.dev/license) icons included in the CrossPoint icon source | ISC | [Lucide notice](flash/licenses/LUCIDE-ISC.txt) |
| [JPEGDEC](https://github.com/bitbank2/JPEGDEC) — progressive (SOF2) JPEG decoding, vendored and patched in `components/jpegdec/`; its `FORK.md` lists the changes | Apache-2.0 | [Component license](components/jpegdec/LICENSE) |
| [epdiy](https://github.com/vroland/epdiy), including the locally modified driver | LGPL-3.0-or-later | [Modification notes](components/epdiy/LICENSE), [LGPLv3](licenses/LGPL-3.0.txt), [GPLv3](licenses/GPL-3.0.txt) |
| [pypinyin](https://github.com/mozillazg/python-pinyin) dictionary data used by offline book search | MIT | [pypinyin notice](components/read_pico_search/LICENSE.pypinyin) |
| Embedded Noto Sans SC Medium font subset | SIL OFL-1.1 | [Font license](main/assets/OFL-Noto.txt) |
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) browser flasher | Apache-2.0 | [Bundled web tool license](flash/vendor/LICENSE) |
| Espressif TinyUSB component | Apache-2.0 | [Component license](components/espressif__esp_tinyusb/LICENSE) |

License terms apply to their respective material. The repository's top-level
Apache-2.0 license does not replace those component licenses. In particular,
the CrossPoint MIT credit does **not** mean the whole firmware is MIT-licensed.

鍚勭粍浠剁户缁€傜敤鍚勮嚜鐨勮鍙€侰rossPoint 鐨?MIT 缃插悕涓嶈〃绀烘暣涓浐浠舵敼涓?MIT 璁稿彲銆?
The firmware bundles only the embedded Noto Sans SC subset; users may supply
their own fonts on a TF card. The repository and flashing site do not provide
additional font packages.
