---
module: dsp
type: overview
version: 1.0
updated: 2026-04-10
sources:
  - Source/DSP/MelSpectrogram.h
  - Source/DSP/MelSpectrogram.cpp
  - Source/DSP/ResamplingManager.h
  - Source/DSP/ResamplingManager.cpp
  - Source/DSP/ScaleInference.h
  - Source/DSP/ScaleInference.cpp
---

# DSP 模块 — 概览

## 模块简介

DSP 模块是 OpenTune 的**信号处理基础设施层**，为 AI 推理管线和音频渲染提供三项核心能力：Mel 对数频谱计算、高质量音频重采样、以及基于 Krumhansl-Schmuckler 算法的调式自动检测。

## 职责边界

| 职责 | 组件 | 输入 | 输出 |
|------|------|------|------|
| Mel 频谱 | `MelSpectrogramProcessor` | 44100 Hz 单声道 PCM | 128-mel × N 帧对数频谱 |
| 重采样 | `ResamplingManager` | 任意采样率音频/数组 | 目标采样率/长度数据 |
| 调式检测 | `ScaleInference` | F0 频率数组 | Key + Scale + Confidence |

## 核心类

### MelSpectrogramProcessor

JUCE FFT + Hann 窗口 + 三角 Mel 滤波器组 + SIMD 加速。配置通过 `MelSpectrogramConfig` 驱动（默认：2048 FFT, 512 hop, 128 mels, 40-16000 Hz）。支持 `thread_local` 复用（通过自由函数 `computeLogMelSpectrogram`）。配置变更通过 FNV-1a hash 检测，避免重复初始化。

### ResamplingManager

封装 r8brain `CDSPResampler24`（24-bit 精度）。提供语义化接口：`downsampleForInference`（44100→16000）、`upsampleForHost`（→设备采样率）、`resampleToTargetLength`（F0 帧率对齐）。每次调用创建新 resampler 实例（oneshot 模式），无状态残留。

### ScaleInference

Krumhansl-Schmuckler 调式检测。从 F0 频率构建 12-bin Pitch Class 直方图（线性插值），与 24 个预计算模板（12 大调 + 12 小调）做点积匹配。支持 confidence/energy 加权。投票确认机制（默认 3 秒）防止瞬态波动。

## 文件清单

| 文件 | 行数 | 描述 |
|------|------|------|
| `Source/DSP/MelSpectrogram.h` | 118 | Mel 频谱配置结构体 + 处理器类声明 |
| `Source/DSP/MelSpectrogram.cpp` | 270 | Mel 频谱计算实现（FFT、滤波器组、SIMD 加速） |
| `Source/DSP/ResamplingManager.h` | 95 | 重采样管理器接口 |
| `Source/DSP/ResamplingManager.cpp` | 118 | 重采样实现（r8brain CDSPResampler24） |
| `Source/DSP/ScaleInference.h` | 139 | 调式检测接口 + Key/Scale/DetectedKey 类型 |
| `Source/DSP/ScaleInference.cpp` | 190 | Krumhansl-Schmuckler 算法实现 |

**总计**: 6 个文件，~930 行代码。

## 依赖关系

```
外部依赖:
  juce::dsp::FFT                     ← MelSpectrogramProcessor
  juce::dsp::WindowingFunction       ← MelSpectrogramProcessor
  r8b::CDSPResampler24               ← ResamplingManager

内部依赖:
  Utils/SimdAccelerator              ← MelSpectrogramProcessor (dotProduct, vectorLog)
  Utils/Error (Result<T>)            ← MelSpectrogramProcessor
  Utils/TimeCoordinate               ← ResamplingManager
  Utils/AppLogger                    ← MelSpectrogramProcessor (调试日志)

被依赖（下游消费者）:
  Inference/RenderingManager          → 消费 Mel 频谱
  PluginProcessor (chunk render)      → 调用 Mel 计算 + 重采样
  F0ExtractionService                 → 调用 downsampleForInference
  PianoRollCorrectionWorker           → 消费 ScaleInference 结果
  NoteGenerator/ScaleSnapConfig       → 消费 DetectedKey
```

## 关键设计决策

1. **thread_local 复用**: `computeLogMelSpectrogram` 使用 `thread_local MelSpectrogramProcessor`，在同线程多次调用间复用 FFT 对象和滤波器组，避免反复分配。
2. **SIMD 自动分派**: Mel 滤波器点积和对数计算委托给 `SimdAccelerator`，运行时选择 AVX2/NEON/Accelerate 最优路径。
3. **oneshot 重采样**: 每次创建新 `CDSPResampler24` 实例，简化生命周期管理，代价是无法复用滤波器状态。
4. **投票防抖**: `ScaleInference` 的候选调式需稳定 3 秒才确认，适配音乐片段中的临时转调/变调。
5. **列优先输出布局**: Mel 输出 `[mel_bin][frame]` 与 ONNX 张量 `[1, 128, frames]` 内存布局一致，避免转置开销。

## 子文档索引

| 文档 | 内容 |
|------|------|
| [api.md](./api.md) | 公开接口契约、方法签名、错误码 |
| [data-model.md](./data-model.md) | 数据结构定义、内存布局、类型关系 |
| [business.md](./business.md) | 算法详解、处理流程、业务规则、线程模型 |
