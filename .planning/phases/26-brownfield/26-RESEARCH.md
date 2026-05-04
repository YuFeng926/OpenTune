# Phase 26: 旧结构删除与 brownfield 守护 - Research

**研究日期：** 2026-04-16  
**阶段范围：** `Phase 26 - 旧结构删除与 brownfield 守护`，对应 `CLEAN-01`、`CLEAN-02`、`CLEAN-03`。  
**研究来源：** live tree 源码、`.planning` 项目文档、Phase 23/24/25 summary 与 verification 产物。  
**研究触发方式：** 用户在无 `CONTEXT.md` 的前提下手动选择“先 research 再 planning”。

## 结论摘要

- Phase 26 不需要新增外部依赖，也不需要改 shared processor 核心设计；它是一次 **现有 snapshot / callback / preferred-region 架构完成后的清尾与 brownfield 锁边界**。
- 当前 live tree 里真正还属于 Phase 26 的生产残留，主要只剩两类：
  1. **孤儿结构**：`Source/ARA/AudioSourceState.h` 仍在仓库和 `CMakeLists.txt` 中登记，但当前生产代码已经不再 include 或消费它。
  2. **source-level projection helper / getter**：`OpenTuneDocumentController` 仍公开并实现 `getAudioSourceContentRevision(...)`、`getAudioSourceMappingRevision(...)`、`getAudioSourceClipBinding(...)`、`getPlaybackRegionClipBinding(...)`、`findAudioSourceByClipId(...)`，以及与之配套的 `PublishedSnapshot::findFirstRegionForAudioSource(...)` / `findProjectedRegionForAudioSource(...)`。这些 helper 已不被 live production consumers 使用，只是 Phase 23/24/25 过渡期留下的 source-level 投影外壳。
- 当前 VST3 editor 与 renderer 已经完成 snapshot-only consumer 收敛：
  - `OpenTunePlaybackRenderer.cpp` 在 `processBlock()` 中只做一次 `loadSnapshot()`，并按 `findRenderableRegionView(*snapshot, region)` 解析 region truth。
  - `PluginEditor.cpp` 的 `recordRequested()` 与 `syncImportedAraClipIfNeeded()` 都只消费 `preferredRegionView`，不再使用 source-level getter。
- 因此，Phase 26 的正确做法不是再改 consumer 逻辑，而是：
  1. 先把 **verification source + executable/static guards** 固定下来；
  2. 再删掉当前已经没有 production caller 的旧 helper / 孤儿文件；
  3. 最后用 fresh `build-phase26-docs` 证据关闭 phase。

## Project Constraints

- 必须继续遵守 `AGENTS.md`：中文输出、先看 `.planning/codebase/` 记忆再用 live tree 复核、禁止“止血/最小改动/兼容层/并行旧新结构/兜底”。
- Standalone 与 VST3 UI 必须继续隔离；Phase 26 不能把 ARA/VST3 的 cleanup 扩散到 `Source/Standalone/*` 行为层。
- `Source/PluginProcessor.*` 是双格式共享核心；Phase 26 只允许**验证它的边界不被扩张**，不允许为了 cleanup 再塞新 ARA 状态抽象进 core。

## Live Tree Evidence

### 1. 还存在的旧结构残留

- `Source/ARA/AudioSourceState.h` 仍存在，文件内容只剩一个孤立 `struct AudioSourceState`，持有 `name`、`audioBuffer`、`sampleRate`、`numChannels`、`numSamples`，但当前 live tree 中没有生产代码 include 它。`CMakeLists.txt:455-462` 仍把它放在 `target_sources(OpenTune ...)` 里。
- `OpenTuneDocumentController.h:195-211` 仍声明以下 source-level helper：
  - `getAudioSourceContentRevision(...)`
  - `getAudioSourceMappingRevision(...)`
  - `getAudioSourceClipBinding(...)`
  - `getPlaybackRegionClipBinding(...)`
  - `findAudioSourceByClipId(...)`
