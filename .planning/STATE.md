---
gsd_state_version: 1.0
milestone: v1.5
milestone_name: PianoRoll Undo/Redo + Async Correction + Playhead Isolation
status: active
stopped_at: v1.5 active development + PianoRoll empty-space seek intent + regular VST3 capture UX refinement (display selection, timeline view domain, transport shortcuts)
last_updated: "2026-05-27"
last_activity: 2026-05-27 -- AUTO(REF) reference-driven pitch and timing alignment replan documented; main-memory contract synchronized on top of the latest cumulative feature landings.
progress:
  total_phases: 0
  completed_phases: 0
  total_plans: 0
  completed_plans: 0
  percent: 0
---

# Project State

## Project Reference

See: `.planning/PROJECT.md` and `.planning/REQUIREMENTS.md` (updated 2026-05-17)

**Core value:** 双格式独立编译，零交叉影响
**Current focus:** `v1.5` PianoRoll 编辑体验增强 + VST3 ARA multi-item birth / editor-reopen 生命周期修复
**Test Strategy:** `OpenTuneTests` 轻量 smoke suites + manual DAW journeys + `AppLogger` / targeted trace

## Current Position

Milestone: `v1.5` — PianoRoll Undo/Redo + Async Correction + Playhead Isolation
Phase: 基础架构已落地，功能集成中
Plan sources:
- (承继 v1.4 全部 plan sources)
- 2026-04-30 自定义 UndoManager + PianoRollEditAction
- 2026-04-30 PianoRollCorrectionWorker 异步修正
- 2026-04-30 PlayheadOverlayComponent 独立化
- 2026-04-30 RenderBadgeComponent 浮动状态徽章
- 2026-05-01 ONNX Runtime 内存优化（F0 释放、共享 Env、DisableCpuMemArena）
- 2026-05-02 GPU/CPU 推理后端重构（删除 DmlRuntimeVerifier、简化 AccelerationDetector、DML1 API）
- 2026-05-26 VST3 ARA OriginalF0 收口子项（metadata-only SourceStore、birth 结果语义收紧、Editor 残留删除、AraFinal 守卫）
- 2026-05-26/27 VST3 ARA multi-item birth + editor reopen structural fix（自动化闭环；REAPER 手工测试用户明确不要求本次执行）
- 2026-05-27 AUTO(REF) reference-driven pitch and timing alignment replan + test-verification（合同重写，尚未实现）

Status: Active development
Last activity: 2026-05-27 -- AUTO(REF) reference-driven pitch and timing alignment replan has been documented and synchronized into main memory. The cumulative feature landings from the last 8 commits remain the latest implementation baseline; AUTO(REF) work is still at the planning/contract stage only.

## Performance Metrics

- Last shipped milestone: `v1.4` (Source/Materialization/Placement persisted truth — frozen, considered shipped)
- Active milestone: `v1.5` PianoRoll Undo/Redo + Async Correction + Playhead Isolation
- Current workspace: active development
- Verification: ARA `OpenTuneTests` Release build PASS; `architecture` / `processor` / `core` / `memory` suites PASS for the 2026-05-27 ARA lifecycle fix; ARA VST3 build PASS; non-ARA VST3 build PASS; Standalone build PASS. `timeline-rendering` / `piano-roll-f0-visual` remain prior focused PASS evidence. `ui` suite currently exits 1 after a PASS line and no `[FAIL]` text, so full-suite PASS must not be claimed.

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

