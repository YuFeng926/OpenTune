# Requirements: OpenTune Post-v2.2 编辑融合、应用偏好与 Source/Materialization/Placement 真相澄清

**Defined:** 2026-04-20
**Status:** ACTIVE MAINLINE
**Core Value:** 双格式独立编译，零交叉影响
**Detailed plan source:** `docs/plans/2026-04-17-single-source-workspace-{design,tasks}.md`, `docs/plans/2026-04-18-app-preferences-refactor.md`, `docs/plans/2026-04-18-app-preferences-refactor-test-verification.md`, `docs/plans/2026-04-19-interaction-scheme-visual-preferences.md`, `docs/plans/2026-04-19-interaction-scheme-visual-preferences-test-verification.md`, `docs/plans/2026-04-19-mac-standalone-bundle-migration.md`, `docs/plans/2026-04-19-mac-standalone-bundle-test-verification.md`, `docs/plans/2026-04-19-undo-affected-range-migration.md`, `docs/plans/2026-04-19-undo-affected-range-test-verification.md`, `docs/plans/2026-04-20-content-placement-two-truth-refactor.md`, `docs/plans/2026-04-20-content-placement-two-truth-refactor-test-verification.md`, `docs/plans/2026-04-21-content-placement-boundary-repair.md`, `docs/plans/2026-04-21-content-placement-boundary-repair-test-verification.md`, `docs/plans/2026-04-21-source-materialization-placement-projection-clarification.md`

本主线承接 `v2.2` 发版后的继续收敛工作。2026-04-20 曾把 active line 写成 `Content/Placement` 两层真相重构；但 2026-04-21 用户明确澄清：同一份 source 在两个 placement 上必须可独立改 note、某个 ARA region 的编辑不得影响 sibling region、split 后左右两段默认拥有独立 editable materialization。由此，official planning 已改正为：`OpenTuneAudioProcessor` 继续作为 `SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession` 的 runtime shell，`AppPreferences` 与 `AudioEditingScheme` 的 owner 边界保持不变，但 persisted business truth 必须改写为 **`Source + Materialization + Placement`**，`Projection` 只是显式 derived contract。

## Current Mainline Requirements

### 已落地到 Live Tree

- [x] **MAIN-01**: `OpenTuneAudioProcessor` 作为 runtime shell 组合 `SourceStore`、`MaterializationStore`、`StandaloneArrangement` 与 `VST3AraSession`；source identity truth、materialization payload truth、Standalone placement truth、VST3 ARA binding truth 不再混在同一套旧容器里
- [x] **MAIN-02**: VST3 当前仍采用 `single instance -> single active audioSource session` 的单工作区边界；但这不再等价于“同 source 只保留一个 shared editable clip/materialization”
- [x] **MAIN-04**: Standalone `commitPreparedImportAsPlacement()` 必须显式接收 `ImportPlacement`，processor 不再隐式猜测 `trackId` 或 `startSeconds`
- [x] **MAIN-05**: Standalone preset chooser 的 `launchAsync()` 回调统一使用 `SafePointer`，禁止再捕获裸 editor `this`
- [x] **MAIN-06**: 应用级 shared/standalone preferences 统一收敛到 `AppPreferences` typed schema；processor state 不再承接 app-level preference
- [x] **MAIN-07**: Standalone 与 VST3 当前都使用显式 preferences page composition；VST3 只暴露 shared pages，Standalone 组装 `Audio + Shared + Standalone-only`
- [x] **MAIN-08**: `AudioEditingScheme` 当前以显式输入规则驱动 parameter target、auto-tune target 与 parameter panel sync，不再依赖隐藏 scheme owner
- [x] **MAIN-09**: notes-first 的第一轮交互反馈已进入 live tree：`NotesPrimary` 下 hand-draw / line-anchor 后会自动选中受影响 notes；`CorrectedF0Primary` 继续保留 line-anchor segment 优先语义
- [x] **MAIN-12**: `曲线优先编辑` / `音符优先编辑` 现在已经固定为仅有的两套交互方案；hand-draw / line-anchor 的 voiced-only 行为已由 `AudioEditingScheme` 纯规则推导，`showUnvoicedFrames` / `noteNameMode` / `showChunkBoundaries` 已作为 shared app preferences 独立持久化
- [x] **MAIN-13**: undo/redo 当前已从“整条曲线刷新”收敛为 content/placement-aware affected-range 执行结果链；`UndoAction` / `UndoManager` / `OpenTuneAudioProcessor` 返回 immutable result，`PianoRollComponent` 与 Standalone/VST3 editor 消费同一份 result chain，且 `createForCurve`、curve-bound applier、static side-channel、whole-curve fallback 已被移除
- [x] **MAIN-14**: dual-format 仓库下的 mac Standalone 打包已补齐 app-only plist metadata 与 bundle docs 路径，并保持 VST3 target 不受污染；当前剩余 gap 仅是 macOS 实机 bundle inspection 仍需在对应环境执行
- [x] **MAIN-17**: 2026-04-20 的 `Content/Placement` phase 已建立 delete-first plan / verification baseline，并清掉 stale deleted-helper compile-break baseline；但 2026-04-21 起这些文档只保留历史/战术价值，不再是 owner-model source of truth
- [x] **MAIN-18**: Standalone `deleteClip()` 当前已先从“删一个 placement 就误删 shared content”修正到“先删 placement，再回收失去最后一个 placement / ARA 引用的 content”；但按 2026-04-21 澄清，这仍只是走向 `placement -> materialization -> source` 生命周期的中间态

