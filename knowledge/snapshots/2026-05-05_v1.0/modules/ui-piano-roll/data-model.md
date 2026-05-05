---
spec_version: 1.0.0
status: draft
module: ui-piano-roll
doc_type: data-model
generated_by: arch-doc-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# ui-piano-roll 数据模型

本文档记录钢琴卷帘 UI 模块的数据结构、枚举与交互瞬态字段。所有类型位于 `namespace OpenTune`。

---

## 1. 工具枚举 `ToolId`

**文件**: `Source/Standalone/UI/ToolIds.h`

```cpp
enum class ToolId : int {
    AutoTune   = 0,  // 点击即触发（不持续交互）
    Select     = 1,  // 框选 / 点选 / 拖拽 / 调整边缘
    DrawNote   = 2,  // 按下拖拽创建矩形音符块
    LineAnchor = 3,  // 多点击放置锚点，两两 log2 插值生成 F0 段
    HandDraw   = 4   // 自由绘制 F0（帧间 log2 插值）
};
```

使用端：
- `PianoRollComponent::currentTool_`（默认 `Select`）
- `PianoRollToolHandler::currentTool_`（通过 `setTool()` 与 Component 同步）
- 右键菜单显示标签含括号字符 `(2)(3)(4)(5)`，对应 `keyPressed` 中的无修饰字符快捷键（注意 5 仅在菜单出现，见 api.md ⚠️2）

---

## 2. InteractionState 体系

**文件**: `Source/Standalone/UI/PianoRoll/InteractionState.h`

### 2.1 `NoteResizeEdge`

```cpp
enum class NoteResizeEdge {
    None,    // 未处于调整状态
    Left,    // 调整左边缘 → 改 startTime
    Right    // 调整右边缘 → 改 endTime
};
```

### 2.2 `SelectionState` — 选择区域

```cpp
struct SelectionState {
    // —— 矩形选区（基于时间 + MIDI）——
    bool   hasSelectionArea   = false;   // 选区是否已成型（拖出后即使鼠标释放也保留）
    bool   isSelectingArea    = false;   // 正在进行中
    double selectionStartTime = 0.0;     // 素材秒
    double selectionEndTime   = 0.0;
    float  selectionStartMidi = 0.0f;
    float  selectionEndMidi   = 0.0f;

    // —— F0 帧选区（用于参数落点）——
    int  selectedF0StartFrame       = -1;
    int  selectedF0EndFrameExclusive= -1;
    bool hasF0Selection             = false;

    void setF0Range(int startFrame, int endFrameExclusive);
    void clearF0Selection();
};
```

规则：
- `hasSelectionArea=true` 但 `|endTime-startTime| < 0.01s` 或 `|endMidi-startMidi| < 0.5` 时，`mouseUp` 会自动退化为 `false`；
- `hasF0Selection` 由 `updateF0SelectionFromNotes(notes)` 跟随选中音符更新；可选范围空时 `clearF0Selection`；
- `setF0Range(s, e)`：若 `e <= s` 等价于 `clearF0Selection`。

### 2.3 `NoteDragState` — 音符拖动

```cpp
struct NoteDragState {
    int  draggedNoteIndex = -1;
    std::vector<int> draggedNoteIndices;           // 多选拖拽的索引集合
    bool isDraggingNotes  = false;

    // —— 原有 F0 修正"随移"预览 ——
    double manualStartTime = -1.0;                 // 素材秒
    double manualEndTime   = -1.0;
    std::vector<std::pair<double,float>> initialManualTargets; // (time, f0) 原始修正点
    int previewStartFrame       = -1;
    int previewEndFrameExclusive= -1;
    std::vector<float> previewF0;                  // 长度 == endExcl - startFrame，-1.0f 表示 voiced gap

    void clear();
};
```

用途：拖动已被手动修正的音符时，按 `shiftFactor = 2^(deltaSemitones/12)` 对原 F0 成比例平移生成预览。释放时由 `ManualCorrectionOp` 提交。

### 2.4 `NoteResizeState` — 音符边缘调整

```cpp
struct NoteResizeState {
    bool isResizing       = false;
    bool isDirty          = false;  // 首次 drag 时置 true
    int  noteIndex        = -1;
    NoteResizeEdge edge   = None;
    double originalStartTime = 0.0;
    double originalEndTime   = 0.0;

    void clear();
};
```

最小时长：0.02 s。

### 2.5 `NoteInteractionDraft` — Draft 事务

