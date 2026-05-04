---
phase: 19-主层场景归一
plan: 04
subsystem: ui
tags: [piano-roll, playhead, renderer, overlay-removal, juce]

requires:
  - phase: 19-03
    provides: gate failure evidence and live-tree drift analysis
provides:
  - PianoRoll single-scene-host implementation without a dedicated playhead overlay child
  - Main-layer playhead renderer contract in `PianoRollRenderer`
  - Live-tree-aligned Phase 19 code closure for later fresh verification
affects: [phase-19-verification, phase-20-invalidation, piano-roll]

tech-stack:
  added: []
  patterns:
    - "Main-layer playhead rendering through `PianoRollRenderer::drawPlayhead(...)`"
    - "Single-scene-host PianoRoll wiring with component-owned playhead state"

key-files:
  created:
    - Source/Standalone/UI/PianoRoll/PianoRollRenderer.h
    - Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp
  modified:
    - Source/Standalone/UI/PianoRollComponent.h
    - Source/Standalone/UI/PianoRollComponent.cpp

key-decisions:
  - "以 live tree 为准重建 Phase 19 contract，不再沿用 `19-02-SUMMARY.md` 的失真完成表述。"
  - "Phase 19 只做主层场景归一，不提前删除 `requestInteractiveRepaint()`、`FrameScheduler`、heartbeat 或 timer 路径。"

patterns-established:
  - "Pattern: 播放头状态保存在 `PianoRollComponent`，主层绘制通过 `RenderContext` 注入 renderer"
  - "Pattern: overlay 视觉语义保留，但组件层级删除，只剩主层 clipped paint contract"

requirements-completed: [LAYER-01, LAYER-02]

duration: "unknown (backfilled from committed evidence)"
completed: 2026-04-14
---

# Phase 19 Plan 04: 主层 renderer contract 与单层场景 closure Summary

**PianoRoll 的播放头现在回并到主层 renderer，组件不再持有 overlay 子层，scroll/zoom/playhead 更新终于回到单一场景宿主。**

## Performance

- **Duration:** Unknown - backfilled from committed evidence
- **Started:** Unknown
- **Completed:** 2026-04-14T15:45:35+08:00
- **Tasks:** 2
- **Files modified:** 4 core implementation files

## Accomplishments

- 新建 `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h` 与 `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`，把主层播放头绘制 contract 放回当前 live tree 的真实路径
- 从 `Source/Standalone/UI/PianoRollComponent.h` 与 `Source/Standalone/UI/PianoRollComponent.cpp` 移除 PianoRoll 专属 overlay 持有与同步接线，改为组件自有 `mainLayerPlayheadSeconds_` / `playheadColour_`
- 把 `paint()`、非播放态 seek 更新、VBlank 播放头更新都接回主层 renderer，同时明确保留 `requestInteractiveRepaint()`、`FrameScheduler`、heartbeat 和 timer 路径给 Phase 20/21 处理

## Task Commits

本计划的两个任务最终一起落在同一个实际提交里：

1. **Task 1: 把主层 renderer contract 校正到当前代码树** - `303cd51` (feat)
2. **Task 2: 把 PianoRoll 接线收敛成单层场景宿主** - `303cd51` (feat)

**Plan metadata:** `303cd51` 也同时吸收了后续 verification/state 回写，导致 summary 产物当时漏写。

## Files Created/Modified

- `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h` - 新增主层播放头绘制 contract 与扩展后的 `RenderContext`
- `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp` - 新增 `drawPlayhead(...)` 的主层绘制实现
- `Source/Standalone/UI/PianoRollComponent.h` - 删除 overlay 成员，改为组件持有播放头状态与颜色
- `Source/Standalone/UI/PianoRollComponent.cpp` - 移除 overlay 同步路径，并在内容区 clipped paint 内直接调用 `renderer_->drawPlayhead(...)`

## Decisions Made

- 以当前 live tree 为准恢复 `PianoRollRenderer`，不再接受“计划引用不存在文件路径”的状态继续存在
- 保留 overlay 的视觉语义，但删除 `PianoRollComponent` 对 overlay 组件的拥有关系；Phase 19 的目标是单层场景，不是改变播放头外观
- 不越权触碰 Phase 20/21 的单入口与单时钟问题；本计划只修正主层场景 contract

## Deviations from Plan

### Auto-fixed Issues

**1. [Artifact Drift] Phase 19 code closure 与 verification/state 更新被同一提交吸收**
- **Found during:** 本次补查 Phase 19 summary / roadmap 漂移
- **Issue:** `19-04-PLAN.md` 的代码 closure 与后续 `19-05-PLAN.md` 的验证/状态回写都被压进 `303cd51`，但 `19-04-SUMMARY.md` 当时没有生成
- **Fix:** 本 backfill summary 明确记录真实提交边界与缺失产物，恢复 plan -> summary 链条的一致性
- **Files modified:** `.planning/phases/19-主层场景归一/19-04-SUMMARY.md`
- **Verification:** `git show --stat 303cd51` 与 phase 目录文件清单能同时证明代码已落地、summary 缺失
- **Committed in:** pending current worktree

---

**Total deviations:** 1 auto-fixed (artifact drift)
**Impact on plan:** 不改变实现事实，只把真实执行历史写回文档，避免 progress 路由把 Phase 19 误判成仍有未执行计划。

## Issues Encountered

- `19-02-SUMMARY.md` 一度把 overlay removal 写成已完成，但 live tree 当时并不满足；本计划的真正任务是把计划、summary 和源码重新拉回同一个真实结构上

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Phase 19 的主层场景 contract 已具备进入 fresh verification closure 的前提
- `19-05` 需要基于 fresh `build-phase19-docs` 重建 L1/L2/L4/L6 证据，并把 roadmap/state 对齐到真实 gate 结果

## Self-Check: BACKFILLED

- `19-04-PLAN.md` 所要求的 `PianoRollRenderer` 与单层接线产物已经存在于 live tree
- 本 summary 明确记录了为何该 plan 在 git 历史中只有实现提交、没有独立 summary 产物
