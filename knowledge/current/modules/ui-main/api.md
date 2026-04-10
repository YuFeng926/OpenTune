---
module: ui-main
type: api
generated: true
warning: "⚠️ 基于源码扫描生成，可能存在遗漏或过时信息"
---

# ui-main API 参考

> 本模块无 HTTP Controller，此文档记录主界面 UI 层对外暴露的编程接口契约

## 1. OpenTuneAudioProcessorEditor

**文件**: `Source/Standalone/PluginEditor.h:38`

中心 Mediator，继承 `juce::AudioProcessorEditor`，同时实现所有子组件的 Listener 接口。

### 1.1 构造/析构

| 签名 | 说明 |
|------|------|
| `explicit OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor&)` | 构造，初始化所有子组件、注册 Listener、设置 LookAndFeel |
| `~OpenTuneAudioProcessorEditor() override` | 析构，清理 macOS 系统菜单、join 后台线程、移除 Listener |

### 1.2 JUCE 基础覆写

| 签名 | 说明 |
|------|------|
| `void paint(juce::Graphics&) override` | 绘制背景、Buffering 指示器、Dry Fallback 警告 |
| `void resized() override` | 主布局：TopBar → 左TrackPanel → 右ParameterPanel → 中央ArrangementView/PianoRoll |
| `bool keyPressed(const juce::KeyPress& key) override` | 全局快捷键：Undo/Redo、PlayPause、PlayFromStart |

### 1.3 FileDragAndDropTarget

| 签名 | 说明 |
|------|------|
| `bool isInterestedInFileDrag(const juce::StringArray& files) override` | 检测拖入文件是否为支持的音频格式 |
| `void filesDropped(const juce::StringArray& files, int x, int y) override` | 处理拖放导入（单文件、弹窗选择轨道） |

### 1.4 ParameterPanel::Listener

| 签名 | 说明 |
|------|------|
| `void retuneSpeedChanged(float speed) override` | 修正速度变化 → 转发 PianoRoll（归一化 0-1） |
| `void vibratoDepthChanged(float value) override` | 颤音深度变化 → 选中音符或全 Clip 校正 |
| `void vibratoRateChanged(float value) override` | 颤音速率变化 → 选中音符或全 Clip 校正 |
| `void noteSplitChanged(float value) override` | 音符分割阈值变化 → 转发 PianoRoll |
| `void toolSelected(int toolId) override` | 工具选择（AutoTune 特殊处理，其余转发 PianoRoll） |
| `void parameterDragEnded(int paramId, float oldValue, float newValue) override` | 参数拖拽完成（预留 Undo 接口） |

### 1.5 MenuBarComponent::Listener

| 签名 | 说明 |
|------|------|
| `void importAudioRequested() override` | 打开文件选择器，支持多选，导入音频 |
| `void exportAudioRequested(MenuBarComponent::ExportType exportType) override` | 导出选中 Clip/Track/Bus |
| `void savePresetRequested() override` | 保存预设 (.otpreset) |
| `void loadPresetRequested() override` | 加载预设 |
| `void preferencesRequested() override` | 打开 OptionsDialog |
| `void helpRequested() override` | 打开本地 UserGuide.html |
| `void showWaveformToggled(bool shouldShow) override` | 切换波形显示 |
| `void showLanesToggled(bool shouldShow) override` | 切换通道显示 |
| `void themeChanged(ThemeId themeId) override` | 全局主题切换 |
| `void undoRequested() override` | 撤销 |
| `void redoRequested() override` | 重做 |
| `void mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme) override` | 鼠标轨迹特效切换 |

### 1.6 TransportBarComponent::Listener

| 签名 | 说明 |
|------|------|
| `void playRequested() override` | 开始播放 |
| `void pauseRequested() override` | 暂停播放 |
| `void stopRequested() override` | 停止并归零 |
| `void loopToggled(bool enabled) override` | 循环开关 |
| `void bpmChanged(double newBpm) override` | BPM 变更 → 同步 Processor + PianoRoll |
| `void scaleChanged(int rootNote, int scaleType) override` | 调式变更 → 带 Undo 的 Clip 级调式设置 |
| `void viewToggled(bool workspaceView) override` | 视图切换（ArrangementView ↔ PianoRoll） |

