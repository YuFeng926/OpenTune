---
spec_version: 1.0.0
status: draft
doc_type: cross-cutting
topic: caching
generated_by: synthesis-agent
generated_at: 2026-04-10
last_updated: 2026-04-10
---

# 横切关注点 — 缓存策略

## 概述

OpenTune 在三个独立层级实施缓存策略：AI 渲染结果缓存（RenderCache）、DSP 计算缓存
（MelSpectrogramConfig hash）、UI 波形显示缓存（WaveformMipmap）。每层服务不同的
性能目标和线程安全约束。

## 1. RenderCache — AI 渲染结果缓存

**位置**: `Source/Inference/RenderCache.h/.cpp` (~613 行)

**目的**: 缓存 NSF-HiFiGAN 声码器合成的音频片段，避免重复推理。是连接渲染管线
（写入端）和音频线程（读取端）的核心数据桥梁。

### 数据结构

```
RenderCache
├── std::map<double, Chunk>    // 按 startSeconds 排序
│   └── Chunk
│       ├── audio (44100 Hz)   // 声码器原始输出
│       └── resampled: map<int, vector<float>>  // 按设备采样率缓存
├── desiredRevision            // 编辑时递增
├── publishedRevision          // 渲染完成时设置
└── juce::SpinLock lock_       // 全局保护
```

### 版本协议

- **desiredRevision**: 用户每次编辑音高时 bump，由 `enqueuePartialRender` 触发
- **publishedRevision**: 渲染 Worker 完成推理后设置
- **有效性**: 仅当 `desired == published` 时，`readAtTimeForRate` 返回缓存数据
- 版本不匹配时，音频线程回退到 dry signal

### 线程安全

| 操作 | 线程 | 锁策略 |
|------|------|--------|
| `store()` | VocoderRenderScheduler Worker | `ScopedLock` (阻塞) |
| `readAtTimeForRate()` | Audio Thread | `ScopedTryLock` (非阻塞) |
| `bumpDesiredRevision()` | Message Thread | `ScopedLock` |
| `getChunkStats()` | UI Thread | `ScopedTryLock` |

**关键不变量**: Audio Thread 对 RenderCache 的访问**永不阻塞**。`ScopedTryLock`
失败时直接回退到 dry signal。

### 容量管理

- **全局上限**: 1.5 GB (`kDefaultGlobalCacheLimitBytes`)
- **驱逐策略**: LRU (最近最少使用)
- **统计跟踪**: `globalCacheCurrentBytes_` / `globalCachePeakBytes_` (static atomic)
- **每个 Clip** 独立的 `RenderCache` 实例

### 重采样缓存

每个 Chunk 维护按目标采样率索引的重采样副本 (`unordered_map<int, vector<float>>`)。
首次以某采样率读取时生成并缓存，后续直接返回。
`clearResampledCache()` 在设备采样率变化时清除。

## 2. MelSpectrogramConfig Hash — 配置变更检测

**位置**: `Source/DSP/MelSpectrogram.h`

**目的**: 避免 Mel 处理器在配置未变时重复初始化 FFT 和滤波器组。

### 机制

- `MelSpectrogramConfig` 提供 FNV-1a hash 方法
- `MelSpectrogramProcessor::configure()` 比较新旧配置 hash
- Hash 相同则跳过 `initFftAndWindow()` 和 `buildMelFilterBank()`
- **节约**: FFT 对象分配 + 128-mel 三角滤波器组计算

### thread_local 复用

```cpp
// 自由函数入口
Result<void> computeLogMelSpectrogram(audio, numSamples, numFrames, output, cfg);
// 内部使用 thread_local MelSpectrogramProcessor 实例
```

- 同一线程多次调用复用 FFT 对象和滤波器组
- 不同线程各自持有独立实例，无锁竞争
- 配置变更时通过 hash 检测触发重初始化

## 3. WaveformMipmap — 波形显示缓存

**位置**: `Source/Standalone/UI/WaveformMipmap.h/.cpp` (~305 行)

**目的**: 多级 LOD (Level of Detail) 波形数据缓存，支撑不同缩放级别的高效波形绘制。

### 架构

```
WaveformMipmap (per clip)
├── Level 0:    32 samples/peak   (最高精度)
├── Level 1:   128 samples/peak
├── Level 2:   512 samples/peak
├── Level 3:  2048 samples/peak
├── Level 4:  8192 samples/peak
└── Level 5: 32768 samples/peak   (最低精度)
```

### 设计特点

- **int8_t 压缩存储**: 峰值归一化到 [-128, 127]，内存效率高
- **增量构建**: `buildIncremental(timeBudgetMs)` 支持时间预算控制，避免导入大文件时 UI 卡顿
- **按需选级**: 根据当前 zoomLevel 选择最合适的 LOD 级别
- **Clip 粒度缓存**: `WaveformMipmapCache` 以 `clipId` 为 key，`unordered_map<uint64_t, unique_ptr<WaveformMipmap>>`

### 线程模型

- 构建: Message Thread (增量，受时间预算控制)
- 读取: Message Thread (paint 过程中)
- 无需跨线程同步（单线程访问）

## 4. PianoRollRenderer correctedF0Cache — 修正 F0 绘制缓存

**位置**: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h`

**目的**: 缓存修正后的 F0 曲线用于绘制，避免每帧 paint 重新从 Snapshot 计算。

### 机制

- `updateCorrectedF0Cache(snapshot)`: 从 PitchCurveSnapshot 提取修正 F0 数据
- `cachedSnapshot_`: 通过 `shared_ptr` 比较判断快照是否变更
- `clearCorrectedF0Cache()`: 快照变更或 Clip 切换时清除
- 仅在 Message Thread paint 路径中使用

## 缓存策略对比

| 缓存 | 容量限制 | 驱逐策略 | 线程安全 | 失效触发 |
|------|---------|---------|---------|---------|
| RenderCache | 1.5 GB 全局 | LRU | SpinLock + TryLock | 版本号 mismatch |
| MelConfig hash | N/A (配置级) | N/A | thread_local 隔离 | 配置 hash 变更 |
| WaveformMipmap | 无限制 (per clip) | 随 clip 删除 | 单线程 | clip 变更/删除 |
| correctedF0Cache | 无限制 | 快照变更时清除 | 单线程 | snapshot ptr 变更 |
