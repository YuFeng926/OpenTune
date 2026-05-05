---
module: ui-main
type: overview
generated: true
date: 2026-05-05
warning: "⚠️ 基于源码扫描生成，可能存在遗漏或过时信息"
---

# ui-main 模块概览

## 模块定位

`ui-main` 是 OpenTune 桌面 Standalone 应用的**主界面 UI 层**，承担窗口容器、菜单栏、传输控制、轨道面板、编排视图、参数面板、偏好设置对话框、渲染状态徽章、播放头覆盖层等所有非钢琴卷帘的一级交互区域，并通过帧调度器 (FrameScheduler) 聚合 repaint 请求、通过 WaveformMipmap 缓存多级波形数据。

本模块体量最大（`PluginEditor.cpp` 约 2800+ 行，`ArrangementViewComponent.cpp` 约 1700 行，`TransportBarComponent.cpp` 约 900 行），采用 **Mediator（中介者）模式**：所有子组件将用户动作通过 `Listener` 回调到 `OpenTuneAudioProcessorEditor`，由它协调 Processor（core-processor）、AsyncAudioLoader（audio）、AppPreferences（utils）等外部服务。

## 上下游依赖

**上游依赖（本模块使用）**：
- `core-processor`：`OpenTuneAudioProcessor` 作为数据源（position atomic、BPM、时值、MaterializationId、ChunkStats、TrackArrangement、PianoKeyAudition）
- `ui-theme`：`UIColors`、`OpenTuneLookAndFeel`、`AuroraLookAndFeel`、`ThemeTokens`、`ToolbarIcons`（主题色/字体/图标路径）
- `ui-piano-roll`：`PianoRollComponent` 作为中央编辑器（非工作区视图）
- `utils`：`AppPreferences`（偏好读写）、`PresetManager`、`LocalizationManager`、`KeyShortcutConfig`、`MouseTrailConfig`、`PianoRollVisualPreferences`、`ZoomSensitivityConfig`、`AudioEditingScheme`、`PitchCurve`、`ParameterPanelSync`、`AppLogger`
- `audio`：`AsyncAudioLoader`（异步文件导入）、`AudioFormatRegistry`
- `inference`：通过 `RenderCache::StateSnapshot`、`RenderStatusSnapshot` 读取渲染进度
- JUCE：`AudioProcessorEditor`、`Timer`、`VBlankAttachment`、`AsyncUpdater`、`FileDragAndDropTarget`、`MenuBarModel`、`TooltipWindow`、`DialogWindow`、`StandalonePluginHolder`

**下游依赖（使用本模块）**：
- 无。本模块是终端 UI 层，入口由 `EditorFactoryStandalone.cpp` 构造 `OpenTuneAudioProcessorEditor`。

## 核心职责

1. **窗口布局与组件装配**：`PluginEditor::resized()` 负责顶栏 / 左栏 / 中心 / 右栏四象限切分，带 12px 阴影留白
2. **事件中介**：实现 `ParameterPanel::Listener`、`MenuBarComponent::Listener`、`TransportBarComponent::Listener`、`TrackPanelComponent::Listener`、`ArrangementViewComponent::Listener`、`PianoRollComponent::Listener` 六类 Listener，桥接子组件动作到 Processor
3. **心跳同步**：主 `Timer` 以 30Hz（空闲）或 10Hz（推理中）回调，刷新 BPM/时值、播放状态、ParameterPanel 选区参数、电平表、渲染徽章与 AutoRenderOverlay
4. **异步任务协调**：多文件导入队列 (`importQueue_`)、导入 batch 管理、`std::async` 后台任务 (`backgroundTasks_`)、导出线程 (`exportWorker_`)
5. **偏好持久化**：持有 `AppPreferences`，通过 `TabbedPreferencesDialog` 打开多标签页（音频设备 / 通用 / 主题语言 / 钢琴卷帘 / 缩放 / 快捷键 / 鼠标轨迹）
6. **VBlank 播放头**：`PlayheadOverlayComponent` 与 `ArrangementViewComponent` 内部 VBlankAttachment 独立于 Timer 绘制，从 `std::weak_ptr<std::atomic<double>> positionSource_` 读取位置
7. **波形 Mipmap 缓存**：`WaveformMipmapCache` 按 materializationId 存取 6 级 LOD，`buildIncremental(timeBudgetMs)` 心跳预算分摊构建
8. **主题切换 / 语言切换 / 鼠标轨迹 / 涟漪特效** 的联动广播

