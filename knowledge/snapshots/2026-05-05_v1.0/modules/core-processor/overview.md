---
spec_version: 1.0.0
status: draft
module: core-processor
doc_type: overview
generated_by: module-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# Core-Processor 模块概览

## 定位

`core-processor` 是 OpenTune 的**核心音频处理器骨干**，以 `OpenTuneAudioProcessor`（`juce::AudioProcessor` 派生）为运行时外壳，组合三条"真值线"：
- **Source 真值**：原始导入音频的不可变身份（`SourceStore`）
- **Editable 真值**：可编辑载荷（notes / corrected F0 / detectedKey / RenderCache）按 Materialization 粒度管理（`MaterializationStore`）
- **Placement/Mix 真值**：Standalone 模式下多轨时间轴上的摆放（`StandaloneArrangement`）

模块负责：音频导入 pipeline（两阶段：worker prepare + main thread commit）、F0 提取服务调度、Chunk 级声码器渲染调度、`processBlock` 实时混音、项目状态序列化、双格式（Standalone / VST3+ARA）seam 分流，以及 Undo 支持的 retire/revive 垃圾回收。

## 模块边界

```
上游依赖（Inference / DSP / Utils / Services）:
├── Inference/F0InferenceService       — 异步 F0 推理（RMVPE）
├── Inference/VocoderDomain            — 声码器域聚合（Service + Scheduler）
├── Inference/RenderCache              — 分 Chunk 渲染缓存 + 状态机
├── Services/F0ExtractionService       — 后台 F0 提取任务队列
├── Services/ImportedClipF0Extraction  — 导入片段原始 F0 提取 inline 辅助
├── DSP/ResamplingManager              — 导入重采样到 44.1kHz
├── DSP/MelSpectrogram                 — Chunk 渲染阶段生成 mel
├── DSP/ChromaKeyDetector              — DetectedKey 类型（仅类型引用）
├── DSP/CrossoverMixer                 — processBlock 干声/湿声频段混合
├── Utils/PitchCurve / Note            — 存于 Materialization 的音高与音符
├── Utils/SilentGapDetector            — 静音段检测（Chunk 边界对齐）
├── Utils/TimeCoordinate               — 采样/秒/帧转换 + kRenderSampleRate=44100
├── Utils/UndoManager + PlacementActions — 操作 Undo/Redo
├── Utils/AccelerationDetector         — GPU/CPU 后端切换
└── Utils/AppLogger / ModelPathResolver / PianoKeyAudition 等

下游消费者:
├── Plugin/PluginEditor (ARA/VST3)      — 通过 EditorFactoryPlugin 创建
├── Standalone/PluginEditor             — 通过 EditorFactoryStandalone 创建
├── UI/PianoRollComponent               — 读取 notes、pitchCurve、chunkStats
├── UI/MultiTrackComponent              — 读取 StandaloneArrangement 快照
└── ARA DocumentController (可选) — 绑定共享 sourceStore/materializationStore/resamplingManager
```

## 文件清单（24 个文件）

