---
spec_version: 1.0.0
status: draft
module: inference
doc_type: overview
generated_by: module-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# Inference 模块概览

## 定位

`Source/Inference/` 是 OpenTune 的 **AI 模型推理层**，在 ONNX Runtime 之上封装两类模型能力：

1. **F0 提取（RMVPE）** — 从原始 PCM 音频中估计基频曲线，用于后续音高分析、编辑与合成。
2. **声码器（PC-NSF-HiFiGAN）** — 接收 F0 + Mel-Spectrogram，合成 44.1 kHz 人声音频。

模块同时实现多后端适配（CPU / macOS CoreML / Windows DirectML）、基于 chunk 的渲染调度器、带版本协议与 LRU 驱逐的渲染缓存，是连接用户音高编辑（上游 PitchCurve/DSP）与音频输出（下游 PluginProcessor/UI）的核心推理管线。

## 模块边界

```
上游依赖：
├── DSP/ResamplingManager      — r8brain 重采样（F0 目标 16 kHz，声码器 44.1 kHz）
├── DSP/MelSpectrogram         — mel 频谱计算（上层准备后传入 synthesize）
├── DSP/CrossoverMixer         — LR4 分频混音（RenderCache 持有，播放侧使用）
├── Utils/AccelerationDetector — GPU/后端选择（DML 适配器、回退策略）
├── Utils/CpuBudgetManager     — ONNX 会话的线程预算（intra/inter）
├── Utils/Error                — 结构化 Result<T> / ErrorCode / Error
├── Utils/AppLogger            — 结构化日志
├── Utils/TimeCoordinate       — 渲染采样率、秒 ↔ 采样的唯一来源
└── onnxruntime_cxx_api        — ONNX Runtime C++ API（含 DmlProviderFactory）

下游消费者：
├── Services/F0ExtractionService   — 异步 F0 提取任务（包装 F0InferenceService）
├── PluginProcessor                 — chunk 渲染触发 + 非阻塞音频叠加（RenderCache）
├── UI/PianoRollComponent           — 渲染进度显示（RenderCache::getChunkStats）
└── Domain/VocoderJobDispatcher(?)  — ⚠️ 待确认：VocoderDomain::Job 的实际投递方
```

## 文件清单（25 个文件）

| 文件 | 行数 | 职责 |
|------|------|------|
| `IF0Extractor.h` | 103 | F0 提取抽象接口 + `F0ModelType` / `F0ModelInfo` |
| `RMVPEExtractor.h` | 132 | RMVPE 提取器声明（含 `PreflightResult` 与内存常量） |
| `RMVPEExtractor.cpp` | 406 | RMVPE 实现：预检、重采样、高通滤波+噪声门、单次推理、倍频修正、空隙填补 |
| `VocoderInterface.h` | 23 | 声码器抽象接口（`synthesize` + 参数常量） |
| `OnnxVocoderBase.h` | 63 | **v1.3 新增**：Onnx 声码器共享基类（`VocoderScratchBuffers` / I/O 探测） |
| `OnnxVocoderBase.cpp` | 227 | 基类实现：输入名自动识别、形状解析、uv 导出、mel 转置 |
| `PCNSFHifiGANVocoder.h` | 18 | CPU/CoreML 声码器声明（继承 `OnnxVocoderBase`） |
| `PCNSFHifiGANVocoder.cpp` | 78 | CPU/CoreML 声码器 `runSession` 实现 |
| `DmlVocoder.h` | 37 | DirectML 声码器声明（仅 Windows，继承 `OnnxVocoderBase`） |
| `DmlVocoder.cpp` | 205 | DirectML 实现：DML EP 注入、IoBinding、输出预分配、结构化诊断 |
| `DmlConfig.h` | 9 | `DmlConfig` 结构（DXGI 适配器索引） |
| `ModelFactory.h` | 40 | F0 模型工厂声明 + `createF0SessionOptions` |
| `ModelFactory.cpp` | 217 | F0 工厂实现（CoreML/CPU 选择 + CpuBudgetManager 集成） |
| `VocoderFactory.h` | 45 | 声码器工厂声明 + `VocoderBackend` 枚举 + `VocoderCreationResult` |
| `VocoderFactory.cpp` | 101 | 声码器工厂实现（DML → CoreML → CPU 回退） |
| `F0InferenceService.h` | 107 | F0 推理服务声明（Pimpl 接口） |
| `F0InferenceService.cpp` | 293 | F0 推理服务实现：`shared_mutex`、30 s 空闲模型卸载 |
| `VocoderDomain.h` | 41 | 声码器领域聚合声明（组合 Service + Scheduler） |
| `VocoderDomain.cpp` | 62 | 领域聚合实现（生命周期绑定与 Job 投递） |
| `VocoderInferenceService.h` | 39 | 声码器推理服务声明（Pimpl） |
| `VocoderInferenceService.cpp` | 109 | 推理服务实现，供 Scheduler 的单线程 worker 调用 |
| `VocoderRenderScheduler.h` | 74 | 渲染调度器声明（串行 FIFO + 同 key 替换） |
| `VocoderRenderScheduler.cpp` | 144 | 调度器实现（worker 线程、队列上限 50、supersede 回调） |
| `RenderCache.h` | 127 | 渲染缓存声明（chunk 状态机 + 版本协议） |
| `RenderCache.cpp` | 442 | 缓存实现：`SpinLock`、overlay 线性插值、全局字节计数、LRU 驱逐 |