## 文件清单

| 文件 | 说明 | 体量 |
|------|------|------|
| `Source/Standalone/PluginEditor.h` | 编辑器声明，六类 Listener 聚合 | ~250 行 |
| `Source/Standalone/PluginEditor.cpp` | 中枢实现（timerCallback / resized / 导入 / 导出 / 偏好 / 主题） | ~2800 行 |
| `Source/Standalone/UI/TopBarComponent.h/cpp` | 顶部容器：MenuBar + TransportBar + 左右栏开关 | 小 |
| `Source/Standalone/UI/MenuBarComponent.h/cpp` | 菜单栏（File/Edit/View + 鼠标轨迹），支持 Standalone / Plugin Profile | 中 |
| `Source/Standalone/UI/TransportBarComponent.h/cpp` | 播放/暂停/停止/循环/录音/BPM/Tap/调式/视图切换 | 大 |
| `Source/Standalone/UI/TrackPanelComponent.h/cpp` | 轨道面板：Mute/Solo/音量旋钮/环形电平表/+按钮 | 大 |
| `Source/Standalone/UI/ArrangementViewComponent.h/cpp` | 编排视图：ClipBounds/拖拽/波形/时标/垂直+水平滚动/VBlank | 特大 |
| `Source/Standalone/UI/ParameterPanel.h/cpp` | 参数面板：RetuneSpeed/Vibrato/NoteSplit/工具按钮 | 大 |
| `Source/Standalone/UI/PlayheadOverlayComponent.h/cpp` | 高性能播放头（独立 VBlank） | 小 |
| `Source/Standalone/UI/WaveformMipmap.h/cpp` | 6 级 LOD 波形 Mipmap 缓存，int8 压缩 | 中 |
| `Source/Standalone/UI/TimeConverter.h/cpp` | 时间 ↔ 像素转换（100 px/s × zoom） | 小 |
| `Source/Standalone/UI/FrameScheduler.h` | 单例 AsyncUpdater，聚合 repaint 请求，按优先级排序 | 140 行 |
| `Source/Standalone/UI/UiText.h` | 工具名称 + 快捷键本地化拼接 | 30 行 |
| `Source/Standalone/UI/SmallButton.h` | 圆角小按钮 + LookAndFeel | 80 行 |
| `Source/Standalone/UI/RippleOverlayComponent.h` | 鼠标涟漪/轨迹特效（8 主题） | 230 行 |
| `Source/Standalone/UI/OpenTuneTooltipWindow.h` | 深色 Tooltip + 快捷键 Badge | 170 行 |
| `Source/Editor/AutoRenderOverlayComponent.h` | AUTO 全屏遮罩（拦截输入 + spinner），含 `evaluateAutoRenderOverlay` 纯函数 | 250 行 |
| `Source/Editor/RenderBadgeComponent.h` | 右上角小徽章（"渲染中 x/y"） | 35 行 |
| `Source/Editor/Preferences/SharedPreferencePages.h/cpp` | Shared 偏好页面构造（主题 / 语言 / 音频方案 / 钢琴卷帘显示 / 缩放 / 渲染优先级） | 中 |
| `Source/Editor/Preferences/StandalonePreferencePages.h/cpp` | Standalone 专属页面（AudioDeviceSelectorComponent / 快捷键捕获 / 鼠标轨迹） | 中 |
| `Source/Editor/Preferences/TabbedPreferencesDialog.h` | 标签页容器 + 关闭按钮 | 70 行 |
| `Source/Utils/ParameterPanelSync.h` | 纯函数 `resolveParameterPanelSyncDecision`（选区参数决策） | 90 行 |
| `Source/Utils/AppPreferences.h/cpp` | 偏好持久化（InterProcessLock + PropertiesFile XML） | 中 |

