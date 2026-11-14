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

以上列出当前实现入口；下节确定后续收敛目标与验收基准。

## 目标/后续收敛要求

- API 关闭时保留 OpenTune 原有产品行为。
- 以已有 UI 产品行为为基准；GUI 与 API 是同一产品命令的不同入口。
- MCP 是外部适配层，不持有另一套工程语义、编辑规则或状态。
- API 负责连接、鉴权、参数校验、对象引用和结果封装；产品操作负责业务约束、数据提交、撤销和状态通知。
- GUI 与 API 共用工程、内容、撤销历史、dirty、选择和渲染状态。
- 同一初始状态下，同一操作的参数含义、修改范围、撤销粒度、保存结果和音频结果保持一致；选择与视图操作保留产品已有的明确副作用。
- 编辑器依赖的交互（例如对话框）必须显式表达所需状态或用户动作，不能伪装为成功。
- 时间使用产品绝对时间和现有精度，不添加换算或取整。
- 控制状态访问遵守消息线程单线程所有权，与 ARA 模型访问、F0 completion 和析构保持一致；后台工作完成由状态变更事件通知。
- 复用产品命令和状态；API 的请求和 job 标识只关联产品任务，完成、失败与取消结果来自真实任务状态。
- 验收以真实导入、编辑、撤销、保存/重开、导出工作流比较 GUI/API 的用户可见结果；当前轮不新增测试。

## 优先收敛的产品操作

1. 选择与撤销：统一结算待完成的参数手势、提交操作及更新 dirty。当前 `ControlEditorBridgeSelection.cpp` 中的 `select` 取消手势，UI 选择入口提交手势；`UndoOps.cpp` 中的 `runStep` 与 bridge 的撤销收尾重复标记 dirty。
2. 工作区切换：由实际参数提交决定保存状态。当前 `ControlEditorBridge::settleViewSwitch` 根据焦点、BPM 和 dirty generation 推断后清除标记，应收敛到共享的视图与参数提交入口。
3. 片段创建与重开工程：由内容仓库统一维护有效 ID 分配和对象生命周期。当前 `controlSkipUsedClipIds` 创建并释放探测片段推进计数器，应由产品自身的 ID 分配能力承担。
4. 异步操作结果：导入、分析、渲染、保存和导出的完成状态来自真实产品任务；请求受理、内容提交和渲染完成分别表达。
5. 启动行为：分别核对编译关闭、运行关闭与运行开启的真实工作流。当前 bridge 在 API 启用时跳过首次演示，`OPENTUNE_DATA_DIR` 独立于运行开关重定向偏好和日志；这些行为需按原有产品体验与显式启动配置统一定义。

## Hooks

本节保留现有代码注释使用的 `README.md#hooks` 锚点。注释标出 processor 生命周期、editor bridge、数据目录、选择和撤销等控制 API 运行时接入点。
