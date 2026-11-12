# ProcessRenderRuntime 共享后端生命周期计划

日期：2026-11-06（2026-11-09 修订）
状态：当前契约，待实施
范围：Standalone、Windows VST3、macOS VST3；AU 保留现状

本文是 `ProcessRenderRuntime`、`VocoderDomain` 和 DML Session 生命周期的当前契约。与 [2026-09-29 渲染生命周期计划](2026-09-29-render-runtime-lifecycle-refactor-plan.md) 冲突的条款以本文为准：owner 数归零保留共享后端，最终关闭只由应用/模块退出路径完成。

## 1. 目标与范围

`VocoderDomain` 与其中的 DML Session 属于其 `ProcessRenderRuntime` 所在二进制/插件模块的共享后端。Processor 或 ARA DocumentController 析构只清理自身任务、回调和事务完成通知；活跃 owner 数归零不停止 control worker、不销毁 Domain、不触发 shutdown/reopen。同一模块内的后续实例复用同一个 Domain/Session。

Standalone、Windows VST3、macOS VST3 分别编译 SharedCode，各自拥有目标内 runtime；不同插件格式不共享 Session。

AU 模块寿命策略未定，本次 AU 保留现有 owner-count shutdown/reopen 行为。共享 transaction 正确性修复适用于所有格式；owner-count 移除只适用于 Standalone/VST3。

## 2. 生命周期边界

| 目标 | owner 数归零 | 最终关闭边界 |
| --- | --- | --- |
| Standalone | runtime、control worker、Domain、DML Session 保持可复用 | 自定义 Standalone 应用/窗口的退出协调器：保存插件状态，在消息线程销毁 Processor/owner，异步完成 runtime 最终关闭后 quit |
| Windows VST3 | 保持可复用 | `Vst3ModulePin` 成功后模块驻留至进程终止；不在 `ExitDll` 或 loader detach 中关闭 backend |
| macOS VST3 | 保持可复用 | 最后一个 `bundleExit` 引用退出时同步关闭 backend，返回前完成 join |
| macOS AU | 保留现有 owner-count 生命周期 | 本次不改变 AU backend 最终关闭边界 |

JUCE 8.0.12 的 VST3 client 导出同步 `ExitDll` 和 `bundleExit`。macOS `bundleExit` 可因多个 bundle 引用而多次调用，只有最后一个引用退出时关闭 backend；关闭不依赖消息线程回调或调用线程。Windows VST3 必须先成功 pin；pin 失败则模块初始化失败。宿主不提供异步 pre-unload hook。

Windows 进程终止路径不执行阻塞 DML teardown，不在 loader detach 中等待 worker。

ARA 的寿命对象是 DocumentController 和模型图；二进制模块寿命与 ARA 对象寿命独立，ARA 不提供模块 pre-unload 回调。DC 创建/销毁、模型访问和模型通知只在消息线程执行；宿主决定 Processor 与 DC 的销毁顺序，两种顺序都支持。

模块级单例只属于单一模块；VST3、AU 和 Standalone 不在同一宿主进程共享 C++ 对象。

## 3. 所有权合同

### 3.1 共享后端

`ProcessRenderRuntime` 持有唯一 control queue、control worker 和 `VocoderDomain`；`VocoderDomain` 持有 Scheduler 与 DML Session。Ensure 去重、权重切换、backend reset 和 Domain 重配由 control worker 串行执行。

Runtime 初始化保持惰性：构造只建立初始状态；首次实际 backend/control 操作启动唯一 control worker。Processor 构造、插件扫描和 ARA wrapper 构造不启动 worker、不加载模型。

owner 数归零后 Standalone/VST3 backend 保持 idle：worker 阻塞在 control queue，不使用 timer 或轮询。`retainOwner()`/`releaseOwner()` 与 `ownerCount_` 驱动的 shutdown/reopen 仅在 Standalone/VST3 编译路径删除；AU 保留现有行为。

### 3.2 Processor / DocumentController owner

owner 析构按以下顺序执行：