### 1.7 TrackPanelComponent::Listener

| 签名 | 说明 |
|------|------|
| `void trackSelected(int trackId) override` | 轨道选择 → 同步 Processor + PianoRoll |
| `void trackMuteToggled(int trackId, bool muted) override` | 静音切换（带 Undo） |
| `void trackSoloToggled(int trackId, bool solo) override` | Solo 切换（带 Undo） |
| `void trackVolumeChanged(int trackId, float volume) override` | 音量变化（带 Undo，阈值 0.01） |
| `void trackHeightChanged(int newHeight) override` | 轨道高度变化（Y轴缩放同步） |

### 1.8 ArrangementViewComponent::Listener

| 签名 | 说明 |
|------|------|
| `void clipSelectionChanged(int trackId, int clipIndex) override` | Clip 选择变化 → 同步 PianoRoll |
| `void clipTimingChanged(int trackId, int clipIndex) override` | Clip 时间偏移变化 |
| `void clipDoubleClicked(int trackId, int clipIndex) override` | 双击 Clip → 切换到 PianoRoll 视图 |
| `void verticalScrollChanged(int newOffset) override` | 垂直滚动同步 TrackPanel ↔ ArrangementView |

### 1.9 PianoRollComponent::Listener

| 签名 | 说明 |
|------|------|
| `void playheadPositionChangeRequested(double timeSeconds) override` | 请求移动播放头 |
| `void playPauseToggleRequested() override` | 播放/暂停切换 |
| `void stopPlaybackRequested() override` | 停止播放 |
| `void autoTuneRequested() override` | AUTO 处理请求 |
| `void pitchCurveEdited(int startFrame, int endFrame) override` | 音高曲线编辑 → 触发局部渲染 |
| `void trackTimeOffsetChanged(int trackId, double newOffset) override` | 轨道时间偏移变化 |
| `void escapeKeyPressed() override` | ESC 键 → 视图切换 |
| `void playFromPositionRequested(double timeSeconds) override` | 从指定位置开始播放 |

### 1.10 LanguageChangeListener

| 签名 | 说明 |
|------|------|
| `void languageChanged(Language newLanguage) override` | 语言变更 → 刷新菜单栏、工具栏、参数面板 |

---

## 2. MenuBarComponent

**文件**: `Source/Standalone/UI/MenuBarComponent.h:21`

继承 `juce::MenuBarModel`，提供 File / Edit / View 三级菜单。

### 2.1 Listener 接口

```cpp
class Listener {
    virtual void importAudioRequested() = 0;
    virtual void exportAudioRequested(ExportType exportType) = 0;
    virtual void savePresetRequested() = 0;
    virtual void loadPresetRequested() = 0;
    virtual void preferencesRequested() = 0;
    virtual void helpRequested() = 0;
    virtual void showWaveformToggled(bool shouldShow) = 0;
    virtual void showLanesToggled(bool shouldShow) = 0;
    virtual void themeChanged(ThemeId themeId) = 0;
    virtual void undoRequested() = 0;
    virtual void redoRequested() = 0;
    virtual void mouseTrailThemeChanged(MouseTrailConfig::TrailTheme theme) = 0;
};
```

### 2.2 ExportType 枚举

```cpp
enum class ExportType { SelectedClip, Track, Bus };
```

### 2.3 Public 方法

| 签名 | 说明 |
|------|------|
| `explicit MenuBarComponent(OpenTuneAudioProcessor& processor)` | 构造 |
| `void addListener(Listener* listener)` | 注册监听 |
| `void removeListener(Listener* listener)` | 移除监听 |
| `void refreshLocalizedText()` | 刷新本地化文本 |
| `juce::StringArray getMenuBarNames() override` | 返回菜单名称列表 |
| `juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override` | 构建菜单内容 |
| `void menuItemSelected(int menuItemID, int topLevelMenuIndex) override` | 菜单项选中回调 |

---

## 3. TransportBarComponent

**文件**: `Source/Standalone/UI/TransportBarComponent.h:108`

传输控制栏：播放/暂停/停止、循环、BPM、调式选择、视图切换。

### 3.1 Listener 接口

