---
spec_version: 1.0.0
status: draft
module: ui-piano-roll
doc_type: api
generated_by: arch-doc-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

> 本模块无 HTTP/RPC 接口，此文档记录钢琴卷帘 UI 层对外暴露的 C++ 公共接口契约。所有类位于 `namespace OpenTune`。

# ui-piano-roll API 契约

## 1. PianoRollComponent

**文件**: `Source/Standalone/UI/PianoRollComponent.h` / `.cpp`
**继承**: `juce::Component`, `juce::ScrollBar::Listener`

顶层协调者。拥有 `ToolHandler / Renderer / CorrectionWorker / WaveformMipmap / PlayheadOverlayComponent` 和一个 `InteractionState`。

### 1.1 嵌套类型

```cpp
class Listener {
public:
    virtual ~Listener() = default;
    virtual void playheadPositionChangeRequested(double timeSeconds) = 0;
    virtual void playPauseToggleRequested() = 0;
    virtual void stopPlaybackRequested() = 0;
    virtual void pitchCurveEdited(int startFrame, int endFrame) {}
    virtual void noteOffsetChanged(size_t noteIndex, float oldOffset, float newOffset) {}
    virtual void autoTuneRequested() {}
    virtual void escapeKeyPressed() {}
    virtual void undoRequested() {}
    virtual void redoRequested() {}
};

enum class TimeUnit { Seconds, Bars };
enum class ScrollMode { Page, Continuous };

static constexpr int kAudioSampleRate = 44100;
```

| 回调 | 触发时机 | 参数语义 |
|------|---------|---------|
| `playheadPositionChangeRequested` | 时间标尺点击、拖拽；空白区域单击 | 时间轴绝对秒数（未 clamp，≥0） |
| `playPauseToggleRequested` | Space 快捷键 | — |
| `stopPlaybackRequested` | Stop 快捷键 | — |
| `pitchCurveEdited` | 任何修正编辑成功提交后 | F0 帧闭区间 `[startFrame, endFrame]` |
| `noteOffsetChanged` | 音符拖拽释放且 offset 变化 >0.001 半音 | 索引、旧/新 `pitchOffset`（半音） |
| `autoTuneRequested` | 快捷键 6 或 AutoTune 工具点击 | — |
| `escapeKeyPressed` | Escape 键 | — |
| `undoRequested` / `redoRequested` | 对应快捷键（由上层监听转发到 `UndoManager`） | — |

### 1.2 生命周期 / JUCE 覆写

| 签名 | 说明 |
|------|------|
| `PianoRollComponent()` | 构造：顺序执行 `initializeUIComponents → initializeRenderer → initializeCorrectionWorker → initializeToolHandler`；设置 `setWantsKeyboardFocus(true)`，挂 VBlank 回调用于平滑滚动 |
| `~PianoRollComponent() override` | 析构先 `correctionWorker_->stop()`；重置 VBlank attachment；移除 ScrollBar 监听 |
| `void paint(juce::Graphics&) override` | 顶层绘制：`buildRenderContext` → `drawTimeRuler / drawGridLines / drawUnvoicedFrameBands / drawWaveform / drawLanes / drawNotes / drawF0Curve(原始+修正) / drawSelected* / drawHandDrawPreview / drawLineAnchorPreview / drawChunkBoundaries / drawPianoKeys / drawSelectionBox`；使用主题 ID 区分 `DarkBlueGrey` 的频谱背景 |
| `void resized() override` | 安放横/纵滚动条（15 px）、`scrollMode` / `timeUnit` 切换按钮、`playheadOverlay_` 占满 local bounds |
| `void visibilityChanged() override` | （仅声明，实现重置部分瞬态） |
| `bool keyPressed(const juce::KeyPress&) override` | 转发到 `toolHandler_->keyPressed`；未消费则返回 false |
| `void mouseMove/Down/Drag/Up/WheelMove(...) override` | 分发到 `toolHandler_` 或自身滚动缩放处理 |
| `void scrollBarMoved(ScrollBar*, double) override` | 处理横/纵滚动条位置变化 |
| `void onHeartbeatTick()` | 每帧（由外部驱动）：`consumeCompletedCorrectionResults` + waveform 增量构建 + `flushPendingVisualInvalidation` |

