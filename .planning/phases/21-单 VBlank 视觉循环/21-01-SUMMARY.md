---
phase: 21-单 VBlank 视觉循环
plan: 01
subsystem: testing
tags: [pianoroll, vblank, verification, tests]
requires:
  - phase: 20-统一失效入口
    provides: unified invalidation reducer and component-level VBlank flush baseline
provides:
  - Phase 21 verification source of truth
  - Single-VBlank regression test baseline in OpenTuneTests
affects: [21-02, 21-03, 21-04]
tech-stack:
  added: []
  patterns: [test-driven-spec, single-vblank-regression-baseline]
key-files:
  created: [.planning/phases/21-单 VBlank 视觉循环/21-TEST-VERIFICATION.md, .planning/phases/21-单 VBlank 视觉循环/21-01-SUMMARY.md]
  modified: [Tests/TestMain.cpp]
key-decisions:
  - "Phase 21 gate fixed to L1/L2/L4/L6 with explicit static-audit anchors before implementation starts."
  - "Single-VBlank cadence is locked by direct PianoRoll host tests instead of editor-driven harnesses."
patterns-established:
  - "Phase-first verification: write test-verification contract before touching cadence code."
  - "Editor timer non-dependency is proven with PianoRoll-only host tests."
requirements-completed: [CLOCK-01, CLOCK-02, FLOW-02]
duration: 0h 0m
completed: 2026-04-15
---

# Phase 21 Plan 01: Verification Contract Summary

**Phase 21 now has an explicit single-VBlank verification contract plus a three-test regression baseline for playhead/scroll flush, decoration-waveform drain, and editor-timer independence**

## Performance

- **Duration:** 0h 0m
- **Started:** 2026-04-15T00:00:00Z
- **Completed:** 2026-04-15T00:00:00Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Added `.planning/phases/21-单 VBlank 视觉循环/21-TEST-VERIFICATION.md` as the Phase 21 verification source of truth.
- Added `runPhase21SingleVBlankVisualLoopTests()` with three named regressions in `Tests/TestMain.cpp`.
- Kept Phase 18/19/20 guards explicitly listed as Phase 21 baseline requirements.

## Task Commits

Each task was committed atomically:

1. **Task 1: 固化 Phase 21 test-verification 契约** - `060c83d` (docs)
2. **Task 2: 先写 Phase 21 single-VBlank RED regressions** - `0dff773` (test)

**Plan metadata:** pending orchestrator closeout

## Files Created/Modified
- `.planning/phases/21-单 VBlank 视觉循环/21-TEST-VERIFICATION.md` - Phase 21 verification source of truth with fixed L1/L2/L4/L6 gates and required journeys.
- `Tests/TestMain.cpp` - Adds `runPhase21SingleVBlankVisualLoopTests()` and the three Phase 21 regression names.
- `.planning/phases/21-单 VBlank 视觉循环/21-01-SUMMARY.md` - Captures the Wave 1 outcome for downstream plans.

## Decisions Made
- Locked Phase 21 around explicit static-audit anchors, including `onVisualVBlankCallback(`, `visualVBlankAttachment_`, editor timer usage, and direct PianoRoll invalidation paths.
- Used direct `PianoRollComponent` host tests to prove cadence behavior so Wave 2 can refactor internals without needing editor harnesses.

## Deviations from Plan

None in task intent. Execution happened on a dirty working tree, and `Tests/TestMain.cpp` already contained pre-existing unstaged changes; the user approved continuing with file-boundary commits for Phase 21 files.

## Issues Encountered

- The initial Wave 1 executor agent returned without creating any artifacts, so execution fell back to inline handling.
- `Tests/TestMain.cpp` already had unrelated unstaged hunks in the same file. Because the user chose Phase-21 file-boundary execution, the task-2 commit followed that boundary instead of hunk-level isolation.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Wave 2 can now implement the single visual tick against explicit test names and verification gates.
- `Tests/TestMain.cpp` still carries pre-existing same-file changes from the dirty worktree, so later Wave 2/3 commits should continue respecting the user-approved file boundary.

## Self-Check: PASSED

- `21-TEST-VERIFICATION.md` exists and contains the required Phase 21 anchors.
- `Tests/TestMain.cpp` contains `runPhase21SingleVBlankVisualLoopTests` and the three required test names.
- Plan commits for `21-01` are present in git history.
