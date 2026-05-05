---
module: ui-main
type: data-model
generated: true
date: 2026-05-05
warning: "⚠️ 基于源码扫描生成，可能存在遗漏或过时信息"
---

# ui-main 数据模型

本模块持有的状态主要包含三类：(1) UI 可见性/几何状态，(2) 波形/时间/渲染缓存，(3) 偏好持久化结构。

## 1. `AppPreferencesState`（偏好根结构）

定义位于 `Source/Utils/AppPreferences.h`。全局加锁访问（`std::mutex`），由 `PluginEditor::appPreferences_` 持有，写入时立即落盘。

```
AppPreferencesState
├─ SharedPreferencesState shared
│   ├─ Language language                                  # enum: English/Chinese/Japanese/Russian/Spanish
│   ├─ ThemeId theme                                      # enum: BlueBreeze/DarkBlueGrey/Aurora（默认 Aurora）
│   ├─ AudioEditingScheme::Scheme audioEditingScheme      # CorrectedF0Primary / NotesPrimary
│   ├─ PianoRollVisualPreferences pianoRollVisualPreferences
│   │   ├─ NoteNameMode noteNameMode                      # ShowAll / COnly / Hide
│   │   ├─ bool showChunkBoundaries
│   │   └─ bool showUnvoicedFrames
│   ├─ ZoomSensitivitySettings zoomSensitivity
│   │   ├─ float horizontalZoomFactor
│   │   ├─ float verticalZoomFactor
│   │   └─ float scrollSpeed
│   └─ RenderingPriority renderingPriority                # GpuFirst / CpuFirst（默认 GpuFirst）
└─ StandalonePreferencesState standalone
    ├─ KeyShortcutSettings shortcuts
    │   └─ std::array<ShortcutBinding, ShortcutId::Count> bindings
    │       # 10 个条目: PlayPause, Stop, PlayFromStart, Undo, Redo, Cut, Copy, Paste, SelectAll, Delete
    └─ MouseTrailConfig::TrailTheme mouseTrailTheme       # None/Classic/Neon/Fire/Ocean/Galaxy/CherryBlossom/Matrix
```

### 持久化 Key 表（`AppPreferences.cpp`）

| Storage Key | 类型 | 默认值 |
|-------------|------|--------|
| `shared.language` | string token | `chinese` |
| `shared.theme.activeTheme` | string token | `aurora` |
| `shared.audioEditing.scheme` | string token | `corrected-f0-primary` |
| `shared.pianoRoll.noteNameMode` | string token | `c-only` |
| `shared.pianoRoll.showChunkBoundaries` | bool | false |
| `shared.pianoRoll.showUnvoicedFrames` | bool | false |
| `shared.zoom.horizontalFactor` | double | ZoomSensitivity 默认 |
| `shared.zoom.verticalFactor` | double | ZoomSensitivity 默认 |
| `shared.scroll.speed` | double | ZoomSensitivity 默认 |
| `shared.rendering.priority` | string token | `gpu-first` |
| `standalone.mouseTrail.theme` | string token | `classic` |
| `standalone.shortcuts.playPause` | canonical shortcut string | 默认绑定 |
| `standalone.shortcuts.stop` | canonical shortcut string | 默认绑定 |
| `standalone.shortcuts.playFromStart` | canonical shortcut string | 默认绑定 |
| `standalone.shortcuts.undo` | canonical shortcut string | 默认绑定 |
| `standalone.shortcuts.redo` | canonical shortcut string | 默认绑定 |
| `standalone.shortcuts.cut` | canonical shortcut string | 默认绑定 |
| `standalone.shortcuts.copy` | canonical shortcut string | 默认绑定 |
| `standalone.shortcuts.paste` | canonical shortcut string | 默认绑定 |
| `standalone.shortcuts.selectAll` | canonical shortcut string | 默认绑定 |
| `standalone.shortcuts.delete` | canonical shortcut string | 默认绑定 |

