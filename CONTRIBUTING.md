# 贡献固件代码

1. 修改 `firmware/`、`protocol/` 或 `resources/` 后，运行 `python tools/generate_models.py` 更新固件内置定义。
2. 运行 `python tools/generate_models.py --check`，再用 `python tools/build_firmware.py --variant q2-f4` 验证至少一种目标。若只改文档或构建工具，说明实际验证范围即可。
3. PR 里写明修改目的、测试结果、使用的板型和任何实机验证。没有做的实机检查请直接说明。

请勿提交构建目录、固件镜像、录音、抓包、设备地址、配对密钥、ITU 软件包或由它生成的码表。固件与 App 的协议实现分别维护；若改动 `resources/` 或 RBP/3 格式，请在 PR 中指出 App 端需要同步的内容。
