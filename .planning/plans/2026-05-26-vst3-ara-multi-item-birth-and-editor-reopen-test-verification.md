# VST3 ARA Multi-Item Birth 与 Editor 重开恢复测试验证

**Date:** 2026-05-26  
**Status:** Implemented - L1-L4/L6 automated gates passed on 2026-05-27; REAPER L5 explicitly not required for this task closure  
**Related plan:** `.planning/plans/2026-05-26-vst3-ara-multi-item-birth-and-editor-reopen-structural-fix.md`

## 范围

验证这次简化方案的四个硬约束：

1. birth owner 改成 `AudioModification persistentId`
2. 同一 modification 只保留一个当前 `SourceWindow` 目标
3. editor 只读，不再破坏 binding
4. restore 无论调用顺序如何都只落到最终 shared stores

## 验收规则

本文件不是参考建议，而是实现验收门槛。

### 必须满足

- 先有失败测试，再允许改生产代码
- 每个 Phase 结束都要跑本文件指定 gate
- 任一 blocking gate 失败，本任务不得宣称完成

### 明确禁止

- 只跑 build，不跑新增测试
- 只看日志判断“应该没问题”
- 用手工推理替代 `rg` / suite / host 验证
- 跳过失败测试但不经用户批准
- 用其它通过的老测试替代本次新增测试

### 证据要求

每个关键断言都必须有对应证据：

- 静态删除项 -> `rg`
- 状态机契约 -> `architecture` suite
- restore 落点 -> `processor` suite
- 宿主行为 -> Reaper L5

缺任一证据，视为未验证。

## 验证要求

- **ARA-LIFE-01:** pending birth 不再由 source 拥有；session 必须按 `audioModificationPersistentId` 维护待执行 birth。
- **ARA-LIFE-02:** 任意回调只要导致 `regionNeedsMaterializationBirthLocked()` 为 true，就必须 upsert 该 modification 的 birth 记录。
- **ARA-LIFE-03:** 同一 source 下不同 `AudioModification persistentId` 必须各自 birth。
- **ARA-LIFE-04:** 同一 `AudioModification` 的多个 region 必须继续共享一个 materialization。
- **ARA-LIFE-05:** `PluginEditor.cpp` 不得再因 payload/buffer 暂缺而调用 `clearPlaybackRegionMaterialization()`。
- **ARA-LIFE-06:** editor destroy/recreate 后必须重新附着到已有 binding，而不是要求重新计算。
- **ARA-LIFE-07:** `setStateInformation()` 早于 `didBindToARA()` 时，恢复结果仍必须进入最终 shared stores。

## L1 - 静态验证

命令：

```powershell
git diff --check
rg -n "queuedForMaterializationBirth|materializationBirthQueue_" Source/ARA
rg -n "clearPlaybackRegionMaterialization\\(" Source/Plugin/PluginEditor.cpp
rg -n "PendingBirth|audioModificationPersistentId|revision" Source/ARA Tests
```

预期：

- `git diff --check` exit code 0
- source 级 birth 队列语义已被移除或不再承担主调度职责
- `PluginEditor.cpp` 不再命中 `clearPlaybackRegionMaterialization(`
- 代码里能看到 modification-owner + latest-revision 的新语义

### L1 Fail Conditions

命中以下任一条则 L1 失败：

- `PluginEditor.cpp` 仍命中 `clearPlaybackRegionMaterialization(`
- `Source/ARA` 中仍只有 source 级 birth owner 而没有 modification-owner 语义
- 生产代码同时保留新旧两套 birth 调度路径

## L2 - 架构与状态机测试

构建：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

至少新增并通过：

- `AraBirth_DidAddOnPreexistingRegionWithNewPersistentIdEnqueuesBirth`
- `AraBirth_SameSourceTwoDifferentPersistentIdsBothBirth`
- `AraBirth_NewPersistentIdArrivesWhileWorkAlreadyInFlight_NotLost`
- `AraBirth_StaleResultForOldWindowDoesNotOverrideLatestDesiredWindow`
- `AraEditor_MissingPayloadDoesNotClearBinding`
- `AraEditor_DestroyRecreateReattachesExistingBinding`
- `AraStateRestore_PreBindSetStateStillRestoresIntoFinalSharedStores`

并保留通过：

- `AraBinding_MultiplePlaybackRegionsSameAudioModificationShareMaterialization`
- `AraBinding_RestoredPersistentIdRebindsNewPlaybackRegion`
- `AraEditor_AttachesRenderableBindingWithoutReadAudioArm`

### L2 Fail Conditions

命中以下任一条则 L2 失败：

