# AUTO(REF) Reference-Driven Pitch And Timing Alignment Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 Standalone 的参考源自动修音对轨重构为正式的 `ClipA -> ClipB` GAME 驱动音高+节奏对轨能力，使 `AUTO(REF)` 能在不依赖预先 seed 的 `TimeTool` 前提下，直接把 `ClipA` 修成更接近 `ClipB` 的音高和节奏形态。

**Architecture:** 参考绑定继续只存放在 `StandaloneArrangement` 的 placement truth；processor 根据 `target/reference placement + overlap window` 构造一次 reference-alignment 请求；特征层拆为 `ReferencePitchFeatures` 与 `ReferenceTimingFeatures`，两者都处于 `materialization-local source-time` 域；aligner 只产出 `pitch patch + timing patch`，其中 timing patch 的正式语义是“写回 `ClipA.timeGridAfter` 的受约束 auto handle drag”，并由 `MaterializationStore` 一次性提交 `notes + correctedSegments + timeGrid`。`TimeTool` seed 仅允许共享 timing-feature extraction，project persistence 只保留 `reference binding + notes + correctedSegments + timeGrid`；`AUTO(REF)` 自动 timing patch 触及到的每个区间都必须满足 `0.8x~1.3x` 局部速度窗口。

**Tech Stack:** C++17, JUCE, StandaloneArrangement, MaterializationStore, ReferenceAnalysisService, ReferenceAutoAlign, TimeGridPatchBuilder, ProjectPersistence, OpenTuneTests

---

## 1. 根问题

当前 live tree 里，`AUTO(REF)` 已经不再是“还没开始做”，而是一版混合原型：

1. 参考绑定真相已经在 `StandaloneArrangement::Placement::referencePlacementId`
2. Arrangement / PianoRoll / AUTO 按钮入口已经打通
3. processor 已经有 feature build、alignment execute、TimeTool seed 等路径
4. 但这些路径把四件事混在了一起:
   - reference binding
   - reference feature production
   - `AUTO(REF)` 对齐执行
   - `TimeTool` 首次 seed

最核心的结构问题有两个:

1. `MaterializationStore::DerivedAnalysis` 同时承载了 notes、temporal events、backend mode、staleness fingerprint、错误状态、以及 project 持久化影子，语义过载
2. `AUTO(REF)` 当前把“target/reference 已有 `TimeGridSnapshot`”误设为硬前提，导致 timing path 实际上绑死在 `TimeTool` seed 叙事上

这两点一起导致现在的计划和实现都会把 `AUTO(REF)` 错误地理解成:

1. 先做一份 `DerivedAnalysis`
2. 再顺便给 `TimeTool` 播种
3. 有了 grid 以后才能做 reference timing alignment

这不是根本需求。

## 2. 正式产品合同

本轮要回到用户真正要的语义:

1. 用户指定 `ClipA` 的参考源为 `ClipB`
2. 用户对 `ClipA` 触发 `AUTO`
3. 若当前 `ClipA` 存在 reference binding，则 `AUTO` 升级为 `AUTO(REF)`
4. `AUTO(REF)` 通过 GAME 分别为 `ClipA`、`ClipB` 生成“音符 + 节奏锚点”特征
5. 将 A/B 特征在重叠窗口内对齐
6. 对 `ClipA` 施加:
   - 对应的音符绘制 / pitch target 更新
   - 对应的时间锚点拖动 / timing patch
7. `ClipB` 只作为参考，不被写回

正式输出必须是:

1. `ClipA.notesAfter`
2. `ClipA.correctedSegmentsAfter`
3. `ClipA.timeGridAfter`

补充正式语义:

1. `AUTO(REF)` 一次流程里同时完成两件事:
   - 自动音符/音高修正
   - 受约束的大规模自动锚点拖拽
2. `ClipA.timeGridAfter` 不是 reference 专用黑盒状态，而是现有 `TimeTool` 的正式真相；用户在 `AUTO(REF)` 前后都可以继续手动拖拽锚点
3. 现有 TimeTool 外观与交互合同必须被复用，而不是另造 AUTO(REF) 专用 UI。按当前代码现状，锚点可视化是整条竖向 guide line + 顶部菱形抓手；auto 生成 handle 通过 `HandleKind::ReferenceAuto` 标识

而不是:

