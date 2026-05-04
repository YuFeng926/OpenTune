---
gsd_state_version: 1.0
milestone: v2.4
milestone_name: PianoRoll Undo/Redo + Async Correction + Playhead Isolation
status: active
stopped_at: v2.4 活跃开发 + .planning 文档同步完成
last_updated: "2026-05-05"
last_activity: 2026-05-05 -- Synchronized .planning docs with live tree. Updated all codebase memory docs (STRUCTURE/TESTING/ARCHITECTURE/STACK/INTEGRATIONS/CONVENTIONS/CONCERNS) to reflect current source state. Deleted stale VST3Merge.md. Corrected test suite count from 4 to 6 (added undo, memory). Removed deleted Host/ directory and ScaleInference references. Updated shipped milestone to v2.3.
progress:
  total_phases: 0
  completed_phases: 0
  total_plans: 0
  completed_plans: 0
  percent: 0
---

# Project State

## Project Reference

See: `.planning/PROJECT.md` and `.planning/REQUIREMENTS.md` (updated 2026-05-05)

**Core value:** 双格式独立编译，零交叉影响
**Current focus:** `v2.4` PianoRoll 编辑体验增强 — 自定义 Undo/Redo、异步修正工作器、播放头独立组件、渲染状态徽章
**Test Strategy:** `OpenTuneTests` 轻量 smoke suites + manual DAW journeys + `AppLogger` / targeted trace

## Current Position

Milestone: `v2.4` — PianoRoll Undo/Redo + Async Correction + Playhead Isolation
Phase: 基础架构已落地，功能集成中
Plan sources:
- (承继 v2.3 全部 plan sources)
- 2026-04-30 自定义 UndoManager + PianoRollEditAction
- 2026-04-30 PianoRollCorrectionWorker 异步修正
- 2026-04-30 PlayheadOverlayComponent 独立化
- 2026-04-30 RenderBadgeComponent 浮动状态徽章
- 2026-05-01 ONNX Runtime 内存优化（F0 释放、共享 Env、DisableCpuMemArena）
- 2026-05-02 GPU/CPU 推理后端重构（删除 DmlRuntimeVerifier、简化 AccelerationDetector、DML1 API）

Status: Active development
Last activity: 2026-05-05 -- Synchronized .planning docs with live tree; all codebase memory docs updated

## Performance Metrics

- Last shipped milestone: `v2.3` (Source/Materialization/Placement persisted truth — frozen, considered shipped)
- Active milestone: `v2.4` PianoRoll Undo/Redo + Async Correction + Playhead Isolation
- Current workspace: active development
- Verification: 三目标（OpenTuneTests/Standalone/VST3）构建全部 PASS

## Accumulated Context

### Decisions

