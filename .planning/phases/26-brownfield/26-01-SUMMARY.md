---
phase: 26-brownfield
plan: 01
subsystem: tests
tags: [ara, cleanup, verification, brownfield]

# Dependency graph
requires:
  - phase: 25-04
    provides: snapshot-only renderer/editor baseline and preferred-region sync truth
provides:
  - Phase 26 verification source in `.planning/phases/26-brownfield/26-TEST-VERIFICATION.md`
  - RED cleanup and brownfield guards in `Tests/TestMain.cpp`
affects: [phase-26-cleanup, roadmap-state, requirements-traceability]

# Tech tracking
tech-stack:
  added: []
  patterns: [verification-source-first, brownfield static audit, red-first cleanup guards]

key-files:
  created:
    - .planning/phases/26-brownfield/26-TEST-VERIFICATION.md
    - .planning/phases/26-brownfield/26-01-SUMMARY.md
  modified:
    - Tests/TestMain.cpp

key-decisions:
  - "Phase 26 先冻结 verification source，再允许删 orphan 和 source-level helper。"
  - "Phase 26 guards 必须同时覆盖 legacy 删除、consumer 不回退、processor 边界不扩张、Standalone 隔离不泄漏。"
  - "RED baseline 必须在 `build-phase26-docs` fresh binary 上成立，后续 cleanup 才有可信入口。"

# Metrics
duration: 6 min
completed: 2026-04-16
---

# Phase 26 Plan 01: Verification Source And RED Guard Summary

**Phase 26 先把 cleanup 的验收口径与 RED 守护固定成唯一事实源，并让 `OpenTuneTests` 直接暴露当前 live tree 仍存在的 orphan / legacy 残留。**

## Performance

- **Duration:** 6 min
- **Started:** 2026-04-16T17:58:02+08:00
- **Completed:** 2026-04-16T18:04:03+08:00
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- 创建 `.planning/phases/26-brownfield/26-TEST-VERIFICATION.md`，固定 `build-phase26-docs`、四条 `CLEAN_*` journeys、L4 brownfield audit 范围与 exit condition
- 在 `Tests/TestMain.cpp` 新增 `runPhase26CleanupBrownfieldTests()`，把 cleanup 守护接到主测试流程，位置紧跟 `runPhase25SnapshotConsumerTests()` 之后
- 用 fresh `build-phase26-docs` + `OpenTuneTests.exe` 跑出 RED baseline，证明当前 tree 仍被 `Source/ARA/AudioSourceState.h` 阻塞，后续 cleanup 有明确目标

## Task Commits

1. **Task 1: 创建 Phase 26 verification source of truth** - `67f451b` (docs)
2. **Task 2: 在 `Tests/TestMain.cpp` 写 Phase 26 cleanup 与 brownfield guards** - `bafca02` (test, RED baseline)

## Files Created/Modified
- `.planning/phases/26-brownfield/26-TEST-VERIFICATION.md` - Phase 26 verification source、L1/L2/L4/L6 口径与 RED baseline
- `Tests/TestMain.cpp` - `runPhase26CleanupBrownfieldTests()` 与主流程接线
- `.planning/phases/26-brownfield/26-01-SUMMARY.md` - 记录 26-01 的验证入口、RED 结果与后续 cleanup 准备度

## Decisions Made
- cleanup 完成标准不是“符号看起来删掉了”，而是 `OpenTuneTests` 能同时锁住 orphan/header/helper/consumer/core-boundary/Standalone-isolation
- Phase 26 的 build 目录固定为 `build-phase26-docs`，避免后续 wave 混用旧 binary 或 Phase 25 产物
- RED baseline 直接写回 verification source，后续 26-02/26-03 不再依赖口头说明 legacy 还在什么地方

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

- 新增 guards 的第一次 fresh 运行按预期失败于 `CLEAN_01_RemovesLegacyAudioSourceStateArtifact`，说明当前 live tree 仍保留 `Source/ARA/AudioSourceState.h`；这正是 26-02 需要删除的首个生产残留

## User Setup Required

None - no manual verification or external credentials required.

## Next Phase Readiness
- 26-02 可以直接删除 orphan header 与 controller source-level helper surface，并以已接入的 Phase 26 guards 转绿作为完成门槛
- `26-TEST-VERIFICATION.md` 已经是本 phase 的唯一 verification source，26-03 只需要补写 GREEN evidence，不必再重定义口径
- shared processor 与 Standalone isolation 的边界检查已经在 tests 中落位，后续 cleanup 不需要触碰 `Source/PluginProcessor.*` 或 `Source/Standalone/PluginEditor.cpp` 逻辑

## Self-Check: PASSED

- Confirmed `.planning/phases/26-brownfield/26-01-SUMMARY.md` exists.
- Confirmed task commits `67f451b` and `bafca02` exist in git history.
