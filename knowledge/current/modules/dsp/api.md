---
module: dsp
type: api
version: 1.0
updated: 2026-04-10
sources:
  - Source/DSP/MelSpectrogram.h
  - Source/DSP/ResamplingManager.h
  - Source/DSP/ScaleInference.h
---

# DSP 模块 — API 接口文档

> 本模块无 HTTP Controller，此文档记录 DSP 层对外暴露的编程接口契约。

## 命名空间

所有类型和函数位于 `OpenTune` 命名空间。

---

## 1. MelSpectrogramProcessor

**文件**: `Source/DSP/MelSpectrogram.h:62`

Mel 对数频谱处理器。持有可复用的 FFT 对象、窗口函数、Mel 滤波器组和工作缓冲区。当配置变化时自动重新初始化。**每个线程应持有独立实例**。

### 构造 / 析构

| 方法 | 签名 | 说明 |
|------|------|------|
| 构造 | `MelSpectrogramProcessor()` | 默认构造，未初始化状态 |
| 析构 | `~MelSpectrogramProcessor()` | 释放 FFT/Window 资源 |

### 配置

```cpp
Result<void> configure(const MelSpectrogramConfig& cfg);
```

- **作用**: 根据 `cfg` 初始化 FFT 对象、Hann 窗口和 Mel 滤波器组。
- **幂等性**: 若 `cfg.hash()` 未变则直接返回成功。
- **错误**: `MelFFTSizeInvalid` — FFT 大小非 2 的幂；`OutOfMemory` — 分配失败。

### 计算

```cpp
// 写入外部缓冲区版本
Result<void> compute(const float* audio, int numSamples, int numFrames, float* output);

// 返回 vector 版本
MelResult compute(const float* audio, int numSamples, int numFrames);
```

- **参数**:
  - `audio` — 单声道 PCM 浮点音频（采样率须与配置中 `sampleRate` 一致）。
  - `numSamples` — 音频采样数。
  - `numFrames` — 期望输出帧数，公式：`ceil(numSamples / hopLength)`。
  - `output` — 外部缓冲区，大小 `nMels * numFrames`，列优先 `[mel_bin][frame]`。
- **返回**: `MelResult` = `Result<std::vector<float>>`，失败时携带 `ErrorCode`。
- **前置条件**: `isInitialized() == true`。
- **错误码**: `MelNotConfigured`, `InvalidAudioInput`, `InvalidAudioLength`, `InvalidParameter`。

### 查询

```cpp
const MelSpectrogramConfig& getConfig() const noexcept;
bool isInitialized() const noexcept;
```

### 重置

```cpp
void reset();
```

释放所有内部资源（FFT、窗口、滤波器组、缓冲区），恢复到未初始化状态。

---

## 2. 自由函数 computeLogMelSpectrogram

**文件**: `Source/DSP/MelSpectrogram.h:113`

```cpp
MelResult computeLogMelSpectrogram(
    const float* audio, int numSamples, int numFrames,
    const MelSpectrogramConfig& cfg);
```

- **作用**: 便捷函数，内部使用 `thread_local MelSpectrogramProcessor` 实例。
- **线程安全**: 每线程独立实例，天然线程安全。
- **配置缓存**: 同线程同配置连续调用不重复初始化（通过 hash 判断）。

---

## 3. ResamplingManager

**文件**: `Source/DSP/ResamplingManager.h:22`

音频重采样管理器。封装 r8brain `CDSPResampler24`（24-bit 精度重采样器）。

### 构造 / 析构

| 方法 | 签名 | 说明 |
|------|------|------|
| 构造 | `ResamplingManager()` | 默认构造 |
| 析构 | `~ResamplingManager()` | 调用 `clearCache()` |

### 下采样（用于推理）

```cpp
std::vector<float> downsampleForInference(
    const float* input, size_t inputLength,
    int inputSR, int targetSR);
```

- **典型用途**: 44100 Hz → 16000 Hz（RMVPE 模型输入）。
- **等价于**: 内部调用 `resample(input, inputLength, inputSR, targetSR)`。

### 上采样（用于宿主）

```cpp
std::vector<float> upsampleForHost(
    const float* input, size_t inputLength,
    int inputSR, int targetSR);
```

- **典型用途**: 推理结果从模型采样率转回设备采样率。
- **等价于**: 同上。

### 长度对齐重采样

```cpp
std::vector<float> resampleToTargetLength(
    const std::vector<float>& input, size_t targetLength);
```

- **用途**: F0 曲线对齐（帧率转换），如 100 fps → ~86 fps。
- **策略**: 将 `input.size()` 和 `targetLength` 作为伪采样率交给 r8brain。
- **短路**: 输入为空或 `targetLength == 0` 返回空；`input.size() == targetLength` 直接拷贝。

### 通用重采样

