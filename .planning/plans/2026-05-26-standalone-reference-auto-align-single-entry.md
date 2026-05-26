# Standalone Reference Auto Align Single Entry Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 Standalone 侧参考源自动对齐收敛为“单一入口、单一真相、单一消费者协议”的正式链路，并在此基础上增加可控的 Basic / GAME 参考特征后端选择。

**Architecture:** `DerivedAnalysis` 是参考特征的唯一真相，`OpenTuneAudioProcessor` 是唯一生产者，`ReferenceAnalysisService` 异步预热与 `executeReferenceAlignmentForPlacement()` 同步兜底必须复用同一个 reference feature builder。旧的 `requestReferenceNoteGeneration()` 不得再作为 AUTO Ref 生产入口存在，避免形成第二条 GAME 旁路。

**Tech Stack:** C++17, JUCE, OpenTuneAudioProcessor, MaterializationStore::DerivedAnalysis, ReferenceAnalysisService, ReferenceAutoAlign, AppPreferences

---

## 1. 背景与问题定义

当前 Standalone AUTO Ref 的正式链路已经很明确：

1. `ReferenceAnalysisService` 为 materialization 异步生成 `DerivedAnalysis`
2. `executeReferenceAlignmentForPlacement()` 读取 target / reference 的 `DerivedAnalysis`
3. `ReferenceAutoAlign::align()` 消费 `basicDerivedNotes` 与 `temporalEvents`
4. processor 统一提交 notes / correctedSegments / time grid patch

但当前仍残留一条历史 GAME 旁路：

1. `requestReferenceNoteGeneration()` 可以触发 note generator
2. 结果目前并不写入 `DerivedAnalysis`
3. 它既不是 AUTO Ref 的正式入口，也不是正式缓存生产者
4. 它保留在 processor / editor 里，只会制造“还有另一条 GAME 链可能可用”的错觉

这正是本轮需要收敛的根因：不是“哪里再补一个触发 GAME 的按钮”，而是把 reference 特征生产权收回到一条正式链路中。

## 2. 目标合同

本轮完成后的合同必须是：

1. AUTO Ref 只有一个参考特征入口：`buildReferenceDerivedAnalysis(...)`
2. 参考特征只有一个真相：`MaterializationStore::DerivedAnalysis`
3. 参考对齐消费者协议不变：`ReferenceAutoAlign` 继续只认 `DerivedAnalysis`
4. Basic / GAME 只是同一生产入口下的后端模式，不得长出第二套提交路径
5. UI 只是选择模式和触发 AUTO Ref，不拥有 reference feature 生产逻辑

## 3. 设计原则

### 3.1 单一入口

所有参考特征生产，无论来自：

- 异步预热
- 同步 cache miss 兜底
- 用户点击 `AUTO(Ref)`

都必须收敛到同一个 processor helper，例如：

```cpp
MaterializationStore::DerivedAnalysis buildReferenceDerivedAnalysis(
    const MaterializationStore::Snapshot& snapshot,
    ExperimentalReferenceAlignMode mode);
```

禁止出现：

- `ReferenceAnalysisService` 走 Basic，sync fallback 走 GAME
- sync fallback 自己拼一份 `DerivedAnalysis`
- `requestReferenceNoteGeneration()` 直接写 notes 或直接驱动 AUTO Ref

### 3.2 单一真相

`DerivedAnalysis` 是唯一正式落点。Reference feature producer 必须一次性产出完整消费者所需结构，而不是：

- notes 写一处
- anchors / temporal events 写另一处
- UI 再临时拼装

本轮默认合同是：对齐链读取什么，producer 就完整写什么。

### 3.3 最小正确科学改动

本轮不是追求“改最少行数”，而是追求：

1. 保留既有消费者协议
2. 清除历史旁路
3. 让后端模式切换发生在唯一正确的生产边界

因此允许为清晰性做必要结构整理，但不允许新增并行链路或临时兼容层。

## 4. 产品行为合同

### 4.1 偏好设置

在 `AppPreferences.shared` 新增：

```cpp
enum class ExperimentalReferenceAlignMode
{
    Off = 0,
    Basic = 1,
    Aggressive = 2
};
```

持久化字段默认 `Off`。

### 4.2 Preferences UI

不新增“推理优先级”页面。

只在现有 `SharedAudioPage` 末尾追加一项：