- 2026-05-15 (VST3 ARA multi-region binding)：按官方 ARA owner 模型收口 — `AudioSource` 只对应 source/provenance，`AudioModification persistentID` 对应持久 editable materialization binding，`PlaybackRegion` 只对应 projection。`VST3AraSession::RegionSlot` 保存 `audioModificationPersistentId`，`materializationBindings_` 保存 persistentID -> materialization binding；同一 AudioModification 的多个 PlaybackRegion 共享 materialization，不同 AudioModification 即使 sourceWindow 相同也默认独立。`OpenTuneDocumentController` archive hooks 转发 versioned binding store/restore；VST3 editor 不再用 `araClipImportArmed_` 作为已绑定 materialization 的显示门。L5 Reaper 多 item/保存恢复仍待手工验证。
- 2026-05-27（ARA multi-item / reopen fix）：birth pending truth 已迁到 `audioModificationPersistentId + SourceWindow + revision`；worker-ready 队列只表达“哪些 persistentId 当前可执行”，不再承担 source 级 birth 真相。旧 worker result 必须在 commit 前按 revision/window 丢弃。
- 2026-05-27（ARA multi-item / reopen fix）：`PluginEditor` 是只读消费者；payload/buffer 暂缺不再触发 destructive clear。`clearPlaybackRegionMaterialization()` 已从 production session API 删除，缺 payload 只能表现为 pending/restoring UI 状态。
- 2026-05-27（ARA pre-bind restore fix）：metadata-only ARA VST3 state 在 `didBindToARA()` 前只缓存，绑定后 replay 到最终 shared stores；regular unbound VST3 state 不走 ARA 缓存路径，仍立即恢复本地 capture/project 状态。
- 2026-05-27（Experimental features gate / TimeTool seed re-scope）：现有 `ExperimentalReferenceAlignMode` 已被明确界定为 AUTO Ref 模式，不再允许兼任实验功能总开关。下一轮 Standalone 计划将新增独立 `experimentalFeaturesEnabled` shared preference，用于统一门控 TimeTool 与 Arrangement 参考源入口；同时 `PianoRollComponent::setCurrentTool(TimeTool)` 将恢复为“首次进入当前 materialization 时请求 processor 播种 identity stretch anchors”的正式语义点，但 seed 只允许共享 timing-feature extraction，不再复用 `DerivedAnalysis` 作为 `AUTO(REF)` backbone，UI 不得自行造 `TimeGridSnapshot`。

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

- 2026-05-17 (Studio One ARA stopped playback gate): Studio One logs show `HostTransportSnapshot: playing=false time=80.000000` immediately followed by repeated ARA playback renderer mappings at the same playback/materialization sample (`mappedLocalSampleForLog=3528000`). This confirms Studio One can pull realtime ARA playback while transport is stopped or paused. `OpenTunePlaybackRenderer` now silences realtime stopped blocks before region mapping/readback while preserving non-realtime ARA reads. The gate clears output and returns `true` from `processBlock(...)` to express ARA-handled silence rather than non-ARA fallback. Plan source: `.planning/plans/2026-05-17-studio-one-ara-stopped-render-gate.md`; verification source: `.planning/plans/2026-05-17-studio-one-ara-stopped-render-gate-test-verification.md`.
- 2026-05-17 (ARA-capable regular VST3 runtime split): Studio One short failure logs for track-insert `Read Audio` contain ctor/prepare only and no `DocumentController created` / `didBindToARA`, proving the failing instance is regular VST3 mode. Cubase uses explicit ARA extension workflows rather than plain channel inserts, REAPER can bind ARA from track FX when enabled, and Live should be treated as regular VST3 only. Implemented fix: ARA builds also create regular `CaptureSession`, expose/use it only when `!isBoundToARA()`, keep ARA-bound instances on DocumentController/session/snapshot, and log `recordRequested mode=ara-bound|regular-vst3 processor=... dc=...`. Plan source: `.planning/plans/2026-05-17-ara-capable-regular-vst3-runtime-mode.md`; verification source: `.planning/plans/2026-05-17-ara-capable-regular-vst3-runtime-mode-test-verification.md`.

- 2026-05-18 (Piano Roll empty-space seek intent): One mouse gesture = one intent. `mouseDown` on empty PianoRoll space arms `EmptySpacePending`; `mouseUp` within 12px threshold seeks to click time and consumes gesture; drag beyond threshold converts to current tool drag path for seek-enabled tools (box selection / note drawing / hand-draw). `setTool()` cancels pending intent so transient state cannot leak across tool switches. Timeline ruler seeking remains immediate. 2026-05-19 correction: LineAnchor main-edit clicks are anchor-tool input, never empty-space seek input. Continuous scroll mode centers on seek via `notifyPlayheadChange`. Plan: `.planning/plans/2026-05-18-piano-roll-empty-space-seek-intent.md`; verification: `.planning/plans/2026-05-18-piano-roll-empty-space-seek-intent-test-verification.md`.

