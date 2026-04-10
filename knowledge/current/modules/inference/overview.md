---
module: inference
type: overview
generated: 2026-04-10
source: 基于源码扫描生成
status: draft
---

# Inference 模块概览

## 定位

`Source/Inference/` 是 OpenTune 的 **AI 模型推理层**，封装了 ONNX Runtime 推理引擎，提供 F0 基频提取和 NSF-HiFiGAN 声码器合成能力。该模块是连接用户音高编辑（上游 `PitchCurve`）与音频输出（下游 `processBlock`）的核心推理管线。

## 模块边界

```
上游依赖：
├── DSP/ResamplingManager      — 音频重采样（r8brain）
├── DSP/MelSpectrogram         — mel 频谱计算（由上层准备后传入）
├── Utils/AccelerationDetector — GPU 后端检测
├── Utils/CpuBudgetManager     — CPU 线程预算
├── Utils/Error                — 结构化错误类型
├── Utils/AppLogger            — 日志
├── Utils/SimdAccelerator      — SIMD 加速（FFT 后的 magnitude 计算）
└── Utils/TimeCoordinate       — 时间 ↔ 采样转换

下游消费者：
├── Services/F0ExtractionService  — 异步 F0 提取（调用 F0InferenceService）
├── PluginProcessor               — 渲染调度 + 音频读取（调用 RenderCache）
└── UI/PianoRollComponent         — 渲染状态显示（调用 RenderCache::getChunkStats）
```

## 文件清单（23 个文件）

| 文件 | 行数 | 职责 |
|------|------|------|
| `IF0Extractor.h` | 103 | F0 提取抽象接口 |
| `RMVPEExtractor.h` | 139 | RMVPE 提取器声明 |
| `RMVPEExtractor.cpp` | 474 | RMVPE 提取器实现（预检查、推理、后处理） |
| `VocoderInterface.h` | 37 | 声码器抽象接口 |
| `PCNSFHifiGANVocoder.h` | 46 | CPU/CoreML 声码器声明 |
| `PCNSFHifiGANVocoder.cpp` | 401 | CPU/CoreML 声码器实现 |
| `DmlVocoder.h` | 74 | DirectML 声码器声明 |
| `DmlVocoder.cpp` | 523 | DirectML 声码器实现（仅 Windows） |
| `DmlConfig.h` | 11 | DirectML 配置结构 |
| `ModelFactory.h` | 40 | F0 模型工厂声明 |
| `ModelFactory.cpp` | 213 | F0 模型工厂实现（含 CoreML/CPU 选择） |
| `VocoderFactory.h` | 45 | 声码器工厂声明 |
| `VocoderFactory.cpp` | 102 | 声码器工厂实现（含 DML/CoreML/CPU 回退） |
| `F0InferenceService.h` | 100 | F0 推理服务声明 |
| `F0InferenceService.cpp` | 267 | F0 推理服务实现（Pimpl，shared_mutex） |
| `VocoderDomain.h` | 90 | 声码器领域聚合声明 |
| `VocoderDomain.cpp` | 117 | 声码器领域聚合实现 |
| `VocoderInferenceService.h` | 49 | 声码器推理服务声明 |
| `VocoderInferenceService.cpp` | 146 | 声码器推理服务实现（Pimpl） |
| `VocoderRenderScheduler.h` | 80 | 渲染调度器声明 |
| `VocoderRenderScheduler.cpp` | 122 | 渲染调度器实现（串行队列） |
| `RenderCache.h` | 133 | 渲染缓存声明 |
| `RenderCache.cpp` | 480 | 渲染缓存实现（调度状态机 + LRU 驱逐） |

## 架构分层

```
┌─────────────────────────────────────────────────────────┐
│                   Domain Layer                           │
│   VocoderDomain  ← 生命周期聚合                          │
│    ├── VocoderInferenceService  ← Pimpl                 │
│    └── VocoderRenderScheduler   ← 串行调度               │
│   F0InferenceService            ← Pimpl, shared_mutex   │
├─────────────────────────────────────────────────────────┤
│                   Factory Layer                          │
│   ModelFactory     ← 静态工厂, F0 提取器创建              │
│   VocoderFactory   ← 静态工厂, 声码器创建                │
├─────────────────────────────────────────────────────────┤
│                   Interface Layer                        │
│   IF0Extractor       ← 纯虚接口                         │
│   VocoderInterface   ← 纯虚接口                         │
├─────────────────────────────────────────────────────────┤
│                Implementation Layer                      │
│   RMVPEExtractor         ← ONNX CPU/CoreML              │
│   PCNSFHifiGANVocoder    ← ONNX CPU/CoreML              │
│   DmlVocoder             ← ONNX DirectML (Windows only) │
├─────────────────────────────────────────────────────────┤
│                   Cache Layer                            │
│   RenderCache  ← SpinLock, 状态机, LRU 驱逐, 1.5GB 限制 │
└─────────────────────────────────────────────────────────┘
```

## 设计模式

| 模式 | 应用 |
|------|------|
| **接口/实现分离** | `IF0Extractor` → `RMVPEExtractor`；`VocoderInterface` → `PCNSFHifiGANVocoder` / `DmlVocoder` |
| **静态工厂** | `ModelFactory::createF0Extractor()`；`VocoderFactory::create()` |
| **Pimpl** | `F0InferenceService`、`VocoderInferenceService`、`VocoderDomain` |
| **领域聚合** | `VocoderDomain` 封装 Service + Scheduler 生命周期 |
| **串行调度** | `VocoderRenderScheduler` 单线程 Worker + 任务队列 |
| **结构化错误** | `Result<T>` + `ErrorCode` + `Error` 替代异常传播 |
| **Fail-fast 预检** | `RMVPEExtractor::preflightCheck()` 三阶段资源验证 |
| **版本协议缓存** | `RenderCache` 的 `desiredRevision` / `publishedRevision` 双版本号 |
| **非阻塞音频读取** | `readAtTimeForRate(nonBlocking=true)` 使用 `ScopedTryLock` |

## 关键数字

| 指标 | 值 |
|------|----|
| RMVPE 模型大小 | ~361 MB |
| F0 帧率 | 100 fps (10ms) |
| 声码器 hop size | 512 samples (11.6ms @ 44100) |
| 渲染缓存全局上限 | 1.5 GB |
| F0 最大输入时长 | 10 分钟 |
| 内存预留下限 | 512 MB |
| 支持的后端 | CPU, CoreML (macOS), DirectML (Windows) |

## Spec 文件

| 文件 | 内容 |
|------|------|
| [api.md](./api.md) | 所有公共接口契约（12 个类/接口的方法签名、线程安全、参数表） |
| [data-model.md](./data-model.md) | 数据结构、继承关系（Mermaid）、Chunk 状态机、关键常量 |
| [business.md](./business.md) | 业务规则（RMVPE 规格、预检查、声码器规格、缓存协议）、核心流程图、线程安全策略 |
