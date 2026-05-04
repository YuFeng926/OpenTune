# Roadmap: OpenTune 主工作区

## Overview

路线图现在保留 milestone 级摘要，同时承认 `v1.3.2` 发版后的 active line 已经不再只是“单工作区结构同步”或“偏好/交互收敛”。2026-04-20 曾把 live tree 的继续收敛写成 `Content/Placement` 两层 persisted truth；但 2026-04-21 用户进一步澄清后，official roadmap 已改正为：在现有 `SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession` runtime 基线上，继续把 owner model 推向 **`Source + Materialization + Placement` persisted truth，`Projection` 只做 derived contract**。

已发版 milestone 的详细 phase 设计、执行记录和验证工件继续归档到 `.planning/milestones/`；2026-04-17 之后的 active mainline 结构与验证细化文档主要来自：

- `docs/plans/2026-04-17-single-source-workspace-{design,tasks}.md`
- `docs/plans/2026-04-18-app-preferences-refactor.md`
- `docs/plans/2026-04-18-app-preferences-refactor-test-verification.md`
- `docs/plans/2026-04-19-interaction-scheme-visual-preferences.md`
- `docs/plans/2026-04-19-interaction-scheme-visual-preferences-test-verification.md`
- `docs/plans/2026-04-19-mac-standalone-bundle-migration.md`
- `docs/plans/2026-04-19-mac-standalone-bundle-test-verification.md`
- `docs/plans/2026-04-19-undo-affected-range-migration.md`
- `docs/plans/2026-04-19-undo-affected-range-test-verification.md`
- `docs/plans/2026-04-20-content-placement-two-truth-refactor.md`
- `docs/plans/2026-04-20-content-placement-two-truth-refactor-test-verification.md`

## Milestones

- ✅ **v1.0 合并基础** - Phases 1-6
- ✅ **v1.1 统一并重构播放读路径（核心）** - Phases 7-10
- ✅ **v1.2 VST3 clip 渲染链路架构收敛** - Phases 11-14
- ✅ **v1.3 样本域边界收敛** - Phases 15-18
- ✅ **v1.3.1 PianoRoll 刷新体系单环收敛** - Phases 19-22 (shipped 2026-04-15)
- ✅ **v1.3.2 ARA2 线程模型与快照架构收敛** - Phases 23-26 (shipped 2026-04-16, archive: `.planning/milestones/v1.3.2-ROADMAP.md`)
- ▶ **v1.4 Source/Materialization/Placement persisted truth** - shipped/frozen (archive: `.planning/milestones/v1.4-ROADMAP.md`)
- ▶ **v1.5 PianoRoll Undo/Redo + Async Correction + Playhead Isolation** - current active milestone

## Current State

- Latest shipped version: `v1.4`
- Current active milestone: `v1.5` PianoRoll Undo/Redo + Async Correction + Playhead Isolation
- Landed on this line already:
  - (全部 v1.4 已落地内容)
  - Custom `UndoManager` (cursor-based, 500-deep) + `PianoRollEditAction` (notes+segments snapshot pair) owned by processor
  - `PianoRollCorrectionWorker` async single-slot worker (ApplyNoteRange + AutoTuneGenerate)
  - `PlayheadOverlayComponent` extracted from PianoRoll paint as independent overlay child
  - `RenderBadgeComponent` floating status badge for render state display
  - `F0Timeline` utility finalized as sole frame/time domain object
  - PianoRoll enhancements: VBlankAttachment scroll, Continuous scroll mode, Line Anchor tool, Vibrato depth/rate per-note control
  - Both Standalone/VST3 Editors expose `undoRequested()`/`redoRequested()` callbacks through processor
  - ONNX Runtime memory optimization: F0 model release-after-use, shared Ort::Env, DisableCpuMemArena (~500-900MB saving)
  - GPU/CPU inference backend restructure: deleted DmlRuntimeVerifier, AccelerationDetector simplified to ORT API probe, DmlVocoder DML2→DML1, VocoderFactory overrideBackend, RMVPEExtractor unified CPU preflight, DirectML 1.15.4 vendored deployment fix
- Still open on this line:
  - Undo/Redo 边界测试（空栈、redo 裁剪、500 层溢出）
  - CorrectionWorker 取消/覆盖并发语义验证
  - L5 manual journeys 和 macOS bundle inspection 继续 deferred
- Next planning action:
  - 确认构建通过后补充自动化测试
  - 评估 v1.5 是否需要更多编辑工具集成

## Progress

| Milestone | Phase Range | Plans | Status | Archive |
|-----------|-------------|-------|--------|---------|
| v1.0 | 1-6 | archived | Complete | - |
| v1.1 | 7-10 | archived | Complete | - |
| v1.2 | 11-14 | archived | Complete | - |
| v1.3 | 15-18 | archived | Complete | - |
| v1.3.1 | 19-22 | archived | Complete | - |
| v1.3.2 | 23-26 | 13/13 | Shipped | `.planning/milestones/v1.3.2-ROADMAP.md` |
| v1.4 | frozen | Task 9-12 + ARA repair + F0Timeline | Shipped/Frozen | `.planning/milestones/v1.4-ROADMAP.md` |
| v1.5 | active | Undo/Redo + CorrectionWorker + Playhead + RenderBadge | In Progress | - |

## Active Convergence Scope

- **Done (v1.5):** Custom UndoManager + PianoRollEditAction, PianoRollCorrectionWorker async worker, PlayheadOverlayComponent isolation, RenderBadgeComponent, F0Timeline finalized, Line Anchor tool, Vibrato per-note control, Continuous scroll mode, ONNX Runtime memory optimization, GPU/CPU inference backend restructure
- **Open:** 三目标构建验证、Undo 边界测试、CorrectionWorker 并发验证
- **Deferred:** L5 manual journeys, macOS bundle inspection, F3/F5 follow-up tasks

---
*Roadmap updated: 2026-05-05 after .planning docs synchronization with live tree*
*Current state: `v1.4` shipped/frozen; `v1.5` is active milestone*
