---
phase: 24-callback-sample-access
plan: 02
subsystem: testing
tags: [ara, lifecycle, tests, sample-access, didEndEditing]

# Dependency graph
requires:
  - phase: 24-01
    provides: lifecycle callback contract, metadata-only property callback, and Phase 24 verification source
provides:
  - Executable Phase 24 lifecycle guards in `OpenTuneTests`
  - Callback-driven source-slot establishment for content and sample-access lifecycle paths
  - Direct automated evidence for `LIFE-01`, `LIFE-02`, and `LIFE-03`
affects: [phase-24-wave-3, lifecycle-regressions, verification-evidence]

# Tech tracking
tech-stack:
  added: []
  patterns: [source-audited lifecycle guards, test-driven snapshot publication proof, callback-established source slots]

key-files:
  created:
    - .planning/phases/24-callback-sample-access/24-02-SUMMARY.md
  modified:
    - Tests/TestMain.cpp
    - Source/ARA/OpenTuneDocumentController.cpp

key-decisions:
  - "Phase 24 的 lifecycle 验收不只看 symbol 是否存在，还要求 source audit 直接锁住 no-retry 与 enable-only copy 入口。"
  - "`doUpdateAudioSourceContent()` 与 `didEnableAudioSourceSamplesAccess()` 都必须先 `ensureSourceSlot(audioSource)`，否则 callback lifecycle 在 host 顺序变化下会静默丢状态。"
  - "`publishPendingSnapshot(...)` 继续作为 batch-end publish 的可执行证明，避免把 callback 内即时 publish 误判为正确实现。"

patterns-established:
  - "Source-audited lifecycle guard: 用仓库源码审计锁住 callback ownership，再用 executable snapshot helper 证明 publish gate。"
  - "Callback-established source slots: content/sample-access callback 自己建立最小 source state，不依赖 property callback 先行。"
  - "Green-on-guard workflow: 新增 journey 先打红，再用最小 controller 改动把整套 `OpenTuneTests` 转绿。"

requirements-completed: [LIFE-01, LIFE-02, LIFE-03]

# Metrics
duration: 2 min
completed: 2026-04-16
---

# Phase 24 Plan 02: Lifecycle Guards And Publish Gate Summary

**Phase 24 现在有可执行的 sample-access / content-dirty lifecycle guards，并且 controller 已按这些 guards 收敛到 callback-owned source-slot state 与 didEndEditing publish gate。**

## Performance

- **Duration:** 2 min
- **Started:** 2026-04-16T15:53:58+08:00
- **Completed:** 2026-04-16T15:55:43+08:00
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- 在 `Tests/TestMain.cpp` 新增 `runPhase24LifecycleCallbackTests()`，把 `LIFE_01` / `LIFE_02` / `LIFE_03` 变成可执行 guard 并接入主测试流程
- 通过一次真实 RED 运行确认旧实现没有在 content/sample-access callback 中主动建立 source lifecycle state
- 在 `OpenTuneDocumentController.cpp` 补 `ensureSourceSlot(audioSource)`，让 sample-access 与 content callbacks 不再依赖 property callback 先行
- 用 `build-phase24-docs` 构建并跑通 `OpenTuneTests.exe`，直接拿到前 3 条 requirements 的自动化证据

## Task Commits

Each task was committed atomically:

1. **Task 1: 先写 Phase 24 lifecycle guards，再把主测试流程接到新 section** - `fcb52ae` (test)
2. **Task 2: 让 controller 以 tests 为准转绿，完成 sample-access 与 content-dirty publish 语义** - `d533a8a` (fix)

**Plan metadata:** pending final docs/state commit for Phase 24 execution artifacts.

## Files Created/Modified
- `Tests/TestMain.cpp` - `runPhase24LifecycleCallbackTests()`、三条 lifecycle journeys、主流程接线与 source-audit helpers
- `Source/ARA/OpenTuneDocumentController.cpp` - callback 内 `ensureSourceSlot(audioSource)`、稳定的 source lifecycle state 建立与绿色实现
- `.planning/phases/24-callback-sample-access/24-02-SUMMARY.md` - 记录 24-02 的 TDD 红绿过程、证据与 readiness

## Decisions Made
- 生命周期 tests 同时覆盖 source audit 与 snapshot publish helper，避免只验证文本或只验证 helper
- `didEnableAudioSourceSamplesAccess()` 在不需要 copy 时也要显式维护 `sampleAccessEnabled`，否则 lifecycle state 会再次悬空
- Wave 2 只关闭 `LIFE-01/02/03`；fresh verification doc 回写与 `LIFE-04` 仍由 Wave 3 负责

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] Callback lifecycle now establishes source slots before mutation**
- **Found during:** Task 1 (先写 Phase 24 lifecycle guards，再把主测试流程接到新 section)
- **Issue:** `doUpdateAudioSourceContent()` 与 `didEnableAudioSourceSamplesAccess()` 依赖已有 `SourceSlot`，如果 host callback 顺序先于 property callback，会静默丢失 dirty/copy lifecycle state
- **Fix:** 在相关 callback 与 copy helper 中统一改用 `ensureSourceSlot(audioSource)` 建立最小 source state，再继续 dirty/copy 流程
- **Files modified:** `Source/ARA/OpenTuneDocumentController.cpp`
- **Verification:** `build-phase24-docs` 构建通过，`OpenTuneTests.exe` 中 `LIFE_02_SampleAccessLifecycleOnlyUsesEnableCallbacks` 从 FAIL 转 PASS
- **Committed in:** `d533a8a`

---

**Total deviations:** 1 auto-fixed (1 missing critical)
**Impact on plan:** 该修复直接保障 callback-owned lifecycle 在真实 host callback 顺序下不会丢状态，没有引入额外范围扩张。

## Issues Encountered

- RED 运行首次失败在 `LIFE_02_SampleAccessLifecycleOnlyUsesEnableCallbacks`，证明确实存在 callback 先行时的 source-state 漏建问题；随后按 tests 定位并修复

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Wave 3 可以在现有 `runPhase24LifecycleCallbackTests()` 上继续补 removal lifecycle guards，关闭 `LIFE-04`
- 当前 controller 已由 tests 证明不再使用 `Timer::callAfterDelay` / `sampleAccessRetryCount`，且 content change 仍保持 batch-end publish gate
- `24-TEST-VERIFICATION.md` 仍待 fresh GREEN evidence 回写，Phase 24 还不能在文档层正式关闭

## Self-Check: PASSED

- Confirmed `.planning/phases/24-callback-sample-access/24-02-SUMMARY.md` exists.
- Confirmed task commits `fcb52ae` and `d533a8a` exist in git history.

---
*Phase: 24-callback-sample-access*
*Completed: 2026-04-16*