**存储格式**：`juce::PropertiesFile::storeAsXML`
**路径**：`userApplicationDataDirectory/OpenTune/app-preferences.settings`
**并发**：`juce::InterProcessLock`（lockName = settingsFile.fullPath + ".lock"），`commonToAllUsers = false`，`millisecondsBeforeSaving = 0`（即时落盘）。

## 2. `WaveformMipmap` 分级结构（`WaveformMipmap.h`）

### 单个 `WaveformMipmap`

```
WaveformMipmap
├─ std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer_
├─ int64_t numSamples_
├─ int numChannels_
└─ Level levels_[6]
    ├─ [0] samplesPerPeak = 32      → highest detail   (~725 peaks/s @ 44.1kHz)
    ├─ [1] samplesPerPeak = 128     → 345 peaks/s
    ├─ [2] samplesPerPeak = 512     → 86 peaks/s
    ├─ [3] samplesPerPeak = 2048    → 21.5 peaks/s
    ├─ [4] samplesPerPeak = 8192    → 5.4 peaks/s
    └─ [5] samplesPerPeak = 32768   → 1.35 peaks/s（coarsest）

Level
├─ std::vector<PeakSample> peaks   # PeakSample = {int8_t min, int8_t max}（±127 量化）
├─ int64_t numSamplesCovered       # 构造时 = numSamples_
├─ bool complete                    # 全层构建完毕
└─ int64_t buildProgress            # 已填充 peak 数量（断点续建用）
```

**内存占用**（每 materialization）：
- 总 peaks ≈ numSamples × (1/32 + 1/128 + 1/512 + 1/2048 + 1/8192 + 1/32768)
       ≈ numSamples × 0.0418
- 每 PeakSample = 2 字节（int8×2）
- 示例：3 分钟 44.1kHz → 7.94M samples → ~332K peaks → ~650 KB

### `WaveformMipmapCache`

```
WaveformMipmapCache
└─ std::unordered_map<uint64_t, std::unique_ptr<WaveformMipmap>> caches_
   key = materializationId (Processor 侧的全局递增 ID)
```

生命周期管理通过 `prune(const std::unordered_set<uint64_t>& alive)`：在 `ArrangementView::onHeartbeatTick()` 中收集所有 placement 的 materializationId，传入 prune 清理不再活跃的缓存。

### LOD 选择策略

```
selectBestLevelIndex(pixelsPerSecond):
    secondsPerPixel = 1 / pixelsPerSecond
    for i in [5, 4, 3, 2, 1, 0]:          # 从粗到细
        secondsPerPeak = kSamplesPerPeak[i] / 44100.0
        if secondsPerPeak <= secondsPerPixel * 2.0
           and levels_[i].complete
           and !levels_[i].peaks.empty():
            return i
    # fallback：首个已完成的层
    for i in [0..5]:
        if levels_[i].complete && !levels_[i].peaks.empty():
            return i
    return 0
```

## 3. `TimeConverter` 状态（`TimeConverter.h`）

```
TimeConverter
├─ double zoomLevel_ = 1.0        # 钳制范围 [0.02, 10.0]（50× 缩出 / 10× 缩入）
└─ double scrollOffset_ = 0.0     # 像素偏移

派生量:
├─ kBasePixelsPerSecond = 100.0
└─ pixelsPerSecond = kBasePixelsPerSecond * zoomLevel_   # 2 px/s .. 1000 px/s
```

换算：
- `timeToPixel(s)` = `round(s * 100 * zoom - scrollOffset)`
- `pixelToTime(x)` = `(x + scrollOffset) / (100 * zoom)`

## 4. `OpenTuneAudioProcessorEditor` 运行时状态（`PluginEditor.h`）

