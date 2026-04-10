---
module: dsp
type: data-model
version: 1.0
updated: 2026-04-10
sources:
  - Source/DSP/MelSpectrogram.h
  - Source/DSP/ResamplingManager.h
  - Source/DSP/ScaleInference.h
  - Source/Utils/Error.h
  - Source/Utils/TimeCoordinate.h
---

# DSP 模块 — 数据模型文档

## 1. MelSpectrogramConfig

**文件**: `Source/DSP/MelSpectrogram.h:20`

Mel 对数频谱计算的全部参数。

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `sampleRate` | `int` | `44100` | 输入音频采样率 (Hz) |
| `nFft` | `int` | `2048` | FFT 窗口大小（须为 2 的幂） |
| `winLength` | `int` | `2048` | 窗口函数长度（通常 = nFft） |
| `hopLength` | `int` | `512` | 帧跳跃长度，决定帧率 = sampleRate / hopLength ≈ 86.13 fps |
| `nMels` | `int` | `128` | Mel 频率 bin 数量 |
| `fMin` | `float` | `40.0f` | Mel 滤波器组最低频率 (Hz) |
| `fMax` | `float` | `16000.0f` | Mel 滤波器组最高频率 (Hz)，实际上限为 `sampleRate / 2` |
| `logEps` | `float` | `1.0e-5f` | 取对数前的 epsilon clamp，防止 log(0) |

### hash() 方法

FNV-1a 风格 hash，组合 `sampleRate`, `nFft`, `winLength`, `hopLength`, `nMels`, `fMin * 1000`, `fMax * 1000`。用于 `MelSpectrogramProcessor` 判断配置是否变化，避免重复初始化。

### 派生关系

```
MelSpectrogramConfig  ──(configure)──>  MelSpectrogramProcessor
                                         ├── juce::dsp::FFT (order = log2(nFft))
                                         ├── juce::dsp::WindowingFunction<float> (Hann, winLength)
                                         └── melFilterbank_ [nMels][nFft/2+1]
```

---

## 2. MelResult

**文件**: `Source/DSP/MelSpectrogram.h:49`

```cpp
using MelResult = Result<std::vector<float>>;
```

- 成功时包含 `std::vector<float>`，大小 = `nMels × numFrames`。
- **内存布局**: 列优先（column-major）—— `output[mel * numFrames + frame]`，即同一 mel bin 的所有帧连续存放。这与 ONNX 模型的 `[1, 128, frames]` 输入张量兼容。
- 失败时包含 `Error{code, message, context}`。

---

## 3. Mel 滤波器组（内部）

**存储**: `MelSpectrogramProcessor::melFilterbank_` — `std::vector<std::vector<float>>`

- 外层大小: `nMels` (128)
- 内层大小: `nFft / 2 + 1` (1025)
- 三角滤波器，中心频率在 Mel 刻度上均匀分布。
- 每个滤波器施加 Slaney 归一化: `enorm = 2.0 / (hzRight - hzLeft)`。

### Mel 刻度转换

| 范围 | 公式 |
|------|------|
| hz < 1000 | `mel = hz / (200/3)` — 线性段 |
| hz ≥ 1000 | `mel = minLogMel + ln(hz/1000) / logStep` — 对数段 |

其中 `logStep = ln(6.4) / 27.0 ≈ 0.06875`。

---

## 4. FFT 工作缓冲区（内部）

| 缓冲区 | 大小 | 用途 |
|--------|------|------|
| `paddedAudio_` | `numSamples + nFft` | 反射填充后的音频 |
| `fftBuffer_` | `nFft * 2` | JUCE FFT 实/虚交错缓冲 |
| `magnitudeBuffer_` | `nFft / 2 + 1` | 幅度谱（预分配但由 FFT performFrequencyOnlyForwardTransform 隐式写入 fftBuffer 前半） |

**反射填充策略**: `reflectIndex(i - pad, numSamples)` — 信号两端 `nFft/2` 样本进行镜像反射，等效于 `numpy.pad(mode='reflect')`。

---

## 5. DetectedKey

**文件**: `Source/DSP/ScaleInference.h:35`

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `root` | `Key` | `Key::C` | 主音（0-11 映射 C 到 B） |
| `scale` | `Scale` | `Scale::Major` | 调式 |
| `confidence` | `float` | `0.0f` | 置信度 = maxScore / totalScore，范围 [0, 1] |

### 静态方法

- `keyToString(Key)` → `"C"`, `"C#"`, `"D"`, ... , `"B"`
- `scaleToString(Scale)` → `"Major"` 或 `"Minor"`

---

## 6. Key（枚举）

**文件**: `Source/DSP/ScaleInference.h:19`

```cpp
enum class Key { C=0, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B };
```

