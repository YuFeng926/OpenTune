# Requirements: OpenTune Post-v1.3.2 编辑融合、应用偏好与 Source/Materialization/Placement 真相澄清

**Defined:** 2026-04-20
**Status:** ACTIVE MAINLINE
**Core Value:** 双格式独立编译，零交叉影响
**Detailed plan source:** `docs/plans/2026-04-17-single-source-workspace-{design,tasks}.md`, `docs/plans/2026-04-18-app-preferences-refactor.md`, `docs/plans/2026-04-18-app-preferences-refactor-test-verification.md`, `docs/plans/2026-04-19-interaction-scheme-visual-preferences.md`, `docs/plans/2026-04-19-interaction-scheme-visual-preferences-test-verification.md`, `docs/plans/2026-04-19-mac-standalone-bundle-migration.md`, `docs/plans/2026-04-19-mac-standalone-bundle-test-verification.md`, `docs/plans/2026-04-19-undo-affected-range-migration.md`, `docs/plans/2026-04-19-undo-affected-range-test-verification.md`, `docs/plans/2026-04-20-content-placement-two-truth-refactor.md`, `docs/plans/2026-04-20-content-placement-two-truth-refactor-test-verification.md`, `docs/plans/2026-04-21-content-placement-boundary-repair.md`, `docs/plans/2026-04-21-content-placement-boundary-repair-test-verification.md`, `docs/plans/2026-04-21-source-materialization-placement-projection-clarification.md`

本主线承接 `v1.3.2` 发版后的继续收敛工作。2026-04-20 曾把 active line 写成 `Content/Placement` 两层真相重构；但 2026-04-21 用户明确澄清：同一份 source 在两个 placement 上必须可独立改 note、某个 ARA region 的编辑不得影响 sibling region、split 后左右两段默认拥有独立 editable materialization。由此，official planning 已改正为：`OpenTuneAudioProcessor` 继续作为 `SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession` 的 runtime shell，`AppPreferences` 与 `AudioEditingScheme` 的 owner 边界保持不变，但 persisted business truth 必须改写为 **`Source + Materialization + Placement`**，`Projection` 只是显式 derived contract。

## Current Mainline Requirements

### 已落地到 Live Tree

- [x] **MAIN-01**: `OpenTuneAudioProcessor` 作为 runtime shell 组合 `SourceStore`、`MaterializationStore`、`StandaloneArrangement` 与 `VST3AraSession`；source identity truth、materialization payload truth、Standalone placement truth、VST3 ARA binding truth 不再混在同一套旧容器里
- [x] **MAIN-02**: VST3 当前仍采用 `single instance -> single active audioSource session` 的单工作区边界；但这不再等价于“同 source 只保留一个 shared editable clip/materialization”
- [x] **MAIN-04**: Standalone `commitPreparedImportAsPlacement()` 必须显式接收 `ImportPlacement`，processor 不再隐式猜测 `trackId` 或 `startSeconds`
- [x] **MAIN-05**: Standalone preset chooser 的 `launchAsync()` 回调统一使用 `SafePointer`，禁止再捕获裸 editor `this`
- [x] **MAIN-06**: 应用级 shared/standalone preferences 统一收敛到 `AppPreferences` typed schema；processor state 不再承接 app-level preference
- [x] **MAIN-07**: Standalone 与 VST3 当前都使用显式 preferences page composition；VST3 只暴露 shared pages，Standalone 组装 `Audio + Shared + Standalone-only`
- [x] **MAIN-08**: `AudioEditingScheme` 当前以显式输入规则驱动 parameter target、auto-tune target 与 parameter panel sync，不再依赖隐藏 scheme owner
- [x] **MAIN-09**: notes-first 的第一轮交互反馈已进入 live tree：`NotesPrimary` 下 hand-draw / line-anchor 后会自动选中受影响 notes；`CorrectedF0Primary` 允许 line-anchor segment selection，但参数面板不再把 LineAnchor segment 元数据作为 retune target，LineAnchor 输出真相固定为 committed `f0Data`
- [x] **MAIN-12**: `曲线优先编辑` / `音符优先编辑` 现在已经固定为仅有的两套交互方案；hand-draw / line-anchor 的 voiced-only 行为已由 `AudioEditingScheme` 纯规则推导，`showUnvoicedFrames` / `noteNameMode` / `showChunkBoundaries` 已作为 shared app preferences 独立持久化
- [x] **MAIN-13**: undo/redo 当前已从“整条曲线刷新”收敛为 content/placement-aware affected-range 执行结果链；`UndoAction` / `UndoManager` / `OpenTuneAudioProcessor` 返回 immutable result，`PianoRollComponent` 与 Standalone/VST3 editor 消费同一份 result chain，且 `createForCurve`、curve-bound applier、static side-channel、whole-curve fallback 已被移除
- [x] **MAIN-14**: dual-format 仓库下的 mac Standalone 打包已补齐 app-only plist metadata 与 bundle docs 路径，并保持 VST3 target 不受污染；当前剩余 gap 仅是 macOS 实机 bundle inspection 仍需在对应环境执行
- [x] **MAIN-17**: 2026-04-20 的 `Content/Placement` phase 已建立 delete-first plan / verification baseline，并清掉 stale deleted-helper compile-break baseline；但 2026-04-21 起这些文档只保留历史/战术价值，不再是 owner-model source of truth
- [x] **MAIN-18**: Standalone `deleteClip()` 当前已先从“删一个 placement 就误删 shared content”修正到“先删 placement，再回收失去最后一个 placement / ARA 引用的 content”；但按 2026-04-21 澄清，这仍只是走向 `placement -> materialization -> source` 生命周期的中间态

