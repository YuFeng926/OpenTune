---
module: ui-main
type: data-model
generated: true
warning: "⚠️ 基于源码扫描生成，可能存在遗漏或过时信息"
---

# ui-main 数据模型

## 1. 布局常量

定义于 `PluginEditor.h:236-240`，控制主窗口布局尺寸：

| 常量 | 值 | 说明 |
|------|-----|------|
| `MENU_BAR_HEIGHT` | 25 | 菜单栏高度（macOS 使用系统菜单时不显示） |
| `TOP_PANEL_HEIGHT` | 45 | 顶部面板高度 |
| `TRANSPORT_BAR_HEIGHT` | 64 | 传输控制栏高度 |
| `TRACK_PANEL_WIDTH` | 180 | 左侧轨道面板宽度 |
| `PARAMETER_PANEL_WIDTH` | 240 | 右侧参数面板宽度 |

布局使用阴影边距模式（`shadowMargin = 12`），各面板绘制在 `reduced(shadowMargin)` 区域内，阴影渲染在边距空间。面板间隙 `gap = 6`。

## 2. 心跳频率常量

定义于 `PluginEditor.cpp:29-30`：

| 常量 | 值 | 说明 |
|------|-----|------|
| `kHeartbeatHzIdle` | 30 | 空闲状态心跳频率 |
| `kHeartbeatHzInferenceActive` | 10 | 推理活动时降低频率减轻压力 |

推理活动时，次要刷新（音频缓冲区同步、轨道电平表）每 4 个 tick 才执行一次。

## 3. PluginEditor 内部状态

### 3.1 导入队列

```cpp
struct PendingImport {
    int trackId;
    juce::File file;
};
std::vector<PendingImport> importQueue_;
```

解决并发导入问题：当 `isImportInProgress_` 为 true 时，后续导入请求入队排队。导入完成后 `processNextImportInQueue()` 取出下一个。

### 3.2 延迟后处理队列

```cpp
struct DeferredImportPostProcessRequest {
    int trackId{-1};
    uint64_t clipId{0};
};
std::vector<DeferredImportPostProcessRequest> deferredImportPostProcessQueue_;
```

导入的 Clip 完成 commit 后，其后处理（dry signal 生成、F0 提取）被加入延迟队列，在 `timerCallback()` 中由 `processDeferredImportPostProcessQueue()` 异步执行。

### 3.3 AUTO Overlay 事务锁

```cpp
bool autoOverlayLatched_ = false;
int autoOverlayTargetTrackId_ = -1;
uint64_t autoOverlayTargetClipId_ = 0;
```

AUTO（自动修音）启动时 latch，覆盖层显示"正在渲染中"。释放条件：
- 目标 Clip 不存在
- AutoTune 处理完成 **且** 所有 Chunk 渲染完成

### 3.4 RMVPE Overlay 事务锁

```cpp
bool rmvpeOverlayLatched_ = false;
int rmvpeOverlayTargetTrackId_ = -1;
uint64_t rmvpeOverlayTargetClipId_ = 0;
```

F0 提取启动时 latch。释放条件：
- Clip 不存在
- F0 提取失败
- F0 提取完成 **且** 当前 PianoRoll 上下文匹配 **且** F0 曲线可见

### 3.5 视图状态

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `isWorkspaceView_` | bool | true | ArrangementView(true) vs PianoRoll(false) |
| `isTrackPanelVisible_` | bool | true | 左侧面板可见性 |
| `isParameterPanelVisible_` | bool | true | 右侧面板可见性 |
| `showingSingleNoteParams_` | bool | false | 参数面板是否显示单音符参数 |
| `inferenceActive_` | bool | false | 推理是否活动 |

### 3.6 Undo 追踪

| 字段 | 类型 | 说明 |
|------|------|------|
| `lastScaleRootNote_` | int | 上次调式根音 |
| `lastScaleType_` | int | 上次调式类型（1=Major, 2=Minor, 3=Chromatic） |
| `lastUndoRedoShortcutMs_` | uint32_t | Undo/Redo 快捷键防抖时间戳（120ms 间隔） |
| `suppressScaleChangedCallback_` | bool | 程序化设置调式时抑制回调 |
| `lastTrackVolumes_` | array<float, MAX_TRACKS> | 轨道音量历史值（用于 Undo 阈值判断） |

## 4. RenderStatusUiState

定义于 `PluginEditor.cpp:69`，用于渲染状态显示：

```cpp
struct RenderStatusUiState {
    bool showRendering = false;
    int uiPendingTasks = 0;
    juce::String detailText;
};
```

由 `buildRenderStatusUiState(bool isTxnActive)` 从 Chunk 状态构建。

## 5. TimeConverter 参数

定义于 `Source/Standalone/UI/TimeConverter.h`：

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `bpm_` | double | 120.0 | 每分钟拍数 |
| `timeSignatureNum_` | int | 4 | 拍号分子 |
| `timeSignatureDenom_` | int | 4 | 拍号分母 |
| `zoomLevel_` | double | 1.0 | 缩放级别 |
| `scrollOffset_` | double | 0.0 | 滚动偏移 |
| `pixelsPerSecondBase_` | double (constexpr) | 100.0 | 基础像素/秒比率 |

