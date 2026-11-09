# ProcessRenderRuntime 共享后端生命周期收敛计划

日期：2026-11-06
状态：架构修订，待实施
范围：Standalone、VST3、AU 及其各自编译的 SharedCode runtime

本文是 `ProcessRenderRuntime`、`VocoderDomain` 和 DML Session 生命周期的当前契约。它更新并覆盖 [2026-09-29 渲染生命周期计划](2026-09-29-render-runtime-lifecycle-refactor-plan.md) 中与 runtime owner shutdown/reopen 相冲突的条款，尤其是 §2.5、§6.5、§10.2、§10.11。RenderCache、Scheduler、ARA、线程安全与测试条款继续有效，除非本文明确调整。

## 1. 目标

`VocoderDomain` 与其中的 DML Session 属于其 `ProcessRenderRuntime` 所在二进制/插件模块的共享后端。Processor 或 ARA DocumentController 析构只清理自身任务和回调；活跃 owner 数归零不停止 control worker，不销毁 Domain，不触发 shutdown/reopen。runtime 尚在其规定宿主边界内时，后续实例复用同一 Domain/Session。

本契约不要求不同插件格式之间共享同一个 Session。Standalone、VST3、AU 等目标分别编译 SharedCode，各自拥有目标内 runtime。

## 2. 生命周期边界

| 目标 | owner 数归零 | 最终关闭边界 |
| --- | --- | --- |
| Standalone | runtime、control worker、Domain、DML Session 保持可复用 | Standalone 应用关闭前，由应用级退出协调器关闭 runtime，并在 MessageManager 停止前完成 worker/scheduler 收敛与 join |
| Windows VST3 | runtime、control worker、Domain、DML Session 保持可复用 | 确认 `Vst3ModulePin` 成功后，保持至宿主进程终止；不在 Processor 析构或 loader detach 中关闭 DML |
| 未 pin 的插件模块 | owner 析构不关闭 backend；同一模块仍加载时后续实例复用 | 插件模块卸载前，经宿主支持的模块级退出路径关闭 runtime；该路径在 module code unload 前完成 join |

Windows VST3 的 pin 返回值必须可检查。未验证 pin 成功的构建按“未 pin 模块”执行。模块退出不得依赖 loader lock 内执行耗时 DML 析构；模块级退出入口必须能在代码映像卸载前协调完成异步 shutdown。

模块级单例不是跨模块进程级单例。不得以本计划推导 VST3、AU 和 Standalone 在同一宿主进程共享同一个 C++ 对象。

## 3. 所有权合同

### 3.1 Shared backend

`ProcessRenderRuntime` 持有唯一 control queue、control worker 与 `VocoderDomain`。`VocoderDomain` 持有 Scheduler 和 DML Session。Ensure 去重、模型权重切换、backend reset 和 Domain 重配继续由 control worker 串行处理。

Backend 在 owner 数归零时保持 idle：control worker 阻塞等待已有 control queue，不轮询、不使用 timer。真正的最终关闭只由 §2 的 Standalone app 或 plugin module 生命周期入口发起。

Runtime 初始化保持惰性。构造 Processor、插件扫描对象或 ARA wrapper 不得仅为计 owner 而启动 worker 或加载模型。worker 在首次实际 backend/control 操作时启动。

### 3.2 Processor / DocumentController owner

owner 析构保留现有自身清理顺序：

1. 在消息线程关闭 completion gate；ARA DocumentController 构造、析构、模型访问和通知继续遵守消息线程单线程所有权。
2. 停止并 join owner 自己的 F0 extraction worker。
3. detach 自己的 CRS execution lease，收敛同步和异步 in-flight render；owner 级 deferred retry 从共享 runtime 路由中移除。
4. 收敛该 owner 提交的 control transaction，释放其 CRS pause/引用并完成 ack；gate 关闭后不访问该 owner 或其 cache。
5. 析构 owner 对象。

owner teardown 不发送 backend Stop，不 join `ProcessRenderRuntime` control worker，不销毁共享 Domain/DML Session。当前 `detachExecutionLease()` 等待 owner 的在途推理完成；本阶段保留该等待语义，不增加 DML job 的 owner 级强制取消协议。

### 3.3 全局 control transaction

权重、vocoder 和 inference backend 设置作用于共享 runtime，不能以发起命令的 owner 作为其最终状态所有者。

- 已被 control worker 接受的全局 Domain 操作完成后更新 runtime 状态，即使来源 owner 已析构。
- 仍在 queue 中且来源 owner 已关闭的 owner-bound command 在执行 Domain 操作前取消，并通过现有 transaction 完成路径释放引用、按 paused 状态收敛 CRS、完成一次 ack。
- 已进入 Domain 操作的 command 完成当前原子操作；来源 owner 关闭后不执行 owner/cache completion，仍完成 CRS resume、引用释放和 ack。
- 来源 owner 的 UI/cache completion 受该 owner gate 控制；gate 关闭后只完成 transaction 收尾、CRS resume、引用释放和 ack。
- 改变共享 backend 后，对仍注册的活跃 CRS 在消息线程执行 cache 失效/重渲染通知。该路由按当前活跃 owner 注册集工作；不保留已经关闭的 owner。注册/注销绑定 CRS lease 生命周期，不参与 backend 创建或销毁。
- 每条 transaction 只 pause/resume 一次并 ack 一次。control worker 在 ack 前不执行下一条命令。

不得因移除 owner-driven shutdown 而删除 transaction 的 ack、paused、one-shot completion 或 gate 约束。

## 4. 最终关闭协议

最终关闭入口属于 Standalone 应用生命周期或未 pin 插件模块生命周期，不属于 `releaseOwner()`。

