# VST3 ARA Multi-Item Birth 与 Editor 重开恢复结构性修复方案
**Date:** 2026-05-26  
**Status:** Implemented - automated closure on 2026-05-27; REAPER manual test explicitly not required for this task closure  
**Verification source:** `.planning/plans/2026-05-26-vst3-ara-multi-item-birth-and-editor-reopen-test-verification.md`  
**Related plans:**  
- `.planning/plans/2026-05-15-vst3-ara-multi-region-standard-fix-plan.md`  
- `.planning/plans/2026-05-26-vst3-ara-originalf0-final-convergence.md`

## 目标

把这两个现象收敛为同一个生命周期问题，并用最少结构一次修完：

1. Reaper 多 item 场景下，`Read Audio` / auto-birth 不能再只稳定落到最后一个 item。
2. 关闭再打开 VST3 ARA editor 后，已有 materialization/binding 不能丢；窗口重建不应触发重新计算，更不允许 UI 反向销毁已有 binding。

本次方案明确拒绝“补一层 fallback，再补一层恢复，再补一层兜底”的修法。  
只保留三个严格约束：

- `AudioModification persistentId` 是唯一 editable owner
- `VST3AraSession` 是唯一 birth 调度者
- `PluginEditor` 是纯读者，绝不改 binding 真相

## 执行契约

本节是实现约束，不是建议。执行子代理不得自行放宽。

### 允许修改的文件范围

本任务默认只允许修改下列文件：

- `Source/ARA/VST3AraSession.h`
- `Source/ARA/VST3AraSession.cpp`
- `Source/Plugin/PluginEditor.h`
- `Source/Plugin/PluginEditor.cpp`
- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- 与本问题直接相关的 `Tests/*ARA*` / `Tests/*PluginEditor*` / `Tests/*Processor*`
- 本计划文档与对应测试验证文档

如果实现者认为还必须改其它生产文件，必须先停下，回写计划，再由主线程明确批准。不得偷偷扩散到 renderer、CaptureSession、GAME、RMVPE、UI 渲染样式等无关模块。

### 明确禁止

本任务禁止出现以下做法：

- 新增第二套 birth owner、第二套恢复 owner、第二套 editor 恢复真相
- 新增 fallback birth、retry timer、editor 自救重绑、host 特判
- 保留 source 级 birth 队列作为真实调度路径
- 把 `preferredRegion_` 升级为恢复真相或 birth owner
- 新增 focus-hint 持久化作为这次问题的解决主体
- 在 editor 中新增任何会修改 binding 的路径
- 因为“更安全”而额外加防御性旁路、兼容层、双写、双队列

### 必删项

本任务完成时，生产代码必须满足以下删除条件：

- `PluginEditor.cpp` 中不再出现 `clearPlaybackRegionMaterialization(` 调用
- `VST3AraSession` 中不再由 `queuedForMaterializationBirth` 承担 birth 调度真相
- `materializationBirthQueue_` 不再以 `ARAAudioSource*` 为队列元素驱动 birth
- `didAddPlaybackRegionToAudioModification()` 不再以 `projectionChanged || bindingChanged` 作为 birth enqueue 的必要前提

如果上述任一项仍保留在生产路径中，则视为任务未完成。

### 偏离即停工

若实现过程中发现下列任一情况，子代理不得自行发挥，必须停下：

- 需要新增本计划未声明的新 owner、新 worker、新状态机层
- 需要改动本任务范围外模块才能通过测试
- 需要保留旧路径并并行运行一段时间
- 发现现有测试命名或断言不足以表达本计划契约

停工后的唯一正确动作是：先更新计划，再继续实现。

## 第一性原则

### 1. owner 必须唯一

`AudioSource` 只负责样本来源。  
`PlaybackRegion` 只负责投影与 UI 焦点。  
真正对应“这份可编辑内容”的 owner 只能是 `AudioModification persistentId`。

所以 birth 的排队、去重、过期判断，都必须围绕 `AudioModification persistentId`，不能再围绕 source。

### 2. 一个 owner 只能有一个当前目标

对任意一个 `AudioModification persistentId`，session 在任一时刻只维护一份“当前应该绑定到哪个 `SourceWindow`”的记录。  
如果宿主回调又来了新 window，就覆盖旧目标；worker 永远只执行最新目标。

