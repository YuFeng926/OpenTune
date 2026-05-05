---
spec_version: 1.0.0
status: draft
module: dsp
doc_type: data-model
generated_by: arch-doc-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# DSP 模块 — 数据模型文档

本文档聚焦 DSP 模块的**内部数据结构**与**参数配置**：Mel 滤波器组、Chroma 12 维向量、LR4 分频滤波器状态、以及各配置结构体。

---

## 1. MelSpectrogramConfig（参数配置）

**文件**: `Source/DSP/MelSpectrogram.h:20`

Mel 对数频谱计算的全部参数。所有字段均为值类型。

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `sampleRate` | `int` | `44100` | 输入音频采样率 (Hz) |
| `nFft` | `int` | `2048` | FFT 窗口大小（**必须为 2 的幂**） |
| `winLength` | `int` | `2048` | 窗口函数长度（通常 = `nFft`） |
| `hopLength` | `int` | `512` | 帧跳跃长度 → 帧率 ≈ 44100/512 ≈ 86.13 fps |
| `nMels` | `int` | `128` | Mel 频率 bin 数量（内部上限 128） |
| `fMin` | `float` | `40.0f` | Mel 滤波器组最低频率 (Hz) |
| `fMax` | `float` | `16000.0f` | Mel 滤波器组最高频率 (Hz)；实际 clamp 到 `sampleRate/2` |
| `logEps` | `float` | `1.0e-5f` | `log(max(mel, eps))` 的 epsilon |

### hash()

FNV-1a 风格 hash，组合 `sampleRate`, `nFft`, `winLength`, `hopLength`, `nMels`, `(int)(fMin*1000)`, `(int)(fMax*1000)`。用于 `MelSpectrogramProcessor` 判断配置是否变化，避免重复初始化。

```cpp
size_t hash() const noexcept;
// 初始化 h = 14695981039346656037ULL (FNV offset)
// 每个字段：h ^= v; h *= 1099511628211ULL; (FNV prime)
```

### 派生关系

```
MelSpectrogramConfig ──configure()──▶ MelSpectrogramProcessor
                                       ├─ juce::dsp::FFT (order = log2(nFft))
                                       ├─ juce::dsp::WindowingFunction<float> (Hann, winLength, noScale=false)
                                       ├─ melFilterbank_ [nMels][nFft/2+1]
                                       ├─ paddedAudio_   [numSamples + nFft]
                                       ├─ fftBuffer_     [nFft * 2]
                                       └─ magnitudeBuffer_ [nFft/2 + 1]
```

---

## 2. MelResult

**文件**: `Source/DSP/MelSpectrogram.h:49`

```cpp
using MelResult = Result<std::vector<float>>;
```

- 成功时包含 `std::vector<float>`，长度 `= nMels × numFrames`。
- **内存布局**: **列优先**（column-major by mel bin）—— `output[mel * numFrames + frame]`，同一 mel bin 的所有帧连续存放。与 ONNX 张量 `[1, 128, frames]` 一致。
- 失败时包含 `Error { ErrorCode, message, context }`。

---

## 3. Mel 滤波器组（内部）

**存储**: `MelSpectrogramProcessor::melFilterbank_` — `std::vector<std::vector<float>>`

- 外层大小：`nMels`（默认 128）
- 内层大小：`nFft / 2 + 1`（默认 1025）
- 三角滤波器，中心频率在 Mel 刻度上均匀分布
- 每个滤波器施加 Slaney 归一化：`enorm = 2.0 / (hzRight - hzLeft)`

### Mel 刻度转换（HTK/Slaney 混合风格）

| 范围 | 公式 |
|------|------|
| hz < 1000 | `mel = hz / (200/3)` — 线性段 |
| hz ≥ 1000 | `mel = minLogMel + ln(hz/1000) / logStep` — 对数段 |

其中常量：

```cpp
fSp       = 200.0f / 3.0f;          // ≈ 66.667
minLogHz  = 1000.0f;
minLogMel = minLogHz / fSp;          // ≈ 15.0
logStep   = ln(6.4) / 27.0;          // ≈ 0.06875
```

### 滤波器构建过程

1. `melMin = hzToMel(fMin)`、`melMax = hzToMel(min(fMax, sampleRate/2))`。
2. 在 `[melMin, melMax]` 上生成 `nMels + 2` 个等距 Mel 点。
3. 每点映射回 Hz 并转换为 FFT bin 索引：`bin = floor((nFft+1) * hz / sampleRate)`，clamp 到 `[0, nFftBins-1]`。
4. 第 m 个滤波器使用三元组 `(binPoints[m], binPoints[m+1], binPoints[m+2])` 构造三角形响应：左上升、右下降。
5. 施加 Slaney 归一化：整条滤波器乘以 `enorm = 2.0 / (hzRight - hzLeft)`（`juce::FloatVectorOperations::multiply`）。

---

## 4. FFT 工作缓冲区（内部）

