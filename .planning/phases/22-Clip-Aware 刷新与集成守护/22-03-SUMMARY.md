---
phase: 22-Clip-Aware 刷新与集成守护
plan: 03
subsystem: integration
tags: [pianoroll, integration, playhead, fit-to-screen, guards]
requires:
  - phase: 22-Clip-Aware 刷新与集成守护
    provides: renderer clip-aware convergence and fresh build path
provides:
  - PAINT-01 strip-dirty integration guard
  - FLOW-01 fit-to-screen projection guard
affects: [22-04]
tech-stack:
  added: []
  patterns: [integration-guard, retained-regression-gate]
key-files:
  created: []
  modified:
    - Tests/TestMain.cpp
key-decisions:
  - "PAINT_01_* 通过测试 probe 固化 strip dirty + single flush bridge，不为了验证去改生产结构。"
  - "Wave 3 不扩 scope 到 editor shell 全面 repaint 清理；只保留 no-new-cadence / no-new-direct-invalidate 边界。"
patterns-established:
  - "Phase 22 integration closure runs on the same fresh binary as retained Phase 18-21 guards."
requirements-completed: [PAINT-01, FLOW-01]
duration: 0h 0m
completed: 2026-04-15
---

# Phase 22 Plan 03: integration guards Summary

**Phase 22 现在已经有 `PAINT_01_PianoRollPlayheadDirtyStripStillUsesSingleFlushBridge` 和 `FLOW_01_PianoRollFitToScreenUsesClipTimelineProjection` 两条集成守护，而且当前组件实现无需额外改动就能通过。**

## Accomplishments

- 在 `Tests/TestMain.cpp` 新增 `PAINT_01_PianoRollPlayheadDirtyStripStillUsesSingleFlushBridge`，把 strip dirty + single flush bridge 固化为常驻 integration guard。
- 保留并验证 `FLOW_01_PianoRollFitToScreenUsesClipTimelineProjection`，确保 projected start / duration 继续命中 clip timeline projection。
- `PAINT_01_*`、`PAINT_02_*`、`FLOW_01_*` 与 retained `TIME_01_*` / `LAYER_01_*` / `INVAL_01_*` / `CLOCK_01_*` 在同一 fresh binary 中同时为绿。
- 当前 `PianoRollComponent.cpp` 已满足这些 guards，因此 Wave 3 没有引入新的 production component 改动。

## Files Created/Modified

- `Tests/TestMain.cpp` - 新增 `PAINT_01_*` guard，并扩展 `PianoRollComponentTestProbe` 以读取 projected playhead 和 playhead dirty strip。

## Decisions Made

- 通过 test probe 验证 private playhead contract，而不是为了测试暴露新的 public API。
- 既有 `PianoRollComponent` 行为已经满足 Phase 22 集成守护，因此不为了“看起来做了更多事”去改生产代码。

## Issues Encountered

- 无新的实现阻塞；Wave 2 的 renderer 收敛没有引入 playhead / fit-to-screen 回归。

## Self-Check: PASSED

- `PAINT_01_PianoRollPlayheadDirtyStripStillUsesSingleFlushBridge` exists and passes.
- `FLOW_01_PianoRollFitToScreenUsesClipTimelineProjection` passes alongside retained guards.
- No new component-side cadence, overlay, or direct-invalidate path was introduced.
