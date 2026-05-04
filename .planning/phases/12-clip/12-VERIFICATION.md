---
phase: 12-clip
verified: 2026-04-14T23:05:00+08:00
status: passed
score: 3/3 must-haves verified
---

# Phase 12: Clip 真相源与映射收敛 Verification Report

**Phase Goal:** 让 ARA/VST3 播放链路统一命中 `clipId -> processor clip truth source`，并让 mapping-only 变化只更新映射语义，不进入 render queue。

## Observable Truths

| # | Truth | Status | Evidence |
| --- | --- | --- | --- |
| 1 | ARA runtime 读路径先经 `binding.clipId` 解析 processor 读源 | ✓ PASS | `Source/ARA/OpenTunePlaybackRenderer.cpp:193` calls `getPluginPlaybackReadSource(binding.clipId, readSource)` |
| 2 | ARA runtime 继续只复用共享 `readPlaybackAudio()` 读核 | ✓ PASS | `Source/ARA/OpenTunePlaybackRenderer.cpp:209` calls `readPlaybackAudio(request)` |
| 3 | mapping-only 语义进入 plugin clip write path，并只 bump `mappingRevision` | ✓ PASS | `Source/PluginProcessor.cpp:2317` defines `setPluginClipStartSeconds`; `Source/Plugin/PluginEditor.cpp:1037`, `:1064`, `:1278` use it |

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
| --- | --- | --- | --- |
| Phase 12 tests | `build\OpenTuneTests.exe` | `REV_01_SetPluginClipStartSecondsBumpsMappingRevisionWithoutRenderInvalidation` PASS | ✓ PASS |
| Clip-centric read-source tests | `build\OpenTuneTests.exe` | `ClipPlaybackReadSourceUsesCoreClipCache`, `TRUTH_01_ClipIdentityIsSingleTruthAcrossAraBindingAndPlaybackRead` PASS | ✓ PASS |
| 静态接线审计 | `rg -n "getPluginPlaybackReadSource\(|setPluginClipStartSeconds\(" Source` | 命中 | ✓ PASS |

## Notes

- 当前实现没有回补“adapter 私有 renderCache 旁路”的旧 Phase 8/12 结构；相反，它已经被更优的 `binding.clipId -> processor.getPluginPlaybackReadSource() -> readPlaybackAudio()` 结构替代。
- `AudioSourceState` 现在只保留宿主音频缓冲与元数据，不再承载第二套 render cache 真相源。

## Gate Status

- PASS
