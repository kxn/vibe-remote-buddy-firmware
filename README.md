# Vibe Remote Buddy 固件

ESP32-S3 接收器固件：板子负责蓝牙连接、遥控器按键与语音解码，通过 USB 向 Windows 或 macOS 提供键盘和麦克风。管理、配对和固件更新使用 [Vibe Remote Buddy App](https://github.com/kxn/vibe-remote-buddy-app)。固件可以脱离 App 运行已配置的按键与语音功能。

本仓库只发布可构建的固件源码，不提供预编译固件。构建结果留在你自己的电脑上。

## 编译

需要 ESP-IDF **5.4.0**、Python 3.10+、Git。Windows 安装 ESP-IDF 后设置 `IDF_PATH`；Linux/macOS 先运行 ESP-IDF 的 `export.sh`。从仓库根目录运行：

```sh
python tools/build_firmware.py
```

这个命令检查内置机型定义，然后依次编译三种硬件配置：

| 参数 | 板子配置 | 固件镜像 |
| --- | --- | --- |
| `q2` | 8 MB Flash、2 MB Quad PSRAM | `build/esp32s3-q2/buddy_s3_q2_ab1.bin` |
| `o8` | 8 MB Flash、8 MB Octal PSRAM | `build/esp32s3-o8/buddy_s3_o8_ab1.bin` |
| `q2-f4` | 4 MB Flash、2 MB Quad PSRAM | `build/esp32s3-q2-f4/buddy_s3_q2_f4_ab2.bin` |

只编译一款板子：

```sh
python tools/build_firmware.py --variant q2-f4
```

`build/esp32s3-<配置>/` 还包含首次刷写所需的 bootloader、分区表、OTA 初始化数据及 `flash_args`。选择与你的 Flash 和 PSRAM 一致的配置；不要把单独的应用镜像刷到地址 `0x0`。编译后可用同一构建目录执行 ESP-IDF 刷写，例如：

```sh
idf.py -C firmware/esp32s3 -B build/esp32s3-q2-f4 flash
```

首次编译 ICO 解码器时，构建脚本从 [ITU 官方页面](https://www.itu.int/rec/T-REC-G.722.1-200505-I/en)下载 G.722.1 Release 2.1 软件包，核对固定 SHA-256，并在被忽略的 `build/` 目录生成数值表。下载的包、数值表及查找表均不在仓库内。已有官方 ZIP 时，可设置环境变量 `BUDDY_ITU_G7221_ARCHIVE` 指向它，然后离线编译。构建不会编译或链接 ITU/PJPROJECT 的参考解码器。

## 代码与共享定义

`firmware/` 是板端实现；`protocol/` 包含板端使用的 RBP/3 C 编解码与协议格式；`resources/` 是编译固件所需的机型、按键和语音协议定义。App 使用自己的 TypeScript/Rust 实现。本仓库的 JSON 是与 App 协调的固定版本，固件编译不依赖 App 仓库或子模块。

项目自有源码按 [MIT](LICENSE) 发布。ESP-IDF、TinyUSB、NimBLE、mSBC 等依赖保留各自许可，详见 [第三方声明](THIRD_PARTY_NOTICES.md)。ITU 软件包内的数值有独立的权利声明；从官方包下载并在本机编译，不代表获得分发含表镜像的许可。

修改代码或提交 PR 前请看 [贡献说明](CONTRIBUTING.md)。
