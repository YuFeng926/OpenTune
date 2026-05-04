---
phase: 08-playback-read
plan: 01
subsystem: playback-read
tags: [playback, unified-api, fallback-strategy, tdd]
requires: []
provides:
  - Unified PlaybackReadRequest/PlaybackReadResult/PlaybackReadLayer types
  - readClipAudioForPlayback unified read API
  - Four-level fallback: resampled -> rendered -> dry -> blank
affects:
  - Source/PluginProcessor.h
  - Source/PluginProcessor.cpp
  - Tests/TestMain.cpp
tech-stack:
  added:
    - PlaybackReadLayer enum
    - PlaybackReadRequest struct
    - PlaybackReadResult struct
    - readClipAudioForPlayback method
  patterns:
    - Unified read API pattern
    - Four-level fallback strategy
    - TDD workflow
key-files:
  created: []
  modified:
    - Source/PluginProcessor.h
    - Source/PluginProcessor.cpp
    - Tests/TestMain.cpp
decisions:
  - D-01: Types placed in public section after TrackState definition
  - D-02: Four-level fallback implemented: resampled -> rendered -> dry -> blank
  - D-03: processBlock now uses unified API instead of inline read logic
metrics:
  duration: ~30 minutes
  tasks: 3
  files: 3
  commits: 3
completed: 2026-04-07
---

# Phase 08 Plan 01: 统一 Playback Read 核心实现

## One-liner

在 Processor 共享层建立统一 Playback Read 核心，实现 resampled -> rendered -> dry -> blank 四级回退策略，并将常规播放路径完整切换到该 API。

## Summary

### Task 1: 定义统一 read API 契约与结果语义

**TDD 实现：** 添加了 `PlaybackReadLayer`、`PlaybackReadRequest`、`PlaybackReadResult` 类型和 `readClipAudioForPlayback` 方法声明。

**类型设计：**
- `PlaybackReadLayer` enum: None, Resampled, Rendered, Dry, Blank
- `PlaybackReadRequest` struct: clip 句柄、读取时间、采样率、样本数
- `PlaybackReadResult` struct: 实际样本数、命中层级、静音回退标志、音频数据

**Commit:** `feat(08-01): define unified playback read API contract and result semantics`

### Task 2: 实现固定四级回退策略

**TDD 实现：** 添加了行为测试验证四级回退逻辑。

**回退顺序：**
1. Resampled cache（最高优先级）
2. Rendered cache
3. Dry signal
4. Blank/silence（最低优先级）

**关键行为：**
- resampled 命中时不再继续尝试 rendered/dry
- rendered 命中失败且 dry 可读时走 dry，不进入 blank
- 源无效/越界/不可读时输出 blank/silence

**Commit:** `test(08-01): add four-level fallback behavior tests`

### Task 3: 常规播放路径切换到统一 read API

**实现：** 重构 `processBlock` 中的 clip 读取逻辑，移除旧的内联读取分支，改用 `readClipAudioForPlayback`。

**变更：**
- 移除了 ~40 行内联读取代码
- 添加了 ~15 行统一 API 调用代码
- 保持混音语义（fade in/out、gain）不变

**Commit:** `feat(08-01): switch regular playback to unified read API`

## Deviations from Plan

None - plan executed exactly as written.

## Key Decisions

1. **类型位置：** 将类型定义放在 `TrackState` 之后的 `public:` 部分，以便测试代码访问 `TrackState::AudioClip*` 指针。

2. **回退实现：** 在 `readClipAudioForPlayback` 内部实现完整四级回退，而不是在调用方处理。

3. **混音语义：** 保持现有混音逻辑（fade in/out、gain 计算）在 processBlock 中，只替换读取逻辑。

## Verification

- [x] 所有测试通过（58 tests）
- [x] 统一 read API 可被测试代码访问
- [x] processBlock 使用单一读取入口
- [x] 四级回退策略行为正确

## Known Stubs

None.

## Next Steps

- Plan 08-02: 将 ARA 播放路径切换到统一 read API