- VST3 ARA 读侧的唯一正确结构现在是 `mutable model + immutable snapshot + callback-driven sample access + region-level playback truth`。
- renderer 和 VST3 editor 只能继续消费 snapshot / epoch / preferred-region truth，不允许旧 getter、retry 或 source-level 单槽 mapping 回流。
- `OpenTuneAudioProcessor` 当前是组合 `SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession` 的 runtime shell，source / editable payload / placement / ARA binding 四类职责继续分别收敛。
- VST3 当前产品语义仍然是 `single instance -> single active audioSource session`，但 2026-04-21 起 official planning 已明确否定"same-source sibling regions 共享一个 editable workspace clip/materialization"。
- Standalone import placement 必须显式传入 `trackId + startSeconds`，processor 不再隐式决定 placement。
- preset chooser / modal async callback 必须使用 `SafePointer`，不允许重新捕获裸 editor `this`。
- app-level preferences 当前由 `AppPreferences` 持有，必须继续独立于 processor/project state。
- shared / standalone preferences page 当前采用显式 composition，不再允许 mixed dialog boolean flags 回流。
- `AudioEditingScheme` 当前是显式纯规则输入；`曲线优先编辑` / `音符优先编辑` 固定映射到 `CorrectedF0Primary` / `NotesPrimary`，voiced-only 行为必须继续由 scheme 推导，而不是独立 preference。
- `showUnvoicedFrames` / `noteNameMode` / `showChunkBoundaries` 必须进入 `AppPreferences` shared state，在 Standalone / VST3 间共享且不被 scheme 切换覆盖。
- Standalone-only mac plist metadata、bundle docs 路径与帮助文档 lookup 已经收敛到 `OpenTune_Standalone` owner 边界；当前剩余只是 macOS 实机 bundle inspection 仍待在对应环境执行。
- undo/redo 当前已经收敛到 content/placement-aware processor/store result chain；后续不得让 `createForCurve`、static 全局 side-channel、whole-curve fallback 或 blind refresh 回流。
- 2026-04-19：用户已明确允许先跳过 Standalone / VST3 的 L5 手工旅程，因此当前 official state 必须把它记为 deferred verification gap，而不是 PASS。
- Standalone 仍是主开发目标；双格式 UI 隔离和 shared processor 双分支边界继续是后续 milestone 的硬约束。
- 2026-04-21：当前唯一正确的产品真相是 `Source + Materialization + Placement`；`Projection` 只是 derived contract。`Note.startTime/endTime` 与 `CorrectedSegment.startFrame/endFrame` 必须永远 materialization-local，Piano Roll 应只拿 `materializationId + projection`，ARA `AudioSource` 对应 source、`PlaybackRegion` 对应 placement，并绑定到 region-local materialization。
- 2026-04-21：materialization 当前显式持有 `sourceStartSeconds/sourceEndSeconds + lineageParentMaterializationId`；split 会重写 child provenance，merge 只在 provenance window 连续且 payload 可无损合并时成立。
- 2026-04-23 (Task 10)：lineage state machine 落地 — `MaterializationStore` / `SourceStore` / `StandaloneArrangement` 都引入 `bool isRetired_` 与双 surface（业务 surface 永不返回 retired，lineage surface 仅供 UndoAction + sweep 使用）。物理 reclaim 仅由 `OpenTuneAudioProcessor::handleAsyncUpdate` 触发的 message-thread sweep 执行，业务函数禁止散调 reclaim。
- 2026-04-23 (Task 10)：sweep invariant = active+retired placements 引用为 0 AND ARA published snapshot 引用为 0 AND undo history actions 中无 lineage 引用。`OpenTuneUndoableAction::lineageReferencesMaterialization` 虚方法是 sweep 与 undo history 的协议接口。
- 2026-04-23 (Task 10)：`PlacementSplitAction.undo/redo` 现在持 `SplitOutcome`（trackId + originalPlacementId/MaterializationId + leadingPlacementId/MaterializationId + trailingPlacementId/MaterializationId），undo 走 retire/revive 切换而不是重跑 mergeStandaloneSplit。Task 11 将基于此扩 PlacementMergeAction / PlacementDeleteAction。
- 2026-04-23 (Task 10 verify)：OpenTuneTests CMake target 现在显式定义 `JucePlugin_Build_Standalone=1 / JucePlugin_Build_VST3=0`。这是硬约束 — 与 SharedCode .lib 编译开关必须对齐，否则 `#if !JucePlugin_Build_Standalone` 守护的 `hostTransportSnapshot_` 等成员会让测试 TU 看到的 `OpenTuneAudioProcessor` 类布局比真实对象多 64 字节，导致 ODR 违规与字段偏移静默错位。

- 2026-04-24 (Task 12 F6)：VST3 PluginEditor.cpp 4 处 command-path silent-return 改为 `AppLogger::log("InvariantViolation: ...")` + `jassertfalse`。涉及 `syncImportedAraClipIfNeeded` 的 prepareImport 失败和 null buffer，以及 `pitchCurveEdited` 的 no-materialization 和 null-curve。新增 architecture guard 测试。
- 2026-04-24 (Task 12 scope)：F3 (SourceStore hydration 迁移) 经评估为高风险（hydration worker 跨 store 锁序问题），标记为后续独立 Task 需专门锁序设计。F5 (reclaim registry 统一) 评估为低价值（sweep 里只有 15 行 `#if`），标记为可选后续 Task。

