# 贡献固件代码

1. 修改 `firmware/`、`protocol/` 或 `resources/` 后，运行 `python tools/generate_models.py` 更新固件内置定义。
2. 运行 `python tools/generate_models.py --check`，再用 `python tools/build_firmware.py --variant q2-f4` 验证至少一种目标。若只改文档或构建工具，说明实际验证范围即可。
3. PR 里写明修改目的、测试结果、使用的板型和任何实机验证。没有做的实机检查请直接说明。

请勿提交构建目录、固件镜像、录音、抓包、设备地址、配对密钥、ITU 软件包或由它生成的码表。固件与 App 的协议实现分别维护；若改动 `resources/` 或 RBP/3 格式，请在 PR 中指出 App 端需要同步的内容。

修改主机识别、HID、快捷键或语音状态机后，在有 C11 编译器的环境运行主机回归测试：

```sh
cmake -S tests -B build/adapter-tests
cmake --build build/adapter-tests
ctest --test-dir build/adapter-tests --output-on-failure
```

Windows 的 Visual Studio Developer Command Prompt 可在配置时加 `-G "NMake Makefiles"`。
测试直接驱动生产适配器，覆盖 Protocol Mode 的正常切换、空值兼容边界、完整旧款 HID 发现与订阅、缺失语音通道、录音启停帧、缓存恢复和提交重试；不能替代遥控器实机录音验证。

`host_hid` 覆盖 USB 主机识别的等待、复位与歧义边界，包含 macOS 无 255 字节请求时的枚举回归；同时检查语音预设、自定义映射，以及生产 HID 描述符中的原生 Fn（`0x00ff/0x03`）位、键盘和媒体键布局。它不模拟 macOS 的 HID 驱动或豆包快捷键处理；Fn 功能仍需按 README 在 Mac 上实测。

`voice_runtime` 使用最小的时钟、锁和解码器替身，直接驱动生产语音状态机，覆盖麦克风未打开、延迟打开、提前关闭、正常尾音完整发送、下一次语音可正常开始及计时回绕。

`shortcut` 覆盖 Power、Power、方向键的两秒边界、计时回绕、长按重复、误触、USB/蓝牙连接变化、跨遥控器隔离、平台独立覆盖、语音配置优先级和原键盘/动作输出不变。`shortcut_store` 使用 NVS 替身驱动生产存储代码，检查三平台分别保存与重新加载、无效值过滤、重复写入抑制及失败重试。管理命令和绑定存储格式保持不变；App 展示硬件覆盖需要读取只读 `STATS {"index":82}`。

实机检查应退出 App，在 Windows/macOS/Linux 上分别设置不同按键，核对语音输入及原 Power/方向键动作；检查超时无效、Windows/Linux 的“下”不改设置，以及松键稍等后断电重插仍保留各平台的选择。主机测试不替代 USB 主机识别、输入法激活和实际闪存掉电保持的验证。

## USB 身份与 Fn 验证

当前 USB 身份为 `05AC:0220`，设备描述符版本为 `0x0402`，匹配 macOS 的 `Wired Keyboard 2007 ANSI Map` / `AppleHIDKeyboardEventDriver`。确认 `AppleVendorSupported=true`、Fn Usage 为 `0x00ff/0x03`，并检查实际系统事件。仅看到原始 HID 数据或 Fn 状态位不足以证明输入法兼容：长按应只有一次 Fn 按下和松开，松开后没有额外键码 `179`；短按允许系统产生 `179`。

Fn 保持在报告 1 的第二字节最低位，与普通键盘状态一起发送；报告 1 为 8 字节、报告 2 为 2 字节。产品名称、独立序列号和其他接口布局保持不变。VID/PID 作用于整个 CDC/HID/UAC 复合设备，更新身份时必须检查串口、音频和各平台驱动；App 需要同步设备发现规则，并避免把旧 Fn 软件转发器用于新身份。

排查主机识别可读取 `INFO.host_os`（`0` 未知、`1` Windows、`2` macOS、`3` 其他、`4` Linux）及 `STATS {"index":80}` 的枚举计数。为保持诊断兼容性，`globe_requested` 字段沿用旧名称，表示固件希望发送的 Fn 电平，不代表系统或输入法已收到。管理会话可能影响维护频率，诊断采样与退出 App 后的独立语音测试应分开进行。

## 功耗与发布配置

修改发布构建设置后，运行 `python tests/test_build_config.py`，验证旧 `sdkconfig` 的省电开关和时钟选择会被更新、禁用项校验有效。改变无线或任务调度策略后，还应在接收器上核对 `STATS {"index":81}`、前台/后台扫描切换、遥控器唤醒重连和连续语音输入；主机测试不能代替射频与录音实测。
