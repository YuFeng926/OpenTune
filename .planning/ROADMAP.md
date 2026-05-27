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
  - 2026-05-27 VST3 ARA multi-item birth + editor reopen structural fix: persistentId-owned pending birth, stale worker-result drop, editor destructive clear removal, metadata-only ARA pre-bind restore caching, architecture/processor/core/memory PASS, ARA VST3/non-ARA VST3/Standalone builds PASS
  - 2026-05-27 Cumulative features:
    - Track-level color system: `Placement::colour` → `TrackState::colour`, `TrackColorMode` (919c544)
    - ImportDropTarget with hover preview: 几何拖放定位 + blank-area create-track (8ea7305 / 706c844)
    - Shortcuts promotion from Standalone-only to Shared: 22 entries + tool-switching (7b9945d)
    - Experimental features gate: `experimentalFeaturesEnabled` boolean (706c844)
    - Snap settings: `SnapSettings` + preference page (8ea7305)
    - ARA revision-based PendingBirth lifecycle: persistentId + SourceWindow + revision (c4766c5)
    - Arrangement render model cache: waveform tile caching + drag preview + vertical geometry caching (c77d847 / c03fabe)
- Still open on this line:
  - Validate ARA-bound vs regular VST3 runtime split in Studio One track insert, Studio One ARA workflow, REAPER ARA track FX, Cubase extension workflow, and Live VST3 insert
  - Validate Studio One pause/stop/play behavior with the rebuilt ARA VST3
  - Validate PianoRoll empty-space seek intent L5 manual visual behavior
  - Validate regular VST3 capture display selection (completed capture visibility), timeline view domain (late-capture scroll), and transport shortcuts L5
  - DAW timeline rendering pipeline refactor landed: prepared render models, exposed-strip scroll invalidation, and timeline focused suites PASS; runtime perf / L5 smoke still pending if needed
  - Undo/Redo 边界测试（空栈、redo 裁剪、500 层溢出）
  - CorrectionWorker 取消/覆盖并发语义验证
  - `OpenTuneTests.exe ui` runner exit=1/no `[FAIL]` text needs explanation before claiming full-suite PASS
  - Reaper ARA multi-item/project reload L5 journey is not required from Codex for the 2026-05-27 closure; do not report it as PASS unless manually executed later
  - L5 manual journeys 和 macOS bundle inspection 继续 deferred
- Next planning action:
  - 先按 2026-05-27 新规划重写 `AUTO(REF)` 正式 shared-core 合同，收口为 `ClipA -> ClipB` 参考驱动音高+节奏对轨
  - 把 TimeTool identity seed 收紧为“只共享 timing-feature extraction”的辅助路径，不再扩展旧 experimental seed backbone
  - 再继续 v1.5 其余验证与编辑器侧 open 项

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
- **Done (2026-05-27):** VST3 ARA multi-item birth + editor reopen structural fix automated closure: persistentId 单槽 birth、stale result pre-commit drop、editor 只读、metadata-only ARA pre-bind restore cache/replay 全部落地
- **Done (2026-05-27 cumulative):**
  - Track-level color system: `Placement::colour` → `TrackState::colour`, `TrackColorMode` 枚举 — 919c544
  - ImportDropTarget with hover preview: 几何拖放定位 + blank-area create-track — 8ea7305 / 706c844
  - Shortcuts migration from Standalone-only to Shared (22 entries + tool-switching) — 7b9945d
  - Experimental features gate: `experimentalFeaturesEnabled` boolean 开关 + `ExperimentalReferenceAlignMode` 解耦 — 706c844
  - Snap settings: `SnapSettings` + preference page — 8ea7305
  - ARA revision-based PendingBirth lifecycle: persistentId + SourceWindow + revision — c4766c5
  - Arrangement render model cache: waveform tile caching + drag preview + vertical geometry caching — c77d847 / c03fabe
- **Open:** ARA-bound/regular VST3 host L5 validation、Undo 边界测试、CorrectionWorker 并发验证、UI suite exit-code investigation、2026-05-18 四项 L5、Studio One long-audio ARA L5
- **Deferred:** L5 manual journeys, macOS bundle inspection, F3/F5 follow-up tasks

---
## 2026-05-27 Roadmap Closure: Multi-Item / Reopen 自动化闭环

The 2026-05-26 audit was correct at the time: OriginalF0 convergence alone did not finish the Reaper multi-item / editor reopen lifecycle repair. The follow-up structural fix is now implemented and automatically verified.

Already landed:

