---
phase: 20-统一失效入口
plan: 01
subsystem: testing
tags: [testing, piano-roll, invalidation, repaint, docs]

requires:
  - phase: 19-主层场景归一
    provides: PianoRoll single-scene-host baseline and Phase 18 projected-playhead guards
provides:
  - Phase 20 verification source of truth for unified invalidation gates
  - RED regressions for dirty-rect union, full-repaint promotion, and interactive-priority merge
affects: [phase-20, invalidation, piano-roll, regression-suite]

tech-stack:
  added: []
  patterns:
    - "Lock the unified invalidation reducer contract in tests before editing PianoRollComponent wiring"
    - "Keep RED honest by referencing the future PianoRollVisualInvalidation contract directly instead of inventing compatibility aliases"

key-files:
  created:
    - .planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md
  modified:
    - Tests/TestMain.cpp

key-decisions:
  - "D20-01-01: Phase 20 verification gates are fixed to build-phase20-docs and must continue carrying Phase 18/19 guards"
  - "D20-01-02: RED tests include the future PianoRollVisualInvalidation header directly so Phase 20 cannot land through repaint compatibility hooks"

patterns-established:
  - "Pattern: document L1/L2/L4/L6 contract first, then force the missing reducer surface to fail in OpenTuneTests"

requirements-completed: []

duration: 25min
completed: 2026-04-14
---

# Phase 20 Plan 01: unified invalidation contract and RED baseline Summary

**Phase 20 现在已有统一失效入口的验证契约，以及直接约束 dirty union、full repaint 提升和 interactive priority merge 的 RED 回归入口。**

## Performance

- **Duration:** 25 min
- **Started:** 2026-04-14T15:19:19.421Z
- **Completed:** 2026-04-14T23:44:01.3006321+08:00
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- 创建 `20-TEST-VERIFICATION.md`，把 Phase 20 的 L1 / L2 / L4 / L6 gate、Required Journeys 与静态审计口径一次性写死
- 在 `Tests/TestMain.cpp` 新增 `runPhase20UnifiedInvalidationTests()`，先把 reducer contract 名称与三条核心行为事实固定成 RED regressions
- 保留 `TIME_01_*` 与 `LAYER_01_*` 守护在 Phase 20 contract 中继续作为回归底线，避免越权偷改播放头语义或主层场景结构

## Task Commits

Each task was committed atomically:

1. **Task 1: 固化 Phase 20 test-verification 契约** - `b9ff010` (test)
2. **Task 2: 先写 Phase 20 unified invalidation RED regressions** - `b002a2f` (test)

## Files Created/Modified

- `.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md` - 固定 Phase 20 的统一验证源、四级 gate、Required Journeys 与 Evidence To Capture
- `Tests/TestMain.cpp` - 新增 `runPhase20UnifiedInvalidationTests()` 并把 future reducer contract 写成显式 RED regressions

## Decisions Made

- 直接用未来的 `PianoRollVisualInvalidation.h`、`PianoRollVisualInvalidationState`、`PianoRollVisualInvalidationRequest` 与 `makeVisualFlushDecision(...)` 做 RED 约束，不给旧 `requestInteractiveRepaint()` / `requestRepaint` 路径留下兼容层空间
- Phase 20 只冻结统一失效入口 contract，不把 Phase 21 的单 VBlank 时钟收敛偷渡进本计划的完成条件

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

- `.planning/` 受 `.gitignore` 保护，新建 `20-TEST-VERIFICATION.md` 时需要显式 `git add -f`
- `Tests/TestMain.cpp` 已有未提交内容，因此 Task 2 提交时通过 index-only staging 只提交本次 Phase 20 hunks，保留其余未提交改动不进任务 commit
- fresh `build-phase20-docs` 构建按预期在 `Tests/TestMain.cpp` 的新 include 处报错，证明 reducer contract 仍处于 RED 状态而没有被旧 repaint API 旁路掩盖

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- `20-02` 可以直接实现 `PianoRollVisualInvalidation` reducer contract，并以现有 RED regressions 做 green 目标
- 当前工作树里与本计划无关的未提交改动仍保持原样，未被纳入本计划的两个 task commits

## Self-Check: PASSED

- Summary file exists at `.planning/phases/20-统一失效入口/20-01-SUMMARY.md`
- Commits `b9ff010` and `b002a2f` are present in git history
- Stub scan on `.planning/phases/20-统一失效入口/20-TEST-VERIFICATION.md` and `Tests/TestMain.cpp` found no tracked placeholders
