# AUTO(REF) Reference-Driven Pitch And Timing Alignment

**日期:** 2026-05-27
**状态:** Verified / Main Contract Landed
**验证文档:** `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment-test-verification.md`

## 1. Contract Summary

`AUTO(REF)` 的正式含义是：

> 对 target clip 执行 `ClipA -> ClipB` reference-driven pitch + timing alignment。

这条主链不依赖用户先进入 TimeTool，也不依赖 UI 层生成分析或时间网格。正式路线为：

```text
StandaloneArrangement reference binding
-> MaterializationStore feature cache
-> OpenTuneAudioProcessor request assembly
-> ReferenceAutoAlign pure patch
-> TimeGridPatchBuilder compiler
-> atomic commit to notes / correctedSegments / timeGrid
```

## 2. Single Truths

- Reference binding 真值属于 placement 层，只表达 target placement 指向 reference placement。
- Reference feature 真值是 runtime cache，不是 project persistence。
- Pitch 特征由 `ReferencePitchFeatures` 表达。
- Timing 特征由 `ReferenceTimingFeatures` 表达。
- 输入时间映射由 `EffectiveTimeMap` 表达。
- AUTO(REF) timing 输出必须回写普通 `TimeGridSnapshot`，不得建立单独的 reference-only timing state。

## 3. Request Contract

`ReferenceAlignmentRequest` 必须携带：

- target notes / corrected segments。
- target `ReferenceFeatureSet`。
- reference `ReferenceFeatureSet`。
- target effective time map。
- reference effective time map。
- target time grid before patch（可由 processor 在无用户编辑时提供 identity 视图）。
- overlap window 与 placement-local 时间关系。

算法层只消费 request，不读取 UI state，不读取 project preferences，不自行解析 placement binding。

## 4. Processor Ownership

`OpenTuneAudioProcessor` 负责：

- 读取 target placement 的 reference binding。
- 解析 target/reference placement overlap。
- 从 `MaterializationStore` 获取 target/reference 快照。
- 确保 target/reference `ReferenceFeatureSet` 可用。
- 构造 target/reference `EffectiveTimeMap`。
- 调用 `ReferenceAutoAlign::align(...)`。
- 将 patch 一次性提交到 notes、correctedSegments、timeGrid。

UI 只负责触发用户意图和展示结果，不拥有 feature extraction、alignment 或 TimeGrid patch 生成逻辑。

## 5. Timing Compiler

AUTO(REF) 不直接拼接 time handles。算法只产出 timing intents，随后交由 `TimeGridPatchBuilder` 编译为普通
`TimeGridSnapshot`。

`TimeGridPatchBuilder` 是 timing intent -> time grid 的唯一 compiler，并负责：

- 保持端点与 source/output 域约束。
- 对内部 handles 排序、去重、裁剪。
- 应用 0.8x-1.3x 局部速度窗。
- 对不可行 patch 执行投影或回滚到最近可行解。

该速度窗只约束 AUTO(REF) 自动 patch，不改变手动 TimeTool 编辑合同。

## 6. Persistence Contract

Project persistence 只保存正式 project truth：

- target placement 与 reference placement 的 binding。
- notes。
- correctedSegments。
- timeGrid。

Reference feature cache、alignment intermediate、算法 mode 文案、UI exposure state 都不得写入 project truth。

## 7. TimeTool Relationship

TimeTool identity seed 与 AUTO(REF) 共享 timing-feature extraction，但不是 AUTO(REF) 前置步骤。

- AUTO(REF) 可以在用户从未进入 TimeTool 的 clip 上执行。
- TimeTool 首次进入只播种 identity handles，不能触发 reference alignment。
- TimeTool 已有用户编辑时，AUTO(REF) 必须把该 grid 作为 target time context，而不是覆盖它。

## 8. Files Owning This Contract

- `Source/DSP/ReferenceFeatures.h`
- `Source/DSP/ReferenceAutoAlign.h`
- `Source/DSP/ReferenceAutoAlign.cpp`
- `Source/DSP/TimeGridPatchBuilder.h`
- `Source/DSP/TimeGridPatchBuilder.cpp`
- `Source/MaterializationStore.h`
- `Source/MaterializationStore.cpp`
- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- `Source/StandaloneArrangement.h`
- `Source/Utils/ProjectModel.h`
- `Source/Utils/ProjectPersistence.cpp`
- `Tests/TestAutoRefArchitecture.cpp`
- `Tests/TestAutoRefIntegration.cpp`
- `Tests/TestReferenceAutoAlign.cpp`
- `Tests/TestReferenceFeaturesCacheSmoke.cpp`
- `Tests/TestReferenceFeaturesCacheLifecycle.cpp`
- `Tests/TestReferenceAnalysisService.cpp`
- `Tests/TestTimeGridPatchBuilder.cpp`

## 9. Final Kill List

AUTO(REF) 活跃主线不得出现：

- UI-owned feature extraction。
- UI-owned TimeGrid patch generation。
- reference-only timing persistence。
- project-persisted runtime feature cache。
- duplicate alignment path outside `ReferenceAutoAlign`。
- timing handle compiler outside `TimeGridPatchBuilder`。
- hidden bypass path that silently avoids `ReferenceFeatureSet`。
- mode names or analysis objects that imply a second product truth.

## 10. Completion Definition

本合同已满足：

- Reference binding、feature cache、processor request assembly、pure align patch、TimeGrid compiler、atomic commit 的单链路已落地。
- AUTO(REF) 不要求用户预先进入 TimeTool。
- TimeTool seed 与 AUTO(REF) 共享 timing features 但互不充当前置步骤。
- Project persistence 已保持为正式 truth，不写 runtime feature/intermediate。
- Focused verification 已完成，详见配套验证文档。
