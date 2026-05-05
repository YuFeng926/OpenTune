---
spec_version: 1.0.0
status: draft
doc_type: cross-cutting/caching
generated_by: synthesis-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# 缓存体系（Caching）

OpenTune 作为 C++ 音频桌面应用，不涉及 Web / 分布式缓存。本文档汇总所有**进程内内存缓存**的层级、键、容量、失效策略与失效触发源。

## 缓存总览表

| 缓存 | 持有者 | 键（Key） | 容量 / TTL / 驱逐策略 | 失效触发 | 模块 |
|---|---|---|---|---|---|
| **RenderCache**（声码器渲染 PCM） | `Inference::RenderCache`（单例，跨 materialization 共享） | `(materializationId, chunkId, renderRevision)` | **LRU**，全局上限 `kDefaultGlobalCacheLimitBytes = 256 MB`；单 chunk 超出则整个 materialization 条目淘汰 | `renderRevision` 递增（notes / correctedSegments / pitchCurve / detectedKey 变化）；`enqueueMaterializationPartialRenderById`；`replaceMaterializationWithNewLineage` | inference |
| **MaterializationStore 已渲染音频** | `MaterializationStore` | `materializationId` | 无硬上限（随 materialization 生命周期）；`retirePlacement` → `ReclaimSweep` 回收 | `retirePlacement` / `runReclaimSweepOnMessageThread` | core-processor |
| **SourceStore 原始 PCM** | `SourceStore` | `sourceId` | 无硬上限；ReclaimSweep 回收 | `retireSource` / ReclaimSweep | core-processor |
| **PitchCurve COW Snapshot** | `MaterializationSnapshot::pitchCurve` | `materializationId + revision` | 短暂（每次修改生成新 snapshot，旧 snapshot 在无读者后析构） | 编辑 F0 / AutoTune / Worker 回写 | pitch-correction |
| **PlaybackSnapshot** | `OpenTuneAudioProcessor::publishPlaybackSnapshotLocked` | 进程级最新 | 单槽（`std::atomic<shared_ptr<const PlaybackSnapshot>>`），旧 snapshot 在读者散去后析构 | arrangement / materialization 变动 | core-processor |
| **WaveformMipmap** | `WaveformMipmap` | `materializationId + sourceRange` | 6 级 LOD（int8 压缩），常驻；按 materialization 销毁释放 | 原始音频变更 / materialization 回收 | ui-main |
| **Mel Filterbank 常量表** | `MelSpectrogramProcessor` | 按 `(sampleRate, nFft, nMels, fMin, fMax)` | 进程级常量缓存（无驱逐） | 参数变化时重建 | dsp |
| **RMVPE thread_local scratch** | `RMVPEExtractor`（thread_local） | 每线程 | 复用避免每次 inference 分配；线程退出释放 | 模型重载 / 重建 extractor | inference |
| **VocoderScratchBuffers** | `OnnxVocoderBase`（thread_local） | 每线程（按 I/O shape） | 每次 synthesize 复用；shape 变化才扩容 | 模型切换 / shape 变 | inference |
| **ONNX Session** | `ModelFactory` / `VocoderFactory` | 按 `(backend, modelPath, sessionOptions)` | 单实例由 `F0InferenceService` / `VocoderInferenceService` 持有；切换后端时重建 | `resetInferenceBackend(forceCpu)` / 后端失败降级 | inference |
| **CrossoverMixer per-placement 状态** | `RenderCache::getCrossoverMixer` | `placementId` | LR4 滤波器状态 per-placement；placement 销毁释放 | placement retire / revive | dsp, inference |
| **Theme UIColors 静态缓存** | `UIColors`（全局 static） | 单槽 | 进程级；主题切换时 `applyTheme()` 重写 | `applyTheme(themeId)` | ui-theme |
| **AppPreferences** | `AppPreferences`（内存态 + XML 磁盘） | Storage Key（21 键） | 内存副本 + 立即 XML 落盘（InterProcessLock）；无容量限制 | `setXxx` 调用 | utils |
| **LocalizationManager 绑定栈** | `LocalizationManager::LanguageState` | 栈式 `ScopedLanguageBinding` | 按作用域 RAII | push/pop binding | utils |
| **AccelerationDetector 结果** | `AccelerationDetector`（懒初始化） | 进程级 | 单次初始化 + 缓存；无驱逐 | `forceRedetect()` 显式调用 | utils |