12 个半音，C=0，以半音递增。`Cs` 代表 C#，`Ds` 代表 D#，以此类推。

---

## 7. Scale（枚举）

**文件**: `Source/DSP/ScaleInference.h:26`

```cpp
enum class Scale { Major, Minor, Chromatic };
```

- `Major` / `Minor` — 由 `ScaleInference::findBestMatch()` 产生。
- `Chromatic` — 不由检测算法产生，仅用于外部 `ScaleSnapConfig` 表示"不做音阶量化"。

---

## 8. Krumhansl-Schmuckler 调式模板（内部）

### 大调 Profile（Krumhansl, 1990）

| C | C# | D | D# | E | F | F# | G | G# | A | A# | B |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 6.35 | 2.23 | 3.48 | 2.33 | 4.38 | 4.09 | 2.52 | 5.19 | 2.39 | 3.66 | 2.29 | 2.88 |

### 小调 Profile（Krumhansl, 1990）

| C | C# | D | D# | E | F | F# | G | G# | A | A# | B |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 6.33 | 2.68 | 3.52 | 5.38 | 2.60 | 3.53 | 2.54 | 4.75 | 3.98 | 2.69 | 3.34 | 3.17 |

**存储**: `ScaleInference::templates_[24]` — 索引 0-11 为 C 大调至 B 大调（通过旋转大调 profile），索引 12-23 为 C 小调至 B 小调。

旋转公式: `result[i] = base[(i - semitones + 12) % 12]`

---

## 9. 音高直方图（内部）

**存储**: `ScaleInference::histogram_` — `std::array<float, 12>`

12 个 bin 对应 12 个音名（Pitch Class）。F0 频率 → cents → 归一化到 [0, 1200) cents → 线性插值分配到相邻两个 bin。

### 频率到 cents 转换

```
cents = 1200 * log2(frequency / 440) + 5700
```

5700 = 1200 × 4.75（A4 = 440 Hz 在第 4.75 个八度），使 C0 对应 ~0 cents。

### Bin 分配（线性插值）

```
normalizedCents = (cents + 1200) mod 1200
lowerBin = floor(normalizedCents / 100) mod 12
ratio = fmod(normalizedCents, 100) / 100
histogram[lowerBin] += (1 - ratio) * weight
histogram[upperBin] += ratio * weight
```

---

## 10. 投票状态机（ScaleInference 内部）

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `candidateKey_` | `DetectedKey` | — | 当前候选调式 |
| `candidateHoldTime_` | `float` | `0.0f` | 候选已持续时间（秒） |
| `votingDuration_` | `float` | `3.0f` | 确认所需持续时间（秒） |
| `confirmedKey_` | `DetectedKey` | — | 已确认的稳定调式 |

**状态转移**: 每帧调用 `update(deltaTime)`：
- 若 `findBestMatch()` 结果与 `candidateKey_` 相同 → `candidateHoldTime_ += deltaTime`
- 若 `candidateHoldTime_ >= votingDuration_` → `confirmedKey_ = candidateKey_`
- 否则 → 切换候选，重置计时器

---

## 11. 外部依赖类型

### Result\<T\> / Result\<void\>

**文件**: `Source/Utils/Error.h:102`

Rust 风格的 Result 类型，基于 `std::variant<T, Error>`。支持 `map()`、`andThen()`、`valueOr()`。

### Error

**文件**: `Source/Utils/Error.h:79`

```cpp
struct Error {
    ErrorCode code;
    std::string message;
    std::string context;
};
```

### TimeCoordinate

**文件**: `Source/Utils/TimeCoordinate.h`

命名空间 `OpenTune::TimeCoordinate`，提供采样数/秒数转换工具。`kRenderSampleRate = 44100.0`。`ResamplingManager` 使用其 `samplesToSeconds` / `secondsToSamples` 计算输出长度。

### SimdAccelerator

**文件**: `Source/Utils/SimdAccelerator.h`

单例，运行时检测 SIMD 级别（AVX512/AVX2/SSE2/NEON/Accelerate），提供 `dotProduct()` 和 `vectorLog()` 等加速方法。`MelSpectrogramProcessor::compute()` 在 Mel 滤波器点积和对数计算中使用。

---

## ⚠️ 待确认

1. **Mel 输出布局** — 列优先 `[mel][frame]` 是否与所有下游消费者一致？ONNX vocoder 需要 `[1, 128, frames]` 张量。
2. **ScaleInference::processF0Data 的重置语义** — 每次调用重置直方图。若需要跨 clip 累积统计，调用者需自行管理。是否有此需求？
3. **r8brain CDSPResampler24 精度等级** — 当前固定使用 24-bit 精度。是否需要为不同场景（实时 vs 离线）提供可配置的精度等级？
