# AUTO(REF) Reference-Driven Pitch And Timing Alignment Test And Verification Plan

**日期:** 2026-05-27  
**状态:** Proposed  
**关联执行方案:** `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment.md`

## 1. 验证目标

本轮验证只服务一个结论: `AUTO(REF)` 已经从“experimental gate + DerivedAnalysis + TimeTool seed”混合原型，
收敛为正式的 `ClipA -> ClipB` 参考驱动音高+节奏对轨能力。

因此必须同时证明:

1. 参考绑定真相仍然只在 `StandaloneArrangement::Placement::referencePlacementId`
2. `AUTO(REF)` 的特征合同已经逻辑拆分为 `pitch features` 与 `timing features`
3. `AUTO(REF)` 不再要求 target/reference 先有 seeded `TimeGrid`
4. `TimeTool` seed 只共享 timing-feature extraction，不再拥有或驱动 `AUTO(REF)` 主链
5. 参考特征只属于 runtime cache，不再作为 project truth 持久化
6. `AUTO(REF)` timing 结果写回的是普通 `ClipA.timeGridAfter`，不是额外 reference-only 状态
7. `AUTO(REF)` 自动 timing patch 触及到的每个区间都满足 `0.8x~1.3x` 局部速度窗口；超限时只能有限对齐，不能越界强拉

## 2. 阻断断言

以下任一成立，都视为本轮失败:

- `AUTOREF-01`: `Source/PluginProcessor.cpp` 仍把 `oldTimeGrid == nullptr || referenceTimeGrid == nullptr` 当作 `AUTO(REF)` 前置条件
- `AUTOREF-02`: `ReferenceAutoAlign::align(...)` 仍要求 target/reference 都必须携带 `TimeGridSnapshot*`
- `AUTOREF-03`: `MaterializationStore::DerivedAnalysis` 仍是 `AUTO(REF)` 的正式 production feature contract
- `AUTOREF-04`: `ProjectMaterializationEntry.basicAnalysis` / `enhancedAnalysis` 仍被写出为 project truth
- `AUTOREF-05`: `ProjectReferenceBinding.analysisMode` 仍被当作正式产品语义持久化
- `AUTOREF-06`: `ensureTimeToolAnchorSeed(...)` 仍直接走 `buildGameReferenceDerivedAnalysis(...)` 或同类专用旁路
- `AUTOREF-07`: `AUTO(REF)` 只有在用户先进入过 `TimeTool` 后才能产生 timing patch
- `AUTOREF-08`: 参考 clip 本身被错误写回，或 target 以外的 materialization 被 mutation
- `AUTOREF-09`: `AUTO(REF)` 自动 patch 触及到的任一区间最终速度落到 `0.8x` 以下或 `1.3x` 以上
- `AUTOREF-10`: 为了追求参考对齐而把 `ClipA.timeGridAfter` 写成普通 `TimeTool` 无法继续手改的专用状态
- `AUTOREF-11`: 参考目标超出速度窗口时仍强行精确命中 `ClipB`，而不是投影/饱和到最近可行解

## 3. L0 规格完整性

人工核对以下文档已经同时存在并互相引用:

1. `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment.md`
2. `.planning/plans/2026-05-27-auto-ref-reference-driven-pitch-and-timing-alignment-test-verification.md`
3. `.planning/PROJECT.md`
4. `.planning/REQUIREMENTS.md`
5. `.planning/ROADMAP.md`
6. `.planning/STATE.md`

若主 `.planning` 仍只记录旧的 `DerivedAnalysis + TimeTool seed` 叙事，则不允许开始实现。

## 4. L1 静态检查

运行:

```powershell
git diff --check
```

预期:

1. Exit code 0
2. 无 patch 格式错误
3. 无尾随空白
4. 新旧规划文档链接路径正确

## 5. L2 架构守护测试

构建:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

运行:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

至少需要新增或重写以下守护:

