---
module: ui-main
type: overview
generated: true
warning: "⚠️ 基于源码扫描生成，可能存在遗漏或过时信息"
---

# ui-main 模块概览

## 模块定位

ui-main 是 OpenTune 的**主界面 UI 层**，负责窗口布局、用户交互、组件间通信和异步任务协调。采用 **Mediator 模式**，以 `PluginEditor` 为中心枢纽连接所有 UI 子组件。

## 技术栈

- **框架**: JUCE (C++17)
- **渲染**: JUCE Graphics + VBlank 同步播放头
- **线程模型**: 消息线程为主 + 后台 std::async 任务
- **状态管理**: Listener/Observer 模式 + Timer 心跳同步

## 核心组件

| 组件 | 职责 | 文件 |
|------|------|------|
| **PluginEditor** | 中心 Mediator，管理所有子组件 | `PluginEditor.h/cpp` (~2800 行) |
| **TopBarComponent** | 顶部容器（菜单 + 传输控制 + 侧边栏开关） | `TopBarComponent.h/cpp` |
| **MenuBarComponent** | 应用菜单（File/Edit/View），支持 macOS 原生菜单 | `MenuBarComponent.h/cpp` |
| **TransportBarComponent** | 播放控制、BPM、调式选择、视图切换 | `TransportBarComponent.h/cpp` |
| **TrackPanelComponent** | 左侧轨道面板（选择/Mute/Solo/音量/电平表） | `TrackPanelComponent.h/cpp` |
| **ArrangementViewComponent** | 多轨道编排视图（Clip 显示/拖拽/波形） | `ArrangementViewComponent.h/cpp` |
| **ParameterPanel** | 右侧参数面板（RetuneSpeed/Vibrato/工具选择） | `ParameterPanel.h/cpp` |
| **TimelineComponent** | 时间线标尺（时间/小节显示、网格） | `TimelineComponent.h/cpp` |
| **PlayheadOverlayComponent** | 高性能播放头（VBlank 独立刷新、全 atomic） | `PlayheadOverlayComponent.h/cpp` |
| **TimeConverter** | 时间 ↔ 像素坐标转换（考虑 BPM/缩放/滚动） | `TimeConverter.h/cpp` |
| **WaveformMipmap** | 6 级波形 Mipmap 缓存（增量构建） | `WaveformMipmap.h/cpp` |
| **FrameScheduler** | 帧调度器单例（合并 repaint 请求、优先级排序） | `FrameScheduler.h` |

### 辅助组件

| 组件 | 职责 |
|------|------|
| **RippleOverlayComponent** | 鼠标轨迹特效（涟漪 + 拖尾，8 种主题） |
| **AutoRenderOverlayComponent** | AUTO/RMVPE 处理遮罩（拦截输入 + 转圈动画） |
| **KawaiiDecorationComponent** | Kawaii 主题浮动装饰（爱心/星星） |
| **OptionsDialogComponent** | 设置对话框（音频/鼠标灵敏度/快捷键/语言） |
| **SmallButton** | 紧凑型圆角按钮 |
| **UiText** | 工具名称本地化辅助 |

## 依赖关系

```
PluginEditor
├── OpenTuneAudioProcessor (数据层, 双向通信)
├── AsyncAudioLoader (异步音频加载)
├── F0ExtractionService (F0 提取服务)
├── PresetManager (预设管理)
├── LocalizationManager (国际化)
├── TopBarComponent
│   ├── MenuBarComponent
│   └── TransportBarComponent
├── TrackPanelComponent
├── ArrangementViewComponent
│   ├── TimeConverter
│   ├── WaveformMipmapCache
│   ├── PlayheadOverlayComponent
│   └── SmallButton
├── ParameterPanel
├── PianoRollComponent (不在本模块，但作为核心子组件使用)
├── RippleOverlayComponent
└── AutoRenderOverlayComponent
```

## 关键设计决策

