# Standalone Experimental Features Gate And Time Tool Anchor Seed Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在 Standalone 侧为“参考轨 / 时间伸缩工具”增加单一的实验性功能总开关，明确当前能力仍不完善；同时修复 TimeTool 首次进入当前 clip 时不再触发 GAME 锚点来源准备的问题，但这次准备只允许播种 identity 锚点，不允许偷偷改动时间拉伸结果。

**Architecture:** `AppPreferences` 持有“实验性功能可见性”这一唯一 app-level truth；`ExperimentalReferenceAlignMode` 继续只表达 AUTO Ref 后端模式，禁止一字段双含义。TimeTool 的首次入场锚点准备必须下沉到 `OpenTuneAudioProcessor` 的单一入口，由 processor 复用既有 `DerivedAnalysis` 生产链并负责把 GAME/Basic 导出的节奏事件编译成 **identity `TimeGridSnapshot`**。UI 只负责展示、切换工具与触发该入口，不拥有锚点生产逻辑。

**Tech Stack:** C++17, JUCE, AppPreferences, SharedPreferencePages, ParameterPanel, PianoRollComponent, ArrangementViewComponent, PluginEditor, OpenTuneAudioProcessor, MaterializationStore::DerivedAnalysis, TimeGridSnapshot

---

## 1. 背景与问题定义

当前代码里已经存在一套“实验性功能”偏好，但它的真实含义是：

1. `ExperimentalReferenceAlignMode::Off / Basic / Aggressive`
2. 只服务 AUTO Ref 的参考特征后端模式选择
3. 并不是“整组实验功能入口是否对用户可见”的总开关

与此同时，当前 live tree 又已经把以下能力直接暴露出来：

1. TimeTool 按钮在工具栏始终可见
2. PianoRoll 的工具菜单始终包含 Time Tool
3. Arrangement clip 右下角参考源按钮/菜单始终可见

这导致产品层出现两个问题：

1. AUTO Ref 仍有 bug 时，用户仍会直接看到一整组未成熟能力
2. “实验性功能”这一文案被错误复用成 AUTO Ref 模式，而不是 UI 曝光总开关

此外，TimeTool 的当前入场链路也有一处结构性退化：

1. `setCurrentTool(TimeTool)` 现在只切换工具状态
2. 它不再触发任何 GAME / DerivedAnalysis / TimeGrid 预热
3. `TimeTool` 鼠标交互只消费现有 `TimeGridSnapshot`
4. `MaterializationStore` 默认给每个 materialization 提供 identity grid
5. 所以用户第一次进入 TimeTool 时，往往只有“空白 identity 网格”，没有可拖动的内部锚点来源

因此本轮不是“补一个按钮事件”，而是要同时收敛两个合同：

1. **实验功能曝光合同**
2. **TimeTool 首次入场锚点播种合同**

## 2. 本轮目标合同

本轮完成后的正式合同必须是：

1. “启用实验性功能”是一个单独的 app-level boolean，总管参考轨相关 UI 与 TimeTool UI 的可见性
2. `ExperimentalReferenceAlignMode` 继续只描述 AUTO Ref 后端模式，不再承担“实验功能是否显示”的职责
3. TimeTool 只有在实验性功能开关开启时才会进入任何 Standalone UI
4. Arrangement clip 右下角参考源按钮、hover、cursor、点击入口，只有在实验性功能开关开启时才存在
5. 每个 clip 第一次进入 TimeTool 时，只触发一次“锚点来源准备”
6. 这次准备只能生成 identity 内部 handles，不能在用户未拖拽前偷偷改变时间映射、播放结果或 clip 长度
7. 锚点来源准备必须复用既有 `DerivedAnalysis` 正式生产链，禁止再长出第二条 GAME 旁路

## 3. 设计原则

### 3.1 两个偏好，两个职责

必须明确拆开：

1. `experimentalFeaturesEnabled`
2. `experimentalReferenceAlignMode`

前者负责：

1. 是否展示 TimeTool
2. 是否展示参考源按钮/菜单
3. 是否向用户暴露“实验性能力仍不完善”的提示

后者负责：