### 1.3 依赖注入

| 签名 | 契约 |
|------|------|
| `void setProcessor(OpenTuneAudioProcessor*)` | 绑定 Processor 并 `refreshEditedMaterializationNotes`。传入 `nullptr` 安全，后续 commit 都会短路返回 false |
| `void setEditedMaterialization(uint64_t id, std::shared_ptr<PitchCurve>, std::shared_ptr<const juce::AudioBuffer<float>>, int sampleRate)` | 切换被编辑素材：若 `id` 变化则清空 Draft/Undo 快照并自增 `editedMaterializationEpoch_`；必要时 `fitToScreen`；最终 `invalidateVisual(Content)` |
| `void setPianoKeyAudition(PianoKeyAudition*)` | 绑定试听回调对象。仅存指针，不拥有 |
| `void setMaterializationProjection(const MaterializationTimelineProjection&)` | 仅在 `timelineStart/timelineDuration/materializationDuration` 任一差值 > 1e-9 时生效；同时更新 `playheadOverlay_` 与 `invalidateVisual(Viewport, Interactive)` |
| `void setPlayheadPositionSource(std::weak_ptr<std::atomic<double>>)` | 由 VBlank/heartbeat 读取的原子时间源 |
| `void addListener(Listener*) / removeListener(Listener*)` | Listener 集合管理；通过 `juce::ListenerList` 广播 |

### 1.4 状态 setter（全部伴随 `invalidateVisual`）

| 签名 | 默认值 | 失效原因 |
|------|-------|---------|
| `setZoomLevel(double)` | 1.0，clamp 到 [0.02, 10.0] | Viewport, Interactive |
| `setCurrentTool(ToolId)` | Select | Interaction, Interactive（fullRepaint 当工具切换时）；同时变更 `MouseCursor` 并清理 LineAnchor 预览 |
| `setScrollOffset(int)` | 0，clamp ≥0 | Viewport, Interactive；若 delta < contentWidth 则用平移脏区域 |
| `setScrollMode(ScrollMode)` | Continuous | Viewport, Interactive |
| `setShowWaveform/setShowLanes/setNoteNameMode/setShowChunkBoundaries/setShowUnvoicedFrames` | true / true / COnly / false / false | Content |
| `setShowOriginalF0/setShowCorrectedF0` | true / true | Content |
| `setInferenceActive(bool)` | false | 不直接失效；影响波形增量构建节奏 |
| `setBpm(double) / setTimeSignature(int,int) / setTimeUnit(TimeUnit)` | 120 / 4,4 / Seconds | 仅最后一个触发 Viewport |
| `setScale(int rootNote, int scaleType)` | 0 / 1（Major） | Content |
| `setAudioEditingScheme(AudioEditingScheme::Scheme)` | CorrectedF0Primary | 无直接失效 |
| `setZoomSensitivity / setShortcutSettings` | `getDefault()` | — |
| `setRetuneSpeed / setVibratoDepth / setVibratoRate` | 来自 `PitchControlConfig` | 仅保存，不失效 |
| `setNoteSplit(float)` | 透传 `NoteSegmentationPolicy::transitionThresholdCents`，clamp 到 `[kMinNoteSplitCents, kMaxNoteSplitCents]` | Content |
| `setPlayheadColour(juce::Colour)` | 0xFFE74C3C | 由 overlay 重绘 |

### 1.5 选区/参数读写