```cpp
struct NoteInteractionDraft {
    bool active = false;
    std::vector<Note> baselineNotes;   // beginNoteDraft 快照
    std::vector<Note> workingNotes;    // 交互过程中反复重置回 baseline 再套用当前 delta
    void clear();
};
```

规则：
- `beginNoteDraft()`：`active=true; baseline = working = cachedNotes_`；
- 拖动/Resize 中每次 `handle*Drag` 首先 `workingNotes = baselineNotes`，再应用最新增量（保证幂等）；
- `commitNoteDraft()`：把 `workingNotes` 写回 processor 并 clear；
- `clearNoteDraft()`：丢弃，不写回。

### 2.6 `DrawingState` — 绘制/锚点瞬态

```cpp
struct DrawingState {
    // —— HandDraw ——
    bool  isDrawingF0      = false;
    std::vector<float> handDrawBuffer;   // 长度与 originalF0 一致，未写帧为 -1
    double dirtyStartTime  = -1.0;       // 本次绘制覆盖的时间区间
    double dirtyEndTime    = -1.0;
    juce::Point<float> lastDrawPoint;    // 上一采样点 (time, f0)

    // —— DrawNote ——
    bool  isDrawingNote    = false;
    double drawingNoteStartTime = 0.0;
    double drawingNoteEndTime   = 0.0;
    float  drawingNotePitch     = 0.0f;
    int    drawingNoteIndex     = -1;    // 在 workingNotes 中的索引

    // —— LineAnchor ——
    bool  isPlacingAnchors = false;
    std::vector<LineAnchor> pendingAnchors;
    juce::Point<float> currentMousePos;  // 虚线预览的末端
};
```

### 2.7 `InteractionState` 聚合

```cpp
class InteractionState {
public:
    SelectionState selection;
    NoteInteractionDraft noteDraft;
    NoteDragState noteDrag;
    NoteResizeState noteResize;
    DrawingState drawing;

    bool isPanning = false;
    juce::Point<int> dragStartPos;

    bool drawNoteToolPendingDrag = false;         // DrawNote 工具：等阈值过后再决定进入 drag
    juce::Point<int> drawNoteToolMouseDownPos;
    bool handDrawPendingDrag = false;             // HandDraw 工具同理

    std::vector<int> selectedLineAnchorSegmentIds;
};
```

> `isPanning` 与 `dragStartPos` 当前在 `InteractionState` 保留但更多由 Component 的 `dragStartScrollOffset_ / dragStartVerticalScrollOffset_` 承担滚动拖拽；见 ⚠️1。

---

## 3. ManualCorrectionOp — 手动修正操作

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h:30`

```cpp
struct ManualCorrectionOp {
    int startFrame            = 0;
    int endFrameExclusive     = 0;
    std::vector<float> f0Data;                                          // f0Data.size() == endFrameExclusive - startFrame
    CorrectedSegment::Source source = CorrectedSegment::Source::HandDraw;
    float retuneSpeed         = -1.0f;                                  // -1 → 使用默认
};
```

生成路径：
- HandDraw：`appendManualCorrectionOps` 遍历 `trimmedRange`（由 `AudioEditingScheme::trimFrameRangeToEditableBounds` 裁剪），遇不可编辑帧或 ≤0 的 F0 值时切断当前 Op，形成多段；
- LineAnchor：对两个锚点间每帧计算 `pow(2, logA + (logB-logA)*t)`；
- NoteDrag（带原手动修正）：直接按 `previewF0` 生成单段。

---

## 4. `PianoRollVisualPreferences`

**文件**: `Source/Utils/PianoRollVisualPreferences.h`

```cpp
enum class NoteNameMode { ShowAll = 0, COnly = 1, Hide = 2 };

struct PianoRollVisualPreferences {
    NoteNameMode noteNameMode       = COnly;
    bool         showChunkBoundaries= false;
    bool         showUnvoicedFrames = false;
};
```

影响点：
- `NoteNameMode` 由 `drawPianoKeys` / `drawNotes` 读取；`COnly` 仅在 C 音显示音名；`ShowAll` 显示所有音；`Hide` 完全隐藏。
- `showChunkBoundaries` 控制 `drawChunkBoundaries` 是否绘制虚线；仅供调试 vocoder chunk 分界。
- `showUnvoicedFrames` 控制 `drawUnvoicedFrameBands`，在清音帧位置绘制淡色竖带。

---

## 5. 失效数据结构（Visual Invalidation）

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h`

### 5.1 `PianoRollVisualInvalidationReason`（位掩码）