```cpp
class Listener {
    virtual void playRequested() = 0;
    virtual void pauseRequested() = 0;
    virtual void stopRequested() = 0;
    virtual void loopToggled(bool enabled) = 0;
    virtual void bpmChanged(double newBpm) = 0;
    virtual void scaleChanged(int rootNote, int scaleType) = 0;
    virtual void viewToggled(bool workspaceView) = 0;
    virtual void audioSettingsRequested() {}  // 可选
};
```

### 3.2 Public 方法

| 签名 | 说明 |
|------|------|
| `void setPlaying(bool playing)` | 设置播放状态 |
| `bool isPlaying() const` | 查询播放状态 |
| `void setLoopEnabled(bool enabled)` | 设置循环状态 |
| `void setLooping(bool looping)` | setLoopEnabled 别名 |
| `void setBpm(double bpm)` | 设置 BPM 值 |
| `double getBpm() const` | 获取 BPM |
| `void setScale(int rootNote, int scaleType)` | 设置调式 |
| `void setPositionSeconds(double seconds)` | 更新播放位置显示 |
| `void setWorkspaceView(bool workspaceView)` | 设置视图模式 |
| `bool isWorkspaceView() const` | 查询视图模式 |
| `void setRenderStatusText(const juce::String& text)` | 设置渲染状态文本 |
| `void applyTheme()` | 应用主题 |
| `void refreshLocalizedText()` | 刷新本地化文本 |
| `juce::Component& getFileButton()` | 获取 File 菜单按钮引用 |
| `juce::Component& getEditButton()` | 获取 Edit 菜单按钮引用 |
| `juce::Component& getViewButton()` | 获取 View 菜单按钮引用 |

### 3.3 回调属性

| 属性 | 说明 |
|------|------|
| `std::function<void()> onFileMenuRequested` | File 菜单点击回调 |
| `std::function<void()> onEditMenuRequested` | Edit 菜单点击回调 |
| `std::function<void()> onViewMenuRequested` | View 菜单点击回调 |

---

## 4. TopBarComponent

**文件**: `Source/Standalone/UI/TopBarComponent.h:12`

顶部容器，包含 MenuBar + TransportBar，以及侧边栏折叠按钮。

### 4.1 Public 方法

| 签名 | 说明 |
|------|------|
| `TopBarComponent(MenuBarComponent& menuBar, TransportBarComponent& transportBar)` | 构造 |
| `void applyTheme()` | 应用主题 |
| `void setSidePanelsVisible(bool trackPanelVisible, bool parameterPanelVisible)` | 同步侧边栏按钮状态 |
| `void refreshLocalizedText()` | 刷新本地化文本 |

### 4.2 回调属性

| 属性 | 说明 |
|------|------|
| `std::function<void()> onToggleTrackPanel` | 切换轨道面板可见性 |
| `std::function<void()> onToggleParameterPanel` | 切换参数面板可见性 |

---

## 5. TrackPanelComponent

**文件**: `Source/Standalone/UI/TrackPanelComponent.h:443`

左侧轨道面板：轨道选择、Mute/Solo、音量旋钮、电平表。

### 5.1 Listener 接口

```cpp
class Listener {
    virtual void trackSelected(int trackId) = 0;
    virtual void trackMuteToggled(int trackId, bool muted) = 0;
    virtual void trackSoloToggled(int trackId, bool solo) = 0;
    virtual void trackVolumeChanged(int trackId, float volume) = 0;
    virtual void trackHeightChanged(int newHeight) {}     // 可选
    virtual void verticalScrollChanged(int offset) {}     // 可选
};
```

### 5.2 Public 方法