最终关闭按以下顺序执行：

```text
应用/module 生命周期停止接收新 backend 命令和 render job
    -> 等待 owner 自身 gate、CRS lease 和 owner transaction 收敛
    -> control queue 排入唯一 Stop
    -> control worker 完成已接受事务并停止 Domain/Scheduler
    -> DML active Run 收到 terminate，Scheduler worker 退出并 join
    -> control worker 发布退出/dispatch 结果并退出
    -> message thread 处理最终 completion 并 join control worker
    -> runtime final shutdown 完成
    -> MessageManager 停止或 plugin module unload
```

消息线程不得同步等待一个尚需消息线程 ack 的 transaction。Standalone 退出协调器在最终 completion 到达前保持 message dispatcher 可用，并在完成后继续应用退出。未 pin 插件模块使用宿主提供的 pre-unload 生命周期入口；shutdown/join 不运行在 loader lock 内。

Issue #43 的 worker-exit 丢失唤醒随最终关闭协议一并修复：worker 在与 waiter 相同的 mutex 下发布退出谓词，解锁后通知；每个等待都检查对应 predicate；需要 join 的 message callback 先完成退出等待并释放 mutex，再 join 已退出的 worker。最终关闭只有一代，不保留 owner reopen 所需的 generation 状态。

Standalone 应用级关闭和未 pin module unload 均为一次性关闭，不重新创建 worker。Windows VST3 process termination 路径不执行 DML teardown。

## 5. 代码迁移范围

### 阶段 A：建立宿主最终关闭入口

- 为 JUCE Standalone 增加应用级退出协调器，在 MessageManager teardown 前请求异步 runtime final shutdown，并在 shutdown completion 后继续退出。
- 为未 pin 的 plugin targets 确认并接入 module pre-unload shutdown 路径；覆盖 AU、macOS VST3 和其他未 pin 格式。
- 检查 Windows VST3 module pin 的实际成功状态，并验证宿主卸载请求后模块保持加载至进程终止。

验收：三类宿主边界均有可观测入口；没有 owner destructor 或 loader-lock callback 承担全局 backend shutdown。

### 阶段 B：分离 owner 清理与 backend 生命周期

- 移除 `ownerCount_` 驱动的 `shutdownLocked()`、Stop、control worker join 和 `retainOwner()` reopen。
- Processor 和 DocumentController 删除仅用于 backend refcount 的 retain/release 调用；保留 owner/CRS 注册、completion gate、F0 worker shutdown、CRS detach、deferred-job 清理。
- `ProcessRenderRuntime` 构造只建立惰性状态；首次实际 backend/control 操作启动唯一 control worker。
- 删除只服务于 owner shutdown/reopen 的 `shutdownGeneration_` 和旧 worker dispatch 状态；保留最终关闭所需的 Stop、worker-exit handoff 与 join 机制，并按 §4 修复通知协议。

验收：owner 数归零后 Domain/Session live count 不变、worker 仍可接收命令；后续 owner 使用同一个 Domain；扫描路径不启动 backend。

### 阶段 C：闭合 owner transaction 与共享状态通知

- 让已接受的全局 backend 命令与来源 owner completion 解耦。
- owner detach 能收敛其 queued/active transaction，保证 CRS 恢复和 ack；已执行的 runtime-global 设置继续发布。
- 将共享 backend 改变通知当前活跃 CRS，保证多个 owner 的 cache 与 runtime generation 一致。

验收：来源 owner 在 queued、Domain operation、message completion、ack 各阶段析构，control worker 均继续处理后续命令；其他活跃 owner 收到且只收到一次 backend-change 通知。

### 阶段 D：测试迁移

- 将 `RenderRuntimeLifecycleTests` 从 last-owner shutdown/reopen 改为 zero-owner persistence、后续实例复用与多 owner 隔离。
- 增加 owner A 在途 DML 推理期间 teardown、owner B 继续提交的真实 Scheduler/CRS 测试。
- 增加来源 owner 退出时 queued/active transaction 收敛和共享状态广播测试。
- 增加 Standalone app final shutdown 测试：message dispatcher 保持运行至 worker、Scheduler、DML Run 全部收敛，再继续 app quit。
- 增加未 pin module pre-unload 测试和 Windows VST3 pin 成功/驻留验证。测试覆盖分别按构建目标执行，不以一个 SharedCode test target 代替 module pin 验收。
- 移除 `ControlProcessorSupport.h` 中永久 retain owner 以绕过 shutdown 的 workaround。

## 6. 必须保持的运行时不变量

```text
Domain 创建、重配、最终销毁由唯一 control worker 执行；
旧 Scheduler shutdown/join 后才替换或销毁 Domain；
domainSubmitInFlight_ 归零后才释放 Domain；
owner gate 关闭后不访问 Processor、DocumentController 或其 cache；
ARA model access、F0 completion 和 DocumentController 析构保持 message-thread-only；
每条 ControlTransaction 恰好一次 resume 和 ack；
最终关闭中所有实际关闭的 worker/Scheduler 均 join 后再卸载其代码模块；
owner 数归零不改变共享 Domain/DML Session 的存活状态。
```

不得增加 timer、忙等、后台心跳、每 owner 的 DML Session 或跨插件格式共享层。

## 7. 范围外

- 不重写 DML inference queue、Ensure 去重、Domain generation 或 render-cache 算法。
- 不新增 owner 级 DML 强制取消；析构继续等待该 owner 的已接受 render work 收敛。
- 不要求 AU、VST3、Standalone 共享一个跨模块 Session。
- 不执行源码实现、构建或提交；这些工作在用户确认计划后按阶段进行。