- `OpenTuneDocumentController.cpp:590-721` 仍实现这些 helper；它们全部建立在 `loadSnapshot()` 后再做 source-level 投影之上，而不是 region-level consumer 直读。
- `OpenTuneDocumentController.cpp:17-29` 仍保留 `findProjectedRegionForAudioSource(...)`；`OpenTuneDocumentController.h:141-149` 仍保留 `PublishedSnapshot::findFirstRegionForAudioSource(...)`。这两个函数的唯一价值就是支撑上述 source-level getter。

### 2. 当前正确结构已经落地，因此可以安全清理

- `OpenTunePlaybackRenderer.cpp:149-167` 已把每 block 单次 `loadSnapshot()` 和 `findRenderableRegionView(*snapshot, region)` 固化为当前 renderer contract。
- `PluginEditor.cpp:855-919` 的 `recordRequested()` 已只从 `preferredRegionView` 读取 `copiedAudio`、`sampleRate`、`sourceStartSeconds`、`sourceEndSeconds`、`playbackStartSeconds`，然后调用 `registerPlaybackRegionClipBinding(...)` 回写应用真相。
- `PluginEditor.cpp:1004-1149` 的 `syncImportedAraClipIfNeeded()` 已只消费：
  - `snapshot->epoch`
  - `preferredRegionView->regionIdentity`
  - `binding.appliedRegion`
  - `lastConsumedAraSnapshotEpoch_`
  - `lastConsumedPreferredAraRegion_`
- 生产代码搜索显示：当前 `Source/ARA` / `Source/Plugin` / `Tests` 中，source-level getter 的唯一生产定义在 controller 本身；没有 consumer 继续调用这些 helper。

### 3. Phase 26 需要同步修的 brownfield 守护点

- `Tests/TestMain.cpp` 当前已经有 Phase 23/24/25 的 source-audit helpers 和 main-runner 注册模式；Phase 26 最自然的方式是继续在同一文件新增 `runPhase26...Tests()`，并加入 `main()` 调用链。
- `Tests/TestMain.cpp` 当前有一些 section extraction 仍把 `getAudioSourceClipBinding(...)` 当作分段 marker；Phase 26 删除 getter 后，测试里的提取边界也必须一起迁移到新的稳定 marker，避免“生产代码删掉旧结构，测试反而因旧 marker 崩掉”。
- `PluginEditor.cpp:1245-1250` 还有一处与 `OriginalF0` 提示相关的普通英文文案 `Please retry in a moment.`。这不是 ARA source-level lifecycle retry，也不属于本 phase 要删除的错误结构；Phase 26 的 grep-zero 范围应精确限定在 ARA controller / source-level helper / consumer path，而不能把整个仓库里所有自然语言 `retry` 都当成阻塞项。

## What Phase 26 Should Deliver

### CLEAN-01

- 删除孤儿文件 `Source/ARA/AudioSourceState.h`，并从 `CMakeLists.txt` 的 ARA `target_sources` 列表中移除。
- 删除 `OpenTuneDocumentController` 中不再被生产消费者使用的 source-level helper surface：
  - `getAudioSourceContentRevision(...)`
  - `getAudioSourceMappingRevision(...)`
  - `getAudioSourceClipBinding(...)`
  - `getPlaybackRegionClipBinding(...)`
  - `findAudioSourceByClipId(...)`
  - `PublishedSnapshot::findFirstRegionForAudioSource(...)`
  - `findProjectedRegionForAudioSource(...)`
- 删除后不允许再回流任何 source-level “preferred if same source, else first region” 这类 projection fallback。

### CLEAN-02

- 在 `Tests/TestMain.cpp` 中补 Phase 26 的可执行与静态守护，至少覆盖：
  - 旧孤儿文件已经消失；
  - 旧 helper declaration / definition 全部 grep-zero；
  - `OpenTunePlaybackRenderer.cpp` 与 `PluginEditor.cpp` 继续保持 snapshot / preferred-region consumer 结构，不因 cleanup 回退；
  - 测试提取边界不再依赖已删除 helper 名。