1. AUTO Ref 关闭
2. AUTO Ref Basic
3. AUTO Ref Aggressive / GAME

禁止结果：

1. 用 `ExperimentalReferenceAlignMode != Off` 伪装实验功能总开关
2. 用实验功能总开关去偷改 AUTO Ref 后端模式
3. 一个字段同时表示“可见性”和“算法模式”

### 3.2 TimeTool 只播种锚点，不自动拉伸

首次进入 TimeTool 的正确行为不是：

1. 自动执行参考对齐
2. 自动修改 `output_seconds`
3. 自动生成非 identity 的 `TimeGridSnapshot`

而是：

1. 若当前 materialization 尚无 TimeTool 可用内部锚点，则准备一次 `DerivedAnalysis`
2. 从其中的节奏事件/边界事件推导内部 handles
3. 生成一个 **identity** `TimeGridSnapshot`
4. 其中每个 handle 初始满足 `source_seconds == output_seconds`
5. 端点继续锁定，内部 handle 仅作为用户后续拖拽的锚点来源

这保证：

1. 首次进入 TimeTool 不改听感
2. 首次进入 TimeTool 不改 clip 时长
3. 伸缩行为只在用户真实拖拽后出现

### 3.3 单一生产入口

TimeTool 首次入场的锚点准备，必须复用 processor 既有 reference-analysis / derived-analysis 正式链路。

允许新增一个 processor 单一入口，例如：

```cpp
void ensureTimeToolAnchorSeed(uint64_t materializationId);
```

但它内部必须遵循：

1. 读取 materialization snapshot
2. 判断当前 `TimeGridSnapshot` 是否已经有内部 handles 或已有非 identity 编辑
3. 需要时复用 `buildReferenceDerivedAnalysis(...)` / `preheatReferenceAlignmentFeatures(...)` 的正式 producer
4. 再把 `DerivedAnalysis.temporalEvents` 编译成 identity handles
5. 最终只由 processor / store 提交 `TimeGridSnapshot`

禁止：

1. UI 自己造 `TimeGridSnapshot`
2. `PianoRollToolHandler` 自己缓存“是否已播种”
3. 再造一条 `requestGameAnchorGeneration()` 之类的平行链

## 4. 产品行为合同

### 4.1 Shared Preferences UI

在现有声码器模型设置下方追加一个新的 shared 开关区块：

1. 控件：`启用实验性功能`
2. 说明文案：参考轨、伸缩工具等能力仍不完善，当前版本可能存在 bug
3. 作用域：shared app preference（Standalone / VST3 都持久化同一值，但当前实际只影响 Standalone 暴露的这些入口）

现有 AUTO Ref 模式控件保持独立语义：

1. 仍可保留 `关闭 / 自动对齐参考源（基础） / 自动对齐参考源（激进）`
2. 但其文案必须只解释 AUTO Ref 模式，不再冒充“实验性功能总开关”

推荐 UI 顺序：

1. 声码器模型下拉
2. `启用实验性功能` 开关
3. bug/未完善提示文字
4. AUTO Ref 模式选择（可视项目可按是否启用实验性功能决定显示/禁用，但语义必须独立）

### 4.2 TimeTool 显示合同

当且仅当 `experimentalFeaturesEnabled == true` 时：

1. `ParameterPanel` 工具栏显示 TimeTool 图标
2. PianoRoll 的工具菜单显示 Time Tool 项
3. 与 TimeTool 相关的快捷切换路径允许选中该工具

当开关关闭时：

1. 工具栏中不渲染 TimeTool
2. 工具菜单中不出现 TimeTool
3. 若当前工具恰好是 TimeTool，关闭开关后必须回退到一个稳定非实验工具（推荐 `ToolId::Select`）

### 4.3 Arrangement 参考源入口合同

当且仅当 `experimentalFeaturesEnabled == true` 时：

1. clip 右下角参考源按钮可绘制
2. hover 命中可生效
3. 指针可变为 pointing hand
4. 点击后才允许弹出参考源菜单

关闭时：

1. 不画按钮
2. 不允许 hover 命中
3. 不改 cursor
4. 不响应点击
5. `PluginEditor` 中的参考源菜单构建入口也应视为不可达

