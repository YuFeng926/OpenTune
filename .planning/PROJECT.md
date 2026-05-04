# OpenTune 主工作区项目

## What This Is

OpenTune 是 AI 自动调音应用，集成 RMVPE F0 提取与 PC-NSF-HiFiGAN 声码器合成。当前主工作区同时承载 Standalone 与 VST3/ARA 两条产品形态；`OpenTuneAudioProcessor` 已经稳定为组合 `SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession` 的 shared runtime shell，格式专属 editor 继续只承担各自 UI 壳层与交互协调。

在这条 post-v1.3.2 主线上，代码库已经先完成应用偏好与编辑交互两类关键收敛。2026-04-20 曾把主线定义成 `Content/Placement` 两层真相重构；但 2026-04-21 用户进一步澄清原始产品意图后，official planning 已改正为 **`Source + Materialization + Placement` 三层 persisted truth，`Projection` 只是 derived contract**：

- 应用级偏好不再散落在隐藏 owner / mutable singleton 中，而是由 `AppPreferences` 统一承载 shared 与 standalone-only preferences
- PianoRoll 交互不再依赖隐式 scheme manager，而是通过显式 `AudioEditingScheme` 规则支持 `CorrectedF0Primary` 与 `NotesPrimary`
- `Source` 只负责原始来源与 provenance；`Materialization` 才是 notes / corrected-F0 / detected key / editable audio slice 的真正 owner；`Placement` 只负责时间轴摆放；PianoRoll、Standalone playback 与 VST3/ARA 都应回到 `materialization + projection` 协议

## Core Value

**双格式独立编译，零交叉影响**：通过统一代码库支撑 Standalone 与 VST3，但不允许任何一侧为了兼容另一侧而牺牲自身正确架构、生命周期安全和行为稳定性。

## Current State

`v1.4` 已冻结/发布。当前 live tree 进入 **`v1.5 PianoRoll Undo/Redo + Async Correction + Playhead Isolation`** 活跃开发阶段。

截至 2026-05-05 工作区：

- (v1.4 全部已落地内容继续保持)
- **自定义 Undo/Redo 系统**：`UndoManager`（cursor-based，500 层上限）+ `PianoRollEditAction`（notes + correctedSegments 双快照对），Processor 持有，双 Editor 共享
- **PianoRollCorrectionWorker**：异步后台修正工作器，单槽 pending + completed，支持 ApplyNoteRange / AutoTuneGenerate
- **PlayheadOverlayComponent**：从 PianoRoll paint 拆出的独立透明覆盖层子组件
- **RenderBadgeComponent**：浮动渲染状态徽章，双 Editor 持有
- **F0Timeline**：唯一 F0 frame/time 域值对象，已定稿
- **PianoRoll 编辑增强**：VBlankAttachment 滚动、Continuous scroll mode、Line Anchor 工具、Vibrato depth/rate per-note 控制
- **ONNX Runtime 内存优化（2026-05-01）**：F0 模型用完释放（~350-500MB）；共享单个 Ort::Env；Vocoder DisableCpuMemArena；OrtDeviceAllocator 统一
- **GPU/CPU 推理后端重构（2026-05-02）**：删除 DmlRuntimeVerifier 预验证层（512行）；AccelerationDetector 从 DLL 大小/VRAM 阈值启发式改为 ORT GetExecutionProviderApi("DML") 官方 API 查询；DmlVocoder 从 DML2 改为 DML1 API（显式 adapterIndex 绑定）；VocoderFactory DML 失败时 overrideBackend(CPU) 保持状态一致；RMVPEExtractor preflight 统一走系统内存（F0 始终 CPU）；DirectML 1.15.4 vendored 包部署修复

