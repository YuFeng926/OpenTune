---
phase: 16-hop-对齐切分与末块语义
plan: 02
subsystem: render-worker
tags: [frozen-boundaries, last-chunk-padding, f0-gap-fill, worker]

requires:
  - phase: 16-01
    provides: Sample-domain silent gaps, hop-aligned chunk splits, sample-aware pending jobs
provides:
  - Sample-input frozen boundary helper that rejects non-final misaligned chunks
  - Worker mel/vocoder input zero-padding only for the final chunk
  - Trailing-F0 gating that keeps padded tail frames unvoiced
affects: [16-03, 17, render-worker]

tech-stack:
  added: []
  patterns:
    - "Only the final chunk may expand from publish samples to synth samples"
    - "Padded inference tail stays local and never re-enters voiced F0 semantics"

key-files:
  created: []
  modified:
    - Source/PluginProcessor.h
    - Source/PluginProcessor.cpp

key-decisions:
  - "D16-02-01: freezeRenderBoundaries now consumes exact sample spans and rejects non-final misaligned chunks"
  - "D16-02-02: fillF0GapsForVocoder keeps internal/leading repair but disables trailing extension on padded last chunks"

patterns-established:
  - "Pattern: zero padding exists only in worker-local mel input sized to synthSampleCount"
  - "Pattern: trailing F0 repair is policy-driven instead of unconditionally extending every tail"

requirements-completed: [CHNK-03, CHNK-04, TASK-04]

duration: 22min
completed: 2026-04-13
---

# Phase 16 Plan 02: only-last-chunk padding 与 pad-tail F0 语义 Summary

**worker 现在把真实末端与推理末端彻底分开了：`freezeRenderBoundaries(...)` 只认 sample 输入，中间 chunk 不再允许隐式 padding，最后一个 chunk 的 zero padding 只存在于 mel/vocoder 输入里，且 padded tail 不再被 trailing-F0 extension 改写。**

## Performance

- **Duration:** 22 min
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- 把 `freezeRenderBoundaries(...)` 改成 sample-input helper，并让非末块不整 hop 时直接失败
- worker 读取真实音频后，统一把 `monoAudio` 扩到 `synthSampleCount`，只在最后一个 chunk 的尾部显式补零
- `fillF0GapsForVocoder()` 新增 trailing-extension policy，最后一个 padded chunk 的尾部 frame 保持 zero-F0

## Task Commits

1. **Task 1: 用 sample-span 冻结边界并把 zero padding 限制在最后一个 chunk 的输入准备阶段** - `1541d9c` (feat)
2. **Task 2: 禁止 padded tail 触发 trailing-F0 extension** - `4152cd8` (fix)
3. **Verification fix: 未初始化 hop size 回退到 512** - `8043ffa` (fix)

## Files Created/Modified

- `Source/PluginProcessor.h` - 更新 frozen boundary helper 签名，并暴露 namespace-scope 的 F0 gap-fill helper 声明
- `Source/PluginProcessor.cpp` - worker 只在最后一个 chunk 进行 zero padding，且对 padded tail 关闭 trailing F0 extension

## Decisions Made

- `publishSampleCount` 与 `trueEndSample` 继续代表真实边界；`synthSampleCount` 只是最后一个 chunk 的推理视图长度
- `fillF0GapsForVocoder()` 不再无条件延伸尾部 zero-F0，而是由 worker 根据 padding 语义显式决定是否允许 trailing extension

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] 未初始化 vocoder hop size 会把 chunk segmentation 直接打空**
- **Found during:** Phase 16 regression verification
- **Issue:** `VocoderDomain::getVocoderHopSize()` 在未初始化时返回 0，`enqueuePartialRender()` 和 worker 若直接信任这个值，会让 hop-aligned chunk builder / frozen boundary helper 进入非法 hop 路径。
- **Fix:** 仅当当前 hop size `> 0` 时才覆盖默认 512；`enqueuePartialRender()` 与 worker 都采用同样的 guard。
- **Files modified:** `Source/PluginProcessor.cpp`
- **Commit:** `8043ffa`

## Known Stubs

- None.

## Self-Check: PASSED

- Summary file exists and task commit hashes `1541d9c` / `4152cd8` are present in git history