共 **35 个文件**（含 .h / .cpp）。

## 架构约束

- **消息线程独占**：所有 JUCE 组件方法 / Timer / Listener 回调必须在消息线程执行；`FrameScheduler::requestInvalidate` 从其它线程调用时通过 `MessageManager::callAsync` 切换
- **线程安全状态**：`ArrangementViewComponent::isPlaying_` 为 `std::atomic<bool>`，播放头位置通过 `std::weak_ptr<std::atomic<double>>` 传递（VBlank 线程读）
- **SafePointer 防悬挂**：`FrameScheduler` 使用 `juce::Component::SafePointer` 持有组件；`callAfterDelay` 闭包统一用 SafePointer
- **增量构建预算**：`WaveformMipmap::buildIncremental(timeBudgetMs)` 每轮受限（ArrangementView 在 Timer 中分摊），batchSize=256 个 peak 后检查超时
- **AppPreferences 锁**：所有 setter / getState 持 `std::mutex`，写入立即 `saveIfNeeded()`（`millisecondsBeforeSaving = 0`）
- **偏好存储路径**：`userApplicationDataDirectory/OpenTune/app-preferences.settings`（XML），配套 `InterProcessLock` 防多实例争用
- **Profile 分支**：`MenuBarComponent::Profile`（Standalone/Plugin）与 `TransportBarComponent::LayoutProfile`（StandaloneFull/VST3AraSingleClip）影响菜单项可用性与布局

## 关键设计决策

### D-1: Mediator 模式
所有子组件以 Listener 接口通知 `OpenTuneAudioProcessorEditor`，避免子组件之间横向耦合。代价是 PluginEditor 本身体量大。

### D-2: VBlank 播放头 + 30Hz Timer
主 Timer 30Hz 刷新"慢"状态（BPM/参数/电平/状态文本），播放头位置由 `PlayheadOverlayComponent`（PianoRoll 侧）和 `ArrangementViewComponent::scrollVBlankAttachment_` 通过 VBlank 回调独立读 `std::atomic<double>`，避免 30Hz 上限成为播放流畅度瓶颈。

### D-3: 6 级 WaveformMipmap + int8 压缩
`kSamplesPerPeak = {32, 128, 512, 2048, 8192, 32768}`，每个 peak 存 `int8_t min/max`（即 ±127 量化）。`selectBestLevel(pixelsPerSecond)` 挑选 `secondsPerPeak ≤ secondsPerPixel * 2` 的最大 LOD（即分辨率刚好够用），降级至已完成的 LOD。

### D-4: FrameScheduler 聚合
`FrameScheduler::instance()` 为单例 `AsyncUpdater`，多次 `requestInvalidate` 合并 dirty 区域（union）、升级优先级（max(old, new)）、`triggerAsyncUpdate` 排空时按优先级 Interactive → Normal → Background 处理；仅对 `isShowing()` 的组件调 `repaint()`。

### D-5: 纯函数决策器
- `resolveParameterPanelSyncDecision`（`ParameterPanelSync.h`）：根据 `AudioEditingScheme` + 选区上下文决定 ParameterPanel 显示 Note / LineAnchor / Clip 参数
- `evaluateAutoRenderOverlay`（`AutoRenderOverlayComponent.h`）：根据 `RenderStatusSnapshot` + `hasAutoTargetClip` + `autoTuneProcessing` 决定遮罩显隐和目标清理

### D-6: 偏好双层结构
`AppPreferencesState = { SharedPreferencesState, StandalonePreferencesState }`。Shared 层（语言/主题/编辑方案/钢琴卷帘视觉/缩放/渲染优先级）可被未来的 VST 宿主复用；Standalone 层（快捷键 10 项 / 鼠标轨迹）仅 Standalone 使用。

### D-7: 多文件导入队列 + batch
`importQueue_` 串行化多文件导入；`importBatchNextStartSeconds_` 按批次计算下一文件的顺序 append 起点；`importBatchRemainingItems_` 跟踪完成释放。