这就够了，不需要再拆出“binding truth / pending truth / focus hint truth”三四层并行结构。

### 3. editor 只显示，不裁决

payload 暂时不可见，只能说明：

- birth 还没做完，或
- restore 还没落到最终 shared store

它绝不意味着 binding 无效。  
所以 editor 不能调用 `clearPlaybackRegionMaterialization()` 去“帮 session 清状态”。

### 4. restore 只能落到最终 store

ARA VST3 的恢复入口必须保证：无论 `setStateInformation()` 与 `didBindToARA()` 谁先来，恢复结果最终都进入 `DocumentController` 的 shared stores。  
不能先恢复到临时 local store，再在 bind 时把它换掉。

## 当前代码中已确认的问题

### A. birth 仍然按 source 排队

当前 `VST3AraSession` 仍使用：

- `SourceSlot::queuedForMaterializationBirth`
- `materializationBirthQueue_` 仅保存 `ARAAudioSource*`

这会把“同一 source 下多个独立 `AudioModification` 的 birth 需求”压扁成一个 source 级 pending，结构上就是错的。

### B. `didAddPlaybackRegionToAudioModification()` 的 enqueue 条件过窄

当前只在 `projectionChanged || bindingChanged` 时 enqueue。  
如果 region 早已存在，但这次只是新 attach 了 `audioModificationPersistentId`，即使 `regionNeedsMaterializationBirthLocked()` 已经为 true，也可能完全不入队。

### C. worker 现在消费的是 source，不是 owner

`runMaterializationBirthsForSourceLocked()` 进入后才临时扫描 `regions_`。  
这意味着：

- 它没有稳定记住“哪个 modification 仍然待 birth”
- 晚到的新 modification 或新 window 只能赌下次 source 是否会再被入队

### D. editor 里存在破坏性 clear

`PluginEditor::syncAraPreferredRegionBindingOnly()` 在 payload/buffer 暂缺时，仍会调  
`clearPlaybackRegionMaterialization()`。

这会把“暂时不可见”升级成“真的删掉 binding”，属于错误 owner 越权。

### E. restore 顺序仍有最终 store 风险

当前 `setStateInformation()` 可能先把恢复数据写进 bind 前的 local stores，随后 `didBindToARA()` 再切到 shared stores。  
这样恢复结果就没有落在 ARA 真正消费的那套 store 上。

## 修复方案

## 1. 把 birth 模型改成 modification 单槽模型

删除 source 级 birth 队列语义，改成：

```cpp
struct PendingBirth
{
    juce::String audioModificationPersistentId;
    SourceWindow desiredWindow;
    uint64_t revision;
};
```

以及一套以 `audioModificationPersistentId` 为 key 的 session 内表：

- 当前已生效 binding
- 当前待执行 birth
- 当前正在执行的 revision

含义非常简单：

- 一个 modification 同时只允许有一个当前目标 `desiredWindow`
- 新目标到来时直接覆盖旧目标并提升 revision
- worker 只认最新 revision

这就是唯一的 pending 结构，不再保留 source 级并行概念。

### 固定状态语义

这里的“单槽模型”不是宽泛说法，而是固定语义：

- 对每个 `audioModificationPersistentId`，session 最多只有一份待执行 birth 记录
- 这份记录只描述“当前最新目标 window 是什么”
- 新 window 到来时覆盖旧 window，不允许并存两条待执行 birth
- worker 完成旧 revision 后，如果发现自己不是最新 revision，结果必须丢弃，不允许落库

子代理不得把它实现成：

- source -> 多 modification 的嵌套待执行集合
- region 指针队列
- editor 本地待执行标志
- “主队列 + 补偿队列 + 恢复队列”三套并行结构

## 2. 所有 enqueue 入口统一成一句规则

统一规则：

```text
只要某次回调结束后，某个 region 对应的 modification 满足
regionNeedsMaterializationBirthLocked()，
就 upsert 该 modification 的 PendingBirth。
```

不再允许：

- 依赖 `projectionChanged || bindingChanged` 这种外层门槛
- 依赖 “source 之后可能还会再回调一次”
- 依赖 editor reopen 或 timer tick 补救