```cpp
enum class PianoRollVisualInvalidationReason : uint32_t {
    Interaction = 1u << 0,   // 选区、音符拖拽、工具预览 → 影响 overlay
    Viewport    = 1u << 1,   // scroll / zoom / 尺子 / projection
    Content     = 1u << 2,   // 音符列表、F0 曲线、波形
    Playhead    = 1u << 3,   // 播放头位置
    Decoration  = 1u << 4    // 主题切换、装饰
};
```

使用方式：多原因以位或合并（`toInvalidationMask(Interaction) | toInvalidationMask(Viewport)`）。

### 5.2 `PianoRollVisualInvalidationPriority`

```cpp
enum class PianoRollVisualInvalidationPriority : int {
    Background  = 0,
    Normal      = 1,
    Interactive = 2
};
```

与 `FrameScheduler::Priority` 一一对应（静态断言于 `PianoRollVisualInvalidation.cpp:7-12`）。

### 5.3 `PianoRollVisualInvalidationRequest`

```cpp
struct PianoRollVisualInvalidationRequest {
    uint32_t reasonsMask = 0;
    bool     fullRepaint = false;
    bool     hasDirtyArea= false;
    juce::Rectangle<int> dirtyArea;
    PianoRollVisualInvalidationPriority priority = Background;
};
```

由 `PianoRollComponent::invalidateVisual` 的三个重载构造：
- `invalidateVisual(mask, priority=Normal)` → `fullRepaint=true`；
- `invalidateVisual(mask, dirty, priority=Interactive)` → `fullRepaint = dirty.isEmpty(); hasDirtyArea = !dirty.isEmpty()`。

### 5.4 `PianoRollVisualInvalidationState`

聚合容器，字段同 Request。方法：
- `merge(const Request&)`：并 `reasonsMask`；取最高优先级；按规则合并脏矩形或升级为 fullRepaint；
- `hasWork() const`：`reasonsMask != 0`；
- `clear()`：重置所有字段。

### 5.5 `PianoRollVisualFlushDecision`

```cpp
struct PianoRollVisualFlushDecision {
    bool shouldRepaint = false;
    bool fullRepaint   = false;
    bool hasDirtyArea  = false;
    juce::Rectangle<int> dirtyArea;
    PianoRollVisualInvalidationPriority priority = Background;
};
```

由 `makeVisualFlushDecision(state, localBounds)` 生成，丢弃与 `localBounds` 相交为空的脏区域（升级为 fullRepaint）。

---

## 6. RenderContext 字段（渲染快照）

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h:37`

参见 api.md §3.1。主要字段分组：

| 组 | 字段 | 来源 |
|----|------|------|
| 布局 | `width, height, pianoKeyWidth, rulerHeight, pixelsPerSecond, pixelsPerSemitone` | Component 在 `buildRenderContext` 时写入 |
| 范围 | `minMidi, maxMidi` | 静态 constexpr 24 / 108 |
| 节拍 | `bpm, timeUnit` | `setBpm / setTimeUnit` |
| 投影 | `materializationProjection` | `setMaterializationProjection` |
| 时间线 | `f0Timeline` | `currentF0Timeline()` 派生自 `PitchCurveSnapshot` |
| 音阶 | `scaleRootNote, scaleType` | `setScale` |
| 视觉偏好 | `noteNameMode, showLanes, showChunkBoundaries, showUnvoicedFrames` | Component 字段 |
| 试听 | `pressedPianoKey` | `PianoKeyAudition::getPressedNote` |
| 波形 | `hasUserAudio` | `audioBuffer_ != nullptr` |
| Chunk | `chunkBoundaries` | 由上层填入（调试特性） |
| F0 数据 | `pitchSnapshot (shared_ptr<const>)` | `currentCurve_->getSnapshot()` |
| F0 选区 | `hasF0Selection, f0SelectionStartFrame, f0SelectionEndFrameExclusive` | `selection.hasF0Selection` 与范围 |
| 坐标函数 | `midiToY, freqToY, freqToMidi, xToTime, timeToX` | Component 的成员函数闭包 |

---

## 7. AsyncCorrectionRequest（异步修正）

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h:29`