| 文件 | 行数 | 职责 |
|------|------|------|
| `PluginProcessor.h` | 619 | `OpenTuneAudioProcessor` 声明：Import API、Playback Read API、Chunk Render 调度、Undo outcome 结构体 |
| `PluginProcessor.cpp` | 3881 | 处理器实现：ctor/dtor、processBlock、导入 pipeline、partial render、序列化、导出、split/merge/delete |
| `StandaloneArrangement.h` | 151 | 多轨时间轴数据模型声明：Track / Placement / PlaybackSnapshot |
| `StandaloneArrangement.cpp` | 620 | 多轨状态管理实现（ReadWriteLock + snapshot SpinLock） |
| `MaterializationStore.h` | 210 | 可编辑载荷仓库声明：CreateMaterializationRequest / MaterializationSnapshot / PendingRenderJob |
| `MaterializationStore.cpp` | 696 | 可编辑载荷仓库实现 + render queue + 静音段驱动 chunk 边界 |
| `SourceStore.h` | 77 | 源音频仓库声明：CreateSourceRequest / SourceSnapshot |
| `SourceStore.cpp` | 160 | 源音频仓库实现（ReadWriteLock） |
| `Audio/AsyncAudioLoader.h` | 203 | 后台线程音频文件加载（含进度/完成回调 + 有效性令牌） |
| `Audio/AudioFormatRegistry.h` | 25 | 导入音频格式注册接口 |
| `Audio/AudioFormatRegistry.cpp` | 217 | WAV/AIFF/FLAC/Ogg/CoreAudio/MP3/WMF 的条件注册 + 容器探测 |
| `Services/F0ExtractionService.h` | 98 | 多线程 F0 提取任务服务声明（请求去重 + 取消） |
| `Services/F0ExtractionService.cpp` | 138 | Worker 循环 + LockFreeQueue + token-based 取消 |
| `Services/ImportedClipF0Extraction.h` | 117 | Inline 辅助：从 MaterializationSnapshot 提取 F0 + alignment 诊断字段 |
| `Editor/EditorFactory.h` | 17 | 编辑器工厂函数声明（`createOpenTuneEditor`） |
| `Editor/EditorFactoryPlugin.cpp` | 16 | VST3/ARA 构建：返回 `PluginUI::OpenTuneAudioProcessorEditor` |
| `Standalone/EditorFactoryStandalone.cpp` | 16 | Standalone 构建：返回 `OpenTuneAudioProcessorEditor` |
| `Utils/F0Timeline.h` | 75 | F0 帧-秒坐标工具（hopSize × sampleRate → secondsPerFrame） |
| `Utils/MaterializationState.h` | 14 | `OriginalF0State` 枚举（NotRequested/Extracting/Ready/Failed） |
| `Utils/MaterializationTimelineProjection.h` | 74 | Timeline ↔ Materialization-local 秒坐标投影 |
| `Utils/SourceWindow.h` | 21 | Source-absolute 时间窗结构（lineage 事实） |
| `Utils/AudioEditingScheme.h` | 226 | 编辑方案（Notes-primary / CorrectedF0-primary）+ 参数/AutoTune 目标解析 |
| `Utils/PlacementActions.h` | 108 | Undo Action 类声明：Split/Merge/Delete/Move/GainChange |
| `Utils/PlacementActions.cpp` | 193 | Undo Action 实现：基于 retire/revive 双态切换 |

## 架构分层

```
┌───────────────────────────────────────────────────────────────────────┐
│                     JUCE Runtime Shell                                 │
│   OpenTuneAudioProcessor : AudioProcessor, AsyncUpdater                │
│     ├── processBlock(float/double)  ← 实时混音 + fade-out              │
│     ├── chunkRenderWorkerLoop       ← 后台 vocoder 渲染调度            │
│     ├── materializationRefreshService (F0ExtractionService, 1+64)      │
│     └── handleAsyncUpdate → runReclaimSweepOnMessageThread             │
├───────────────────────────────────────────────────────────────────────┤
│                     Data Stores (共享所有权)                            │
│   SourceStore              ← ReadWriteLock, 软删除                     │
│   MaterializationStore     ← ReadWriteLock + renderQueueMutex          │
│   StandaloneArrangement    ← ReadWriteLock + SpinLock(snapshot)        │
├───────────────────────────────────────────────────────────────────────┤
│                     Inference Adapter                                  │
│   F0InferenceService* (unique_ptr, 惰性初始化)                         │
│   VocoderDomain*       (unique_ptr, 惰性初始化)                        │
│   RenderCache (持有者在 MaterializationEntry 内)                       │
├───────────────────────────────────────────────────────────────────────┤
│                     Dual-Format Seam                                   │
│   EditorFactory.createOpenTuneEditor()                                 │
│     ├── JucePlugin_Build_Standalone → OpenTuneAudioProcessorEditor     │
│     └── !Standalone                  → PluginUI::OpenTuneAudioProcessorEditor │
├───────────────────────────────────────────────────────────────────────┤
│                     Import Pipeline                                    │
│   AsyncAudioLoader.run()  → prepareImport()  → commit*AsPlacement      │
│   AudioFormatRegistry.registerImportFormats() (WAV/AIFF + optional)    │
└───────────────────────────────────────────────────────────────────────┘
```

## 设计模式

