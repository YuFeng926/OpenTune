---
module: ui-piano-roll
type: api
generated: true
source_scan: true
---

> 本模块无 HTTP Controller，此文档记录钢琴卷帘编辑器 UI 层对外暴露的编程接口契约

# API 接口文档 — ui-piano-roll

## 1. PianoRollComponent

**文件**: `Source/Standalone/UI/PianoRollComponent.h`
**继承**: `juce::Component`, `juce::ScrollBar::Listener`, `juce::Timer`(private）
**命名空间**: `OpenTune`

### 1.1 Listener 接口

```cpp
class Listener {
    virtual ~Listener() = default;
    virtual void playheadPositionChangeRequested(double timeSeconds) = 0;
    virtual void playPauseToggleRequested() = 0;
    virtual void stopPlaybackRequested() = 0;
    virtual void pitchCurveEdited(int startFrame, int endFrame) {}
    virtual void noteOffsetChanged(size_t noteIndex, float oldOffset, float newOffset) {}
    virtual void autoTuneRequested() {}
    virtual void trackTimeOffsetChanged(int trackId, double newOffset) {}
    virtual void escapeKeyPressed() {}
    virtual void playFromPositionRequested(double timeSeconds) {}
};
```

回调说明：
| 回调 | 触发时机 | 参数语义 |
|------|----------|----------|
| `playheadPositionChangeRequested` | 点击/拖拽时间轴标尺 | 绝对时间（秒） |
| `playPauseToggleRequested` | Space 键 | — |
| `stopPlaybackRequested` | Stop 快捷键 | — |
| `pitchCurveEdited` | 任何修正操作完成 | F0 帧范围 [start, end] |
| `noteOffsetChanged` | 音符拖拽移动完成 | 音符索引、旧/新偏移量（半音） |
| `autoTuneRequested` | AutoTune 工具触发或快捷键 6 | — |
| `escapeKeyPressed` | Escape 且无选中音符 | — |
| `playFromPositionRequested` | 双击时间轴/空白区域 | 绝对时间（秒） |

### 1.2 枚举

```cpp
enum class TimeUnit { Seconds, Bars };
enum class ScrollMode { Page, Continuous };
```

### 1.3 常量

| 常量 | 值 | 用途 |
|------|----|------|
| `kContextMenuCommandSelect` | 3001 | 右键菜单 — 选择工具 |
| `kContextMenuCommandDrawNote` | 3002 | 右键菜单 — 绘制音符工具 |
| `kContextMenuCommandHandDraw` | 3003 | 右键菜单 — 手绘工具 |
| `kAudioSampleRate` | 44100 | 内部音频采样率 |

### 1.4 生命周期与依赖注入

```cpp
PianoRollComponent();
~PianoRollComponent() override;

void setPitchCurve(std::shared_ptr<PitchCurve> curve);
void setAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, int sampleRate);
void setGlobalUndoManager(UndoManager* um);
void setProcessor(OpenTuneAudioProcessor* processor);
void setPlayheadPositionSource(std::weak_ptr<std::atomic<double>> source);
```

### 1.5 工具与显示控制

```cpp
void setCurrentTool(ToolId tool);
ToolId getCurrentTool() const;
bool selectToolByContextMenuCommand(int commandId);

void setShowWaveform(bool shouldShow);
void setShowLanes(bool shouldShow);
void setShowOriginalF0(bool show);
void setShowCorrectedF0(bool show);
bool isShowingOriginalF0() const;
```

### 1.6 播放状态与滚动

```cpp
void setIsPlaying(bool playing);
void setZoomLevel(double zoom);            // 范围 [0.02, 10.0]
void setScrollOffset(int offset);
int getScrollOffset() const;
void setScrollMode(ScrollMode mode);
ScrollMode getScrollMode() const;
void fitToScreen();
```

### 1.7 音乐参数

```cpp
void setBpm(double bpm);                   // 范围 [60.0, 240.0]
void setTimeSignature(int numerator, int denominator);
void setTimeUnit(TimeUnit unit);
TimeUnit getTimeUnit() const;
void setScale(int rootNote, int scaleType); // rootNote: 0-11, scaleType: 1=Major 2=Minor 3=Chromatic
void setHopSize(int hopSize);
void setF0SampleRate(double rate);
```

### 1.8 音符编辑参数

```cpp
void setRetuneSpeed(float speed);          // 归一化 [0.0, 1.0]
float getCurrentRetuneSpeed() const;
bool applyRetuneSpeedToSelection(float speed);

void setVibratoDepth(float depth);         // [0.0, 100.0]
float getCurrentVibratoDepth() const;
bool applyVibratoDepthToSelection(float depth);

void setVibratoRate(float rate);           // [0.1, 30.0] Hz
float getCurrentVibratoRate() const;
bool applyVibratoRateToSelection(float rate);

bool getSingleSelectedNoteParameters(float& retuneSpeedPercent, float& vibratoDepth, float& vibratoRate) const;
bool applyCorrectionAsyncForEntireClip(float retuneSpeed, float vibratoDepth, float vibratoRate);
void setNoteSplit(float value);            // cents 阈值
```

### 1.9 音符数据

```cpp
void setNotes(const std::vector<Note>& notes);
std::vector<Note> getNotes() const;
std::shared_ptr<PitchCurve> getPitchCurve() const;
```

### 1.10 Clip 上下文

```cpp
void setCurrentClipContext(int trackId, uint64_t clipId);
void clearClipContext();
bool hasActiveClipContext() const;
int getCurrentTrackId() const;
uint64_t getCurrentClipId() const;
```

### 1.11 偏移与对齐

```cpp
void setTrackTimeOffset(double offsetSeconds);
double getTrackTimeOffset() const;
void setAlignmentOffset(double offsetSeconds);
double getAlignmentOffset() const;
void setPlayheadColour(juce::Colour colour);
```

### 1.12 渲染状态与刷新

```cpp
void setRenderingProgress(float progress, int pendingTasks);
void setInferenceActive(bool active);
void setHasUserAudio(bool hasAudio);
bool isAutoTuneProcessing() const;
bool hasSelectionRange() const;
std::pair<double, double> getSelectionTimeRange() const;

void refreshAfterUndoRedo();
void refreshAfterUndoRedoWithRange(int startFrame, int endFrame);
```

### 1.13 事件处理（JUCE 覆写）

```cpp
void paint(juce::Graphics& g) override;
void resized() override;
bool keyPressed(const juce::KeyPress& key) override;
void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override;
void visibilityChanged() override;
void onHeartbeatTick();  // 外部定时器回调
```

### 1.14 AutoTune

```cpp
bool applyAutoTuneToSelection();  // 需要先选中音符
```

### 1.15 回调函数成员

```cpp
std::function<void(int root, int scaleType)> onKeyDetected;
std::function<void()> onRenderComplete_;
void setRenderCompleteCallback(std::function<void()> cb);
```

### 1.16 Listener 管理

```cpp
void addListener(Listener* listener);
void removeListener(Listener* listener);
```

---

## 2. PianoRollToolHandler

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h`
**命名空间**: `OpenTune`

### 2.1 ManualCorrectionOp

```cpp
struct ManualCorrectionOp {
    int startFrame = 0;
    int endFrameExclusive = 0;
    std::vector<float> f0Data;
    CorrectedSegment::Source source = CorrectedSegment::Source::HandDraw;
    float retuneSpeed = -1.0f;
};
```

### 2.2 Context（回调桥接结构体）

`Context` 包含约 60 个 `std::function` 回调，由 `PianoRollComponent::buildToolHandlerContext()` 构建。核心分组：

| 分组 | 回调 | 说明 |
|------|------|------|
| 坐标转换 | `xToTime`, `timeToX`, `yToFreq`, `freqToY` | 像素↔物理量 |
| 音符操作 | `getNotes`, `getSelectedNotes`, `findNoteAt`, `deselectAllNotes`, `selectAllNotes`, `insertNoteSorted` | 音符 CRUD |
| 曲线操作 | `getPitchCurve`, `getCurveSize`, `clearCorrectionRange`, `clearAllCorrections`, `restoreCorrectedSegment`, `getOriginalF0` | F0 修正 |
| 参数查询 | `getRetuneSpeed`, `getVibratoDepth`, `getVibratoRate`, `getMinMidi`, `getMaxMidi` | 编辑参数 |
| 绘制状态 | `getDirtyStartTime/setDirtyStartTime`, `getDrawingNote*`, `getNoteDragManual*` | 交互状态读写 |
| 通知 | `notifyPlayheadChange`, `notifyPitchCurveEdited`, `notifyAutoTuneRequested`, `notifyPlayPauseToggle`, `notifyStopPlayback`, `notifyEscapeKey`, `notifyNoteOffsetChanged` | 向上层广播 |
| 事务 | `beginEditTransaction`, `commitEditTransaction`, `isTransactionActive` | Undo 事务 |
| 修正 | `applyManualCorrection`, `enqueueNoteBasedCorrection` | 异步修正入口 |

### 2.3 公共方法

```cpp
explicit PianoRollToolHandler(Context context);

void setTool(ToolId tool);
ToolId getTool() const;

void mouseMove(const juce::MouseEvent& e);
void mouseDown(const juce::MouseEvent& e);
void mouseDrag(const juce::MouseEvent& e);
void mouseUp(const juce::MouseEvent& e);
void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel);