- 2026-05-18 (Regular VST3 capture display selection): After regular VST3 insert capture finishes, the VST3 editor maintains display of the captured materialization even when host playhead leaves the segment. `CaptureSession` callback stores active segment/materialization id; `resolveCurrentMaterializationProjection()` updates selection on playhead hit and resolves stored selection on miss. ARA-bound instances continue using ARA snapshot truth and do not consume capture display selection. Automated architecture guard `AraRuntime_RegularCaptureDisplayKeepsCompletedSelection` PASS; `architecture/processor/core` suites PASS; ARA/non-ARA VST3 builds PASS. Status: Implemented; host L5 pending. Plan: `.planning/plans/2026-05-18-regular-vst3-capture-display-selection.md`; verification: `.planning/plans/2026-05-18-regular-vst3-capture-display-selection-test-verification.md`.

- 2026-05-18 (Regular VST3 capture timeline view domain): Studio One regular VST3 capture can record a late timeline slice (e.g. at `03:25`). Previously PianoRoll treated the captured segment projection start as scroll/view origin, making earlier timeline time negative and unreachable. Fix: PianoRoll view domain defaults to time zero, preserving the recorded segment at its actual timeline position while allowing horizontal scroll to earlier time. Automated test `PianoRollTimelineViewDomain_LateCaptureCanBrowseBeforeSegment` PASS. Status: active implementation; host L5 pending. Plan: `.planning/plans/2026-05-18-regular-vst3-capture-timeline-view-domain.md`; verification: `.planning/plans/2026-05-18-regular-vst3-capture-timeline-view-domain-test-verification.md`.

- 2026-05-18 (Regular VST3 transport shortcuts): Regular VST3 must not pretend to own host transport. Keyboard shortcuts and transport buttons route through a unified helper; no host-specific branch, no global keyboard hook, no fake transport truth. Architecture guards (`Vst3KeyboardShortcuts_RouteThroughUnifiedHelper`, `Vst3TransportButtons_DoNotForgeRegularPlaybackTruth`, `Vst3RegularTransport_SurfacesHostControlledSemantics`, `RegularVst3Capture_UsesHostPlayheadTruth`) PASS; ARA/non-ARA VST3 builds PASS. Status: Implemented; host L5 pending. Verification: `.planning/plans/2026-05-18-regular-vst3-transport-shortcuts-test-verification.md`.

### VST3 ARA OriginalF0 子收口 (2026-05-26)

This subset did land, but it is not the final closure for the Reaper multi-item / editor reopen bug family:

- **Phase 1: SourceStore metadata-only** — `CreateSourceRequest` 新增 `numChannels`/`numSamples`；`createSource()` 允许 `audioBuffer == nullptr` 时从 request 字段直接注册；Standalone full-PCM 路径不变。
- **Phase 2: ARA birth 结果语义收紧** — 删除 `makePartialResult`，改为 `buildBirthResult`；F0 service not ready/nullptr 时显式写 `OriginalF0State::Failed`；成功/失败路径都调用 `releaseImmediately()`。
- **Phase 3: Worker 命名收口** — `hydrationWorkerLoop` → `birthWorkerLoop`；`hydrationCv_` → `birthCv_`；`hydrationWorkerThread_` → `birthWorkerThread_`。
- **Phase 4: Editor 残留删除** — 删除 `rmvpeOverlayLatched_`/`rmvpeOverlayTargetMaterializationId_`；`PluginEditor.h/.cpp` ARA-related 代码精简 358 行。
- **Phase 5: 契约注释刷新** — `requestMaterializationRefresh` 改为正向"Standalone/regular VST3 F0 refresh"；不再以 ARA 为中心解释。
- **Phase 6: 测试收口** — 7 个 `AraFinal_*` 终审守卫（negative guard + path guard）；3 个 active test 从旧 `copiedAudio` 叙事改名为中立 binding/payload 语义。
- **setStateInformation 修复** — 移除 ARA source 恢复路径中 `sourceAudioBuffer == nullptr` 早退条件，null buffer 的 source 通过 metadata-only 注册创建。
- 三目标编译通过（Tests/Standalone/VST3）+ architecture suite 7/7 AraFinal PASS。
- 2026-05-26 之前 5 个 commit（vocal-time-stretch + reference auto-align + vocoder dual weights + Aurora theme）也包含在内，当前工作区快照同步完成。

2026-05-27 follow-up structural fix closed the audit gaps:

