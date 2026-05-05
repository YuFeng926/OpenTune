---
spec_version: 1.0.0
status: draft
module: ara-vst3
doc_type: overview
generated_by: module-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# ara-vst3 模块概览

## 定位

`Source/ARA/` + `Source/Plugin/` 是 OpenTune 自 v1.3 起新增的 **VST3 + ARA 双格式集成层**。它让 OpenTune 作为插件嵌入 DAW（Logic Pro、Studio One、Cubase、REAPER 等），通过 ARA 2 协议直接访问 DAW 时间线上的音频片段，而无需用户手动导入/导出。本模块是 ARA SDK（由 JUCE 封装）与 OpenTune 内部多轨数据模型之间的 **桥接与适配层**。

与 Standalone 版本使用 `StandaloneArrangement` 为多轨数据源不同，VST3/ARA 模式下 `PlaybackRegion` 由宿主 DAW 管理，OpenTune 只负责对 **preferred region**（当前用户聚焦的那一条）做音高分析、编辑、重渲染并把合成结果回写到 DAW 播放引擎。

## 模块边界

```
上游依赖（来自宿主 DAW / ARA SDK）：
├── juce::ARADocumentControllerSpecialisation  — ARA SDK 入口
├── juce::ARAPlaybackRenderer                  — 实时音频渲染回调
├── juce::ARAAudioSource                        — 宿主音频句柄
├── juce::ARAPlaybackRegion                     — 宿主片段描述
├── juce::ARAMusicalContext                     — BPM / 节拍信息
└── ARA::PlugIn::HostAudioReader               — 从宿主读取 PCM

下游消费者（OpenTune 内部）：
├── PluginProcessor                 — 通过 ensureAraRegionMaterialization 接收 region 绑定
├── MaterializationStore            — 承载 ARA region 对应的 editable materialization
├── SourceStore                     — ARA 源音频 shared ownership
├── ResamplingManager               — ARA 源采样率适配
├── Plugin/PluginEditor (VST3)      — 读取 preferred region 驱动 PianoRoll
└── Inference (RenderCache)         — 通过 CrossoverMixer 混合 materialization
```

## 文件清单（8 个文件）

| 文件 | 行数 | 职责 |
|------|------|------|
| `ARA/OpenTuneDocumentController.h` | 91 | ARA DocumentController 声明（JUCE ARADocumentControllerSpecialisation 派生） |
| `ARA/OpenTuneDocumentController.cpp` | 213 | 14 个 ARA 回调委托到 `VST3AraSession` + 3 个 playback control 转发给 host |
| `ARA/OpenTunePlaybackRenderer.h` | 52 | PlaybackRenderer 声明 + `computeRegionBlockRenderSpan` 自由函数 |
| `ARA/OpenTunePlaybackRenderer.cpp` | 285 | `processBlock` 实时渲染：读 snapshot → 投影时间 → 从 RenderCache mix |
| `ARA/VST3AraSession.h` | 326 | Session 状态机声明（SourceSlot / RegionSlot / PublishedSnapshot / BindingState） |
| `ARA/VST3AraSession.cpp` | 1010 | 单线程 hydration worker、增量快照发布、auto-birth materialization、生命周期 |
| `Plugin/PluginEditor.h` | 155 | VST3 编辑器声明（JucePlugin_Build_VST3 守卫、Timer 心跳） |
| `Plugin/PluginEditor.cpp` | ~1500 | VST3 编辑器实现：preferred region → PianoRoll 投影、import/bind/重渲染 |

## 架构分层

```
┌──────────────────────────────────────────────────────────────┐
│              ARA SDK 边界（由 JUCE 提供）                       │
│   ARADocumentControllerSpecialisation / ARAPlaybackRenderer   │
├──────────────────────────────────────────────────────────────┤
│                   Adapter Layer                               │
│   OpenTuneDocumentController  ← 14 个 ARA 回调 + 3 个 play 控制 │
│   OpenTunePlaybackRenderer    ← processBlock（Realtime, noexcept）│
├──────────────────────────────────────────────────────────────┤
│                   Session Layer                               │
│   VST3AraSession  ← 状态所有权（SourceMap / RegionMap）          │
│    ├── hydrationWorkerThread_    (专用线程，从 host 拷 PCM)       │
│    ├── atomic_load/store<PublishedSnapshot>  (RT-safe 发布)     │
│    ├── BindingState 状态机  (Unbound→Hydrating→BoundNeedsRender→Renderable)│
│    └── auto-birth materialization  (通过 processor 回调)         │
├──────────────────────────────────────────────────────────────┤
│                   UI Layer (VST3)                             │
│   PluginUI::OpenTuneAudioProcessorEditor                      │
│    ├── Timer 30 Hz 心跳 → resolvePreferredAraRegion           │
│    ├── syncImportedAraClipIfNeeded  (检测 source range 变化)    │
│    ├── syncMaterializationProjectionToPianoRoll               │
│    └── playback control 转发到 host PlaybackController        │
└──────────────────────────────────────────────────────────────┘
```

