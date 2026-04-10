---
module: ui-piano-roll
type: overview
generated: true
source_scan: true
---

# 模块概览 — ui-piano-roll

## 模块定位

`ui-piano-roll` 是 OpenTune 的核心交互编辑模块，提供钢琴卷帘式的音高编辑器 UI。用户通过此组件可视化和编辑 AI 提取的音高曲线（F0）与音符序列，驱动后端的音高修正和声码器合成管线。

## 架构概览

```
┌─────────────────────────────────────────────────────┐
│                PianoRollComponent                     │
│  (juce::Component — 中央协调者)                       │
│                                                      │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │ToolHandler   │  │Renderer      │  │UndoSupport │ │
│  │(交互分发)     │  │(绘制管线)    │  │(事务管理)  │ │
│  └──────┬───────┘  └──────────────┘  └────────────┘ │
│         │                                            │
│  ┌──────┴───────┐  ┌──────────────┐                  │
│  │InteractionSt │  │Correction    │                  │
│  │(交互状态)     │  │Worker        │                  │
│  └──────────────┘  │(异步修正)    │                  │
│                     └──────────────┘                  │
└─────────────────────────────────────────────────────┘
```

## 组件职责

| 组件 | 文件 | 职责 |
|------|------|------|
| **PianoRollComponent** | `PianoRollComponent.h/.cpp` | 顶层 JUCE Component；管理生命周期、依赖注入、paint 调度、scroll/zoom、heartbeat；是 ToolHandler/Renderer/UndoSupport/CorrectionWorker 的所有者和协调者 |
| **PianoRollToolHandler** | `PianoRoll/PianoRollToolHandler.h/.cpp` | 工具交互处理器；接收 mouse/key 事件，按当前工具模式分发到各 handle* 方法；通过 Context 回调与 Component 交互 |
| **PianoRollRenderer** | `PianoRoll/PianoRollRenderer.h/.cpp` | 纯绘制逻辑；接收不可变 RenderContext 快照，绘制钢琴键、网格、波形、音符、F0 曲线等所有视觉元素 |
| **PianoRollUndoSupport** | `PianoRoll/PianoRollUndoSupport.h/.cpp` | 编辑事务管理；begin/commit 模式捕获前后快照，生成 NotesChangeAction / CorrectedSegmentsChangeAction |
| **PianoRollCorrectionWorker** | `PianoRoll/PianoRollCorrectionWorker.h/.cpp` | 异步音高修正；后台线程执行 `PitchCurve::applyCorrectionToRange`，主线程通过 heartbeat 轮询结果 |
| **InteractionState** | `PianoRoll/InteractionState.h/.cpp` | 交互状态聚合体；包含 SelectionState、NoteDragState、NoteResizeState、DrawingState 等所有瞬态交互数据 |
| **ToolIds** | `ToolIds.h` | 工具枚举：AutoTune(0), Select(1), DrawNote(2), LineAnchor(3), HandDraw(4) |
| **ToolbarIcons** | `ToolbarIcons.h` | SVG Path 图标库（24x24 viewBox），提供所有工具栏/菜单图标的静态工厂方法 |

## 关键设计模式

1. **Context 回调桥接**：ToolHandler/UndoSupport 通过 `std::function` 回调结构体访问 Component 内部，避免直接耦合
2. **RenderContext 快照**：每次 paint 构建不可变参数快照，确保渲染一致性
3. **事务式 Undo**：begin/commit 包裹编辑操作，自动 diff 生成最小 Action
4. **异步修正**：CorrectionWorker 在后台线程执行耗时的音高修正，主线程非阻塞轮询
5. **重绘节流**：`requestInteractiveRepaint` 限制 ~60fps，通过 FrameScheduler 合并脏区域

## 外部依赖

| 依赖 | 方向 | 说明 |
|------|------|------|
| `PitchCurve` / `PitchCurveSnapshot` | 读写 | F0 数据和修正段的核心数据结构（COW） |
| `Note` / `NoteSequence` | 读写 | 音符数据结构 |
| `OpenTuneAudioProcessor` | 读 | 获取 clip 音符列表引用、clip 索引 |
| `UndoManager` | 写 | 注册 undo action |
| `TimeConverter` | 读 | 时间↔像素转换 |
| `WaveformMipmap` | 读 | 波形多级缓存 |
| `FrameScheduler` | 写 | 重绘调度 |
| `UIColors` / `Theme` | 读 | 主题颜色和字体 |
| `ZoomSensitivityConfig` | 读 | 缩放灵敏度配置 |
| `KeyShortcutConfig` | 读 | 快捷键配置 |
| `ScaleSnapConfig` / `ScaleInference` | 读 | 音阶对齐 |

## 文件清单

| 文件 | 行数 | 说明 |
|------|------|------|
| `PianoRollComponent.h` | 416 | 主组件声明 |
| `PianoRollComponent.cpp` | 1965 | 主组件实现 |
| `PianoRoll/PianoRollToolHandler.h` | 173 | 工具处理器声明 |
| `PianoRoll/PianoRollToolHandler.cpp` | 1566 | 工具处理器实现 |
| `PianoRoll/PianoRollRenderer.h` | 104 | 渲染器声明 |
| `PianoRoll/PianoRollRenderer.cpp` | 768 | 渲染器实现 |
| `PianoRoll/PianoRollUndoSupport.h` | 58 | 撤销支持声明 |
| `PianoRoll/PianoRollUndoSupport.cpp` | 207 | 撤销支持实现 |
| `PianoRoll/InteractionState.h` | 99 | 交互状态声明 |
| `PianoRoll/InteractionState.cpp` | 64 | 交互状态实现 |
| `PianoRoll/PianoRollCorrectionWorker.h` | 89 | 异步修正声明 |
| `ToolIds.h` | 15 | 工具枚举 |
| `ToolbarIcons.h` | 528 | SVG 图标库 |
| **合计** | **~6052** | |