```
OpenTuneAudioProcessorEditor（~50 个字段，节选）
├─ 外部依赖引用
│   ├─ OpenTuneAudioProcessor& processorRef_
│   ├─ AppPreferences appPreferences_
│   ├─ std::shared_ptr<LanguageState> languageState_
│   ├─ ScopedLanguageBinding languageBinding_
│   └─ KeyShortcutSettings shortcutSettings_
├─ LookAndFeel 与光标
│   ├─ OpenTuneLookAndFeel openTuneLookAndFeel_
│   ├─ AuroraLookAndFeel auroraLookAndFeel_
│   └─ juce::MouseCursor techCursor_
├─ 子组件（值类型）
│   ├─ MenuBarComponent menuBar_
│   ├─ TransportBarComponent transportBar_
│   ├─ TopBarComponent topBar_
│   ├─ TrackPanelComponent trackPanel_
│   ├─ ParameterPanel parameterPanel_
│   ├─ ArrangementViewComponent arrangementView_
│   ├─ PianoRollComponent pianoRoll_
│   ├─ RippleOverlayComponent rippleOverlay_
│   ├─ AutoRenderOverlayComponent autoRenderOverlay_
│   ├─ RenderBadgeComponent renderBadge_
│   └─ OpenTuneTooltipWindow tooltipWindow_{ this, 600 }
├─ 异步任务基础设施
│   ├─ PresetManager presetManager_
│   ├─ AsyncAudioLoader asyncAudioLoader_
│   ├─ bool isImportInProgress_
│   ├─ std::vector<PendingImport> importQueue_
│   ├─ std::unordered_map<int, double> importBatchNextStartSeconds_
│   ├─ std::unordered_map<int, int> importBatchRemainingItems_
│   ├─ int nextImportBatchId_ = 1
│   ├─ std::thread exportWorker_
│   ├─ std::atomic<bool> exportInProgress_
│   └─ std::vector<std::future<void>> backgroundTasks_
├─ 布局/视图状态
│   ├─ bool isWorkspaceView_ = true        # ArrangementView vs PianoRoll 切换
│   ├─ bool isTrackPanelVisible_ = true
│   └─ bool isParameterPanelVisible_ = true
├─ 主题/语言
│   ├─ ThemeId appliedThemeId_ = Aurora
│   ├─ Language appliedLanguage_ = Chinese
│   └─ bool f0ParamsSyncedFromInference_
├─ 推理状态追踪
│   ├─ bool inferenceActive_
│   └─ int inferenceActiveTickCounter_     # 推理中 4 帧 1 次次要刷新
├─ 最近一次同步到 PianoRoll 的快照（diff 判断）
│   ├─ uint64_t lastPianoRollMaterializationId_
│   ├─ int lastPianoRollSampleRate_
│   ├─ std::shared_ptr<PitchCurve> lastPianoRollCurve_
│   ├─ std::shared_ptr<const AudioBuffer<float>> lastPianoRollBuffer_
│   ├─ double lastSyncedBpm_
│   ├─ int lastSyncedTimeSigNum_, lastSyncedTimeSigDenom_
│   └─ std::array<float, MAX_TRACKS> lastTrackVolumes_  # Undo 记录
├─ 调式 Undo 状态
│   ├─ int lastScaleRootNote_, lastScaleType_
│   ├─ uint32_t lastUndoRedoShortcutMs_     # 去抖
│   └─ bool suppressScaleChangedCallback_
├─ RMVPE OriginalF0 overlay latch
│   ├─ bool rmvpeOverlayLatched_
│   └─ uint64_t rmvpeOverlayTargetMaterializationId_
├─ ParameterPanel 显示模式
│   └─ bool showingSingleNoteParams_
└─ 布局常量
    ├─ MENU_BAR_HEIGHT = 25
    ├─ TOP_PANEL_HEIGHT = 45
    ├─ TRANSPORT_BAR_HEIGHT = 64
    ├─ TRACK_PANEL_WIDTH = 180
    └─ PARAMETER_PANEL_WIDTH = 240

嵌套结构 PendingImport:
├─ OpenTuneAudioProcessor::ImportPlacement placement
├─ juce::File file
├─ int batchId = 0
└─ bool appendSequentially = false
```

## 5. `ArrangementViewComponent` 状态（`ArrangementViewComponent.h`）