1. 在消息线程关闭 completion gate；ARA DC 构造、析构、模型访问和通知保持消息线程单线程所有权。Processor 先于 DC 销毁和 DC 先于 Processor 销毁都是合法顺序。
2. 停止并 join 该 owner 的 F0 extraction worker，关闭该 owner 的 F0 completion 路由。
3. detach 自己的 CRS execution lease，等待同步和异步 in-flight render 收敛。
4. 移除该 owner 的 deferred retries。
5. 该 owner 已接受的 control transaction 由 worker-owned completion 收敛：worker 执行 pause/resume、释放 CRS strong reference、完成一次 ack；owner 不取消、不代完成。completion gate 只控制 owner-specific UI 回调；cache invalidation 由共享 CRS 通知统一处理。
6. 清除 owner 状态并析构 owner 对象。

owner teardown 不发送 Stop，不 join control worker，不销毁 Domain/DML Session，不执行 owner 级 DML 取消。

### 3.3 Control transaction

- 成功入队是唯一线性化/接受点：在 control queue 锁内 push 成功即 accepted。入队被拒绝时没有 Domain 操作，不持久化这次选择，提交以拒绝结果完成。
- 每条 accepted 的共享 backend 设置执行到完成，不受来源 owner 生命周期影响。accepted 的 queued 设置不取消。
- worker 拥有 pause、resume、CRS strong reference 释放和一次 ack。ack 与消息回调独立；worker 在 transaction mutex 下发布 ack 谓词并 notify，等待者在同一 mutex 下检查谓词，无丢失唤醒。
- message completion 独立投递；执行前检查 owner completion gate，gate 关闭后跳过 owner-specific UI 回调，不访问 owner 或 DocumentController。worker 不等待 message completion 才处理下一条命令。
- 活跃 CRS 注册跟随现有 execution lease 的 attach/detach。成功且有效的 backend/generation 改变在消息线程通知每个当前注册的活跃 CRS 一次；该通知是 cache invalidation 与重渲染的唯一入口。Unchanged、Failed 和拒绝不通知。注册与通知复用现有 CRS/RenderWorker 路由，不新增全局框架。
- 每条需要 pause 的 accepted transaction 恰好一次 pause/resume 和一次 ack。
- 偏好写入只在 enqueue 返回 accepted 后发生；入队拒绝保持已持久化偏好不变。

## 4. 最终关闭协议

```text
所有 owner 已析构
    -> control queue 锁内停止接收新命令
    -> 在已接受命令之后追加唯一 Stop
    -> worker 执行完所有已接受命令
    -> 销毁 Domain/Scheduler，终止并 join active DML Run
    -> worker 在 shutdown mutex 下发布退出谓词，释放锁后通知
    -> macOS VST3 不投递消息；bundleExit 等待谓词、解锁并 join 后返回
    -> Standalone worker 投递消息 completion；消息线程 join 后 quit
```

最终关闭入口属于 Standalone 应用或 VST3 模块，不属于 Standalone/VST3 owner 析构。关闭开始后不再接受命令；已接受命令全部执行，不取消。AU 仍由最后 owner 触发现有 shutdown。

Stop 是唯一的队列终止命令；worker 消费 Stop 后销毁 Domain/Scheduler 和 active DML Run，发布退出谓词。最终关闭只有一代；不重建 worker、不保留 owner reopen 所需的 generation 状态。

macOS VST3 最后一个 bundle 引用退出时同步 join：关闭路径不投递消息，`bundleExit` 等待 worker 退出并直接 join，不依赖消息回调。Windows VST3 pin 成功后不执行模块级 backend shutdown。Standalone 用异步关闭：请求后消息循环继续运行，worker 投递 completion，消息线程 join 并 quit。

### 4.1 Standalone

现有默认窗口 `closeButtonPressed()` 先 `savePluginState()` 再 `JUCEApplicationBase::quit()`；Control API 通过 `ControlEditorBridge::requestQuit()` 调用这个虚方法。新 Standalone 应用/窗口使用 `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP`，OS quit、窗口关闭和 Control API 关闭进入同一个退出协调器。

协调器顺序：保存插件状态一次（保持现有保存语义）→ 在消息线程销毁 Processor/owner → 请求 runtime 最终关闭并立即返回 → 消息循环处理最终 completion（join worker）→ 调用 `quit()`。`shutdown()` 只清空已经关闭的应用对象。