| 签名 | 行为 |
|------|------|
| `bool hasSelectionArea() const` | 是否存在矩形选区（基于 `selectionStartTime != selectionEndTime`） |
| `std::pair<double,double> getSelectionTimeRange() const` | 返回 `{min,max}` 的时间范围（素材秒） |
| `bool applyRetuneSpeedToSelection(float)` / `applyRetuneSpeedToSelectedLineAnchorSegments(float)` | 根据 `AudioEditingScheme::resolveParameterTarget` 决定落点：选中音符 / 线段 / 帧选区；clamp 到 `[0,1]`；设 `pendingUndoDescription_ = "修改调速"` |
| `bool applyVibratoDepthToSelection(float)` / `applyVibratoRateToSelection(float)` | Depth clamp `[0,100]`；Rate clamp `[0.1,30]` |
| `bool applyVibratoParameterToSelection(VibratoParam, float)` *(private)* | 内部分派 Depth/Rate |
| `bool getSingleSelectedNoteParameters(float&,float&,float&)` | 仅当恰好 1 个选中音符时返回 true；百分比形式的 retune 和 depth/rate |
| `bool getSelectedSegmentRetuneSpeed(float&)` | 仅当方案允许选中线段且恰好 1 段被选时返回 |
| `bool applyAutoTuneToSelection()` | 触发一次性 AutoTune（内部走 `applyCorrectionAsyncForEntireClip` 或选区范围） |
| `bool applyCorrectionAsyncForEntireClip(float retune,float depth,float rate)` | 入队 `PianoRollCorrectionWorker::AsyncCorrectionRequest::Kind::ApplyNoteRange` 覆盖整个 clip；若 `isAutoTuneProcessing()` 为 true 则返回 false |

### 1.6 LineAnchor 段选中

| 签名 | 行为 |
|------|------|
| `int findLineAnchorSegmentNear(int x, int y) const` | 在像素容差 15 px 内寻找最靠近鼠标 y 的 LineAnchor 段；无命中返回 -1 |
| `void selectLineAnchorSegment(int idx)` | 独占选中，先清空 |
| `void toggleLineAnchorSegmentSelection(int idx)` | 加/去 |
| `void clearLineAnchorSegmentSelection()` | 清空 |

### 1.7 渲染失效 API

```cpp
void invalidateVisual(const PianoRollVisualInvalidationRequest&);
void invalidateVisual(uint32_t reasonsMask,
                      PianoRollVisualInvalidationPriority priority = Normal);
void invalidateVisual(uint32_t reasonsMask,
                      const juce::Rectangle<int>& dirtyArea,
                      PianoRollVisualInvalidationPriority priority = Interactive);
void flushPendingVisualInvalidation();
```

- 第一种重载 `fullRepaint=true`，适用于整体内容变更；
- 第二种 `fullRepaint = dirtyArea.isEmpty()`，用于局部重绘；
- `priority=Interactive` 且组件可见时，若距上次 flush >= 1000/60 ms 立即 flush，否则合并等待 heartbeat；
- `flushPendingVisualInvalidation` 调用 `makeVisualFlushDecision` 并最终 `FrameScheduler::requestInvalidate`。

### 1.8 异步修正闭环（内部）

| 签名（private） | 说明 |
|----|----|
| `bool enqueueManualCorrectionPatchAsync(ops, dirtyStart, dirtyEnd, trigger)` | 克隆当前 PitchCurve，对每个 `ManualCorrectionOp` 执行 `setManualCorrectionRange`；原子 `commitEditedMaterializationCorrectedSegments`；可选广播 `pitchCurveEdited` |
| `void enqueueNoteBasedCorrectionAsync(notes, start, endExcl, retune, depth, rate)` | 克隆 curve，构造 `ApplyNoteRange` 请求，附带 `materializationIdSnapshot/materializationEpochSnapshot`，入队 worker |
| `void consumeCompletedCorrectionResults()` | 由 heartbeat 调用；取一条已完成请求，按 `Kind` 分别调 `commitCompletedAutoTuneResult / commitCompletedNoteCorrectionResult`；成功则广播 `pitchCurveEdited`；AutoTune 完成后将 `autoTuneInFlight_` 置 false |

### 1.9 Undo 事务（内部）

| 签名（private） | 说明 |
|----|----|
| `void captureBeforeUndoSnapshot()` | 复制 `cachedNotes_` 与 `getCurrentSegments()` 到 `beforeUndoNotes_/Segments_`；`undoSnapshotCaptured_ = true` |
| `void recordUndoAction(const juce::String& description)` | 构造 `PianoRollEditAction(processor, materializationId, desc, beforeNotes, afterNotes, beforeSegs, afterSegs)` 并 `processor_->getUndoManager().addAction` |
| `bool commitEditedMaterializationNotes(notes)` | 若未捕获则自动捕获快照 → `processor_->setMaterializationNotesById` → refresh → `recordUndoAction` |
| `bool commitEditedMaterializationNotesAndSegments(notes, segments)` | 同上，改走 `commitMaterializationNotesAndSegmentsById`，一次事务 |
| `bool commitEditedMaterializationCorrectedSegments(segments)` | 仅 F0 段；同样产生一条 Undo |

