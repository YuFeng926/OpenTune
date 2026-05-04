# Phase 11 Research — 最小状态模型定稿

**Date:** 2026-04-12  
**Phase:** 11-最小状态模型定稿  
**Requirements:** TRUTH-01, TRUTH-03, SAFE-03

## Goal of this research

回答：**为了把 Phase 11 规划好，我必须先知道什么？**

本 research 只基于当前主工作区真实源码与已完成的 Phase 09 / 10 结果，不把旧记忆文档直接当现状。

## Current state map

### 1. `clipId`

- **已存在，且已是当前 VST3/ARA 路径中的主要身份键。**
- 共享核心里，`TrackState::AudioClip` 直接持有 `clipId`：`Source/PluginProcessor.h:220-245`
- `PluginProcessor` 已提供大量 `ById` API：`getClipPlaybackReadSourceById`、`findClipIndexById`、`getClipRenderCacheById`、`replaceClipAudioById`、`enqueuePartialRenderById`：`Source/PluginProcessor.h:501-573`
- ARA 绑定真相源里，`AudioSourceClipBinding` 以 `clipId` 连接 source 与 clip：`Source/ARA/OpenTuneDocumentController.h:80-138`
- ARA playback renderer 已通过 `binding.clipId -> getClipPlaybackReadSourceById(...) -> readPlaybackAudio(...)` 读取：`Source/ARA/OpenTunePlaybackRenderer.cpp:192-213`
- VST3 editor 的同步链路也以 `binding.clipId` 找 clip：`Source/Plugin/PluginEditor.cpp:1052-1177`

### 2. `mappingRevision`

- **已存在，但只存在于 ARA/source 同步层，还没有进入共享 clip 状态模型。**
- `AudioSourceSyncState` 持有 `mappingRevision`：`Source/ARA/OpenTuneDocumentController.h:141-148`
- `didUpdatePlaybackRegionProperties()` / `didAddPlaybackRegionToAudioModification()` 在映射变化时 bump：`Source/ARA/OpenTuneDocumentController.cpp:33-64, 72-105`
- UI 同步路径通过 `binding.appliedMappingRevision` 判断 mapping-only / mappingChanged：`Source/Plugin/PluginEditor.cpp:1057-1061`
- **缺口：** processor 的 `TrackState::AudioClip` 不知道 `mappingRevision`；当前 mapping 版本还是 source-side 概念，不是 clip-side 最小状态模型的一部分。

### 3. `renderRevision`

- **概念缺失；当前只有 `RenderCache` chunk 级 `desiredRevision / publishedRevision`。**
- `RenderCache::Chunk` 的 revision 字段：`Source/Inference/RenderCache.h:29-45`
- `requestRenderPending()` 递增 chunk `desiredRevision`：`Source/Inference/RenderCache.cpp:304-326`
- `completeChunkRender()` 依据 chunk revision 判断 stale/requeue：`Source/Inference/RenderCache.cpp:360-407`
- `DiagnosticInfo` 和 UI snapshot 读到的是 `RenderCache::StateSnapshot.maxDesiredRevision / maxPublishedRevision`：`Source/PluginProcessor.h:347-355`, `Source/PluginProcessor.cpp:1240-1246`
- **缺口：** 当前没有“每个 `clipId` 独立、由 render-affecting 唯一驱动”的显式 `renderRevision` 抽象。

### 4. `layoutEpoch`

- **当前完全缺失。**
- 主工作区没有 `layoutEpoch` 字段、API、日志或测试。
- ARA source/playback range 变化目前只体现为 `contentRevision / mappingRevision` 与同步后的 clip replace / mapping update：`Source/ARA/OpenTuneDocumentController.cpp:324-420`, `Source/Plugin/PluginEditor.cpp:1057-1177`
- **规划含义：** Phase 11 只能先冻结字段归属、命名与读取边界；真正把它接进旧队列 stale publish 屏障应留给 Phase 13。

### 5. `RenderCache`

- **已存在且稳定，是当前旧队列核心。**
- 负责 chunk 状态机、pending 索引、desired/published revision、resampled cache：`Source/Inference/RenderCache.h`, `Source/Inference/RenderCache.cpp`
- 处理器 worker 仍按“遍历 track / clip -> 向每个 clip 的 renderCache 拉 pending job”运转：`Source/PluginProcessor.cpp:2895-3225`
- `replaceClipAudioById()` 已明确保留 renderCache 实例：`Source/PluginProcessor.cpp:1920-1942`
- **结论：** Phase 11 不该重写它；只应明确它在新模型中的职责边界。

### 6. `RenderTask`

- **显式公共抽象不存在；当前只有 worker 内部临时结构 `WorkerRenderJob`。**
- 定义位置：`Source/PluginProcessor.cpp:2901-2908`
- 该结构目前携带 `trackId + clipId + start/end + targetRevision + cache`，仍明显带有旧容器语义。
- **缺口：** Phase 11 需要冻结 future `RenderTask` 至少以 `clipId + renderRevision + layoutEpoch + chunk/range` 为主语，但不应在本 phase 里重写旧 worker 调度本体。

## Remaining semantic leaks

### `track` 仍在 VST3/ARA 路径里高频出现

