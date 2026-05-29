# Roadmap: OpenTune 主工作区

## Overview

路线图只保留 milestone 级摘要。当前活跃主线仍是 **v1.5**，但 2026-05-29 起，
时间线相关工作的优先级被重新定性为：

**DAW timeline follow rendering architecture 重做**

这不是 `cont` 跟随参数微调，而是一次时间线渲染内核收敛：把播放时间、走带呈现、
内容绘制三件事拆开，后续实现统一向 `presentation clock + viewport policy + prepared content + overlay`
收口。

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

### v1.5 当前最高优先级收口

- `DAW timeline follow rendering architecture`
  - 先反转 focused tests / kill-list 契约
  - 再做 live implementation
  - 明确禁止把 steady scroll 继续做成 full repaint / full rebuild 路径

## Open Work

- DAW timeline follow rendering architecture 实装
- Undo/Redo 边界测试
- CorrectionWorker 并发验证
- `ui` suite exit=1 待解释
- L5 手工旅程：Standalone/VST3 undo、宿主验证、macOS bundle inspection
- Arrangement min-zoom waveform + cross-track drag preview

## Active Planning Docs

- `.planning/plans/2026-05-29-daw-timeline-follow-rendering-architecture.md`
- `.planning/plans/2026-05-29-daw-timeline-follow-rendering-architecture-test-verification.md`

## Next Planning Actions

1. 按 2026-05-29 timeline 架构合同开始 live code 重构，不再接受 cont 参数微调作为主方向。
2. 优先删除 steady scroll 的 full-content invalidation / render-model rebuild 旧契约。
3. 把 Continuous follow 改为 pinned playhead + viewport presentation policy。
4. 之后再补 focused build/test、runtime diagnostics、visual smoke、L5 旅程。

---

*Roadmap 只保留当前主线与优先级。完整上下文见 `.planning/STATE.md` 与各计划文档。*
*Last updated: 2026-05-29*