### 1.10 Draft API（内部）

| 签名 | 说明 |
|------|------|
| `beginNoteDraft()` | `interactionState_.noteDraft.active = true; baseline = working = cachedNotes_` |
| `bool commitNoteDraft()` | 若 active 则 `commitEditedMaterializationNotes(workingNotes)`；成功则 clear draft |
| `clearNoteDraft()` | 丢弃 draft，不写回 |
| `const std::vector<Note>& getCommittedNotes() const` | 总是返回 `cachedNotes_` |
| `const std::vector<Note>& getDisplayedNotes() const` | active 时返回 `workingNotes`，否则 `cachedNotes_` |

### 1.11 坐标/投影辅助（内部）

```cpp
float midiToY(float), yToMidi(float);
float freqToMidi(float), midiToFreq(float);
float yToFreq(float), freqToY(float);
double toVisibleTimelineSeconds(double), toAbsoluteTimelineSeconds(double);
double projectTimelineTimeToMaterialization(double), projectMaterializationTimeToTimeline(double);
double getTimelinePixelsPerSecond();
double getPlayheadAbsolutePixelX(double playheadTimeSeconds);
int timeToX(double), double xToTime(int);
juce::Rectangle<int> getNoteBounds(const Note&), getNotesBounds(...), getSelectionBounds();
juce::Rectangle<int> getHandDrawPreviewBounds(), getLineAnchorPreviewBounds(), getNoteDragCurvePreviewBounds();
```

---

