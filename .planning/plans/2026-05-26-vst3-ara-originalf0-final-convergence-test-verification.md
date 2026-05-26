# VST3 ARA OriginalF0 终审收敛测试与验证

**日期:** 2026-05-26  
**状态:** Proposed  
**关联执行方案:** `.planning/plans/2026-05-26-vst3-ara-originalf0-final-convergence.md`  
**继承方案:** `.planning/plans/2026-05-25-vst3-ara-originalf0-minimal-pipeline.md`

## 验证目标

本验证文档是下一轮执行的准绳。当前 2026-05-25 方案已经完成了主要 reader-based birth 方向，但终审发现仍有 ARA editor 内容兜底、source-level hydration pass、公开 `copiedAudio` 残留和源码形状测试偏多的问题。因此本轮验证只接受以下合同：

```text
ARA sample access callback
-> session queues source-window materialization birth
-> processor.birthAraMaterializationWithOriginalF0(req)
-> chunk read requested source window
-> prepareImport birth playable materialization
-> ch0 source-rate input -> RMVPE internal resampling
-> commit OriginalF0/energy
-> OriginalF0State::Ready
-> F0InferenceService::releaseImmediately()
```

## 阻塞断言

- **ARA-FINAL-01:** ARA-bound `PluginEditor::recordRequested()` 不得调用 `requestReferenceNoteGeneration()`。
- **ARA-FINAL-02:** ARA-bound `PluginEditor::recordRequested()` 不得调用 `requestMaterializationRefresh()`。
- **ARA-FINAL-03:** `VST3AraSession` 不得保留 source-level hydration read pass；worker 只负责 materialization birth 调度。
- **ARA-FINAL-04:** `PublishedRegionView` 不得公开 `copiedAudio` 字段，snapshot 不再固化 always-null compatibility API。
- **ARA-FINAL-05:** `prepareImportFromAraRegion()` 这类旧 `sourceBuffer -> sliced -> prepareImport` helper 必须删除。
- **ARA-FINAL-06:** `requestMaterializationRefresh()` 的注释和测试必须明确为 Standalone / regular VST3 F0 refresh，不再提 ARA `skipNoteGeneration`。
- **ARA-FINAL-07:** `requestReferenceNoteGeneration()` 只允许 Standalone / regular VST3 显式入口调用，VST3 ARA 自动链和 ARA Read Audio 都不得触达。
- **ARA-FINAL-08:** 行为验证必须覆盖“无 GAME、无 refresh fallback、RMVPE Ready 后释放、播放仍可读 born materialization”，不能只验证符号存在。

## L1 静态验证

命令：

```powershell
git diff --check
```

预期：

- Exit code 0。
- 无空白错误。
- 无 patch 格式错误。

## L2 架构守护测试

构建测试：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

必须新增或更新以下测试：

- `AraFinal_RecordRequestedDoesNotTriggerGame`
- `AraFinal_RecordRequestedDoesNotFallbackToMaterializationRefresh`
- `AraFinal_SessionHasNoSourceLevelHydrationReadPass`
- `AraFinal_PublishedRegionViewDoesNotExposeCopiedAudio`
- `AraFinal_PluginEditorHasNoPrepareImportFromAraRegionHelper`
- `AraFinal_RequestMaterializationRefreshContractIsNonAra`
- `AraFinal_AraBirthPathOwnsOriginalF0Release`

预期：

- 上述测试全部 PASS。
- 现有 architecture 仅允许保留已经明确登记的预存 `LineageStateMachine` 失败；如果出现新的 ARA-F0 失败，必须视为本轮阻塞。

## L3 行为级/半行为级验证

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
```

最低行为证据：

- `requestReferenceNoteGeneration()` 仍能在 Standalone / regular VST3 允许路径被显式调用。
- ARA auto birth 的 processor contract 仍只通过 `AraOriginalF0BirthRequest` 进入。
- `F0InferenceService::releaseImmediately()` 在 ARA birth 成功和失败后都有明确调用点。
- `requestMaterializationRefresh()` 仍能服务非 ARA materialization F0 refresh，但不再被 ARA editor 调用。

如果 ARA SDK 的 `HostAudioReader` 难以在单元测试中完整 fake，本层允许采用 `OPENTUNE_TEST_BUILD` 下的窄 test probe，但 probe 只能观测，不得成为生产 fallback 或兼容层。

## L4 构建矩阵

命令：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

预期：

- ARA VST3 PASS。
- non-ARA VST3 PASS。
- Standalone PASS。
- 不引入 Ninja 或手写脚本。

## L5 宿主旅程验证

本层在用户确认前不能标记 PASS。

### Studio One ARA 长音频自动 OriginalF0

步骤：

1. 安装本轮重建后的 ARA VST3。
2. 在 Studio One ARA 工作流中加载一个超过 5 分钟的音频 event。
3. 触发 ARA 自动 birth / Read Audio。
4. 观察日志中只有 `ARA OriginalF0 pipeline`，没有 `[NoteGen]` / `GAME` enqueue。
5. 确认 `OriginalF0State::Ready` 后出现 RMVPE immediate release 日志。
6. 确认 F0 曲线可见后播放干声仍正常。
7. 观察内存从 RMVPE 峰值回落到只保留播放缓存的稳态。

用户确认口径：

```text
用户旅程 "Studio One ARA 长音频 OriginalF0 终审收敛" 已完成验证。
请确认是否符合预期：符合 / 不符合
```

## L6 回归边界

建议命令：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe
```

预期：

- focused suites 必须 PASS。
- full binary 如仍有预存失败，必须逐项列出并证明与本轮 ARA-F0 收敛无关。
- 不得把 `ui` runner exit=1/no `[FAIL]` 这类历史问题包装成 full PASS。

## 完成证据

本轮完成时必须留下：

- 删除项清单：旧 API、旧 helper、旧 field、旧测试 guard。
- 新增/更新测试清单。
- 三目标构建结果。
- focused suites 结果。
- 宿主 L5 是否完成；未完成必须明确标为 pending。