- `AppPreferences` typed schema、持久化与 shared/standalone page composition 已落地
- `AudioEditingScheme`、parameter sync decision、notes-first 初始交互路由已落地
- `NotesPrimary` 下 hand-draw / line-anchor 的 voiced-only 行为已按 scheme 收口；unvoiced frame reject/trim 已进入 `AudioEditingScheme` + `PianoRollToolHandler`
- `showUnvoicedFrames` / `noteNameMode` / `showChunkBoundaries` 已收敛为 shared app preferences，并接入 shared preferences page、两套 editor 同步和 PianoRoll 渲染
- dual-format 仓库下的 mac Standalone bundle metadata / docs 路径已落地到 `OpenTune_Standalone` 边界内，并有 source/build structure smoke guards
- `UndoAction` / `UndoManager` / `OpenTuneAudioProcessor` / `PianoRollComponent` / Standalone+VST3 editor 当前已进一步收敛到 `materializationId + projection` public contract；`MaterializationRefreshRequest`、`setEditedMaterialization(...)`、`reclaimUnreferencedMaterialization()` / `reclaimUnreferencedSource()` 已替换旧 content-era 命名
- VST3 ARA published contract 现在已经显式改成 `AppliedMaterializationProjection` + `bindPlaybackRegionToMaterialization()` / `updatePlaybackRegionMaterializationRevisions()` / `clearPlaybackRegionMaterialization()`；`PublishedRegionView` 公开 `sourceId`，`recordRequested()` 默认为当前 region birth 新 materialization，不再复用 previous workspace materialization
- Standalone `ArrangementViewComponent` 与 `PluginEditor` 当前选择/overlay/main sync 路径已经继续往 `placementIndex + placementId + materializationId` 收口；Piano Roll 当前公开 contract 也已从 `ContentTimelineProjection` / `setEditedContent(...)` 切到 `MaterializationTimelineProjection` / `setEditedMaterialization(...)`
- `OpenTuneTests` 已覆盖 app preferences、scheme 决策、shared visual preferences、mac Standalone packaging、parameter panel sync、undo result-chain owner guard、compound clip-core delta、scheme-independent replay、undo matrix、ARA renderer block span，以及本 phase 新增的 projection/materialization-owner guards（`PianoRollProjection_ConsumesMaterializationIdAndPlacementProjectionOnly`、`SplitPlacement_PianoRollDisplaysProjectedWindowOnly`、`ContentMetadataUndo_SharedContentDoesNotResolvePlacementByContentId`、`EditingCommand_DoesNotMutatePlacement`、`MaterializationCommands_DoNotMutateTimelinePlacementTruth`、`PlacementCommands_DoNotMutateClipCoreTruth`、`AraSession_SnapshotExposesSourceMaterializationAndPlacementOwnership`、`ProcessorModel_RejectsMixedClipOwnerApis`）
- Piano Roll 当前已从 content-era 选择协议升级为显式 `MaterializationTimelineProjection` value object；Standalone placement 与 VST3 preferred region 都会把 `timelineStart/timelineDuration/materializationStart/materializationDuration` 完整喂给同一条 UI contract，split trailing placement / partial region 不再按整段 materialization 窗口显示
- Standalone materialization scale/key undo 回调当前已切回 materialization-owner lookup，不再把 `contentId` 当 placement lookup key；same-source multi-placement 路径已有 focused guard
- `docs/plans/2026-04-20-content-placement-two-truth-refactor*.md` 与 `docs/plans/2026-04-21-content-placement-boundary-repair*.md` 目前只保留 delete-first / boundary-repair 的历史价值；owner-model 前提已被 `docs/plans/2026-04-21-source-materialization-placement-projection-clarification.md` supersede
- 已知 stale deleted-helper 导致的 compile-break baseline 已清掉；`ArrangementViewComponent` / `PianoRollComponent` / `Tests` 当前不再卡在那批已删接口残留上
- Standalone delete / split / merge / ARA unbind 当前 live-tree 语义已收敛为 `placement -> materialization -> source` 生命周期；`reclaimUnreferencedContent()` 两层回收 public contract 已删除
- materialization 当前还显式持有 source provenance window + lineage metadata；split 会 birth 两个 child materialization 并继承 / 重写 provenance，merge 只在 source window 连续且 payload 可无损合并时成立
- 当前自动化验证现实已更新为：`OpenTuneTests.exe processor/ui/architecture` PASS、全量 `OpenTuneTests.exe` PASS、`ctest` PASS、`OpenTune_Standalone` / `OpenTune_VST3` build PASS；这些结果现在覆盖了 materialization-facing public contract、source provenance / lineage 持久化、merge payload preservation、forced ARA source seeding 与 region-local ARA 录音路径，但 L5 手工旅程与 macOS bundle inspection 仍是显式 gap

