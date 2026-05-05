---
spec_version: 1.0.0
status: draft
module: ui-piano-roll
doc_type: overview
generated_by: module-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# ui-piano-roll 模块概览

## 定位

`ui-piano-roll` 是 OpenTune 钢琴卷帘编辑器的完整 UI 子系统：在 JUCE `juce::Component` 之上承载音高曲线（F0）、音符序列、波形与节拍尺的可视化与交互。用户通过此组件选择/绘制/调整/拖动音符，手绘或用线段锚点修改 F0，触发 AutoTune，并配合 `PianoKeyAudition` 在左侧琴键上试听单音。

组件自身不执行音频合成或 F0 推理，而是作为「交互层」把用户手势翻译为：

1. **数据层写入** — 通过 `OpenTuneAudioProcessor` 的 `commitMaterialization*` 接口替换 `Note` 列表和 `CorrectedSegment` 列表；
2. **Undo 事务** — 每次成功提交都向 `UndoManager` 注册一条 `PianoRollEditAction`（持有 before/after 快照）；
3. **异步修正请求** — AutoTune 与基于音符的范围修正通过 `PianoRollCorrectionWorker` 后台线程执行；
4. **渲染失效** — 通过 `PianoRollVisualInvalidation` 汇总脏区域、优先级和原因掩码，经 `FrameScheduler` 节流为 ≤60 fps 的局部重绘。

## 模块边界

```
上游依赖（读取）：
├── pitch-correction/PitchCurve  — F0 数据 + CorrectedSegment（COW 快照）
├── pitch-correction/Note        — 音符数据结构（startTime/endTime/pitch/pitchOffset/...）
├── pitch-correction/NoteGenerator — 分段策略 NoteSegmentationPolicy、PIP 计算
├── utils/F0Timeline             — 帧↔时间换算
├── utils/MaterializationTimelineProjection — 时间轴↔素材时间投影
├── utils/AudioEditingScheme     — 工具与参数目标策略（可编辑帧、参数落点）
├── utils/KeyShortcutConfig      — 用户快捷键配置
├── utils/ZoomSensitivityConfig  — 缩放灵敏度参数
├── utils/UndoManager            — 500 层线性撤销栈
└── ui-theme/ThemeTokens + UIColors + OpenTuneLookAndFeel — 颜色与字体令牌

下游消费（写入/通知）：
├── OpenTuneAudioProcessor
│    ├── commitMaterializationNotesAndSegmentsById    — 音符+F0段原子提交
│    ├── setMaterializationNotesById                  — 仅音符
│    ├── setMaterializationCorrectedSegmentsById      — 仅F0段
│    ├── commitAutoTuneGeneratedNotesByMaterializationId — AutoTune 结果
│    └── getUndoManager().addAction(PianoRollEditAction) — 注册撤销项
├── PianoKeyAudition             — noteOn/noteOff（消息线程 → 音频线程无锁 SPSC）
└── Listener 回调（ui-main / Standalone Editor）
     ├── playheadPositionChangeRequested — 时间轴点击
     ├── pitchCurveEdited(startFrame,endFrame) — 失效通知
     ├── noteOffsetChanged                — 音符偏移已改
     ├── autoTuneRequested                — AutoTune 工具 / 快捷键 6
     └── undoRequested/redoRequested      — 撤销/重做
```

## 架构概览

```
┌─────────────────────────────────────────────────────────────────┐
│                     PianoRollComponent                          │
│         （juce::Component, ScrollBar::Listener — 协调者）        │
│                                                                 │
│  ┌─────────────────┐   ┌──────────────────┐  ┌───────────────┐  │
│  │ ToolHandler     │   │ Renderer         │  │ Correction    │  │
│  │ (交互状态机)    │   │ (RenderContext   │  │ Worker        │  │
│  │ Select/Draw/    │   │  驱动的绘制管线) │  │ (后台线程)    │  │
│  │ Line/HandDraw/  │   └──────────────────┘  └───────────────┘  │
│  │ AutoTune        │                                            │
│  └────────┬────────┘   ┌──────────────────┐  ┌───────────────┐  │
│           │            │ VisualInvalidate │  │ InteractionSt │  │
│           │            │ (脏区域合并)     │  │ (交互瞬态)    │  │
│           │            └──────────────────┘  └───────────────┘  │
│           │  ┌────────────────┐                                 │
│           └──│ PianoKey       │        ┌────────────────────┐   │
│              │ Audition       │        │ WaveformMipmap     │   │
│              │ (88 键采样)    │        │ PlayheadOverlay    │   │
│              └────────────────┘        └────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
           │
           ▼
     PianoRollEditAction   →   UndoManager（Utils/）
```

