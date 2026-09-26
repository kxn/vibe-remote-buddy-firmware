# 第三方声明

本仓库中的 ESP32-S3 固件使用 ESP-IDF 5.4.0 及其组件。ESP-IDF、NimBLE、TinyUSB、esp_tinyusb 等源码由依赖管理器取得，原许可保留在依赖包中。mSBC 解码器来自 ESP-IDF 的 Open Interface SBC 解码器，相关原始版权与 Apache-2.0 声明也保留在 ESP-IDF 中。

ICO 解码器为本项目独立实现。构建时从 ITU-T G.722.1 Fixed-200505-Rel.2.1 官方 ZIP 中提取所需数值；官方包的 SHA-256 为 `1b0b5c32495af09249b30fdae68c20a1f2397bf8c69cfef332fac984f2b62580`。官方表文件标注 Copyright 2004 Polycom, Inc.; All rights reserved。官方包和本地生成的数值表不属于本仓库的 MIT 授权，也不随源码发布。固件不编译或链接 PJPROJECT 的 G.722.1 参考代码。

`resources/` 内的共享定义来自项目的 [公开 App 仓库](https://github.com/kxn/vibe-remote-buddy-app)，由同一项目维护者按 MIT 发布。协议与模型定义在固件中独立实现。
