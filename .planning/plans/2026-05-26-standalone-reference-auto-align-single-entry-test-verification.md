# Standalone Reference Auto Align Single Entry Test And Verification Plan

**日期:** 2026-05-26  
**状态:** Proposed  
**关联执行方案:** `.planning/plans/2026-05-26-standalone-reference-auto-align-single-entry.md`

## 1. 验证目标

本验证文档只服务一个目的：证明 Standalone 参考自动对齐已经收敛成单一入口、单一真相，而不是“表面可用、内部并行”。

因此，本轮不接受只验证 UI 可见或功能大致可用的结论，必须验证：

1. producer 只有一个正式入口
2. `DerivedAnalysis` 是唯一正式真相
3. async / sync 两条运行时路径复用同一套 backend 选择
4. Aggressive 若上线，则必须满足既定合同，而不是偷换成半实现

## 2. 阻断断言

以下任一项不成立，都视为本轮阻断：

- `AUTO-BRIDGE-01`: AUTO Ref 仍可通过 `requestReferenceNoteGeneration()` 这类历史旁路影响结果
- `AUTO-BRIDGE-02`: `ReferenceAnalysisService` 与同步 fallback 未复用同一 reference producer
- `AUTO-BRIDGE-03`: 参考 notes 与参考 temporal events 来自两套独立真相
- `AUTO-BRIDGE-04`: editor / UI 拥有 reference feature 生产职责
- `AUTO-BRIDGE-05`: Aggressive 在产品文案上承诺“GAME notes + anchors”，实现却只是“GAME notes + Basic anchors”

## 3. L1 静态检查

运行：

```powershell
git diff --check
```

预期：

1. Exit code 0
2. 无 patch 格式错误
3. 无尾随空白

## 4. L2 架构守护测试

构建：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

必须新增或更新以下守护测试：

1. `ReferenceAutoAlign_UsesDerivedAnalysisAsOnlyReferenceTruth`
2. `ReferenceAutoAlign_AsyncAndSyncPathsShareSameProducerEntry`
3. `ReferenceAutoAlign_RequestReferenceNoteGenerationIsNotProductionAutoRefEntry`
4. `ReferenceAutoAlign_EditorDoesNotOwnReferenceFeatureGeneration`
5. `ReferenceAutoAlign_AutoRefButtonRequiresExperimentalModeAndReferenceBinding`
6. `ReferenceAutoAlign_PreferenceModePersistsInSharedAudioSettings`

如果 Aggressive 上线，还必须新增：

7. `ReferenceAutoAlign_AggressiveModeContractMatchesImplementation`

这个测试的要求不是“看见 GAME 被调用”，而是验证：

- 如果产品合同写“GAME notes + anchors”
- 那 `DerivedAnalysis` 中供对齐消费的 notes 与 temporal events 必须都来自 GAME producer

若实现仍是混合后端，该测试必须失败。

## 5. L3 处理器与服务层验证

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
```

最低要求：

1. preference mode 变化会影响 reference analysis backend 选择
2. `preheatReferenceAlignmentFeatures(...)` 与 `executeReferenceAlignmentForPlacement()` 调用的是同一个 producer helper
3. `DerivedAnalysis.backendMode` 能反映实际 producer，而不是 UI 意图值
4. cache hit / stale / miss 三种情况下都不产生第二条提交路径

如果需要，为 `OPENTUNE_TEST_BUILD` 增加只读 probe：

- 允许记录“本次 DerivedAnalysis 由哪个 producer 生成”
- 不允许引入新的运行时兼容层或 fallback

## 6. L4 Build Matrix

运行：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```

预期：

1. 测试目标 PASS
2. Standalone PASS
3. non-ARA VST3 PASS

本轮是 Standalone 主体改动，但 shared processor / preference / services 会波及 non-ARA 构建，因此不能只编 Standalone。

## 7. L5 手动行为验证

只有用户确认后，才能标记 PASS。

### 场景 A: 默认关闭

步骤：

1. 打开 Standalone
2. 导入含 reference binding 的 placement
3. 确认“实验性功能”默认为 `关闭`
4. 确认参数面板显示普通 `AUTO`
5. 确认不会预热 reference derived analysis

预期：

1. 不显示 `AUTO(Ref)` 变体
2. 不出现 reference AUTO 行为误触发

### 场景 B: Basic

步骤：

1. 将“实验性功能”设为 `自动对齐参考源（基础）`
2. 绑定 reference clip
3. 确认按钮切换到 `AUTO(Ref)`
4. 触发 AUTO Ref
5. 观察结果与日志

预期：

1. `DerivedAnalysis` 被正式生成并缓存
2. 参考 notes 与 anchors 来自 Basic producer
3. 不经过 `requestReferenceNoteGeneration()` 旧入口

### 场景 C: Aggressive

步骤：

1. 将“实验性功能”设为 `自动对齐参考源（激进）`
2. 绑定 reference clip
3. 触发 AUTO Ref
4. 观察结果与日志