## 2. PianoRollToolHandler

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h` / `.cpp`

无状态音频数据，仅靠 `Context` 回调访问外部数据。

### 2.1 ManualCorrectionOp

```cpp
struct ManualCorrectionOp {
    int startFrame = 0;
    int endFrameExclusive = 0;
    std::vector<float> f0Data;                      // 长度 == endFrameExclusive - startFrame
    CorrectedSegment::Source source = HandDraw;     // HandDraw / LineAnchor
    float retuneSpeed = -1.0f;                      // -1 表示使用默认 retune
};
```

### 2.2 Context 回调结构体（节选）

由 `PianoRollComponent::buildToolHandlerContext()` 装配。以下是主要组：

| 组 | 回调 | 返回 / 副作用 |
|----|------|-------------|
| **状态访问** | `getState()` | `InteractionState&` |
| **坐标** | `xToTime/timeToX/yToFreq/freqToY` | 像素↔时间/频率 |
| **音符** | `getCommittedNotes/getDisplayNotes/getNoteDraft/beginNoteDraft/commitNoteDraft/clearNoteDraft/commitNotesAndSegments` | Draft 生命周期 |
| **PitchCurve** | `getPitchCurve/getOriginalF0/getF0Timeline` | 曲线、原始 F0、时间线 |
| **配置** | `getMinMidi/getMaxMidi/getRetuneSpeed/getVibratoDepth/getVibratoRate/getAudioEditingScheme/getShortcutSettings` | 只读 |
| **PIP** | `recalculatePIP(Note&)` | 重新计算音符的 `originalPitch` |
| **投影** | `getMaterializationProjection/projectTimelineTimeToMaterialization/projectMaterializationTimeToTimeline` | 时间轴映射 |
| **视觉** | `invalidateVisual(rect)/setMouseCursor/grabKeyboardFocus` | 触发重绘与指针 |
| **工具** | `setCurrentTool(ToolId)/showToolSelectionMenu()` | 切工具 / 弹右键菜单 |
| **通知** | `notifyPlayheadChange/notifyPitchCurveEdited/notifyAutoTuneRequested/notifyPlayPauseToggle/notifyStopPlayback/notifyEscapeKey/notifyNoteOffsetChanged` | 广播 Listener |
| **手动修正** | `applyManualCorrection(ops, startFrame, endFrame, triggerRenderEvent)` | 同步 clone curve → commit；返回 true 表示成功 |
| **选择辅助** | `selectNotesOverlappingFrames(start, endExcl)` | 返回是否有重叠 |
| **LineAnchor 段** | `findLineAnchorSegmentNear/selectLineAnchorSegment/toggleLineAnchorSegmentSelection/clearLineAnchorSegmentSelection` | 段选中辅助 |
| **Undo** | `setUndoDescription(juce::String)` | 写入 `pendingUndoDescription_`，下一次 commit 使用 |
| **NoteDrag 预览** | `getNoteDragManualStartTime/...PreviewF0/...PreviewStartFrame/...PreviewEndFrameExclusive` 及对应 setter | 拖动音符时的 F0 预览缓冲 |

### 2.3 构造与公共方法

```cpp
explicit PianoRollToolHandler(Context context);
void setTool(ToolId tool);
void mouseMove/mouseDown/mouseDrag/mouseUp(const juce::MouseEvent&);
bool keyPressed(const juce::KeyPress&);
```

### 2.4 工具分派契约（private 但定义状态机）

| 工具 | Down | Drag | Up |
|------|------|------|----|
| `AutoTune` | `notifyAutoTuneRequested` | — | — |
| `Select` | 命中边缘 → 进入 Resize；命中音符 → 选中并可能进入 NoteDrag；未命中 → 启动框选 | 按状态走 Resize / Drag / 框选 | 有编辑则 commit 音符与 F0 段；清 Resize/NoteDrag，如选区退化则清 `hasSelectionArea` |
| `DrawNote` | `handleDrawNoteMouseDown`（标记 pendingDrag） | 阈值过后调用 `handleDrawNoteTool` 插入并更新绘制中音符 | `handleDrawNoteUp`：强制最小时长 0.02 s、分割重叠音符、重算 PIP、原子 commit (notes, segments) |
| `HandDraw` | 记录 pendingDrag | 阈值过后持续调用 `handleDrawCurveTool`，对帧间 log2 插值写入 `handDrawBuffer` | `handleDrawCurveUp`：调用 `applyManualCorrection` 生成 `CorrectedSegment::Source::HandDraw` |
| `LineAnchor` | 首次按下放第一个锚点；第二次起按 log2 插值生成 `CorrectedSegment::Source::LineAnchor`；双击 commit；右键取消 | 仅更新鼠标预览位置 | — |

### 2.5 快捷键契约（`keyPressed`）

| 条件 | 行为 |
|------|------|
| `KeyShortcutConfig::SelectAll` 匹配 | 选中所有音符 + 将选区扩展到 F0Timeline 全长 |
| 无修饰键、字符 `'2'/'3'/'4'/'6'` | 分别切到 `DrawNote/Select/LineAnchor`，或触发 AutoTune |
| `PlayPause/Stop` 匹配 | 分别通知播放/停止 |
| `Delete` 匹配或字符 `'1'` | `handleDeleteKey`：删选中音符 + 擦除选区的修正 F0，原子 commit 并广播 `pitchCurveEdited` |
| `Escape` | `notifyEscapeKey` |

### 2.6 Note 选择静态辅助

```cpp
static int findNoteIndexAt(const std::vector<Note>&, double time, float targetHz, float toleranceHz);
static std::vector<int> collectSelectedNoteIndices(const std::vector<Note>&);
static void deselectAllNotes(std::vector<Note>&);
static void selectAllNotes(std::vector<Note>&);
static int findLastSelectedNoteIndex(const std::vector<Note>&);
static void selectNotesBetween(std::vector<Note>&, int start, int end);
```

---

## 3. PianoRollRenderer

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h` / `.cpp`

纯 pull 式绘制器：调用者构造一次性 `RenderContext` 并顺序调用 draw* 方法。

### 3.1 RenderContext