### 当前仍需持续守护 / 收敛

- [ ] **MAIN-10**: `.planning/PROJECT.md`、`.planning/ROADMAP.md`、`.planning/STATE.md` 与 live tree 的真实结构、测试现状、调试策略必须保持同步，后续不再允许 official planning 滞后于 `docs/plans/2026-04-18-*`、`docs/plans/2026-04-19-*` 或源码现实
- [ ] **MAIN-11**: 当前主线的验证口径固定为“`OpenTuneTests` 轻量 smoke suites + 人工旅程 + `AppLogger` / targeted trace”；若后续需要补自动化，只允许新增针对性高信号守护，不恢复旧的 Phase 23-26 guard 家族
- [ ] **MAIN-16**: undo result-chain 的 Standalone / VST3 L5 手工旅程本轮按用户许可暂缓；official planning 必须显式保留这条未完成验证，而不能把它记成 PASS
- [ ] **MAIN-15**: 当前主线仍是 `post-v1.3.2` 的未编号收敛线；待上述范围冻结后，需要再决定下一个正式 milestone / release boundary
- [ ] **MAIN-19**: shared runtime 必须彻底废弃 `clipId` 同时代表 source/materialization/placement 的混合 owner 语义；正式 persisted owners 改为 `sourceId`、`materializationId`、`placementId`，`Projection` 明确保持为 derived value object
- [ ] **MAIN-20**: 同一份 source 再次出现在新的 Standalone placement 或新的 ARA playback region 上时，默认必须创建新的 editable materialization，而不是复用 existing shared content/workspace clip
- [ ] **MAIN-21**: split 默认必须 birth left/right 两个新 materialization；不得再用 `placement.contentStartSeconds`、切 buffer 窗口或 shared content projection 来模拟“独立可编辑左右半段”
- [ ] **MAIN-22**: PianoRoll 必须只消费 `materializationId + projection`；`Note.startTime/endTime` 与 `CorrectedSegment.startFrame/endFrame` 永远 materialization-local，不允许把 placement 时间直接当 persisted editable truth
- [ ] **MAIN-23**: Standalone playback 与 VST3 ARA mapping 必须按 placement projection 把 timeline block 映射到 materialization-local audio/time；不得继续使用 `AudioSource -> shared content -> sibling reuse` 这类混合 owner 读法
- [ ] **MAIN-24**: delete/reclaim 流程必须从“placement -> materialization -> source”三层生命周期重写；`reclaimUnreferencedContent()` 这类两层回收逻辑不再足够表达产品真相
- [ ] **MAIN-25**: 现有 `OpenTuneTests` 的 L1-L4/L6 绿灯只证明旧 `Content/Placement` 假设下的一部分结构守护仍成立；official planning 必须把它明确记成 partial evidence，而不是 owner-model PASS。L5 手工旅程与 macOS bundle inspection 继续显式保留为未完成 gap
- [x] **MAIN-26**: VST3 ARA editable owner 必须按 AudioModification persistent ID 绑定 materialization；同一 AudioModification 的多个 PlaybackRegion 共享一个 materialization，新的 AudioModification 即使 source window 相同也默认独立 materialization。binding table 必须由 ARA session/document archive 持久化，不得由 editor preferred-region 或 transient PlaybackRegion 指针拥有。
- [x] **MAIN-27**: VST3 ARA playback renderer 在 realtime host block 且 transport `isPlaying=false` 时必须输出静音并停止 region mapping/readback；非实时 ARA reads/export/materialization access 不得因此被静音。实现已采用 ARA-handled silence return semantics（gate 分支 `buffer.clear()` 后 `processBlock(...)` 返回 `true`，不请求 non-ARA fallback）。Studio One pause/stop/play 行为仍保留为 L5 验证项，不能只凭 REAPER 行为推断。
- [x] **MAIN-28**: ARA-capable VST3 binary 必须按运行时绑定状态分流，而不能把 `JucePlugin_Enable_ARA` 等同于 ARA-bound instance。`isBoundToARA() == true` 时继续只走 ARA `DocumentController` / `VST3AraSession` / immutable snapshot；未绑定但作为 VST3 insert 运行时必须启用 regular `CaptureSession`。`recordRequested()` 和 `processBlock()` 必须先判运行时 mode：有 DC 走 ARA Read Audio / focused-region refresh，无 DC 但有 capture session 走普通轨道录音读取，两者都没有才提示当前实例不可读取。实现已让 ARA build 也创建 regular capture state，并通过 `getCaptureSession()` 在 ARA-bound 后隐藏；Studio One / REAPER / Cubase / Live host L5 仍需用户确认。