```cpp
struct AsyncCorrectionRequest {
    enum class Kind { ApplyNoteRange, AutoTuneGenerate } kind = ApplyNoteRange;
    std::shared_ptr<PitchCurve> curve;           // clone(); worker 线程独占
    std::vector<Note> notes;
    int    startFrame           = 0;
    int    endFrameExclusive    = 0;
    float  retuneSpeed          = 1.0f;
    float  vibratoDepth         = 0.0f;
    float  vibratoRate          = 5.0f;
    double audioSampleRate      = 44100.0;
    uint64_t version            = 0;

    // —— AutoTune 专用 ——
    int    autoHopSize          = 160;
    double autoF0SampleRate     = 16000.0;
    int    autoStartFrame       = 0;
    int    autoEndFrame         = 0;
    NoteGeneratorParams autoGenParams;
    std::vector<float>  autoOriginalF0Full;

    // —— Epoch 守卫（防止串轨）——
    uint64_t materializationEpochSnapshot = 0;
    uint64_t materializationIdSnapshot    = 0;

    enum class ErrorKind { None, InvalidRange, VersionMismatch, ExecutionError } errorKind = None;
    bool   success        = false;
    std::string errorMessage;
};
```

主线程消费方：`PianoRollComponent::consumeCompletedCorrectionResults()` → `commitCompletedAutoTuneResult / commitCompletedNoteCorrectionResult`。

---

## 8. Undo Action 快照（`PianoRollEditAction`）

**文件**: `Source/Utils/PianoRollEditAction.h`

持有字段：
```cpp
OpenTuneAudioProcessor& processor_;
uint64_t materializationId_;
juce::String description_;
std::vector<Note> oldNotes_, newNotes_;
std::vector<CorrectedSegment> oldSegments_, newSegments_;
int affectedStartFrame_{0};
int affectedEndFrame_{0};
```

构造时：
- `description_` 落入 `getDescription()`（优先 `pendingUndoDescription_`，否则 `"编辑"`）；
- `affectedStartFrame/EndFrame` = `min(all old&new segment.startFrame) .. max(all old&new segment.endFrame)`；全空时取 0。

运行时：
- `undo()` → `processor_.commitMaterializationNotesAndSegmentsById(id, oldNotes_, oldSegments_)`；
- `redo()` → 同上但用 `newNotes_ / newSegments_`。

---

## 9. PianoKeyAudition 内部结构

**文件**: `Source/Utils/PianoKeyAudition.h`

### 9.1 `Sample`

```cpp
struct Sample {
    juce::AudioBuffer<float> pcm;    // 单声道
    int onsetSample = 0;             // 首个 |amp|>0.005 的帧
};
```

### 9.2 `Voice`

```cpp
struct Voice {
    bool   active = false;
    int    midiNote = -1;
    int    sampleIndex = -1;
    double position = 0.0;           // 浮点索引，用于线性插值
    double playbackRate = 1.0;       // = kSampleRate / outSampleRate
    float  gain = 0.7f;
    bool   releasing = false;
    float  releaseGain = 1.0f;
    float  releaseDecrement = 0.0f;  // = 1 / (kReleaseTimeSeconds * sampleRate)
};
```

### 9.3 `NoteEvent`

```cpp
struct NoteEvent { int midiNote = 0; bool isOn = false; };
```

容器：
- `std::array<NoteEvent, 64> eventBuffer_`；`std::atomic<int>` 读/写位；
- `std::array<Sample, 88> samples_`；
- `std::array<Voice, 8> voices_`；
- `std::atomic<int> pressedNote_{-1}`。

---

## 10. 常量汇总

| 常量 | 值 | 来源 | 作用 |
|------|----|------|------|
| `kAudioSampleRate` | 44100 | `PianoRollComponent` | 默认采样率（fallback） |
| `minMidi_ / maxMidi_` | 24 / 108 | `PianoRollComponent` (constexpr) | 卷帘 MIDI 显示范围 |
| `pixelsPerSemitone_` | 25.0 初始 | `PianoRollComponent` | 每半音纵向像素 |
| `zoomLevel_` | 1.0，clamp [0.02,10] | `PianoRollComponent` | 横向缩放 |
| `pianoKeyWidth_` | 60 | `PianoRollComponent` | 左侧琴键宽度 |
| `rulerHeight_` | 30 | `PianoRollComponent` | 顶部时间尺高度 |
| `timelineExtendedHitArea_` | 20 | `PianoRollComponent` | 时间尺下方额外命中区 |
| `dragThreshold_` | 5 | `PianoRollComponent` | 进入 drag 的最小像素位移 |
| `minMidi=24, maxMidi=108` | — | `PianoRollRenderer::RenderContext` | RenderContext 默认值 |
| `最小音符时长` | 0.02 s | `PianoRollToolHandler` (两处) | DrawNote/Resize |
| `edgeThreshold` | 6 px | `PianoRollToolHandler` | 边缘 Resize 命中阈值 |
| `pitchToleranceHz` | 100.0 | `findNoteIndexAt` 调用处 | 音符命中的频率容差 |
| `tolerancePixels` (LineAnchor 命中) | 15 px | `findLineAnchorSegmentNear` | 段选中容差 |
| `kMaxVoices` | 8 | `PianoKeyAudition` | voice 池容量 |
| `kReleaseTimeSeconds` | 0.05 | `PianoKeyAudition` | release 时长 |
| `kEventBufferSize` | 64 | `PianoKeyAudition` | SPSC 事件环形 |
| `kMidiMin=21, kMidiMax=108` | — | `PianoKeyAudition` | 采样范围 |
| `minIntervalMs` | 1000/60 | `PianoRollComponent::invalidateVisual` | 交互式刷新节流 |
| `maxSize_` (UndoManager) | 500 | `UndoManager` | 撤销栈上限（全局） |