bool keyPressed(const juce::KeyPress& key);
void cancelDrag();
```

---

## 3. PianoRollRenderer

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h`
**命名空间**: `OpenTune`

### 3.1 RenderContext

```cpp
struct RenderContext {
    int width, height;
    int pianoKeyWidth = 60;
    int rulerHeight = 30;
    double zoomLevel = 1.0;
    int scrollOffset = 0;
    float pixelsPerSemitone = 15.0f;
    float minMidi = 24.0f;    // C1
    float maxMidi = 108.0f;   // C8
    double bpm = 120.0;
    int timeSigNum = 4, timeSigDenom = 4;
    double trackOffsetSeconds = 0.0;
    double audioSampleRate = 44100.0;
    int hopSize = 512;
    double f0SampleRate = 16000.0;
    int scaleRootNote = 0, scaleType = 1;
    bool showWaveform, showLanes, showOriginalF0, showCorrectedF0;
    bool isRendering; float renderingProgress; bool hasUserAudio;
    bool hasF0Selection; int f0SelectionStartFrame, f0SelectionEndFrame;
    enum class TimeUnit { Seconds, Bars } timeUnit;

    // 坐标转换回调
    std::function<float(float)> midiToY, freqToY, freqToMidi;
    std::function<double(int)> xToTime;
    std::function<int(double)> timeToX;
    std::function<double(double)> clipSecondsToFrameIndex;
    std::function<double(int)> frameIndexToClipSeconds;
};
```