## Out of Scope

| Feature | Reason |
|---------|--------|
| Melodyne 式多 source 同时编辑 UI | 当前产品阶段只支持单实例单 active workspace |
| 同 source 切换 region 时重新创建 detached clip 再删除 previous clip | 这会破坏单工作区语义并引入不必要的 clip churn |
| `commitPreparedImportAsPlacement()` 在 processor 内隐式决定 placement | placement 是 Standalone editor 的显式语义，不允许再回退成隐藏决策 |
| `FileChooser::launchAsync()` 回调捕获裸 editor `this` | 会重新引入异步生命周期悬空风险 |
| 把 app-level preferences 写进 processor/project state | app preference 与 clip/project truth 生命周期不同，不能混写 |
| 重新引入 `AudioEditingSchemeManager`、boolean-flag mixed preferences dialog、hidden mutable theme/language owner | 与显式 state carrier 和双格式 UI 边界相冲突 |
| 把 `voicedOnlyEditing` 做成独立用户 preference，或让 scheme 切换覆盖 shared visual preferences | voiced-only 是 scheme-derived interaction policy，visual prefs 是独立 shared app preference，二者都不应变成额外 owner |
| 把 mac app-only plist key 直接挂到 shared `juce_add_plugin` | 会污染 VST3 target，不符合 dual-format build 边界 |
| 兼容 getter、retry、`createForCurve`、并行旧新结构、兜底路径 | 与项目“唯一正确结构”原则冲突 |
| 把 `clipId` 继续同时当成 content owner 与 placement owner | 会让 shared runtime、PianoRoll、playback、ARA 持续处于混合真相状态 |
| 把 projection、adapter cache、UI 草稿或 hydration 副本升级成 persisted truth owner | persisted truth 已明确是 `Source + Materialization + Placement`；derived projection 与 transient/cache state 不能再伪装成第四个 owner |
| 用“通用 move = 改 content 时间”或“split/merge 默认切 buffer / 复制 content”维持旧行为 | 会让 placement 编辑污染共享素材，直接破坏同一 content 多处摆放语义 |

## Traceability