## Current Mainline Goals

- 持续把 `docs/plans/2026-04-18-*`、`docs/plans/2026-04-19-*`、`docs/plans/2026-04-20-*`、`docs/plans/2026-04-21-*` 与源码已经反映的 live-tree 现实折回 `.planning`，不再让 official planning 落后于源码
- 把 shared runtime 的 owner model 从旧 `Content/Placement` 两层假设，改正为 `Source + Materialization + Placement`；彻底删除 mixed `clipId` 公共协议和“same-source shared editable content”假设
- 把 Standalone playback、PianoRoll、split/delete、以及 VST3 ARA source/region mapping 全部接回 `materialization + projection` 协议；默认 timeline 命令只允许改 placement，editable payload 只允许改 materialization
- 继续守住 `single active workspace`、Standalone explicit placement、async `SafePointer`、app preferences 不进入 processor/project state 这些已完成边界，但不再把 `same-source shared-clip + appliedRegion` 当作目标架构
- 把现有自动化绿灯明确标注为“旧假设下的 partial evidence”，继续把 Standalone / VST3 手工旅程与 macOS bundle inspection 记成显式 gap，而不是假 PASS
- 当前结构范围已冻结为 `v1.4`；下一步是补齐 release gate（L5 manual journeys + macOS bundle inspection）并决定是否发版

<details>
<summary>Archived milestone review: v1.3.2 ARA2 线程模型与快照架构收敛</summary>

## Latest Shipped Milestone: v1.3.2 ARA2 线程模型与快照架构收敛

**Goal:** 按 ARA2/JUCE 官方契约，把 VST3 分支的 ARA 适配层收敛为最小正确线程模型与状态发布结构，并彻底删除旧错误接口和冗余状态。

**Target features:**
- `OpenTuneDocumentController` 收敛为 `sources_ + regions_ + preferredRegion_ + publishedSnapshot_`，读侧只暴露 `loadSnapshot()`
- `OpenTunePlaybackRenderer` 改为只消费 immutable snapshot，播放真相从 `audio source` 单槽映射改成 `playback region` 级真相
- VST3 `PluginEditor` 改为 snapshot epoch consumer，保留 timer，但删除多 getter 拼装真相与 retry 思路
- 删除 `AudioSourceState`、旧 binding/sync/range getter、retry、错误 callback 职责分配，并补一条 `appliedRegion` 回归守护

**Current state:** `v1.3.2` 的结构成果仍保留在 live tree，但 ARA 真相已进一步沉到 `VST3AraSession`，`OpenTuneDocumentController` 当前只是 callback forwarder。`OpenTunePlaybackRenderer` 与 VST3 `PluginEditor` 继续只消费 immutable snapshot / preferred-region truth，sample access 生命周期仍由官方 callback 驱动，`AudioSourceState` 与旧 source-level getter/retry 残留已删除；历史 Phase 23-26 guard 资产现已不保留在 live tree，也不再被视为当前主线必须恢复的 shipped baseline。

</details>

## Requirements

### Validated

