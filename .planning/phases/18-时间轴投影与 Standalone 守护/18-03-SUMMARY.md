---
phase: 18-时间轴投影与 Standalone 守护
plan: 03
subsystem: ui
tags: [arrangement, timeline, timeconverter, projection, sample-truth]

requires:
  - phase: 18-01
    provides: shared clip timeline projection helpers in Processor
provides:
  - Arrangement clip bounds derived from shared projection
  - Arrangement auto-scroll math unified with TimeConverter-backed drawing path
affects: [18-04, arrangement, clip-hit-test, timeline]

tech-stack:
  added: []
  patterns:
    - "Arrangement clip painting and hit testing share one projected bounds helper"
    - "Arrangement auto-scroll uses the same converter-backed pixel path as the visible timeline"

key-files:
  created: []
  modified:
    - Source/Standalone/UI/ArrangementViewComponent.h
    - Source/Standalone/UI/ArrangementViewComponent.cpp

key-decisions:
  - "D18-03-01: Arrangement clip rectangles must come from shared clip projection, not ad-hoc startSeconds + numSamples math"
  - "D18-03-02: Arrangement auto-scroll removes duplicate raw zoom math and reuses the existing timeline conversion path"

patterns-established:
  - "Pattern: build projected clip bounds once, then reuse for paint and hit testing"

requirements-completed: [TIME-02, TIME-03]

duration: 8min
completed: 2026-04-13
---

# Phase 18 Plan 03: arrangement projection consumption Summary

**ArrangementView 的 clip range 和滚动像素换算现在都归到 shared projection + TimeConverter，同一条时间轴数学终于同时服务绘制、命中和滚动。**

## Performance

- **Duration:** 8 min
- **Started:** 2026-04-13T20:30:00+08:00
- **Completed:** 2026-04-13T20:38:00+08:00
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- 新增 `buildProjectedClipBounds(...)`，让 Arrangement 的 clip bounds 统一来自 shared projection
- `getClipBounds()` 现在只是 projected helper 的薄封装
- `updateAutoScroll()` / `onScrollVBlankCallback()` 不再复制 raw `100.0 * zoomLevel_` 像素公式

## Task Commits

1. **Task 1: 让 Arrangement clip bounds 改用 shared clip projection** - `a0dfff1` (feat)
2. **Task 2: 统一 ArrangementView 的时间轴换算与自动滚动路径** - `40443e7` (feat)

## Files Created/Modified

- `Source/Standalone/UI/ArrangementViewComponent.h` - 声明 projected clip bounds helper
- `Source/Standalone/UI/ArrangementViewComponent.cpp` - 用 shared projection 重建 clip bounds，并把 auto-scroll 改成 converter-backed path

## Decisions Made

- Arrangement 不再自己维护 clip width 公式；共享 core projection 是唯一 clip range 真相源
- 时间轴滚动继续沿用现有 UI 行为，但底层像素换算必须与绘制一致

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

- None

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Phase 18 回归测试现在可以直接覆盖 Arrangement 的 static contract
- PianoRoll / Arrangement 两条 UI 路径都已经完成 shared projection 收敛

## Self-Check: PASSED

- Summary file exists at `.planning/phases/18-时间轴投影与 Standalone 守护/18-03-SUMMARY.md`
- Commits `a0dfff1` and `40443e7` are present in git history
