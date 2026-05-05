---
module: ui-main
type: api
generated: true
date: 2026-05-05
warning: "⚠️ 基于源码扫描生成，可能存在遗漏或过时信息"
---

# ui-main API 契约

> 所有类位于 `namespace OpenTune`。参数若无特殊说明均为消息线程调用。

## 1. `OpenTuneAudioProcessorEditor`（`PluginEditor.h`）

主编辑器窗口 = `juce::AudioProcessorEditor` + 6 个 Listener + `FileDragAndDropTarget` + `LanguageChangeListener` + 私有 `Timer`。

### 1.1 生命周期

| 方法 | 用途 |
|------|------|
| `explicit OpenTuneAudioProcessorEditor(OpenTuneAudioProcessor&)` | 构造：持有 processorRef_；初始化 AppPreferences / LocalizationManager 绑定；构造所有子组件；注册六类 Listener；启动 `Timer`（30Hz）；通过 `callAfterDelay(60)` 启用原生标题栏 |
| `~OpenTuneAudioProcessorEditor()` | 停止 Timer；`waitForBackgroundUiTasks()`；join `exportWorker_`；解除 LanguageState 绑定 |
| `void paint(juce::Graphics&) override` | 填充 `UIColors::backgroundDark` 背景 |
| `void resized() override` | 按 `topBar / trackPanel / parameterPanel / center` 四象限布局，12 px 阴影留白；`arrangementView_` 与 `pianoRoll_` 重叠铺满中央（通过 `setVisible` 切换） |

### 1.2 拖放（`juce::FileDragAndDropTarget`）

- `bool isInterestedInFileDrag(const juce::StringArray& files)`：返回 true 当存在受支持的音频后缀
- `void filesDropped(const juce::StringArray& files, int x, int y)`：命中 Track 区域则导入该轨；否则弹出 `promptTrackSelectionForDroppedFile`

### 1.3 心跳（私有 `Timer`）

`void timerCallback() override`：
1. `syncSharedAppPreferences()` 拉取偏好快照
2. `pianoRoll_.isAutoTuneProcessing()` → `setInferenceActive(active)` → `startTimerHz(active ? 10 : 30)`
3. `syncParameterPanelFromSelection()`（调用 `resolveParameterPanelSyncDecision`）
4. `arrangementView_.onHeartbeatTick()` / `pianoRoll_.onHeartbeatTick()`（仅在 `isShowing()` 时）
5. 同步 BPM / 时值（diff 触发 `transportBar_.setBpm()` / `pianoRoll_.setBpm()`）
6. 同步 PianoRoll 编辑 materialization（diff materializationId/sampleRate/curve/buffer）
7. 更新 `transportBar_.setPositionSeconds`、`RenderStatusSnapshot`、RMVPE overlay latch、AutoRenderOverlay 显隐、RenderBadge 文本
8. 同步播放状态到 transport / pianoRoll / arrangementView
9. 每 4 帧（推理中）或每帧（空闲）刷 12 条轨道电平表

### 1.4 Listener 回调实现（摘要）

- **`ParameterPanel::Listener`**：`retuneSpeedChanged` / `vibratoDepthChanged` / `vibratoRateChanged` / `noteSplitChanged` / `toolSelected(int)` / `parameterDragEnded(paramId, oldValue, newValue)`（用于 Undo）
- **`MenuBarComponent::Listener`**：`importAudioRequested` → 打开 FileChooser 并 queue 导入；`exportAudioRequested(ExportType)`；`savePresetRequested` / `loadPresetRequested`；`preferencesRequested` → `showPreferencesDialog()`；`helpRequested` → 打开本地 `docs/UserGuide.html`；`showWaveformToggled` / `showLanesToggled` / `noteNameModeChanged(NoteNameMode)` / `showChunkBoundariesToggled(bool)` / `showUnvoicedFramesToggled(bool)`；`themeChanged(ThemeId)` → `applyThemeToEditor`；`undoRequested` / `redoRequested` → `performUndoRedoAction`；`mouseTrailThemeChanged(MouseTrailConfig::TrailTheme)`
- **`TransportBarComponent::Listener`**：`playRequested` / `pauseRequested` / `stopRequested` / `loopToggled(bool)` / `bpmChanged(double)` / `scaleChanged(int rootNote, int scaleType)` / `viewToggled(bool workspaceView)`
- **`TrackPanelComponent::Listener`**：`trackSelected(int)` / `trackMuteToggled(int, bool)` / `trackSoloToggled(int, bool)` / `trackVolumeChanged(int, float)` / `trackHeightChanged(int)`
- **`ArrangementViewComponent::Listener`**：`placementSelectionChanged(trackId, placementId)` / `placementTimingChanged(trackId, placementIndex)` / `placementDoubleClicked(trackId, placementIndex)` / `verticalScrollChanged(int)`
- **`PianoRollComponent::Listener`**：`playheadPositionChangeRequested(seconds)` / `playPauseToggleRequested` / `stopPlaybackRequested` / `autoTuneRequested` / `pitchCurveEdited(startFrame, endFrame)` / `escapeKeyPressed`