### 4.4 TimeTool 首次入场行为合同

对“每个 clip 第一次点开 TimeTool”的正式语义定义为：

1. 当前编辑对象有确定的 `materializationId`
2. 用户从非 TimeTool 切换进入 TimeTool
3. 若当前 materialization 尚未具备 TimeTool 可用内部锚点，则触发一次锚点播种

这里的“尚未具备”至少包括：

1. `TimeGridSnapshot` 只有端点或仍是纯 identity 默认态，且没有内部 handles
2. 没有已经存在的用户编辑 stretch handles

一旦已有内部 handles 或已有非 identity 编辑：

1. 再次进入 TimeTool 不得重复覆盖
2. 不得重新 GAME
3. 不得抹掉用户已有 stretch 调整

## 5. 当前代码现状与根因

### 5.1 偏好层

当前 `AppPreferences` 已有：

1. `ExperimentalReferenceAlignMode`
2. `shared.align.experimental` 持久化字段
3. SharedAudioPage 中的“实验性功能”下拉框

这套结构当前只表达 AUTO Ref 模式，不能直接承担本轮总开关职责。

### 5.2 TimeTool 展示层

当前 `ParameterPanel` 无条件创建 TimeTool button，`PianoRollComponent` 的工具菜单也无条件加入 Time Tool。  
因此只改 toolbar 不够，必须同步收口：

1. toolbar button
2. menu item
3. 快捷切换与工具回退

### 5.3 Arrangement 参考源入口层

当前 `ArrangementViewComponent` 对参考源按钮的 draw / hit-test / cursor / click 都只受 clip 宽度约束，不受实验开关控制；`PluginEditor` 也会继续构建参考源菜单。  
因此必须统一加门，而不是只在绘制时藏起来。

### 5.4 TimeTool 首次入场链路

当前 `ParameterPanel -> PluginEditor -> PianoRollComponent::setCurrentTool(TimeTool)` 只完成工具切换。  
`PianoRollToolHandler` 也只是切状态，不做任何 TIME GRID / GAME / DerivedAnalysis 预热。  
TimeTool 鼠标逻辑只读当前 `TimeGridSnapshot`，而 store 默认提供的是 identity grid。

根因不是“GAME 算法丢了”，而是：

1. 进入 TimeTool 的正式语义点没有再连到 processor
2. processor 侧也没有一个“确保 stretch 锚点来源存在”的单一入口

## 6. 目标结构

## 6.1 AppPreferences 扩展

新增 shared boolean，例如：

```cpp
bool experimentalFeaturesEnabled = false;
```

职责只限：

1. UI 暴露门
2. 实验功能提示
3. Standalone 参考源入口与 TimeTool 入口的显示/隐藏

`ExperimentalReferenceAlignMode` 保持现有 enum，不改为 bool。

### 6.2 UI 门控收口

需要统一收口到以下模块：

1. `SharedPreferencePages`：新增 toggle 与说明文案
2. `ParameterPanel`：TimeTool 按钮创建/布局/显示门控
3. `PianoRollComponent`：工具菜单与 tool fallback
4. `ArrangementViewComponent`：按钮 draw / hover / cursor / click 四件套
5. `PluginEditor`：参考源菜单入口与开关变化后的工具/界面同步

所有门控都必须读同一份 `AppPreferences` shared state，禁止每个组件各存一份布尔缓存真相。

### 6.3 Processor 单一入口

新增一个 processor 侧正式入口，例如：

```cpp
void OpenTuneAudioProcessor::ensureTimeToolAnchorSeed(uint64_t materializationId);
```

它的职责是：

1. 判断这个 materialization 是否已经有 stretch internal handles
2. 若已经有，则 no-op
3. 若没有，则确保当前 materialization 的 `DerivedAnalysis` 可用
4. 根据 `DerivedAnalysis.temporalEvents` 编译 identity `TimeGridSnapshot`
5. 提交到 `MaterializationStore`

### 6.4 DerivedAnalysis 到 TimeGrid 的编译合同

本轮不允许 UI 直接用 notes 现拼 handles。  
正式做法应是 processor 新增一个小型编译 helper，例如：

