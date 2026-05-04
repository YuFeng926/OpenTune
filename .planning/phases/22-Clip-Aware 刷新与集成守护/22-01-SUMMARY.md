---
phase: 22-Clip-Aware 刷新与集成守护
plan: 01
subsystem: testing
tags: [testing, pianoroll, clip-aware, verification, docs]
requires:
  - phase: 21-单 VBlank 视觉循环
    provides: single-VBlank visual loop baseline and editor-shell cadence boundary
provides:
  - Phase 22 verification source of truth for clip-aware paint and integration guards
  - RED regressions for visible note window, unified visible time window, and fit-to-screen projection
affects: [22-02, 22-03, 22-04]
tech-stack:
  added: []
  patterns: [test-driven-spec, structural-red-regressions, fresh-build-red-evidence]
key-files:
  created:
    - .planning/phases/22-Clip-Aware 刷新与集成守护/22-TEST-VERIFICATION.md
    - .planning/phases/22-Clip-Aware 刷新与集成守护/22-01-SUMMARY.md
  modified:
    - Tests/TestMain.cpp
    - .planning/phases/22-Clip-Aware 刷新与集成守护/22-02-PLAN.md
    - .planning/phases/22-Clip-Aware 刷新与集成守护/22-03-PLAN.md
key-decisions:
  - "Phase 22 的 toolchain 入口改为 `.planning/scripts/invoke-msvc-cmake.ps1`，通过 `vswhere.exe` 动态发现 `VsDevCmd.bat`，不再依赖机器特定绝对路径。"
  - "RED gate 必须通过 fresh build/run 得到真实失败证据，而不是只 grep 测试名。"
  - "OpenTuneTests.exe 在当前 Ninja 构建下落在 build-phase22-docs 根目录，因此后续 22-02 / 22-03 verify 统一改用该路径。"
patterns-established:
  - "先写验证契约和结构性 RED regressions，再进入 renderer/component 实现。"
  - "用 source-level structural tests 锁死 drawNotes full scan 缺口，避免伪优化绕过测试。"
requirements-completed: []
duration: 0h 0m
completed: 2026-04-15
---

# Phase 22 Plan 01: verification contract and RED baseline Summary

**Phase 22 现在已有 clip-aware paint 的验证契约，并且 fresh binary 已真实失败在 `drawNotes()` full scan 缺口上，后续 Wave 2 可以直接对着这个缺口做 green。**

## Performance

- **Duration:** 0h 0m
- **Started:** 2026-04-15T00:00:00Z
- **Completed:** 2026-04-15T00:00:00Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments

- 创建 `22-TEST-VERIFICATION.md`，固定了 Phase 22 的 L1 / L2 / L4 / L6 gate、Required Journeys 与 retained guards。
- 在 `Tests/TestMain.cpp` 新增 `runPhase22ClipAwarePaintTests()`，先把 visible note window、统一 visible time window、fit-to-screen projection 写成 RED regressions。
- 用当时确认可用的 VS 开发者命令行 + fresh `build-phase22-docs` 构建并运行 `OpenTuneTests.exe`，拿到了当前 live tree 的首轮失败证据。
- 修正了 `22-02-PLAN.md` 与 `22-03-PLAN.md` 的 verify 二进制路径，使其与当前 Ninja 构建产物一致。

## Files Created/Modified

- `.planning/phases/22-Clip-Aware 刷新与集成守护/22-TEST-VERIFICATION.md` - Phase 22 的验证源文件，已回写真实 toolchain 命令和 RED 失败证据。
- `Tests/TestMain.cpp` - 新增 `runPhase22ClipAwarePaintTests()` 与三条 Phase 22 回归名。
- `.planning/phases/22-Clip-Aware 刷新与集成守护/22-02-PLAN.md` - 把 verify 二进制路径对齐到 `build-phase22-docs/OpenTuneTests.exe`。
- `.planning/phases/22-Clip-Aware 刷新与集成守护/22-03-PLAN.md` - 把 verify 二进制路径对齐到 `build-phase22-docs/OpenTuneTests.exe`。
- `.planning/phases/22-Clip-Aware 刷新与集成守护/22-01-SUMMARY.md` - 记录 Wave 1 结果，供后续 plans 继续消费。

## Decisions Made

- 采用 source-level structural RED tests 直接锁定 `drawNotes()` full scan residue 和 unified visible-time-window helper 缺口，不给“full scan + if”留下伪优化空间。
- `FLOW_01_PianoRollFitToScreenUsesClipTimelineProjection` 直接用 runtime host + clip projection 验证 projected start / duration，而不是只做静态 grep。
- 当前 Phase 22 仍不扩张到 editor shell 全面 repaint 清理；本轮只保留 no-new-cadence / no-new-direct-invalidate 边界审计。

## Deviations from Plan

- 构建系统实际生成的是 `build-phase22-docs/OpenTuneTests.exe`，而不是先前计划里假定的 `Release/OpenTuneTests.exe`；该差异已在同一轮执行中回写到相关 plans。

## Issues Encountered

- 首轮 fresh build 并没有失败在业务缺口，而是先失败在测试代码直接调用了 private `timeToX()`；这已通过 `PianoRollComponentTestProbe` 修正为合法测试访问。
- 当前 live tree 的首个真实 RED failure 是 `PAINT_02_PianoRollDrawNotesUsesVisibleNoteWindow`，说明 `drawNotes()` 仍未做 visible note window 收敛。

## User Setup Required

- None for Wave 2 implementation. 当前机器上的可用 toolchain 路径已经识别并写入 `22-TEST-VERIFICATION.md`。

## Next Phase Readiness

- `22-02` 可以直接围绕 `drawNotes()` full scan residue 和 `computeVisibleTimeWindow(...)` helper 缺口做 green。
- `22-03` 不需要重新猜 toolchain 或二进制路径，可以直接复用 `22-TEST-VERIFICATION.md` 中的确认结果。

## Self-Check: PASSED

- `22-TEST-VERIFICATION.md` exists and contains the Phase 22 title, required journeys, and confirmed toolchain command.
- `Tests/TestMain.cpp` contains `runPhase22ClipAwarePaintTests` and the three required Phase 22 test names.
- Fresh `build-phase22-docs/OpenTuneTests.exe` was built and the current live tree failed at the expected Phase 22 gap instead of a stale-binary path.