| 缓冲区 | 大小 | 用途 |
|--------|------|------|
| `paddedAudio_` | `numSamples + nFft` | 反射填充后的音频（两端各 `nFft/2`） |
| `fftBuffer_` | `nFft * 2` | JUCE FFT 实/虚交错缓冲；`performFrequencyOnlyForwardTransform` 复用为幅度输出 |
| `magnitudeBuffer_` | `nFft / 2 + 1` | 预分配，当前实现未直接使用（由 `fftBuffer_` 前半承载） |

**反射填充策略**: `reflectIndex(i - pad, numSamples)` — 信号两端 `nFft/2` 样本进行镜像反射，等效于 `numpy.pad(mode='reflect')`。

---

## 5. Chroma 12 维向量（内部）

**存储**: `ChromaKeyDetector::chroma_` — `std::array<float, 12>`

12 个 bin 对应 12 个 pitch class（`C=0, C#=1, ..., B=11`）。

### 填充公式

```
frequency → midiNote = 12 · log2(freq / 440) + 69
         → pitchClass = fmod(midiNote, 12)
         → normalizedPc = pitchClass < 0 ? pitchClass + 12 : pitchClass
lowerBin  = floor(normalizedPc) mod 12
upperBin  = (lowerBin + 1) mod 12
ratio     = normalizedPc - floor(normalizedPc)
energy    = magnitude²
chroma[lowerBin] += (1 - ratio) · energy
chroma[upperBin] += ratio       · energy
```

- **能量加权**：使用幅度平方（`magnitude²`），突出强频率成分。
- **线性插值**：频率偏移按比例分摊到相邻两个 pitch class，保留微分音信息。
- **参考音**：以 A4 = 440 Hz 为基准（`A = 9`）。

### 频率范围过滤

```cpp
kMinFreqHz = 50.0f
kMaxFreqHz = 5000.0f
```

对应 FFT bin 索引：

```
minBin = ceil(kMinFreqHz / freqResolution)
maxBin = min(numBins - 1, floor(kMaxFreqHz / freqResolution))
```

其中 `freqResolution = sampleRate / kFftSize`（44100/4096 ≈ 10.77 Hz）。

---

## 6. Key Profile 模板（内部常量）

**存储**: `static const std::array<float, 12>` 在 `ChromaKeyDetector` 内。

### Krumhansl-Schmuckler (1990) — C 大调 / C 小调参考

```
kKSMajorProfile = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 }
kKSMinorProfile = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 }
```

### Temperley (2001, *Music and Probability*) — C 大调 / C 小调参考

```
kTemperleyMajorProfile = { 5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0 }
kTemperleyMinorProfile = { 5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0 }
```

### 旋转公式

```cpp
result[i] = base[(i - semitones + 12) % 12]
```

对 12 个 root × 2 个 scale = 24 个候选，对每个候选分别在 K-S 与 Temperley 两个 profile 上取 Pearson r，平均后作为集成分数。

---

## 7. DetectedKey（检测结果，公开）

**文件**: `Source/DSP/ChromaKeyDetector.h:57`

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `root` | `Key` | `Key::C` | 主音（0-11 对应 C 到 B） |
| `scale` | `Scale` | `Scale::Major` | 调式（枚举详见 api.md） |
| `confidence` | `float` | `0.0f` | 置信度 `= (bestPearsonR + 1) / 2`，范围 [0,1] |

### 静态方法

- `keyToString(Key)` → `"C"`, `"C#"`, `"D"`, ..., `"B"`
- `scaleToString(Scale)` → 8 种名称（`"Major"`, `"Minor"`, `"Chromatic"`, `"Harmonic Minor"`, `"Dorian"`, `"Mixolydian"`, `"Pentatonic Major"`, `"Pentatonic Minor"`）

### 持久化

`DetectedKey` 被 `MaterializationStore` 持久化，序列化顺序：`int(root)` → `int(scale)` → `float(confidence)`（见 `PluginProcessor.cpp:695-710` 的 `writeDetectedKey` / `readDetectedKey`）。

---

## 8. LR4 分频滤波器系数（内部）

**存储**: `CrossoverMixer` 持有两个独立的 `juce::dsp::LinkwitzRileyFilter<float>` 实例：

| 成员 | 用途 |
|------|------|
| `dryFilter_` | 对原始信号做 LR4 分频，取 **高频** 部分 |
| `renderedFilter_` | 对 vocoder 输出做 LR4 分频，取 **低频** 部分 |

### 数学结构

4 阶 Linkwitz-Riley 由两个串联的 2 阶 Butterworth 组成：

```
H_LP(s) = [ω₀² / (s² + √2·ω₀·s + ω₀²)]²
H_HP(s) = [s²  / (s² + √2·ω₀·s + ω₀²)]²
```

其中 `ω₀ = 2π · 14000`（由 `setCutoffFrequency(14000.0f)` 设定）。

### 关键性质

| 性质 | 说明 |
|------|------|
| 斜率 | 24 dB/oct（4 阶） |
| 交叉相位 | LP/HP 在 `ω₀` 处相位同步，**和** 幅度平坦 |
| 幅度求和 | `|H_LP(jω) + H_HP(jω)| ≈ 1`（全频段） |
| 分频点衰减 | 各支路在 `ω₀` 处 −6 dB，合成后 0 dB |

### 状态变量