- 2026-04-24 (VST3 ARA Repair)：完成 4-task 结构修复计划 — (1) BindingState 枚举 + transport API 声明 + 守护测试基线, (2) region materialization birth 从 Editor 下沉到 session hydrationWorkerLoop + processor ensureAraRegionMaterialization, (3) VST3 transport 收归 processor coordinator (requestAraTransportPlay/Stop/Seek + PendingTransportIntent), (4) renderer renderability 改用 BindingState::Renderable 显式合约。preprocessor guard 从 `#if !JucePlugin_Build_Standalone && JucePlugin_Enable_ARA` 改为 `#if JucePlugin_Enable_ARA` 以解决 SharedCode.lib ODR 链接问题。14 个新增 ARA 守护测试全部 PASS，三目标（Tests/Standalone/VST3）构建通过。

- 2026-04-25 (VST3 ARA Read Audio UI/F0)：确认 Read Audio 已能输入 waveform、提取并显示 OriginalF0；进一步修复 OriginalF0 时间轴错位。根因是 `ImportedClipF0Extraction` 按 silent gaps/voiced segments 多次调用 RMVPE 再用 frameOffset 拼回全局曲线，违反 materialization-local 连续派生曲线原则并引入模型上下文/边界/padding 错位。现已改为对完整 materialization-local mono audio 单次 RMVPE 提取，OriginalF0 frame index 重新唯一对应 `i * hopSize / f0SampleRate`，UI projection/renderer 不改。

- 2026-04-25 (ARA auto-birth lineage)：补齐 `ensureAraRegionMaterialization()` 的 sourceWindow owner truth。auto-birth 路径此前按 ARA region sourceWindow 切了正确音频，但 `prepareImport()` 后没有把真实 sourceWindow 写回 `PreparedImport`，导致 `commitPreparedImportAsMaterialization()` 把 lineage 伪造成 `0..duration`。现已在 commit 前写入 `SourceWindow{sourceId, sourceStart, sourceEnd}`，并新增 `ARA-BIND-02b` architecture guard。

- 2026-04-25 (VST3 ARA transport/projection/F0 diagnostics)：Transport 重构为 host snapshot truth + pending command + UI-only presentation 三段明确职责。processor `positionAtomic_` 继续只保存 host committed truth；VST3 editor 用私有 `transportPresentationPosition_` 驱动 PianoRoll 停止态 pending seek 显示。复测发现 atomic 写入仍未触发 stopped overlay 主动刷新，后续补齐 `PianoRollComponent::setPresentedPlayheadPosition()`，并删除 `PlayheadOverlayComponent` stopped-state repaint early return；pending command 也从单一 last-command 改为 playback-state 与 position 两个 pending 分量，避免 stop+seek 互相覆盖。`OpenTunePlaybackRenderer` 删除 renderer-local timeline->materialization 映射 helper，改用 `MaterializationTimelineProjection`，并显式拒绝当前不支持的非 1:1 duration projection。F0 refresh commit 现在打印 `F0Alignment`（audioDuration、firstAudibleTime、firstVoicedFrame/Time、f0FrameCount、expectedInferenceFrameCount、sourceWindow），用于下一轮 DAW 手工判断红线晚出现是非人声前奏还是 RMVPE/input 异常。验证：`OpenTuneTests.exe` PASS，`OpenTune_VST3` build PASS，`OpenTune_Standalone` build PASS。最新日志显示 host engine 为 96k，但 ARA source sampleRate 为 44.1k，waveform 与 OriginalF0 均来自同一 44.1k materialization-local buffer；当前证据不支持 96k/44.1k 时间域错配是 OriginalF0 晚出现根因。

- 2026-04-25 (PianoRoll F0Timeline domain cleanup)：按删除优先原则继续收口 F0 帧域。新增 `Source/Utils/F0Timeline.h` 作为唯一 F0 frame/time 域对象，持有 `frameCount`；`endFrameExclusive` 直接等于 `frameCount`（F0 帧域 authority 由数据本身决定，不被 projection duration 二次裁剪）。删除 PianoRollComponent 旧秒帧 helper，删除 Renderer/ToolHandler/Component 调用点的 `drawableEndFrame`、局部 floor/ceil、audioBuffer-duration F0 裁剪、preview range-as-frameCount、`? getCurveSize() : 0` 空域兜底和未使用公式 API。Renderer margin range 收进 `F0Timeline::rangeForTimesWithMargin()`；AutoTune / entire-clip correction / note selection / hand-draw / line-anchor / note-drag preview 统一消费 `F0FrameRange` 或 `endFrameExclusive()`。验证：`git diff --check` PASS，`OpenTuneTests.exe` PASS，`OpenTune_VST3` build PASS，`OpenTune_Standalone` build PASS。