预期：

1. 若 GAME 完整可用，则 `DerivedAnalysis` 由 GAME producer 生成
2. 若产品合同承诺 anchors 也来自 GAME，则需确认 temporal events 同样来自 GAME
3. 若 GAME 不可用或合同未兑现，Aggressive 不得伪装成功

用户确认口径：

```text
用户旅程 “Standalone Reference Auto Align Single Entry” 已完成验证。请确认是否符合预期：符合 / 不符合
```

## 8. Kill List 审查

执行：

```powershell
rg -n "requestReferenceNoteGeneration\\(|buildReferenceAlignmentFeaturesForJob\\(|preheatReferenceAlignmentFeatures\\(|executeReferenceAlignmentForPlacement\\(|setAutoButtonMode\\(|ExperimentalReferenceAlignMode" Source Tests
```

人工审查必须确认：

1. `requestReferenceNoteGeneration()` 不再是 AUTO Ref 正式入口
2. 生产入口收敛到一个 helper，而不是多个函数各自产生 `DerivedAnalysis`
3. `AUTO(Ref)` 只取决于 mode 与 reference binding
4. 不存在“UI 侧拼 notes / service 侧拼 anchors”的双真相

## 9. 完成证据

本轮完成时必须留下：

1. 新旧入口关系说明
2. 新增或更新的 architecture guards 清单
3. focused test suite 结果
4. build matrix 结果
5. L5 手动验证状态

如果 Aggressive 因合同未满足而不上线，也必须把这件事明确记为：

1. 产品决策
2. 技术原因
3. 非回归，不是遗漏

## 10. 不可接受的结论

以下说法都不算通过：

1. “功能能跑，结构以后再收”
2. “Aggressive 先混着用，后面再把 anchors 补成 GAME”
3. “旧的 `requestReferenceNoteGeneration()` 先留着，不影响现在”
4. “只要按钮显示成 `AUTO(Ref)` 就算打通”

本轮验收只认结构真实收敛，不认表面可用。

## 11. 分阶段验收门禁

本节用于限制执行子代理不得跳步报完成。

### Gate 0: 文档合同阶段

必须证明：

1. 主方案与 test-verification 一致
2. Aggressive 合同已明确
3. `requestReferenceNoteGeneration()` 的处置策略已明确

没有这三项，不允许进入任何代码实现。

### Gate 1: 守护测试阶段

必须提交：

1. 新增/更新的 architecture guards 名单
2. focused suite 运行命令
3. 当前失败或暴露缺口的证据

没有“先失败的证据”，不允许声称完成了测试先行。

### Gate 2: producer 收口阶段

必须提交：

1. `buildReferenceDerivedAnalysis(...)` 已成为唯一正式入口的证据
2. async 与 sync 复用同一 helper 的证据
3. `requestReferenceNoteGeneration()` 未影响 AUTO Ref 的证据

没有这三项，不允许进入 Aggressive 或 UI 阶段。

### Gate 3: Aggressive 判定阶段

二选一：

1. 证明 GAME 已完整提供 notes + temporal events，并通过合同测试
2. 明确记录 Aggressive 暂不上线，并通过收紧后的合同测试

GAME 产能确认：segmenter 产出 boundaries [1,T] bool（100fps），temporal events
可从 GAME notes 的 startTime 派生（startTime 是 bd2dur 从 boundaries 编码后的
边界点，等价于 GAME boundaries 的活跃子集）。Aggressive 合同：
- Notes 来自 GameNoteGenerator::generate()
- Temporal events 来自 GAME notes 的 startTime → sourceSeconds
- 不得混入 BasicReferenceFeatureBuilder 的 temporal events

不允许存在第三种状态，即“UI 上给了 Aggressive，但实现是混合版”。

### Gate 4: preferences / UI 阶段

必须提交：

1. `ExperimentalReferenceAlignMode` persistence 证据
2. `AUTO(Ref)` 仅由 mode + reference binding 控制的证据
3. UI 没有 reference feature 生产逻辑的证据

没有这三项，不允许声称用户入口已完成。

## 12. 子代理回报格式

每个执行子代理在阶段结束时，必须按以下格式回报，不得只给泛泛总结：

1. `阶段:` 当前 Gate 编号
2. `修改文件:` 精确列表
3. `新增/更新测试:` 精确测试名
4. `运行命令:` 实际执行过的命令
5. `结果:` PASS / FAIL
6. `删除的旧结构:` 一句话
7. `仍禁止进入的区域:` 一句话

如果缺任一项，主线程应视为未完成。

## 13. 强制 Kill-List

每个阶段结束都必须人工审查以下问题：

1. 是否新增了第二个 producer helper
2. 是否保留了旧 GAME 入口对 AUTO Ref 的影响
3. 是否出现 UI 侧拼装 reference features
4. 是否出现 Basic / GAME 混合真相
5. 是否出现“先保留 fallback 以防万一”

任一项为“是”，该阶段验收失败。
