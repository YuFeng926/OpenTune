---
phase: 18-时间轴投影与 Standalone 守护
plan: 02
subsystem: ui
tags: [piano-roll, playhead, auto-scroll, projection, sample-truth]

requires:
  - phase: 18-01
    provides: shared clip timeline and clip playhead projection helpers
provides:
  - PianoRoll playhead projection consumption
  - PianoRoll auto-scroll clamped to clip sample span
  - PianoRoll fit-to-screen duration derived from clip projection
affects: [18-04, piano-roll, autoplay, focus]

tech-stack:
  added: []
  patterns:
    - "PianoRoll reads projected absolute playhead time from Processor before updating overlay or auto-scroll"
    - "PianoRoll fit baseline prefers shared clip projection over raw buffer-length math"

key-files:
  created: []
  modified:
    - Source/Standalone/UI/PianoRollComponent.h
    - Source/Standalone/UI/PianoRollComponent.cpp

key-decisions:
  - "D18-02-01: PianoRoll keeps its existing absolute-time coordinate model, but the source seconds are now projected from clip samples"
  - "D18-02-02: fitToScreen consumes shared clip projection when clip context is available"

patterns-established:
  - "Pattern: overlay, auto-scroll, and focus baseline share one projected playhead source"

requirements-completed: [TIME-01, TIME-03]

duration: 12min
completed: 2026-04-13
---

# Phase 18 Plan 02: piano roll projection consumption Summary

**PianoRoll 现在不再直接盯 raw absolute seconds，而是统一消费 sample-authoritative clip playhead projection 和 clip duration projection。**

## Performance

- **Duration:** 12 min
- **Started:** 2026-04-13T20:18:00+08:00
- **Completed:** 2026-04-13T20:30:00+08:00
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- 新增 `readProjectedPlayheadTime()`，把播放头与自动滚动切到 shared projection
- 新增 active clip timeline helper，让 PianoRoll 可以直接读取 clip projection
- `fitToScreen()` 改为优先消费 shared clip duration / start projection

## Task Commits

1. **Task 1: 让 PianoRoll 读取 sample-authoritative playhead projection** - `ce9df65` (feat)
2. **Task 2: 让 PianoRoll 的可见 duration/focus baseline 复用 clip projection** - `aa07526` (feat)

## Files Created/Modified

- `Source/Standalone/UI/PianoRollComponent.h` - 声明 projected playhead / active clip projection helper
- `Source/Standalone/UI/PianoRollComponent.cpp` - 让 overlay、auto-scroll、fit baseline 统一消费 shared projection

## Decisions Made

- 不重写 PianoRoll 的绝对时间坐标体系；改的是时间源，而不是再造一套并行 UI 结构
- `trackOffsetSeconds_` 继续存在，但 fit baseline 优先以 shared clip projection 为准

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

- None

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- PianoRoll 已具备直接用回归测试验证 TIME-01 / TIME-03 的条件
- ArrangementView 可以独立推进，不需要再修改 PianoRoll 路径

## Self-Check: PASSED

- Summary file exists at `.planning/phases/18-时间轴投影与 Standalone 守护/18-02-SUMMARY.md`
- Commits `ce9df65` and `aa07526` are present in git history
