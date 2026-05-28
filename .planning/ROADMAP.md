# Roadmap: OpenTune 主工作区

## Overview

路线图保留 milestone 级摘要。已发布 milestone 归档到 `.planning/milestones/`。当前 active line 是
**v1.5** PianoRoll Undo/Redo + Async Correction + Playhead Isolation + 累积功能落地。

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

## Current State

**已落地 v1.5 主要功能：**

- Custom UndoManager (cursor-based, 500-deep) + PianoRollEditAction。
- PianoRollCorrectionWorker (async single-slot)。
- PlayheadOverlayComponent（独立透明层）。
- RenderBadgeComponent（浮动渲染状态徽章）。
- F0Timeline 定型。
- PianoRoll 增强：VBlankAttachment 滚动、Continuous scroll、Line Anchor、per-note Vibrato。
- ONNX Runtime 内存优化：F0 用完释放、共享 Env、DisableCpuMemArena。
- GPU/CPU 推理后端重构：删除 DmlRuntimeVerifier、DML1 API。
- VST3 ARA: multi-region binding、Studio One stopped gate、regular VST3 分流、multi-item birth 闭环。
- Standalone: ImportDropTarget、Track color system、Shortcuts Shared、Experimental Gate、Snap Settings、Arrangement cache。
- AUTO(REF): reference-driven pitch + timing alignment 主合同已落地，focused verification 已完成。
- TimeTool identity seed: processor 单入口播种 identity handles，focused verification 已完成。

**仍 open：**

- Undo/Redo 边界测试。
- CorrectionWorker 并发验证。
- `ui` suite exit=1 待解释。
- L5 手工旅程：Standalone/VST3 undo、宿主验证、macOS bundle inspection。
- Arrangement min-zoom waveform + cross-track drag preview（planning 阶段）。

## Next Planning Actions

1. 继续 v1.5 剩余 open 项：Undo/Redo 边界、CorrectionWorker 并发、UI runner。
2. 补齐 L5 手工旅程和 macOS bundle inspection。
3. 决定 v1.5 release boundary。
4. 后续新功能继续沿用“计划文档 + 验证文档 + 单真值 kill list”流程。

---

*Roadmap 大幅压缩已实现细节。完整 context 见 `.planning/STATE.md`、`.planning/PROJECT.md`。*
*Last updated: 2026-05-28*