| 模式 | 应用 |
|------|------|
| **三层身份模型** | Source → Materialization → Placement 各负责一层事实 |
| **两阶段导入** | Worker 线程 `prepareImport` + 主线程 `commitPreparedImportAs*` |
| **软删除 (retire/revive)** | 三层 Store 均支持 retire + `runReclaimSweepOnMessageThread` 延迟物理回收 |
| **不可变快照 (Snapshot)** | 音频线程读取 `StandaloneArrangement::PlaybackSnapshot` (shared_ptr<const>) 与 `MaterializationSnapshot` |
| **惰性初始化 (Double-checked)** | `ensureF0Ready()` / `ensureVocoderReady()`，原子 flag + mutex + attemptedFlag |
| **AsyncUpdater** | `scheduleReclaimSweep` → `triggerAsyncUpdate` → 主线程 sweep，避免锁内回收 |
| **请求去重 (Token)** | `F0ExtractionService::submit` 同 `requestKey` 返回 `AlreadyInProgress` |
| **版本门 (targetRevision)** | Chunk 渲染完成时对照 `RenderCache` 版本决定发布/重试 |
| **静态工厂条件编译** | `createOpenTuneEditor` 由 `EditorFactoryPlugin` / `EditorFactoryStandalone` 分别提供定义 |
| **Pimpl-like 组合** | Processor 组合 shared_ptr 三个 Store + unique_ptr 推理服务，支持 ARA 会话切换共享指针 |

## 关键数字

| 指标 | 值 |
|------|----|
| 最大轨道数 `MAX_TRACKS` / `kTrackCount` | 12 |
| 存储采样率 `kRenderSampleRate` | 44100 Hz（所有 Materialization 音频统一落盘） |
| 默认设备采样率 `DefaultSampleRate` | 44100 Hz |
| Fade-out 时长 | 200 ms |
| Render lookahead | 5 s |
| Render timeout | 30 s |
| Render poll interval | 20 ms |
| F0 刷新服务 worker 数 | 1 |
| F0 刷新服务最大队列 | 64 |
| Merge 时间戳 epsilon | 1/44100 s |
| Split 最小片段时长 | 0.1 s |
| ARA window 重复 epsilon | 0.001 s |

## 核心约束

1. **存储采样率锁定 44.1kHz**：`prepareImport` 强制重采样输入到 `TimeCoordinate::kRenderSampleRate`；`readPlaybackAudio` 内部线性插值到 device 采样率。
2. **音频线程零锁竞争**：`processBlock` 只调 `loadPlaybackSnapshot()`（SpinLock 取 shared_ptr 副本）和 `MaterializationStore::getPlaybackReadSource`；写操作全部在消息线程或 worker 线程。
3. **惰性推理初始化**：构造函数不触发 ONNX Runtime 加载；`ensureF0Ready`/`ensureVocoderReady` 在首次需要时才尝试，失败会 latch `attemptedFlag=true`。
4. **三层软删除级联**：Placement retire → Materialization retire →（当无任一引用时）Source retire，由 `runReclaimSweepOnMessageThread` 三阶段推进。
5. **双格式 seam**：`EditorFactory.h` 仅声明工厂函数；`JucePlugin_Build_Standalone` 宏在构建时选择一个 `.cpp` 提供实现，Processor 本身无分支。
6. **ARA 绑定后共享 Store**：`didBindToARA` 将 `sourceStore_`/`materializationStore_`/`resamplingManager_` 替换为 DocumentController 的共享实例，实现跨 plugin 实例一致。
7. **Chunk 边界对齐 hopSize**：`MaterializationStore::buildChunkBoundariesFromSilentGaps` 让 chunk 边界落在 hopSize 倍数，最后一 chunk 除外（`freezeRenderBoundaries` 处理 partial）。

## Spec 文件

| 文件 | 内容 |
|------|------|
| [api.md](./api.md) | 所有公共类/结构体/方法契约（OpenTuneAudioProcessor、Stores、ExtractionService、Factory） |
| [data-model.md](./data-model.md) | 内存数据结构（Source/Materialization/Placement/Snapshot/PendingRenderJob/FrozenRenderBoundaries）+ 关系图 + 序列化格式 |
| [business.md](./business.md) | 核心业务流程（import → F0 → render、processBlock、dual-format seam、reclaim sweep）+ Mermaid 流程图 |
