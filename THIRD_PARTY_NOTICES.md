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
| [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) icon artwork used as a design source | MIT | [CrossPoint notice](flash/licenses/CROSSPOINT-MIT.txt) |
| [Lucide](https://lucide.dev/license) icons included in the CrossPoint icon source | ISC | [Lucide notice](flash/licenses/LUCIDE-ISC.txt) |
| [epdiy](https://github.com/vroland/epdiy), including the locally modified driver | LGPL-3.0-or-later | [Modification notes](components/epdiy/LICENSE), [LGPLv3](licenses/LGPL-3.0.txt), [GPLv3](licenses/GPL-3.0.txt) |
| [pypinyin](https://github.com/mozillazg/python-pinyin) dictionary data used by offline book search | MIT | [pypinyin notice](components/read_pico_search/LICENSE.pypinyin) |
| Embedded Noto Sans SC Medium font subset | SIL OFL-1.1 | [Font license](main/assets/OFL-Noto.txt) |
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) browser flasher | Apache-2.0 | [Bundled web tool license](flash/vendor/LICENSE) |
| Espressif TinyUSB component | Apache-2.0 | [Component license](components/espressif__esp_tinyusb/LICENSE) |

License terms apply to their respective material. The repository's top-level
Apache-2.0 license does not replace those component licenses. In particular,
the CrossPoint MIT credit does **not** mean the whole firmware is MIT-licensed.

各组件继续适用各自的许可。CrossPoint 的 MIT 署名不表示整个固件改为 MIT 许可。
The firmware bundles only the embedded Noto Sans SC subset; users may supply
their own fonts on a TF card. The repository and flashing site do not provide
additional font packages.
