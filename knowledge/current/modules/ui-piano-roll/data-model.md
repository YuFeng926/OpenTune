---
module: ui-piano-roll
type: data-model
generated: true
source_scan: true
---

# 数据模型文档 — ui-piano-roll

## 1. ToolId 枚举

**文件**: `Source/Standalone/UI/ToolIds.h:5`

```cpp
enum class ToolId : int {
    AutoTune   = 0,   // 自动调音（点击触发，不持续交互）
    Select     = 1,   // 选择工具（框选、点选、拖拽移动、边缘调整）
    DrawNote   = 2,   // 绘制音符（拖拽创建矩形音符块）
    LineAnchor = 3,   // 线段锚点（多次点击放置锚点，锚点间 log2 插值 F0）
    HandDraw   = 4    // 手绘 F0（按住拖拽自由绘制 F0 曲线）
};
```

## 2. InteractionState 体系

**文件**: `Source/Standalone/UI/PianoRoll/InteractionState.h`

### 2.1 NoteResizeEdge

```cpp
enum class NoteResizeEdge {
    None,    // 未在调整
    Left,    // 调整左边缘（startTime）
    Right    // 调整右边缘（endTime）
};
```

### 2.2 SelectionState

```cpp
struct SelectionState {
    // 框选拖拽（仅 isSelectingArea==true 时有效）
    bool isSelectingArea = false;
    double dragStartTime = 0.0;       // 框选起点（clip-relative 秒）
    double dragEndTime = 0.0;         // 框选终点
    float dragStartMidi = 0.0f;       // 框选起点 MIDI 音高
    float dragEndMidi = 0.0f;         // 框选终点 MIDI 音高

    // 从选中音符派生的 F0 帧范围
    int selectedF0StartFrame = -1;
    int selectedF0EndFrame = -1;
    bool hasF0Selection = false;

    void setF0Range(int startFrame, int endFrame);
    void clearF0Selection();
};
```

**关键语义**：
- `dragStart*/dragEnd*` 仅在拖拽过程中有意义，是瞬态数据
- `selectedF0*Frame` 由 `updateF0SelectionFromNotes()` 从 `Note.selected` 推导，是持久数据
- F0 选区用于渲染高亮和 RenderContext 传递

### 2.3 NoteDragState

```cpp
struct NoteDragState {
    Note* draggedNote = nullptr;                          // 拖拽发起的音符指针
    std::vector<std::pair<Note*, float>> initialNoteOffsets; // 所有选中音符的初始 pitchOffset
    bool isDraggingNotes = false;                         // 是否已开始实际拖拽（超过阈值）

    // 手动修正回退数据（拖拽音符时同步移动已有 F0 修正）
    double manualStartTime = -1.0;
    double manualEndTime = -1.0;
    std::vector<std::pair<double, float>> initialManualTargets;  // (time, f0Hz)

    void clear();
};
```

**关键语义**：
- 拖拽开始时记录所有选中音符的 `pitchOffset` 快照
- 若选中音符区域有已有 F0 修正（`hasCorrectionInRange`），同步保存渲染后的 F0 数据到 `initialManualTargets`
- 拖拽过程中，音符 pitchOffset 和 F0 修正数据同步按半音偏移

### 2.4 NoteResizeState

```cpp
struct NoteResizeState {
    bool isResizing = false;
    bool isDirty = false;                  // 是否发生了实际变更（用于触发 undo）
    Note* note = nullptr;                  // 正在调整的音符指针
    NoteResizeEdge edge = NoteResizeEdge::None;
    double originalStartTime = 0.0;        // 调整前的 startTime
    double originalEndTime = 0.0;          // 调整前的 endTime

    void clear();
};
```

**关键语义**：
- 最小音符时长 `minDuration = 0.02` 秒
- 调整完成时，若 `isDirty`，触发 `enqueueNoteBasedCorrection` 异步重算修正
- 受影响帧范围取 `min(original, resized)` 到 `max(original, resized)`

### 2.5 DrawingState

