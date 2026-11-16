<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# 本机控制API产品边界

本目录实现 OpenTune Standalone 的本机控制 API。构建需启用 `OPENTUNE_ENABLE_CONTROL_API`，运行时需设置 `OPENTUNE_CONTROL_API=1` 或传入 `--control-api`；监听地址为 `127.0.0.1`。

## 当前入口与启用

- 构建选项：`-DOPENTUNE_ENABLE_CONTROL_API=ON`。
- 运行开关：环境变量 `OPENTUNE_CONTROL_API=1` 或命令行参数 `--control-api`。
- 网络入口：仅监听 loopback `127.0.0.1`。
- `ControlService`：由 processor 持有，负责运行时启停、消息线程接入和 jobs 唤醒。
- `ControlDispatcher`：请求执行入口，负责分发并将产品状态访问送到消息线程。
- `Ops/OpsRegistry`：控制操作的注册表及共享操作上下文。
- `ControlUiBridge` 与 `Source/Standalone/ControlEditorBridge*`：访问编辑器专属状态和行为的桥接入口。
- `Source/PluginProcessor.*` 与 `Source/Standalone/PluginEditor.*`：processor 生命周期和 editor bridge 的运行时接入点。
