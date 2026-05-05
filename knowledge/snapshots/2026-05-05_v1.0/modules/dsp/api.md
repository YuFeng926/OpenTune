---
spec_version: 1.0.0
status: draft
module: dsp
doc_type: api
generated_by: arch-doc-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# DSP 模块 — API 接口文档

> 本模块无 HTTP Controller，此文档记录 DSP 层对外暴露的编程接口契约。

## 命名空间

所有类型与函数位于 `OpenTune` 命名空间。

---

## 1. MelSpectrogramProcessor

**文件**: `Source/DSP/MelSpectrogram.h:62`

Mel 对数频谱处理器。持有可复用的 FFT 对象、Hann 窗口、Mel 滤波器组与工作缓冲区。配置变更时自动重新初始化。**每线程独立实例**。

### 构造 / 析构

| 方法 | 签名 | 说明 |
|------|------|------|
| 构造 | `MelSpectrogramProcessor()` | 默认构造，未初始化状态 |
| 析构 | `~MelSpectrogramProcessor()` | 释放 FFT / Window / 滤波器组资源 |

拷贝/移动：未显式声明，依赖 `unique_ptr` 自动成员语义；实际调用路径不应拷贝实例。

### 配置

```cpp
Result<void> configure(const MelSpectrogramConfig& cfg);
```

- **作用**: 根据 `cfg` 初始化 FFT 对象、Hann 窗口与 Mel 滤波器组。
- **幂等性**: 若 `cfg.hash()` 未变则跳过初始化并返回成功。
- **错误**:
  - `ErrorCode::MelFFTSizeInvalid` — `nFft` 非 2 的幂。

### 计算

```cpp
// 写入外部缓冲区版本
Result<void> compute(const float* audio, int numSamples, int numFrames, float* output);

// 返回 vector 版本（内部分配 nMels * numFrames 大小）
MelResult compute(const float* audio, int numSamples, int numFrames);
```

- **参数**:
  - `audio` — 单声道 PCM 浮点音频（采样率须与配置中 `sampleRate` 一致）。
  - `numSamples` — 音频采样数。
  - `numFrames` — 期望输出帧数，约定为 `ceil(numSamples / hopLength)`。
  - `output` — 外部缓冲区，大小 `nMels * numFrames`，**列优先** `output[mel_bin * numFrames + frame]`。
- **返回**: `Result<void>` 或 `MelResult = Result<std::vector<float>>`。
- **前置条件**: `isInitialized() == true`。
- **错误码**: `MelNotConfigured`, `InvalidAudioInput`, `InvalidAudioLength`, `InvalidParameter`。
- **内部上限**: `nMels` 在单帧内最多处理 128 bins（`nMelsActual = std::min(config_.nMels, 128)`，栈缓冲区 `melSums[128]`）。

### 查询

```cpp
const MelSpectrogramConfig& getConfig() const noexcept;
bool isInitialized() const noexcept;
```

---

## 2. 自由函数 computeLogMelSpectrogram

**文件**: `Source/DSP/MelSpectrogram.h:111`

```cpp
MelResult computeLogMelSpectrogram(
    const float* audio, int numSamples, int numFrames,
    const MelSpectrogramConfig& cfg);
```

- **作用**: 便捷函数，内部使用 `thread_local MelSpectrogramProcessor` 实例。
- **线程安全**: 每线程独立实例，天然线程安全。
- **配置缓存**: 同线程同配置连续调用不重复初始化（通过 `hash()` 判断）。
- **错误码**: 同 `MelSpectrogramProcessor::compute` + 转发 `configure` 失败。

---

## 3. ResamplingManager

**文件**: `Source/DSP/ResamplingManager.h:22`

音频重采样管理器。封装 r8brain `CDSPResampler24`（24-bit 精度）。**oneshot 模式无内部状态**。

### 构造 / 析构

| 方法 | 签名 | 说明 |
|------|------|------|
| 构造 | `ResamplingManager()` | 默认构造 |
| 析构 | `~ResamplingManager()` | 默认 |

`JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR` 禁止拷贝。

### 下采样（用于推理）

```cpp
std::vector<float> downsampleForInference(
    const float* input, size_t inputLength,
    int inputSR, int targetSR);
```

- **典型用途**: 44100 → 16000（RMVPE 模型输入）。
- **等价于**: 内部调用 `resample(input, inputLength, inputSR, targetSR)`。

### 上采样（用于宿主）

```cpp
std::vector<float> upsampleForHost(
    const float* input, size_t inputLength,
    int inputSR, int targetSR);
```

