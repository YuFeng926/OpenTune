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
  - VST3 ARA multi-region binding repair: `AudioModification persistentID -> materializationId` binding table in `VST3AraSession`, source/window reuse removed from ARA default birth, ARA archive hooks wired, existing renderable binding display no longer depends on Read Audio arm state
  - Studio One stopped/pause ARA playback gate implemented: realtime stopped blocks clear output before mapping/readback and `processBlock(...)` returns ARA-handled silence (`true`), with plan and verification source under `.planning/plans/2026-05-17-studio-one-ara-stopped-render-gate*.md`
  - Studio One normal track-insert `Read Audio` failure implemented as a runtime-mode split: ARA-capable VST3 instances are ARA-bound only after host `bindToDocumentController*()`, otherwise they are regular VST3 and use `CaptureSession`.
  - 2026-05-18 PianoRoll empty-space seek intent: one mouse gesture = one intent (click seeks, drag edits, tool switch cancels pending) for seek-enabled tools; 2026-05-19 correction keeps LineAnchor main-edit clicks inside the anchor tool and disables playhead seek there
  - 2026-05-18 Regular VST3 capture display selection: completed capture stays visible when host playhead leaves segment; `architecture/processor/core` PASS, ARA/non-ARA builds PASS
  - 2026-05-18 Regular VST3 capture timeline view domain: PianoRoll view defaults to zero so late-capture segments remain scrollable to earlier time
  - 2026-05-18 Regular VST3 transport shortcuts: unified routing helper, no fake host transport truth; architecture guards PASS
- Still open on this line:
  - Validate ARA-bound vs regular VST3 runtime split in Studio One track insert, Studio One ARA workflow, REAPER ARA track FX, Cubase extension workflow, and Live VST3 insert
  - Validate Studio One pause/stop/play behavior with the rebuilt ARA VST3
  - Validate PianoRoll empty-space seek intent L5 manual visual behavior
  - Validate regular VST3 capture display selection (completed capture visibility), timeline view domain (late-capture scroll), and transport shortcuts L5
  - Undo/Redo 边界测试（空栈、redo 裁剪、500 层溢出）
  - CorrectionWorker 取消/覆盖并发语义验证
  - `OpenTuneTests.exe ui` runner exit=1/no `[FAIL]` text needs explanation before claiming full-suite PASS
  - Reaper ARA multi-item/project reload L5 journey remains pending
  - L5 manual journeys 和 macOS bundle inspection 继续 deferred
- Next planning action:
  - 用户确认 2026-05-18 各项 L5 手工旅程
  - 确认构建通过后评估是否需要更多自动化测试
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

- **Studio One ARA stopped gate:** renderer-local realtime stopped-state silence is implemented; non-realtime ARA reads are preserved; Studio One pause/stop/play L5 behavior remains pending.
- **ARA-capable regular VST3 mode:** implemented runtime split so unbound ARA-capable VST3 instances use regular capture instead of reporting missing `DocumentController`; host L5 remains pending.
- **PianoRoll empty-space seek intent (2026-05-18):** `mouseDown` arms pending, `mouseUp` within 12px seeks, drag beyond threshold starts tool editing; `piano-roll-intent` suite PASS; L5 remains pending.
- **Regular VST3 capture display selection (2026-05-18):** completed capture stays visible when playhead leaves segment; architecture guards PASS; host L5 pending.
- **Regular VST3 capture timeline view domain (2026-05-18):** PianoRoll view defaults to zero so late-capture segments remain scrollable; automated test PASS; host L5 pending.
- **Regular VST3 transport shortcuts (2026-05-18):** unified routing helper, no fake host transport truth; architecture guards PASS; host L5 pending.
- **Done (v1.5):** Custom UndoManager + PianoRollEditAction, PianoRollCorrectionWorker async worker, PlayheadOverlayComponent isolation, RenderBadgeComponent, F0Timeline finalized, Line Anchor tool, Vibrato per-note control, Continuous scroll mode, ONNX Runtime memory optimization, GPU/CPU inference backend restructure
- **Open:** ARA-bound/regular VST3 host L5 validation, Undo 边界测试、CorrectionWorker 并发验证、UI suite exit-code investigation、Reaper ARA multi-item L5 validation、2026-05-18 四项 L5
- **Deferred:** L5 manual journeys, macOS bundle inspection, F3/F5 follow-up tasks

---
*Roadmap updated: 2026-05-18 after implementing PianoRoll empty-space seek intent and regular VST3 capture UX refinement*
*Current state: `v1.4` shipped/frozen; `v1.5` is active milestone*
