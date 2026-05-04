---
phase: 18-时间轴投影与 Standalone 守护
plan: 01
subsystem: ui
tags: [timeline, sample-truth, processor, projection, standalone, vst3]

requires:
  - phase: 17-01
    provides: sample-authoritative cache publish and time projection baseline
  - phase: 17-02
    provides: regression discipline and verification closure for sample-boundary work
provides:
  - Shared clip timeline projection contract for UI consumers
  - Shared clip playhead projection clamped to stored sample spans
  - Plugin and Standalone accessors that reuse the same projection logic
affects: [18-02, 18-03, 18-04, piano-roll, arrangement, timeline]

tech-stack:
  added: []
  patterns:
    - "UI timeline consumers read clip duration/end/playhead projections from Processor instead of rebuilding seconds truth locally"

key-files:
  created: []
  modified:
    - Source/PluginProcessor.h
    - Source/PluginProcessor.cpp

key-decisions:
  - "D18-01-01: Clip timeline projection is a shared-core contract, not a UI-local helper"
  - "D18-01-02: Clip-local playhead must be clamped in sample space before projecting back to time"

patterns-established:
  - "Pattern: derive clip duration and timeline end from stored sample count, never from caller-supplied seconds"
  - "Pattern: plugin helpers must forward to the same shared projection implementation as Standalone"

requirements-completed: [TIME-01, TIME-02, TIME-03]

duration: 8min
completed: 2026-04-13
---

# Phase 18 Plan 01: shared timeline projection contract Summary

**Processor 现在直接提供 clip timeline projection 和 clip playhead projection，UI 时间轴终于有了唯一的 sample-authoritative 读取面。**

## Performance

- **Duration:** 8 min
- **Started:** 2026-04-13T20:10:00+08:00
- **Completed:** 2026-04-13T20:18:00+08:00
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- 在 shared core 中新增 `ClipTimelineProjection` 与 `ClipPlayheadProjection`
- 把 clip duration / end time / clip-local playhead clamp 统一沉到 `OpenTuneAudioProcessor`
- 为 plugin 与 Standalone 同时暴露同一套 projection helper，避免两边复制算法

## Task Commits

1. **Task 1: 在 Processor 中定义 clip timeline projection 结构** - `389fef1` (feat)
2. **Task 2: 实现 sample-authoritative clip/playhead projection helper** - `40e65b5` (feat)

## Files Created/Modified

- `Source/PluginProcessor.h` - 定义 Phase 18 的 clip/playhead projection contract
- `Source/PluginProcessor.cpp` - 实现 shared timeline projection 与 plugin forwarding helper

## Decisions Made

- clip duration 与 timeline end 必须继续从 stored sample count 投影，而不是从 UI 侧 seconds 继续当真相源
- clip-local playhead 必须先 clamp 到 stored sample span，再投影回 absolute seconds

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

- None

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- PianoRoll 与 ArrangementView 已可以直接消费 shared projection contract
- Phase 18 后续 UI 改造不需要再自己定义第二套 clip duration / playhead semantics

## Self-Check: PASSED

- Summary file exists at `.planning/phases/18-时间轴投影与 Standalone 守护/18-01-SUMMARY.md`
- Commits `389fef1` and `40e65b5` are present in git history