这样 `didAddPlaybackRegionToAudioModification()`、`didUpdatePlaybackRegionProperties()`、sample access ready、restore/rebind 后的回调都走同一条逻辑。

### 固定 enqueue 契约

实现后必须能用一句话描述所有入队点：

```text
回调先把 region slot 归一化到当前 owner/window；
只要该 owner 仍满足 regionNeedsMaterializationBirthLocked()，
就 upsert 它的单槽 PendingBirth。
```

不允许任何额外条件把这条规则绕开。

## 3. worker 只消费 modification key

worker 改成：

1. 出队一个 `audioModificationPersistentId`
2. 读取该 key 当前最新 `PendingBirth`
3. 若 source sample access 还没 ready，则保留该 key 待后续重试
4. 调用 `birthAraMaterializationWithOriginalF0(...)`
5. 完成后核对 revision
6. 只有 revision 仍是最新时才提交 binding；否则丢弃结果，继续等最新目标

这样天然满足：

- 同一 source 下两个不同 modification 一定各自 birth
- 同一 modification 的多个 region 仍共享一个 materialization
- 旧 window 的慢结果不能覆盖新 window 的目标

### worker 固定提交规则

worker 成功返回后，提交 binding 前必须再次检查 revision。  
这不是建议，是唯一允许的提交门槛：

- revision 仍是最新 -> 允许提交
- revision 已过期 -> 结果直接丢弃

不允许：

- 旧结果先提交，再指望后续新结果覆盖
- 旧结果提交后由 editor 或 timer 清理
- 旧结果落库但打一个“stale”标记继续共存

## 4. Editor 去真值化

`PluginEditor` 只保留读路径：

- 读取 snapshot
- 解析当前可显示 region/materialization
- 显示 ready / pending / restoring / failed

明确删除：

- payload 缺失时调用 `clearPlaybackRegionMaterialization()`
- 任何 editor 自己“修正” session binding 的路径

窗口关闭再打开时，editor 只是重新 attach 到 session/store 的既有状态。  
恢复工作内容不再依赖 editor 的瞬态成员。

### Editor 固定边界

本任务中 editor 的职责只剩三项：

1. 读 snapshot
2. 解析当前显示目标
3. 画出 ready / pending / restoring / failed

除此之外，一律不准碰 binding 真相。  
如果子代理提交的代码让 editor 新增任何“修状态”“补绑”“清绑”“重试”行为，直接判定偏离方案。

## 5. 恢复入口只保留一个最终落点

`setStateInformation()` 若早于 `didBindToARA()` 到达，不直接恢复进当时的 store。  
而是先缓存原始 state block 或已解析结果，等 `didBindToARA()` 完成 shared-store attach 后，再一次性恢复到最终 shared stores。

这里也只保留一个原则：

- ARA 模式下，恢复只能以最终 shared stores 为落点

不引入额外旁路，不让 editor 或 session 再各自做二次恢复。

### restore 固定入口

这次恢复方案只允许一种入口语义：

- `setStateInformation()` 早到：缓存，不写临时 store
- `didBindToARA()` 完成：一次性恢复到最终 shared stores

不允许：

- 先写 pre-bind local stores，再在 bind 后“同步过去”
- local/shared 双写
- 由 editor reopen 或 timer tick 再补恢复
- 新增另一套专门给 ARA editor 的 restore cache

## 删除项

这轮方案会主动删掉下列“多余结构”：

- source 级 `queuedForMaterializationBirth` 作为 birth 真相
- `materializationBirthQueue_` 以 `ARAAudioSource*` 为单位的调度模型
- `didAdd...` 上仅靠 `projectionChanged || bindingChanged` 的 enqueue 门槛
- editor timer sync 内的 `clearPlaybackRegionMaterialization()` 调用
- 依赖 editor 瞬态成员维持恢复真相的思路

## 实施顺序

### Phase 0: 先补失败测试

先让这些用例失败：

- `AraBirth_DidAddOnPreexistingRegionWithNewPersistentIdEnqueuesBirth`
- `AraBirth_SameSourceTwoDifferentPersistentIdsBothBirth`
- `AraBirth_NewPersistentIdArrivesWhileWorkAlreadyInFlight_NotLost`
- `AraBirth_StaleResultForOldWindowDoesNotOverrideLatestDesiredWindow`
- `AraEditor_MissingPayloadDoesNotClearBinding`
- `AraEditor_DestroyRecreateReattachesExistingBinding`
- `AraStateRestore_PreBindSetStateStillRestoresIntoFinalSharedStores`