1. 持久化 reference analysis 缓存
2. 把 `TimeTool` seed 本身当作对齐结果
3. 把参考 clip 写成“双向耦合”的第二真相

## 3. 四层正式合同

### 3.1 Reference Binding Contract

binding 的 persisted truth 继续只有一条:

```cpp
targetPlacement.referencePlacementId -> referencePlacementId
```

它只表达:

1. 哪个 target placement 参考哪个 reference placement
2. 不表达特征缓存
3. 不表达算法模式
4. 不表达已经执行过几次 AUTO(REF)

因此 `ProjectReferenceBinding.analysisMode` 不是长期可接受的产品真相，应从正式持久化里退出。

### 3.2 Reference Feature Contract

参考特征必须逻辑拆分为两个子合同，而不是继续混装在 `DerivedAnalysis`:

```cpp
struct ReferencePitchFeatures {
    bool ready{false};
    int64_t inputFingerprint{0};
    double sourceDurationSeconds{0.0};
    std::vector<Note> notes;
    juce::String diagnostics;
};

struct ReferenceTimingAnchor {
    uint64_t anchorId{0};
    double sourceSeconds{0.0};
    float strength{0.0f};
    float confidence{0.0f};
    enum class Kind : uint8_t { Onset, Transition };
    Kind kind{Kind::Onset};
};

struct ReferenceTimingFeatures {
    bool ready{false};
    int64_t inputFingerprint{0};
    double sourceDurationSeconds{0.0};
    std::vector<ReferenceTimingAnchor> anchors;
    juce::String diagnostics;
};

struct ReferenceFeatureSet {
    enum class Producer : uint8_t { Game };
    Producer producer{Producer::Game};
    ReferencePitchFeatures pitch;
    ReferenceTimingFeatures timing;
};
```

关键约束:

1. `ClipA` 和 `ClipB` 都要生成各自一份完整 feature set
2. `ClipB` 的 notes/anchors 是“参考 notes/anchors”
3. 两个子合同都以 `materialization-local source-time` 为唯一时间域
4. feature cache 只是 runtime cache，不是 project truth

### 3.3 Alignment Request Contract

`AUTO(REF)` 的正式 request 必须显式承载“绑定、特征、有效时间映射、重叠窗口、target 现状”:

```cpp
struct EffectiveTimeMap {
    double sourceDurationSeconds{0.0};
    double outputDurationSeconds{0.0};
    bool isIdentity{true};
    double tau(double sourceSeconds) const;
    double tauInverse(double outputSeconds) const;
};

struct ReferenceAlignmentRequest {
    ReferenceClipProjection target;
    ReferenceClipProjection reference;
    EffectiveTimeMap targetTimeMap;
    EffectiveTimeMap referenceTimeMap;
    ReferenceFeatureSet targetFeatures;
    ReferenceFeatureSet referenceFeatures;
    std::vector<Note> targetNotesBefore;
    std::vector<CorrectedSegment> targetSegmentsBefore;
    std::shared_ptr<const TimeGridSnapshot> targetTimeGridBefore;
    double overlapStartTimelineSeconds{0.0};
    double overlapEndTimelineSeconds{0.0};
};
```

关键点:

1. overlap 仍然由 placement timeline intersection 决定
2. 但 patch 落点必须映射回 target materialization-local source-time
3. `EffectiveTimeMap` 允许 identity map 或已有用户 warp
4. `AUTO(REF)` 不能再因为没有 seed 过 `TimeTool` 就失败

### 3.4 Alignment Patch Contract

aligner 的职责仍然是纯 patch 生产，不直接提交 store:

```cpp
struct AlignmentPatch {
    bool success{false};
    int affectedStartFrame{0};
    int affectedEndFrame{0};
    std::vector<Note> notesAfter;
    std::vector<CorrectedSegment> correctedSegmentsAfter;
    std::vector<TimeGridIntent> timingIntents;
    bool pitchChanged{false};
    bool timingChanged{false};
    juce::String diagnostics;
};
```

但合同需要重写:

1. pitch path 基于 `ReferencePitchFeatures`
2. timing path 基于 `ReferenceTimingFeatures + EffectiveTimeMap`
3. `timingIntents` 必须能在 identity-or-user-warp 上编译
4. 不允许再要求“先有完整 `TimeGridSnapshot` 才能进入 timing path”

