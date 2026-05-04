---
phase: 24-callback-sample-access
plan: 01
subsystem: ara
tags: [ara, lifecycle, sample-access, snapshot, verification]

# Dependency graph
requires:
  - phase: 23-03
    provides: snapshot-first controller contract, immutable published snapshot, and snapshot-only read-side guards
provides:
  - Phase 24 verification source of truth with fixed lifecycle journeys and build/test gates
  - Official callback-driven source lifecycle contract in `OpenTuneDocumentController`
  - Metadata-only audio-source property updates with eager-read and retry entrypoints removed
affects: [phase-24-wave-2, phase-24-wave-3, controller-lifecycle, tests]

# Tech tracking
tech-stack:
  added: []
  patterns: [callback-owned sample access, metadata-only property callback, didEndEditing publish gate]

key-files:
  created:
    - .planning/phases/24-callback-sample-access/24-TEST-VERIFICATION.md
    - .planning/phases/24-callback-sample-access/24-01-SUMMARY.md
  modified:
    - Source/ARA/OpenTuneDocumentController.h
    - Source/ARA/OpenTuneDocumentController.cpp

key-decisions:
  - "Phase 24 先冻结 verification source，再允许实现进入 callback lifecycle 改造，避免后续 wave 再发明 gate。"
  - "`didUpdateAudioSourceProperties()` 收敛为 metadata-only callback，host sample copy 只允许经由 sample-access enable lifecycle 进入。"
  - "保留 Phase 23 的 `didEndEditing()` batch-end publish gate，不把 sample-access 或 content-dirty 变化升级成即时 publish。"

patterns-established:
  - "Verification-first lifecycle contract: 先把 L1/L2/L4/L6 gate、journey 和 evidence 写进 phase verification doc。"
  - "Explicit source lifecycle state: 用 `sampleAccessEnabled` / `contentDirty` / `pendingCopyFromHost` 代替 retry 计数。"
  - "Callback ownership split: metadata 在 property callback，audio copy 在 sample-access callback，publish 仍由 batch-end gate 控制。"

requirements-completed: [LIFE-01, LIFE-02, LIFE-03]

# Metrics
duration: 2 min
completed: 2026-04-16
---

# Phase 24 Plan 01: Callback Lifecycle Contract Summary

**Phase 24 现在有固定的 callback-driven lifecycle 验证契约，并且 controller 已切断 eager read / retry 入口，后续 wave 只需在这套 contract 上补 tests 与 removal purge。**

## Performance

- **Duration:** 2 min
- **Started:** 2026-04-16T15:46:34+08:00
- **Completed:** 2026-04-16T15:47:13+08:00
- **Tasks:** 3
- **Files modified:** 3

## Accomplishments
- 创建 `24-TEST-VERIFICATION.md`，固定 Phase 24 的 L1/L2/L4/L6 gate、required journeys 与 evidence capture
- 在 `OpenTuneDocumentController.h` 冻结官方 callback 名称、显式 source lifecycle state，以及后续 removal helper contract
- 在 `OpenTuneDocumentController.cpp` 把 property callback 收敛为 metadata-only，并删除 delayed retry / eager read 入口
- 用 `build-phase24-docs` 跑通 `OpenTuneTests` 构建 gate，确认 contract 改造未把 tree 改断

## Task Commits

Each task was committed atomically:

1. **Task 1: 创建 Phase 24 test-verification source of truth** - `aaefd5f` (docs)
2. **Task 2: 在 header 冻结 callback-driven source lifecycle contract** - `12dbc99` (refactor)
3. **Task 3: 在 cpp 建立 callback-driven lifecycle scaffolding，并切断 eager read/retry 入口** - `9641ad4` (refactor)

**Plan metadata:** pending final docs/state commit for Phase 24 execution artifacts.

## Files Created/Modified
- `.planning/phases/24-callback-sample-access/24-TEST-VERIFICATION.md` - Phase 24 的 verification source of truth 与后续 GREEN evidence 落点
- `Source/ARA/OpenTuneDocumentController.h` - 官方 lifecycle callback declarations、显式 source-slot lifecycle state、后续 removal helper contract
- `Source/ARA/OpenTuneDocumentController.cpp` - metadata-only property callback、sample-access callback scaffolding、无 retry 的 host-audio copy helper
- `.planning/phases/24-callback-sample-access/24-01-SUMMARY.md` - 记录 24-01 的执行结果、提交与下一 wave 准备状态

## Decisions Made
- 先冻结 verification contract，再推进 lifecycle 改造，避免 Wave 2/3 对验收标准产生分叉
- `copyAudioSourceSamples()` 不再 bump content revision；content revision 只允许由 content update callback 驱动
- `didEnableAudioSourceSamplesAccess()` 只在需要 copied audio 或待处理 host copy 时触发 copy helper，避免 property callback 回流

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

- `.planning/` 仍被 `.gitignore` 忽略，因此 verification doc 与 summary 后续提交需要 `git add -f`

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Wave 2 可以直接在 `24-TEST-VERIFICATION.md` 上补 `runPhase24LifecycleCallbackTests()`，把 `LIFE-01/02/03` 变成可执行 guard
- 当前 controller 已去掉 `tryReadAudioData`、`juce::Timer::callAfterDelay` 与 `sampleAccessRetryCount`，后续实现无需再绕回旧入口
- removal purge 仍留待 Wave 3 通过 TDD 接入 shared helper 与 immediate publish

## Self-Check: PASSED

- Confirmed `.planning/phases/24-callback-sample-access/24-01-SUMMARY.md` exists.
- Confirmed task commits `aaefd5f`, `12dbc99`, and `9641ad4` exist in git history.

---
*Phase: 24-callback-sample-access*
*Completed: 2026-04-16*
