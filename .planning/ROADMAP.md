# Roadmap: OpenTune 主工作区

## Overview

路线图只保留 milestone 级摘要。当前活跃主线为 **v1.5**。

2026-05-29 代码审计确认：时间线渲染架构已收敛到正确状态（超扫描渲染带 + 离屏表面 +
独立 PlayheadOverlay + FrameScheduler 合并）。之前 STATE.md 描述的"steady scroll
触发 full repaint / rebuild"问题已在历史重构中解决，`smoothScrollCurrent_` 已不存在。

## Milestones

| Milestone | Phase Range | Status | Archive |
|-----------|-------------|--------|---------|
| v1.0 合并基础 | 1-6 | Complete | - |
| v1.1 统一播放读路径 | 7-10 | Complete | - |
| v1.2 VST3 clip 渲染链路收敛 | 11-14 | Complete | - |
| v1.3 样本域边界收敛 | 15-18 | Complete | - |
| v1.3.1 PianoRoll 刷新体系收敛 | 19-22 | Shipped 2026-04-15 | - |
| v1.3.2 ARA2 线程模型与快照架构 | 23-26 | Shipped 2026-04-16 | `.planning/milestones/v1.3.2-*` |
| v1.4 Source/Materialization/Placement truth | - | Shipped/Frozen | `.planning/milestones/v1.4-ROADMAP.md` |
| **v1.5 当前活跃** | - | **Active** | - |

## Current Active Lines

### v1.5 已落地主线

- Custom UndoManager + PianoRollEditAction
- PianoRollCorrectionWorker
- PlayheadOverlayComponent
- RenderBadgeComponent
- F0Timeline
- ONNX Runtime 内存优化
- GPU/CPU 推理后端重构
- VST3 ARA multi-region / regular-VST3 分流 / multi-item birth
- Standalone 累积功能
- AUTO(REF) 主合同
- TimeTool identity seed
- Timeline rendering architecture（已收敛 ✓）
- 代码质量清理：StandaloneArrangementHelpers.h + TrackConstants.h

### v1.5 当前优先级

- Standalone visual smoke（含 UI 修复验证）
- Undo/Redo 边界测试
- CorrectionWorker 并发验证
- `OpenTuneTests.exe ui` exit=1 修复
- Arrangement min-zoom waveform + cross-track drag preview

## Open Work

- Undo/Redo 边界测试
- CorrectionWorker 并发验证
- `ui` suite exit=1 待解释
- L5 手工旅程：Standalone/VST3 undo、宿主验证、macOS bundle inspection
- Arrangement min-zoom waveform + cross-track drag preview

## Active Planning Docs

- `.planning/plans/2026-05-27-arrangement-min-zoom-waveform-and-cross-track-drag-preview.md`
- `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment.md`
- `.planning/plans/2026-05-30-track-panel-context-menu-and-lane-alignment.md`

## Archived Planning Docs（已完成/过时）

- `.planning/plans/2026-05-29-daw-timeline-follow-rendering-architecture.md` — 目标已达成，代码已收敛
- `.planning/plans/2026-05-29-daw-timeline-follow-rendering-architecture-test-verification.md` — 同上

## Next Planning Actions

1. Visual smoke 验证 UI 修复（Time/Cont 按钮 + 钢琴卷帘标尺背景）。
2. 补齐 Undo/Redo 边界测试。
3. 解释 `OpenTuneTests.exe ui` exit=1。
4. Arrangement min-zoom waveform 功能实现。

---

*Roadmap 只保留当前主线与优先级。完整上下文见 `.planning/STATE.md` 与各计划文档。*
*Last updated: 2026-05-29*