```cpp
struct DrawingState {
    // === HandDraw 手绘 F0 ===
    bool isDrawingF0 = false;
    std::vector<float> handDrawBuffer;   // 大小=originalF0.size(), -1.0=未绘制, >0=绘制值
    double dirtyStartTime = -1.0;        // 脏区起点（clip-relative 秒）
    double dirtyEndTime = -1.0;          // 脏区终点
    juce::Point<float> lastDrawPoint;    // 上一帧绘制位置 (time, freq)

    // === DrawNote 音符绘制 ===
    bool isDrawingNote = false;
    double drawingNoteStartTime = 0.0;
    double drawingNoteEndTime = 0.0;
    float drawingNotePitch = 0.0f;       // 已对齐到半音的频率
    int drawingNoteIndex = -1;           // 在 notes vector 中的临时索引

    // === LineAnchor 锚点放置 ===
    bool isPlacingAnchors = false;
    std::vector<LineAnchor> pendingAnchors;    // 已放置的锚点序列
    juce::Point<float> currentMousePos;        // 当前鼠标位置（用于实时预览线段）

    void clearF0Drawing();
    void clearNoteDrawing();
    void clearAnchors();
};
```

**关键语义**：
- `handDrawBuffer` 帧级缓冲，与 `originalF0` 等长；`-1.0f` 表示该帧未被绘制
- HandDraw 和 LineAnchor 的 F0 数据在 mouseUp/commit 时通过 `clipDrawDataToNotes()` 裁剪到音符边界
- LineAnchor 的锚点间使用 log2 空间线性插值

### 2.6 InteractionState（聚合类）

```cpp
class InteractionState {
public:
    SelectionState selection;
    NoteDragState noteDrag;
    NoteResizeState noteResize;
    DrawingState drawing;

    bool isPanning = false;                      // Space+拖拽平移
    juce::Point<int> dragStartPos;               // 平移起始鼠标位置

    bool drawNoteToolPendingDrag = false;        // DrawNote 工具等待拖拽阈值
    juce::Point<int> drawNoteToolMouseDownPos;   // DrawNote 鼠标按下位置
    bool handDrawPendingDrag = false;            // HandDraw 工具等待拖拽阈值
};
```

## 3. ManualCorrectionOp

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h:18`

```cpp
struct ManualCorrectionOp {
    int startFrame = 0;           // 起始帧（含）
    int endFrameExclusive = 0;    // 结束帧（不含）
    std::vector<float> f0Data;    // F0 值（Hz），长度 = endFrameExclusive - startFrame
    CorrectedSegment::Source source = CorrectedSegment::Source::HandDraw;
    float retuneSpeed = -1.0f;    // -1 表示使用直接绘制值，不做 retune 混合
};
```

**用途**：HandDraw 和 LineAnchor 工具生成的逐帧 F0 修正指令。通过 `clipDrawDataToNotes()` 裁剪后，每个被覆盖的音符产生一个独立的 `ManualCorrectionOp`。

## 4. PianoRollRenderer::RenderContext

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h:32`

```
RenderContext 是纯值 + 回调的快照结构，每次 paint() 调用时由 buildRenderContext() 构建。
不持有任何可变状态的引用，确保渲染时参数一致性。
```

核心字段分组：

| 分组 | 字段 | 说明 |
|------|------|------|
| 布局 | `width`, `height`, `pianoKeyWidth(60)`, `rulerHeight(30)` | 组件像素尺寸 |
| 缩放 | `zoomLevel`, `scrollOffset`, `pixelsPerSemitone` | 视口变换 |
| 音高范围 | `minMidi(24=C1)`, `maxMidi(108=C8)` | MIDI 范围 |
| 音乐 | `bpm`, `timeSigNum`, `timeSigDenom`, `timeUnit` | 时间单位 |
| 音频 | `trackOffsetSeconds`, `audioSampleRate(44100)`, `hopSize(512)`, `f0SampleRate(16000)` | 帧↔时间转换 |
| 调性 | `scaleRootNote`, `scaleType` | 音阶根音与类型 |
| 显示 | `showWaveform`, `showLanes`, `showOriginalF0`, `showCorrectedF0`, `hasUserAudio` | 开关 |
| 渲染 | `isRendering`, `renderingProgress` | 进度动画 |
| 选区 | `hasF0Selection`, `f0SelectionStartFrame`, `f0SelectionEndFrame` | F0 帧选区高亮 |
| 回调 | `midiToY`, `freqToY`, `freqToMidi`, `xToTime`, `timeToX`, `clipSecondsToFrameIndex`, `frameIndexToClipSeconds` | 坐标转换 |