1. `AutoRefArchitecture_BindingTruthLivesOnPlacementOnly`
2. `AutoRefArchitecture_FeatureContractIsSplitIntoPitchAndTiming`
3. `AutoRefArchitecture_AutoRefDoesNotRequireExistingTimeGridHandles`
4. `AutoRefArchitecture_TimeToolSeedOnlySharesTimingFeatureExtraction`
5. `AutoRefArchitecture_ProjectPersistenceDoesNotWriteReferenceFeatureShadow`
6. `AutoRefArchitecture_GameIsFormalAutoRefProducer`
7. `AutoRefArchitecture_TimeGridAfterRemainsEditableTimeToolTruth`
8. `AutoRefArchitecture_AutoTimingPatchRespectsLocalSpeedWindow`
9. `AutoRefArchitecture_AutoTimingPatchSaturatesInsteadOfOverstretching`

同时必须删除或重写以下旧守护语义:

1. “`DerivedAnalysis` is the only reference truth”
2. “`Aggressive` mode contract is the production AUTO Ref contract”
3. “TimeTool seed must reuse `DerivedAnalysis` main chain as AUTO Ref backbone”
4. “reference analysis mode persists as product truth”

如果旧守护继续要求 `DerivedAnalysis`、`analysisMode`、`Aggressive` 产品模式这些语义，则说明架构测试仍在保护错误目标。

## 6. L3 算法与集成验证

运行:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe core
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
```

最少要覆盖以下事实:

1. `ReferenceAutoAlign` 在 target/reference 都是 identity effective time map 时可直接生成 timing patch
2. `ReferenceAutoAlign` 在 target 已有用户 warp 时，能在受影响范围内编译 reference-auto timing patch，而不是要求先 seed
3. `AUTO(REF)` 只写 target materialization，不写 reference materialization
4. target/reference overlap 仍以 placement timeline intersection 决定，但 patch 落点是 materialization-local source-time
5. 旧工程即使带有 legacy `DerivedAnalysis` / `analysisMode` 节点，读取后也不会把它们恢复为新真相
6. 新写出的工程不再生成 `basicAnalysis` / `enhancedAnalysis` / `analysisMode`
7. `ensureTimeToolAnchorSeed(...)` 只消费 timing features，且生成的是 identity handles，不改变 clip 时长和现有用户 stretch 编辑
8. `TimeGridPatchBuilder` 对 `AUTO(REF)` 自动 patch 触及区间执行最终速度投影，保证 `0.8 <= speed_i <= 1.3`
9. 当参考要求超过速度窗口时，最终结果是受限后的有限对齐，而不是整段失败或越界强拉
10. `AUTO(REF)` 写回后的 `ReferenceAuto` handles 仍可被 `TimeTool` 当作普通 handles 继续编辑

建议直接聚焦以下测试文件:

1. `Tests/TestReferenceAutoAlign.cpp`
2. `Tests/TestAutoRefIntegration.cpp`
3. `Tests/TestAutoRefFailure.cpp`
4. `Tests/TestTimeGridPatchBuilder.cpp`
5. `Tests/TestAutoRefArchitecture.cpp`
6. `Tests/TestBasicDerivedAnalysis.cpp`
7. `Tests/TestMaterializationDerivedAnalysis.cpp`

其中 6/7 不应继续保留为正向合同；应删除或迁移为新 feature-contract 测试。

## 7. L4 Build Matrix

运行:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```

预期:

1. `OpenTuneTests` PASS
2. ARA Standalone / shared code PASS
3. ARA VST3 PASS
4. non-ARA VST3 PASS

说明:

1. 本轮虽然是 Standalone `AUTO(REF)` 产品能力重构，但 feature contract、processor、persistence、tests 都是 shared code
2. 因此不能只编译单一目标

## 8. L5 手动行为验证

只有用户确认后，才能标记 PASS。

### 场景 A: 不先进入 TimeTool，直接执行 AUTO(REF)

步骤:

1. 在 Arrangement 中把 `ClipA` 绑定参考源 `ClipB`
2. 保证 `ClipA` 和 `ClipB` 时间上有明确重叠
3. 不先进入 `TimeTool`
4. 直接在 `ClipA` 上执行 `AUTO(REF)`

预期:

1. `ClipA` 的音符和节奏对轨发生变化
2. `ClipB` 保持不变
3. 不因“未 seed TimeTool”而失败
4. 切回 `TimeTool` 后，可以立刻看到常规锚点可视化；按当前实现口径应为竖向 guide line + 顶部菱形抓手，而不是一套 AUTO(REF) 专用外观