```cpp
std::vector<float> resample(
    const std::vector<float>& input,
    double srcSampleRate, double dstSampleRate);
```

- **短路**: 输入为空返回空；`|src - dst| < 0.01` 直接拷贝。
- **输出长度**: `TimeCoordinate::secondsToSamples(duration, dstSampleRate)`。

### 延迟查询

```cpp
int getLatencySamples(int inputSR, int targetSR) const;
```

- **当前返回**: `0`（r8brain oneshot 模式内部补偿延迟）。

### 缓存管理

```cpp
void clearCache();
```

当前为空实现，预留接口。

---

## 4. ScaleInference

**文件**: `Source/DSP/ScaleInference.h:49`

调式推断器。基于 Krumhansl-Schmuckler 调式检测算法，从 F0 数据推断音乐的调式（大调/小调）和主音。

### 构造 / 析构

| 方法 | 签名 | 说明 |
|------|------|------|
| 构造 | `ScaleInference()` | 初始化直方图为零，预计算 24 个调式模板 |
| 析构 | `~ScaleInference()` | 默认 |

### 批量处理

```cpp
void processF0Data(const std::vector<float>& f0Frequencies);

void processF0Data(const std::vector<float>& f0Frequencies,
                   const std::vector<float>& confidences,
                   const std::vector<float>& energies);
```

- **作用**: 从 F0 频率数组构建 12-bin 音高直方图（Pitch Class Distribution）。
- **频率范围**: 仅处理 50 Hz ~ 2000 Hz 的有效帧。
- **加权**: `confidences` 加权投票权重；`energies` 先归一化后取 `sqrt` 压缩动态范围。
- **注意**: 每次调用**重置**直方图后重新计算（非累加）。

### 流式处理

```cpp
void updateWithNewF0(float frequency);
```

- **作用**: 单帧累加到直方图（不重置），适用于实时流式场景。

### 检测结果

```cpp
DetectedKey findBestMatch() const;
DetectedKey getCurrentDetection() const;
```

- `findBestMatch()`: 遍历 24 个模板（12 大调 + 12 小调），返回最高点积分数的调式。置信度 = `maxScore / totalScore`。
- `getCurrentDetection()`: 返回经投票确认的稳定调式 `confirmedKey_`。

### 投票机制

```cpp
void setVotingDuration(float seconds);  // 默认 3.0 秒
void update(float deltaTime);
```

- **作用**: 候选调式需持续稳定 `votingDuration_` 秒才被确认，防止短暂波动导致频繁切换。

### 重置

```cpp
void reset();
```

清零直方图和候选保持时间。

---

## 5. 枚举与数据结构

### Key（音名枚举）

```cpp
enum class Key { C = 0, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B };
```

### Scale（调式枚举）

```cpp
enum class Scale { Major, Minor, Chromatic };
```

### DetectedKey（检测结果）

```cpp
struct DetectedKey {
    Key root = Key::C;
    Scale scale = Scale::Major;
    float confidence = 0.0f;

    static juce::String keyToString(Key key);
    static juce::String scaleToString(Scale scale);
};
```

### MelSpectrogramConfig（Mel 频谱配置）

```cpp
struct MelSpectrogramConfig {
    int sampleRate = 44100;
    int nFft = 2048;
    int winLength = 2048;
    int hopLength = 512;
    int nMels = 128;
    float fMin = 40.0f;
    float fMax = 16000.0f;
    float logEps = 1.0e-5f;

    size_t hash() const noexcept;  // FNV-1a hash
};
```

### MelResult（类型别名）

```cpp
using MelResult = Result<std::vector<float>>;
```

---

## 6. 错误码（DSP 相关）

| ErrorCode | 值 | 含义 |
|-----------|----|------|
| `MelConfigInvalid` | 500 | Mel 配置无效 |
| `MelFFTSizeInvalid` | 501 | FFT 大小非 2 的幂 |
| `MelNotConfigured` | 502 | Processor 未配置 |
| `InvalidAudioInput` | 300 | 音频缓冲区为 null |
| `InvalidAudioLength` | 302 | 采样数无效 |
| `InvalidParameter` | 600 | 通用参数错误 |
| `OutOfMemory` | 601 | 内存分配失败 |

---

## ⚠️ 待确认

1. **ResamplingManager::clearCache()** 当前为空实现 — 是否计划引入 resampler 对象缓存池以避免重复创建 `CDSPResampler24`？
2. **MelSpectrogramProcessor 的线程所有权** — 文档注释 "每个线程应持有独立实例"，但 `computeLogMelSpectrogram` 使用 `thread_local`，上层调用者是否总是通过自由函数还是也直接使用 `MelSpectrogramProcessor`？
3. **ScaleInference 的 Chromatic 枚举值** — `Scale::Chromatic` 在检测逻辑中从未产生，仅用于外部 `ScaleSnapConfig`？
