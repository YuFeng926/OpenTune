---
phase: 18-时间轴投影与 Standalone 守护
plan: 04
subsystem: testing
tags: [tests, verification, timeline, piano-roll, arrangement, sample-truth]

requires:
  - phase: 18-02
    provides: PianoRoll projection consumption
  - phase: 18-03
    provides: Arrangement projection consumption
provides:
  - Phase 18 regression coverage for timeline projection semantics
  - Phase 18 verification docs with real L1 L2 L4 and L6 evidence
  - Requirement traceability for TIME-01 TIME-02 and TIME-03
affects: [phase-gate, roadmap, requirements, timeline]

tech-stack:
  added: []
  patterns:
    - "UI VBlank tests that depend on isShowing attach the host component to a desktop peer instead of relaxing production guards"

key-files:
  created:
    - .planning/phases/18-时间轴投影与 Standalone 守护/18-VERIFICATION.md
  modified:
    - Tests/TestMain.cpp
    - .planning/phases/18-时间轴投影与 Standalone 守护/18-TEST-VERIFICATION.md

key-decisions:
  - "D18-04-01: VBlank auto-scroll regressions must exercise a real showing component, not bypass isShowing guards in production code"

patterns-established:
  - "Pattern: verify native JUCE timeline semantics with a desktop-attached host component when VBlank callbacks require isShowing"

requirements-completed: [TIME-01, TIME-02, TIME-03]

duration: 18min
completed: 2026-04-13
---

# Phase 18 Plan 04: timeline projection verification closure Summary

**Phase 18 现在既有真实 timeline projection 回归测试，也有完整的 verification traceability；PianoRoll 与 Arrangement 的 shared projection 收敛已经有可重复证据。**

## Performance

- **Duration:** 18 min
- **Started:** 2026-04-13T20:38:00+08:00
- **Completed:** 2026-04-13T20:56:00+08:00
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments

- 新增 `runPhase18TimelineProjectionTests()`，直接覆盖 TIME-01 / TIME-02 / TIME-03
- 真实执行了 L1 / L2 / L4 / L6，并把结果写回 `18-TEST-VERIFICATION.md`
- 完成 `18-VERIFICATION.md`，把 TIME requirements 逐条绑定到测试名和静态审计证据

## Task Commits

1. **Task 1: 为 Phase 18 时间轴投影补回归测试** - `8add09e` (test)
2. **Task 2: 执行 Phase 18 验证并完成 traceability 文档** - `0cf159f` (docs)

## Files Created/Modified

- `Tests/TestMain.cpp` - 新增 Phase 18 时间轴投影回归测试
- `.planning/phases/18-时间轴投影与 Standalone 守护/18-TEST-VERIFICATION.md` - 回填真实 L1 / L2 / L4 / L6 结果
- `.planning/phases/18-时间轴投影与 Standalone 守护/18-VERIFICATION.md` - requirement 级 traceability

## Decisions Made

- VBlank auto-scroll 测试必须让组件真的进入 showing 状态；正确做法是给测试宿主添加 desktop peer，而不是删掉生产代码的 `isShowing()` 守卫

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] PianoRoll VBlank regression initially never entered auto-scroll logic**
- **Found during:** Task 1 (为 Phase 18 时间轴投影补回归测试)
- **Issue:** `PianoRollComponent::onScrollVBlankCallback()` 需要 `isShowing()` 为真；最初的测试只创建裸组件，没有 desktop peer，导致 VBlank 路径根本未执行。
- **Fix:** 在 Phase 18 两个 PianoRoll regression 中，为宿主组件调用 `addToDesktop(0)`，让测试真正命中 showing/VBlank 路径。
- **Files modified:** `Tests/TestMain.cpp`
- **Verification:** 重新构建 `OpenTuneTests` 并跑完整套后，四个 Phase 18 用例全部通过。
- **Committed in:** `8add09e`

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** 这是验证链路必须解决的真实 gate，没有改变生产行为，也没有引入测试绕过逻辑。

## Issues Encountered

- 第一次 L2 运行暴露了 JUCE `isShowing()` 对 VBlank 测试宿主的真实要求，随后通过 desktop peer 补齐了测试环境

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Phase 18 requirements 已具备 build + test + static-audit 三层证据
- 可以进入 phase-level state / roadmap / requirements 更新和最终 metadata commit

## Self-Check: PASSED

- Summary file exists at `.planning/phases/18-时间轴投影与 Standalone 守护/18-04-SUMMARY.md`
- Commits `8add09e` and `0cf159f` are present in git history