- auto-birth pending truth is now keyed by `audioModificationPersistentId + SourceWindow + revision`
- callback paths that leave `regionNeedsMaterializationBirthLocked()` true upsert the persistentId pending birth, including attach-only and ready-source requeue paths
- editor destructive clear is gone; `clearPlaybackRegionMaterialization()` is no longer a production session API
- metadata-only ARA pre-bind state is cached and replayed after `didBindToARA()` into final shared stores; regular unbound VST3 state restores immediately

Plan source:

- `.planning/plans/2026-05-26-vst3-ara-multi-item-birth-and-editor-reopen-structural-fix.md`
- `.planning/plans/2026-05-26-vst3-ara-multi-item-birth-and-editor-reopen-test-verification.md`

### AUTO(REF) Contract Corrections (2026-05-27)

- `AUTO(REF)` 的正式产品语义改为 `ClipA -> ClipB` 参考驱动音高+节奏对轨；formal output 只允许写回 `ClipA.notesAfter + ClipA.correctedSegmentsAfter + ClipA.timeGridAfter`。
- `AUTO(REF)` 的 timing path 必须依赖 `ReferenceTimingFeatures + EffectiveTimeMap`，而不是要求 target/reference 先有 seeded `TimeGridSnapshot` 或先进入一次 `TimeTool`。
- `TimeTool` 首次 identity anchor seed 仍是 processor-owned 入口，但只允许共享 timing-feature extraction，不再复用 `DerivedAnalysis` 作为 `AUTO(REF)` backbone。
- `basicAnalysis`、`enhancedAnalysis`、`analysisMode` 必须退出正式 project truth，只允许短期存在于 legacy reader ignore/migration path。
- `ClipA.timeGridAfter` 的正式含义进一步收口为“受约束 auto handle drag 写回普通 `TimeTool` truth”；`AUTO(REF)` 前后用户都必须还能继续手工拖拽这些 handles。
- `AUTO(REF)` 自动 timing patch 触及到的每个局部区间都必须满足 `0.8 <= speed_i <= 1.3`；超出参考要求时只能投影/饱和到最近可行解，不允许为了强行命中参考而过度拉伸。
- 上述 `0.8x~1.3x` 速度窗口当前只约束 `AUTO(REF)` 自动 patch，不自动改写既有手工 `TimeTool` 拖拽合同。

### Pending Todos

- 持续把 `.planning` 与 live tree 保持同步。
- ✅ 实现并验证 experimental-features gate：独立 boolean、提示文案、TimeTool/Arrangement 入口统一门控。已通过 706c844 落地。
- 实现并验证 TimeTool 首次入场 identity 锚点播种：processor 单入口、只共享 timing-feature extraction、不覆盖既有 stretch 编辑，也不再承担 `AUTO(REF)` backbone 语义。
- Request user confirmation for Studio One / REAPER / Cubase / Live L5 journeys after installing the rebuilt VST3 (regular-vst3 capture display, timeline view domain, transport shortcuts).
- Request user confirmation for PianoRoll empty-space seek intent L5 manual visual behavior.
- REAPER ARA multi-item/project reload L5 不由 Codex 本次执行；用户明确说手工测试不用做。若之后有人手工执行，可补记录，但不得倒填为本次 PASS。
- 解释并修复 `OpenTuneTests.exe ui` exit=1/no `[FAIL]` text 的 runner 现象，然后才能恢复 full-suite PASS 口径。
- **后续独立 Task（非阻塞）**：F3 SourceStore hydration 迁移（需锁序设计）、F5 reclaim registry 双格式统一（可选）。
- 在合适时机补 Standalone / VST3 undo result-chain 的手工旅程确认。
- 在有 macOS 环境时补一轮真实 `.app` bundle inspection。
- **v1.5 待完成**：确认三目标构建通过；Undo/Redo 边界测试（undo 到空栈、redo 裁剪、500 层溢出）；CorrectionWorker 取消/覆盖语义验证。

### AUTO(REF) Priority Reset