| Source | Responsibility |
|--------|----------------|
| `docs/plans/2026-04-17-single-source-workspace-design.md` | 单工作区、shared clip reuse、三段真相 owner 的基础结构约束 |
| `docs/plans/2026-04-17-single-source-workspace-tasks.md` | post-v1.3.2 基础结构收敛是如何落地的任务拆解 |
| `docs/plans/2026-04-18-app-preferences-refactor.md` | app-level preferences owner、显式 preferences composition、shared rule 输入的结构约束 |
| `docs/plans/2026-04-18-app-preferences-refactor-test-verification.md` | 当前 app preferences / editing scheme 相关验证口径 |
| `docs/plans/2026-04-19-interaction-scheme-visual-preferences.md` | 固定 interaction scheme、scheme-managed voiced-only 行为、shared visual preferences 的执行计划 |
| `docs/plans/2026-04-19-interaction-scheme-visual-preferences-test-verification.md` | interaction scheme 与 shared visual preferences 迁移的 L1-L6 验证口径 |
| `docs/plans/2026-04-19-mac-standalone-bundle-migration.md` | Standalone-only plist metadata、bundle docs 路径、help lookup owner boundary 的执行计划 |
| `docs/plans/2026-04-19-mac-standalone-bundle-test-verification.md` | mac Standalone bundle source/build structure 的已执行验证与 macOS 实机 bundle inspection gap |
| `docs/plans/2026-04-19-undo-affected-range-migration.md` | undo/redo 从整曲线刷新迁到 processor-owned clip-based execution result chain 的执行计划 |
| `docs/plans/2026-04-19-undo-affected-range-test-verification.md` | undo affected-range 迁移的 L1-L6 验证口径与手工旅程要求 |
| `docs/plans/2026-04-20-content-placement-two-truth-refactor.md` | 历史上的两层真相尝试；今天只保留 delete-first / boundary-first 执行策略价值，不再作为 owner-model 真相 |
| `docs/plans/2026-04-20-content-placement-two-truth-refactor-test-verification.md` | 历史上的两层真相验证口径；今天只作旧假设证据归档 |
| `docs/plans/2026-04-21-content-placement-boundary-repair.md` | 在旧 owner 假设下做的边界修复计划；需按 materialization-local 语义重新解释 |
| `docs/plans/2026-04-21-content-placement-boundary-repair-test-verification.md` | 边界修复验证口径；仍有参考价值，但不再单独证明 owner model 正确 |
| `docs/plans/2026-04-21-source-materialization-placement-projection-clarification.md` | 当前唯一正确的 owner-model 澄清：`Source + Materialization + Placement` persisted truth，`Projection` 为 derived contract |
| `.planning/plans/2026-05-17-studio-one-ara-stopped-render-gate.md` | Studio One stopped/pause ARA playback noise root cause and renderer-local fix plan |
| `.planning/plans/2026-05-17-studio-one-ara-stopped-render-gate-test-verification.md` | ARA stopped-state render gate automated and manual verification contract |
| `.planning/plans/2026-05-17-ara-capable-regular-vst3-runtime-mode.md` | Runtime split plan for ARA-capable VST3 instances loaded without ARA binding in Studio One/Live/regular insert contexts |
| `.planning/plans/2026-05-17-ara-capable-regular-vst3-runtime-mode-test-verification.md` | Verification source for ARA-bound versus regular VST3 runtime mode split |
| `.planning/codebase/TESTING.md` | 当前 live tree 的 smoke tests、人工验证方式与日志调试口径 |

## Notes

- `v1.3.2` 仍是最新 shipped milestone；本文件描述的是其后的 active mainline，而不是新的已发版版本号。
- 当前 active mainline 的 live-tree reality 已包含 2026-04-18 app preferences refactor、2026-04-19 landed 的 scheme-managed voiced-only 行为 / shared visual preferences / Standalone-only mac bundle packaging cleanup / undo result-chain 实现，以及 2026-04-20 启动的 `Content/Placement` owner-cleanup 尝试；但 2026-04-21 的用户澄清已经说明：`ContentStore` 这层 live-tree reality 更接近 materialization owner，而不是最终想要共享的 source/content owner。
- 当前主线仍不把队友仓库的 `tracks_` 单体模型、mutable note ref、curve-bound undo、standalone-only build 假设当成回流目标；只迁移其中与当前 owner 边界兼容、且符合 fixed-scheme + shared-preference 合约的正确部分。
- 当前 phase 的完整验证仍未结束：旧 `Content/Placement` phase 自己的 L1-L4/L6 已按旧 verification source 重新执行并 PASS，但这不再等价于 owner-model PASS；现在新的 `Source/Materialization/Placement` guards 也已覆盖 source provenance / lineage、structured merge rejection、merge payload preservation 与 ARA source owner seeding，并重新执行通过。剩余未完成项只剩 L5 Standalone/VST3 手工旅程与 macOS bundle inspection。
- 2026-05-15 reality 补充：ARA session / renderer / VST3 editor 当前公开 contract 已进一步收口到 `AppliedMaterializationProjection` + `bindPlaybackRegionToMaterialization()`；`PublishedRegionView` 已公开 `sourceId`，`RegionSlot` 保存 `audioModificationPersistentId`，session 用 `materializationBindings_` 按 AudioModification persistent ID 绑定 materialization。`recordRequested()` 仍是 focused-region refresh/ensure command，但不再是多 region 播放真相 owner；processor refresh / undo / reclaim side 也已切到 `MaterializationRefreshRequest`、materialization-facing getter/setter、`reclaimUnreferencedMaterialization()` / `reclaimUnreferencedSource()`；Piano Roll 当前公开 contract 已切到 `MaterializationTimelineProjection` + `setEditedMaterialization(...)`；split / merge / state serialization 现在都会保留 materialization 的 source provenance window 与 lineage metadata。

---
*Requirements defined: 2026-04-20*
## 2026-05-26 Requirement Addendum: ARA OriginalF0 Minimal Chain

