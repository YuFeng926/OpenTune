# VST3 ARA OriginalF0 终审收敛执行方案

**日期:** 2026-05-26  
**状态:** Proposed  
**验证文档:** `.planning/plans/2026-05-26-vst3-ara-originalf0-final-convergence-test-verification.md`  
**继承方案:** `.planning/plans/2026-05-25-vst3-ara-originalf0-minimal-pipeline.md`

## 目标

把 VST3 ARA 自动 `OriginalF0` 从“新链路旁路旧结构”继续收敛为“唯一最小正确链路”。最终 ARA-bound 路径只允许 `VST3AraSession` 持有 ARA sample access 生命周期并发起 source-window birth，`OpenTuneAudioProcessor::birthAraMaterializationWithOriginalF0()` 负责一次性完成 playable materialization 和 OriginalF0，Editor 只做 binding/display，不做内容处理，不触发 GAME，不触发通用 refresh 兜底。

## 当前终审差距

| 差距 | 当前位置 | 风险 |
|------|----------|------|
| ARA `recordRequested()` 仍触发 GAME | `Source/Plugin/PluginEditor.cpp` | 违反“VST3 ARA 不实装 GAME ref 链” |
| ARA `recordRequested()` 仍可 fallback 到 `requestMaterializationRefresh()` | `Source/Plugin/PluginEditor.cpp` | Editor 仍是内容处理入口之一 |
| source-level hydration pass 仍整源 chunk read | `Source/ARA/VST3AraSession.cpp` | 虽不常驻 PCM，但仍不是 window birth 最小链路 |
| `PublishedRegionView::copiedAudio` always-null 字段残留 | `Source/ARA/VST3AraSession.h` | 公开 API 固化旧 copiedAudio 结构 |
| `prepareImportFromAraRegion()` helper 已无调用但仍存在 | `Source/Plugin/PluginEditor.cpp` | 明确死代码 |
| `requestMaterializationRefresh()` 注释仍提 `skipNoteGeneration` / ARA | `Source/PluginProcessor.cpp` / `.h` | 文档契约和实际结构不一致 |
| ARA-F0 测试仍偏源码形状 | `Tests/TestMain.cpp` | 不能充分证明运行时合同 |

## 目标架构

```text
ARA callbacks
  -> VST3AraSession stores source metadata + shared reader lease
  -> enqueue materialization birth for affected source/window
  -> ara worker builds AraOriginalF0BirthRequest
  -> OpenTuneAudioProcessor::birthAraMaterializationWithOriginalF0(req)
       -> validate reader lease/window
       -> chunk read only requested sourceWindow
       -> build playableAccum
       -> extract ch0 source-rate vector
       -> prepareImport(playableAccum) -> commit materialization
       -> f0Service.extractF0(ch0, sourceSampleRate)
       -> commit PitchCurve originalF0/energy
       -> set OriginalF0State::Ready
       -> releaseImmediately()
  -> VST3AraSession binds playback region to born materialization
  -> PluginEditor consumes snapshot and syncs PianoRoll display only
```

## 保留边界

- `requestMaterializationRefresh()` 保留，但只服务 Standalone / regular VST3 / 非 ARA materialization 的 F0 refresh。
- `requestReferenceNoteGeneration()` 保留，但只允许 Standalone / regular VST3 显式入口调用。
- Standalone 侧 GAME ref 链不在本轮删除范围。
- regular VST3 capture 的 F0/GAME 策略不在本轮重构范围。
- ARA playback 仍从 born materialization audio buffer / render cache 读取干声或渲染结果。

## 禁止边界

- ARA-bound Editor 不得调用 `requestReferenceNoteGeneration()`。
- ARA-bound Editor 不得调用 `requestMaterializationRefresh()`。
- ARA session 不得再保留 source-level 整源 read validation pass。
- ARA published snapshot 不得再暴露 `copiedAudio`。
- 不得用 always-null 字段、1-sample 占位、skip flag 或 defensive fallback 表示“已清理”。

## 执行任务

### Task 1: 先补终审 guard 测试

**文件:**

- 修改: `Tests/TestMain.cpp`

**步骤:**