- ✓ ONNX 推理引擎（RMVPE F0 提取 + PC-NSF-HiFiGAN 声码器合成）— 现有
- ✓ 调式检测算法（Krumhansl-Schmuckler）— 现有
- ✓ 音频处理流水线（`RenderCache` + `VocoderDomain`）— 现有
- ✓ 钢琴卷帘编辑界面（`PianoRollComponent` + 多工具支持）— 现有
- ✓ Standalone 完整多轨道 UI（MenuBar / TrackPanel / Arrangement / PianoRoll）— 现有
- ✓ VST3 ARA 真相当前由 `VST3AraSession` 维护，播放读取只消费 immutable snapshot / preferred-region truth — `v1.3.2` 结构结果 + current live tree
- ✓ `OpenTuneAudioProcessor` 当前 runtime shell 已拆成 `SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession` 四段真相分工 — post-v1.3.2 live tree
- ✓ 2026-04-21 前的 live tree 中，VST3 `recordRequested()` 曾在同 source 下复用既有 workspace clip 以避免 detached clip churn；该行为现已被降级为历史实现现实，不再视为目标产品语义
- ✓ Standalone `commitPreparedImportAsPlacement()` 已切到显式 `ImportPlacement`，processor 不再隐式决定 placement — post-v1.3.2 live tree
- ✓ Standalone preset chooser 的异步回调已使用 `SafePointer`，不再捕获裸 editor `this` — post-v1.3.2 live tree
- ✓ 应用级偏好当前由 `AppPreferences` 统一承载；shared preferences 与 standalone-only preferences 已完成 schema 分离与持久化 — 2026-04-18 refactor
- ✓ Standalone 与 VST3 当前都使用显式 preferences page composition：Standalone 组装 `Audio + Shared + Standalone-only`，VST3 只组装 `Shared` — 2026-04-18 refactor
- ✓ `AudioEditingScheme` 当前是显式输入规则，不再依赖隐藏 mutable manager；`ParameterPanelSync` 与 AutoTune target 决策已改成吃显式 scheme — 2026-04-18 refactor
- ✓ `NotesPrimary` 与 `CorrectedF0Primary` 两套交互语义已完成第一轮迁移：notes-first 当前会在 hand-draw / line-anchor 后自动选中受影响 notes，corrected-f0-first 继续保留 line-anchor segment 优先语义 — current live tree
- ✓ `NotesPrimary` 下 hand-draw / line-anchor 的 voiced-only 行为当前由 `AudioEditingScheme` 纯规则推导；unvoiced frame reject/trim 已落地，`voicedOnlyEditing` 没有回流成独立 preference — current live tree
- ✓ `showUnvoicedFrames` / `noteNameMode` / `showChunkBoundaries` 当前已进入 `AppPreferences` shared state，并在 Standalone / VST3 editor、shared preferences page、View menu 与 PianoRoll 渲染间保持同步 — current live tree
- ✓ mac Standalone app-only plist metadata、bundle docs 路径与帮助文档 bundle-aware lookup 已落地到 `OpenTune_Standalone` 边界，VST3 target 保持未污染 — current live tree
- ✓ GPU/CPU 推理后端选择由 ORT GetExecutionProviderApi("DML") 判断可用性，DML session 创建失败自动回退 CPU；用户可通过偏好切换强制 CPU — 2026-05-02 重构
- ✓ ONNX Runtime 内存管理：F0 模型按需加载/用完释放；vocoder 常驻但禁用 CPU arena；共享单个 Ort::Env — 2026-05-01 优化

### Active

- [ ] 持续把当前主线的真实结构、测试现状和调试策略同步回 `.planning` 主线，避免 official planning 再次落后 live tree
- [ ] Standalone / VST3 undo result-chain 的手工旅程本轮按用户许可暂缓；official planning 必须显式保留这条未完成验证，而不能把它写成 PASS
- [ ] 当前主线仍是 `post-v1.3.2` 的未编号收敛线；待范围冻结后，需要再决定下一个正式 milestone / release boundary

### Out of Scope

- Melodyne 式多 source 同时编辑 UI 或多工作区并存模型 — 当前产品阶段明确只支持单实例单 active workspace
- 同 source 切换 region 时重新创建 detached clip 再删除 previous clip — 与单工作区语义和稳定 clipId 目标冲突
- 让 `commitPreparedImportAsPlacement()` 在 processor 内隐式决定 `trackId` / `startSeconds` — placement 必须是 Standalone editor 的显式语义
- 在 `FileChooser::launchAsync()` 回调里重新捕获裸 editor `this` — 会把异步生命周期风险带回主线
- 把 app-level preferences 写进 processor state / project serialization — 应用偏好与工程状态必须继续分离
- 重新引入 `AudioEditingSchemeManager`、hidden mutable theme owner、boolean-flag mixed preferences dialog、`createForCurve`、static undo side-channel、并行旧新结构或兜底路径 — 当前主线继续追求唯一正确结构
- 把 `voicedOnlyEditing` 做成独立用户 preference、让 scheme 切换覆盖 visual preferences，或把 mac app-only plist key 直接挂到 shared `juce_add_plugin` — 都属于错误 owner