### 3.5 Time-Grid Truth And Constrained Auto Drag Contract

`AUTO(REF)` 的 timing 结果必须直接落到常规 `TimeGridSnapshot` 真相，而不是额外引入一层 reference-only timing state。

核心规则:

1. `AUTO(REF)` 自动生成的 timing handles 必须作为普通 `TimeTool` handles 写回 `ClipA.timeGridAfter`
2. auto 生成 handles 统一标记为 `HandleKind::ReferenceAuto`，用于 rerun/渲染/调试识别，但不改变它们作为正式 `TimeTool` truth 的地位
3. rerun `AUTO(REF)` 时，只允许重建受影响范围内旧的 `ReferenceAuto` handles；用户范围外的已有 handles、端点与普通手工 handles 不得被顺手清掉
4. 本轮新增硬性速度限制只约束 `AUTO(REF)` 自动 patch；它不自动改写现有 `TimeTool` 手动拖拽合同

局部速度窗口定义:

```cpp
speed_i = (sourceSeconds[i + 1] - sourceSeconds[i]) / (outputSeconds[i + 1] - outputSeconds[i])
```

对 `AUTO(REF)` 自动 patch 触及到的每个区间，都要求:

```cpp
0.8 <= speed_i && speed_i <= 1.3
```

这意味着:

1. 若参考锚点要求的对齐会把某个区间推到 `0.8x` 以下或 `1.3x` 以上，则不能继续强拉
2. patch builder 必须把候选 handle 位置投影/饱和到最近的可行解，允许“有限对齐”，而不是追求逐点精确命中 `ClipB`
3. 速度窗口、单调性、最小 spacing、端点锁定、总时长守恒必须同时成立；不能为了其中一个约束破坏另外几个不变量

## 4. 时间域与重叠窗口

正式时间域必须收紧为三层，而不是混用:

1. `Placement timeline time`
   - 用于表达 `ClipA` 与 `ClipB` 在 Arrangement 上的重叠窗口
2. `Materialization-local source-time`
   - 用于表达 GAME 生成的 notes 与 timing anchors
   - 也是 patch 真正落点的时间域
3. `Output-time / warped-time`
   - 只由 `EffectiveTimeMap` / `TimeGridSnapshot` 表达
   - 用于把“当前播放/显示节奏”投影回 source-time

正式算法流程:

1. 先算 `overlapStartTimelineSeconds / overlapEndTimelineSeconds`
2. 对 target/reference 各自用 `EffectiveTimeMap.tauInverse(...)` 求出重叠窗口对应的 source-time
3. 在 source-time 上进行 A/B note-anchor 对齐
4. 产出 target 的 pitch patch 与 timing intents
5. 由 `TimeGridPatchBuilder` 把 timing intents 编译为普通 `TimeGridSnapshot` patch，并在最终落地前执行 `0.8x~1.3x` 局部速度窗口投影/饱和

因此:

1. feature 真相永远不跟 placement timeline 混在一起
2. 是否已有 `TimeTool` 编辑，只影响当前 `EffectiveTimeMap`
3. 不影响“是否能够做 `AUTO(REF)`”
4. 即使参考对齐目标不可完全达到，`AUTO(REF)` 也应输出受限后的可行 `timeGridAfter`，而不是因为“无法精确对齐”整体失败

## 5. TimeTool 的正式关系

`TimeTool` 与 `AUTO(REF)` 的关系必须改成“共享一部分特征生产”，而不是互为前提:

1. `TimeTool` seed 只允许共享 `ReferenceTimingFeatures`
2. `TimeTool` seed 不能拥有 `AUTO(REF)` 的正式 producer 语义
3. `AUTO(REF)` 不能反向依赖“用户先进入过 TimeTool”

正式规则:

1. `AUTO(REF)` 可以在从未 seed 过 `TimeTool` 的 clip 上直接执行
2. `ensureTimeToolAnchorSeed(...)` 只负责把 timing anchors 编译成 identity internal handles
3. 该 seed 不写 pitch patch，不写 notes，不执行 reference alignment
4. 若 target 已存在用户 warp，则 `AUTO(REF)` 读取现有 `TimeGridSnapshot` 形成 `EffectiveTimeMap`，而不是拒绝执行