1. 新增 `AraFinal_RecordRequestedDoesNotTriggerGame`，断言 ARA editor content branch 不包含 `requestReferenceNoteGeneration(`。
2. 新增 `AraFinal_RecordRequestedDoesNotFallbackToMaterializationRefresh`，断言 ARA editor branch 不包含 `requestMaterializationRefresh(`。
3. 新增 `AraFinal_SessionHasNoSourceLevelHydrationReadPass`，断言 `hydrationQueue_`、`sourceNeedsHydrationLocked`、`source hydration readAudioSamples` 等 source-level pass 被移除。
4. 新增 `AraFinal_PublishedRegionViewDoesNotExposeCopiedAudio`，断言 `PublishedRegionView` 不再包含 `copiedAudio`。
5. 新增 `AraFinal_PluginEditorHasNoPrepareImportFromAraRegionHelper`，断言 helper 删除。
6. 新增 `AraFinal_RequestMaterializationRefreshContractIsNonAra`，断言 refresh 注释不再出现 `skipNoteGeneration` 或 ARA auto path 描述。
7. 运行 architecture，确认这些测试先失败或至少准确暴露当前缺口。

**验证:**

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

### Task 2: 收敛 ARA-bound `recordRequested()`

**文件:**

- 修改: `Source/Plugin/PluginEditor.cpp`

**目标行为:**

ARA-bound `recordRequested()` 只做三件事：

1. 找到 preferred ARA region。
2. 如果 materialization 未 birth，提示 hydration/birth 正在处理，返回。
3. 如果 materialization 已存在，调用 `syncMaterializationProjectionToPianoRoll()` 并根据 F0 状态给用户提示。

**删除行为:**

- 删除 `requestReferenceNoteGeneration(materializationId)` 调用。
- 删除 `requestMaterializationRefresh(refreshRequest)` 调用。
- 删除 `rmvpeOverlayLatched_` 在 ARA editor fallback refresh 下的设置。

**F0 状态处理建议:**

```cpp
if (existingF0State == OriginalF0State::Extracting) {
    // show/info: still processing
    return;
}
if (existingF0State == OriginalF0State::Failed) {
    // show/info: re-import/re-open source to regenerate via session birth
    return;
}
// Ready or NotRequested: display sync only; session worker owns content.
```

不要在 Editor 里试图修复 failed/not-started F0；源头应由 session birth 状态机重新调度，Editor 不是内容 owner。

### Task 3: 删除 dead helper

**文件:**

- 修改: `Source/Plugin/PluginEditor.cpp`

**删除:**

- `prepareImportFromAraRegion(OpenTuneAudioProcessor&, const juce::AudioBuffer<float>&, ...)`

**验证:**

```powershell
rg -n "prepareImportFromAraRegion\\(" Source Tests
```

预期只允许 `.planning` 历史文档命中；`Source` / `Tests` 不得命中。

### Task 4: 移除 source-level hydration pass

**文件:**

- 修改: `Source/ARA/VST3AraSession.h`
- 修改: `Source/ARA/VST3AraSession.cpp`

**结构调整:**

1. 删除 `hydrationQueue_`。
2. 删除 `sourceNeedsHydrationLocked()`。
3. 删除 `enqueueSourceHydrationLocked()` 或改名为只调度 birth 的 `enqueueMaterializationBirthLocked()`。
4. 删除 `hydrationWorkerLoop()` 中读取 `hydrationQueue_` 的整段 read validation 分支。
5. worker wait predicate 只等待 `materializationBirthQueue_` 或 shutdown。
6. source sample access enable/content update 只负责更新 source metadata、reader lease、content revision，并 enqueue materialization birth。
7. 如果命名继续叫 `hydrationWorkerLoop()` 会误导，建议改名为 `araMaterializationWorkerLoop()`；若改名成本过高，至少改注释为 birth worker，不再称 source hydration。

**关键约束:**

- 不得读取 `numSamples` 的整源 scratch 作为 readiness gate。
- 不得为了验证 host reader 可用而额外读完整 source。
- 读取失败只发生在 `birthAraMaterializationWithOriginalF0()` 的目标 source window chunk read 中，并 fail fast。

### Task 5: 删除 published snapshot `copiedAudio`

**文件:**

- 修改: `Source/ARA/VST3AraSession.h`
- 修改: `Source/ARA/VST3AraSession.cpp`
- 修改: `Tests/TestMain.cpp`

**删除:**

- `PublishedRegionView::copiedAudio`
- `view.copiedAudio = nullptr`
- 测试中“copiedAudio always nullptr”的旧过渡断言

