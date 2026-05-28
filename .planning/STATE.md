---
gsd_state_version: 1.0
milestone: v1.5
milestone_name: PianoRoll Undo/Redo + Async Correction + Playhead Isolation
status: active
last_updated: "2026-05-28"
---

# Project State

## Project Reference

**Core value:** 双格式独立编译，零交叉影响
**Current focus:** v1.5 编辑体验增强 + 累积功能落地
**Test Strategy:** OpenTuneTests 轻量 smoke suites + manual DAW journeys + AppLogger/targeted trace

## Current Position

Milestone: v1.5 - 基础架构已落地，功能集成中。

已落地主线：UndoManager+PianoRollEditAction、PianoRollCorrectionWorker、PlayheadOverlayComponent、
RenderBadgeComponent、F0Timeline、ONNX 内存优化、GPU/CPU 重构、VST3 ARA lifecycle 闭环、
Standalone 累积功能（ImportDropTarget、color system、shortcuts、experimental gate、snap、arrangement cache）、
AUTO(REF) reference-driven pitch + timing alignment、TimeTool identity seed。

当前 open：Undo 边界测试、CorrectionWorker 并发验证、`ui` suite exit=1 解释、L5 手工旅程、
Arrangement min-zoom/drag-preview、macOS bundle inspection。

## 关键决策（已实现）

- VST3 ARA 读侧：mutable model + immutable snapshot + callback-driven sample access + region-level playback truth。
- OpenTuneAudioProcessor = SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession 组合。
- VST3 单实例单 active workspace。
- Standalone import 必须显式 ImportPlacement。
- AppPreferences 独立于 processor/project state。
- AudioEditingScheme 显式规则 + scheme 推导 voiced-only。
- Shared visual prefs（showUnvoicedFrames/noteNameMode/showChunkBoundaries）独立持久化。
- Undo/Redo processor-owned result chain，不走 createForCurve/static side-channel。
- ARA editable owner 按 AudioModification persistentID 绑定，非 source/window 或 PlaybackRegion 指针。
- ARA birth pending truth 由 audioModificationPersistentId + SourceWindow + revision 持有。
- experimentalFeaturesEnabled 是独立 boolean，不兼任 AUTO Ref 算法模式。
- AUTO(REF) 走 `ReferenceFeatureSet + EffectiveTimeMap + TimeGridPatchBuilder` 单路线。
- TimeTool identity seed 只复用 `ReferenceTimingFeatures`，由 processor 单入口提交 identity `TimeGridSnapshot`。

## Pending Todos

- [ ] Undo/Redo 边界测试（空栈、redo 裁剪、500 层溢出）。
- [ ] CorrectionWorker 取消/覆盖验证。
- [ ] `OpenTuneTests.exe ui` exit=1 解释修复。
- [ ] Standalone/VST3 undo L5 手工旅程。
- [ ] REAPER/Studio One/Cubase/Live L5 宿主验证。
- [ ] macOS bundle inspection。
- [ ] Arrangement min-zoom waveform + cross-track drag preview。

## Completed Verification Notes

- AUTO(REF) 主合同 focused verification 已完成：architecture、integration、failure、reference-features cache、reference analysis service 均通过。
- TimeTool identity seed focused verification 已完成：processor-owned entry、identity handles、re-entry no-overwrite、experimental UI gate 均已纳入当前收口文档。
- 当前 “完成” 只覆盖 AUTO Ref / TimeTool seed 主合同，不代表 v1.5 全仓所有 open 项已完成。

## Blockers/Concerns

当前无硬阻塞。v1.5 manual verification gaps 已降级为非阻塞 deferred items。

---

*本文件只保留当前决策摘要和待办。完整历史见 `.planning/plans/` 中各执行计划的归档版本。*