### Active

- [x] **MAIN-29**: VST3 ARA automatic OriginalF0 must have exactly one content-processing owner: `VST3AraSession` schedules source-window birth and `OpenTuneAudioProcessor::birthAraMaterializationWithOriginalF0(AraOriginalF0BirthRequest)` performs the only ARA content read/F0 commit path. — **Done**: `birthAraMaterializationWithOriginalF0` 是唯一 ARA 内容读取/F0 提交路径
- [x] **MAIN-30**: ARA-bound `PluginEditor::recordRequested()` must be binding/display only. It must not trigger `requestReferenceNoteGeneration()` and must not fallback to `requestMaterializationRefresh()`. — **Done**: `rmvpeOverlayLatched_` 删除；`requestReferenceNoteGeneration()` 调用删除；`requestMaterializationRefresh()` fallback 删除
- [x] **MAIN-31**: VST3 ARA automatic OriginalF0 and ARA Read Audio must never enqueue GAME reference-note generation. GAME remains allowed only in Standalone / regular VST3 explicit entry points. — **Done**: ARA-bound Editor 路径不再调用 GAME
- [x] **MAIN-32**: ARA session must remove source-level full-read hydration validation. Host sample reads for this path must happen only for the target source window inside the materialization birth request. — **Done**: hydration worker 重命名为 birth worker；source-level full-read pass 删除
- [x] **MAIN-33**: `PublishedRegionView` must not expose obsolete `copiedAudio` state. Playback/renderability must be expressed by materialization binding state, not copied PCM availability. — **Done**: `copiedAudio` 删除；测试从 copiedAudio 叙事改名为 binding/payload 语义
- [x] **MAIN-34**: ARA-F0 tests must include negative contract guards and behavior evidence for no GAME, no refresh fallback, RMVPE immediate release, and playback via born materialization. Source-shape-only tests are insufficient as final proof. — **Done**: 7 个 `AraFinal_*` 终审守卫（negative guard + path guard）全部 PASS
- [x] **MAIN-35**: VST3 ARA auto-birth owner must be `AudioModification persistentId`, not source. For each persistentId, session may hold at most one current pending birth target, defined by `latest desired SourceWindow + revision`; stale results must be discarded before binding commit, not committed and cleaned later. — **Done 2026-05-27**: `PendingBirth` is keyed by persistentId, carries `SourceWindow + revision`, and worker-result commit rejects stale revisions before binding.
- [x] **MAIN-36**: Any callback path that leaves `regionNeedsMaterializationBirthLocked()` true must upsert that persistentId's single pending birth record. `audioModificationPersistentId` attach alone is sufficient to enqueue. Production code must not keep `projectionChanged || bindingChanged` as a required outer gate for birth scheduling. — **Done 2026-05-27**: birth scheduling is centralized through persistentId upsert, including attach-only/new persistentId and ready-source requeue paths.
- [x] **MAIN-37**: `PluginEditor` is a read-only consumer in this lifecycle. Production code must not call `clearPlaybackRegionMaterialization()` from editor paths because payload/buffer is temporarily unavailable. Missing payload may only surface as pending/restoring UI state. — **Done 2026-05-27**: destructive clear API was removed from `VST3AraSession`; `PluginEditor.cpp` has no `clearPlaybackRegionMaterialization(` hit.
- [x] **MAIN-38**: ARA VST3 full-state restore must have exactly one final landing zone: the post-`didBindToARA()` shared stores. If `setStateInformation()` happens before ARA bind, state may be cached temporarily, but production code must not restore into pre-bind local stores and later mirror/synchronize them. — **Done 2026-05-27**: metadata-only ARA VST3 state is cached pre-bind and replayed after `didBindToARA()`, while regular unbound VST3 state restores immediately; processor tests cover both branches.
- [x] **MAIN-39**: Automated coverage must first demonstrate RED, then GREEN, for: multi-item birth across one source, late-arriving newer birth overriding older in-flight work, editor destroy/recreate restore, destructive-clear prohibition, and pre-bind state-restore ordering. Existing AraFinal/source-shape guards are necessary but not sufficient. — **Done 2026-05-27 automated closure**: focused architecture/processor tests now cover multi-item birth, in-flight newer persistentId, stale window drop, editor missing-payload/reopen behavior, destructive-clear grep guard, and pre-bind restore ordering. Reaper L5 remains an external host-validation gap, not an automated blocker.

### Traceability Addendum