## Context

### 当前结构现状

当前 live tree 的 shared runtime shell 已稳定为四段真相分工：`SourceStore` 管 source identity 与 provenance，`MaterializationStore` 管 content-local payload（notes / corrected-F0 / detected key / editable audio slice）与 render/read truth，`StandaloneArrangement` 管 Standalone 的 placement / mix / selection truth，`VST3AraSession` 管 ARA source/region/content-binding/preferred-region truth。4 月 18 日之后又补上一层应用级结构：`AppPreferences` 管 app-level shared/standalone preferences，`SharedPreferencePages` 与 `StandalonePreferencePages` 负责明确的 UI 组装边界。4 月 19 日的继续收口已把 scheme-managed voiced-only 行为、shared visual preferences、以及 Standalone-only mac bundle packaging 边界一起折回 live tree。

### 当前主线架构主张

当前主线不再允许“隐藏 owner + UI 直接读全局状态”的老路径回流。编辑 scheme、theme、language、shortcut 等配置都必须通过显式 state carrier 流动；PianoRoll 的交互决策必须由纯规则 helper 接收显式 scheme 输入；processor/project state 与 app-level preferences 必须继续分离。

### 当前编辑融合现状

当前两套 fixed scheme 已完成 owner 正确的产品收口：`NotesPrimary` 在手绘 / line-anchor 成功提交后会自动选中受影响 notes，并且只允许 voiced frame 被编辑；`CorrectedF0Primary` 继续保留 line-anchor segment selection 与 retune-speed 优先语义。`showUnvoicedFrames` / `noteNameMode` / `showChunkBoundaries` 也已经进入 `AppPreferences` shared state，通过 shared preferences page、View menu、Standalone/VST3 editor 同步到 PianoRoll 渲染。当前 undo/redo 也已经完成 processor-owned content/placement-aware affected-range result chain 收口；自动化验证已重新跑通，剩余验证缺口只在手工旅程确认。

## Constraints