```
ArrangementViewComponent
├─ WaveformMipmapCache waveformMipmapCache_
├─ 选区状态
│   ├─ int selectedTrack_, selectedPlacementIndex_
│   ├─ uint64_t selectedPlacementId_
│   ├─ std::set<PlacementSelectionKey> selectedPlacements_   # 多选
│   │       where PlacementSelectionKey = {trackId, placementId}
│   ├─ bool isMultiSelectMode_
│   └─ PlacementSelectionKey shiftAnchor_ (+ hasShiftAnchor_)
├─ 拖拽状态
│   ├─ bool isDraggingPlacement_ / isAdjustingGain_ / isDraggingPlayhead_ / isPanning_
│   ├─ Point<int> dragStartPos_, lastMousePos_
│   ├─ double dragStartPlacementSeconds_
│   ├─ float dragStartPlacementGain_
│   ├─ uint64_t dragStartPlacementId_
│   ├─ int dragStartTrackId_
│   └─ std::vector<DragStartState> multiDragStartStates_
│       where DragStartState = {trackId, placementId, startSeconds}
├─ 缩放/滚动
│   ├─ double zoomLevel_ = 1.0
│   ├─ int scrollOffset_ = 0 / verticalScrollOffset_ = 0
│   ├─ bool userHasManuallyZoomed_
│   ├─ float smoothScrollCurrent_
│   ├─ bool isSmoothScrolling_
│   ├─ double lastPaintedPlayheadTime_
│   └─ ScrollMode scrollMode_ = Continuous  # {Page, Continuous}
├─ 标尺
│   └─ TimeUnit timeUnit_ = Seconds    # {Seconds, Bars}
├─ 时间/节拍上下文
│   ├─ double lastContextBpm_
│   └─ int lastContextTimeSigNum_, lastContextTimeSigDenom_
├─ 推理/播放状态
│   ├─ std::atomic<bool> isPlaying_
│   ├─ bool inferenceActive_
│   └─ int waveformBuildTickCounter_   # 播放时降频构建
├─ 配置
│   ├─ ZoomSensitivityConfig::ZoomSensitivitySettings zoomSensitivity_
│   └─ KeyShortcutConfig::KeyShortcutSettings shortcutSettings_
├─ 子组件
│   ├─ juce::ScrollBar horizontalScrollBar_(false) / verticalScrollBar_(true)
│   ├─ juce::TextButton scrollModeToggleButton_, timeUnitToggleButton_
│   ├─ PlayheadOverlayComponent playheadOverlay_
│   └─ std::unique_ptr<juce::VBlankAttachment> scrollVBlankAttachment_
└─ 播放头位置源
    └─ std::weak_ptr<std::atomic<double>> positionSource_
```

**常量**：`rulerHeight_ = 30`。

## 6. `TrackPanelComponent` 状态（`TrackPanelComponent.h`）

```
TrackPanelComponent
├─ std::array<TrackControl, 12> tracks_
│   where TrackControl = {
│       MuteSoloIconButton muteButton / soloButton,
│       VolumeKnob volumeSlider,
│       CircularLevelMeter levelMeter,
│       bool isActive
│   }
├─ int activeTrackId_ = 0
├─ int visibleTrackCount_ = 2           # DEFAULT_VISIBLE_TRACKS
├─ int trackHeight_ = 100               # DEFAULT_TRACK_HEIGHT
├─ int verticalScrollOffset_ = 0
├─ int trackStartYOffset_ = 30          # 对齐 rulerHeight
├─ AddTrackButton addTrackButton_
└─ VolumeKnobLookAndFeel knobLnF_

常量：
├─ MAX_TRACKS = 12
├─ DEFAULT_VISIBLE_TRACKS = 2
├─ MIN_TRACK_HEIGHT = 70
├─ DEFAULT_TRACK_HEIGHT = 100
└─ MAX_TRACK_HEIGHT = 300
```

## 7. `FrameScheduler` 队列（`FrameScheduler.h`）

```
FrameScheduler (singleton)
└─ std::unordered_map<Component*, PendingInvalidate> pending_
    where PendingInvalidate = {
        Component::SafePointer<Component> component,
        Rectangle<int> dirty,
        bool hasDirty,
        bool fullRepaint,
        int priority    # 0=Background / 1=Normal / 2=Interactive
    }
```