| 签名 | 说明 |
|------|------|
| `void setActiveTrack(int trackId)` | 设置当前活动轨道 |
| `int getActiveTrack() const` | 获取活动轨道 ID |
| `void setTrackMuted(int trackId, bool muted)` | 设置静音状态 |
| `bool isTrackMuted(int trackId) const` | 查询静音状态 |
| `void setTrackSolo(int trackId, bool solo)` | 设置 Solo 状态 |
| `bool isTrackSolo(int trackId) const` | 查询 Solo 状态 |
| `void setTrackVolume(int trackId, float volume)` | 设置音量 |
| `float getTrackVolume(int trackId) const` | 获取音量 |
| `void setTrackLevel(int trackId, float levelDB)` | 设置电平表值 (dB) |
| `void setTrackClipping(int trackId, bool isClipping)` | 设置过载状态 |
| `void setInferenceActive(bool active)` | 推理模式下降低刷新率 |
| `void setVisibleTrackCount(int count)` | 设置可见轨道数 |
| `int getVisibleTrackCount() const` | 获取可见轨道数 |
| `void showMoreTracks()` | 增加可见轨道数量 |
| `void setTrackHeight(int height)` | 设置轨道高度（Y轴缩放） |
| `int getTrackHeight() const` | 获取轨道高度 |
| `void setVerticalScrollOffset(int offset)` | 设置垂直滚动偏移 |
| `void setTrackStartYOffset(int offset)` | 设置轨道起始Y偏移 |
| `void applyTheme()` | 应用主题 |

### 5.3 常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `MIN_TRACK_HEIGHT` | 70 | 最小轨道高度 |
| `DEFAULT_TRACK_HEIGHT` | 100 | 默认轨道高度 |
| `MAX_TRACK_HEIGHT` | 300 | 最大轨道高度 |
| `MAX_TRACKS` | 12 | 最大轨道数 |
| `DEFAULT_VISIBLE_TRACKS` | 2 | 默认可见轨道数 |

---

## 6. ArrangementViewComponent

**文件**: `Source/Standalone/UI/ArrangementViewComponent.h:30`

多轨道编排视图：Clip 显示/拖拽/选择、波形可视化、时间标尺。

### 6.1 Listener 接口

```cpp
class Listener {
    virtual void clipSelectionChanged(int trackId, int clipIndex) = 0;
    virtual void clipTimingChanged(int trackId, int clipIndex) = 0;
    virtual void clipDoubleClicked(int trackId, int clipIndex) {}  // 可选
    virtual void trackHeightChanged(int newHeight) {}              // 可选
    virtual void verticalScrollChanged(int newOffset) {}           // 可选
};
```

### 6.2 Public 方法

| 签名 | 说明 |
|------|------|
| `void setIsPlaying(bool playing)` | 设置播放状态（atomic） |
| `void setPlayheadColour(juce::Colour colour)` | 设置播放头颜色 |
| `void setPlayheadPositionSource(std::weak_ptr<std::atomic<double>> source)` | 设置播放头位置源 |
| `void setZoomLevel(double zoom)` | 设置缩放级别 |
| `double getZoomLevel() const` | 获取缩放级别 |
| `void setScrollOffset(int pixels)` | 设置水平滚动偏移 |
| `void setVerticalScrollOffset(int offset)` | 设置垂直滚动偏移 |
| `void setInferenceActive(bool active)` | 设置推理活动状态 |
| `void fitToContent()` | 自动缩放适配内容 |
| `void prioritizeWaveformBuildForClip(int trackId, uint64_t clipId)` | 优先构建指定 Clip 的波形缓存 |
| `bool isWaveformCacheCompleteForClip(int trackId, uint64_t clipId) const` | 查询波形缓存完成状态 |
| `void resetUserZoomFlag()` | 重置用户缩放标记 |
| `bool hasUserManuallyZoomed() const` | 查询是否手动缩放过 |
| `void onHeartbeatTick()` | 心跳定时器回调（由 PluginEditor 调用） |

---

## 7. ParameterPanel

**文件**: `Source/Standalone/UI/ParameterPanel.h:47`

右侧参数面板：Retune Speed、Vibrato、Note Split、工具选择。

### 7.1 Listener 接口

```cpp
class Listener {
    virtual void retuneSpeedChanged(float speed) = 0;
    virtual void vibratoDepthChanged(float value) = 0;
    virtual void vibratoRateChanged(float value) = 0;
    virtual void noteSplitChanged(float value) = 0;
    virtual void toolSelected(int toolId) = 0;
    virtual void parameterDragEnded(int paramId, float oldValue, float newValue) {}  // 可选
};
```

### 7.2 Public 方法