```cpp
TimeGridSnapshot buildIdentityTimeGridFromDerivedAnalysis(
    const MaterializationStore::Snapshot& snapshot,
    const MaterializationStore::DerivedAnalysis& analysis) const;
```

编译规则：

1. 端点按既有 identity grid 合同保留并锁定
2. 内部 handles 来自 `analysis.temporalEvents`
3. 每个内部 handle 初值满足 `source_seconds == output_seconds`
4. handle kind 统一选择一类正式语义（推荐 `ReferenceAuto` 或新建 stretch-seed 语义前，优先复用现有 contract）
5. 去重、排序、边界裁剪由 processor 完成

## 7. 行为细节与边界条件

### 7.1 首次进入 TimeTool 的触发点

最佳触发点是 `PianoRollComponent::setCurrentTool(ToolId::TimeTool)`。

原因：

1. 这是唯一明确的“用户进入该工具”语义点
2. 此时已知当前 `editedMaterializationId_`
3. 不需要把逻辑分散到 toolbar button、快捷键、菜单和鼠标事件里

禁止把触发散落到：

1. `ParameterPanel::onToolClicked`
2. `PianoRollToolHandler::mouseDown`
3. `handleTimeToolMouseDown`

否则会重复制造多入口。

### 7.2 已播种判断

以下任一成立时，应视为“已有 TimeTool anchors”，不再重播种：

1. `TimeGridSnapshot` 已有内部 handles
2. grid 已非 identity
3. store 中存在明确的 stretch 编辑痕迹

若只存在默认双端点 identity grid，则视为未播种。

### 7.3 DerivedAnalysis 后端模式

TimeTool 的锚点来源要不要强制 GAME，本轮必须写死合同：

1. 若 `ExperimentalReferenceAlignMode == Aggressive`，优先使用 GAME-derived `DerivedAnalysis`
2. 若当前模式不是 Aggressive，但仍需要 stretch anchors，则仍可使用当前正式 `DerivedAnalysis` producer 的输出

这里要特别说明：

1. 用户口述里把需求叫做“触发一次 GAME 流程”
2. 但工程正确性上，TimeTool 不应再拥有独立于 `DerivedAnalysis` 主链之外的专用 GAME 入口

因此正式合同建议写成：

1. TimeTool 首次入场触发一次“正式 reference/time analysis 准备”
2. 该准备在 Aggressive 模式下会走 GAME producer
3. 其它模式下则走当前正式 producer

如果产品明确要求“Stretch anchors 必须始终走 GAME”，那也必须通过同一个 `buildReferenceDerivedAnalysis(...)` 入口来表达，而不是再造私链。

### 7.4 开关关闭后的状态回退

用户关闭“启用实验性功能”后，必须处理两种已有状态：

1. 当前 UI 正停在 TimeTool
2. 当前 clip 已经绑定 reference source

本轮建议：

1. 关闭开关后，当前工具若为 TimeTool，则立即回退到 Select
2. 现有 reference binding 数据不删除，只是不再暴露入口
3. 已播种/已编辑的 TimeGrid 数据保留，不做 destructive clear

原因：

1. 开关表达的是“暴露与使用入口”，不是“销毁已有数据”
2. destructive cleanup 会引入额外状态机与数据丢失风险

## 8. 需要修改的模块

### Task A: 偏好合同解耦

**Files:**
- Modify: `Source/Utils/AppPreferences.h`
- Modify: `Source/Utils/AppPreferences.cpp`
- Modify: `Source/Editor/Preferences/SharedPreferencePages.h`
- Modify: `Source/Editor/Preferences/SharedPreferencePages.cpp`

**目标：**

1. 新增 `experimentalFeaturesEnabled`
2. 新增持久化键
3. SharedAudioPage 追加 toggle + warning text
4. 保留 `ExperimentalReferenceAlignMode` 的独立语义

### Task B: TimeTool UI 门控

**Files:**
- Modify: `Source/Standalone/UI/ParameterPanel.h`
- Modify: `Source/Standalone/UI/ParameterPanel.cpp`
- Modify: `Source/Standalone/UI/PianoRollComponent.cpp`
- Modify: `Source/Standalone/PluginEditor.cpp`