**聚合规则**（同一 Component 多次 requestInvalidate）：
- dirty 区域取 union
- `fullRepaint = fullRepaint || newIsFull`
- priority 取 max
- 一次 `triggerAsyncUpdate()` 排空时按 Interactive → Normal → Background 三轮

## 8. `ParameterPanelSyncContext/Decision`（`ParameterPanelSync.h`）

纯数据结构，无身份/生命周期。每帧由 `PluginEditor::syncParameterPanelFromSelection()` 从 PianoRoll 收集，传入 `resolveParameterPanelSyncDecision(scheme, ctx)` 返回 Decision。

```
ParameterPanelSyncContext
├─ bool hasSelectedNoteParameters
├─ float selectedNoteRetuneSpeedPercent / selectedNoteVibratoDepth / selectedNoteVibratoRate
├─ bool hasSelectedSegmentRetuneSpeed
├─ float selectedSegmentRetuneSpeedPercent
├─ float clipRetuneSpeedPercent / clipVibratoDepth / clipVibratoRate
└─ bool wasShowingSelectionParameters

ParameterPanelSyncDecision
├─ bool shouldSetRetuneSpeed; float retuneSpeedPercent
├─ bool shouldSetVibratoDepth; float vibratoDepth
├─ bool shouldSetVibratoRate; float vibratoRate
└─ bool nextShowingSelectionParameters
```

## 9. `RenderStatusSnapshot`（`AutoRenderOverlayComponent.h`）

```
RenderStatusSnapshot
├─ RenderStatus status              # Idle / Rendering / Ready
├─ RenderCache::ChunkStats chunkStats   # {idle, blank, pending, running, ...}
├─ uint64_t materializationId
├─ uint64_t placementId
└─ bool hasContent

AutoRenderOverlayDecision
├─ bool shouldClearTargetClip
├─ bool shouldShowOverlay
├─ bool shouldDisplayRenderStatus
└─ RenderStatus displayStatus
```

### 状态机（由 `evaluateRenderStatus` 推导）

| 条件 | 输出 |
|------|------|
| `chunkStats.hasActiveWork()` | `Rendering` |
| `total == 0 || !hasNonBlankChunks` | `Idle` |
| `hasPublishedAudio` | `Ready` |
| 否则 | `Idle` |

### overlay 决策表（由 `evaluateAutoRenderOverlay`）

| hasAutoTargetClip | status | autoTuneProcessing | 结果 |
|---|---|---|---|
| true | materializationId == 0 | - | `shouldClearTargetClip` |
| true | Ready/Idle | false | `shouldClearTargetClip` |
| true | Rendering | - | `shouldShowOverlay + shouldDisplayRenderStatus` |
| false | Rendering | - | `shouldShowOverlay + shouldDisplayRenderStatus` |
| false | Idle/Ready | - | 什么都不做 |

## ⚠️ 待确认

1. `PianoRollVisualPreferences` 结构（`Utils/PianoRollVisualPreferences.h`）未在本模块详细展开，`showUnvoicedFrames`/`showChunkBoundaries` 等字段的默认值应与 utils 模块文档对齐
2. `ZoomSensitivityConfig::ZoomSensitivitySettings` 的字段类型 `float horizontalZoomFactor / verticalZoomFactor / scrollSpeed` 的数值范围（是否归一化）未在本模块源码中验证
3. `KeyShortcutConfig::ShortcutId::Count` 目前硬编码 10（见 `kShortcutStorageKeys`）；但实际枚举是否覆盖所有活跃快捷键需对照 `KeyShortcutConfig.h` 确认
4. `PendingImport::appendSequentially` 与 `importBatchNextStartSeconds_` 的组合语义（是否严格链式追加、是否跳过重叠 placement）未从头文件字段名完全明确
5. `ArrangementViewComponent::smoothScrollCurrent_` 与 `isSmoothScrolling_` 的更新时机、与 `VBlankAttachment` 回调的耦合点未在头文件显式声明
6. `WaveformMipmap::Level::numSamplesCovered` 字段在当前实现中始终等于 `numSamples_`，是否为未来支持部分覆盖（流式加载）预留的扩展点？
