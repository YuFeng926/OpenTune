---
phase: 21-单 VBlank 视觉循环
plan: 02
subsystem: pianoroll
tags: [pianoroll, vblank, cadence, scroll]
requires:
  - phase: 21-单 VBlank 视觉循环
    provides: single-VBlank verification contract and regression baseline
provides:
  - Single visual cadence inside PianoRollComponent
  - Removal of non-VBlank auto-scroll side path
affects: [21-03, 21-04]
tech-stack:
  added: []
  patterns: [single-vblank-visual-loop, projected-playhead, unified-flush]
key-files:
  created: [.planning/phases/21-单 VBlank 视觉循环/21-02-SUMMARY.md]
  modified: [Source/Standalone/UI/PianoRollComponent.h, Source/Standalone/UI/PianoRollComponent.cpp]
key-decisions:
  - "setScrollMode() no longer advances page scroll outside the PianoRoll VBlank loop."
  - "onVisualVBlankCallback(...) remains the only visual cadence entry for playhead, scroll, decoration, waveform work, and final flush."
patterns-established:
  - "Scroll-mode changes only enqueue visual work; the next VBlank owns the actual visual advance."
  - "Phase 21 reuses the Wave 1 regression suite as the green proof for the live implementation."
requirements-completed: [CLOCK-01, CLOCK-02]
duration: 0h 0m
completed: 2026-04-15
---

# Phase 21 Plan 02: Single Visual Tick Summary

**PianoRollComponent now keeps scroll progression inside the single VBlank visual loop instead of allowing `setScrollMode()` to trigger an out-of-band page-scroll update**

## Performance

- **Duration:** 0h 0m
- **Started:** 2026-04-15T00:00:00Z
- **Completed:** 2026-04-15T00:00:00Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Removed the `setScrollMode() -> updateAutoScroll()` side path so mode changes no longer create a second scroll cadence outside VBlank.
- Deleted the redundant `updateAutoScroll()` helper and left `onVisualVBlankCallback(...)` as the single visual-loop owner.
- Rebuilt `OpenTuneTests` in `build-phase21-docs` and turned the Phase 21 regression suite green on the live tree.

## Files Created/Modified
- `Source/Standalone/UI/PianoRollComponent.h` - removes the extra auto-scroll entry point from the component interface and mode switch path.
- `Source/Standalone/UI/PianoRollComponent.cpp` - removes the duplicated page-scroll helper so all visual work stays under `onVisualVBlankCallback(...)`.
- `.planning/phases/21-单 VBlank 视觉循环/21-02-SUMMARY.md` - captures the Wave 2 convergence and fresh verification result.

## Verification
- L1 fresh build passed with `build-phase21-docs` producing `build-phase21-docs/OpenTuneTests.exe`.
- L2 regression run passed and included `CLOCK_01_PianoRollSingleVisualTickFlushesPlayheadAndScroll`, `CLOCK_01_PianoRollSingleVisualTickDrainsDecorationAndWaveformWork`, `CLOCK_02_PianoRollVisualLoopDoesNotNeedEditorTimerToFlush`.
- Retained guards stayed green: `TIME_01_*`, `LAYER_01_*`, `INVAL_02_*`, `INVAL_01_*`.

## Decisions Made
- Chose the structural fix instead of preserving a mode-switch helper, because any immediate scroll update outside VBlank would keep a second cadence alive.
- Kept projected playhead, waveform incremental build, render decoration, and unified invalidation reducer behavior unchanged; only the duplicate scroll entry was removed.

## Deviations from Plan

`Tests/TestMain.cpp` already had the needed Wave 1 coverage, so Wave 2 did not need new tests; the task closed by turning the existing regressions green on fresh binaries.

## Issues Encountered

- The original L1 command needed Windows quoting cleanup before `VsDevCmd.bat` and CMake could run correctly from PowerShell.

## Next Phase Readiness

- Wave 3 can now remove the remaining editor-side visual-clock residue without risking a component-level cadence split.
- Fresh Phase 21 build output is available for Wave 4 evidence capture.

## Self-Check: PASSED

- `Source/Standalone/UI/PianoRollComponent.cpp` contains one `onVisualVBlankCallback(...)` implementation and no `updateAutoScroll()` helper.
- `build-phase21-docs/OpenTuneTests.exe` was rebuilt and Phase 21 regressions passed.