```cpp
struct RenderContext {
    int width, height;
    int pianoKeyWidth = 60;
    int rulerHeight  = 30;
    double pixelsPerSecond = 100.0;
    float pixelsPerSemitone = 15.0f;
    float minMidi = 24.0f, maxMidi = 108.0f;
    double bpm = 120.0;
    MaterializationTimelineProjection materializationProjection;
    F0Timeline f0Timeline;
    int scaleRootNote = 0;
    int scaleType = 1;
    NoteNameMode noteNameMode = COnly;
    bool showLanes = true, showChunkBoundaries = false, showUnvoicedFrames = false;
    int pressedPianoKey = -1;
    bool hasUserAudio = false;
    std::vector<double> chunkBoundaries;
    std::shared_ptr<const PitchCurveSnapshot> pitchSnapshot;

    bool hasF0Selection = false;
    int  f0SelectionStartFrame = -1;
    int  f0SelectionEndFrameExclusive = -1;

    enum class TimeUnit { Seconds, Bars } timeUnit = Seconds;

    std::function<float(float)> midiToY, freqToY, freqToMidi;
    std::function<double(int)>  xToTime;
    std::function<int(double)>  timeToX;
};
```

### 3.2 绘制入口

| 签名 | 作用 |
|------|------|
| `setWaveformMipmap(WaveformMipmap*)` | 注入多级波形缓存（不转移所有权） |
| `drawLanes(g, ctx)` | 按 `scaleType/scaleRootNote` 高亮在音阶内的 lane；黑白键条带 |
| `drawUnvoicedFrameBands(g, ctx)` | `showUnvoicedFrames` 开启时在清音帧上绘竖带 |
| `drawWaveform(g, ctx)` | 使用 mipmap 按 `pixelsPerSecond` 选层绘制 |
| `drawTimeRuler(g, ctx)` | 按 `timeUnit` 选秒或拍间隔；使用 `selectBeatInterval / selectMarkerInterval` |
| `drawGridLines(g, ctx)` | 节拍竖线（pixelsPerBeat 自适应） |
| `drawChunkBoundaries(g, ctx)` | `showChunkBoundaries` 时绘出 chunkBoundaries |
| `drawPianoKeys(g, ctx)` | 左侧琴键，黑白区分；按 `noteNameMode` 在 C 或所有音画音名 |
| `drawNotes(g, ctx, notes)` | 音符矩形 + 选中描边 |
| `drawF0Curve(g, f0, colour, alpha, thin, ctx, curve, visibleMask=nullptr)` | 绘制 F0 折线；`visibleMask==nullptr` 时绘制全部 voiced 帧 |
| `updateCorrectedF0Cache(snapshot)` | 增量生成 `correctedF0Cache_`（仅当 `hasAnyCorrection()`） |
| `clearCorrectedF0Cache()` | 清空缓存 |
| `const std::vector<float>& getCorrectedF0Cache()` | 返回缓存，用于绘制 |

### 3.3 绘制顺序（由 Component::paint 决定）

`timeRuler → [clip: viewport-pianoKey]{gridLines → unvoicedBands → waveform → lanes → notes → originalF0 → correctedF0(cached) → noteDragPreview → handDrawPreview → lineAnchorPreview → chunkBoundaries} → pianoKeys → selectionBox`

---

## 4. PianoRollVisualInvalidation

**文件**: `Source/Standalone/UI/PianoRoll/PianoRollVisualInvalidation.h` / `.cpp`

独立的失效聚合与调度决策模块。

### 4.1 枚举

```cpp
enum class PianoRollVisualInvalidationReason : uint32_t {
    Interaction = 1u << 0,   // 选区框、音符拖拽、预览
    Viewport    = 1u << 1,   // 滚动/缩放
    Content     = 1u << 2,   // 音符、F0 数据变更
    Playhead    = 1u << 3,   // 播放头移动
    Decoration  = 1u << 4    // 主题/装饰
};

enum class PianoRollVisualInvalidationPriority : int {
    Background = 0,   // 对齐 FrameScheduler::Priority
    Normal     = 1,
    Interactive= 2
};
```

### 4.2 结构体

```cpp
struct PianoRollVisualInvalidationRequest {
    uint32_t reasonsMask = 0;
    bool fullRepaint = false;
    bool hasDirtyArea = false;
    juce::Rectangle<int> dirtyArea;
    PianoRollVisualInvalidationPriority priority = Background;
};

struct PianoRollVisualInvalidationState {
    uint32_t reasonsMask = 0;
    bool fullRepaint = false;
    bool hasDirtyArea = false;
    juce::Rectangle<int> dirtyArea;
    PianoRollVisualInvalidationPriority priority = Background;

    void merge(const Request&);
    bool hasWork() const;         // reasonsMask != 0
    void clear();
};

struct PianoRollVisualFlushDecision {
    bool shouldRepaint = false;
    bool fullRepaint = false;
    bool hasDirtyArea = false;
    juce::Rectangle<int> dirtyArea;
    PianoRollVisualInvalidationPriority priority = Background;
};
```