| Source | Responsibility |
|--------|----------------|
| `.planning/plans/2026-05-26-vst3-ara-originalf0-final-convergence.md` | Code-level execution plan for removing remaining ARA-F0 fallback, parallel structure, and dead API residue |
| `.planning/plans/2026-05-26-vst3-ara-originalf0-final-convergence-test-verification.md` | Verification contract for final ARA-F0 convergence, including negative guards, focused suites, builds, and Studio One L5 |
| `.planning/plans/2026-05-26-vst3-ara-multi-item-birth-and-editor-reopen-structural-fix.md` | Structural repair plan for ARA multi-item birth loss, editor reopen recovery, and pre-bind restore ordering |
| `.planning/plans/2026-05-26-vst3-ara-multi-item-birth-and-editor-reopen-test-verification.md` | Verification contract for the ARA multi-item birth and editor-reopen lifecycle fix |

---
## 2026-05-27 Requirement Addendum: Standalone Arrangement Visual Contracts

### Active

- [ ] **MAIN-40**: Standalone Arrangement clip waveform drawing must preserve a positive drawable waveform bounds when
  a visible placement has positive width, including at the minimum horizontal zoom. The fix must address the bounds/tile
  contract, not hide the problem by raising the minimum zoom or only changing waveform alpha/color.
- [ ] **MAIN-41**: Standalone Arrangement cross-track move drag must expose a target-track clip preview before
  `mouseUp()`, while real placement time/track truth remains unchanged until the final release commit. Preview state must
  remain UI-only render-model state and must not enter `StandaloneArrangement`, `OpenTuneAudioProcessor`, undo history,
  project serialization, VST3/ARA state, or audio-thread snapshots.

### Traceability Addendum

| Source | Responsibility |
|--------|----------------|
| `.planning/plans/2026-05-27-arrangement-min-zoom-waveform-and-cross-track-drag-preview.md` | Standalone Arrangement visual contract plan for min-zoom waveform visibility and target-track drag preview |
| `.planning/plans/2026-05-27-arrangement-min-zoom-waveform-and-cross-track-drag-preview-test-verification.md` | Verification contract for Arrangement waveform bounds, UI-only move preview, focused tests, and manual smoke |

---
*Last updated: 2026-05-27 after adding MAIN-40..MAIN-41 Arrangement visual contracts*
---
## 2026-05-27 Requirement Addendum: Experimental Features Gate And TimeTool Anchor Seed

### Validated (2026-05-27 via 706c844)

- [x] **MAIN-42**: Standalone 必须把"实验性功能入口可见性"与 `ExperimentalReferenceAlignMode` 解耦。前者是 shared app-level boolean，用于控制参考轨/TimeTool 入口是否对用户暴露；后者继续只表达 AUTO Ref 后端模式（`Off / Basic / Aggressive`），不得兼任入口开关。
- [x] **MAIN-43**: TimeTool 的所有 Standalone 入口必须统一受实验性功能总开关控制，包括 ParameterPanel toolbar、PianoRoll 工具菜单、快捷切换后的 current-tool 收口。关闭开关时，不得仍能通过隐藏旁路停留在 TimeTool。
- [x] **MAIN-44**: Arrangement clip 右下角参考源入口必须统一受实验性功能总开关控制。draw、hover hit-test、cursor、click/menu 四条路径都必须同时关闭；只藏绘制而保留交互命中不算完成。

### Active (TimeTool anchor seed still pending)

> 2026-05-27 contract correction: the legacy wording in `MAIN-45..47` is no longer the formal `AUTO(REF)` backbone.
> Read these seed-only requirements together with `MAIN-66..70`, which supersede the old `DerivedAnalysis + TimeTool seed`
> product narrative and reduce `TimeTool` seed to timing-feature extraction reuse only.
>
> Interpretation override:
> `MAIN-45` 中出现的 `DerivedAnalysis producer` 现在一律应理解为“正式 timing-feature extraction 单入口”。
> `MAIN-46` 中出现的 `GAME` / `Aggressive` / `DerivedAnalysis` 主链表述现在只保留“禁止新增第二 producer 旁路”这一含义，
> 不再代表正式 `AUTO(REF)` 产品主链，也不再允许把旧 prototype 合同抬回 shared-core 正式语义。

