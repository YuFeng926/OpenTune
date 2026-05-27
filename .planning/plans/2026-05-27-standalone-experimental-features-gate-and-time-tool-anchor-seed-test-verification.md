# Standalone Experimental Features Gate And Time Tool Anchor Seed Test And Verification Plan

**日期:** 2026-05-27  
**状态:** Proposed  
**关联执行方案:** `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed.md`

## 1. 验证目标

本轮验证只服务一个目的：证明 Standalone 实验功能曝光与 TimeTool 首次锚点播种已经收敛为单一合同，而不是“按钮藏了一半、内部仍然并行”。

因此必须同时验证：

1. 实验功能总开关与 AUTO Ref 模式已经解耦
2. TimeTool 与 Arrangement 参考源入口都受同一总开关控制
3. TimeTool 首次进入只播种 identity anchors，不自动产生 stretch 结果
4. stretch anchors 的生产只有 processor 单一入口

## 2. 阻断断言

以下任一成立，都视为本轮阻断：

- `EXP-GATE-01`: `ExperimentalReferenceAlignMode` 仍兼作“实验功能总开关”
- `EXP-GATE-02`: TimeTool 只在 toolbar 隐藏，但 menu / shortcut / current-tool 状态仍可漏进
- `EXP-GATE-03`: Arrangement 参考源按钮只隐藏绘制，但 hover / cursor / click / menu 仍可达
- `EXP-GATE-04`: 首次进入 TimeTool 会直接生成非 identity `TimeGridSnapshot`
- `EXP-GATE-05`: TimeTool seed 逻辑在 UI 层组装 `TimeGridSnapshot`
- `EXP-GATE-06`: 为 stretch anchors 新增了独立 GAME 旁路，没有复用正式 `DerivedAnalysis` producer
- `EXP-GATE-07`: 再次进入 TimeTool 会覆盖已有 stretch 编辑

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

至少新增或更新以下守护：

1. `ExperimentalFeaturesGate_AppPreferenceIsIndependentFromReferenceAlignMode`
2. `ExperimentalFeaturesGate_TimeToolVisibilityRequiresExperimentalFeaturesEnabled`
3. `ExperimentalFeaturesGate_ReferenceButtonVisibilityRequiresExperimentalFeaturesEnabled`
4. `TimeToolAnchorSeed_FirstEntrySeedsIdentityHandlesOnly`
5. `TimeToolAnchorSeed_ReentryDoesNotOverwriteExistingTimeGridEdits`
6. `TimeToolAnchorSeed_UsesProcessorOwnedSingleEntry`

若测试命名需贴近现有风格，可调整命名，但合同不可削弱。

## 5. L3 Processor / Store Focused 验证

运行：

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe core
```

最低要求：

1. `experimentalFeaturesEnabled` 的持久化值可正确读回
2. `ensureTimeToolAnchorSeed(...)` 在已有内部 handles 时 no-op
3. `ensureTimeToolAnchorSeed(...)` 在默认 identity grid 时会创建内部 handles
4. 播种后的所有内部 handles 满足 `source_seconds == output_seconds`
5. seed 过程不会改写 clip duration、不会生成非 identity tau 映射

必要时允许增加测试专用只读 probe，但禁止引入运行时兼容层。

## 6. L4 Build Matrix

运行：

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```

预期：

1. `OpenTuneTests` PASS
2. Standalone PASS
3. non-ARA VST3 PASS

说明：

1. 本轮主改动在 Standalone UI，但 `AppPreferences` 与 processor/store 是 shared 代码
2. 因此不能只编 Standalone

## 7. L5 手动行为验证

只有用户确认后，才能标记 PASS。

### 场景 A: 默认关闭

步骤：

1. 打开 Standalone
2. 进入 shared preferences
3. 确认“启用实验性功能”默认关闭
4. 观察 ParameterPanel 与 Arrangement clip

预期：

1. 不显示 TimeTool
2. clip 右下角不显示参考源入口
3. 不应再通过菜单直接看见 TimeTool

### 场景 B: 打开实验性功能

步骤：

1. 打开“启用实验性功能”
2. 返回主界面
3. 观察 ParameterPanel 与 Arrangement clip

预期：

1. TimeTool 出现在工具栏
2. PianoRoll 工具菜单出现 TimeTool
3. clip 右下角出现参考源入口
4. hover / cursor / click 行为完整恢复

### 场景 C: 首次进入 TimeTool

步骤：

1. 选择一个尚未做过 stretch 编辑的 clip
2. 首次切换到 TimeTool
3. 观察 TimeTool handles 与播放结果

预期：

1. 会准备出内部锚点来源
2. 但当前声音与时间长度不应自动变化
3. handles 初始处于 identity 位置

### 场景 D: 二次进入 TimeTool

步骤：

1. 先手动拖动一个 TimeTool handle，形成明确 stretch 编辑
2. 切回其它工具
3. 再次进入 TimeTool

预期：

1. 原有 stretch 编辑仍保留
2. 不会被重新 seed 覆盖
3. 不会再次回到 identity

用户确认口径：

```text
用户旅程 “Standalone Experimental Features Gate And Time Tool Anchor Seed” 已完成验证。请确认是否符合预期：符合 / 不符合
```

## 8. Kill List 审查

执行：

```powershell
rg -n "ExperimentalReferenceAlignMode|experimentalFeaturesEnabled|TimeTool|ensureTimeToolAnchorSeed|referenceButtonClicked|referencePlacementId|temporalEvents|TimeGridSnapshot" Source Tests
```

人工审查必须确认：

1. 实验功能总开关与 AUTO Ref mode 是两个独立字段
2. TimeTool 门控不只存在于 toolbar
3. Arrangement 门控不只存在于 draw path
4. seed 逻辑不在 UI 层造 grid
5. 没有新长出来的独立 GAME stretch producer

## 9. 完成证据

本轮完成时必须留下：

1. 新增/更新的 plan 与 verification docs
2. 新增/更新的 focused architecture / processor guards
3. `git diff --check` 结果
4. focused suites 结果
5. build matrix 结果
6. L5 手工状态

## 10. 不可接受的结论

以下说法都不算通过：

1. “按钮先藏住就行，内部以后再收”
2. “第一次进入 TimeTool 自动拉一下没关系”
3. “先让 UI 自己拼 handles，后面再下沉 processor”
4. “Arrangement 按钮看不见就算关掉了”
5. “只要 GAME 有跑过一次就算完成”

本轮验收只认合同真实收敛，不认表面勉强可用。

## 11. 分阶段门禁

### Gate 0: 规划合同阶段

必须明确：

1. 总开关与 AUTO Ref mode 解耦
2. TimeTool 首次进入只播种 identity anchors
3. seed producer 只有 processor 单一入口

没有这三项，不允许进入实现。

### Gate 1: UI 门控阶段

必须提交：

1. TimeTool toolbar/menu/tool-state 门控证据
2. Arrangement draw/hover/cursor/click 门控证据

没有“整组入口都被门控”的证据，不允许报 UI 完成。

### Gate 2: Seed 合同阶段

必须提交：

1. 首次进入会 seed 的证据
2. seed 后仍是 identity grid 的证据
3. 二次进入不覆盖的证据

少任何一项，都不允许声称 stretch anchors 已修复。

### Gate 3: Shared Build 阶段

必须提交：

1. tests build PASS
2. Standalone build PASS
3. non-ARA VST3 build PASS

否则不允许把这轮共享改动标成完成。