- 标签：`实验性功能`
- 选项：`关闭` / `自动对齐参考源（基础）` / `自动对齐参考源（激进）`

这是唯一正确的 UI 落点，因为当前产品并不存在独立“推理优先级”标签页。

### 4.3 AUTO 按钮样式

只有在以下两个条件同时满足时，参数面板 AUTO 按钮才进入 `AUTO (Ref)` 变体：

1. `ExperimentalReferenceAlignMode != Off`
2. 当前 placement 绑定了 reference clip

其余情况保持普通 `AUTO`。

已有 `ParameterPanel::setAutoButtonMode(bool hasReference)` 是正确资产，不重新发明按钮形态。

## 5. 后端模式合同

### 5.1 Off

- 不做 reference feature 预热
- 不显示 `AUTO(Ref)` 变体
- 不允许误触发 reference AUTO 链

### 5.2 Basic

- 参考 notes 使用现有 Basic 机制生成
- 参考 temporal events / anchors 使用同一 Basic producer 的标准产物
- 输出统一写入 `DerivedAnalysis`

### 5.3 Aggressive

Aggressive 的产品合同必须写清楚，不能再含糊。

本方案的正式合同是：

1. GAME 负责生成参考 notes（`GameNoteGenerator::generate()` → `basicDerivedNotes`）
2. GAME 负责生成参考对齐所需 anchors / temporal events
3. 两者在同一个 producer 结果中一次性写入 `DerivedAnalysis`

**GAME 产能确认（2026-05-26）：**

GAME 的五步 ONNX 流水线（encoder → segmenter → bd2dur → estimator → walk）中，
segmenter 产出逐帧 `boundaries [1,T] bool`（100fps，timestep=0.01s），这正是
temporal events 所需的时间锚点数据。当前 `INoteGenerator::generate()` 接口仅
返回 `std::vector<Note>`，boundaries 在内部消费后被丢弃，但数据是存在的。

Aggressive 合同的兑现方式：

- Notes：直接使用 `GameNoteGenerator::generate()` 的输出
- Temporal events：从 GAME notes 的 startTime 派生。GAME notes 的 startTime 是
  bd2dur 从 segmenter boundaries 编码后的边界点，每个 voiced segment（presence=true）
  对应一个 note，其 startTime 即为 GAME 识别的分段边界。等价于 GAME boundaries
  的活跃子集，且 GAME 无法给出比自身 note 更细粒度的锚点。

禁止：

- 把 "GAME notes + Basic temporal events" 伪装成 Aggressive 完成态
- 用 BasicReferenceFeatureBuilder 的 temporal events 替代 GAME 产物

## 6. 目标结构

### 6.1 新的正式生产边界

processor 新增唯一 reference feature builder，例如：

```cpp
MaterializationStore::DerivedAnalysis OpenTuneAudioProcessor::buildReferenceDerivedAnalysis(
    const MaterializationStore::Snapshot& snapshot,
    ExperimentalReferenceAlignMode mode) const;
```

内部再根据 mode 分派：

- `buildBasicReferenceDerivedAnalysis(snapshot)`
- `buildGameReferenceDerivedAnalysis(snapshot)`

但对外只有一个正式入口。

### 6.2 异步与同步统一

以下两条路径必须复用同一 helper：

1. `ReferenceAnalysisService` 的异步预热路径
2. `executeReferenceAlignmentForPlacement()` 的同步 cache miss 兜底路径

任何一处自己分叉实现，都算重新引入并行结构。

### 6.3 DerivedAnalysis 写入规则

无论 Basic 还是 Aggressive，producer 都必须完整填充对齐消费者所需字段，至少包括：

- `inputFingerprint`
- `backendMode`
- `basicDerivedNotes`
- `temporalEvents`

必要时可以扩展 `DerivedAnalysis` 字段命名，但不能引入“消费者临时推导第二真相”的结构。

## 7. 残留结构处置

### 7.1 `requestReferenceNoteGeneration()`

本函数不得再承担 AUTO Ref 的生产职责。

本轮有两个允许结果：

1. 如果没有其他正式产品功能依赖它，直接删除
2. 如果仍有显式非 AUTO Ref 功能依赖它，把合同缩窄为“独立 note-generation 功能入口”，并且禁止它写入 AUTO Ref 使用的 `DerivedAnalysis`

推荐优先级：

1. 删除
2. 实在删不掉再收窄

禁止结果：

