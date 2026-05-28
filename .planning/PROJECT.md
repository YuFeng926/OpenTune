# OpenTune 主工作区项目

## What This Is

OpenTune 是 AI 驱动的人声修音应用。`OpenTuneAudioProcessor` 是 shared runtime shell，组合
`SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession` 四段职责；格式专属 editor
只承担各自 UI 壳层与交互协调。

## Core Value

**双格式独立编译，零交叉影响**：同一代码库支持 Standalone + VST3/ARA，但不允许任一格式为了兼容另一格式而牺牲自身正确架构。

## Current State

`v1.4` 已冻结发布。当前 live tree 处于 **v1.5 PianoRoll Undo/Redo + Async Correction + Playhead Isolation + 累积功能落地** 阶段。

已落地核心结构：

- **三仓真值线**：`Source`（原始音频来源）-> `Materialization`（notes/F0/correctedSegments/timeGrid/render）-> `Placement`（时间轴摆放）；`Projection` 只做 derived contract。
- **AppPreferences**：typed schema 统一承载 shared/standalone preferences，Standalone/VST3 各自显式 page composition。
- **AudioEditingScheme**：显式输入规则（CorrectedF0Primary / NotesPrimary），voiced-only 行为由 scheme 推导。
- **UndoManager + PianoRollEditAction**：processor 持有，双 editor 共享，500 层 cursor-based。
- **PianoRollCorrectionWorker**：异步后台修正（单槽 + 版本号取消）。
- **PlayheadOverlayComponent**：独立透明覆盖层。
- **RenderBadgeComponent**：浮动渲染状态徽章。
- **F0Timeline**：唯一 F0 frame/time 域值对象。
- **VST3 ARA**：多 region binding、Studio One stopped render gate、ARA-capable regular VST3 运行时分流、multi-item birth 自动化闭环。
- **Standalone**：ImportDropTarget、Track-level color system、Shortcuts Shared、Experimental Features Gate、Snap Settings。
- **AUTO(REF)**：`ClipA -> ClipB` reference-driven pitch + timing alignment 主合同已落地，focused verification 已完成。
- **TimeTool identity seed**：首次进入 TimeTool 只通过 processor 单入口复用 `ReferenceTimingFeatures` 播种 identity handles，focused verification 已完成。
- **ONNX Runtime 内存优化**：F0 模型用完释放、共享 `Ort::Env`、DisableCpuMemArena。
- **GPU/CPU 推理后端重构**：删除 DmlRuntimeVerifier，AccelerationDetector 由 ORT API 查询，DmlVocoder DML1 API。

Roadmap 与 STATE 详见 `.planning/ROADMAP.md`、`.planning/STATE.md`。

## Current Mainline Goals

- 继续完成 v1.5 剩余验证：Undo 边界测试、CorrectionWorker 并发验证、UI suite exit-code 修复。
- 继续补齐 L5 手工旅程：Standalone/VST3 undo、宿主 DAW 行为验证、macOS bundle inspection。
- 后续功能规划保持单真值路线，新增实验能力必须先写计划和验证文档。

## Requirements

### Validated（完整列表见 REQUIREMENTS.md）

- ONNX 推理引擎（RMVPE + PC-NSF-HiFiGAN）- 现有。
- 钢琴卷帘编辑（多工具）- 现有。
- Standalone 完整多轨 UI - 现有。
- VST3 ARA 集成 - 现有（VST3AraSession + immutable snapshot）。
- 三仓真值线 - 已落地。
- AppPreferences/AudioEditingScheme - 已落地。
- Undo/Redo + CorrectionWorker - 已落地。
- ONNX 内存优化 + GPU/CPU 重构 - 已落地。
- VST3 ARA multi-region/multi-item/birth lifecycle - 已落地闭环。
- AUTO(REF) shared-core contract - 已落地并通过 focused verification。
- TimeTool identity seed - 已落地并通过 focused verification。

### Active

- [ ] Undo/Redo 边界测试（空栈、redo 裁剪、500 层溢出）。
- [ ] CorrectionWorker 取消/覆盖并发语义验证。
- [ ] `OpenTuneTests.exe ui` runner exit=1 解释修复。
- [ ] L5 手工旅程（Standalone/VST3 undo、宿主验证）。
- [ ] macOS bundle inspection。

### Out of Scope

- Melodyne 式多 source 同时编辑 UI。
- 同 source 切换 region 时 detached clip churn。
- processor 隐式决定 placement（必须 editor 显式 `ImportPlacement`）。
- app-level preferences 进入 processor/project state。
- `voicedOnlyEditing` 作为独立 preference（由 scheme 推导）。
- 旧分析字段作为 project truth。
- 兼容 getter、retry、并行新旧结构、兜底路径。

## Context

当前 shared runtime shell 已稳定为四段职责分工：`SourceStore`（source identity）、`MaterializationStore`
（editable payload/render）、`StandaloneArrangement`（placement/mix）、`VST3AraSession`（ARA binding）。
App-level 结构由 `AppPreferences` 统一承载；PianoRoll 交互决策由 `AudioEditingScheme` 与纯规则 helper 驱动。

当前主线不再允许“隐藏 owner + UI 直接读全局状态”的旧路径回流。

## Constraints

- **Brownfield**: 不能推倒重来。
- **UI 隔离**: `Source/Standalone/` 与 `Source/Plugin/` editor 隔离。
- **Processor 边界**: 不把 app preference 或格式专属适配层扩散进 shared core。
- **ARA 契约**: model graph edit 生命周期受限，sample access 通过官方 callback 驱动。
- **Playback Truth**: 单位必须是 PlaybackRegion。
- **Single Workspace**: 单实例单 active workspace。
- **Explicit Placement**: Standalone import 必须 editor 显式决定。
- **App Preferences**: 不进入 processor/project state。
- **Editing Scheme**: `voicedOnlyEditing` 不得降级为独立 preference。
- **Shared Visual Prefs**: `showUnvoicedFrames/noteNameMode/showChunkBoundaries` 留在 shared state。
- **Undo Ownership**: 走 processor/store 提交链，不进 createForCurve/static side-channel。
- **Standalone Packaging**: mac plist/docs 只落地 OpenTune_Standalone。
- **Async Lifetime**: chooser/modal 用 SafePointer。
- **Audio Thread**: 只读 immutable snapshot。
- **Cleanup**: 清除冗余代码，不残留兼容 getter/retry/并行旧结构/兜底路径。

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| OpenTuneAudioProcessor 作为 SourceStore+MaterializationStore+StandaloneArrangement+VST3AraSession 的 runtime shell | 四类真值生命周期不同 | Good |
| ARA editable owner 按 AudioModification persistent ID | 官方 ARA 模型正解 | Good |
| Standalone commitPreparedImportAsPlacement() 必须接收 ImportPlacement | placement 是 UI 语义 | Good |
| 应用级偏好由 AppPreferences 统一承载 | app 与 project 生命周期不同 | Good |
| AudioEditingScheme 是显式输入规则 | 交互策略可测试可组合 | Good |
| CorrectedF0Primary/NotesPrimary 固定映射 | 两套交互方案足够 | Good |
| shared visual prefs 独立持久化 | 不被 scheme 切换覆盖 | Good |
| undo/redo 走 content/placement-aware processor/store 结果链 | 当前主线已收敛 | Good |
| AUTO(REF) 走 binding truth -> feature cache -> pure align patch -> TimeGrid compiler -> atomic commit | reference alignment 只有一条正式路线 | Good |

---

*Last updated: 2026-05-28 - AUTO Ref 文档 kill list 已完成，主合同已落地，focused verification 已完成。*