`LinkwitzRileyFilter<float>` 内部为每个 channel 保存 2×2 级联 biquad 状态（单精度浮点延迟线）。由 `prepare(ProcessSpec)` 分配，`reset()` 清零。状态大小正比于 `numChannels`。

### ProcessSpec 配置

```cpp
juce::dsp::ProcessSpec spec{};
spec.sampleRate = sampleRate;              // 由 prepareToPlay 传入
spec.maximumBlockSize = maxBlockSize;      // 由 prepareToPlay 传入
spec.numChannels = numChannels;            // 默认 2（stereo）
```

### 构造默认值

```cpp
CrossoverMixer() { prepare(44100.0, 512, 2); }
```

构造即 prepare，避免未配置状态。实际使用中 `PluginProcessor::prepareToPlay` 会以宿主实际采样率再次调用 `prepare`。

---

## 9. ResamplingManager 状态（内部）

`ResamplingManager` **无成员状态字段**——每次 `resample()` 调用都在栈上创建新的 `r8b::CDSPResampler24` 实例（oneshot 模式），处理完即销毁。这使得它天然"可重入"但无法复用滤波器历史。

### 输出长度计算

```cpp
durationSeconds = TimeCoordinate::samplesToSeconds(inputLength, inputSR);
outputLength64  = TimeCoordinate::secondsToSamples(durationSeconds, targetSR);
outputLength    = min(outputLength64, INT_MAX);
if (outputLength <= 0) outputLength = 1;   // 保底
```

---

## 10. 外部依赖类型

### Result\<T\> / Result\<void\>

**文件**: `Source/Utils/Error.h`

Rust 风格的 Result 类型，基于 `std::variant`。支持 `success() / failure() / operator bool()`。

### Error / ErrorCode

**文件**: `Source/Utils/Error.h`

```cpp
struct Error {
    ErrorCode code;
    std::string message;
    std::string context;
};
```

DSP 模块使用到的 `ErrorCode`：`MelFFTSizeInvalid`, `MelNotConfigured`, `InvalidAudioInput`, `InvalidAudioLength`, `InvalidParameter`。

### TimeCoordinate

**文件**: `Source/Utils/TimeCoordinate.h`

命名空间 `OpenTune::TimeCoordinate`，提供 `samplesToSeconds` / `secondsToSamples` 与常量 `kRenderSampleRate = 44100.0`。`ResamplingManager::resample()` 依赖此工具计算输出长度。

### juce::dsp::LinkwitzRileyFilter\<float\>

**来源**: JUCE DSP 模块。`setCutoffFrequency(Hz)` → `prepare(ProcessSpec)` → `reset()` → `processSample(channel, in, outLow, outHigh)`。默认类型为 lowpass+highpass 分频（JUCE API 自动同时输出两路）。

---

## 11. 数据结构尺寸汇总

| 结构/缓冲 | 大小 | 备注 |
|-----------|------|------|
| `MelSpectrogramConfig` | ~40 字节 | 值类型 |
| `MelSpectrogramProcessor::melFilterbank_` | 128 × 1025 × 4B = **512 KB** | 默认配置 |
| `MelSpectrogramProcessor::fftBuffer_` | 2048 × 2 × 4B = 16 KB | |
| `MelSpectrogramProcessor::paddedAudio_` | (numSamples + 2048) × 4B | 动态 |
| `ChromaKeyDetector::fftBuffer_` | 4096 × 2 × 4B = 32 KB | |
| `ChromaKeyDetector::chroma_` | 12 × 4B = 48 B | |
| `DetectedKey` | 12 B（含 padding） | `int + int + float` |
| `CrossoverMixer`（2 个 LR4） | 依赖 JUCE 实现（约 2 × 2channels × 2biquads × ~64B） | |
| Key profile 静态常量 | 4 × 12 × 4B = 192 B（`.rodata`） | |

---

## ⚠️ 待确认

1. **`magnitudeBuffer_` 实际使用** — 被预分配但代码中似乎未显式写入；是否应移除以简化内存？
2. **Slaney 归一化的必要性** — `enorm = 2 / (hzRight - hzLeft)` 会放大窄 Mel 滤波器的响应，NSF-HiFiGAN 训练时是否使用相同归一化？不匹配会导致 spectrum 整体量级偏差。
3. **Chroma `kMaxFreqHz = 5000`** — 对高音女声 / 器乐泛音可能丢失信息；是否应与 Mel 的 `fMax = 16000` 看齐？
4. **LR4 滤波器状态复用粒度** — 当前每条 placement 持有独立 `CrossoverMixer`（`RenderCache::getCrossoverMixer`）；拖拽/删除 placement 后状态是否会泄漏或跨 placement 污染？
5. **`DetectedKey` 持久化格式版本** — 当前用 `writeInt / writeFloat` 裸序列化，无版本号。Scale 枚举扩展若变动枚举值顺序将破坏兼容（现约定新值追加末尾）。
6. **`ResamplingManager` 无状态设计的性能代价** — 每次调用创建 `CDSPResampler24`（含系数计算）；chunk 渲染频繁调用时是否应引入按 `(srcSR, dstSR)` 缓存的复用池？