- 继续让它成为 GAME 参考特征正式入口
- 让它与 `buildReferenceDerivedAnalysis(...)` 并存且都能影响 AUTO Ref

### 7.2 editor / UI 侧逻辑

editor 只负责：

- 读取当前 mode
- 判断是否存在 reference binding
- 更新按钮样式
- 触发 `executeReferenceAlignmentForPlacement()`

不得拥有：

- GAME/Basic producer 选择逻辑
- 参考 notes 组装逻辑
- DerivedAnalysis 写入逻辑

## 8. 需要修改的模块

### Task A: Preferences 合同落地

**Files:**
- Modify: `Source/Utils/AppPreferences.h`
- Modify: `Source/Utils/AppPreferences.cpp`
- Modify: `Source/Editor/Preferences/SharedPreferencePages.h`
- Modify: `Source/Editor/Preferences/SharedPreferencePages.cpp`

**目标：**

- 增加 `ExperimentalReferenceAlignMode`
- 完成 shared preference 持久化
- 在 Audio 页追加“实验性功能”下拉框

### Task B: 统一 reference feature producer

**Files:**
- Modify: `Source/PluginProcessor.h`
- Modify: `Source/PluginProcessor.cpp`
- Modify: `Source/Services/ReferenceAnalysisService.h`
- Modify: `Source/Services/ReferenceAnalysisService.cpp`
- Modify: `Source/DSP/BasicReferenceFeatureBuilder.h`
- Modify: `Source/DSP/BasicReferenceFeatureBuilder.cpp`
- Modify: `Source/MaterializationStore.h`
- Modify: `Source/MaterializationStore.cpp`

**目标：**

- 引入单一 `buildReferenceDerivedAnalysis(...)`
- 异步预热与同步 fallback 全部复用它
- `DerivedAnalysis` 成为唯一正式写入点

### Task C: GAME producer 正式接入或阻断

**Files:**
- Modify: `Source/PluginProcessor.h`
- Modify: `Source/PluginProcessor.cpp`
- Modify: `Source/Utils/NoteGenerator.h`
- Modify: `Source/Utils/NoteGenerator.cpp`
- Modify: `Source/DSP/ReferenceAutoAlign.cpp`

**目标：**

- 确认 GAME 是否能生产完整 `DerivedAnalysis`
- 如果能：以单一 producer 接入
- 如果不能：Aggressive 不上线，文档和 UI 合同都要收紧

注意：

`ReferenceAutoAlign` 的消费者协议原则上不应改；这里只允许核对字段是否满足，不允许为了迁就混合真相去改消费者。

### Task D: UI 只做模式与状态映射

**Files:**
- Modify: `Source/Standalone/PluginEditor.cpp`
- Modify: `Source/Standalone/UI/ParameterPanel.h`
- Modify: `Source/Standalone/UI/ParameterPanel.cpp`

**目标：**

- `AUTO(Ref)` 显示条件收敛到 `mode != Off && hasReferenceBinding`
- 保持既有按钮资产
- 不在 editor 侧增加 reference feature 生产逻辑

### Task E: 清理历史并行结构

**Files:**
- Modify: `Source/PluginProcessor.h`
- Modify: `Source/PluginProcessor.cpp`
- Modify: `Tests/TestMain.cpp`

**目标：**

- 删除或收窄 `requestReferenceNoteGeneration()` 的 AUTO Ref 角色
- 删除相关旧注释、旧日志、旧假入口
- 新增 architecture guard，防止未来再走回旁路

## 9. Kill List

本轮必须主动检查并清理以下坏味道：

1. 任何“Basic 预热 / GAME fallback”双实现
2. 任何“DerivedAnalysis 一部分 + 临时 notes 一部分”的双真相
3. 任何 editor 直接决定 reference notes 生成方式
4. 任何 `requestReferenceNoteGeneration()` 影响 AUTO Ref 正式链路的入口
5. 任何“先上线混合版，再以后再补”的隐性合同偷换

## 10. 完成标准

只有同时满足以下条件，本轮才算完成：

1. AUTO Ref 参考特征正式入口只有一个
2. 参考特征正式真相只有 `DerivedAnalysis`
3. async preheat 与 sync fallback 用的是同一 producer
4. UI 只负责模式映射和触发，不拥有内容生产权
5. `AUTO(Ref)` 只受 mode + reference binding 控制
6. Aggressive 的产品合同明确且真实可兑现
7. 历史 GAME 旁路不再能以任何形式影响 AUTO Ref