### 1.5 键盘与语言

- `bool keyPressed(const juce::KeyPress& key)`：处理 Undo/Redo/播放控制快捷键（依据 `shortcutSettings_`）
- `void languageChanged(Language)`：递归调 `refreshLocalizedText()` 于各子组件

### 1.6 私有辅助（节选）

| 方法 | 用途 |
|------|------|
| `showPreferencesDialog()` | 组装 `createAudioPages` + `create`（Shared） + `createStandaloneOnlyPages` → `TabbedPreferencesDialog` → `DialogWindow::LaunchOptions::launchAsync` |
| `syncSharedAppPreferences()` | 从 `appPreferences_.getState()` 拉取最新快照，diff 应用到 LocalizationManager / 主题 / 钢琴卷帘视觉 / 缩放灵敏度 / 快捷键 / 渲染优先级 / 鼠标轨迹 |
| `applyThemeToEditor(ThemeId)` | `UIColors::applyTheme(ThemeId)` → `openTuneLookAndFeel_.refresh` → 广播 `applyTheme()` 到各子组件 → repaint |
| `getRenderStatusSnapshot()` | 从 processor 拉 RenderCache 快照，调 `makeRenderStatusSnapshot` |
| `queuePendingImport` / `startPendingImport` / `processNextImportInQueue` / `releaseImportBatchSlot` | 多文件导入串行管线 |
| `launchBackgroundUiTask(std::function<void()>)` | `std::async(std::launch::async, ...)` 入队到 `backgroundTasks_` |
| `waitForBackgroundUiTasks()` | 析构时同步等待所有 future |
| `syncPianoRollFromPlacementSelection(trackId, placementIndex)` | 选区切换时把 clip 参数推送到 PianoRoll |
| `computeTrackAppendStartSeconds(trackId)` | 计算追加导入的起始时间（取末尾 placement end） |

## 2. `TopBarComponent`（`TopBarComponent.h`）

容器：持有 `MenuBarComponent&` + `TransportBarComponent&` 引用 + 两个 `UnifiedToolbarButton` 作为左右栏开关。

| 方法 | 签名 | 说明 |
|------|------|------|
| 构造 | `TopBarComponent(MenuBarComponent&, TransportBarComponent&)` | 引用绑定 |
| `void paint(juce::Graphics&)` override | 顶栏背景（主题色） |
| `void resized()` override | 菜单栏左侧 + 传输栏中央 + 开关右侧 |
| `void applyTheme()` | 主题刷新 |
| `void setSidePanelsVisible(bool track, bool param)` | 同步按钮 ToggleState |
| `void setTrackPanelToggleVisible(bool)` | VST 单 clip 模式可隐藏左栏开关 |
| `void refreshLocalizedText()` | 本地化刷新 |

**公共回调字段**：
- `std::function<void()> onToggleTrackPanel`
- `std::function<void()> onToggleParameterPanel`

## 3. `MenuBarComponent`（`MenuBarComponent.h`）

继承 `juce::MenuBarModel`；内嵌 `juce::MenuBarComponent menuBar_`。

**枚举**：`Profile { Standalone, Plugin }`、`ExportType { SelectedClip, Track, Bus }`、`MenuItemIDs`（ImportAudio=1, Export*, SavePreset, LoadPreset, EditUndo=50, EditRedo, Show*=100, Theme*, MouseTrail*=150, OpenPreferences=200, OpenHelp）