Control API 保持 after-reply 语义：`app.quit` 结果先写回连接，退出步骤由 dispatcher 在消息线程执行；先停 control service，再关闭窗口。协调器不改变这个顺序。退出路径不新增 timer。

### 4.2 VST3 模块退出

macOS `bundleExit` 是同步 pre-unload 路径，但每个 bundle 引用退出都会调用 JUCE `shutdownModule()`。通过现有 JUCE patch 只在最后一个 bundle 引用退出时调用 OpenTune 同步最终关闭：停止接受、追加 Stop、等待 worker 退出并 join，然后返回。关闭不依赖消息分发器存活。

Windows VST3 的 `GetModuleHandleExW(... PIN)` 结果必须验证成功；失败则模块初始化失败。成功后模块驻留至进程终止，owner 数归零后的 backend 继续复用。`ExitDll` 和进程终止路径不执行阻塞 DML teardown，不在 loader detach 中等待 worker。

VST3 client 不提供宿主异步 pre-unload hook。AU 的 owner-count backend 生命周期保持不变。

### 4.3 Issue #43 丢失唤醒

worker 在 `shutdownMutex_` 下发布 worker-exit 谓词，释放锁后 notify；等待者在同一 `shutdownMutex_` 下检查谓词。Standalone worker 在发布谓词前记录 message completion 投递结果，投递失败形成显式硬失败；macOS VST3 worker 不投递消息，`bundleExit` 等待谓词后解锁并直接 join。`finishShutdown()`/join 在谓词等待返回并释放 mutex 后执行。

## 5. 实施阶段

### 阶段 A：建立退出契约

- 用 `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP` 建立 Standalone 应用/窗口退出协调器，覆盖 OS quit、窗口关闭和 Control API 关闭。
- 在 macOS VST3 `shutdownModule()` 接入同步最终关闭，通过现有 `scripts/github_actions/patch-juce-ara.py` 的 needle patch 落地；验证 Windows pin 成功并让 pin 失败时模块初始化失败。
- 验收：Standalone 和 macOS VST3 有最终关闭入口，Windows VST3 验证进程寿命驻留；owner 析构和 loader-detach 路径不承担全局 backend shutdown。

### 阶段 B：worker-owned transaction 与活跃 CRS 通知

- control command 入队返回 accepted/rejected；偏好写入使用 accepted 结果。
- worker-owned transaction settlement：pause/resume、CRS 引用释放、一次 ack；message completion 独立并检查 gate。
- 活跃 CRS 注册跟随 execution lease；有效 backend/generation 改变在消息线程通知一次。
- 验收：来源 owner 在 queued、running、completion 阶段析构时设置仍完成；拒绝不改持久化偏好；另一活跃 CRS 恰好一次通知。

### 阶段 C：连接应用/模块最终关闭

- Standalone 异步最终关闭；macOS VST3 同步最终关闭。
- worker-exit 谓词与等待 mutex 配对；`finishShutdown`/join 在等待之外执行。
- 验收：Standalone 在 completion 后 quit；macOS VST3 `bundleExit` 返回前 join；Windows VST3 pin 驻留；无丢失唤醒。

### 阶段 D：移除 ownerCount shutdown/reopen

- Standalone/VST3 删除 owner-count shutdown/reopen 与 owner reopen 状态；AU 保留 `retainOwner()`/`releaseOwner()` 路径。worker 在首次实际操作时启动。
- owner 析构删除 Stop、全局 join、Domain/DML 销毁和 owner 级 DML 取消。
- 验收：owner 归零后 Domain/Session 保持 live；后续实例复用同一 Domain；扫描路径不启动 backend。

### 阶段 E：替换生命周期测试