**总计**：25 个文件，约 3142 行。

## 架构分层

```
┌──────────────────────────────────────────────────────────────┐
│                      Domain Layer                             │
│   VocoderDomain              ← 聚合 Service+Scheduler 生命周期│
│     ├── VocoderInferenceService  (Pimpl)                      │
│     └── VocoderRenderScheduler   (串行 worker + FIFO)         │
│   F0InferenceService         ← Pimpl + shared_mutex + 空闲卸载 │
├──────────────────────────────────────────────────────────────┤
│                      Factory Layer                            │
│   ModelFactory     ← F0 session 构建 + CoreML/CPU 选择        │
│   VocoderFactory   ← 声码器 session 构建 + DML/CoreML/CPU 回退│
├──────────────────────────────────────────────────────────────┤
│                     Interface Layer                           │
│   IF0Extractor         ← 纯虚接口                             │
│   VocoderInterface     ← 纯虚接口                             │
│   OnnxVocoderBase      ← Onnx 共享抽象基类（v1.3 提取）       │
├──────────────────────────────────────────────────────────────┤
│                  Implementation Layer                         │
│   RMVPEExtractor         ← ONNX CPU/CoreML + 单次全量推理     │
│   PCNSFHifiGANVocoder    ← ONNX CPU/CoreML（继承 OnnxBase）   │
│   DmlVocoder             ← ONNX DirectML + IoBinding（Win）   │
├──────────────────────────────────────────────────────────────┤
│                       Cache Layer                             │
│   RenderCache  ← SpinLock + Chunk 状态机 + 全局 256 MB 上限   │
│                + 版本协议（desired/published）+ LRU 驱逐      │
└──────────────────────────────────────────────────────────────┘
```

## 设计模式

| 模式 | 应用 |
|------|------|
| **接口/实现分离** | `IF0Extractor` → `RMVPEExtractor`；`VocoderInterface` → `OnnxVocoderBase` → `PCNSFHifiGANVocoder` / `DmlVocoder` |
| **模板方法**（v1.3 新增） | `OnnxVocoderBase::synthesize` 调用子类 `runSession`，将 I/O tensor 准备与 session 执行解耦 |
| **静态工厂** | `ModelFactory::createF0Extractor()`；`VocoderFactory::create()` |
| **Pimpl** | `F0InferenceService` / `VocoderInferenceService`（隐藏 ONNX 依赖） |
| **领域聚合** | `VocoderDomain` 管理 Service + Scheduler 生命周期与投递链路 |
| **串行调度** | `VocoderRenderScheduler` 单 worker + `std::deque` 队列 + `condition_variable` |
| **结构化错误** | `Result<T>` + `ErrorCode` + `Error`（F0 路径），`VocoderCreationResult` 包含 backend 信息 |
| **Fail-fast 预检** | `RMVPEExtractor::preflightCheck()` ：时长闸门 + 内存预算 + 会话可用性 |
| **版本协议缓存** | `RenderCache::Chunk` 的 `desiredRevision` / `publishedRevision` 双版本号 |
| **非阻塞音频路径** | `overlayPublishedAudioForRate` 使用 `SpinLock::ScopedLockType`；跳过未发布 chunk |

