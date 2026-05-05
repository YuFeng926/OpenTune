---
spec_version: 1.0.0
status: draft
module: dsp
doc_type: overview
generated_by: module-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# DSP 模块 — 概览

## 模块简介

DSP 模块是 OpenTune 的**数字信号处理基础设施层**，为 AI 推理管线与音频渲染后处理提供四项核心能力：

1. **Mel 对数频谱计算** — 将 44100 Hz PCM 转换为 NSF-HiFiGAN vocoder 所需的 128-mel 对数频谱。
2. **高质量音频重采样** — 基于 r8brain `CDSPResampler24`（24-bit 精度），在宿主采样率、推理采样率、F0 帧率之间无缝转换。
3. **Chroma 调性检测** — 从音频 PCM 直接提取 12 维 Chroma 向量，结合 Krumhansl-Schmuckler 与 Temperley 双 profile 模板匹配推断大/小调。
4. **LR4 分频混音** — 4 阶 Linkwitz-Riley 滤波器实现的 14 kHz 高低频分频混合器，将 vocoder 低频与原始音频高频平滑合成，保留原始高频细节。

## 职责边界

### 上游（调用方）

| 调用方 | 文件 | 使用组件 | 场景 |
|--------|------|----------|------|
| `OpenTuneAudioProcessor` | `Source/PluginProcessor.cpp` | `MelSpectrogramProcessor`, `ResamplingManager`, `CrossoverMixer`, `ChromaKeyDetector` | chunk 渲染、导出、读音频 |
| `F0InferenceService` / `F0ExtractionService` | `Source/Inference/` | `ResamplingManager` | 44100→16000 下采样喂给 RMVPE |
| `RMVPEExtractor` | `Source/Inference/RMVPEExtractor.cpp` | `MelSpectrogramProcessor` | 可选 Mel 特征 |
| `RenderCache` | `Source/Inference/RenderCache.{h,cpp}` | `CrossoverMixer` | 持有每条 placement 的分频混音状态 |
| `MaterializationStore` | `Source/MaterializationStore.cpp` | `ChromaKeyDetector::DetectedKey` | 存储素材检测到的调式 |
| `OpenTunePlaybackRenderer` | `Source/ARA/OpenTunePlaybackRenderer.cpp` | `CrossoverMixer` | ARA 回放时分频混音 |

### 下游（依赖）

- **r8brain-free-src** — `CDSPResampler24`（第三方高精度重采样库）。
- **JUCE DSP** — `juce::dsp::FFT`, `juce::dsp::WindowingFunction<float>`, `juce::dsp::LinkwitzRileyFilter<float>`, `juce::dsp::ProcessSpec`。
- **Utils/Error** — `Result<T>` / `Error` / `ErrorCode`，用于 Mel 路径返回值。
- **Utils/TimeCoordinate** — `samplesToSeconds` / `secondsToSamples` / `kRenderSampleRate = 44100.0`。
- **Utils/AppLogger** — 调试日志（Mel、Chroma 路径）。
- **Utils/SimdAccelerator**（如适用）— 点积/vectorLog 加速（当前 Mel 实现中使用 `juce::FloatVectorOperations` 与内联循环，SIMD 加速通过 JUCE 完成）。

## 文件清单

| 文件 | 行数 | 描述 |
|------|------|------|
| `Source/DSP/MelSpectrogram.h` | 116 | Mel 配置、`MelSpectrogramProcessor` 类、`computeLogMelSpectrogram` 自由函数、`MelResult` 别名 |
| `Source/DSP/MelSpectrogram.cpp` | 257 | hzToMel/melToHz 变换、反射填充、FFT + Hann 窗、Mel 滤波器组构建、三角滤波器 Slaney 归一化、逐帧点积 + log |
| `Source/DSP/ResamplingManager.h` | 65 | 重采样管理器，暴露 `downsampleForInference` / `upsampleForHost` 两个语义化接口 |
| `Source/DSP/ResamplingManager.cpp` | 70 | 封装 `r8b::CDSPResampler24` oneshot 调用，输出长度由 `TimeCoordinate` 计算 |
| `Source/DSP/ChromaKeyDetector.h` | 145 | `Key` / `Scale` / `DetectedKey` 基础类型 + `ChromaKeyDetector` 类 + Krumhansl-Schmuckler / Temperley profile 声明 |
| `Source/DSP/ChromaKeyDetector.cpp` | 250 | Chroma 提取（STFT → pitch class 折叠 + 线性插值）、24 种 key × scale 模板旋转、双 profile 集成 Pearson 相关 |
| `Source/DSP/CrossoverMixer.h` | 33 | LR4 分频混音器类声明，持有两个 `LinkwitzRileyFilter<float>` |
| `Source/DSP/CrossoverMixer.cpp` | 33 | 14 kHz cutoff 准备 + `processSample` 返回 `LPF(rendered) + HPF(dry)` |

**总计**：8 个文件，~970 行代码。

## 关键约束

1. **采样率契约**：Mel 默认 `sampleRate = 44100`，与 `TimeCoordinate::kRenderSampleRate` 对齐；Chroma 的采样率由调用者显式传入（当前上游固定 44100）。
2. **FFT 尺寸幂约束**：`MelSpectrogramConfig::nFft` 必须是 2 的幂；Chroma 固定为 `kFftSize = 4096` (`kFftOrder = 12`)。
3. **线程模型**：`MelSpectrogramProcessor` 与 `ChromaKeyDetector` 均**非线程安全，每线程独立实例**。`ResamplingManager` oneshot 无状态。`CrossoverMixer` 由调用方管理 per-placement 的状态（滤波器有历史样本依赖）。
4. **Mel 输出布局**：`[mel_bin][frame]` 列优先，与 ONNX 张量 `[1, 128, frames]` 兼容。
5. **LR4 magnitude-flat 性质**：`LPF(x) + HPF(x) ≈ x`（幅度平坦），保证 `rendered == dry` 时混音器输出与 dry 等价。
6. **Chroma 分析长度上限**：超过 5 分钟的音频截取首尾各 30 秒；避免整首分析耗时。
7. **枚举兼容性**：`Scale` 枚举值一经固定不得重排（`Major=0, Minor=1, Chromatic=2`），新值追加末尾，确保持久化兼容（`DetectedKey` 被 `MaterializationStore` 序列化）。

## 子文档索引

| 文档 | 内容 |
|------|------|
| [api.md](./api.md) | 类方法契约、配置结构、错误码 |
| [data-model.md](./data-model.md) | Mel 滤波器组、Chroma 向量、LR4 系数等内部数据结构 + 参数配置 |
| [business.md](./business.md) | Mel / 重采样 / Chroma 匹配 / LR4 分频的数学原理与流程图 |