### Listener

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
    virtual void noteNameModeChanged(NoteNameMode) = 0;
    virtual void showChunkBoundariesToggled(bool) = 0;
    virtual void showUnvoicedFramesToggled(bool) = 0;
    virtual void themeChanged(ThemeId) = 0;
    virtual void undoRequested() = 0;
    virtual void redoRequested() = 0;
    virtual void mouseTrailThemeChanged(MouseTrailConfig::TrailTheme) = 0;
};
```

### 公开 API

| 方法 | 说明 |
|------|------|
| `explicit MenuBarComponent(OpenTuneAudioProcessor&, Profile = Standalone)` | |
| `void addListener(Listener*)` / `removeListener(Listener*)` | |
| `void refreshLocalizedText()` | |
| `void setMouseTrailTheme(TrailTheme)` / `setNoteNameMode(NoteNameMode)` / `setShowChunkBoundaries(bool)` / `setShowUnvoicedFrames(bool)` | 同步菜单勾选状态 |
| `StringArray getMenuBarNames() / PopupMenu getMenuForIndex / void menuItemSelected` | override `MenuBarModel` |

## 4. `TransportBarComponent`（`TransportBarComponent.h`）

内部类：
- `UnifiedToolbarButton`：icon + `ConnectedEdge` 分组渲染
- `DigitalTimeDisplay`：LED 七段风格时间显示，`setTimeString` + `setTooltip`
- `BpmValueField`：可点击数值编辑，光标 Timer 闪烁；`setValue` / `getValue` / `onCommit(double)`

### Listener

```cpp
class Listener {
    virtual void playRequested() = 0;
    virtual void pauseRequested() = 0;
    virtual void stopRequested() = 0;
    virtual void loopToggled(bool enabled) = 0;
    virtual void bpmChanged(double newBpm) = 0;
    virtual void scaleChanged(int rootNote, int scaleType) = 0;
    virtual void viewToggled(bool workspaceView) = 0;
    virtual void recordRequested() {}
};
```

### 公开 API

| 方法 | 说明 |
|------|------|
| `TransportBarComponent()` / `~TransportBarComponent()` | |
| `void applyTheme()` / `setEmbeddedInTopBar(bool)` / `setLayoutProfile(LayoutProfile)` / `getLayoutProfile()` | `LayoutProfile ∈ {StandaloneFull, VST3AraSingleClip}` |
| `void setPlaying(bool)` / `isPlaying() const` | |
| `void setLoopEnabled(bool)` / `isLoopEnabled()` | |
| `void setBpm(double)` / `getBpm()` | |
| `void setScale(int rootNote, int scaleType)` | |
| `void setPositionSeconds(double)` | 更新 DigitalTimeDisplay |
| `void setWorkspaceView(bool)` / `isWorkspaceView()` | |
| `void setRenderStatusText(const juce::String&)` | |
| `juce::Component& getFileButton() / getEditButton() / getViewButton()` | Plugin profile 重定位菜单触发按钮 |

**公共字段**：`onFileMenuRequested` / `onEditMenuRequested` / `onViewMenuRequested` 回调。

## 5. `TrackPanelComponent`（`TrackPanelComponent.h`）

内部类：
- `CircularLevelMeter`（30Hz 刷新，`setLevel(dB)` / `setClipping(bool)` / `setInferenceActive(bool)`）
- `MuteSoloIconButton`（`IconType { Mute, Solo }`）
- `VolumeKnob`（RotaryVerticalDrag，range [0, 4]，mid=1.0=0dB，双击回默认）
- `VolumeKnobLookAndFeel`（覆盖 `drawRotarySlider`）
- `TransparentLabel`（hover 变色）
- `AddTrackButton`（60Hz Timer，涟漪 + glow + `onClick`）

### Listener

```cpp
class Listener {
    virtual void trackSelected(int trackId) = 0;
    virtual void trackMuteToggled(int trackId, bool muted) = 0;
    virtual void trackSoloToggled(int trackId, bool solo) = 0;
    virtual void trackVolumeChanged(int trackId, float volume) = 0;
    virtual void trackHeightChanged(int newHeight) {}
    virtual void verticalScrollChanged(int offset) {}
};
```

### 公开 API

| 方法 | 说明 |
|------|------|
| `TrackPanelComponent()` | 初始化 12 条 TrackControl（Mute/Solo/Volume/LevelMeter） |
| `void setActiveTrack(int)` / `getActiveTrack()` | |
| `void setTrackMuted(int, bool)` / `isTrackMuted(int)` | |
| `void setTrackSolo(int, bool)` / `isTrackSolo(int)` | |
| `void setTrackVolume(int, float)` / `getTrackVolume(int)` | |
| `void setTrackLevel(int, float levelDB)` / `setTrackClipping(int, bool)` | |
| `void setInferenceActive(bool)` | 推理中降频 30→12Hz |
| `void setVisibleTrackCount(int)` / `getVisibleTrackCount()` / `showMoreTracks()` | |
| `void setTrackHeight(int)` / `getTrackHeight()` | `MIN=70 / DEFAULT=100 / MAX=300` |
| `void setVerticalScrollOffset(int)` / `getVerticalScrollOffset()` | 与 ArrangementView Y 滚动同步 |
| `void setTrackStartYOffset(int)` / `getTrackStartYOffset()` | 与 rulerHeight 对齐（默认 30） |

常量：`MAX_TRACKS = 12`，`DEFAULT_VISIBLE_TRACKS = 2`，12 色 `trackPastelColors`。

## 6. `ArrangementViewComponent`（`ArrangementViewComponent.h`）

继承 `juce::Component` + `juce::ScrollBar::Listener` + `juce::Timer`。

### Listener

```cpp
class Listener {
    virtual void placementSelectionChanged(int trackId, uint64_t placementId) = 0;
    virtual void placementTimingChanged(int trackId, int placementIndex) = 0;
    virtual void placementDoubleClicked(int, int) {}
    virtual void trackHeightChanged(int newHeight) {}
    virtual void verticalScrollChanged(int newOffset) {}
};
```

### 公开 API

| 方法 | 说明 |
|------|------|
| `ArrangementViewComponent(OpenTuneAudioProcessor&)` | 订阅 processor、装配 scrollbar、创建 `scrollVBlankAttachment_` |
| `void onHeartbeatTick()` | editor Timer 调用；触发 `buildWaveformCaches(timeBudgetMs)`、状态聚合、autoScroll |
| `void setIsPlaying(bool)` | atomic 存储 + `playheadOverlay_.setPlaying` + 触发同步 |
| `void setPlayheadColour(juce::Colour)` | 主题切换 |
| `void setPlayheadPositionSource(std::weak_ptr<std::atomic<double>>)` | 注入播放头位置 |
| `void setZoomLevel(double)` / `setScrollOffset(int)` / `setVerticalScrollOffset(int)` | |
| `void setInferenceActive(bool)` | |
| `void fitToContent()` | 计算所有 placement 最大结束时间 → 调整 zoom |
| `void setZoomSensitivity(ZoomSensitivityConfig::ZoomSensitivitySettings)` | |
| `void setShortcutSettings(KeyShortcutConfig::KeyShortcutSettings)` | |
| `bool isWaveformCacheCompleteForMaterialization(int trackId, uint64_t materializationId) const` | |
| `void resetUserZoomFlag()` / `bool hasUserManuallyZoomed()` | |

**私有关键方法**：`hitTestPlacement(Point<int>)` → `{trackId, placementIndex, bounds, isTopEdge}`；`absoluteTimeToContentX/ViewportX`、`viewportXToAbsoluteTime`、`updateAutoScroll()`、`performPageScroll(double)`、`onScrollVBlankCallback(double)`、`drawTimeRuler`、`drawGridLines`、`buildWaveformCaches(timeBudgetMs)`。

常量：`rulerHeight_ = 30`。

## 7. `ParameterPanel`（`ParameterPanel.h`）

### Listener

```cpp
class Listener {
    virtual void retuneSpeedChanged(float speed) = 0;
    virtual void vibratoDepthChanged(float value) = 0;
    virtual void vibratoRateChanged(float value) = 0;
    virtual void noteSplitChanged(float value) = 0;
    virtual void toolSelected(int toolId) = 0;
    virtual void autoTuneRequested() {}
    virtual void parameterDragEnded(int paramId, float oldValue, float newValue) {}
};
```

### 公开 API

| 方法 | 说明 |
|------|------|
| `ParameterPanel()` / `~ParameterPanel()` | 构造工具按钮 + 四个大旋钮 |
| `void setActiveTool(int toolId)` | 刷新按钮 togged 状态 |
| `void setRetuneSpeed(float)` / `setVibratoDepth(float)` / `setVibratoRate(float)` / `setNoteSplit(float)` / `setF0Min(float)` / `setF0Max(float)` | 均 `dontSendNotification` |
| `float getRetuneSpeed() / getVibratoDepth() / getVibratoRate() / getNoteSplit() / getF0Min() / getF0Max()` const | |
| `void applyTheme()` / `refreshLocalizedText()` | |

**参数 ID 常量**：`kParamRetuneSpeed=0, kParamVibratoDepth=1, kParamVibratoRate=2, kParamNoteSplit=3`。

内嵌 `ToolIconButton`（toolId + iconPath + textIcon），`LargeKnobLookAndFeel`（覆盖 `drawRotarySlider` 渲染大号白色圆环）。

## 8. `PlayheadOverlayComponent`（`PlayheadOverlayComponent.h`）

轻量播放头渲染组件，所有 setter 直接 `repaint()`：

| setter | 含义 |
|---|---|
| `setPlayheadSeconds(double)` | |
| `setZoomLevel(double)` | |
| `setScrollOffset(double)` | |
| `setTimelineStartSeconds(double)` | |
| `setPianoKeyWidth(int)` | PianoRoll 的 key 列宽度偏移 |
| `setPlaying(bool)` | |
| `setPlayheadColour(juce::Colour)` | 默认 `0xFFE74C3C` |

私有：`paint(Graphics&)`、`calculatePlayheadPixelX(double) const`。

## 9. `WaveformMipmap` / `WaveformMipmapCache`（`WaveformMipmap.h`）

### 常量

- `kNumLevels = 6`
- `kBaseSampleRate = 44100`
- `kSamplesPerPeak[6] = {32, 128, 512, 2048, 8192, 32768}`

### `PeakSample`

`int8_t min, max`；`setRange(float, float)` 量化到 ±127；`getMin/getMax/getMagnitude` 还原到 [-1, 1]；`isZero()`。

### `Level`

`std::vector<PeakSample> peaks`、`int64_t numSamplesCovered`、`bool complete`、`int64_t buildProgress`。

### `WaveformMipmap` API

| 方法 | 说明 |
|------|------|
| `void setAudioSource(std::shared_ptr<const AudioBuffer<float>>)` | 重置并按 `kSamplesPerPeak[i]` 预分配每层 peaks |
| `bool hasSource() const` | |
| `bool isSourceChanged(buffer) const` | |
| `int64_t getNumSamples() / int getNumChannels()` | |
| `Level& getLevel(int) / const Level& getLevel(int) const` | |
| `bool buildIncremental(double timeBudgetMs)` | 串行填充各层（batchSize=256 peaks/批），超预算时返回；返回是否有进展 |
| `bool isComplete() const` | |
| `float getBuildProgress() const` | 全局 peaks 完成率 |
| `const Level& selectBestLevel(double pixelsPerSecond) const` / `int selectBestLevelIndex(...) const` | 选择最高可用 LOD（`secondsPerPeak ≤ secondsPerPixel * 2`），降级已完成层 |
| `void clear()` | |

### `WaveformMipmapCache` API

| 方法 | 说明 |
|------|------|
| `WaveformMipmap& getOrCreate(uint64_t materializationId)` | |
| `void remove(uint64_t)` | |
| `void prune(const std::unordered_set<uint64_t>& alive)` | 清除不在活跃集合内的条目 |
| `void clear()` | |
| `bool buildIncremental(double timeBudgetMs)` | 遍历所有缓存并分摊预算 |
| `const WaveformMipmap* get(uint64_t) const` | |

## 10. `TimeConverter`（`TimeConverter.h/cpp`）

| 方法 | 说明 |
|------|------|
| `TimeConverter()` / `~TimeConverter()` | |
| `void setZoom(double zoomLevel)` | 钳制到 `[0.02, 10.0]` |
| `void setScrollOffset(double)` | |
| `int timeToPixel(double seconds) const` | `round(seconds * kBasePixelsPerSecond * zoom - scrollOffset)` |
| `double pixelToTime(int pixelX) const` | `(pixelX + scrollOffset) / (kBasePixelsPerSecond * zoom)` |
| `double getPixelsPerSecond() const` | `kBasePixelsPerSecond * zoom` |

常量：`kBasePixelsPerSecond = 100.0`。非拷贝。

## 11. `FrameScheduler`（`FrameScheduler.h`）

单例 `juce::AsyncUpdater`。

**枚举**：`Priority { Background=0, Normal=1, Interactive=2 }`

| 方法 | 说明 |
|------|------|
| `static FrameScheduler& instance()` | 静态局部单例 |
| `void requestInvalidate(Component&, const Rectangle<int>& dirty, Priority = Normal)` | 非消息线程会 `MessageManager::callAsync` 重定向；聚合时 union dirty 区；`max(prevPriority, newPriority)`；触发 `triggerAsyncUpdate()` |
| `void requestInvalidate(Component&, Priority = Normal)` | 全量 repaint 变体；与 dirty 变体并存时升级为 `fullRepaint = true` |

**内部**：`handleAsyncUpdate()` 按 Interactive → Normal → Background 三轮遍历 `pending_`，仅对 `isShowing()` 的 SafePointer 调 `repaint(dirty)` 或 `repaint()`。

## 12. `UiText`（`UiText.h`）

命名空间内 inline 函数（返回 `juce::String`，附加 `\t[shortcut]`）：
- `pianoRollToolSelect()` → `kMouseSelectTool + "\t[3]"`
- `pianoRollToolDrawNote()` → `kDrawNoteTool + "\t[2]"`
- `pianoRollToolLineAnchor()` → `kLineAnchorTool + "\t[4]"`
- `pianoRollToolHandDraw()` → `kHandDrawTool + "\t[5]"`

## 13. `SmallButton` / `SmallButtonLookAndFeel`（`SmallButton.h`）

- `SmallButton`（`juce::TextButton`）：`setFontHeight(float)`（默认 13f），`paintButton` 绘制 3px 圆角矩形
- `SmallButtonLookAndFeel`：覆盖 `getTextButtonFont` 返回 11f 字体

## 14. `RippleOverlayComponent`（`RippleOverlayComponent.h`）

继承 `juce::Component` + `juce::Timer`。全局鼠标监听（`Desktop::addGlobalMouseListener`），30Hz Timer：
- `setTrailTheme(MouseTrailConfig::TrailTheme)`
- `paint/paintTrail/paintRipples` 依据 `MouseTrailConfig::ThemeStyle`（baseColor / accentColor / thickness / useGradient / hueShift / fadeSpeed）
- `mouseDown` 新增 `Ripple{x, y, radius=6, life=1}`
- `shouldIgnoreComponent` 识别带 `"minimalKnob"` 属性的祖先组件

## 15. `OpenTuneTooltipWindow`（`OpenTuneTooltipWindow.h`）

继承 `juce::TooltipWindow`，默认 600ms 延时，深色 + 快捷键 Badge 样式。
- 静态 `getTooltipSize(const juce::String& tip)` 按 `\n` 分行计算宽高
- 内部 `TooltipLookAndFeel` 覆盖 `getTooltipBounds` / `drawTooltip`
- 文本格式：`"Label\nShortcut"`（第二行 → 小徽章）

## 16. `AutoRenderOverlayComponent` + 纯函数（`AutoRenderOverlayComponent.h`）

### `RenderStatus` enum

`Idle, Rendering, Ready`

### `RenderStatusSnapshot` struct

`status, chunkStats, materializationId, placementId, hasContent`

### `AutoRenderOverlayDecision` struct

`shouldClearTargetClip, shouldShowOverlay, shouldDisplayRenderStatus, displayStatus`

### 纯函数

- `RenderStatus evaluateRenderStatus(const RenderCache::StateSnapshot&)`
- `RenderStatusSnapshot makeRenderStatusSnapshot(materializationId, placementId, cacheSnapshot)`
- `AutoRenderOverlayDecision evaluateAutoRenderOverlay(snapshot, hasAutoTargetClip, autoTuneProcessing)`

### `AutoRenderOverlayComponent`

- `setInterceptsMouseClicks(true, true)` + `setAlwaysOnTop(true)`
- `setMessageText(const String&)` / `setMessageText(main, sub)` / `setStatus(RenderStatus, customSubText)`
- `paint` 绘制 70% 黑遮罩 + 标题 + 转圈 spinner（`Time::getMillisecondCounterHiRes * 0.001` 相位）
- `visibilityChanged`：可见时 `startTimer(16)` ≈ 60Hz，隐藏时停 Timer
- 拦截所有 `mouseDown/Up/Drag/Move/WheelMove` 与 `keyPressed`（消费全部）

## 17. `RenderBadgeComponent`（`RenderBadgeComponent.h`）

极简右上角徽章：
- `setMessageText(const String&)` 触发 repaint
- `paint` 绘制 80% 黑色圆角背景 + 13px 白字居中
- `setInterceptsMouseClicks(false, false)` + `setAlwaysOnTop(true)`

## 18. Preferences 页面（`Editor/Preferences/`）

### `TabbedPreferencesDialog`（`TabbedPreferencesDialog.h`）

```cpp
struct PageSpec {
    juce::String title;
    std::unique_ptr<juce::Component> content;
};