- zero-owner same Domain+Session reuse；断言 Session 生命周期计数/身份，不以 runtime 地址或 generation 代替。
- queued 与 running accepted operation 在来源 owner teardown 下完成；零 dead-owner callback；恰好一次 worker resume/ack；另一活跃 CRS 恰好一次通知。
- Standalone OS quit、窗口关闭、Control API 三条退出路径、保存语义、final close 先于 quit。
- VST3 pin 成功；macOS 最后一个 `bundleExit` 引用退出时 pre-unload 同步关闭。
- AU owner-count shutdown/reopen 行为保持现状。
- ARA harness：消息线程 DC teardown，DC/Processor 两种合法销毁顺序；不测试模块卸载。
- 移除 `ControlProcessorSupport.h` 的永久 retain workaround。

测试按构建目标执行；不建立一个跨目标 SharedCode 测试替代模块 pin 验收。

## 6. 必须保持的运行时不变量

```text
Domain 创建、重配、最终销毁由唯一 control worker 执行；
旧 Scheduler shutdown/join 后才替换或销毁 Domain；
domainSubmitInFlight_ 归零后才释放 Domain；
owner gate 关闭后不访问 Processor、DocumentController 或 cache；
ARA 模型访问、F0 completion、DC 构造/析构保持 message-thread-only；
每条需要 pause 的 accepted transaction 恰好一次 pause/resume 和一次 ack；
accepted 设置不因来源 owner 析构而取消；
最终关闭中的 worker/Scheduler/DML Run 全部 join 后才卸载模块；
owner 数归零不改变 Domain/DML Session 的存活状态。
```

不增加 timer、忙等、后台心跳、每 owner 的 DML Session 或跨插件格式共享层。

## 7. 范围外

- AU 的模块寿命策略；本次保留现有 owner-count 行为，不扩展 AU 最终关闭边界。
- 不重写 DML inference queue、Ensure 去重、Domain generation 或 render-cache 算法。
- 不做广泛重构；不新增 owner 级 DML 强制取消。
- 不改变退出与状态保存之外的 user-facing 行为。
- 不添加 timer。
- 本计划定义契约；实现按阶段另行执行。

## 8. 依据

- Standalone：`JUCE-master/modules/juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h:796-804`（close 保存插件状态后 `JUCEApplicationBase::quit()`）；`JUCE-master/modules/juce_audio_plugin_client/juce_audio_plugin_client_Standalone.cpp:148-175,222-230`（`shutdown()`、`systemRequestedQuit()`、custom app macro）；`JUCE-master/modules/juce_events/messages/juce_ApplicationBase.h:151-163,174-186`（dispatch loop 停止后 `shutdown()`；OS quit 进入 `systemRequestedQuit()`）。
- OpenTune Standalone/Control：`Source/Standalone/ControlEditorBridge.cpp:193-203`；`Source/Control/Ops/AppQuitOps.h:33-38`。
- VST3 模块退出：`JUCE-master/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp:4015-4048,4058-4098`（同步 `ExitDll`、`ModuleExit`、`bundleExit`；macOS 只在最后 bundle ref 退出时关闭）。
- Runtime 现状：`Source/Runtime/ProcessRenderRuntime.cpp:945-952`（worker 发布 shutdown 谓词）；`:1064-1073`（worker 等待投递到消息线程的 ack）；`Tests/Control/ControlProcessorSupport.h:192-217`（永远 retain owner 的 workaround）；`Tests/RenderRuntimeLifecycleTests.cpp:734-769`（shutdown/reopen 测试）。
- 偏好：`Source/Editor/Preferences/SharedPreferencePages.cpp:252-260`（先写偏好再投递 control command）。
- Windows pin：`Source/Utils/Vst3ModulePin.cpp:35-50`（`GetModuleHandleExW` 返回值当前未检查）。
- ARA：[`ARAInterface.h`](https://github.com/Celemony/ARA_API/blob/main/ARAInterface.h)（Document 与 DocumentController 1:1；模型编辑由 `beginEditing()`/`endEditing()` 编辑周期保护，用于渲染同步）；[`ARAVST3.h`](https://github.com/Celemony/ARA_API/blob/main/ARAVST3.h)（DC 在 plug-in in use 期间有效；Processor 与 DC 销毁顺序未定义，两种顺序都必须支持）。
- 本地 JUCE checkout 为 8.0.12（`JUCE-master/modules/juce_core/juce_core.h:47`），本文引用的行号与该 checkout 一致。