**替代断言:**

- `PublishedRegionView` 暴露 `sourceId`、`materializationId`、`sourceWindow`、`bindingState`。
- Renderer 判断 renderable 只依赖 binding/materialization state，不依赖 copied PCM。

### Task 6: 清理 refresh / GAME 契约注释

**文件:**

- 修改: `Source/PluginProcessor.h`
- 修改: `Source/PluginProcessor.cpp`

**更新内容:**

- `MaterializationRefreshRequest` 注释：明确是 Standalone / regular VST3 F0 refresh。
- `requestMaterializationRefresh()` 注释：不再提 ARA、`skipNoteGeneration`、birthAraWithF0。
- `noteGenerator_` / `noteGeneratorPool_` 注释：明确 GAME 在 processor 内保留是因为 Standalone / regular VST3 仍需要显式 note-ref 生成，不属于 ARA auto chain。
- `requestReferenceNoteGeneration()` 注释：扩展为“VST3 ARA auto path 和 ARA Read Audio 都不得调用”。

### Task 7: 加强行为证据

**文件:**

- 修改: `Tests/TestMain.cpp`
- 可选修改: `Tests/TestSupport.h`

**最低要求:**

- 保留必要 source guard，但不要只检查“符号存在”。
- 对禁止路径使用 negative guard：函数体内不能出现禁止调用。
- 对保留路径使用正向 guard：Standalone / regular VST3 仍能显式调用 GAME。
- 对 RMVPE release 使用路径 guard：`birthAraMaterializationWithOriginalF0()` 成功 commit `Ready` 后必须同函数内调用 `releaseImmediately()`。

**可选 test probe:**

如果需要证明调用顺序，可在 `OPENTUNE_TEST_BUILD` 下提供只读 probe 或计数器，但不得引入生产 fallback。

### Task 8: 全量 kill-list 审查

**命令:**

```powershell
rg -n "copiedAudio|hydrationQueue_|sourceNeedsHydrationLocked|source hydration|prepareImportFromAraRegion|skipNoteGeneration|requestReferenceNoteGeneration\\(|requestMaterializationRefresh\\(" Source Tests
```

**预期:**

- `requestReferenceNoteGeneration(` 只在 processor 声明/定义和 Standalone/regular VST3 入口出现。
- `requestMaterializationRefresh(` 不在 ARA-bound editor 分支出现。
- `copiedAudio` 不在 `Source` / active tests 中出现。
- `hydrationQueue_`、`sourceNeedsHydrationLocked`、source hydration read pass 不在 `Source` 中出现。
- `skipNoteGeneration` 不在 `Source` / active tests 中出现。

### Task 9: 构建与测试矩阵

按验证文档执行 L1-L6。至少完成：

```powershell
git diff --check
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

## 完成标准

- ARA-bound Editor 无 GAME、无 refresh 内容兜底。
- ARA session 无 source-level hydration full-read pass。
- ARA published snapshot 无 `copiedAudio` API 残留。
- 旧 helper / 旧 skip 字段 / 旧注释 / 旧测试 guard 全部删除或更新。
- ARA auto OriginalF0 唯一内容入口是 `birthAraMaterializationWithOriginalF0(AraOriginalF0BirthRequest)`。
- RMVPE 在 `OriginalF0State::Ready` 后立即释放。
- Standalone / regular VST3 的 F0 refresh 与显式 GAME ref 行为不被破坏。

## 风险与处理

- 如果删除 source-level hydration pass 后宿主 sample access 时序暴露真实失败，不允许恢复整源 read validation；应把失败定位到 birth request 的 reader lease / source window / callback 时序。
- 如果 ARA Read Audio 遇到 F0 failed/not-started，不允许 Editor 兜底 refresh；应由 session 状态机重新 birth 或给出明确用户提示。
- 如果测试无法 fake ARA `HostAudioReader`，可以保留少量 negative architecture guard，但 L5 宿主日志必须补齐行为证据。

## 非目标

- 不重构 Standalone GAME ref 链。
- 不重构 regular VST3 capture 的 F0/GAME 策略。
- 不改变 materialization playback buffer 作为可播放干声 owner 的事实。
- 不解决历史 `ui` suite exit=1/no `[FAIL]` 问题，除非它被本轮改动触发。