- **Brownfield**: 主工作区已存在 Standalone + VST3/ARA 双形态，不能通过推倒重来解决问题
- **UI 隔离**: `Source/Standalone/...` 与 `Source/Plugin/...` 必须继续保持各自 editor 壳层隔离，不允许把一侧 UI 语义泄漏到另一侧
- **Processor 边界**: `Source/PluginProcessor.*` 现有导入/替换/局部重渲染/统一播放读 API 保持可复用，但不允许把 app preference 或格式专属适配层问题继续扩散进 shared core
- **ARA 契约**: model graph edit 只能在 `beginEditing()` / `endEditing()` 生命周期内观察与发布；sample access 生命周期必须通过官方 callback 驱动
- **Playback Truth**: 播放真相单位必须是 `PlaybackRegion`，不能继续用 `AudioSource -> single range` 充当 region mapping 真相
- **Single Workspace**: 当前 VST3 产品语义只允许单实例单 active workspace；新 source 才允许新 workspace，同 source 只能原位刷新既有 clip
- **Explicit Placement**: Standalone import placement 必须在 editor 侧显式决定，再把 `trackId + startSeconds` 传给 processor commit
- **App Preferences**: app-level preferences 必须留在 `AppPreferences`，不得进入 `OpenTuneAudioProcessor` runtime shell 或工程状态序列化
- **Editing Scheme**: `AudioEditingScheme` 负责固定交互方案及其推导出来的 voiced-only 行为；`voicedOnlyEditing` 不得降级为独立可持久化偏好
- **Shared Visual Preferences**: `showUnvoicedFrames` / `noteNameMode` / `showChunkBoundaries` 必须留在 `AppPreferences` shared state，Standalone / VST3 共享，切换 scheme 不得覆盖
- **Undo Ownership**: undo/redo 若要引入 affected-range，必须继续走 content/placement-aware processor/store 提交，不得回流 `createForCurve` 或 static 全局 side-channel
- **Standalone Packaging**: mac app-only plist metadata 与帮助文档路径只能落在 `OpenTune_Standalone`，不得污染 VST3 target
- **Async Lifetime**: chooser / modal / async callback 不允许捕获裸 editor `this`；统一使用 `SafePointer`
- **Audio Thread**: 音频线程只能读 immutable snapshot，不得触碰 `stateMutex_`、长生命周期 host reader 或任何 mutable adapter state
- **Cleanup**: 当前主线必须清除冗余代码和错误结构，不允许残留兼容 getter、retry、并行旧新结构或兜底路径
- **Standalone 守护**: 当前主线不得破坏 Standalone 既有多轨工作流与主开发目标定位
- **线程与生命周期**: 方案不得引入线程不安全、悬空引用、reader 跨线程共享、内存泄漏或 stale publish 风险

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| `OpenTuneAudioProcessor` 继续作为组合 `SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession` 的 runtime shell | source identity、materialization payload、placement、ARA binding 四类真相拥有不同生命周期，不应再混回单体容器 | ✓ Good |
| VST3 当前产品语义固定为单实例单 active workspace | 当前阶段不做多 source 并行编辑，先把 single-source workspace 语义收紧到唯一正确结构 | ✓ Good |
| same-source sibling regions 默认不得共享 editable owner；共享的只能是 source provenance 与 hydration 资产 | 用户已明确要求 region-local 编辑彼此隔离；旧 workspace-clip reuse 只剩历史实现价值，不再是目标产品语义 | ✓ Good |
| Standalone `commitPreparedImportAsPlacement()` 必须接收显式 `ImportPlacement` | placement 是 UI 语义，不应藏在 processor commit 内部猜测 | ✓ Good |
| chooser async callback 必须捕获 `SafePointer` | 裸 editor `this` 会重新引入异步生命周期悬空风险 | ✓ Good |
| 应用级偏好由 `AppPreferences` 统一承载，不进入 processor/project state | app-level preference 与 clip/project truth 生命周期不同；写进 processor state 会污染工程边界 | ✓ Good |
| Shared / Standalone-only preferences page 必须显式组装，不再复用 boolean-flag mixed dialog | dual-format 仓库下 UI 边界必须靠 composition，而不是 runtime flag | ✓ Good |
| `AudioEditingScheme` 必须是显式输入规则，不再依赖隐藏 mutable scheme manager | 交互策略需要可测试、可组合、可跨 editor 明确传递 | ✓ Good |
| `曲线优先编辑` / `音符优先编辑` 固定映射到 `CorrectedF0Primary` / `NotesPrimary`，并由 scheme 纯规则推导 voiced-only 行为 | 交互方案不只是显示名字；它同时决定 parameter target、auto-tune target 与 hand-draw / line-anchor 行为，但不需要额外 `voicedOnlyEditing` 偏好 | ✓ Good |
| `showUnvoicedFrames` / `noteNameMode` / `showChunkBoundaries` 作为 shared app preferences 独立持久化 | 这些是全局视觉偏好，不应被 scheme 切换覆盖，也不属于 processor/project state | ✓ Good |
| undo/redo 若补 affected-range，必须走 content/placement-aware processor/store 结果链，不回流 `createForCurve` / static side-channel | 当前主线已经是 content-local truth owner；旧 curve-bound undo 会绕过提交边界并污染双格式共享结构 | ✓ Good |

## Evolution

This document evolves at phase transitions and milestone boundaries.

**After each phase transition**:
1. Requirements invalidated? → Move to Out of Scope with reason
2. Requirements validated? → Move to Validated with phase reference
3. New requirements emerged? → Add to Active
4. Decisions to log? → Add to Key Decisions
5. `What This Is` still accurate? → Update if drifted

**After each milestone**:
1. Full review of all sections
2. Core Value check — still the right priority?
3. Audit Out of Scope — reasons still valid?
4. Update Context with current state

---
*Last updated: 2026-05-05 after synchronizing .planning docs with live tree — updated codebase memory docs (STRUCTURE/TESTING/ARCHITECTURE/STACK/INTEGRATIONS/CONVENTIONS/CONCERNS), deleted stale VST3Merge.md, corrected test suite count to 6, removed deleted Host/ and ScaleInference references*