## 11. 非目标

本轮不做：

1. 重新设计 `ReferenceAutoAlign` 消费协议
2. 发明新的 reference cache 类型
3. 在 UI 层做 reference feature 拼装
4. 用兼容层保留旧 GAME 入口与新入口并行
5. 以“最少改动”为理由保留错误结构

## 12. 实施建议

实施顺序建议是：

1. 先写 architecture guard，卡死并行结构回流
2. 再引入 preference 与统一 producer
3. 再决定 Aggressive 是否满足“notes + anchors”完整合同
4. 最后才接 UI 显示与用户触发

这样可以避免先把 UI 做出来，后面再发现 GAME 只能产半套特征，导致合同倒挂。

## 13. 强制执行顺序

本节是给执行子代理的硬约束，不是建议。

### 13.1 总体纪律

1. 必须严格按 Phase 0 -> Phase 1 -> Phase 2 -> Phase 3 -> Phase 4 串行执行
2. 前一阶段未满足“退出条件”，下一阶段禁止开始
3. 每一阶段都必须先写 guard / 测试，再写实现
4. 每一阶段结束都必须做一次 kill-list 审查
5. 任何一步如果发现需要修改本方案合同，必须停下，不得自行改口径

### 13.2 Phase 0: 锁定合同，不准动代码

**目的：**

把本轮合同写死，防止后续执行中偷换成“混合版也先上”。

**允许修改文件：**

- `.planning/plans/2026-05-26-standalone-reference-auto-align-single-entry.md`
- `.planning/plans/2026-05-26-standalone-reference-auto-align-single-entry-test-verification.md`

**步骤：**

1. 逐条核对本方案与 test-verification 的一致性
2. 明确 Aggressive 的合同是否为“GAME notes + GAME anchors”
3. 明确 `requestReferenceNoteGeneration()` 的归宿是“删除”还是“收窄”
4. 在文档中写清阻断条件与不可接受结论

**退出条件：**

1. 主方案与验证文档无自相矛盾
2. Aggressive 合同不再含糊
3. 后续代码实现不需要再猜产品口径

### 13.3 Phase 1: 先补守护，不准做功能

**目的：**

先把“不能绕回旧结构”的护栏写出来。

**允许修改文件：**

- `Tests/TestMain.cpp`
- 必要时只读查看相关 `Source/*` 文件

**禁止：**

- 不准改 `Source/PluginProcessor*`
- 不准改 `ReferenceAnalysisService`
- 不准改 UI
- 不准顺手实现 preference

**步骤：**

1. 写 `ReferenceAutoAlign_RequestReferenceNoteGenerationIsNotProductionAutoRefEntry`
2. 写 `ReferenceAutoAlign_AsyncAndSyncPathsShareSameProducerEntry`
3. 写 `ReferenceAutoAlign_UsesDerivedAnalysisAsOnlyReferenceTruth`
4. 写 `ReferenceAutoAlign_EditorDoesNotOwnReferenceFeatureGeneration`
5. 如果已有 Aggressive 合同，则写 `ReferenceAutoAlign_AggressiveModeContractMatchesImplementation`
6. 运行对应 focused suite，确认这些测试当前失败或至少暴露缺口

**退出条件：**

1. 新 guards 已落地
2. 失败原因准确指向当前结构缺口
3. 尚未引入任何生产代码改动

### 13.4 Phase 2: 先统一 producer，再谈模式

**目的：**

先收入口，再决定 Basic / GAME 的分派细节。

**允许修改文件：**

- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- `Source/Services/ReferenceAnalysisService.h`
- `Source/Services/ReferenceAnalysisService.cpp`
- `Source/MaterializationStore.h`
- `Source/MaterializationStore.cpp`

**禁止：**

- 不准改 UI
- 不准接 preferences
- 不准在这一阶段处理按钮显示
- 不准保留第二个正式 producer 入口

**步骤：**

1. 引入唯一 `buildReferenceDerivedAnalysis(...)`
2. 让异步预热走这个 helper
3. 让同步 fallback 走这个 helper
4. 清理任何旧的“自己拼 DerivedAnalysis”逻辑
5. 跑 Phase 1 的 guards，直到通过

**退出条件：**

1. reference feature 正式入口只剩一个
2. async 与 sync 已复用同一 helper
3. tests 证明旧旁路还没被 UI / service 重新接回去