### 4.3 合并规则

`State::merge(req)`：
- `reasonsMask |= req.reasonsMask`；
- `priority = max(state.priority, req.priority)`；
- 若 `req.fullRepaint` 或（`!req.hasDirtyArea && 已有工作`），升级为 `fullRepaint`；
- 否则若 `fullRepaint` 已置位则忽略新矩形；
- 否则取 `dirtyArea.getUnion(req.dirtyArea)`。

### 4.4 决策函数

```cpp
PianoRollVisualFlushDecision makeVisualFlushDecision(
    const PianoRollVisualInvalidationState&,
    const juce::Rectangle<int>& localBounds);
```

流程：无工作 → `shouldRepaint=false`；`fullRepaint` → 照搬；`hasDirtyArea` → 与 `localBounds` 相交，若相交为空则升级为 full；否则返回裁剪后的脏矩形。

---

## 5. InteractionState

**文件**: `Source/Standalone/UI/PianoRoll/InteractionState.h` / `.cpp`

纯数据聚合体，不含行为。详细字段见 `data-model.md §2`。公开方法只有 `SelectionState::setF0Range / clearF0Selection` 以及各子结构的 `clear()`。

```cpp
enum class NoteResizeEdge { None, Left, Right };

struct SelectionState { /* 矩形 + F0 帧选区 */ };
struct NoteDragState  { /* draggedNoteIndex, manualStartTime/EndTime, previewF0 ... */ };
struct NoteResizeState{ /* noteIndex, edge, originalStartTime/EndTime ... */ };
struct NoteInteractionDraft { bool active; std::vector<Note> baseline, working; };
struct DrawingState   { /* isDrawingF0, handDrawBuffer, isPlacingAnchors, pendingAnchors ... */ };

class InteractionState {
public:
    SelectionState selection;
    NoteInteractionDraft noteDraft;
    NoteDragState noteDrag;
    NoteResizeState noteResize;
    DrawingState drawing;

    bool isPanning = false;
    juce::Point<int> dragStartPos;
    bool drawNoteToolPendingDrag = false;
    juce::Point<int> drawNoteToolMouseDownPos;
    bool handDrawPendingDrag = false;
    std::vector<int> selectedLineAnchorSegmentIds;
};
```

---

## 6. PianoRollEditAction

**文件**: `Source/Utils/PianoRollEditAction.h` / `.cpp`
**继承**: `OpenTune::UndoAction`

```cpp
class PianoRollEditAction : public UndoAction {
public:
    PianoRollEditAction(OpenTuneAudioProcessor& processor,
                        uint64_t materializationId,
                        juce::String description,
                        std::vector<Note> oldNotes,
                        std::vector<Note> newNotes,
                        std::vector<CorrectedSegment> oldSegments,
                        std::vector<CorrectedSegment> newSegments);

    void undo() override;  // processor.commitMaterializationNotesAndSegmentsById(id, oldNotes, oldSegments)
    void redo() override;  // 同上，用 newNotes/newSegments
    juce::String getDescription() const override;

    uint64_t getMaterializationId() const;
    int getAffectedStartFrame() const;
    int getAffectedEndFrame() const;
};
```

契约：
- `affectedStartFrame/EndFrame` 在构造时由 `old ∪ new` 的 `CorrectedSegment::startFrame/endFrame` 合并得出（若两侧都为空则 0）。
- `undo/redo` 通过 `commitMaterializationNotesAndSegmentsById` 原子替换音符和段，保证事务一致。
- **不** 重新计算 F0 或波形，调用方（UndoManager 或其上层）负责广播 `pitchCurveEdited`。

---

## 7. PianoKeyAudition

**文件**: `Source/Utils/PianoKeyAudition.h` / `.cpp`

88 键采样试听。