class TabbedPreferencesDialog : public juce::Component {
    explicit TabbedPreferencesDialog(std::vector<PageSpec> pages);
    void paint(juce::Graphics&) override;
    void resized() override;
};
```

内部 `juce::TabbedComponent tabbedComponent_{ TabsAtTop }`，`tabBarDepth = 32`；底部关闭按钮通过 `findParentComponentOfClass<DialogWindow>()->exitModalState(0)` 关闭。

### `SharedPreferencePages`（`SharedPreferencePages.h`）

```cpp
struct SharedPreferencePages {
    static std::vector<TabbedPreferencesDialog::PageSpec> create(
        AppPreferences& appPreferences,
        std::function<void()> onPreferencesChanged);
    static std::unique_ptr<juce::Component> createRenderingPriorityComponent(
        AppPreferences& appPreferences,
        std::function<void()> onPreferencesChanged,
        std::function<void(bool forceCpu)> onRenderingPriorityChanged);
};
```

页面包括：通用（主题 / 语言）、钢琴卷帘（NoteNameMode / 波形显示 / Chunk 边界 / 无声帧）、缩放（horizontalFactor / verticalFactor / scrollSpeed）、音频编辑方案（CorrectedF0Primary / NotesPrimary）。

### `StandalonePreferencePages`（`StandalonePreferencePages.h`）

```cpp
struct StandalonePreferencePages {
    static std::vector<TabbedPreferencesDialog::PageSpec> createAudioPages(
        juce::AudioDeviceManager* audioDeviceManager,
        AppPreferences& appPreferences,
        std::function<void()> onPreferencesChanged,
        std::function<void(bool forceCpu)> onRenderingPriorityChanged);
    static std::vector<TabbedPreferencesDialog::PageSpec> createStandaloneOnlyPages(
        AppPreferences& appPreferences,
        std::function<void()> onPreferencesChanged);
};
```

页面包括：音频设备（`AudioDeviceSelectorComponent` + RenderingPriority）、快捷键（10 个可捕获 `KeyPress` 的绑定行，调 `tryBuildCapturedBinding`）、鼠标轨迹（8 主题预览）。

## 19. `AppPreferences`（`Utils/AppPreferences.h/cpp`）

### 状态结构

```cpp
struct SharedPreferencesState {
    Language language = Language::Chinese;
    ThemeId theme = ThemeId::Aurora;
    AudioEditingScheme::Scheme audioEditingScheme = CorrectedF0Primary;
    PianoRollVisualPreferences pianoRollVisualPreferences;
    ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity;
    RenderingPriority renderingPriority = GpuFirst;
};
struct StandalonePreferencesState {
    KeyShortcutConfig::KeyShortcutSettings shortcuts;
    MouseTrailConfig::TrailTheme mouseTrailTheme = Classic;
};
struct AppPreferencesState {
    SharedPreferencesState shared;
    StandalonePreferencesState standalone;
};
enum class RenderingPriority { GpuFirst = 0, CpuFirst };
```

### StorageOptions

```cpp
struct StorageOptions {
    juce::String applicationName = "OpenTune";
    juce::File settingsDirectory;          // 空 → userApplicationDataDirectory/OpenTune
    juce::String fileName = "app-preferences.settings";
};
```

### 公开 API

| 方法 | 说明 |
|------|------|
| `explicit AppPreferences(const StorageOptions& = {})` | 构造时 `initialiseStorage()` + `load()` |
| `AppPreferencesState getState() const` | 加锁返回副本 |
| `void load()` | 从 PropertiesFile 解码到 state_ |
| `void save()` / `void flush()` | 写 PropertiesFile（`saveIfNeeded`） |
| `void setLanguage(Language)` | |
| `void setTheme(ThemeId)` | |
| `void setAudioEditingScheme(AudioEditingScheme::Scheme)` | |
| `void setPianoRollVisualPreferences(const PianoRollVisualPreferences&)` | |
| `void setNoteNameMode(NoteNameMode)` / `setShowChunkBoundaries(bool)` / `setShowUnvoicedFrames(bool)` | |
| `void setZoomSensitivity(const ZoomSensitivityConfig::ZoomSensitivitySettings&)` | |
| `void setStandaloneShortcuts(const KeyShortcutConfig::KeyShortcutSettings&)` | |
| `void setRenderingPriority(RenderingPriority)` | |
| `void setMouseTrailTheme(MouseTrailConfig::TrailTheme)` | |

每个 setter 写入后立即 `saveLocked()`（`millisecondsBeforeSaving = 0`）。

## 20. `ParameterPanelSync`（`Utils/ParameterPanelSync.h`）

纯函数（无状态）：

```cpp
struct ParameterPanelSyncContext {
    bool hasSelectedNoteParameters;
    float selectedNoteRetuneSpeedPercent, selectedNoteVibratoDepth, selectedNoteVibratoRate;
    bool hasSelectedSegmentRetuneSpeed;
    float selectedSegmentRetuneSpeedPercent;
    float clipRetuneSpeedPercent, clipVibratoDepth, clipVibratoRate;
    bool wasShowingSelectionParameters;
};