### 3.2 公共绘制方法

```cpp
void drawBackground(juce::Graphics& g, const RenderContext& ctx);
void drawLanes(juce::Graphics& g, const RenderContext& ctx);
void drawWaveform(juce::Graphics& g, const RenderContext& ctx);
void drawTimeRuler(juce::Graphics& g, const RenderContext& ctx);
void drawGridLines(juce::Graphics& g, const RenderContext& ctx);
void drawPianoKeys(juce::Graphics& g, const RenderContext& ctx);
void drawNotes(juce::Graphics& g, const RenderContext& ctx,
               const std::vector<Note>& notes, double trackOffsetSeconds);
void drawF0Curve(juce::Graphics& g, const std::vector<float>& f0,
                 juce::Colour colour, float alpha, bool isThinLine,
                 const RenderContext& ctx, std::shared_ptr<PitchCurve> currentCurve,
                 const std::vector<uint8_t>* visibleMask = nullptr);
```

### 3.3 F0 缓存管理

```cpp
void setWaveformMipmap(WaveformMipmap* mipmap);
WaveformMipmap* getWaveformMipmap() const;
void updateCorrectedF0Cache(std::shared_ptr<const PitchCurveSnapshot> snapshot);
void clearCorrectedF0Cache();
const std::vector<float>& getCorrectedF0Cache() const;
```

---

## 4. PianoRollUndoSupport

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollUndoSupport.h`
**命名空间**: `OpenTune`

### 4.1 Context

```cpp
struct Context {
    std::function<std::vector<Note>()> getNotesCopy;
    std::function<std::shared_ptr<PitchCurve>()> getPitchCurve;
    std::function<uint64_t()> getCurrentClipId;
    std::function<int()> getCurrentTrackId;
    std::function<OpenTuneAudioProcessor*()> getProcessor;
    std::function<UndoManager*()> getUndoManager;
};
```

### 4.2 公共方法

```cpp
explicit PianoRollUndoSupport(Context context);

UndoManager* getCurrentUndoManager() noexcept;

void beginTransaction(const juce::String& description);
void commitTransaction();
bool isTransactionActive() const noexcept;

