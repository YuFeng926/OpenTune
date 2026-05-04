# 渲染链路内存优化方案

**日期**: 2026-05-01
**问题**: 4分钟音频渲染时内存占用2.4GB，固有模型仅700MB，渲染缓存消耗近2GB
**目标**: 将渲染数据内存从~1.7GB降至<200MB

---

## 一、问题根因分析

### 2.4GB 构成拆解

| 类别 | 组件 | 大小 | 备注 |
|------|------|------|------|
| **固有** | ONNX 模型 (RMVPE + Vocoder) | ~300MB | 必须常驻 |
| **固有** | DirectML GPU 分配 + 运行时 | ~200MB | 必须常驻 |
| 🔴 **冗余** | drySignalBuffer × N个materialization | ~138MB × N | 与 audioBuffer 完全重复，仅SR不同 |
| 🔴 **冗余** | RenderCache resampledAudio × N | ~69MB × N | 与 chunk.audio 完全重复，仅SR不同 |
| 🟡 **膨胀** | 多个 materialization 未及时回收 | ~302MB × 每多一个 | retire 不释放，reclaim 异步延迟 |
| 🟡 **膨胀** | VocoderRenderScheduler 队列无界 | 可无限增长 | std::queue 无容量限制 |
| 🟡 **膨胀** | thread_local scratch 只增不减 | 每线程若干MB | recentPeakFrames_ 永不重置 |
| 🟡 **膨胀** | F0 提取时 makeCopyOf 全量复制 | ~63MB | lambda 持有期间 |

### 核心原则违反

**同一音频数据被存储2-3份（不同采样率）**，这是架构层面的根本错误。

业界做法：存储一份（44.1kHz），播放时对当前块（512 samples）实时重采样。512 samples 的重采样开销 < 1微秒，processBlock 预算 > 5毫秒。

---

## 二、修复方案（按优先级）

### P0-1：删除 drySignalBuffer

**节省**: ~138MB × materialization 数量

**改动**:
1. `MaterializationStore.h` — 从 `MaterializationEntry` 删除 `drySignalBuffer` 和 `drySignalSampleRate`
2. `MaterializationStore.cpp` — 删除 `buildDrySignalBuffer()` 及所有调用处
3. 播放路径 `readPlaybackAudio()` — 从 `audioBuffer`(44.1kHz) 实时重采样当前块到设备SR
   - 使用 `ResamplingManager` 或简单线性插值（CrossoverMixer 已在14kHz做LPF）
   - 每次只重采样 processBlock 大小的片段（512 samples），开销可忽略

**风险**: 无。audioBuffer 是 `shared_ptr<const>`，只读，无线程安全问题。

### P0-2：删除 RenderCache::resampledAudio

**节省**: ~69MB × materialization 数量

**改动**:
1. `RenderCache.h` — 从 `Chunk` 结构删除 `resampledAudio` map
2. `RenderCache.cpp` — 删除 `addResampledChunk()`、`rebuildPlaybackCache()`、`clearResampledCache()`
3. `overlayPublishedAudioForRate()` — 从 `chunk.audio`(44.1kHz) 实时重采样到目标SR
   - 当前实现已有线性插值逻辑，只需将数据源从 resampledAudio 改为 audio
4. worker 完成回调 — 删除 `addResampledChunk()` 调用

**风险**: 无。overlayPublishedAudioForRate 已经支持比率读取。

### P0-3：降低全局缓存上限

**改动**: `kDefaultGlobalCacheLimitBytes` 从 1536MB → 256MB

上限存在的意义是兜底，合理架构下4分钟音频渲染结果仅~42MB。256MB 足够多首歌同时缓存。

### P1-1：retired materialization 立即释放音频数据

**节省**: 每个未回收的 retired materialization 节省~300MB

**改动**:
1. `retireMaterialization()` — 立即释放 audioBuffer、renderCache（只保留元数据供 reclaim sweep 判断）
2. 或者改为同步 reclaim：retire 时如果引用计数允许，直接删除

### P1-2：VocoderRenderScheduler 队列加水位控制

**改动**:
1. 队列容量上限（如 16 个 Job）
2. 超限时新 submit 阻塞等待，或丢弃队列中版本过期的旧 Job
3. 每个 Job 的 f0/energy/mel 向量在 vocoder 消费后立即释放

### P1-3：thread_local scratch 重置机制

**改动**:
1. `DmlScratchBuffers::recentPeakFrames_` — 添加衰减逻辑或定期 shrink_to_fit
2. `PCNSFScratchBuffers` 同理
3. 或者改为预分配固定大小（基于最大 chunk 长度的合理上限）

### P2-1：F0 提取避免全量音频复制

**改动**:
1. `PluginProcessor.cpp:3169` 的 `makeCopyOf` — 改为持有 shared_ptr 引用（audioBuffer 本身是 const 不可变的）
2. 既然 audioBuffer 是 `shared_ptr<const>`，F0 worker lambda 直接捕获 shared_ptr 即可

---

## 三、预期效果

### 修改前（4分钟 @96kHz，6个 materialization）

| 组件 | 大小 |
|------|------|
| 模型 + GPU | 500MB |
| 6 × (audioBuffer + drySignalBuffer + renderCache×2) | 6 × 302 = 1812MB |
| 其他 | ~100MB |
| **合计** | **~2412MB** |

### 修改后

| 组件 | 大小 |
|------|------|
| 模型 + GPU | 500MB |
| 1 × audioBuffer (shared) | 63MB |
| 6 × renderCache (44.1kHz only) | 6 × 32 = 192MB |
| 其他 | ~50MB |
| **合计** | **~805MB** |

**节省 ~1.6GB (67%)**

---

## 四、实施顺序

1. P0-1 + P0-2 同步进行（删除双存储）
2. P0-3（降低缓存上限）
3. P1-1（retired 立即释放）
4. P1-2（队列水位控制）
5. P1-3 + P2-1（thread_local + F0 复制）

每步编译验证，确保播放链路正常。