### 当前仍需持续守护 / 收敛

- [ ] **MAIN-10**: `.planning/PROJECT.md`、`.planning/ROADMAP.md`、`.planning/STATE.md` 与 live tree 的真实结构、测试现状、调试策略必须保持同步，后续不再允许 official planning 滞后于 `docs/plans/2026-04-18-*`、`docs/plans/2026-04-19-*` 或源码现实
- [ ] **MAIN-11**: 当前主线的验证口径固定为“`OpenTuneTests` 轻量 smoke suites + 人工旅程 + `AppLogger` / targeted trace”；若后续需要补自动化，只允许新增针对性高信号守护，不恢复旧的 Phase 23-26 guard 家族
- [ ] **MAIN-16**: undo result-chain 的 Standalone / VST3 L5 手工旅程本轮按用户许可暂缓；official planning 必须显式保留这条未完成验证，而不能把它记成 PASS
- [ ] **MAIN-15**: 当前主线仍是 `post-v2.2` 的未编号收敛线；待上述范围冻结后，需要再决定下一个正式 milestone / release boundary
- [ ] **MAIN-19**: shared runtime 必须彻底废弃 `clipId` 同时代表 source/materialization/placement 的混合 owner 语义；正式 persisted owners 改为 `sourceId`、`materializationId`、`placementId`，`Projection` 明确保持为 derived value object
- [ ] **MAIN-20**: 同一份 source 再次出现在新的 Standalone placement 或新的 ARA playback region 上时，默认必须创建新的 editable materialization，而不是复用 existing shared content/workspace clip
- [ ] **MAIN-21**: split 默认必须 birth left/right 两个新 materialization；不得再用 `placement.contentStartSeconds`、切 buffer 窗口或 shared content projection 来模拟“独立可编辑左右半段”
- [ ] **MAIN-22**: PianoRoll 必须只消费 `materializationId + projection`；`Note.startTime/endTime` 与 `CorrectedSegment.startFrame/endFrame` 永远 materialization-local，不允许把 placement 时间直接当 persisted editable truth
- [ ] **MAIN-23**: Standalone playback 与 VST3 ARA mapping 必须按 placement projection 把 timeline block 映射到 materialization-local audio/time；不得继续使用 `AudioSource -> shared content -> sibling reuse` 这类混合 owner 读法
- [ ] **MAIN-24**: delete/reclaim 流程必须从“placement -> materialization -> source”三层生命周期重写；`reclaimUnreferencedContent()` 这类两层回收逻辑不再足够表达产品真相
- [ ] **MAIN-25**: 现有 `OpenTuneTests` 的 L1-L4/L6 绿灯只证明旧 `Content/Placement` 假设下的一部分结构守护仍成立；official planning 必须把它明确记成 partial evidence，而不是 owner-model PASS。L5 手工旅程与 macOS bundle inspection 继续显式保留为未完成 gap

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
| `docs/plans/2026-04-17-single-source-workspace-tasks.md` | post-v2.2 基础结构收敛是如何落地的任务拆解 |
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
| `.planning/codebase/TESTING.md` | 当前 live tree 的 smoke tests、人工验证方式与日志调试口径 |

## Notes

- `v2.2` 仍是最新 shipped milestone；本文件描述的是其后的 active mainline，而不是新的已发版版本号。
- 当前 active mainline 的 live-tree reality 已包含 2026-04-18 app preferences refactor、2026-04-19 landed 的 scheme-managed voiced-only 行为 / shared visual preferences / Standalone-only mac bundle packaging cleanup / undo result-chain 实现，以及 2026-04-20 启动的 `Content/Placement` owner-cleanup 尝试；但 2026-04-21 的用户澄清已经说明：`ContentStore` 这层 live-tree reality 更接近 materialization owner，而不是最终想要共享的 source/content owner。
- 当前主线仍不把队友仓库的 `tracks_` 单体模型、mutable note ref、curve-bound undo、standalone-only build 假设当成回流目标；只迁移其中与当前 owner 边界兼容、且符合 fixed-scheme + shared-preference 合约的正确部分。
- 当前 phase 的完整验证仍未结束：旧 `Content/Placement` phase 自己的 L1-L4/L6 已按旧 verification source 重新执行并 PASS，但这不再等价于 owner-model PASS；现在新的 `Source/Materialization/Placement` guards 也已覆盖 source provenance / lineage、structured merge rejection、merge payload preservation 与 ARA source owner seeding，并重新执行通过。剩余未完成项只剩 L5 Standalone/VST3 手工旅程与 macOS bundle inspection。
- 2026-04-21 reality 补充：ARA session / renderer / VST3 editor 当前公开 contract 已进一步收口到 `AppliedMaterializationProjection` + `bindPlaybackRegionToMaterialization()`；`PublishedRegionView` 已公开 `sourceId`，`recordRequested()` 默认为当前 region birth 新 materialization，不再复用 previous workspace materialization，并会在首次导入前显式 seed 缺失的 `SourceStore` owner。processor refresh / undo / reclaim side 也已切到 `MaterializationRefreshRequest`、materialization-facing getter/setter、`reclaimUnreferencedMaterialization()` / `reclaimUnreferencedSource()`；Piano Roll 当前公开 contract 已切到 `MaterializationTimelineProjection` + `setEditedMaterialization(...)`；split / merge / state serialization 现在都会保留 materialization 的 source provenance window 与 lineage metadata。

---
*Requirements defined: 2026-04-20*
*Last updated: 2026-05-05 after synchronizing .planning docs with live tree*