### D-1: Mediator 而非 MVC
所有子组件通过 Listener 向 PluginEditor 通知事件，PluginEditor 负责跨组件协调。没有独立的 Controller 层。优点是单一协调点、简单直接；代价是 PluginEditor 体量大（~2800 行）。

### D-2: VBlank 播放头
`PlayheadOverlayComponent` 使用 JUCE 的 VBlank 机制独立于主 Timer 刷新播放头位置，避免 30Hz Timer 成为播放头流畅度的瓶颈。所有属性使用 `std::atomic` 确保线程安全。

### D-3: 增量波形缓存
`WaveformMipmap` 采用 6 级 LOD 架构（32~32768 samples/peak），使用 `int8_t` 压缩存储，`buildIncremental()` 支持时间预算控制的增量构建，避免导入大文件时 UI 卡顿。

### D-4: 两阶段导入
音频导入分为 prepare（后台线程，CPU 密集）和 commit（消息线程，写锁）两阶段，确保消息线程不被长时间阻塞。

### D-5: 事务覆盖层
AUTO 和 RMVPE 处理使用 latch 机制管理覆盖层：启动时锁定，满足释放条件时解锁。覆盖层拦截所有用户输入，确保异步处理期间 UI 状态一致。

## 文件清单

| 文件 | 行数 | 说明 |
|------|------|------|
| `PluginEditor.h` | 245 | 主编辑器声明 |
| `PluginEditor.cpp` | 2797 | 主编辑器实现 |
| `TopBarComponent.h` | 42 | 顶部栏 |
| `TopBarComponent.cpp` | ~200 | 顶部栏实现 |
| `MenuBarComponent.h` | 104 | 菜单栏 |
| `MenuBarComponent.cpp` | ~300 | 菜单栏实现 |
| `TransportBarComponent.h` | 218 | 传输控制栏 |
| `TransportBarComponent.cpp` | ~600 | 传输控制栏实现 |
| `TrackPanelComponent.h` | 544 | 轨道面板（含内联辅助类） |
| `TrackPanelComponent.cpp` | ~400 | 轨道面板实现 |
| `ArrangementViewComponent.h` | 236 | 编排视图 |
| `ArrangementViewComponent.cpp` | ~1200 | 编排视图实现 |
| `TimelineComponent.h` | 89 | 时间线 |
| `TimelineComponent.cpp` | ~300 | 时间线实现 |
| `ParameterPanel.h` | 165 | 参数面板 |
| `ParameterPanel.cpp` | ~500 | 参数面板实现 |
| `PlayheadOverlayComponent.h` | 97 | 播放头覆盖层 |
| `PlayheadOverlayComponent.cpp` | ~80 | 播放头实现 |
| `WaveformMipmap.h` | 105 | 波形 Mipmap |
| `WaveformMipmap.cpp` | ~200 | 波形 Mipmap 实现 |
| `TimeConverter.h` | 53 | 时间转换器 |
| `TimeConverter.cpp` | ~100 | 时间转换器实现 |
| `FrameScheduler.h` | 140 | 帧调度器 |
| `OptionsDialogComponent.h` | 597 | 设置对话框 |
| `SmallButton.h` | 81 | 小型按钮 |
| `RippleOverlayComponent.h` | 226 | 涟漪覆盖层 |
| `AutoRenderOverlayComponent.h` | 152 | 渲染遮罩 |
| `KawaiiDecorationComponent.h` | 258 | Kawaii 装饰 |
| `UiText.h` | 30 | UI 文本辅助 |

## 关联模块

| 模块 | 关系 |
|------|------|
| `piano-roll` | PianoRoll 音高编辑器，PluginEditor 的核心子组件 |
| `inference` | AI 模型层（RMVPE/HiFiGAN），通过 Processor 间接调用 |
| `dsp` | 信号处理（Mel/Resampling），通过 Processor 间接使用 |
| `utils` | PitchCurve/Note/UndoAction 等数据结构 |
| `audio` | AsyncAudioLoader 异步加载 |
| `services` | F0ExtractionService 提取服务 |