## 设计模式

| 模式 | 应用 |
|------|------|
| **Adapter** | `OpenTuneDocumentController` / `OpenTunePlaybackRenderer` 把 ARA SDK 映射到 `VST3AraSession` |
| **Immutable Snapshot** | `PublishedSnapshot` 通过 `std::atomic_load/store<shared_ptr>` 发布，RT 线程 loadSnapshot 零拷贝 |
| **状态机** | `BindingState`: `Unbound → HydratingSource → BoundNeedsRender → Renderable` |
| **单生产者-单消费者队列** | `hydrationQueue_` + `condition_variable` 专用后台线程从宿主拷贝 PCM |
| **Lease / Generation** | `SourceSlot.leaseGeneration` 防止 stale reader 在 lease 失效后继续写 |
| **Deferred Cleanup** | `pendingRemoval` / `pendingLeaseReset` 在 `readingFromHost` 期间延后销毁 |
| **Editing Depth** | `editingDepth_` 计数避免 `willBeginEditing` 之间频繁发布快照 |
| **Epoch / Revision** | `epoch`（snapshot 版本）、`contentRevision`（source 内容）、`projectionRevision`（region 映射）三级版本号 |
| **条件编译守卫** | `JucePlugin_Build_VST3` + `JucePlugin_Enable_ARA` 双守卫 |

## 关键约束

| 约束 | 说明 |
|------|------|
| **processBlock 不可分配内存** | `playbackScratch_` 预分配到 `maxSamplesPerBlock`；loadSnapshot 仅做原子 shared_ptr 复制 |
| **同一 audio block 一份 snapshot** | 契约：渲染一个 block 内始终使用同一个 `PublishedSnapshot` |
| **projection 必须 isometric** | `playbackDurationSeconds` 与 `materializationDurationSeconds` 差值 >0.001s 则跳过渲染并 `jassertfalse` |
| **快照发布仅在 editingDepth_==0** | `willBeginEditing` 之间只做 dirty 标记，`didEndEditing` 时统一 publish |
| **hydration 单线程 + chunk 32768 samples** | 防止 host `readAudioSamples` 阻塞主线程；`cancelRead` 支持中途放弃 |
| **sample access 生命周期** | 必须 `didEnableAudioSourceSamplesAccess(true)` 之后才能创建 `HostAudioReader` lease |
| **DocumentController 拥有 Session** | `VST3AraSession` 生命周期绑定 DC，而非 Processor；DC 析构时先 detach processor 再 join worker |
| **Renderer 只读** | PlaybackRenderer 不修改 session 状态，不持久化任何数据；session 拥有 region truth，clip core 拥有 audio truth |

## 关键数字

| 指标 | 值 |
|------|----|
| Editor 心跳频率 | 30 Hz |
| Hydration chunk 大小 | 32768 samples |
| Snapshot 发布层 | 1 个全局 `atomic<shared_ptr<const PublishedSnapshot>>` |
| Projection duration 容差 | 0.001s（1ms） |
| Seconds 等值容差 | 1e-9 s（nearlyEqualSeconds）/ 1/kRenderSampleRate（Editor） |
| Audio sample 等值容差 | 1e-5f |
| Mapping 日志上限 | 前 24 个 block（后续静默） |

## Spec 文件

| 文件 | 内容 |
|------|------|
| [api.md](./api.md) | 4 个核心类的公共方法契约（DocumentController / PlaybackRenderer / VST3AraSession / VST3 Editor） |
| [data-model.md](./data-model.md) | ARA 实体（AudioSource / AudioModification / PlaybackRegion）→ OpenTune 内部（SourceSlot / RegionSlot / PublishedSnapshot）映射 |
| [business.md](./business.md) | ARA 生命周期、hydration 流程、auto-birth materialization、渲染时序（含 Mermaid） |