- 新增测试没有先经历 RED
- 任何一个新增用例未通过
- 旧有共享 materialization 契约测试转红
- 通过方式依赖修改测试弱化断言而不是修正实现

## L3 - processor / restore focused suites

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe core
```

预期：

- processor suite 证明 restore 最终落点正确
- memory/core suites 不引入新的生命周期回归

### L3 Fail Conditions

命中以下任一条则 L3 失败：

- `processor` suite 不能证明 pre-bind restore 正确落到 shared stores
- 为了通过 `processor` suite 引入 local/shared 双写恢复结构
- `memory` 或 `core` 出现新回归

## L4 - 构建矩阵

命令：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

预期：

- ARA VST3 PASS
- non-ARA VST3 PASS
- Standalone PASS

### L4 Fail Conditions

命中以下任一条则 L4 失败：

- 任一目标构建失败
- 为解决 ARA 问题而破坏 non-ARA 或 Standalone 构建

## L5 - Reaper 7.72 手工宿主验证

2026-05-27 scope note: the user explicitly stated that REAPER manual testing is not required from Codex for this task. This section remains a host/manual checklist only; it is not a blocker for the current automated closure and must not be reported as PASS unless someone actually executes it.

环境：

- Windows 11
- REAPER 7.72
- 当前工作区重建后的 ARA VST3
- 用户工程：`tiehuafei_test (2).zip`

### 场景 1：多 item 读取

1. 打开 `tiehuafei_test (2).zip` 对应工程
2. 确认至少两个 item 来自同一 source
3. 对 item A / item B 分别触发 `Read Audio` 或等待 auto-birth
4. 观察 waveform / OriginalF0 / PianoRoll

预期：

- 每个独立 `AudioModification` 都有自己的 F0 / materialization
- 不再只显示最后一个 item 的音高

### 场景 2：关闭并重开插件窗口

1. 在已有 F0 的工程上关闭 OpenTune 插件窗口
2. 重新打开同一实例窗口
3. 切换焦点到不同 item

预期：

- 不需要重新计算音高
- 不因窗口重建而丢 binding / 丢显示
- 若 payload 尚在恢复窗口中，只允许看到 pending/restoring 提示，不允许清 binding

### 场景 3：保存工程并重开 REAPER

1. 保存工程
2. 关闭并重开 REAPER / 重新加载工程
3. 再次检查 item A / item B

预期：

- 每个 item 恢复到正确 materialization
- 不出现“只剩最后一个 item”
- 不出现“第一次还能看见，保存后彻底丢了”

### L5 Fail Conditions

命中以下任一条则 L5 失败：

- 多 item 中仍有任一 item 没有独立 F0/materialization
- 关闭再打开窗口后需要重新计算
- reopen 或 restore 后 binding 被清掉
- 保存并重开 REAPER 后再次退化成 last-item-only

## L5 必留证据

- `AppLogger` 日志中：
  - `audioModificationPersistentId`
  - `desiredWindow`
  - `materializationId`
  - birth upsert / birth apply / stale drop
  - editor reopen sync 结果
- 若失败，必须记录发生在哪一层：
  - modification birth 未 upsert
  - 旧 revision 覆盖新目标
  - editor clear binding
  - restore 未进入最终 shared stores

### 日志最小要求

若实现未补齐足够日志以判断上面四类失败位置，则视为验证准备不足，不允许宣称 L5 完成。

## L6 - 回归口径

命令：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

预期：

- focused suites PASS
- 三目标构建 PASS
- `ui` suite 若仍有历史 exit=1 / 无 `[FAIL]` 异常，继续按既有口径单独记录，不包装成 full-suite PASS

## 完成定义

本次 Codex 自动化闭环定义如下：

1. L1-L4 全绿
2. L5 未执行且不得伪报；用户明确不要求本次 REAPER 手工测试
3. 没有通过 fallback、兼容层、双路径把问题藏起来
4. 主方案文档里的“必删项”和“代码评审拒收清单”全部满足

## 2026-05-27 Automated Evidence

- Build: `cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"` PASS.
- Static kill list: `rg -n "queuedForMaterializationBirth|materializationBirthQueue_" Source/ARA` returned no matches.
- Static kill list: `rg -n "clearPlaybackRegionMaterialization\(" Source/Plugin/PluginEditor.cpp` returned no matches.
- Suites: `OpenTuneTests.exe architecture`, `processor`, `core`, and `memory` PASS.
- Builds: `OpenTune_VST3` in `build-ara-overlay-vs18-clean`, `OpenTune_VST3` in `build-nonara-overlay-vs18-clean`, and `OpenTune_Standalone` in `build-ara-overlay-vs18-clean` PASS.