- **典型用途**: 推理结果从模型采样率转回设备采样率。
- **等价于**: 同上，语义化命名，便于 IDE 阅读。

### 内部 resample

```cpp
private:
std::vector<float> resample(
    const float* input, size_t inputLength,
    int inputSR, int targetSR);
```

- **短路**: `inputSR == targetSR || inputLength == 0` 时直接拷贝。
- **输出长度**: `secondsToSamples(samplesToSeconds(inputLength, inputSR), targetSR)`，并 clamp 到 `INT_MAX`，最小为 1。
- **调用方式**: `r8b::CDSPResampler24(srcSR, dstSR, inputLength).oneshot(input, inputLength, output.data(), outputLength)`。

### 已移除接口

> **注意**：当前源码中不再提供 `resampleToTargetLength`、`clearCache`、`getLatencySamples` 等方法。旧版文档若涉及请忽略。

---

## 4. ChromaKeyDetector

**文件**: `Source/DSP/ChromaKeyDetector.h:70`

基于 Chroma / HPCP 特征与 Pearson 相关模板匹配的调式检测器。**天然支持单音与复音素材**。

### 构造 / 析构

| 方法 | 签名 | 说明 |
|------|------|------|
| 构造 | `ChromaKeyDetector()` | 初始化 FFT (order 12, size 4096) + Hann 窗 + 清零 `chroma_` |
| 析构 | `~ChromaKeyDetector()` | 默认 |

`JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR` 禁止拷贝。

### 检测主入口

```cpp
DetectedKey detect(const float* audio, int numSamples, int sampleRate);
```

- **作用**: 从音频 PCM 直接检测调式。
- **返回**: `DetectedKey { root, scale, confidence }`。
- **线程安全**: **非线程安全**，每线程应持有独立实例。
- **不抛异常**：输入无效时返回默认 `{ Key::C, Scale::Major, 0.0 }`。
- **长音频策略**: 超过 `kMaxAnalysisDurationSec = 300` 秒时递归截取首尾各 `kTruncateAnalysisSec = 30` 秒累加到同一 chroma 向量。

### 私有计算方法

```cpp
private:
void computeChroma(const float* audio, int numSamples, int sampleRate);
DetectedKey matchBestKey() const;
static float pearsonCorrelation(const std::array<float,12>& a,
                                const std::array<float,12>& b);
static void rotateProfile(const std::array<float,12>& base,
                          int semitones,
                          std::array<float,12>& result);
```

### DetectedKey 静态工具

```cpp
static juce::String DetectedKey::keyToString(Key key);   // "C","C#","D",...,"B"
static juce::String DetectedKey::scaleToString(Scale scale);
```

`scaleToString` 支持 8 种：`"Major"`, `"Minor"`, `"Chromatic"`, `"Harmonic Minor"`, `"Dorian"`, `"Mixolydian"`, `"Pentatonic Major"`, `"Pentatonic Minor"`。

### 硬编码参数（constexpr）

| 常量 | 值 | 含义 |
|------|----|------|
| `kFftOrder` | 12 | `2^12 = 4096` |
| `kFftSize` | 4096 | STFT 帧大小 |
| `kHopSize` | 2048 | STFT 帧跳跃（50% overlap） |
| `kMinFreqHz` | 50.0f | 最低有效频率 |
| `kMaxFreqHz` | 5000.0f | 最高有效频率 |
| `kMaxAnalysisDurationSec` | 300.0 | 触发分段分析的音频长度上限 |
| `kTruncateAnalysisSec` | 30.0 | 首尾各截取 30 秒 |

---

## 5. CrossoverMixer

**文件**: `Source/DSP/CrossoverMixer.h:16`

4 阶 Linkwitz-Riley（LR4, 24 dB/oct）分频混音器。将 dry（原始）与 rendered（vocoder）信号通过固定 14 kHz 交叉频率分频合成：

```
output = LPF(rendered) + HPF(dry)
```

**幅度平坦性**：`LPF(x) + HPF(x) ≈ x`，因此 `rendered == dry` 时输出近似等于 dry，无染色。

### 构造 / 生命周期

| 方法 | 签名 | 说明 |
|------|------|------|
| 构造 | `CrossoverMixer()` | 内部调用 `prepare(44100.0, 512, 2)` 默认就绪 |

### 准备

```cpp
void prepare(double sampleRate, int maxBlockSize, int numChannels = 2);
```

