# Pico Reader

[English](README.md) · [简体中文](README.zh-CN.md) · [Third-party notices](THIRD_PARTY_NOTICES.md)

MindReset Read Pico（RDP-G01-W）向けの独立したオープンソース読書ファームウェアです。[MindReset の公式デモ](https://github.com/MindReset/read_pico_firmware)を基にしていますが、**MindReset の公式リリースではありません**。

EPUB と TXT の読書、読書位置の保存、TF カードのファイル管理、Wi-Fi・ホットスポット・USB での転送に対応します。[HTTPS オンライン書き込みページ](https://wegooo-cell.github.io/read-pico-reader/)は PC 版 Chrome または Edge と USB データケーブルで利用できます。書き込む前に基板型番を確認してください。通常の書き込みでは設定、読書記録、TF カードの内容を消去しません。

このファームウェアを使用中で自動的にダウンロードモードへ切り替わらない場合は、本体の「設定 → Pico のカード → 升级 → 进入 BOOT 模式」を選び、PC がシリアルポートを再認識してから書き込みページで再試行してください。

ビルドには ESP-IDF v6.1 が必要です。ファームウェアには Noto Sans SC Medium のサブセットのみを内蔵し、追加のフォントや書籍は配布しません。

本体の上流コードは Apache-2.0、参照した CrossPoint Reader のアイコンは MIT、関連する Lucide アイコンは ISC、epdiy は LGPL-3.0-or-later、内蔵フォントは SIL OFL 1.1 に従います。適用範囲は [第三者ライセンス一覧](THIRD_PARTY_NOTICES.md)をご覧ください。