### 场景 B: 先进入 TimeTool，再执行 AUTO(REF)

步骤:

1. 对同类素材先进入 `TimeTool`
2. 不做手动 stretch 编辑，或只做轻微编辑
3. 再执行 `AUTO(REF)`

预期:

1. `AUTO(REF)` 结果与“是否先进入 TimeTool”无主链依赖关系
2. 只允许共享 timing-feature extraction，不允许因为 seed 改变产品语义
3. 若当前只是 identity seed，则 `AUTO(REF)` 后的 timing 结果仍落在普通 `timeGridAfter`，并可继续手动拖拽

### 场景 C: Target 已有用户时间编辑

步骤:

1. 手动给 `ClipA` 做少量 `TimeTool` 拖动
2. 绑定 `ClipB`
3. 再执行 `AUTO(REF)`

预期:

1. 用户 handles 在受影响范围外保留
2. reference-auto handles 只在受影响范围内替换/更新
3. 不出现整段 time grid 被重置为 identity
4. `AUTO(REF)` 完成后仍可继续手动拖拽这些锚点；本轮不要求 TimeTool 手动拖拽也强制服从 `0.8x~1.3x` 限速

### 场景 D: 工程保存与重开

步骤:

1. 执行一次 `AUTO(REF)`
2. 保存工程
3. 关闭并重开工程

预期:

1. reference binding、notes、correctedSegments、timeGrid 正常恢复
2. 不依赖持久化的 reference feature cache 才能显示正确结果

### 场景 E: 参考锚点要求超过局部速度窗口

步骤:

1. 准备一个 `ClipA/ClipB` 组合，使参考锚点要求明显超过 `0.8x~1.3x` 可行区间
2. 在 `ClipA` 上执行 `AUTO(REF)`
3. 进入 `TimeTool` 检查自动生成 handles 的最终位置

预期:

1. `AUTO(REF)` 仍然产生可行结果，而不是整段失败
2. 受影响区间的最终速度仍满足 `0.8 <= speed_i <= 1.3`
3. 自动 handles 只做到有限对齐，不会为了精确命中 `ClipB` 而越界强拉

用户确认口径:

```text
用户旅程 "AUTO(REF) Reference-Driven Pitch And Timing Alignment" 已完成验证。请确认是否符合预期: 符合 / 不符合
```

## 9. L6 回归验证

运行:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe core
```

如本轮实现触及 TimeTool 可视化或 Arrangement 入口，还应补跑:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe piano-roll-f0-visual
```

明确限制:

1. 当前 `ui` runner 仍存在 exit=1 / 无 `[FAIL]` 文本的历史问题
2. 因此本轮不得把 `ui` runner 当作 full-suite PASS 证据

## 10. Kill List 审查

运行:

```powershell
rg -n "DerivedAnalysis|basicAnalysis|enhancedAnalysis|analysisMode|buildGameReferenceDerivedAnalysis|temporalEvents|backendMode" Source Tests
```

人工审查必须确认:

1. 正式 production 路径不再以 `DerivedAnalysis` 作为 AUTO(REF) 合同
2. project persistence 不再写 `basicAnalysis` / `enhancedAnalysis` / `analysisMode`
3. `TimeTool` seed 不再直接以 `buildGameReferenceDerivedAnalysis(...)` 为单独主链
4. 没有出现 old/new 并行 reference feature truth

允许存在的 residual hit 只能是:

1. legacy project-reader ignore path
2. 明确标注为迁移清理中的测试名或注释

## 11. 完成证据

本轮实现完成时必须留下:

1. 新的 plan 与 test-verification 文档
2. 主 `.planning` addendum
3. focused architecture / processor / core 测试结果
4. build matrix 结果
5. `git diff --check` 结果
6. L5 用户确认状态

## 12. 不可接受的结论

以下说法都不算完成:

1. “先保留 `DerivedAnalysis`，以后再拆”
2. “先要求用户进一次 `TimeTool` 再跑 AUTO(REF)”
3. “先把 `analysisMode` 留在工程里，后面再清”
4. “先让 `Aggressive` 继续充当正式产品模式”
5. “reference feature cache 先持久化着，省得重算”

本轮验收只认正式合同收敛，不认过渡态补丁。
