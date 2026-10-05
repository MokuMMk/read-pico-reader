# Third-party notices / 第三方许可

This is an independent reader firmware for MindReset's Read Pico board. It is
based on the [MindReset demo firmware](https://github.com/MindReset/read_pico_firmware),
which is licensed under Apache-2.0. The original copyright and license notices
remain in the source. This project is not an official MindReset release.

本项目是基于 MindReset Read Pico 官方示例固件开发的独立阅读固件，并非 MindReset
官方发布。原项目的版权声明和 Apache-2.0 许可均予保留。

| Material / 组件 | License / 许可 | Notice / 声明 |
| --- | --- | --- |
| MindReset Read Pico firmware and board components | Apache-2.0 | [LICENSE](LICENSE); source file headers |
| [Lucide](https://lucide.dev/icons/) — the whole UI icon set, vendored as SVG sources in `tools/icons/lucide/` and compiled into `main/assets/ui_icons.h` by `tools/gen_ui_icons.py` | ISC | [Lucide notice](flash/licenses/LUCIDE-ISC.txt) |
| [epdiy](https://github.com/vroland/epdiy), including the locally modified driver | LGPL-3.0-or-later | [Modification notes](components/epdiy/LICENSE), [LGPLv3](licenses/LGPL-3.0.txt), [GPLv3](licenses/GPL-3.0.txt) |
| [pypinyin](https://github.com/mozillazg/python-pinyin) dictionary data used by offline book search | MIT | [pypinyin notice](components/read_pico_search/LICENSE.pypinyin) |
| [CrossMux](https://github.com/0x1abin/crossmux) WeRead protocol, streaming download and EPUB writer, ported from commit `d6a1727bb27a858ba2ee9a44529ba8e458defbad` | MIT | [CrossMux notice](components/pico_weread/vendor/LICENSE-CrossMux.txt) |
| FreeInk SDK StreamingJsonParser, commit `96de1be6ce08eb732909e6e8149af8f892b9a2c5` | MIT | [FreeInk SDK notice](components/pico_weread/vendor/LICENSE-FreeInk-SDK.txt) |
| Embedded Noto Sans SC Medium font subset | SIL OFL-1.1 | [Font license](main/assets/OFL-Noto.txt) |
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) browser flasher | Apache-2.0 | [Bundled web tool license](flash/vendor/LICENSE) |
| Espressif TinyUSB component | Apache-2.0 | [Component license](components/espressif__esp_tinyusb/LICENSE) |

License terms apply to their respective material. The repository's top-level
Apache-2.0 license does not replace those component licenses.

各组件继续适用各自的许可，不能把整个固件简称为其中某一个组件的许可。
The firmware bundles only the embedded Noto Sans SC subset; users may supply
their own fonts on a TF card. The repository and flashing site do not provide
additional font packages.