### Phase 0 Gate

未看到上述用例先红，禁止进入生产实现。  
若某个测试写不出来或无法稳定表达契约，先修测试设计，不准跳过。

### Phase 1: 改 session birth owner

- 改为 modification 单槽模型
- 把所有 enqueue 入口统一到 `regionNeedsMaterializationBirthLocked() => upsert`
- worker 改为消费 modification key + latest revision

### Phase 1 Gate

只有当下面四项同时成立，Phase 1 才算完成：

1. 同 source 多 modification 测试转绿
2. stale result 测试转绿
3. 生产路径不再依赖 source 级 birth 队列语义
4. 没有新增第二套补偿/恢复队列

### Phase 2: 删 editor destructive clear

- 删 `syncAraPreferredRegionBindingOnly()` 中的 clear 路径
- 改为 pending/restoring 只读 UI

### Phase 2 Gate

只有当下面两项同时成立，Phase 2 才算完成：

1. `PluginEditor.cpp` grep 不再命中 `clearPlaybackRegionMaterialization(`
2. `AraEditor_MissingPayloadDoesNotClearBinding` 与 `AraEditor_DestroyRecreateReattachesExistingBinding` 转绿

### Phase 3: 修复 pre-bind restore 入口

- `setStateInformation()` 提前到达时只缓存
- `didBindToARA()` 后一次性恢复到最终 shared stores

### Phase 3 Gate

只有当下面两项同时成立，Phase 3 才算完成：

1. `AraStateRestore_PreBindSetStateStillRestoresIntoFinalSharedStores` 转绿
2. 生产路径中不存在“先写 local store 再同步到 shared store”的并行恢复结构

### Phase 4: 宿主验证

- focused tests
- 三目标构建
- Reaper 7.72 + `tiehuafei_test (2).zip`
- 关闭/重开窗口
- 保存/重开 REAPER

2026-05-27 closure note: focused tests and build matrix passed. The user explicitly stated that REAPER manual testing is not required from Codex for this closure, so Phase 4 host execution is out of scope for this task and must not be reported as PASS.

## 非目标

- 不新增 editor-owned fallback birth
- 不新增 host 特判
- 不把 `preferredRegion_` 升级为生命周期 owner
- 不为这次问题再引入第二套恢复真相

## 代码评审拒收清单

出现以下任一条，评审必须直接拒收：

- patch 还在生产路径里保留 source 级 birth 真相
- patch 让 editor 新增任何改 binding 的行为
- patch 通过新增 fallback/retry/兼容层来“掩盖”测试
- patch 触碰无关模块来规避本问题
- patch 没有先补红测试就直接改实现
- patch 让旧结构和新结构并行存在

## 成功标准

1. 同一 source 下多个不同 `AudioModification persistentId` 都能各自稳定 birth，不再只剩最后一个 item。
2. 同一 `AudioModification` 的多个 region 仍共享一个 materialization，不引入重复 birth。
3. 关闭并重开 editor 后，无需重新计算也能重新显示已有内容。
4. payload 暂缺时 editor 不会再清 binding。
5. `setStateInformation()` 与 `didBindToARA()` 顺序变化不再影响最终恢复结果。
6. Reaper 7.72 + `tiehuafei_test (2).zip` 手工验证不由 Codex 本次执行；不得伪报为 PASS。

## 2026-05-27 Closure Evidence

- `VST3AraSession` birth owner now lives at `audioModificationPersistentId + SourceWindow + revision`; worker-ready queue entries are persistentIds, not source-level truth.
- stale worker results are rejected before materialization binding commit.
- `clearPlaybackRegionMaterialization()` was removed from production session API; `PluginEditor.cpp` no longer owns destructive binding cleanup.
- metadata-only ARA VST3 state is cached before `didBindToARA()` and replayed into final shared stores after bind; regular unbound VST3 state restores immediately.
- `OpenTuneTests.exe architecture`, `processor`, `core`, and `memory` pass.
- ARA VST3, non-ARA VST3, and Standalone Release targets build.