## 关键缓存详解

### 1. RenderCache — 核心渲染缓存

**数据结构**
```
RenderCache
├── byMaterialization: map<materializationId, MaterializationEntry>
│     └── chunks: map<chunkId, ChunkData{PCM, revision, state, lastAccess}>
├── lruList: intrusive list（访问时移到头部）
└── stats: ChunkStats per materialization
```

**Chunk 状态机**：`Clean` → `Dirty` → `Rendering` → `Ready` → `Clean`；`desiredRevision > renderedRevision` 时标脏。

**驱逐规则**：
1. 写入新 chunk 前检查是否超 256 MB；
2. 从 LRU 尾部选 materialization 条目整体淘汰；
3. Rendering 中的 chunk 不参与淘汰（写保护）。

**失效传播**：
- `commitMaterializationNotesAndSegmentsById` → `renderRevision++` → 所有 chunk 标 Dirty → `enqueueMaterializationPartialRenderById`；
- `replaceMaterializationAudioById` → 整体 invalidate。

### 2. Materialization / Source Store — 内存驻留

这两个 store 持久化到工程文件（Standalone）或由 ARA host 管理（VST3/ARA）。内存中保留完整 PCM，无 LRU，依赖 retire/revive + ReclaimSweep 垃圾回收。

**ReclaimSweep 逻辑**：
- 在 Message Thread 定期运行；
- 扫描所有 retired entries；
- 若无任何 Placement / Snapshot / UndoAction 引用 → 真删除。

### 3. COW Snapshot 缓存

`PitchCurve` / `MaterializationSnapshot` / `PlaybackSnapshot` / `PublishedSnapshot`（ARA）均采用 `shared_ptr<const Snapshot>` + `std::atomic_load/store`。

**读者**：lock-free `std::atomic_load(&slot)` 得到不可变视图。
**写者**：clone 旧 snapshot → 修改 → `std::atomic_compare_exchange_strong` 发布。
**析构**：最后一个 shared_ptr 析构时释放（RAII）。

### 4. thread_local Scratch

RMVPE 与 OnnxVocoderBase 为每个工作线程保留一份 scratch：
- 输入/输出张量缓冲；
- Mel filterbank 中间结果；
- 输入名/shape 探测结果（首次推理时填充）。

**好处**：避免每帧 inference 分配；线程切换开销可忽略。
**风险**：Worker 数量膨胀时内存占用线性增长（每线程几十 MB）。当前通过 `CpuBudgetManager::onnxInter` 限制 session 内部线程池大小。

## 缓存失效触发矩阵

| 触发源 | 影响的缓存 |
|---|---|
| 用户编辑音符 / LineAnchor | PitchCurve Snapshot，MaterializationSnapshot，RenderCache（chunks dirty），PlaybackSnapshot，WaveformMipmap（if PCM 变） |
| 导入新音频 | 创建 Source + Materialization，新建 RenderCache 条目，注册 WaveformMipmap |
| 拆分 / 合并 / 删除 Placement | MaterializationStore（retire/revive），PlaybackSnapshot 重发 |
| 切换推理后端（CPU/GPU） | ONNX Session 重建，thread_local scratch 重建 |
| 切换主题 | UIColors 全局重写，LookAndFeel 替换 |
| 宿主修改 ARA region | PublishedSnapshot 失效 → hydration worker 重拉 PCM |

## ⚠️ 待确认

- Mel filterbank 常量表是否全局 singleton 还是每个 Processor 实例一份（`MelSpectrogramProcessor` 当前按实例持有）；
- WaveformMipmap 在 ARA 模式下的生命周期（PreferredRegion 切换时是否释放）未验证；
- RenderCache 在同一 materialization 被多个 Placement 引用时的复用语义：**单条目共享**（同 materializationId 同 chunk 通用），CrossoverMixer 按 placement 区分。
