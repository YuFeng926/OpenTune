# Requirements: OpenTune Post-v1.3.2 主线

**Status:** ACTIVE MAINLINE
**Core Value:** 双格式独立编译，零交叉影响
**Current version:** v1.5 开发中

**架构主张:** `OpenTuneAudioProcessor` 作为
`SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession` 的 runtime shell。三条 persisted
truth 为 **Source + Materialization + Placement**，`Projection` 只做 derived contract。

## 已落地到 Live Tree（压缩列表，详情见 PROJECT.md）

- **MAIN-01**: OpenTuneAudioProcessor 四段职责分工 - 落地。
- **MAIN-04**: Standalone import 显式 ImportPlacement - 落地。
- **MAIN-05**: SafePointer 异步回调 - 落地。
- **MAIN-06/07**: AppPreferences 收敛 + 显式 page composition - 落地。
- **MAIN-08/09**: AudioEditingScheme 显式规则 + NotesPrimary 交互反馈 - 落地。
- **MAIN-12**: 两套固定交互方案 + scheme 推导 voiced-only + shared visual prefs - 落地。
- **MAIN-13**: Undo/Redo content/placement-aware result chain - 落地。
- **MAIN-14**: Mac Standalone-only 打包 - 落地。
- **MAIN-26**: ARA AudioModification persistentID 绑定 materialization - 落地。
- **MAIN-27**: Studio One stopped render gate - 落地。
- **MAIN-28**: ARA-capable VST3 运行时分流 - 落地。
- **MAIN-29-39**: VST3 ARA OriginalF0 minimal chain 最终收口 - 落地。
- **MAIN-42-44**: Experimental Features Gate - 落地。
- **MAIN-45-47**: TimeTool 首次入场 identity anchor seed - 落地并验证。
- **MAIN-48-53**: Standalone Import Track-Target Drop UX - 落地。
- **MAIN-54-60**: Track color system, Shortcuts, Snap, ARA PendingBirth revision, Arrangement cache - 落地。
- **MAIN-66-73**: AUTO(REF) reference-driven pitch and timing alignment - 落地并验证。

## AUTO Ref / TimeTool 已验证合同

- **MAIN-45**: TimeTool seed 由 processor 单入口负责，UI 只触发工具切换和展示。
- **MAIN-46**: TimeTool 首次入场只复用 `ReferenceTimingFeatures` 生成 identity handles，不自动改变时间映射。
- **MAIN-47**: 已存在内部 handles 或已有 stretch 编辑时，TimeTool re-entry 不覆盖用户数据。
- **MAIN-66**: AUTO(REF) 是 `ClipA -> ClipB` 参考驱动音高 + 节奏对齐。
- **MAIN-67**: 参考特征合同拆为 `ReferencePitchFeatures` 与 `ReferenceTimingFeatures`。
- **MAIN-68**: AUTO(REF) 依赖 `EffectiveTimeMap` 表达输入时间映射，不要求用户预先进入 TimeTool。
- **MAIN-69**: TimeTool seed 只共享 timing-feature extraction，不是 AUTO(REF) 前置步骤。
- **MAIN-70**: persistence 只保留正式 project truth：reference binding、notes、correctedSegments、timeGrid。
- **MAIN-71**: timing 输出编译为普通 `TimeGridSnapshot`，由常规 time-grid 真值承载。
- **MAIN-72**: AUTO(REF) automatic patch 受 0.8x-1.3x 局部速度窗约束。
- **MAIN-73**: 速度窗只约束 AUTO(REF) automatic patch，不改变手动 TimeTool 合同。

## 当前仍需持续守护 / 收敛

- [ ] **MAIN-10**: `.planning` 文档与 live tree 保持同步。
- [ ] **MAIN-16**: undo result-chain 的 Standalone/VST3 L5 手工旅程 - deferred。
- [ ] **MAIN-40/41**: Standalone Arrangement min-zoom waveform + cross-track drag preview - planned。
- [ ] Undo/Redo 边界测试（空栈、redo 裁剪、500 层溢出）。
- [ ] CorrectionWorker 取消/覆盖并发语义验证。
- [ ] `OpenTuneTests.exe ui` runner exit=1 解释修复。
- [ ] L5 手工旅程（Standalone/VST3 undo、宿主验证）。
- [ ] macOS bundle inspection。

## Out of Scope

| Feature | Reason |
|---------|--------|
| Melodyne 式多 source 同时编辑 UI | 当前产品阶段只支持单实例单 active workspace |
| 同 source 切换 region 时 detached clip churn | 破坏单工作区语义 |
| commitPreparedImportAsPlacement() 隐式决定 placement | placement 是显式 UI 语义 |
| 回调裸 `editor this` | 异步生命周期风险 |
| app-level preferences 进入 processor/project state | 生命周期不同 |
| 重新引入 AudioEditingSchemeManager/bool-flag dialog/hidden mutable owner | 与显式 state carrier 冲突 |
| voicedOnlyEditing 作为独立 preference | scheme-derived interaction policy |
| 旧分析字段作为 project truth | 已退出正式合同 |
| 兼容 getter/retry/createForCurve/并行新旧结构/兜底路径 | 与唯一正确结构冲突 |

---

*Last updated: 2026-05-28 - AUTO Ref 与 TimeTool seed 主合同已落地并完成 focused verification。*
