---
spec_version: 1.0.0
status: draft
doc_type: cross-cutting/error-handling
generated_by: synthesis-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# 错误处理（Error Handling）

OpenTune 作为桌面音频应用，**不采用** Java/Spring 风格的异常栈。本文档定义项目硬约束、结构化错误模型、音频线程降级策略与日志约定。

## 硬约束

1. **项目内部代码不抛 C++ 异常。** 所有业务函数返回 `Result<T>` 或 `void + log`。
2. **异常仅在外部 API 边界捕获。** ONNX Runtime 与 JUCE 的部分 API（文件 I/O、UI Event Dispatch）会抛；在调用点用 `try/catch(std::exception&)` 转成 `Error`。
3. **Audio Thread（`processBlock`）绝对不处理异常**，任何在该上下文运行的代码都必须 noexcept。
4. **优先静默降级而非崩溃。** 音频路径上任何不可用资源（模型未就绪、锁竞争、缓存缺失）都回退到"直通干信号"。

## 结构化错误类型

定义于 `Source/Utils/Error.h`（utils 模块）。核心三件套：

```cpp
enum class ErrorCode {
    // 通用
    Unknown, InvalidArgument, NotFound, AlreadyExists,
    OutOfRange, PreconditionFailed, ResourceUnavailable,
    // IO
    FileIoError, PermissionDenied,
    // 推理
    ModelNotLoaded, InferenceRuntimeError, UnsupportedBackend,
    // 版本协议
    VersionMismatch, Cancelled,
    // ...（详见 Utils/Error.h 完整枚举）
};

struct Error {
    ErrorCode code;
    juce::String message;
    juce::String context;   // 模块/函数上下文
};

template<typename T>
class Result {
    std::variant<T, Error> value_;
public:
    bool ok() const;
    const T& unwrap() const;            // 断言 ok()
    const Error& error() const;
    T unwrapOr(T fallback) const;
    // ...
};
```

**设计权衡**
- 相比 `std::expected<T, E>`：JUCE 要求 C++17，`std::expected` 需要 C++23；自定义 `Result<T>` 保持最小依赖。
- 相比异常：编译产物更小、音频线程 noexcept 可保证、Result 显式强制调用方处理失败路径。

## 错误传播约定

### 分层传播

```
[外部 API]  ONNX Runtime throws Ort::Exception
    │
[边界捕获]  try { session.Run(...); } catch (const std::exception& e) {
    │         return Error{ErrorCode::InferenceRuntimeError, e.what(), "VocoderInferenceService::synthesize"};
    │      }
    │
[业务层]    Result<PcmChunk> VocoderDomain::renderChunk(...)
    │
[调度层]    RenderCache 根据 Result 决定：
    │        ok → 写 cache，推进状态机；
    │        err(InferenceRuntimeError) → 标 Dirty，等下一轮重试；
    │        err(Cancelled) → 不写 cache，静默；
    │        err(VersionMismatch) → 丢弃（已有更新版本）。
    │
[音频线程]  readPlaybackAudio 若 cache miss → 回退干信号（不尝试内联渲染）。
```

### 常见错误码的处理语义

| ErrorCode | 典型来源 | 处理策略 |
|---|---|---|
| `ModelNotLoaded` | F0/Vocoder 模型路径未找到或未完成初始化 | UI 显示警告，功能旁路（AutoTune 按钮置灰），processBlock 回退干信号 |
| `InferenceRuntimeError` | ONNX Runtime 推理时抛 Ort::Exception（OOM / 设备挂起） | 首次：日志 + retry；连续失败：触发 `resetInferenceBackend(forceCpu=true)` 降级到 CPU |
| `UnsupportedBackend` | 用户选 DML 但无 GPU / CoreML 不可用 | `AccelerationDetector` 预检失败 → 静默选 CPU |
| `Cancelled` | `AsyncCorrectionRequest` / F0 提取任务被 token 取消 | 静默丢弃，不打日志 |
| `VersionMismatch` | Worker 结果相对 `renderRevision` 过期 | 静默丢弃，最新版本覆盖（pending slot） |
| `FileIoError` | 导入文件解码失败 / 工程保存失败 | 弹对话框提示用户 |
| `ResourceUnavailable` | `ScopedTryLock` 尝试失败 | Audio thread 回退干信号；UI 线程延帧重试 |
| `PreconditionFailed` | 不变式违反（SourceWindow 超出范围等） | `jassertfalse` + 日志；业务返回 err，不崩溃 |