## 关键约束

| 约束项 | 取值 | 来源 |
|--------|------|------|
| RMVPE 模型大小 | 361 MB | `ModelFactory::getAvailableF0Models` |
| RMVPE 目标采样率 | 16 kHz | `RMVPEExtractor::getTargetSampleRate` |
| RMVPE Hop size | 160 samples (10 ms) | `RMVPEExtractor::getHopSize` |
| F0 最大输入时长 | 600 s（10 min） | `RMVPEExtractor::kMaxAudioDurationSec` |
| F0 内存预留下限 | 512 MB | `kMinReservedMemoryMB` |
| F0 模型内存估算 | 350 MB | `kModelMemoryMB` |
| F0 模型内存开销系数 | 6.0 × input bytes | `kMemoryOverheadFactor` |
| F0 置信度阈值（RMVPE UV） | 默认 0.5 | `confidenceThreshold_` |
| F0 范围 | 50 – 1100 Hz | `f0Min_` / `f0Max_` |
| F0 空隙填补上限 | 8 帧（≈80 ms） | `kMaxGapFramesDefault` |
| F0 高通滤波 | 50 Hz, 48 dB/oct（8 阶 Butterworth） | `RMVPEExtractor::extractF0` |
| F0 噪声门阈值 | −50 dBFS | `kNoiseGateThreshold` |
| F0 空闲模型卸载 | 30 秒未调用 | `kModelRetentionMs` |
| 声码器 Hop size | 512 samples | `VocoderInterface::getHopSize` |
| 声码器采样率 | 44100 Hz | `VocoderInterface::getSampleRate` |
| 声码器 Mel bins | 128（默认，基类从模型反推） | `OnnxVocoderBase::melBinsHint_` |
| 渲染缓存全局上限 | 256 MB（默认） | `kDefaultGlobalCacheLimitBytes` |
| 调度器队列深度 | 50 | `VocoderRenderScheduler::kMaxQueueDepth` |
| 支持的后端 | CPU / CoreML（macOS）/ DirectML（Windows） | `VocoderBackend` |

## v1.3 版本变更（相对 v1.0 参考）

1. **新增 `OnnxVocoderBase.{h,cpp}`**：将原先分散在 `PCNSFHifiGANVocoder` 和 `DmlVocoder` 中的 I/O 名称识别、tensor 形状解析、uv 导出、mel 转置等共享逻辑抽到基类，子类仅需实现 `runSession`。
2. **文件清单**：23 → 25 个文件（新增 2 个文件）。
3. **RenderCache 全局上限默认值**：由参考文档中的 1.5 GB 调整为代码实际值 256 MB（`kDefaultGlobalCacheLimitBytes`）。⚠️ 待确认：是否曾在更早版本设置为 1.5 GB，后期被下调。

## Spec 文件

| 文件 | 内容 |
|------|------|
| [api.md](./api.md) | 所有公共接口契约：IF0Extractor / VocoderInterface / OnnxVocoderBase / Factory / Service / Scheduler / RenderCache 方法签名、线程安全、参数表 |
| [data-model.md](./data-model.md) | 数据结构：`F0ModelInfo` / `PreflightResult` / `VocoderScratchBuffers` / `Chunk` / `PendingJob` / `ChunkStats`；ONNX 模型 I/O schema；Chunk 状态机 |
| [business.md](./business.md) | 业务规则：F0 提取流程、声码器推理流程、调度状态机、后端选择、LRU 驱逐；含 2 个 Mermaid 图 |