- **作用**: 构造 `juce::dsp::ProcessSpec` 并对内部两个 `LinkwitzRileyFilter` 设置 cutoff 为 14000 Hz、调用 `prepare + reset`。
- **调用时机**: 采样率或 block size 变化时重新调用（`PluginProcessor::prepareToPlay` 会通过 `MaterializationStore::prepareAllCrossoverMixers` 批量准备）。

### 样本级处理

```cpp
float processSample(int channel, float dry, float rendered);
```

- **作用**: 对指定 channel 同时对 `dry`、`rendered` 做 LR4 分频（调用 JUCE 的 `processSample(channel, input, outLow, outHigh)`），返回 `renderedLow + dryHigh`。
- **有状态**: 滤波器有内部延迟线，调用方需保证相同 placement 的样本顺序连续、避免跨段跳变。

### 硬编码参数

| 常量 | 值 | 含义 |
|------|----|------|
| `kCrossoverFrequencyHz` | `14000.0f` | LR4 分频截止频率 |

---

## 6. 枚举与数据结构（公开）

### Key（音名枚举）

**文件**: `Source/DSP/ChromaKeyDetector.h:33`

```cpp
enum class Key {
    C = 0, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B
};
```

12 个半音，C=0，以半音递增。`Cs` 代表 C#（避免 `#` 标识符非法）。

### Scale（调式枚举）

**文件**: `Source/DSP/ChromaKeyDetector.h:43`

```cpp
enum class Scale {
    Major = 0,
    Minor = 1,
    Chromatic = 2,
    HarmonicMinor = 3,
    Dorian = 4,
    Mixolydian = 5,
    PentatonicMajor = 6,
    PentatonicMinor = 7
};
```

- **兼容性约束**: 新增枚举值必须追加在末尾，保持 `Major=0, Minor=1, Chromatic=2` 不变（持久化数据向后兼容）。
- **实际产生**: `ChromaKeyDetector::matchBestKey()` 目前仅产生 `Major` 或 `Minor`（另 6 种保留给未来扩展与外部配置）。

### DetectedKey

**文件**: `Source/DSP/ChromaKeyDetector.h:57`

```cpp
struct DetectedKey {
    Key root = Key::C;
    Scale scale = Scale::Major;
    float confidence = 0.0f;

    static juce::String keyToString(Key key);
    static juce::String scaleToString(Scale scale);
};
```

### MelSpectrogramConfig

**文件**: `Source/DSP/MelSpectrogram.h:20`

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

    size_t hash() const noexcept;  // FNV-1a
};
```

### MelResult（类型别名）

```cpp
using MelResult = Result<std::vector<float>>;
```

---

## 7. 错误码（DSP 相关）

| ErrorCode | 使用位置 | 含义 |
|-----------|----------|------|
| `MelFFTSizeInvalid` | `configure` | FFT 大小非 2 的幂 |
| `MelNotConfigured` | `compute` | 未先调用 `configure()` |
| `InvalidAudioInput` | `compute` | 音频缓冲区为 null |
| `InvalidAudioLength` | `compute` | 采样数 <= 0 |
| `InvalidParameter` | `compute` | `numFrames <= 0` 或 `output == nullptr` |

ResamplingManager / ChromaKeyDetector / CrossoverMixer 使用**空/默认返回值**表达错误（无 `Result` 包装）。

---

## ⚠️ 待确认

1. **`MelSpectrogramProcessor` 无 `reset()` 方法** — 旧文档提到的 `reset()` 在当前头文件中不存在。是否应增加以显式释放 FFT 资源？
2. **`ResamplingManager` 缺失长度对齐接口** — 旧文档中的 `resampleToTargetLength` 与 `clearCache` 已被移除，F0 帧率对齐（100fps→~86fps）目前由谁完成？是否通过 `resample` 传入伪采样率实现？
3. **`CrossoverMixer` 无拷贝/析构声明** — 隐式可拷贝但 `LinkwitzRileyFilter` 内部状态需独占；是否应 `JUCE_DECLARE_NON_COPYABLE` 保护？
4. **`ChromaKeyDetector::detect` 的 `sampleRate` 参数** — 所有调用点当前传入 `44100`。是否考虑改为成员固定（与 Mel 一样通过 `configure`）？
5. **Scale 扩展枚举（HarmonicMinor/Dorian/...）** — 检测器当前不产生；是否有路线图补齐模板匹配？
6. **`CrossoverMixer::processSample` 逐样本调用开销** — 相比 JUCE 的 block 级 `process(ProcessContext)` 更慢，是否考虑为 chunk 渲染提供批量接口？
