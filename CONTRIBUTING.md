# 贡献固件代码

1. 修改 `firmware/`、`protocol/` 或 `resources/` 后，运行 `python tools/generate_models.py` 更新固件内置定义。
2. 运行 `python tools/generate_models.py --check`，再用 `python tools/build_firmware.py --variant q2-f4` 验证至少一种目标。若只改文档或构建工具，说明实际验证范围即可。
3. PR 里写明修改目的、测试结果、使用的板型和任何实机验证。没有做的实机检查请直接说明。

请勿提交构建目录、固件镜像、录音、抓包、设备地址、配对密钥、ITU 软件包或由它生成的码表。固件与 App 的协议实现分别维护；若改动 `resources/` 或 RBP/3 格式，请在 PR 中指出 App 端需要同步的内容。

修改 HID 初始化时，可在有 C11 编译器的环境运行主机回归测试：

```sh
cmake -S tests -B build/adapter-tests
cmake --build build/adapter-tests
ctest --test-dir build/adapter-tests --output-on-failure
```

Windows 的 Visual Studio Developer Command Prompt 可在配置时加 `-G "NMake Makefiles"`。
测试直接驱动生产适配器，覆盖 Protocol Mode 的正常切换、空值兼容边界、完整旧款 HID 发现与订阅、缺失语音通道、录音启停帧、缓存恢复和提交重试；不能替代遥控器实机录音验证。

`host_hid` 另外覆盖 USB 主机识别的等待、复位与歧义边界，语音预设和自定义映射，以及生产 HID 描述符中的 Globe 位、键盘和媒体键布局。它不模拟 macOS 的 HID 驱动或豆包快捷键处理；Fn / Globe 功能仍需按 README 在 Mac 上实测。