## 音频线程降级策略

`OpenTuneAudioProcessor::processBlock` 内每个潜在失败点都有兜底：

| 失败场景 | 降级动作 |
|---|---|
| `ScopedTryLock` 获取 PlaybackSnapshot 失败 | 输出静音或直通 input buffer（Standalone：静音；VST3：passthrough） |
| `RenderCache::readPlaybackAudio` cache miss | 使用 MaterializationStore 中的原始已渲染 PCM（如果有）；否则静音 |
| `CrossoverMixer` 未就绪 | 跳过高频合成，只输出 vocoder 低频或干信号 |
| F0 模型未加载（`ensureF0Ready` 失败） | AutoTune 不触发，pitchCurve 保持旧值 |
| Vocoder 模型未加载（`ensureVocoderReady` 失败） | 直接使用原始 PCM（不进行音高修正合成） |
| 渲染预算耗尽（`CpuBudgetManager::allowSpinning = false`） | Scheduler 推迟 chunk 渲染，不在 audio thread 内做同步渲染 |

**关键不变式**：processBlock 永不返回错误码，永不阻塞，永不丢缓冲区大小一致性。

## 异常→错误转换边界

| 外部 API | 边界位置 | 转换示例 |
|---|---|---|
| ONNX Runtime `Ort::Session::Run` | `VocoderInferenceService::synthesize` / `F0InferenceService::extract` | `Ort::Exception` → `InferenceRuntimeError` |
| JUCE 文件 I/O | `AsyncAudioLoader::load` / `PresetManager::save` | `std::exception` → `FileIoError` |
| r8brain `CDSPResampler24` | `ResamplingManager::resample` | 抛 → `Error{InvalidArgument}` |
| JUCE AudioFormatReader | `AudioFormatRegistry` 条件注册 | 格式不支持 → null reader，不抛 |
| ARA SDK callbacks | `OpenTuneDocumentController::doXxx` | ARA 回调本身 noexcept；内部调用 processor 用 try/catch |
| Windows DLL delay-load | `OnnxRuntimeDelayLoadHook.cpp` | Hook 返回 `nullptr` → 降级提示 |

## 日志约定

由 `Utils::AppLogger` 管理，四级别：

| 级别 | 使用场景 |
|---|---|
| `trace` | 单次请求内的细粒度事件（chunk 状态转移、每帧 F0 提取） |
| `info` | 用户级事件（导入成功、模型加载完成、后端切换） |
| `warn` | 可恢复错误（初次推理失败、DML 适配器无 SW fallback） |
| `error` | 不可恢复错误（模型文件缺失、文件保存失败） |

**结构化约定**
- 每条日志带 `[模块名]` 前缀（`[inference]` / `[ara-vst3]` 等）；
- `context` 字段用 `类名::方法名` 或 `模块名/函数名`；
- 高频事件（audio thread 内）**不直接打日志**，而是累加 counter / 放 LockFreeQueue 给 UI 线程消费。

## v1.3 重要变更

- 移除了 `DmlRuntimeVerifier` 的 8 阶段预检；DML 初始化失败通过 `AccelerationDetector` 一次性判定 + ONNX Runtime 异常边界捕获。
- `LocalizationManager` 改用 `ScopedLanguageBinding` RAII，语言绑定失败不抛，返回原字符串。
- `UndoManager` 线性栈：push 失败（栈满）→ 日志 warn，不抛。

## ⚠️ 待确认

- `InferenceRuntimeError` 连续失败触发 `resetInferenceBackend(forceCpu)` 的阈值当前为硬编码（文档暂记 "连续 N 次"，N 值需核对源码）；
- `FileIoError` 在工程保存场景下是否触发"自动保存到临时文件"逻辑尚未统一验证。