---

## 11. 关系图

```mermaid
classDiagram
    class PianoRollComponent {
      +Listener
      +TimeUnit/ScrollMode
      -currentTool_ : ToolId
      -interactionState_ : InteractionState
      -currentCurve_ : shared_ptr~PitchCurve~
      -cachedNotes_ : vector~Note~
      -pendingVisualInvalidation_ : State
      +invalidateVisual()
      +setEditedMaterialization()
    }
    class PianoRollToolHandler {
      -ctx_ : Context
      -currentTool_ : ToolId
      +mouseDown/Drag/Up()
      +keyPressed()
    }
    class PianoRollRenderer {
      -correctedF0Cache_ : vector~float~
      -waveformMipmap_ : WaveformMipmap*
      +drawLanes/Notes/F0Curve()
    }
    class InteractionState {
      +SelectionState selection
      +NoteInteractionDraft noteDraft
      +NoteDragState noteDrag
      +NoteResizeState noteResize
      +DrawingState drawing
    }
    class PianoRollEditAction {
      +undo() / redo()
      -oldNotes_ / newNotes_
      -oldSegments_ / newSegments_
    }
    class PianoKeyAudition {
      +noteOn(midi)
      +noteOff(midi)
      +mixIntoBuffer()
    }
    class PianoRollCorrectionWorker {
      +enqueue(AsyncCorrectionRequest)
      +takeCompleted()
    }
    class PianoRollVisualInvalidationState {
      +merge(Request)
      +clear()
    }

    PianoRollComponent --> PianoRollToolHandler : owns
    PianoRollComponent --> PianoRollRenderer : owns
    PianoRollComponent --> PianoRollCorrectionWorker : owns
    PianoRollComponent --> InteractionState : owns
    PianoRollComponent --> PianoRollVisualInvalidationState : owns
    PianoRollComponent ..> PianoRollEditAction : creates
    PianoRollComponent ..> PianoKeyAudition : references
    PianoRollToolHandler ..> InteractionState : mutates via ctx_.getState()
    PianoRollRenderer ..> "PitchCurveSnapshot" : reads via ctx
    PianoRollEditAction ..> OpenTuneAudioProcessor : commits notes+segments
```

---

## ⚠️ 待确认

1. **`InteractionState::isPanning` / `dragStartPos` 似未被工具处理器真正使用**（Component 用独立的 `dragStartScrollOffset_`），疑为历史遗留字段，考虑清理或明确语义。
2. **`SelectionState::selectionStart/EndMidi` 是 float 而非 int 半音**（代码中 `69.0f + 12*log2(f/440) - 0.5f` 得到的是连续值）。该设计允许选区在半音间，但与基于整数 MIDI 的 lane 绘制可能不完全一致，需确认视觉上的锯齿差距是否在 ±0.5 以内。
3. **`AsyncCorrectionRequest::version` 与 `materializationEpochSnapshot` 同时存在**：代码当前只用 epoch 校验丢弃旧结果，`version` 字段看似预留但未在 worker 中主动比对。是否已废弃？
4. **`NoteDragState::initialManualTargets` 用 `vector<pair<double,float>>`**（稀疏时间点），`previewF0` 则为按帧稠密数组；两者需要保持同步。尚不清楚是否会出现 `initialManualTargets.empty()` 但 `previewF0` 非空的中间态，需确认边界。
5. **`PianoRollVisualPreferences` 结构体本身未被 Component 直接持有**，偏好值分布在多个独立字段（`noteNameMode_ / showChunkBoundaries_ / showUnvoicedFrames_`），后续是否会整合以简化序列化/持久化？