这意味着旧计划里“TimeTool seed 复用 `DerivedAnalysis` 主链”的说法需要下线。新的准确说法应是:

1. `TimeTool` seed 复用 timing-feature extraction
2. 但 `AUTO(REF)` 主链独立成立

补充产品约束:

1. `AUTO(REF)` 执行完成后，用户切回 `TimeTool` 必须可以继续对这些 handles 做 hover、选择、拖拽、插入、删除等常规编辑
2. 本轮不顺手把 `TimeTool` 手动拖拽也统一改成 `0.8x~1.3x` 限速；除非后续单独立项，否则该速度窗口只约束 auto patch

## 6. Persistence Contract

project 持久化必须收紧，只保留真正的业务真相:

继续持久化:

1. `referencePlacementId`
2. user notes
3. corrected segments
4. time grid

停止作为 project truth 持久化:

1. `ProjectMaterializationEntry.basicAnalysis`
2. `ProjectMaterializationEntry.enhancedAnalysis`
3. `ProjectReferenceBinding.analysisMode`

迁移原则:

1. legacy project reader 可以忽略旧节点
2. 新版本写工程时不再写出这些节点
3. 不允许长期保留 old/new 双真相并行

## 7. 对现有实现的直接结论

以下实现属于本轮必须拆除或重写的错误前提:

1. `Source/MaterializationStore.h`
   - `DerivedAnalysis` 作为正式 AUTO(REF) 合同
2. `Source/PluginProcessor.cpp`
   - `buildReferenceDerivedAnalysis(...)`
   - `buildGameReferenceDerivedAnalysis(...)`
   - `executeReferenceAlignmentForPlacement(...)` 中的 `oldTimeGrid/referenceTimeGrid` 硬前提
   - `ensureTimeToolAnchorSeed(...)` 对 `buildGameReferenceDerivedAnalysis(...)` 的直连
3. `Source/DSP/ReferenceAutoAlign.h/.cpp`
   - `using AlignmentFeatures = MaterializationStore::DerivedAnalysis`
   - target/reference `timeGrid != nullptr` 的前提
4. `Source/Utils/ProjectModel.h`
   - `basicAnalysis/enhancedAnalysis/analysisMode`
5. `Source/Utils/ProjectPersistence.cpp`
   - reference feature shadow的写出/读回

## 8. 实施阶段

### Phase 1: 拆掉旧合同

目标:

1. 删除 `DerivedAnalysis` 作为正式 AUTO(REF) feature contract 的地位
2. 引入新的 `ReferencePitchFeatures` / `ReferenceTimingFeatures` / `ReferenceFeatureSet`

建议落点:

1. `Source/DSP/ReferenceAutoAlign.h`
2. `Source/Services/ReferenceAnalysisService.h/.cpp`
3. `Source/MaterializationStore.h/.cpp`

完成标准:

1. production AUTO(REF) 不再 `using AlignmentFeatures = MaterializationStore::DerivedAnalysis`
2. runtime cache 语义与 project truth 分离

### Phase 2: 重写 GAME producer

目标:

1. 把现有 `buildGameReferenceDerivedAnalysis(...)` 拆成正式 `buildReferenceFeatureSet(...)`
2. 让 GAME 一次生成 A/B 各自的 notes 与 timing anchors

建议落点:

1. `Source/PluginProcessor.h/.cpp`
2. `Source/DSP/BasicReferenceFeatureBuilder.*`
3. 如有必要，新建 `Source/DSP/GameReferenceFeatureBuilder.*`

完成标准:

1. `AUTO(REF)` 的正式产品 producer 是 GAME
2. 不再以 `Basic/Aggressive` 模式分裂产品语义
3. 如果调试期保留模式开关，也只能是临时开发开关，不得继续主导正式合同

### Phase 3: 重写 ReferenceAutoAlign request/patch

目标:

1. 用 `ReferenceFeatureSet + EffectiveTimeMap` 替换旧 request
2. 移除“必须先有 TimeGrid”前提
3. 把 timing patch 正式收口为“受约束 auto handle drag”，并在 `TimeGridPatchBuilder` 中加入 `0.8x~1.3x` 局部速度投影/饱和

建议落点:

1. `Source/DSP/ReferenceAutoAlign.h/.cpp`
2. `Source/DSP/TimeGridPatchBuilder.h/.cpp`
3. `Tests/TestReferenceAutoAlign.cpp`
4. `Tests/TestAutoRefFailure.cpp`