- `PluginEditor.cpp` 顶部把 VST3 语义固定到 `constexpr int kVst3TrackId = 0;`：`Source/Plugin/PluginEditor.cpp:21`
- 几乎所有 VST3 editor 与 processor 的交互仍以 `kVst3TrackId` 作为第一个参数：如 `getDiagnosticInfo`、`setClipStartSecondsById`、`replaceClipAudioById`、`enqueuePartialRenderById`：`Source/Plugin/PluginEditor.cpp:450, 954, 1094, 1138, 1159, 1311`
- ARA playback renderer 也固定 `kAraPluginTrackId = 0`：`Source/ARA/OpenTunePlaybackRenderer.cpp:13, 199`
- `PluginProcessor::getCurrentClipIdForPlugin()` 仍通过 `tracks_[0].selectedClipIndex` 取得当前 clip：`Source/PluginProcessor.cpp:1606-1616`

### `audioSource` 仍是 ARA 同步入口主语

- `AudioSourceClipBinding`、`audioSourceSyncStates_`、`getCurrentPlaybackAudioSource()` 都以 `juce::ARAAudioSource*` 为键：`Source/ARA/OpenTuneDocumentController.h:37-55, 150-159`
- `syncImportedAraClipIfNeeded()` 先取 `currentPlaybackAudioSource`，再读 binding：`Source/Plugin/PluginEditor.cpp:1047-1058`

### `playbackRegion` 仍主导 mapping 更新

- `didUpdatePlaybackRegionProperties()` 和 `didAddPlaybackRegionToAudioModification()` 通过 `ARAPlaybackRegion` 改写 source/playback range，并 bump mapping revision：`Source/ARA/OpenTuneDocumentController.cpp:33-64, 72-105`

## Planning implications

1. **Phase 11 不应直接实现 Phase 13 的 queue 收敛。**
   - 当前 `RenderCache` chunk revision 已在工作；真正缺的是 clip 级状态模型与命名边界。

2. **Phase 11 应优先把“clip 语义”从“track 0 + source-side revisions”里抽出来。**
   - 现有代码已经大量使用 `clipId`，但 clip-side state 还不完整。

3. **`mappingRevision` 需要下沉到 clip 语义层，`renderRevision / layoutEpoch` 需要显式建模，但本 phase 先做状态与接口冻结，不把它们提前接入旧队列。**

4. **`RenderTask` 在本 phase 更适合先定义契约与携带字段，而不是推倒 `chunkRenderWorkerLoop()`。**

## Recommended plan split

### Plan 11-01 — 定义最小状态模型与核心状态查询面

- 核心目标：把 `clipId / renderRevision / mappingRevision / layoutEpoch` 明确放进共享 clip 语义层，补足 processor 的只读状态模型与相应文档。
- 主要文件：
  - `Source/PluginProcessor.h`
  - `Source/PluginProcessor.cpp`
  - `Source/ARA/OpenTuneDocumentController.h`
  - `Source/ARA/OpenTuneDocumentController.cpp`
  - `.planning/phases/11-最小状态模型定稿/11-STATE-MODEL.md`

### Plan 11-02 — 让 VST3/ARA 入口按 clip 语义消费状态模型

- 核心目标：减少 `track 0` / `audioSource` / `playbackRegion` 在 VST3/ARA 路径里的语义主角地位，让它们退回“容器/输入来源”，真正消费 clip-centric 状态与 API。
- 主要文件：
  - `Source/Plugin/PluginEditor.h`
  - `Source/Plugin/PluginEditor.cpp`
  - `Source/ARA/OpenTunePlaybackRenderer.cpp`
  - `Source/PluginProcessor.h`
  - `Source/PluginProcessor.cpp`

### Plan 11-03 — 自动化验证与 phase 证据闭环

- 核心目标：为 TRUTH-01 / TRUTH-03 / SAFE-03 建立测试与 phase verification evidence。
- 主要文件：
  - `Tests/TestMain.cpp`
  - `.planning/phases/11-最小状态模型定稿/11-TEST-VERIFICATION.md`
  - `.planning/phases/11-最小状态模型定稿/11-VERIFICATION.md`

## Risks that must shape the plans

1. **源码与“下一阶段目标”之间存在语义空档。**
   - `renderRevision` / `layoutEpoch` 在代码里还不存在；计划必须把 Phase 11 限定为“状态模型定稿 + 边界收敛”，不能偷跑到完整 queue 改造。

2. **当前 ARA 状态容器明显有线程/生命周期风险。**
   - `audioSourceStates_` / `audioSourceSyncStates_` / `audioSourceClipBindings_` 是普通 `std::map`，`Timer::callAfterDelay` lambda 直接捕获 `this` 和 `audioSource`：`Source/ARA/OpenTuneDocumentController.cpp:174-189`
   - 即使本 phase 不彻底解决，也必须在研究与计划里标明，避免新状态模型继续扩大风险面。

3. **`kVst3TrackId = 0` 和 `kAraPluginTrackId = 0` 是 SAFE-03 的主要残留。**
   - 如果不在计划里显式处理，track 仍会被下游实现继续当成 VST3 语义主角。

4. **Render queue 仍是 track/clip 双层遍历。**
   - `chunkRenderWorkerLoop()` 里 worker job 仍带 `trackId`：`Source/PluginProcessor.cpp:2901-2947`
   - Phase 11 只能先冻结 future `RenderTask` 契约与 clip-centric 边界，Phase 13 再收敛旧队列实现。

5. **当前测试已经覆盖 mapping-only 不触发 render invalidation。**
   - `Tests/TestMain.cpp:1434-1447, 2527-2540`
   - Phase 11 测试应复用该模式，升级为 state-model invariant 级别，而不是重写一套无关测试。

---

**Conclusion:** 当前代码已经具备 `clipId + RenderCache + ARA binding revision` 的基础，但最小状态模型还没有真正冻结成一层统一的 clip 语义。Phase 11 的正确任务不是重写队列，而是把 clip-side state contract、VST3/ARA 消费边界、以及验证闭环一次性定清楚。