### 13.5 Phase 3: 决定 Aggressive 是否真实成立

**目的：**

在结构已统一后，单独判断 GAME 是否有资格成为 Aggressive 正式后端。

**允许修改文件：**

- `Source/PluginProcessor.h`
- `Source/PluginProcessor.cpp`
- `Source/Utils/NoteGenerator.h`
- `Source/Utils/NoteGenerator.cpp`
- 与 GAME producer 直接相关的最小文件集合

**禁止：**

- 不准为了让 Aggressive“先亮起来”去改 UI 文案掩盖能力缺口
- 不准把“GAME notes + Basic anchors”伪装成 Aggressive 完成态
- 不准修改 `ReferenceAutoAlign` 消费协议来迁就半实现

**步骤：**

1. 验证 GAME 是否能产出完整 `DerivedAnalysis` 所需字段
2. 如果能，接入 `buildGameReferenceDerivedAnalysis(...)`
3. 如果不能，明确关停 Aggressive 上线，保留 Off + Basic
4. 更新 guards，使合同与实现完全一致

**退出条件：**

1. Aggressive 要么真实可用，要么明确不上线
2. 不存在半实现混合合同
3. `DerivedAnalysis.backendMode` 与真实 producer 一致

### 13.6 Phase 4: 最后接 preferences 与 UI

**目的：**

在结构和模式都稳定后，才接用户入口。

**允许修改文件：**

- `Source/Utils/AppPreferences.h`
- `Source/Utils/AppPreferences.cpp`
- `Source/Editor/Preferences/SharedPreferencePages.h`
- `Source/Editor/Preferences/SharedPreferencePages.cpp`
- `Source/Standalone/PluginEditor.cpp`
- `Source/Standalone/UI/ParameterPanel.h`
- `Source/Standalone/UI/ParameterPanel.cpp`

**禁止：**

- 不准在 UI 里做 producer 选择
- 不准在 UI 里组装 notes / anchors
- 不准通过按钮点击直接接回旧 GAME 入口

**步骤：**

1. 加 `ExperimentalReferenceAlignMode`
2. 做 shared persistence
3. 在 Audio 页追加“实验性功能”
4. 让 `AUTO(Ref)` 只取决于 `mode != Off && hasReferenceBinding`
5. 跑 architecture / processor / build matrix focused suites

**退出条件：**

1. UI 只映射状态，不拥有内容生产权
2. `AUTO(Ref)` 只受合同字段控制
3. 不引入新的并行入口

## 14. 每阶段固定动作

每个阶段结束都必须按这个顺序执行，不得跳过：

1. 运行该阶段绑定的 focused tests
2. 做一次 `git diff --check`
3. 做一次针对本轮关键词的 `rg` kill-list 审查
4. 用一句话说明“本阶段删除了什么旧结构”
5. 用一句话说明“下一阶段仍然禁止碰什么”

## 15. 子代理执行纪律

执行子代理必须遵守以下纪律：

1. 只允许修改当前阶段列出的文件，超出范围必须停下
2. 不允许把多个阶段混在一个补丁里完成
3. 不允许先改实现、后补测试
4. 不允许以“顺手修一下 UI / 注释 / fallback”为理由跨步
5. 不允许新增任何临时兼容层、保底分支或旧新并行路径
6. 不允许静默修改 Aggressive 合同
7. 不允许把 test failure 解释成“先继续实现，最后一起绿”
8. 不允许因为 GAME 不完整就把 Basic anchors 偷混进 Aggressive 而不改合同
9. 不允许保留 `requestReferenceNoteGeneration()` 对 AUTO Ref 的影响面
10. 不允许在未通过本阶段 focused tests 前进入下一阶段

## 16. 主线程审核清单

主线程在接收每个子代理补丁时，必须只问这几件事：

1. 这次改动是否严格局限在当前阶段文件范围内
2. 这次改动是否删除了旧结构，而不是叠了一层新结构
3. 这次改动是否让单一入口更收敛，而不是更分叉
4. 这次改动是否让 `DerivedAnalysis` 更像唯一真相，而不是新增侧写真相
5. 这次改动是否试图偷渡 Aggressive 半实现
6. 这次改动的 focused tests 是否真的跑了，并给出结果

只要任一项答案是否定的，就退回该阶段重做，不允许带病推进。