转换公式：`pixel = (timeInSeconds * pixelsPerSecondBase_ * zoomLevel_) - scrollOffset_`

## 6. WaveformMipmap 结构

### 6.1 PeakSample

```cpp
struct PeakSample {
    int8_t min;   // 最小值 (-127 ~ 127)，映射 float [-1, 1]
    int8_t max;   // 最大值
};
```

使用 int8_t 压缩存储（每对 peak 仅 2 字节），大幅减少内存。

### 6.2 Level 结构

```cpp
struct Level {
    std::vector<PeakSample> peaks;
    int64_t numSamplesCovered = 0;
    bool complete = false;
    int64_t buildProgress = 0;
};
```

### 6.3 Mipmap 层级

| Level | samples/peak | 适用场景 |
|-------|-------------|----------|
| 0 | 32 | 高倍放大 |
| 1 | 128 | 中等放大 |
| 2 | 512 | 标准视图 |
| 3 | 2048 | 缩小视图 |
| 4 | 8192 | 大范围概览 |
| 5 | 32768 | 最小缩放 |

`selectBestLevel()` 根据 pixelsPerSecond 自动选择最佳层级。

## 7. FrameScheduler 优先级

```cpp
enum class Priority : int {
    Background = 0,    // 低优先级后台刷新
    Normal = 1,        // 普通刷新
    Interactive = 2    // 用户交互触发的高优先级刷新
};
```

同一帧内的多次 repaint 请求被合并：
- 局部区域请求使用 `getUnion()` 合并脏区
- 全量请求覆盖局部请求
- 优先级取最大值

## 8. TrackPanel 常量

定义于 `Source/Standalone/UI/TrackPanelComponent.h`：

| 常量 | 值 | 说明 |
|------|-----|------|
| `TRACK_PANEL_WIDTH` | 120 | 紧凑模式宽度（命名空间级别） |
| `MAX_TRACKS` | 12 | 最大轨道数 |
| `DEFAULT_VISIBLE_TRACKS` | 2 | 默认可见轨道数 |
| `MIN_TRACK_HEIGHT` | 70 | 最小轨道高度 |
| `DEFAULT_TRACK_HEIGHT` | 100 | 默认轨道高度 |
| `MAX_TRACK_HEIGHT` | 300 | 最大轨道高度 |

### 8.1 TrackControl 结构

```cpp
struct TrackControl {
    MuteSoloIconButton muteButton;
    MuteSoloIconButton soloButton;
    VolumeKnob volumeSlider;       // Rotary, range [0.0, 4.0], center=1.0 (0dB)
    CircularLevelMeter levelMeter; // 环形LED电平表
    bool isActive{false};
};
```

12 条轨道的控件数组：`std::array<TrackControl, MAX_TRACKS>`。

### 8.2 轨道淡彩色

```cpp
static constexpr juce::uint32 trackPastelColors[12]; // 12种柔和色（BlueBreeze主题专用）
```

## 9. ArrangementView 内部状态

### 9.1 Clip 多选

```cpp
struct ClipSelectionKey {
    int trackId;
    uint64_t clipId;
};
std::set<ClipSelectionKey> selectedClips_;
```

支持 Shift 范围选择、Ctrl 切换选择。

### 9.2 剪贴板

```cpp
struct ClipboardClip {
    int sourceTrackId;
    ClipSnapshot snapshot;
    double relativeOffset;
};
std::vector<ClipboardClip> clipboard_;
```

支持 Ctrl+C/X/V 剪切复制粘贴，`relativeOffset` 记录相对于参考时间的偏移。

### 9.3 拖拽状态

```cpp
struct DragStartState {
    int trackId;
    uint64_t clipId;
    double startSeconds;
};
```

拖拽时记录所有选中 Clip 的初始状态，支持多 Clip 同步拖拽和跨轨道移动。

## 10. CircularLevelMeter 参数

- 电平范围：-60 dB ~ +6 dB
- 刷新率：30 Hz（推理时降至 12 Hz）
- 24 段 LED 弧形排列（270° 范围）
- 颜色分区：绿色(0-60%) → 黄色(60-80%) → 橙色(80-92%) → 红色(92-100%)
- 平滑衰减：`currentLevel = currentLevel * 0.85 + targetLevel * 0.15`
- 过载保持：2 秒（60 个 tick @ 30Hz）

## 11. ExportType 枚举

```cpp
enum class ExportType {
    SelectedClip,  // 导出选中的片段
    Track,         // 导出整条轨道
    Bus            // 导出总线混音
};
```

## 12. MenuItemIDs 枚举

定义于 `MenuBarComponent.h:70`，菜单项 ID 分段分配：

| 范围 | 用途 |
|------|------|
| 1-10 | File 菜单（Import, Export×3, Preset×2） |
| 50-51 | Edit 菜单（Undo, Redo） |
| 100-199 | View 菜单（Waveform, Lanes, Themes） |
| 150-157 | 鼠标轨迹特效（None ~ Matrix，共 8 种） |
| 200-201 | 系统菜单（Preferences, Help） |