| 签名 | 说明 |
|------|------|
| `void setActiveTool(int toolId)` | 设置当前工具 |
| `void setRetuneSpeed(float speed)` | 设置修正速度值 |
| `void setVibratoDepth(float value)` | 设置颤音深度 |
| `void setVibratoRate(float value)` | 设置颤音速率 |
| `void setNoteSplit(float value)` | 设置音符分割阈值 |
| `void setF0Min(float value)` | 设置 F0 最小值 |
| `void setF0Max(float value)` | 设置 F0 最大值 |
| `float getRetuneSpeed() const` | 获取修正速度 |
| `float getVibratoDepth() const` | 获取颤音深度 |
| `float getVibratoRate() const` | 获取颤音速率 |
| `float getNoteSplit() const` | 获取分割阈值 |
| `void applyTheme()` | 应用主题 |
| `void refreshLocalizedText()` | 刷新本地化文本 |

---

## 8. TimelineComponent

**文件**: `Source/Standalone/UI/TimelineComponent.h:19`

时间线组件：时间标尺、节拍网格、播放头交互。

### 8.1 Listener 接口

```cpp
class Listener {
    virtual void playheadPositionChanged(double timeInSeconds) = 0;
    virtual void zoomLevelChanged(double newZoom) = 0;
};
```

### 8.2 Public 方法

| 签名 | 说明 |
|------|------|
| `void setTimeConverter(TimeConverter* converter)` | 设置时间转换器 |
| `void setPlayheadPosition(double timeInSeconds)` | 设置播放头位置 |
| `double getPlayheadPosition() const` | 获取播放头位置 |
| `void setViewportScroll(int scrollX)` | 设置视口滚动 |
| `void setZoomLevel(double zoom)` | 设置缩放级别 |
| `void setTimeUnit(TimeUnit unit)` | 设置时间单位（Seconds/Bars） |
| `void setBpm(double bpm)` | 设置 BPM |

---

## 9. PlayheadOverlayComponent

**文件**: `Source/Standalone/UI/PlayheadOverlayComponent.h:9`

高性能播放头覆盖层，使用 VBlank 同步，独立于主组件重绘。所有 setter 使用 atomic 操作。

### 9.1 Public 方法

| 签名 | 说明 |
|------|------|
| `void setPlayheadSeconds(double seconds) noexcept` | 设置播放头时间 |
| `void setZoomLevel(double zoom) noexcept` | 设置缩放级别 |
| `void setScrollOffset(double offset) noexcept` | 设置滚动偏移 |
| `void setTrackOffsetSeconds(double offset) noexcept` | 设置轨道偏移 |
| `void setAlignmentOffsetSeconds(double offset) noexcept` | 设置对齐偏移 |
| `void setPianoKeyWidth(int width) noexcept` | 设置钢琴键宽度 |
| `void setPlaying(bool playing) noexcept` | 设置播放状态 |
| `void setPlayheadColour(juce::Colour colour)` | 设置播放头颜色 |

---

## 10. TimeConverter

**文件**: `Source/Standalone/UI/TimeConverter.h:17`

时间坐标转换工具：秒 ↔ 像素，考虑 BPM/拍号/缩放/滚动。

### 10.1 Public 方法

| 签名 | 说明 |
|------|------|
| `void setContext(double bpm, int timeSignatureNum, int timeSignatureDenom)` | 设置 BPM 和拍号 |
| `void setZoom(double zoomLevel)` | 设置缩放级别 |
| `void setScrollOffset(double offset)` | 设置滚动偏移 |
| `int timeToPixel(double timeInSeconds) const` | 时间 → 像素 |
| `double pixelToTime(int pixelX) const` | 像素 → 时间 |
| `int snapToGrid(int pixelX, GridResolution resolution) const` | 像素吸附到网格 |
| `double getBpm() const` | 获取 BPM |
| `double getZoomLevel() const` | 获取缩放级别 |

### 10.2 GridResolution 枚举

```cpp
enum class GridResolution { Bar, Beat, HalfBeat, QuarterBeat, Sixteenth };
```

---

## 11. WaveformMipmap / WaveformMipmapCache

**文件**: `Source/Standalone/UI/WaveformMipmap.h`

多级波形 Mipmap 缓存：6 级分辨率（32~32768 samples/peak），增量构建。

### 11.1 WaveformMipmap