struct ParameterPanelSyncDecision {
    bool shouldSetRetuneSpeed; float retuneSpeedPercent;
    bool shouldSetVibratoDepth; float vibratoDepth;
    bool shouldSetVibratoRate; float vibratoRate;
    bool nextShowingSelectionParameters;
};

inline ParameterPanelSyncDecision resolveParameterPanelSyncDecision(
    AudioEditingScheme::Scheme scheme,
    const ParameterPanelSyncContext& context) noexcept;
```

决策逻辑：调用 `AudioEditingScheme::resolveParameterTarget(scheme, ParameterKind::RetuneSpeed, targetContext)`：
- `SelectedNotes` 且 `hasSelectedNoteParameters` → 推送选中 Note 的三参数，`nextShowingSelectionParameters = true`
- `SelectedLineAnchorSegments` 且 `hasSelectedSegmentRetuneSpeed` → 推送 segment retune
- 否则若 `wasShowingSelectionParameters` 为 true，恢复到 Clip 参数（并将 `nextShowingSelectionParameters = false`）
- 其余返回空决策

## ⚠️ 待确认

1. `ArrangementViewComponent::buildWaveformCaches(timeBudgetMs)` 的具体预算值（每帧）未在头文件显式，需翻 cpp 确认是否固定 2–4ms 或自适应
2. `FrameScheduler` 在多组件同时 repaint（例如 AutoRenderOverlay 可见时是否会短路 Normal 优先级的波形层）的实际时序未验证
3. `AppPreferences` 对 `KeyShortcutSettings` 的序列化仅硬编码 10 个 `kShortcutStorageKeys`；新增 ShortcutId 需同步更新数组（存在维护陷阱）
4. `TransportBarComponent::LayoutProfile::VST3AraSingleClip` 的具体隐藏/显示差异仅在 cpp 内，未在头文件接口中明确
5. `RippleOverlayComponent::shouldIgnoreComponent` 依赖 `"minimalKnob"` 组件属性字符串，PianoRoll / ParameterPanel 是否一致设置该属性未全面审阅
6. `PlayheadOverlayComponent`（本模块声明简单）与 `PianoRollComponent` 内独立 VBlank 绑定、`ArrangementViewComponent::scrollVBlankAttachment_` 的耦合关系未穷尽分析