- 创建 `.planning/phases/26-brownfield/26-TEST-VERIFICATION.md` 作为唯一 verification source，沿用前序 phase 的 L1/L2/L4/L6 结构与 `build-phase26-docs` 路径。

### CLEAN-03

- 用静态 audit 明确锁住 shared `OpenTuneAudioProcessor` 的现有边界：
  - `prepareImportClip`
  - `commitPreparedImportClip`
  - `readPlaybackAudio`
  - `getPluginPlaybackReadSource`
  - `setClipStartSecondsById`
  - `setPluginClipStartSeconds`
  - `replaceClipAudioById`
  - `enqueuePartialRenderById`
  - `enqueuePluginClipPartialRender`
- 证明 Phase 26 没有把 `RegionIdentity` / `PublishedSnapshot` / `AudioSourceClipBinding` / `ARAPlaybackRegion*` 这类 adapter-side 语义重新塞进 `Source/PluginProcessor.h`。
- 同时证明 `Source/Standalone/PluginEditor.cpp` 仍然 grep-zero 于 snapshot/binding cleanup 语义，防止 VST3/ARA cleanup 泄漏到 Standalone UI 壳层。

## Recommended Plan Split

### Plan 26-01 — 先冻结 verification source 与 Phase 26 guards

- 创建 `26-TEST-VERIFICATION.md`。
- 在 `Tests/TestMain.cpp` 中新增 `runPhase26CleanupBrownfieldTests()` 与 main-runner 接线。
- 先把 journey 名、grep-zero 列表、processor boundary audit、Standalone grep-zero guard 写死。

### Plan 26-02 — 删除旧生产结构并修正测试边界

- 删除 `Source/ARA/AudioSourceState.h` 与 `CMakeLists.txt` 对它的登记。
- 删除 controller 中所有 source-level getter / projection helper。
- 同步调整 `Tests/TestMain.cpp` 中受旧符号影响的 section marker / static-audit 代码。
- 用 `build-phase26-docs` 跑到 Phase 26 guards 转绿。

### Plan 26-03 — 用 fresh evidence 关闭 verification source

- 运行 L1/L2/L6。
- 记录 L4 static audit：
  - orphan file / helper 全部 grep-zero；
  - renderer/editor 继续 snapshot-only / preferred-region-only；
  - `PluginProcessor.h` 边界未扩张；
  - `Source/Standalone/PluginEditor.cpp` 不出现 ARA cleanup 泄漏符号。
- 把 GREEN evidence 写回 `26-TEST-VERIFICATION.md`。

## Verification Strategy

- **L1**: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase26-docs -Target OpenTuneTests`
- **L2 / L6**: `& ".\build-phase26-docs\OpenTuneTests.exe"`
- **L4 static audit** 聚焦四类 grep-zero：
  1. `Source/ARA/AudioSourceState.h` 不存在；
  2. controller 头/实现不再包含 source-level helper 名；
  3. `OpenTunePlaybackRenderer.cpp` / `PluginEditor.cpp` 不回流 legacy consumer 路径；
  4. `PluginProcessor.h` / `Source/Standalone/PluginEditor.cpp` 不吸入 adapter-side cleanup 语义。

## Planning Implications

- Phase 26 不需要 phase split beyond 3 plans；复杂度主要在 brownfield audit，而不是大规模架构再设计。
- 因为没有 `CONTEXT.md`，planner 应严格以 `PROJECT.md` / `REQUIREMENTS.md` / `ROADMAP.md` / `STATE.md` / live tree 为准，不补充新的产品层决策。
- 这是 milestone v2.2 的收尾 phase，计划中必须显式把 **“不扩 core、不动 Standalone 行为、只删旧结构并补 guard”** 写成可验证 acceptance criteria，而不是停留在说明文字。
