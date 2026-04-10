---
spec_version: 1.0.0
status: draft
doc_type: cross-cutting
topic: error-handling
generated_by: synthesis-agent
generated_at: 2026-04-10
last_updated: 2026-04-10
---

# 横切关注点 — 错误处理

## 核心原则

**OpenTune 项目约定不主动抛出异常。** 异常仅在外部 API 边界（ONNX Runtime 调用）
以 `try/catch` 捕获。内部错误传播统一使用 `Result<T>` 类型和 `ErrorCode` 枚举。

## 错误处理分层

```
┌──────────────────────────────────────────────────┐
│  Layer 1: Rust-style Result<T> (主要传播机制)      │
│  所有模型/DSP/推理操作返回 Result<T>              │
├──────────────────────────────────────────────────┤
│  Layer 2: Bool 返回值 (简单成功/失败)             │
│  bool initialize(), bool configure()             │
├──────────────────────────────────────────────────┤
│  Layer 3: 结构化诊断 (PreflightResult)            │
│  success + errorMessage + errorCategory           │
├──────────────────────────────────────────────────┤
│  Layer 4: 异常捕获边界 (仅外部 API)               │
│  try { ort->Run() } catch (Ort::Exception&)      │
├──────────────────────────────────────────────────┤
│  Layer 5: 日志 + 安全回退                          │
│  AppLogger::error() + early return with defaults  │
└──────────────────────────────────────────────────┘
```

## 1. Result\<T\> — 主要错误传播机制

**位置**: `Source/Utils/Error.h` (187 行)

### 实现

```cpp
// 基于 std::variant 实现
template<typename T>
class Result {
    std::variant<T, Error> data_;
};

// void 特化使用 std::optional<Error>
template<>
class Result<void> {
    std::optional<Error> error_;
};
```

### API

| 方法 | 用途 |
|------|------|
| `Result::success(value)` | 构造成功结果 |
| `Result::failure(code, context)` | 构造失败结果 |
| `ok()` / `operator bool()` | 检查成功 |
| `value()` | 获取值（失败时抛 `runtime_error`） |
| `valueOr(default)` | 安全获取，失败返回默认值 |
| `error()` | 获取错误详情 |
| `map(f)` | 函数式 map 变换 |
| `andThen(f)` | 函数式 flatMap / bind |

### 使用分布

| 模块 | 使用 Result 的接口 |
|------|-------------------|
| inference | `F0InferenceService::extractF0()`, `VocoderInferenceService::synthesizeAudioWithEnergy()`, `ModelFactory::createF0Extractor()` |
| dsp | `MelSpectrogramProcessor::configure()`, `MelSpectrogramProcessor::compute()`, `computeLogMelSpectrogram()` |

## 2. ErrorCode 枚举 — 结构化错误分类

**位置**: `Source/Utils/Error.h:11`

| 分类 | 范围 | 错误码 |
|------|------|--------|
| 模型相关 | 100-104 | `ModelNotFound`, `ModelLoadFailed`, `ModelInferenceFailed`, `InvalidModelType`, `SessionCreationFailed` |
| 初始化 | 200-202 | `NotInitialized`, `AlreadyInitialized`, `InitializationFailed` |
| 音频输入 | 300-303 | `InvalidAudioInput`, `InvalidSampleRate`, `InvalidAudioLength`, `AudioTooShort` |
| F0/声码器 | 400-402 | `InvalidF0Input`, `F0ExtractionFailed`, `VocoderSynthesisFailed` |
| Mel 频谱 | 500-502 | `MelConfigInvalid`, `MelFFTSizeInvalid`, `MelNotConfigured` |
| 通用 | 600-602 | `InvalidParameter`, `OutOfMemory`, `OperationCancelled` |
| 兜底 | 999 | `UnknownError` |

每个 `ErrorCode` 附带标准化人类可读消息 (`errorCodeMessage`)。

## 3. PreflightResult — 预检查诊断

**位置**: `Source/Inference/RMVPEExtractor.h:34`

专用于 RMVPE 推理前的三阶段资源验证：