- 最高优先级：重写 `AUTO(REF)` 正式 shared-core 合同，拆分 `ReferencePitchFeatures` / `ReferenceTimingFeatures`，把 formal output 收口为 target-only `notes + correctedSegments + timeGrid`。
- 最高优先级：移除 `AUTO(REF)` 对 seeded `TimeGridSnapshot` / existing `TimeTool` handles 的前置依赖，改用 target/reference `EffectiveTimeMap` 驱动 timing alignment。
- 最高优先级：把 `basicAnalysis` / `enhancedAnalysis` / `analysisMode` 从正式 project truth 退场，只保留必要的 legacy reader ignore/migration path。
- 最高优先级：把 timing 结果明确落成普通可编辑 `ClipA.timeGridAfter`，并在 auto patch 范围内施加 `0.8x~1.3x` 局部速度窗口与超限饱和语义。
- TimeTool 首次入场 identity 锚点播种只允许共享 timing-feature extraction，不再承担 `AUTO(REF)` backbone 语义；旧的 “reuse DerivedAnalysis main chain” 口径在此处视为 superseded。

### Blockers/Concerns

- 当前无硬阻塞。v1.4 manual verification gaps 已降级为非阻塞 deferred items。2026-05-18 四组计划自动化验证全部 PASS，L5 所有项待用户确认。

## 2026-05-27 State Addendum: Standalone Import Track-Target Drop UX

Status: implemented.

What changed:

- `PluginEditor::filesDropped(...)` now uses `x,y` coordinates to resolve a target track from Arrangement track-lane geometry, enabling spatial drag-drop import.
- `ImportDropTarget` component handles geometry resolution, hover preview, and click-to-import for the blank-area case.
- Drop onto an existing track lane → import into that track.
- Drop into Arrangement blank space below visible tracks → creates one new visible track (unless `MAX_TRACKS` reached) and imports there.
- Drop outside Arrangement → deterministic active-track fallback.
- Single-file chooser import remains popup-free.
- All real commits still go through explicit editor-owned `ImportPlacement`.
- Preview state remains transient Standalone UI-only data; it does not enter `StandaloneArrangement`, `OpenTuneAudioProcessor`, undo history, project serialization, or VST3/ARA state.

Plan source:

- `.planning/plans/2026-05-27-standalone-import-track-target-drop-ux.md`
- `.planning/plans/2026-05-27-standalone-import-track-target-drop-ux-test-verification.md`

Implemented in commits 8ea7305 / 706c844.

## 2026-05-27 State Addendum: AUTO(REF) Contract Replan

Status: planned, not implemented.

What changed:

- `AUTO(REF)` is now defined as formal `ClipA -> ClipB` reference-driven pitch and timing alignment, not as an extension of the old experimental seed path.
- Reference binding truth remains on `StandaloneArrangement::Placement::referencePlacementId`; `ClipB` is read-only reference input.
- Feature production is split into `ReferencePitchFeatures` and `ReferenceTimingFeatures`, both in materialization-local source-time.
- Timing alignment must consume target/reference `EffectiveTimeMap` instead of requiring seeded `TimeGridSnapshot` handles before the operation can run.
- `TimeTool` first-entry seed is reduced to processor-owned identity-handle preparation that only shares timing-feature extraction.
- Long-term project truth is reduced to `referencePlacementId + notes + correctedSegments + timeGrid`; `basicAnalysis`, `enhancedAnalysis`, and `analysisMode` must leave formal persistence.
- The timing result is further defined as constrained auto handle drag written back into ordinary `ClipA.timeGridAfter`, so the post-run state remains directly editable in `TimeTool`.
- The auto timing patch must obey the local speed window `0.8x~1.3x`; when exact reference timing would exceed it, alignment must saturate/project to the nearest feasible result instead of over-processing audio.
- That speed window is currently scoped only to `AUTO(REF)` automatic patch semantics; manual `TimeTool` dragging remains unchanged unless planned separately later.

Plan source:

- `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment.md`
- `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment-test-verification.md`

## Session Continuity

Last session: 2026-05-27
Stopped at: ARA multi-item birth / editor reopen structural fix 已完成自动化闭环；REAPER 手工测试用户明确不要求 Codex 执行
Resume file: N/A
Next step: 先按 2026-05-27 新规划重写 `AUTO(REF)` 正式合同，再把 TimeTool identity seed 收紧为 timing-feature extraction 共享语义，之后再继续 v1.5 其它 open 项（UI suite exit-code、Undo 边界、CorrectionWorker 并发、其它宿主 L5）。