## 关键设计模式

1. **Context 回调桥接** — `PianoRollToolHandler::Context` 用 `std::function` 结构体承载 40+ 个回调，让工具处理器可在不引用 `PianoRollComponent` 的前提下完成状态读写、Undo 描述、坐标换算、视觉失效、Listener 通知，便于单测替换。
2. **不可变 RenderContext 快照** — 每次 `paint()` 由 `buildRenderContext()` 构造一次性的 `PianoRollRenderer::RenderContext`，包含尺寸、缩放、minMidi/maxMidi、`materializationProjection`、`F0Timeline`、`PitchCurveSnapshot` 和坐标换算闭包，渲染过程全程只读。
3. **Draft/Commit 两阶段编辑** — `NoteInteractionDraft`（baseline + working）包裹一次交互：mouseDown `beginNoteDraft` 捕获 baseline，mouseDrag 只改 working，mouseUp `commitNoteDraft` 统一写回 processor 并生成一条 Undo。
4. **原子音符+段提交** — 当编辑同时影响音符和 F0 段时，`commitNotesAndSegments` 走一次 `commitMaterializationNotesAndSegmentsById`，避免产生两条 Undo。
5. **手动修正累积 + 清除** — 删除音符/选区时先在本地累积 `correctionClearRanges`，配合克隆 `PitchCurve` 一次性 `clearCorrectionRange` + 原子 commit，保证删除音符与擦除修正在同一事务。
6. **异步 CorrectionWorker** — 后台单线程 + 条件变量；主线程仅保留最新 pending 请求，通过 `version_` 递增让旧请求自废；完成后主线程在 `onHeartbeatTick` 内 `takeCompleted` 并校验 `materializationIdSnapshot` + `materializationEpochSnapshot` 后才 commit。
7. **视觉失效聚合** — 原因掩码 `Interaction/Viewport/Content/Playhead/Decoration` + 优先级 `Background/Normal/Interactive` + 脏矩形或 fullRepaint，`makeVisualFlushDecision` 汇总后以 60 fps 阈值节流，并在 `flushPendingVisualInvalidation` 中交给 `FrameScheduler::requestInvalidate`。
8. **无锁试听 SPSC** — `PianoKeyAudition` 用 64 槽位 `NoteEvent` 环形缓冲，消息线程 `noteOn/noteOff` 写、音频线程 `mixIntoBuffer` 读。

## 文件清单（17 个）

| 路径 | 行数 | 说明 |
|------|-----:|------|
| `Source/Standalone/UI/PianoRollComponent.h` | 432 | 顶层组件声明：Listener、TimeUnit/ScrollMode、40+ setter/getter、invalidateVisual 3 套重载 |
| `Source/Standalone/UI/PianoRollComponent.cpp` | 2690 | 顶层实现：paint、mouse/key、Undo 事务、AutoTune 闭环、scroll/zoom、Context 装配 |
| `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h` | 189 | 工具分发器声明 + `Context`（40+ 回调）+ `ManualCorrectionOp` |
| `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp` | 1767 | 5 种工具的 mouseDown/Drag/Up 状态机 + keyPressed + 锚点工具 |
| `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h` | 103 | 渲染器声明 + `RenderContext` 快照结构体 |
| `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp` | 1001 | drawLanes/Waveform/TimeRuler/GridLines/ChunkBoundaries/PianoKeys/Notes/F0Curve |
| `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h` | 59 | 失效原因掩码、优先级、`Request/State/Decision` |
| `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.cpp` | 108 | `merge/clear/hasWork` + `makeVisualFlushDecision` |
| `Source/Standalone/UI/PianoRoll/InteractionState.h` | 110 | `SelectionState/NoteDragState/NoteResizeState/DrawingState/NoteInteractionDraft` |
| `Source/Standalone/UI/PianoRoll/InteractionState.cpp` | 54 | 各状态结构体的 `clear()` 与 `setF0Range/clearF0Selection` |
| `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h` | 94 | 异步修正工作器：Kind/AsyncCorrectionRequest/enqueue/takeCompleted |
| `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.cpp` | 185 | workerLoop + executeRequest（ApplyNoteRange/AutoTuneGenerate 两路） |
| `Source/Utils/PianoRollEditAction.h` | 41 | `UndoAction` 子类，封装 Note + CorrectedSegment 的 before/after |
| `Source/Utils/PianoRollEditAction.cpp` | 47 | undo()/redo() 调用 `commitMaterializationNotesAndSegmentsById` |
| `Source/Utils/PianoKeyAudition.h` | 83 | 88 键试听管线，无锁 SPSC 事件、8 Voice 池 |
| `Source/Utils/PianoKeyAudition.cpp` | 211 | BinaryData 采样加载、onset 检测、release 衰减、线性插值重采样 |
| `Source/Utils/PianoRollVisualPreferences.h` | 18 | `NoteNameMode` 枚举 + `PianoRollVisualPreferences` 结构体 |
| `Source/Standalone/UI/ToolIds.h` | 14 | `ToolId` 枚举：AutoTune(0)/Select(1)/DrawNote(2)/LineAnchor(3)/HandDraw(4) |
| `Source/Standalone/UI/ToolbarIcons.h` | 528 | 24×24 viewBox 的 `juce::Path` SVG 图标静态工厂 |
| **合计** | **~7534** | |

