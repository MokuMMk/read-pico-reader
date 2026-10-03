# Pico 阅读固件

[English](README.md) · [日本語](README.ja-JP.md) · [第三方许可](THIRD_PARTY_NOTICES.md)

这是为 **MindReset Read Pico（RDP-G01-W）** 墨水屏开发板制作的独立开源阅读固件，基于 [MindReset 官方示例固件](https://github.com/MindReset/read_pico_firmware) 开发，**并非 MindReset 官方发布**。

界面包括首页、书架、文件管理和设置；可阅读 TF 卡中的 EPUB、TXT，保存阅读进度，并通过 WiFi、设备热点或 USB 传输文件。

## 在线网页刷机

打开 [HTTPS 在线刷机页](https://wegooo-cell.github.io/read-pico-reader/)。在电脑端 Chrome 或 Edge 中，用支持数据传输的 USB 线连接 **Read Pico RDP-G01-W**，选择设备串口并按提示刷入。**刷机前确认型号。**普通刷机保留设备设置、阅读记录和 TF 卡内容；固件不附带书籍或预设的阅读记录。

如果运行本固件的设备无法自动进入下载模式，可在设备上点击**设置 → Pico 设备卡片 → 升级 → 进入 BOOT 模式**，等电脑重新识别串口后再用网页刷机。

刷机页现有图文新手指南：**连接 Pico → 选择串口 → 确认安装并等待完成**。页面下方也介绍了首页、书架、文件管理和设置，以及常见问题。

刷机清单为 [`flash/manifest.json`](flash/manifest.json)，只写入引导程序、分区表和应用程序。网页由 [Pages 工作流](.github/workflows/pages.yml) 按明确的文件清单部署，不上传本机书籍、备份或额外字体包。

## 自行编译

需要 ESP-IDF **v6.1**，目标芯片为 ESP32-S3：

```sh
idf.py set-target esp32s3
idf.py build
```

`sdkconfig.defaults` 使用本开发板对应的 Flash/PSRAM 时序；`sdkconfig.ci` 仅用于编译检查。硬件资料请看 [MindReset 官方文档](https://dot.mindreset.tech/docs/read_0)。预编译固件仅面向 RDP-G01-W。

## 书籍与字体

首次挂载 TF 卡时，固件会按需建立 `books`、`fonts`、`pictures` 文件夹，不会预装书籍。系统界面内建 **思源黑体 Medium 子集**，除此之外不提供字体包。用户可自行把兼容字体放入 TF 卡的 `fonts` 文件夹，供阅读正文选择。内建字体遵循 [SIL OFL 1.1](main/assets/OFL-Noto.txt)。

阅读时轻点正文中央可切换全屏，按屏幕下方中间触控键打开阅读设置。字体设置主面板可调整字号、选择阅读字体、切换晃动翻页与阅读线；点开「排版设置」可调整边距、行距、段距和字间距。字间距默认位于滑轨中间，正文段落默认首行缩进两字。点按「阅读线」卡片依次切换无、虚线和点线，卡片直接显示当前线型。在「设置 → 阅读操作」中可开启电源键翻页（阅读正文短按下一页、长按锁屏）及全屏沉浸（全屏时隐藏状态栏）。这两个开关默认关闭。

## 许可与致谢

本项目保留官方示例固件的 **Apache-2.0** 许可与原有版权声明。图标设计引用 CrossPoint Reader，保留其 **MIT** 许可和相关 Lucide **ISC** 声明；epdiy 驱动适用 **LGPL-3.0-or-later**，拼音字典数据适用 **MIT**。各许可分别适用于对应材料，不能把整个固件简称为 MIT 项目。细节见 [第三方许可说明](THIRD_PARTY_NOTICES.md)。

固件问题请在本仓库反馈；设备购买和维修请联系 [MindReset 官方渠道](https://dot.mindreset.tech/docs/contact)。