## 5. PianoRollUndoSupport::Context

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollUndoSupport.h:26`

```cpp
struct Context {
    std::function<std::vector<Note>()> getNotesCopy;          // 深拷贝当前音符列表
    std::function<std::shared_ptr<PitchCurve>()> getPitchCurve;
    std::function<uint64_t()> getCurrentClipId;
    std::function<int()> getCurrentTrackId;
    std::function<OpenTuneAudioProcessor*()> getProcessor;
    std::function<UndoManager*()> getUndoManager;
};
```

## 6. AsyncCorrectionRequest

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h:28`

```cpp
struct AsyncCorrectionRequest {
    // === 输入 ===
    std::shared_ptr<PitchCurve> curve;
    std::vector<Note> notes;               // 深拷贝，工作线程安全读取
    int startFrame, endFrameExclusive;
    float retuneSpeed, vibratoDepth, vibratoRate;
    double audioSampleRate;
    uint64_t version;
    bool isAutoTuneRequest;

    // === 输出 ===
    bool success;
    std::string errorMessage;
    enum class ErrorKind { None, InvalidRange, VersionMismatch, ExecutionError } errorKind;
};
```

## 7. 坐标系约定

### 7.1 频率 ↔ MIDI

```
freqToMidi(f) = 12 * log2(f / 440) + 69 - 0.5
midiToFreq(m) = 440 * 2^((m + 0.5 - 69) / 12)
```

**注意**：`-0.5` 偏移使 MIDI 值对应半音中心线（lane center），而非键边界。

### 7.2 MIDI ↔ Y 像素

```
midiToY(m) = (maxMidi - m) * pixelsPerSemitone - verticalScrollOffset
yToMidi(y) = maxMidi - (y + verticalScrollOffset) / pixelsPerSemitone
```

Y 轴向下增长 → 高音在上。

### 7.3 时间 ↔ X 像素

```
timeToX(s) = timeConverter.timeToPixel(s - trackOffset + alignmentOffset) + pianoKeyWidth
xToTime(x) = timeConverter.pixelToTime(x - pianoKeyWidth) + trackOffset - alignmentOffset
```

`pianoKeyWidth = 60` 像素固定偏移。

### 7.4 时间 ↔ F0 帧

```
clipSecondsToFrameIndex(s) = floor(s / frameDuration)
frameDuration = hopSize / f0SampleRate    // 默认 512/16000 = 0.032s = 31.25fps
```

> ⚠️ AGENTS.md 记载 RMVPE 帧率为 100fps (hopSize=160 @ 16kHz)，但 `PianoRollComponent` 默认 `hopSize_=512, f0SampleRate_=16000`。实际值在 `setPitchCurve` 时从 `PitchCurve` 元数据覆盖。

## 8. 布局常量

| 常量 | 值 | 用途 |
|------|----|------|
| `pianoKeyWidth_` | 60 px | 左侧琴键区宽度 |
| `rulerHeight_` | 30 px | 顶部时间标尺高度 |
| `timelineExtendedHitArea_` | 20 px | 标尺下方扩展点击区 |
| `dragThreshold_` | 5 px | 区分点击/拖拽的最小移动距离 |
| `minMidi_` | 24.0 (C1) | MIDI 下限 |
| `maxMidi_` | 108.0 (C8) | MIDI 上限 |
| `pixelsPerSemitone_` | 25.0（初始） | 垂直缩放，范围 [5.0, 60.0] |
