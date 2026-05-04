---
phase: 19-主层场景归一
plan: 01
subsystem: testing
tags: [piano-roll, layering, regression, verification, red-baseline]

requires:
  - phase: 18-04
    provides: Phase 18 projected-playhead regressions and desktop-host verification pattern
provides:
  - Phase 19 verification contract for main-layer scene unification
  - Phase 19 RED regression entry for no-overlay-child and projected-scroll guards
  - Traceability from LAYER requirements to concrete test names and gate commands
affects: [19-02, 19-03, piano-roll, layering]

tech-stack:
  added: []
  patterns:
    - "Lock verification gates before touching PianoRoll layering implementation"
    - "Keep no-overlay-child and projected-scroll regressions in the same OpenTuneTests entry"

key-files:
  created:
    - .planning/phases/19-主层场景归一/19-01-SUMMARY.md
  modified:
    - .planning/phases/19-主层场景归一/19-TEST-VERIFICATION.md
    - Tests/TestMain.cpp

key-decisions:
  - "Keep Phase 18 TIME regressions as mandatory Phase 19 guards instead of renaming or loosening them"
  - "Reuse the Phase 18 projected-playhead setups verbatim for the new LAYER scroll regressions so later overlay removal cannot change scroll semantics"

patterns-established:
  - "Pattern: define the verification contract first, then add RED regressions before implementation"
  - "Pattern: desktop-attached JUCE hosts stay required for VBlank-driven PianoRoll regression coverage"

requirements-completed: []

duration: 34min
completed: 2026-04-14
---

# Phase 19 Plan 01: main-layer scene baseline Summary

**Phase 19 现在有固定下来的验证契约和主层场景 RED 基线：overlay 子层不允许回流，projected playhead 的 continuous/page scroll 语义被明确绑定到后续实现。**

## Performance

- **Duration:** 34 min
- **Started:** 2026-04-14T03:32:57Z
- **Completed:** 2026-04-14T04:06:57Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- 固化了 Phase 19 的 L1 / L2 / L4 / L6 gate、Required Journeys 和 Evidence To Capture，后续执行不再需要猜测验证目标。
- 在 `OpenTuneTests` 中新增 `runPhase19MainLayerSceneTests()`，把 `LAYER_01_PianoRollHasNoPlayheadOverlayChild`、continuous scroll 和 page scroll 三条事实先写成 RED 基线。
- 继续把 `TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead` 与 `TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead` 作为 Phase 19 的既有守护，没有放宽旧断言。

## Task Commits

Each task was committed atomically:

1. **Task 1: 固化 Phase 19 test-verification 契约** - `d4b5b08` (docs)
2. **Task 2: 先写 Phase 19 主层场景 failing regressions** - `342ea76` (test)

## Files Created/Modified

- `.planning/phases/19-主层场景归一/19-TEST-VERIFICATION.md` - 固定 Phase 19 的 gate 命令、Required Journeys 与静态审计证据点。
- `Tests/TestMain.cpp` - 新增 `runPhase19MainLayerSceneTests()`，把 no-overlay-child 和 projected scroll 语义写成后续实现必须转绿的回归测试。
- `.planning/phases/19-主层场景归一/19-01-SUMMARY.md` - 记录本次 Wave 1 基线工作的结果、风险与后续准备情况。

## Decisions Made

- 不把 Phase 18 的 TIME 回归重命名成 Phase 19 测试；它们继续作为 Layer 收敛后的守护基线，避免实现阶段偷改语义。
- `LAYER_01_*` / `LAYER_02_*` 的 scroll 场景直接复用 Phase 18 的 projected-playhead 参数，确保后续 overlay 删除只允许改变层结构，不允许改变滚动事实。

## Deviations from Plan

None - plan content changes stayed within the requested Wave 1 scope and没有提前实现 19-02 / 19-03。

## Issues Encountered

- Fresh L1 build was blocked during CMake regeneration because `ThirdParty\onnxruntime-win-x64-1.24.4\include` is missing in the local workspace; CMake also warned that `ThirdParty\ARA_SDK-releases-2.2.0` is absent, so no fresh test binary could be produced in this session.
- The existing `build\OpenTuneTests.exe` still runs, but its output does not include `Phase 19: Main Layer Scene Tests`, which confirms it is a stale pre-Phase-19 binary and cannot serve as fresh RED evidence.
- Several plan-referenced files were not readable from the current workspace paths: `docs/plans/2026-04-14-pianoroll-refresh-implementation-plan.md`, `docs/plans/2026-04-14-pianoroll-refresh-test-verification.md`, and `Source/Standalone/UI/PlayheadOverlayComponent.h`. The plan was still executable because the target files and required behaviors were explicit in `19-01-PLAN.md` and existing PianoRoll source/tests.

## User Setup Required

Fresh Phase 19 L1 / L2 / L6 verification needs the local build dependencies restored first:

- Restore `ThirdParty\onnxruntime-win-x64-1.24.4\include` so CMake can regenerate successfully.
- Restore `ThirdParty\ARA_SDK-releases-2.2.0` if ARA-enabled builds are expected in this workspace.

## Next Phase Readiness

- Phase 19 now has a fixed verification source of truth and a source-level RED baseline, so 19-02 can remove the overlay only against explicit guards.
- Before anyone claims the new regressions are executing live, the workspace needs a fresh `OpenTuneTests.exe` rebuilt from current source.

## Self-Check: PASSED

- Summary file exists at `.planning/phases/19-主层场景归一/19-01-SUMMARY.md`.
- Task commits `d4b5b08` and `342ea76` are present in git history.