**目标：**

1. TimeTool 按钮只在开关打开时展示
2. 工具菜单只在开关打开时包含 TimeTool
3. 关闭开关后若当前工具是 TimeTool，回退到稳定工具

### Task C: Arrangement 参考源入口门控

**Files:**
- Modify: `Source/Standalone/UI/ArrangementViewComponent.h`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.cpp`
- Modify: `Source/Standalone/PluginEditor.cpp`

**目标：**

1. 绘制门控
2. hover/hit-test 门控
3. cursor 门控
4. click/menu 门控

### Task D: TimeTool 首次入场锚点播种

**Files:**
- Modify: `Source/PluginProcessor.h`
- Modify: `Source/PluginProcessor.cpp`
- Modify: `Source/MaterializationStore.h`（如需辅助 probe / helper）
- Modify: `Source/Standalone/UI/PianoRollComponent.cpp`

**目标：**

1. 增加 processor 单一入口 `ensureTimeToolAnchorSeed(...)`
2. 判断“未播种” materialization
3. 复用正式 `DerivedAnalysis` producer
4. 编译 identity `TimeGridSnapshot`
5. `setCurrentTool(TimeTool)` 时触发一次

### Task E: Kill List 清理

本轮实现后必须审查并收紧以下残留：

1. 是否存在第二条 TimeTool 专用 GAME 入口
2. 是否存在 UI 自己造 `TimeGridSnapshot`
3. 是否存在只藏按钮、不藏点击命中/菜单构建的半门控
4. 是否存在关闭实验开关后仍能通过快捷键进入 TimeTool 的旁路

## 9. 测试与验证策略摘要

本轮 proof gate 必须聚焦：

1. preference 持久化与 UI 门控
2. TimeTool 首次入场只播种 identity anchors
3. 再次进入不覆盖既有 stretch 数据
4. Arrangement 参考源入口四件套门控完整

重点 focused suites 应是：

1. `architecture`
2. `processor`
3. 如已有合适分类，则补 `ui` 子守卫而不是依赖当前全量 `ui` runner

详细口径见配套文档：

- `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed-test-verification.md`

## 10. 明确禁止事项

本轮不允许以下“看似能跑”的错误实现：

1. 继续把 `ExperimentalReferenceAlignMode != Off` 当成实验功能总开关
2. 只隐藏 TimeTool 按钮，但菜单/快捷键/现有状态不收口
3. 只隐藏 Arrangement 按钮绘制，但 hover/cursor/click 仍有效
4. TimeTool 首次进入直接生成非 identity 时间网格
5. TimeTool 首次进入直接执行 AUTO Ref 或直接改 clip 时长
6. UI 自己读 notes/temporalEvents 拼 grid
7. 为 stretch anchors 再造一条 `requestGame...` 专用旁路
8. 开关关闭时 destructive clear 用户已有 reference binding 或 TimeGrid 编辑数据

## 11. 实施顺序

1. 先拆开 `experimentalFeaturesEnabled` 与 `ExperimentalReferenceAlignMode`
2. 再收口 TimeTool / Arrangement 的 UI 门控
3. 再实现 processor 侧 `ensureTimeToolAnchorSeed(...)`
4. 最后补 focused tests 与 kill list 审查

这个顺序不能反过来：

1. 若先补 TimeTool GAME 触发而不先固定开关合同，后续 UI 还会返工
2. 若先在 UI 层拼 seed 逻辑，会直接违背单一入口原则

## 12. 交付完成定义

只有以下条件同时满足，才算本轮完成：

1. Shared preferences 中有独立的“启用实验性功能”总开关与提示
2. TimeTool 的所有 Standalone 入口都被这个总开关统一控制
3. Arrangement 参考源按钮/菜单的 draw + hover + cursor + click 全部被统一控制
4. `ExperimentalReferenceAlignMode` 仍只描述 AUTO Ref 模式
5. 首次进入某个 clip 的 TimeTool 会播种 identity stretch anchors
6. 再次进入不会覆盖已有 stretch 编辑
7. processor 是唯一 seed producer，UI 不拥有生成逻辑
8. 没有新增平行 GAME 链或兜底结构
