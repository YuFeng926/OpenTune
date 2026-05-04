---
phase: 15-样本真相源与冻结边界
plan: 02
subsystem: core
tags: [worker, callback, render-cache, boundary-propagation]

requires:
  - phase: 15-01
    provides: ClipSampleRange, FrozenRenderBoundaries, publish trim helper
provides:
  - Worker-side frozen boundary propagation into mel/F0 timing
  - Strict synth-length mismatch rejection in callback
  - Shared publishedAudio source for base/resampled cache writes
affects: [15-03, 16, render-worker]

tech-stack:
  added: []
  patterns:
    - "Worker timing is projected from frozen sample boundaries"
    - "Base/resampled cache derive from the same trimmed publish audio"

key-files:
  created: []
  modified:
    - Source/PluginProcessor.cpp

key-decisions:
  - "D15-02-01: F0 frame windows and mel timing now consume frozen boundary projections instead of raw job seconds"
  - "D15-02-02: synth mismatch is terminal and no cache publish happens before helper validation succeeds"

patterns-established:
  - "Pattern: callback rejects mismatched synth output before touching RenderCache"
  - "Pattern: resampled cache is derived from the same publishedAudio vector as base cache"

requirements-completed: [SAMP-02, TASK-01, TASK-02, TASK-03]

duration: 12min
completed: 2026-04-13
---

# Phase 15 Plan 02: worker/callback 冻结边界消费 Summary

**`chunkRenderWorkerLoop()` 和 vocoder callback 现在共同消费 frozen sample boundaries，旧的 seconds-based `expectedSamples` / `resize()` 修形路径已经从发布链彻底移除。**

## Performance

- **Duration:** 12 min
- **Started:** 2026-04-13T17:00:00+08:00
- **Completed:** 2026-04-13T17:12:00+08:00
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- worker 的 F0 frame window、mel 时间轴和日志都改为消费 frozen boundary 投影
- callback 在 synth-length mismatch 时直接走 `TerminalFailure`，不再做静默 `resize()`
- base cache 与 resampled cache 都从同一个 `publishedAudio` 结果派生

## Task Commits

1. **Task 1: 在 worker 中冻结并传递 sample-domain render boundaries** - `6503483` (feat)
2. **Task 2: 用严格 synth-length 校验替换 callback 的 resize 修形** - `3dd18e0` (fix)

**Plan metadata:** pending final docs commit

## Files Created/Modified

- `Source/PluginProcessor.cpp` - 把 worker/callback/cache 发布链统一到 frozen sample boundary contract

## Decisions Made

- worker 继续保留现有 `RenderCache` seconds key 体系，但长度、frame 和 publish 判定只认 frozen sample fields
- resampled cache 直接从裁后的 `publishedAudio` 派生，避免再从未裁剪 synth 输出旁路读取

## Deviations from Plan

- 为保持 `PluginProcessor.cpp` 在 helper 引入后始终处于一致状态，部分 boundary propagation scaffolding 已在 `b0ea9b7` 一并落地；本 plan 的两个 commit 继续把 worker 时间投影和 callback 发布来源收紧到最终规则。

## Issues Encountered

- None

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- `15-03` 可以直接用新的 helper 和 callback failure path 写回归测试与 traceability 文档
- Phase 16 已经可以在 frozen sample boundary 之上继续做 hop 对齐切分与 last-chunk 语义收敛

## Self-Check: PASSED

- Summary file exists and task commit hashes `6503483` / `3dd18e0` are present in git history