1. **SourceStore metadata-only**: `createSource` 支持 `audioBuffer==nullptr`
2. **ARA birth 结果语义收紧**: Failed 显式写入；`releaseImmediately()` 始终调用
3. **Worker 命名收口**: `hydration*` → `birth*` in VST3AraSession
4. **Editor 残留删除**: `rmvpeOverlayLatched_` 删除；ARA path 精简
5. **契约注释刷新**: `requestMaterializationRefresh` 改为正向非 ARA 契约
6. **AraFinal 终审守卫**: 7 个新增守卫 PASS
7. **setStateInformation` null-buffer 子项修复**

Now closed in current code:

1. source 级 pending 压扁已删除；birth owner 是 `AudioModification persistentId + SourceWindow + revision`
2. attach-only/new persistentId 与 ready-source requeue 都会 upsert pending birth
3. editor destructive clear 路径已删除，`clearPlaybackRegionMaterialization()` 不再是 production session API
4. metadata-only ARA pre-bind state 只缓存，`didBindToARA()` 后 replay 到最终 shared stores；regular unbound VST3 state 仍立即恢复

Verification:

- `OpenTuneTests.exe architecture`, `processor`, `core`, `memory` PASS
- ARA VST3, non-ARA VST3, Standalone Release builds PASS
- REAPER manual testing was explicitly not required from Codex for this closure; it must not be reported as PASS unless executed later

---
## 2026-05-27 Roadmap Addendum: Arrangement Visual Contract Plan

The next Standalone Arrangement UI task is planned but not implemented:

- minimum horizontal zoom must still show a waveform/envelope for clips with drawable audio;
- cross-track move drag must show a target-track preview before release;
- real placement truth must still commit only on `mouseUp()`;
- focused proof gate is `timeline-rendering`, not the broad `ui` runner.

Plan source:

- `.planning/plans/2026-05-27-arrangement-min-zoom-waveform-and-cross-track-drag-preview.md`
- `.planning/plans/2026-05-27-arrangement-min-zoom-waveform-and-cross-track-drag-preview-test-verification.md`

---
*Roadmap updated: 2026-05-27 after cumulative feature landing: track color system, import drop UX, shortcuts migration, experimental gate, snap settings, ARA pending-birth revision, arrangement cache refactoring*
*Current state: `v1.4` shipped/frozen; `v1.5` is active milestone*
---
## 2026-05-27 Roadmap Addendum: Experimental Features Gate And TimeTool Anchor Seed (Gate Implemented, Seed Planned)

The next Standalone editor-facing task is planned but not implemented:

1. add a real experimental-features visibility gate below the vocoder model selector, with warning text that reference-track and stretch tooling are incomplete and may still contain bugs;
2. show TimeTool in Standalone only when that gate is enabled;
3. show Arrangement clip reference-source button/menu only when that gate is enabled;
4. restore the lost first-entry stretch-anchor preparation path by making the first entry into TimeTool for a clip trigger one processor-owned anchor-seed flow;
5. that seed flow must create identity internal handles only, not automatic stretch output.

Plan source:

- `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed.md`
- `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed-test-verification.md`

This plan is intentionally contract-first:

1. experimental-feature visibility must be a separate boolean from AUTO Ref mode;
2. UI may only expose or trigger the flow, not own anchor generation;
3. processor/store remain the only place allowed to publish seeded `TimeGridSnapshot` truth.

Status: **入口门控已实现**（706c844）；TimeTool identity anchor seed 仍为后续计划，但按新的 `AUTO(REF)` 合同它只允许共享 timing-feature extraction，不再充当 reference alignment backbone。

---
## 2026-05-27 Roadmap Addendum: AUTO(REF) Reference-Driven Pitch And Timing Alignment Replan

The next shared-core task is not to extend the old experimental seed path. It is to rewrite
`AUTO(REF)` as a formal `ClipA -> ClipB` reference-driven pitch and timing alignment contract.

Roadmap-level corrections:

1. formal output is target-only mutation: `ClipA.notesAfter`, `ClipA.correctedSegmentsAfter`, and `ClipA.timeGridAfter`;
2. timing alignment must depend on `ReferenceTimingFeatures + EffectiveTimeMap`, not on pre-seeded `TimeGridSnapshot` handles;
3. project persistence must converge to `referencePlacementId + notes + correctedSegments + timeGrid`, while `basicAnalysis`, `enhancedAnalysis`, and `analysisMode` exit long-term product truth.
4. `ClipA.timeGridAfter` is not a reference-only side state; it is the normal editable `TimeTool` truth produced by constrained auto handle drag.
5. the auto timing patch must respect a hard local speed window of `0.8x~1.3x`; when exact reference timing would exceed that envelope, the system must keep only the nearest feasible limited alignment.
6. the speed window is scoped to `AUTO(REF)` auto patch semantics only; manual `TimeTool` dragging is not implicitly redefined by this roadmap item.

Plan source:

- `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment.md`
- `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment-test-verification.md`

Status: planned; this is a contract rewrite and roadmap realignment only. Implementation has not started.

---
## 2026-05-27 Roadmap Addendum: Standalone Import Track-Target Drop UX Plan

The next Standalone Arrangement/editor UX task is planned but not implemented:

1. drag-drop onto an existing track should import into that track;
2. drag-drop into Arrangement blank space below visible tracks should create one new visible track and import there;
3. drop outside Arrangement should keep a deterministic active-track fallback;
4. single-file chooser import should remain popup-free;
5. explicit `ImportPlacement` stays editor-owned and must not be inferred inside the processor.

Plan source:

- `.planning/plans/2026-05-27-standalone-import-track-target-drop-ux.md`
- `.planning/plans/2026-05-27-standalone-import-track-target-drop-ux-test-verification.md`

This plan is intentionally contract-first:

1. spatial drop intent belongs to Standalone UI geometry, not shared-core placement commit;
2. blank-area drop may grow visible track count, but only at accepted drop time;
3. hover/preview state must remain transient UI-only data;
4. focused proof gate is `arrangement-contract` / `timeline-rendering`, not the broad `ui` runner.

Status: implemented (8ea7305 / 706c844).