- [ ] **MAIN-45**: 每个 materialization 第一次进入 TimeTool 时，processor 必须负责一次性准备 stretch 锚点来源；若当前仅有默认 identity `TimeGridSnapshot` 且无内部 handles，则应复用正式 timing-feature extraction 单入口生成内部时间锚点并提交新的 identity `TimeGridSnapshot`。这一步只允许播种 handles，不允许自动生成非 identity 时间映射、自动改 clip 时长或自动执行 stretch。
- [ ] **MAIN-46**: TimeTool stretch anchors 的生产必须只有 processor 单一入口。UI、ToolHandler、菜单点击、鼠标事件都不得自行拼 `TimeGridSnapshot`，也不得再造独立 producer 旁路。实验模式或调试模式可以影响分析细节，但不得变成第二套 persisted timing truth 或第二条 seed 主链。
- [ ] **MAIN-47**: 已经存在内部 handles 或已有非 identity stretch 编辑的 materialization，再次进入 TimeTool 时不得重新 seed，不得覆盖用户已有时间拉伸编辑。

### Traceability Addendum

| Source | Responsibility |
|--------|----------------|
| `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed.md` | Standalone 实验性功能总开关、参考源入口门控与 TimeTool 首次 identity 锚点播种执行方案 |
| `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed-test-verification.md` | 上述功能的验证合同，覆盖偏好解耦、入口门控、identity seed、不覆盖既有 stretch 编辑 |

---
*Last updated: 2026-05-27 after adding MAIN-42..MAIN-47 experimental feature gate and TimeTool anchor-seed contracts*

---
## 2026-05-27 Requirement Addendum: Standalone Import Track-Target Drop UX

### Validated (2026-05-27 via 8ea7305 / 706c844)

- [x] **MAIN-48**: Standalone drag-drop import must resolve target track from the actual drop location when the drop lands
  on Arrangement track lanes. `PluginEditor::filesDropped(...)` receiving `x,y` but ignoring them is no longer acceptable as
  product behavior.
- [x] **MAIN-49**: Drag-drop onto Arrangement blank space below the last visible track must create one new visible track and
  import there, unless the editor is already at `MAX_TRACKS`. At the limit, the app must surface a direct limit failure and
  must not reopen the legacy per-drop track picker.
- [x] **MAIN-50**: Drag-drop outside the Arrangement drop surface must keep a deterministic fallback path: import to the
  current active track using explicit editor-owned placement, rather than guessing from unrelated widget geometry.
- [x] **MAIN-51**: Single-file chooser import must remain the fast path: current active track, explicit `ImportPlacement`,
  no track-choice popup. Multi-file chooser import may stay explicit only at the import-mode level, not via per-file or
  per-drop track-choice prompts.
- [x] **MAIN-52**: Import target hover/preview for drag-drop must remain transient Standalone UI state only. It must not
  enter `StandaloneArrangement`, `OpenTuneAudioProcessor`, undo history, playback snapshots, project serialization, or
  VST3/ARA state.
- [x] **MAIN-53**: All real import commits must continue to pass an explicit `ImportPlacement` from Standalone editor code
  into `commitPreparedImportAsPlacement()`. The processor must not infer `trackId` or `startSeconds` from pointer location,
  hover state, Arrangement geometry, or drag-preview state.

### Traceability Addendum

| Source | Responsibility |
|--------|----------------|
| `.planning/plans/2026-05-27-standalone-import-track-target-drop-ux.md` | Standalone import UX contract for track-target drag-drop, blank-area create-track behavior, chooser alignment, and explicit placement ownership |
| `.planning/plans/2026-05-27-standalone-import-track-target-drop-ux-test-verification.md` | Verification contract for import target resolution, transient preview state, focused suites, and manual Standalone smoke |

---
*Last updated: 2026-05-27 after adding MAIN-54..MAIN-65 cumulative feature landing contracts*
---
## 2026-05-27 Requirement Addendum: Cumulative Feature Landing

### Validated (2026-05-27)

- [x] **MAIN-54**: Track-level color system must move colour ownership from per-placement `Placement::colour` to per-track `TrackState::colour`, with `TrackColorMode` enum (`Fixed / Custom / Random`) to support per-track colour assignment. Placement-level `colour` field is removed. — **Done 919c544**
- [x] **MAIN-55**: Keyboard shortcut settings must be promoted from Standalone-only to a shared `AppPreferences` feature, available in both Standalone and VST3 preference pages. A minimum of 22 shortcut entries must be migrated, including tool-switching shortcuts. — **Done 7b9945d**
- [x] **MAIN-56**: Snap settings must be exposed as a typed `SnapSettings` struct in `AppPreferences`, with an associated shared preference page for configuring snap-to-grid behavior. — **Done 8ea7305**
- [x] **MAIN-57**: ARA PendingBirth lifecycle must be revision-based. Each `audioModificationPersistentId` may hold at most one current pending birth target, defined by `SourceWindow + revision`. Worker results with stale revisions are discarded before binding commit, not committed and cleaned later. — **Done c4766c5**
- [x] **MAIN-58**: ArrangementRenderModelCache must use waveform tile caching with proper tile-bounds keys to avoid redundant waveform regeneration during scroll/zoom. — **Done c77d847**
- [x] **MAIN-59**: Arrangement cross-track clip drag must expose a target-track clip preview via UI-only render-model state before mouseUp, while real placement time/track truth remains committed only on final release. — **Done c03fabe**
- [x] **MAIN-60**: Arrangement vertical geometry coordinates must be cached to avoid repeated recalculation during drag operations and scroll invalidation. — **Done c03fabe**