```cpp
class PianoKeyAudition {
public:
    PianoKeyAudition();
    void loadSamples();                                            // message thread, 一次
    void noteOn(int midiNote);                                     // message thread
    void noteOff(int midiNote);                                    // message thread
    void mixIntoBuffer(juce::AudioBuffer<float>&, int numSamples,  // audio thread
                       double sampleRate);
    int  getPressedNote() const noexcept;                          // 最近一次 noteOn 的 MIDI，-1 = 无
};
```

常量：
- `kMaxVoices = 8`（voice 池满时抢占第 0 号）；
- `kSampleRate = 44100.0`（采样的原生采样率，重采样比率 `kSampleRate / outSampleRate`）；
- `kReleaseTimeSeconds = 0.05f`（release 线性衰减）；
- `kEventBufferSize = 64`（SPSC 环形，缓冲满则丢弃）；
- `kMidiMin = 21, kMidiMax = 108`（包含）。

契约：
- `loadSamples` 从 `BinaryData::piano_{midi}_mp3` 读入 88 份采样；对每份计算 `onsetSample`（首个 |amp|>0.005 的位置）；
- `noteOn` 写事件到 SPSC，且独占更新 `pressedNote_`；`noteOff` 写事件，并 CAS 清空 `pressedNote_`（仅在其值恰为 midiNote 时）；
- `mixIntoBuffer` 先 `processEvents`（应用 start/stop），再对每个活跃 Voice 按 `playbackRate` 线性插值累加到所有通道；Voice 播放完或 release 衰减至 0 则 `active=false`。

---

## 8. PianoRollVisualPreferences

**文件**: `Source/Utils/PianoRollVisualPreferences.h`

```cpp
enum class NoteNameMode { ShowAll = 0, COnly = 1, Hide = 2 };

struct PianoRollVisualPreferences {
    NoteNameMode noteNameMode = COnly;
    bool showChunkBoundaries = false;
    bool showUnvoicedFrames = false;
};
```

> **注**：结构体本身未被 `PianoRollComponent` 直接持有；Component 分别用三个独立字段存储同语义值（`noteNameMode_ / showChunkBoundaries_ / showUnvoicedFrames_`），通过 setter 设置。上层可用该结构体做偏好整体 Marshal。

---

## 9. ToolIds + ToolbarIcons

```cpp
enum class ToolId : int {
    AutoTune   = 0,
    Select     = 1,
    DrawNote   = 2,
    LineAnchor = 3,
    HandDraw   = 4
};
```

`ToolbarIcons` 暴露静态工厂（返回 `juce::Path`，24×24 viewBox）：`getFileIcon / getEditIcon / getEyeIcon / ...`（完整列表约 20+，具体见头文件）。调用方自行缩放并设置颜色。

---

## ⚠️ 待确认

1. **`DrawNote` 快捷键在右键菜单上标为 "Hand Draw (5)"，但 `keyPressed` 中未显式绑定 '5' 键**，需确认是否靠默认 `KeyShortcutConfig` 某项落到 HandDraw，还是菜单文本与实际快捷键不一致。
2. **`PianoKeyAudition::kMidiMin = 21` 而 `PianoRollComponent::minMidi_ = 24`**：MIDI 21–23（A0–B0）无法在卷帘中显示但仍会加载采样并可通过程序化 `noteOn` 触发，是否有意？
3. **`mixIntoBuffer` 在 voice 抢占时直接覆盖 `voices_.front()` 而不做淡出**：极端情况下可能产生咔哒声，是否需要 tail-release 抢占策略？
4. **`PianoRollEditAction::undo/redo` 不广播 `pitchCurveEdited`**，而 v1.3 的主流程都会主动广播；确认是否依赖 `UndoManager` 或外部调用方补发失效通知，否则 undo 后视觉需等其他失效触发。
5. **`setShortcutSettings` 与 `setZoomSensitivity` 不触发任何 `invalidateVisual`**，若用户即时改快捷键后无需重绘则合理；但若设置中含"是否显示快捷键提示"之类视觉影响项则需失效。
6. **`PianoRollVisualPreferences` 结构体定义后没有被任何现有代码 include 使用**（仅通过 `noteNameMode_` 等单字段），是否计划用于偏好持久化的未来整合？