| 签名 | 说明 |
|------|------|
| `void setAudioSource(std::shared_ptr<const juce::AudioBuffer<float>> buffer)` | 设置音频源 |
| `bool hasSource() const noexcept` | 是否有数据源 |
| `bool isSourceChanged(std::shared_ptr<const juce::AudioBuffer<float>> buffer) const noexcept` | 数据源是否变化 |
| `bool buildIncremental(double timeBudgetMs)` | 增量构建（时间预算控制） |
| `bool isComplete() const noexcept` | 是否完成构建 |
| `float getBuildProgress() const noexcept` | 构建进度 |
| `const Level& selectBestLevel(double pixelsPerSecond) const` | 根据缩放选择最佳 Level |
| `void clear()` | 清空 |

### 11.2 WaveformMipmapCache

| 签名 | 说明 |
|------|------|
| `WaveformMipmap& getOrCreate(uint64_t clipId)` | 获取或创建 |
| `void remove(uint64_t clipId)` | 移除 |
| `void prune(const std::unordered_set<uint64_t>& alive)` | 清理不再存在的条目 |
| `bool buildIncremental(double timeBudgetMs)` | 增量构建所有缓存 |

---

## 12. FrameScheduler

**文件**: `Source/Standalone/UI/FrameScheduler.h:8`

单例帧调度器，合并同一帧内的多次 repaint 请求，按优先级排序执行。

### 12.1 Priority 枚举

```cpp
enum class Priority : int { Background = 0, Normal = 1, Interactive = 2 };
```

### 12.2 Public 方法

| 签名 | 说明 |
|------|------|
| `static FrameScheduler& instance()` | 获取单例 |
| `void requestInvalidate(juce::Component& component, const juce::Rectangle<int>& dirtyArea, Priority priority)` | 请求局部重绘 |
| `void requestInvalidate(juce::Component& component, Priority priority)` | 请求全量重绘 |

---

## 13. 其他辅助组件

### 13.1 UnifiedToolbarButton

**文件**: `Source/Standalone/UI/TransportBarComponent.h:23`

统一工具栏按钮，支持图标路径、切换图标、连接边缘（用于按钮组）。

### 13.2 DigitalTimeDisplay

**文件**: `Source/Standalone/UI/TransportBarComponent.h:48`

数字时间显示（7段数码管风格），用于传输栏。

### 13.3 BpmValueField

**文件**: `Source/Standalone/UI/TransportBarComponent.h:82`

BPM 值输入字段，支持点击编辑、键盘输入、光标闪烁。

### 13.4 ViewToggleSwitch

**文件**: `Source/Standalone/UI/TransportBarComponent.h:74`

视图切换开关（轨道视图 vs 音高视图）。

### 13.5 SmallButton / SmallButtonLookAndFeel

**文件**: `Source/Standalone/UI/SmallButton.h`

小型圆角按钮 + 小字体 LookAndFeel。

### 13.6 RippleOverlayComponent

**文件**: `Source/Standalone/UI/RippleOverlayComponent.h`

鼠标轨迹特效覆盖层：涟漪 + 拖尾效果，支持多种主题风格。

### 13.7 AutoRenderOverlayComponent

**文件**: `Source/Standalone/UI/AutoRenderOverlayComponent.h`

AUTO 处理期间的全屏遮罩：半透明黑色 + 转圈动画 + 拦截所有输入。

### 13.8 KawaiiDecorationComponent

**文件**: `Source/Standalone/UI/KawaiiDecorationComponent.h`

Kawaii 主题专属浮动装饰：爱心、星星、闪光粒子。

### 13.9 OptionsDialogComponent

**文件**: `Source/Standalone/UI/OptionsDialogComponent.h`

设置对话框（Tab 式）：Audio Settings、鼠标灵敏度、键盘快捷键、语言选择。

### 13.10 UiText

**文件**: `Source/Standalone/UI/UiText.h`

工具本地化文本辅助：返回带快捷键后缀的工具名称字符串。

| 函数 | 返回值 |
|------|--------|
| `pianoRollToolSelect()` | "选择工具 [3]" |
| `pianoRollToolDrawNote()` | "画音符工具 [2]" |
| `pianoRollToolLineAnchor()` | "线锚点工具 [4]" |
| `pianoRollToolHandDraw()` | "手绘工具 [5]" |