### Traceability Addendum

| Source | Responsibility |
|--------|----------------|
| `919c544` | Track-level color system refactor: `Placement::colour` → `TrackState::colour`, `TrackColorMode` |
| `7b9945d` | Shortcuts promoted from Standalone-only to Shared (22 entries, tool-switching) |
| `8ea7305` | Snap settings (`SnapSettings` + preference page) |
| `c4766c5` | ARA birth lifecycle revision-based `PendingBirth` + state v8 clipInSeconds |
| `c77d847` | Timeline rendering pipeline with model caches, tile cache, playhead dirty-rect optimization |
| `c03fabe` | Non-ARA compilation guards, arrangement drag preview, vertical geometry caching |

---
## 2026-05-27 Requirement Addendum: AUTO(REF) Reference-Driven Pitch And Timing Alignment

### Active

- [ ] **MAIN-66**: `AUTO(REF)` 的正式产品语义必须是 `ClipA -> ClipB` 参考驱动音高+节奏对轨。参考绑定的 persisted truth 仍然只在 `StandaloneArrangement::Placement::referencePlacementId`，`ClipB` 只作为参考，不得被写回。
- [ ] **MAIN-67**: `AUTO(REF)` 的特征合同必须逻辑拆分为 `ReferencePitchFeatures` 与 `ReferenceTimingFeatures`，两者都以 materialization-local source-time 为唯一时间域。正式 production path 不再允许以 `MaterializationStore::DerivedAnalysis` 充当 reference-auto mixed contract。
- [ ] **MAIN-68**: `AUTO(REF)` 必须只依赖 target/reference feature set、placement overlap window，以及 target/reference `EffectiveTimeMap`，而不再要求 target/reference 先有 seeded `TimeGridSnapshot` 或 existing `TimeTool` handles 才能进入 timing alignment。
- [ ] **MAIN-69**: `TimeTool` seed 只允许共享 timing-feature extraction，用于生成 identity handles。它不是 `AUTO(REF)` 的主链，不是 timing alignment 的前置门槛，也不得再被语义化为“先 seed，再 reference align”。
- [ ] **MAIN-70**: project persistence 只允许持久化 `referencePlacementId + notes + correctedSegments + timeGrid` 这些产品真相。`basicAnalysis`、`enhancedAnalysis`、`analysisMode` 必须退出正式 project truth，仅允许 legacy reader ignore/migration path 短期存在。
- [ ] **MAIN-71**: `AUTO(REF)` 的 timing 输出必须正式定义为“把 `ClipA` 的自动锚点拖拽结果写回普通 `ClipA.timeGridAfter`”。它不是 reference-only 黑盒状态，也不是第二套 timing truth；`AUTO(REF)` 前后用户都必须还能在 `TimeTool` 中继续手动拖拽这些锚点。
- [ ] **MAIN-72**: `AUTO(REF)` 自动 timing patch 触及到的每个局部区间都必须满足速度窗口 `0.8 <= speed_i <= 1.3`，其中 `speed_i = (sourceSeconds[i + 1] - sourceSeconds[i]) / (outputSeconds[i + 1] - outputSeconds[i])`。若参考对齐目标超出可行区间，系统必须把结果投影/饱和到最近可行解，只允许有限对齐，不得越界强拉。
- [ ] **MAIN-73**: `0.8x~1.3x` 速度窗口当前只约束 `AUTO(REF)` 自动 patch，不自动改写现有 `TimeTool` 手动拖拽合同。手动拖拽是否也引入同类限速，必须后续单独立项。

### Traceability Addendum

| Source | Responsibility |
|--------|----------------|
| `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment.md` | `AUTO(REF)` reference-driven pitch and timing alignment 正式合同、feature split、effective time map、普通 `timeGridAfter` truth、受约束 auto handle drag 与 persistence 边界 |
| `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment-test-verification.md` | `AUTO(REF)` contract rewrite 的 L0-L6 验证口径、阻断断言、可编辑 `TimeTool` truth、`0.8x~1.3x` 速度窗口与 kill-list 审查 |