static bool notesEquivalent(const std::vector<Note>& a, const std::vector<Note>& b);
```

### 4.3 事务语义

- `beginTransaction`: 捕获当前音符列表 + CorrectedSegments 快照作为 "before" 状态
- `commitTransaction`: 比较 before/after，若有变化则生成 `NotesChangeAction` 和/或 `CorrectedSegmentsChangeAction`，推入 `UndoManager`
- 支持 `CompoundUndoAction` 原子组合音符+修正段变更

---

## 5. PianoRollCorrectionWorker

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h`
**命名空间**: `OpenTune`

### 5.1 AsyncCorrectionRequest

```cpp
struct AsyncCorrectionRequest {
    std::shared_ptr<PitchCurve> curve;
    std::vector<Note> notes;
    int startFrame = 0;
    int endFrameExclusive = 0;
    float retuneSpeed = 1.0f;
    float vibratoDepth = 0.0f;
    float vibratoRate = 5.0f;
    double audioSampleRate = 44100.0;
    uint64_t version = 0;
    bool isAutoTuneRequest = false;

    enum class ErrorKind { None, InvalidRange, VersionMismatch, ExecutionError };
    bool success = false;
    std::string errorMessage;
    ErrorKind errorKind = ErrorKind::None;
};
```

### 5.2 公共方法

```cpp
PianoRollCorrectionWorker();
~PianoRollCorrectionWorker();

void enqueue(RequestPtr request);
RequestPtr takeCompleted();
void stop();

void setVersion(uint64_t version);
uint64_t getVersion() const;
uint64_t incrementVersion();

void setClipContextGeneration(uint64_t gen);
uint64_t getClipContextGeneration() const;
void setClipContext(int trackId, uint64_t clipId);
void getClipContext(int& trackId, uint64_t& clipId) const;

static void executeRequest(AsyncCorrectionRequest& request);
```

---

## 6. InteractionState

**文件**: `Source/Standalone/UI/PianoRoll/InteractionState.h`
**命名空间**: `OpenTune`

详见 data-model.md。公共方法列举于此：

```cpp
// SelectionState
void setF0Range(int startFrame, int endFrame);
void clearF0Selection();

// NoteDragState
void clear();

// NoteResizeState
void clear();

// DrawingState
void clearF0Drawing();
void clearNoteDrawing();
void clearAnchors();
```

---

## 7. ToolIds

**文件**: `Source/Standalone/UI/ToolIds.h`

```cpp
enum class ToolId : int {
    AutoTune   = 0,
    Select     = 1,
    DrawNote   = 2,
    LineAnchor = 3,
    HandDraw   = 4
};
```

---

## 8. ToolbarIcons

**文件**: `Source/Standalone/UI/ToolbarIcons.h`

所有方法均为 `static`，返回 `juce::Path`（24x24 viewBox）。

| 方法 | 用途 |
|------|------|
| `getFileIcon()` | 文件/文档图标 |
| `getEditIcon()` | 编辑/钢笔图标 |
| `getEyeIcon()` | 查看/眼睛图标 |
| `getSelectIcon()` | 选择工具（箭头） |
| `getDrawIcon()` | 铅笔/绘制 |
| `getDrawNoteIcon()` | 绘制音符（画笔+方块） |
| `getDrawLineIcon()` | 两点直线 |
| `getLineAnchorIcon()` | 线段锚点 |
| `getHandDrawIcon()` | 手绘曲线 |
| `getAutoTuneIcon()` | 音符/自动调音 |
| `getCurveIcon()` | 正弦波/音高曲线 |
| `getCutIcon()` | 剪刀/裁剪 |
| `getEraseIcon()` | 橡皮擦 |
| `getWaveformIcon()` | 波形条 |
| `getTrackViewIcon()` | 轨道视图 |
| `getPianoViewIcon()` | 钢琴卷帘视图 |
| `getTracksIcon()` | 轨道列表 |
| `getPropsIcon()` | 属性/滑块 |
| `getPanelLeftIcon()` | 左面板切换 |
| `getPanelRightIcon()` | 右面板切换 |
| `getPlayIcon()` / `getPauseIcon()` / `getStopIcon()` | 播放控制 |
| `getLoopIcon()` / `getViewIcon()` | 循环/视图 |
| `getBpmIcon()` / `getTapIcon()` | BPM 相关 |
| `getKeyIcon()` / `getScaleIcon()` | 调性/音阶 |

工具方法：

```cpp
static juce::Image createIconImage(const juce::Path& path, juce::Colour color);  // 16x16 图像
static void drawIcon(juce::Graphics& g, const juce::Path& path,
                     juce::Rectangle<float> bounds, juce::Colour color,
                     float strokeThickness = 2.0f, bool filled = false);
```