```cpp
struct PreflightResult {
    bool success;
    std::string errorMessage;
    std::string errorCategory;  // "duration" | "memory" | "model"
};
```

**验证阶段**:
1. **时长检查**: 音频 > 10 分钟则拒绝
2. **内存检查**: 估算 6x 内存开销，检查可用系统内存 (最低 512 MB 预留)
3. **模型文件检查**: ONNX 模型文件是否存在且可读

**设计理由**: 将"能否安全执行推理"的判断前置，避免运行中 OOM 导致的不可恢复状态。

## 4. 异常捕获边界

项目代码**不主动抛出异常**。异常仅在以下边界被捕获：

### ONNX Runtime 调用
```cpp
try {
    session->Run(runOptions, inputNames, inputValues, ...);
} catch (const Ort::Exception& e) {
    AppLogger::error("ONNX inference failed: " + juce::String(e.what()));
    return Result::failure(ErrorCode::ModelInferenceFailed, e.what());
} catch (const std::exception& e) {
    return Result::failure(ErrorCode::ModelInferenceFailed, e.what());
} catch (...) {
    return Result::failure(ErrorCode::ModelInferenceFailed, "Unknown error");
}
```

**捕获模式**:
- 先捕获具体异常类型 (`Ort::Exception`)
- 再捕获标准异常基类 (`std::exception`)
- 最后捕获所有 (`...`)
- 全部转换为 `Result::failure()` 返回

### 涉及模块
- `RMVPEExtractor::extractF0()` — RMVPE 模型推理
- `PCNSFHifiGANVocoder::synthesize()` — 声码器推理
- `DmlVocoder::synthesize()` — DirectML 声码器推理
- `ModelFactory::createF0Extractor()` — ONNX Session 创建
- `VocoderFactory::create()` — ONNX Session 创建

## 5. 日志系统

**位置**: `Source/Utils/AppLogger.h/.cpp` (~204 行)

### 分级日志

| 级别 | 方法 | 用途 |
|------|------|------|
| Debug | `AppLogger::debug()` | 开发调试信息 |
| Info | `AppLogger::info()` | 正常运行日志 |
| Warning | `AppLogger::warn()` | 非致命异常 |
| Error | `AppLogger::error()` | 错误（但不中断运行） |

### PerfTimer (RAII 计时)

```cpp
class PerfTimer {
    // 构造时记录起始时间
    // 析构时计算 elapsed 并调用 AppLogger::logPerf()
    // 注：当前实现中性能日志已禁用 (return at destructor start)
};
```

### 设计特点

- **全静态接口**: 无需依赖注入，任何位置可直接调用
- **文件记录**: 日志输出到用户应用数据目录
- **线程安全**: 通过 JUCE Logger 机制保证

## 6. 安全回退模式

当错误发生时，OpenTune 遵循 "降级不崩溃" 策略：

| 场景 | 回退行为 |
|------|---------|
| RenderCache 读取失败 (TryLock) | 回退到 dry signal 原始音频 |
| F0 提取失败 | Clip 显示原始波形，不显示音高曲线 |
| 声码器推理失败 | Chunk 标记为失败，播放 dry signal |
| ONNX 模型文件缺失 | 推理服务不初始化，编辑功能仍可用 |
| 内存不足 (Preflight) | 拒绝导入并提示用户 |
| GPU 后端不可用 | 自动回退到 CPU 推理 |

## 错误处理模式总结

| 模式 | 适用场景 | 示例 |
|------|---------|------|
| `Result<T>` | 模型/DSP 操作，需携带丰富错误信息 | `extractF0()`, `compute()` |
| `bool` 返回值 | 简单初始化/配置操作 | `initialize()`, `configure()` |
| `PreflightResult` | 资源密集操作前的可行性预检 | `preflightCheck()` |
| Early return | 无效输入参数 | `if (numSamples <= 0) return;` |
| `try/catch` → `Result` | ONNX Runtime 外部 API 调用 | `session->Run()` |
| `AppLogger::error()` + fallback | 运行时非致命错误 | Cache miss, lock contention |
