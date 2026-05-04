# VST3 ARA Transport / Projection / F0 Diagnostics 修复计划

## Goal

从结构上修复 VST3 ARA 停止态点击播放头不动、ARA renderer 与 UI projection 映射并行、OriginalF0 右移缺少判据三个问题。

## 原始设计目的

- ARA 播放音频只信 host playhead snapshot 与 published region snapshot。
- UI 展示需要能表达“用户已发出 seek 命令但 host 尚未回传确认”的 presentation state。
- Projection 是 derived value object，PianoRoll 与 ARA renderer 必须共享同一 `MaterializationTimelineProjection` 映射公式。
- OriginalF0 是 materialization-local 派生曲线，不能靠 UI offset 补偿；需要证明 first audible 与 first voiced 的真实关系。

## 核心需求

1. 删除 `PendingTransportIntent` 作为半成品外泄状态，改成 processor 内部 transport coordinator。
2. 暴露 `TransportPresentationState`，VST3 editor timer 只读 presentation，不再直接读 `getPosition()`。
3. ARA renderer 删除自有 `mapPlaybackTimeToMaterializationLocalSample()` 公式，改用 `MaterializationTimelineProjection`。
4. F0 commit 后记录 `F0Alignment`：materializationId、audioDuration、firstAudibleTime、firstVoicedFrame、firstVoicedTime、f0FrameCount、expectedInferenceFrameCount、sourceWindow。

## Tasks

### Task 1 — TransportCoordinator

Files:
- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- `Source/Plugin/PluginEditor.cpp`

Steps:
1. 删除 public `PendingTransportIntentKind` / `PendingTransportIntent` / `getPendingTransportIntent()`。
2. 新增 public `TransportCommandKind`、`TransportCommand`、`TransportPresentationState`。
3. private 保留单一 `pendingTransportCommand_`，仅由 `requestAraTransportPlay/Stop/Seek()` 写入，仅由 `updateHostTransportSnapshot()` 确认/覆盖。
4. 新增 `getTransportPresentationState()`：基于 host snapshot 生成 UI display state；停止态 pending seek 显示 requested time，播放态仍以 host snapshot 为音频事实。
5. ARA `processBlock()` 写 `positionAtomic_` 时使用 presentation time，`isPlaying_` 使用 presentation playing。
6. VST3 `timerCallback()` 改读 `getTransportPresentationState()`，同步 transportBar/PianoRoll。

### Task 2 — Renderer Projection

Files:
- `Source/ARA/OpenTunePlaybackRenderer.cpp`

Steps:
1. 删除 renderer-local mapping helper。
2. 从 `PublishedRegionView` 构造 `MaterializationTimelineProjection{ playbackStart, playbackDuration, materializationDuration }`。
3. 用 `projectTimelineTimeToMaterialization()` + `clampMaterializationTime()` 得到 materialization-local read start。
4. 日志继续打印 playbackTime、mappedLocalTime、sample offset。

### Task 3 — F0Alignment diagnostics

Files:
- `Source/Services/F0ExtractionService.h`
- `Source/Services/ImportedClipF0Extraction.h`
- `Source/PluginProcessor.cpp`

Steps:
1. 给 `F0ExtractionService::Result` 增加诊断字段：audioDurationSeconds、firstAudibleTimeSeconds、firstVoicedFrame、firstVoicedTimeSeconds、expectedInferenceFrameCount。
2. `extractOriginalF0ForImportedClip()` 在 full materialization-local audio 上计算 first audible sample 与 first voiced frame。
3. commit 成功后打印单行 `F0Alignment`，包含 sourceWindow 与 frame count。

## Verification

- Build `OpenTuneTests` with VS/CMake Debug.
- Run full `OpenTuneTests.exe`.
- Build `OpenTune_VST3` with VS/CMake Debug.
- Manual DAW retest reads latest `OpenTune*.log` for `F0Alignment` and transport seek presentation behavior.

## Verification Result

- `OpenTuneTests` build: PASS.
- Full `OpenTuneTests.exe`: PASS.
- `OpenTune_VST3` build: PASS.
- `OpenTune_Standalone` build: PASS.
- Manual DAW retest after first pass: stopped-state seek still did not move because PianoRoll stopped overlay did not actively consume/repaint presentation position, and stop+seek used one pending command slot.
- Follow-up repair: pending playback state and pending position now coexist; VST3 editor calls `PianoRollComponent::setPresentedPlayheadPosition()`; stopped overlay repaint suppression was removed.
- Latest verification after follow-up repair: `OpenTuneTests` build PASS; full `OpenTuneTests.exe` PASS; `OpenTune_VST3` build PASS; `OpenTune_Standalone` build PASS.