- 2026-04-30 (Custom UndoManager)：全新自定义 Undo/Redo 系统落地。`UndoManager`（cursor-based，500 层上限）+ `PianoRollEditAction`（notes + correctedSegments 双快照对）。Processor 持有 `undoManager_`，Standalone/VST3 Editor 均通过 `processor.getUndoManager()` 暴露 `undoRequested()`/`redoRequested()` 回调。undo 粒度是完整 materialization-level 状态恢复，不做 diff。
- 2026-04-30 (PianoRollCorrectionWorker)：异步后台修正工作器，支持 `ApplyNoteRange` 和 `AutoTuneGenerate` 两种 kind。单槽 pending + completed 设计，后入请求覆盖前一个，版本计数防并发，避免队列堆积。
- 2026-04-30 (PlayheadOverlayComponent)：播放头绘制从 PianoRoll paint 中拆出为独立透明覆盖层子组件。接受 seconds/zoom/scrollOffset/timelineStart/pianoKeyWidth/playing 状态，内部计算像素 X 位置。关注点分离完成。
- 2026-04-30 (RenderBadgeComponent)：新增浮动徽章组件（半透明黑底圆角矩形 + 白字），用于编辑器顶层显示渲染状态文本。Standalone 和 VST3 Editor 均持有实例。
- 2026-04-30 (PianoRoll enhancements)：VBlankAttachment 驱动滚动回调；Continuous scroll mode + userScrollHold 逻辑；Line Anchor 工具（segment selection + retune speed per segment）；Vibrato depth/rate per-note/per-selection 控制。
- 2026-05-01 (Memory optimization)：F0 模型用完释放（extractF0 完成后自动 shutdown，下次调用 re-initialize）；F0 与 Vocoder 共享单个 `std::shared_ptr<Ort::Env>`；所有推理 session 统一 `DisableCpuMemArena()` + `OrtDeviceAllocator`。预计节省 ~500-900MB 常驻内存。
- 2026-05-02 (Backend restructure)：删除 `DmlRuntimeVerifier`（512行整文件）和 AccelerationDetector 中的 DLL 大小检测(12MB)、VRAM 阈值(512MB)、`getRecommendedGpuMemoryLimit()` 等启发式检查层。DML 可用性改为 `Ort::GetApi().GetExecutionProviderApi("DML")` 直接查询 ORT 编译时注册。DmlVocoder 从 DML2 API（Preference+Filter 隐式选 GPU）改为 DML1 API（`SessionOptionsAppendExecutionProvider_DML(adapterIndex)` 显式绑定 DXGI adapter）。VocoderFactory DML 创建失败 catch 块新增 `overrideBackend(CPU)` 确保检测状态与实际后端一致。RMVPEExtractor preflight 从 GPU/CPU 双分支（73行）简化为纯系统内存路径（18行），因 F0 始终 CPU。detect() 调用移到 ensureOnnxRuntimeLoaded() 之后，防止 ORT DLL 延迟加载未就绪时误判 DML 不可用。

### Pending Todos

- 持续把 `.planning` 与 live tree 保持同步。
- **后续独立 Task（非阻塞）**：F3 SourceStore hydration 迁移（需锁序设计）、F5 reclaim registry 双格式统一（可选）。
- 在合适时机补 Standalone / VST3 undo result-chain 的手工旅程确认。
- 在有 macOS 环境时补一轮真实 `.app` bundle inspection。
- **v2.4 待完成**：确认三目标构建通过；Undo/Redo 边界测试（undo 到空栈、redo 裁剪、500 层溢出）；CorrectionWorker 取消/覆盖语义验证。

### Blockers/Concerns

- 当前无硬阻塞。v2.3 manual verification gaps 已降级为非阻塞 deferred items。

## Session Continuity

Last session: 2026-05-05
Stopped at: .planning 文档全量同步完成；三目标构建通过；40/40+ 测试 PASS
Resume file: N/A
Next step: 继续 v2.4 PianoRoll 编辑功能集成；补充 Undo/Redo 边界测试