> 注：`overview.md` 旧版列出的 `PianoRollUndoSupport.h/.cpp`（v1.0 时期）在 v1.3 重构中被删除，其职责（before/after 快照 + Action 构建）已内联到 `PianoRollComponent::captureBeforeUndoSnapshot` + `recordUndoAction`，撤销实现改为 `Utils/UndoManager` 统一管理。

## v1.3 关键变更

| 变更 | 影响 |
|------|------|
| 新拆 `PianoRollVisualInvalidation.{h,cpp}` | 原本散落在 `PianoRollComponent` 内的失效合并逻辑独立为可测试的纯函数模块 |
| `PianoRollEditAction` 移出到 `Source/Utils/` | 和 `UndoManager` 同一目录；替换了旧 `PianoRollUndoSupport` |
| `PianoKeyAudition` 移入 `Source/Utils/` | 与 Processor 试听混音一致的命名空间 |
| 引入 `MaterializationTimelineProjection` | 组件内部统一通过 `projectTimelineTimeToMaterialization` / `projectMaterializationTimeToTimeline` 把时间轴秒数投影到素材秒数，兼容 ARA/Clip 偏移 |
| `editedMaterializationEpoch_` | 每次 `setEditedMaterialization` 自增，AutoTune/异步修正结果带 epoch 快照，epoch 不一致即丢弃结果，避免串轨污染 |
| 选区类型扩充 | `SelectionState` 同时持有矩形 `hasSelectionArea` 和基于帧的 `hasF0Selection`，两者由 `updateF0SelectionFromNotes` 保持一致 |

## 约束与不变量

- **线程**：所有 UI 与 Draft/Undo 写入均在消息线程；`PianoRollCorrectionWorker` 和 `PianoKeyAudition::mixIntoBuffer` 分别在后台线程和音频线程。
- **最小音符时长**：`DrawNote` 工具强制 `endTime - startTime >= 0.02s`；Select 工具 Resize 同样保留 0.02 s 最小时长。
- **音高吸附**：`DrawNote` 按下起始 pitch 用 `round(midi)` 吸附到半音；音符拖动按半音整数 `pitchOffset` 吸附；`LineAnchor` 起点/落点 pitch 吸附到半音。
- **帧可编辑性**：`AudioEditingScheme::canEditFrame` 决定某帧是否允许写入 HandDraw / LineAnchor 的数据；越界帧被 `flushCurrentOp` 自动切断。
- **失效节流**：`invalidateVisual(priority=Interactive)` 内部以 60 fps 为最小间隔直接调用 `flushPendingVisualInvalidation`，其余优先级依赖 `onHeartbeatTick` 刷新。
- **Undo 原子性**：任何影响 (Note, CorrectedSegment) 对的编辑必须通过 `commitEditedMaterializationNotesAndSegments` 或 `commitNotesAndSegments` 提交，禁止分两步写入，以保证 `PianoRollEditAction` 持有一致的 before/after。
- **MIDI 范围**：`minMidi_=24`, `maxMidi_=108`（MIDI C1–C8）固定为 `constexpr float`；`PianoKeyAudition` 仅加载 MIDI 21–108 的采样（MIDI 21–23 未被卷帘显示但仍可被 noteOn 触发）。
