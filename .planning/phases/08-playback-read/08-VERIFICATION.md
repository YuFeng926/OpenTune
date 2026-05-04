---
phase: 08-playback-read
verified: 2026-04-14T22:45:00+08:00
status: passed
score: 4/4 must-haves verified
---

# Phase 8: 统一 Playback Read 核心 Verification Report

**Phase Goal:** 让常规播放与 ARA 播放都复用共享 `readPlaybackAudio()` 读核，并固定遵守 `resampled -> rendered -> dry -> blank` 策略。

## Observable Truths

| # | Truth | Status | Evidence |
| --- | --- | --- | --- |
| 1 | 共享 processor 暴露统一读核 | ✓ PASS | `Source/PluginProcessor.h:455` and `Source/PluginProcessor.cpp:3923` define `readPlaybackAudio()` |
| 2 | 常规播放走共享读核 | ✓ PASS | `Source/PluginProcessor.cpp:1050` calls `readPlaybackAudio(readRequest)` |
| 3 | ARA 播放走共享读核，且先经 `binding.clipId` 解析共享读源 | ✓ PASS | `Source/ARA/OpenTunePlaybackRenderer.cpp:193` calls `getPluginPlaybackReadSource(binding.clipId, readSource)` and `:209` calls `readPlaybackAudio(request)` |
| 4 | 四级回退策略自动化保持通过 | ✓ PASS | `Playback Read API Tests` all PASS |

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
| --- | --- | --- | --- |
| Playback read regression suite | `build\OpenTuneTests.exe` | 全部 `Playback Read API Tests` PASS | ✓ PASS |
| 静态接线审计 | `rg -n "getPluginPlaybackReadSource\(|readPlaybackAudio\(" Source/ARA/OpenTunePlaybackRenderer.cpp Source/PluginProcessor.cpp` | 命中 | ✓ PASS |

## Notes

- 旧文档里把 `AudioSourceState::renderCache` 当作 ARA 读路径事实源的结论已经过时。
- 当前更优结构是：`OpenTunePlaybackRenderer` 只做宿主时间映射与 block 遍历，真正的读取源由 `binding.clipId -> processor.getPluginPlaybackReadSource()` 解析，然后统一交给 `readPlaybackAudio()`；`AudioSourceState` 只保留宿主音频缓冲与元数据，不再承载第二套 render cache 真相源。

## Gate Status

- PASS