完成标准:

1. identity target/reference 也能做 timing patch
2. user-warp target 也能做 timing patch
3. aligner 只输出 patch，不越权提交 store
4. `ClipA.timeGridAfter` 仍是普通 `TimeTool` truth，且 auto 触及区间的最终速度不越过 `0.8x~1.3x`

### Phase 4: 重写 processor 执行链

目标:

1. `executeReferenceAlignmentForPlacement(...)` 正式以 binding + feature set + effective time map 执行
2. reference clip 只读，target clip 一次性提交 patch

建议落点:

1. `Source/PluginProcessor.cpp`
2. `Source/MaterializationStore.cpp`
3. `Tests/TestAutoRefIntegration.cpp`

完成标准:

1. overlap 仍按 placement timeline
2. patch 落点按 target materialization-local source-time
3. 不再依赖预 seeded `TimeGrid`

### Phase 5: 重写 TimeTool seed 关系

目标:

1. `ensureTimeToolAnchorSeed(...)` 改为只共享 timing-feature extraction
2. 不再写 `DerivedAnalysis.temporalEvents`
3. 不再调用专用 GAME-analysis 旁路
4. 明确 `AUTO(REF)` 后的 auto handles 与手工 handles 都继续服从同一套 `TimeTool` 外观与交互合同

建议落点:

1. `Source/PluginProcessor.cpp`
2. `Source/Standalone/UI/PianoRollComponent.cpp`
3. `Tests/TestAutoRefIntegration.cpp`

完成标准:

1. `AUTO(REF)` 与 `TimeTool` seed 互不为前提
2. seed 只生成 identity handles
3. `AUTO(REF)` 结果进入 TimeTool 后可继续手工拖拽，不需要额外 reference-only editor path

### Phase 6: 持久化和测试清理

目标:

1. 从 project model / persistence 删除 reference feature shadow
2. 删除或迁移 `DerivedAnalysis` 相关测试
3. 更新 `.planning` 主文档

建议落点:

1. `Source/Utils/ProjectModel.h`
2. `Source/Utils/ProjectPersistence.h/.cpp`
3. `Tests/TestBasicDerivedAnalysis.cpp`
4. `Tests/TestMaterializationDerivedAnalysis.cpp`
5. `.planning/PROJECT.md`
6. `.planning/REQUIREMENTS.md`
7. `.planning/ROADMAP.md`
8. `.planning/STATE.md`

完成标准:

1. 新工程不再写出 legacy analysis nodes
2. 旧工程读取时可忽略旧节点
3. 主 `.planning` 不再把旧 seed 叙事当成 `AUTO(REF)` 正式来源

## 9. Kill List

本轮必须主动删除的错误结构:

1. `DerivedAnalysis` 作为 AUTO(REF) 正式 feature truth
2. `analysisMode` 作为 reference binding persisted truth
3. “先 seed TimeTool 才能做 AUTO(REF)”的前提
4. `Aggressive` 继续充当正式产品模式
5. reference feature cache 持久化影子
6. old/new 并行 reference feature path
7. 为了追求参考对齐而允许无限时间拉伸的 timing patch 语义

## 10. 与旧计划的关系

本计划**部分 supersede** 以下旧计划中与 `AUTO(REF)` 主链相关的叙事:

1. `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed.md`
2. `.planning/plans/2026-05-27-standalone-experimental-features-gate-and-time-tool-anchor-seed-test-verification.md`

保留有效的部分只有:

1. `experimentalFeaturesEnabled` 作为实验入口总开关
2. `TimeTool` 首次入场要有 processor-owned seed

被替换的部分是:

1. `DerivedAnalysis` 作为正式 AUTO(REF) 合同
2. `TimeTool` seed 作为 AUTO(REF) 主链依赖
3. `Aggressive/GAME` 继续通过旧模式叙事表达正式产品语义

## 11. 本轮完成标准

本轮规划完成，不等于代码完成。真正进入实现前，必须满足:

1. test-verification 文档先行
2. 主 `.planning` 口径已经改正
3. 所有人都以“reference-driven pitch and timing alignment”理解 `AUTO(REF)`，而不是继续沿用旧的 `DerivedAnalysis + seed` 叙事
