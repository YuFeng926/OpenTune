# Milestones

## Active Mainline: v1.5 PianoRoll Undo/Redo + Async Correction + Playhead Isolation

- **Name:** PianoRoll 编辑体验增强
- **Status:** Active
- **Landed on this line:**
  - Custom `UndoManager` (cursor-based, 500-deep) + `PianoRollEditAction` (notes+segments snapshot pair), processor-owned
  - `PianoRollCorrectionWorker` async single-slot worker (ApplyNoteRange + AutoTuneGenerate)
  - `PlayheadOverlayComponent` extracted from PianoRoll paint as independent overlay child
  - `RenderBadgeComponent` floating status badge for render state display
  - `F0Timeline` utility finalized as sole frame/time domain object
  - PianoRoll enhancements: VBlankAttachment scroll, Continuous scroll mode, Line Anchor tool, Vibrato depth/rate per-note control
  - Standalone/VST3 Editors both expose `undoRequested()`/`redoRequested()` through processor
- **Still open:**
  - Undo/Redo 边界测试（空栈、redo 裁剪、500 层溢出）
  - CorrectionWorker 取消/覆盖并发语义验证
- **Reference docs:** `.planning/STATE.md`, `.planning/ROADMAP.md`

---

## v1.4 Source/Materialization/Placement persisted truth (Shipped/Frozen: 2026-04-25)

- **Phase range:** post-v1.3.2 structure docs + Task 9-12 execution + ARA repair + F0Timeline
- **Timeline:** 2026-04-17 -> 2026-04-25
- **Archives:** `.planning/milestones/v1.4-ROADMAP.md`

**Key accomplishments:**

- `Source + Materialization + Placement` 三层 persisted truth 完全落地，`Projection` 降为 derived contract
- `MaterializationStore` 持有 lineage state machine（`isRetired_` + 双 surface），sweep invariant 由 message-thread async update 执行
- `PlacementSplitAction` 持 `SplitOutcome`，undo 走 retire/revive
- Standalone delete/split/merge/ARA unbind 路径 reclaim `materialization -> source`
- VST3 ARA 4-task 结构修复：BindingState 枚举、region materialization birth 下沉、transport coordinator、renderer renderability 显式合约
- OriginalF0 单次 RMVPE 提取修复（从分段拼接改为完整 materialization-local mono audio）
- `F0Timeline` 帧域收口，删除旧 floor/ceil/drawEnd helpers
- VST3 transport 三段职责明确（host snapshot truth + pending command + UI presentation）
- 14 个 ARA 守护测试 + 53 个 smoke suites PASS

---

## v1.3.2 ARA2 线程模型与快照架构收敛 (Shipped: 2026-04-16)

- **Phase range:** 23-26
- **Plans completed:** 13
- **Tasks completed:** 30
- **Related commits:** 40
- **Timeline:** 2026-04-16 11:14 +0800 -> 2026-04-16 18:18 +0800
- **Archives:** `.planning/milestones/v1.3.2-ROADMAP.md`, `.planning/milestones/v1.3.2-REQUIREMENTS.md`, `.planning/milestones/v1.3.2-AUDIT.md`

**Key accomplishments:**

- `v1.3.2` 交付了 `mutable model + immutable published snapshot` 这条结构主线；当前 live tree 又进一步把这套真相沉到 `VST3AraSession`，但 region-level publication 语义仍然保留。
- sample access、content dirty、snapshot publish 和 stale-truth purge 全部回到了 callback-driven 生命周期，不再依赖 eager read 或 retry 猜测。
- `OpenTunePlaybackRenderer` 与 VST3 `PluginEditor` 现在都只消费 snapshot / epoch / preferred-region truth，旧 getter 拼装模型已经退出读侧。
- `AudioSourceState`、source-level helper、binding/retry 残留都已删除，shared `OpenTuneAudioProcessor` 边界和 Standalone UI 隔离没有被扩张或污染。
- `v1.3.2` 发版时曾围绕 `SNAP_* / LIFE_* / CONS_* / CLEAN_*` 建立过一套 phase guard 叙事；这些资产现已不保留在 live tree，且因信号质量有限，不再被视为当前主线必须恢复的 baseline。

---

*Milestones updated: 2026-04-19 after landing the undo result-chain implementation and preserving the remaining manual-verification gaps as explicit open items*
