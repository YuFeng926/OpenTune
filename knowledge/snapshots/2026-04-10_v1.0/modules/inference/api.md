---
module: inference
type: api
generated: 2026-04-10
source: 基于源码扫描生成
status: draft
---

# Inference 模块 — API 接口契约

> 本模块无 HTTP Controller，此文档记录 AI 推理层对外暴露的编程接口契约。

---

## 1. IF0Extractor（抽象接口）

**文件**: `Source/Inference/IF0Extractor.h`
**命名空间**: `OpenTune`

F0（基频）提取的纯虚接口，所有 F0 提取实现必须遵守此契约。

### std::vector\<float\> IF0Extractor::extractF0()

**描述**: 从原始音频 PCM 数据中提取 F0 曲线（Hz 为单位，每帧一个值）。实现内部处理重采样。

**线程安全**: 由调用方保证（通过 `F0InferenceService` 的 `shared_mutex`）

| 参数 | 类型 | 说明 |
|------|------|------|
| `audio` | `const float*` | 输入音频 PCM 浮点采样 |
| `length` | `size_t` | 采样数 |
| `sampleRate` | `int` | 输入采样率（如 44100, 48000） |
| `progressCallback` | `std::function<void(float)>` | 可选，进度回调（0.0 ~ 1.0） |
| `partialCallback` | `std::function<void(const std::vector<float>&, int)>` | 可选，中间结果回调 |

**返回**: `std::vector<float>` — F0 曲线（Hz），错误时返回空向量

### int IF0Extractor::getHopSize()

**描述**: 获取目标采样率下的帧间跳跃采样数

**返回**: 整数（如 RMVPE 为 160 样本 @ 16kHz = 10ms 帧间隔）

### int IF0Extractor::getTargetSampleRate()

**描述**: 获取提取器的目标采样率

**返回**: 整数（通常 16000 Hz）

### F0ModelType IF0Extractor::getModelType()

**描述**: 获取模型类型标识符

### std::string IF0Extractor::getName()

**描述**: 获取人类可读的模型名称（如 "RMVPE"）

### size_t IF0Extractor::getModelSize()

**描述**: 获取模型估算大小（字节），用于 VRAM 管理和显示

### void IF0Extractor::setConfidenceThreshold(float threshold)

**描述**: 设置有声/无声检测的置信度阈值

| 参数 | 类型 | 说明 |
|------|------|------|
| `threshold` | `float` | 0.0 ~ 1.0，默认 0.03 ~ 0.05 |

### void IF0Extractor::setF0Min(float minHz) / setF0Max(float maxHz)

**描述**: 设置最小/最大 F0 频率范围

---

## 2. RMVPEExtractor（IF0Extractor 实现）

**文件**: `Source/Inference/RMVPEExtractor.h` / `.cpp`
**命名空间**: `OpenTune`
**继承**: `IF0Extractor`

RMVPE（Robust Multi-scale Vocal Pitch Estimator）ONNX 模型封装。

### RMVPEExtractor::RMVPEExtractor()

**描述**: 构造函数，接收已创建的 ONNX Session 和重采样管理器

| 参数 | 类型 | 说明 |
|------|------|------|
| `session` | `std::unique_ptr<Ort::Session>` | 已加载的 ONNX 会话 |
| `resampler` | `std::shared_ptr<ResamplingManager>` | 重采样管理器（共享所有权） |

### std::vector\<float\> RMVPEExtractor::extractF0()

**描述**: 完整 F0 提取流程：契约验证 → 预检查 → 重采样到 16kHz → 高通滤波 + 噪声门 → ONNX 推理 → 八度纠错 → 间隙填充

**线程安全**: 非线程安全（由外部 `shared_mutex` 保护）

**异常行为**:
- `std::invalid_argument`: audio 为 null、length 为 0、sampleRate <= 0
- `std::logic_error`: session 或 resampler 未初始化
- `std::runtime_error`: 预检查失败或推理失败

### PreflightResult RMVPEExtractor::preflightCheck()

**描述**: 推理前资源预检查：时长门限 → 内存预算 → 模型会话状态

**线程安全**: const 方法，安全

| 参数 | 类型 | 说明 |
|------|------|------|
| `audioLength` | `size_t` | 音频采样数 |
| `sampleRate` | `int` | 音频采样率 |

**返回**: `PreflightResult` — 结构化诊断结果，含 `success`、`errorMessage`、`errorCategory`（"MEMORY"/"DURATION"/"MODEL"/"SYSTEM"）、内存估算

### void RMVPEExtractor::setEnableUvCheck(bool enable)

**描述**: 启用/禁用 UV（无声）检查

**线程安全**: 非线程安全

---

## 3. VocoderInterface（抽象接口）

**文件**: `Source/Inference/VocoderInterface.h`
**命名空间**: `OpenTune`

声码器合成纯虚接口。

### std::vector\<float\> VocoderInterface::synthesize()

**描述**: 从 mel 频谱图 + F0 合成音频

**线程安全**: 由实现决定（PCNSFHifiGANVocoder 非线程安全，DmlVocoder 内部有 mutex）

| 参数 | 类型 | 说明 |
|------|------|------|
| `f0` | `const std::vector<float>&` | F0 曲线（Hz），每帧一个值 |
| `mel` | `const float*` | mel 频谱数据（melBins × frames 展平） |
| `melSize` | `size_t` | mel 数据总元素数 |

**返回**: `std::vector<float>` — 合成音频（44100 Hz，长度 = frames × 512）

### VocoderInfo VocoderInterface::getInfo()

**描述**: 获取声码器描述信息

### size_t VocoderInterface::estimateMemoryUsage(size_t frames)

**描述**: 估算给定帧数的内存占用

---

## 4. PCNSFHifiGANVocoder（VocoderInterface CPU 实现）

**文件**: `Source/Inference/PCNSFHifiGANVocoder.h` / `.cpp`
**命名空间**: `OpenTune`
**继承**: `VocoderInterface`

Pitch-Controllable NSF-HiFiGAN 声码器，CPU/CoreML 后端。

### PCNSFHifiGANVocoder::PCNSFHifiGANVocoder()

**描述**: 构造函数，自动探测 ONNX 模型输入/输出名称和 shape

| 参数 | 类型 | 说明 |
|------|------|------|
| `session` | `std::unique_ptr<Ort::Session>` | 已加载的 ONNX 会话 |

**自动探测**: 通过名称匹配 mel/f0/uv 输入索引，检测是否需要转置 mel 矩阵

### std::vector\<float\> PCNSFHifiGANVocoder::synthesize()

**描述**: 执行 ONNX 声码器推理。内部使用 `thread_local` 缓冲区避免重复分配。

**线程安全**: 非线程安全（`thread_local` scratch buffer）。需要外部序列化。

**异常**: session 未初始化、f0 为空、mel 大小不匹配、输出类型非 float、输出 shape 异常时抛出 `std::runtime_error`

---

## 5. DmlVocoder（VocoderInterface DirectML 实现）

**文件**: `Source/Inference/DmlVocoder.h` / `.cpp`
**命名空间**: `OpenTune`
**继承**: `VocoderInterface`
**平台**: 仅 Windows (`#ifdef _WIN32`)

Pitch-Controllable NSF-HiFiGAN 声码器，DirectML GPU 后端。

### DmlVocoder::DmlVocoder()

**描述**: 构造函数，初始化 DirectML 会话和 I/O 绑定

| 参数 | 类型 | 说明 |
|------|------|------|
| `modelPath` | `const std::string&` | ONNX 模型文件路径 |
| `env` | `Ort::Env&` | ONNX Runtime 环境 |
| `config` | `const DmlConfig&` | DirectML 配置（设备 ID、性能偏好、设备筛选器） |

**异常**: DML API 不可用或会话创建失败时抛出 `std::runtime_error`

### std::vector\<float\> DmlVocoder::synthesize()

**描述**: 通过 IOBinding 执行 DirectML 推理，使用预分配的输出 tensor

**线程安全**: 内部有 `std::mutex runMutex_` 保护，序列化调用

---

## 6. ModelFactory（F0 模型工厂）

**文件**: `Source/Inference/ModelFactory.h` / `.cpp`
**命名空间**: `OpenTune`
**设计模式**: 静态工厂

### F0ExtractorResult ModelFactory::createF0Extractor()

**描述**: 创建 F0 提取器实例（目前仅支持 RMVPE）。处理模型路径解析、ONNX 会话加载、GPU/CPU 后端选择。

**线程安全**: 静态方法，线程安全

| 参数 | 类型 | 说明 |
|------|------|------|
| `type` | `F0ModelType` | 模型类型（目前仅 `RMVPE`） |
| `modelDir` | `const std::string&` | 模型目录路径 |
| `env` | `Ort::Env&` | ONNX Runtime 环境 |
| `resampler` | `std::shared_ptr<ResamplingManager>` | 重采样管理器 |

**返回**: `Result<std::unique_ptr<IF0Extractor>>` — 成功返回提取器，失败返回结构化错误

### std::string ModelFactory::getModelPath()

**描述**: 解析模型文件路径（RMVPE → `{modelDir}/rmvpe.onnx`）

### bool ModelFactory::isModelAvailable()

**描述**: 检查模型文件是否存在

### std::vector\<F0ModelInfo\> ModelFactory::getAvailableF0Models()

**描述**: 扫描模型目录，返回可用 F0 模型列表

### Ort::SessionOptions ModelFactory::createF0SessionOptions(bool& outGpuMode)

**描述**: 创建 F0 ONNX 会话选项。macOS 尝试 CoreML EP；根据 `CpuBudgetManager` 配置线程数。

---

## 7. VocoderFactory（声码器工厂）

**文件**: `Source/Inference/VocoderFactory.h` / `.cpp`
**命名空间**: `OpenTune`
**设计模式**: 静态工厂（构造函数 `delete`）

### VocoderCreationResult VocoderFactory::create()

**描述**: 创建声码器实例。Windows 上优先尝试 DirectML，失败回退 CPU；macOS 尝试 CoreML EP。

**线程安全**: 静态方法

| 参数 | 类型 | 说明 |
|------|------|------|
| `modelPath` | `const std::string&` | ONNX 模型文件路径 |
| `env` | `Ort::Env&` | ONNX Runtime 环境 |

**返回**: `VocoderCreationResult` — 含 `vocoder`、`backend`（CPU/DML/CoreML）、`errorMessage`

**后端选择逻辑**:
1. Windows + `AccelerationDetector` 选择 DirectML → 尝试 `DmlVocoder`
2. 失败 → 回退到 `PCNSFHifiGANVocoder`（CPU）
3. macOS → 尝试 CoreML EP，失败回退 CPU

---

## 8. F0InferenceService（F0 推理服务）

**文件**: `Source/Inference/F0InferenceService.h` / `.cpp`
**命名空间**: `OpenTune`
**设计模式**: Pimpl

管理 F0 提取器生命周期，支持并发 F0 提取。

### bool F0InferenceService::initialize(const std::string& modelDir)

**描述**: 初始化 ONNX Runtime 环境，加载默认 RMVPE 模型

**线程安全**: 是（内部 `std::shared_mutex`）

### void F0InferenceService::shutdown()

**描述**: 释放资源，重置初始化状态

### Result\<std::vector\<float\>\> F0InferenceService::extractF0()

**描述**: 执行 F0 提取。获取共享读锁后委托给当前提取器。

**线程安全**: 是（`shared_lock` 允许并发读取/提取）

| 参数 | 类型 | 说明 |
|------|------|------|
| `audio` | `const float*` | 音频采样 |
| `length` | `size_t` | 采样数 |
| `sampleRate` | `int` | 采样率 |
| `progressCallback` | `std::function<void(float)>` | 可选进度回调 |
| `partialCallback` | `std::function<void(const std::vector<float>&, int)>` | 可选中间结果回调 |

**返回**: `Result<std::vector<float>>` — 成功返回 F0 曲线，失败返回结构化错误

### bool F0InferenceService::setF0Model(F0ModelType type)

**描述**: 热切换 F0 模型类型（获取写锁）

### 配置方法

| 方法 | 说明 |
|------|------|
| `setConfidenceThreshold(float)` | 设置置信度阈值（写锁） |
| `setF0Min(float)` / `setF0Max(float)` | 设置 F0 频率范围（写锁） |
| `getConfidenceThreshold()` / `getF0Min()` / `getF0Max()` | 获取当前参数（读锁） |
| `getF0HopSize()` / `getF0SampleRate()` | 获取帧参数（读锁） |
| `isInitialized()` | 检查初始化状态（`atomic` 读取） |

---

## 9. VocoderInferenceService（声码器推理服务）

**文件**: `Source/Inference/VocoderInferenceService.h` / `.cpp`
**命名空间**: `OpenTune`
**设计模式**: Pimpl

### bool VocoderInferenceService::initialize(const std::string& modelDir)

**描述**: 初始化 ONNX Runtime 环境，通过 `VocoderFactory` 创建声码器（模型文件 `{modelDir}/hifigan.onnx`）

### Result\<std::vector\<float\>\> VocoderInferenceService::synthesizeAudioWithEnergy()

**描述**: 执行声码器合成。外部获取 `std::mutex` 锁序列化调用。

**线程安全**: 是（`std::lock_guard<std::mutex>` 在外层方法）

| 参数 | 类型 | 说明 |
|------|------|------|
| `f0` | `const std::vector<float>&` | F0 曲线 |
| `energy` | `const std::vector<float>&` | 能量曲线（当前传入但未使用） |
| `mel` | `const float*` | mel 频谱数据 |
| `melSize` | `size_t` | mel 数据大小 |

**返回**: `Result<std::vector<float>>` — 合成音频或错误

### virtual Result\<std::vector\<float\>\> VocoderInferenceService::doSynthesizeAudioWithEnergy()

**描述**: 可覆写的内部合成方法（虚函数），用于测试或子类扩展

### int VocoderInferenceService::getVocoderHopSize() / getMelBins()

**描述**: 获取当前声码器的 hop size（512）和 mel bins（128）

---

## 10. VocoderRenderScheduler（声码器渲染调度器）

**文件**: `Source/Inference/VocoderRenderScheduler.h` / `.cpp`
**命名空间**: `OpenTune`

串行任务队列，保证声码器推理的单线程执行（DML 会话要求）。

### bool VocoderRenderScheduler::initialize(VocoderInferenceService* service)

**描述**: 初始化调度器，创建工作线程

**线程安全**: 是

| 参数 | 类型 | 说明 |
|------|------|------|
| `service` | `VocoderInferenceService*` | 声码器服务（非拥有指针，必须比调度器存活更久） |

### void VocoderRenderScheduler::shutdown()

**描述**: 停止接受任务，join 工作线程，排空队列（对未完成任务调用 `onComplete(false, ...)`）

### void VocoderRenderScheduler::submit(Job job)

**描述**: 提交渲染任务到队列

**线程安全**: 是（`std::mutex` 保护队列）

### int VocoderRenderScheduler::getQueueDepth()

**描述**: 获取当前队列深度

### bool VocoderRenderScheduler::isRunning()

**描述**: 检查调度器是否正在接受任务

---

## 11. VocoderDomain（声码器领域聚合）

**文件**: `Source/Inference/VocoderDomain.h` / `.cpp`
**命名空间**: `OpenTune`
**设计模式**: Pimpl + 领域聚合

封装 `VocoderInferenceService` + `VocoderRenderScheduler` 的生命周期。保证初始化/关闭顺序正确。

### bool VocoderDomain::initialize(const std::string& modelDir)

**描述**: 按顺序初始化推理服务 → 调度器

### void VocoderDomain::shutdown()

**描述**: 按逆序关闭调度器 → 推理服务

### void VocoderDomain::submit(Job job)

**描述**: 提交渲染任务（委托给内部调度器）

**Job 结构**:

| 字段 | 类型 | 说明 |
|------|------|------|
| `f0` | `std::vector<float>` | F0 曲线 |
| `energy` | `std::vector<float>` | 能量曲线 |
| `mel` | `std::vector<float>` | mel 频谱 |
| `onComplete` | `std::function<void(bool, const juce::String&, const std::vector<float>&)>` | 完成回调（success, errorMsg, audio） |

### int VocoderDomain::getQueueDepth() / bool isRunning() / bool isInitialized()

**描述**: 查询调度器/服务状态

### int VocoderDomain::getVocoderHopSize() / getMelBins()

**描述**: 获取声码器参数（委托给推理服务）

---

## 12. RenderCache（渲染缓存）

**文件**: `Source/Inference/RenderCache.h` / `.cpp`
**命名空间**: `OpenTune`

按时间索引的分 Chunk 渲染缓存，支持调度状态管理和 LRU 驱逐。

### void RenderCache::requestRenderPending(double startSeconds, double endSeconds)

**描述**: 编辑事件入口。递增 `desiredRevision`，状态 Idle/Blank → Pending。

**线程安全**: 是（`juce::SpinLock`）

### bool RenderCache::getNextPendingJob(PendingJob& outJob)

**描述**: Worker 拉取下一个 Pending 任务，状态 Pending → Running。

**线程安全**: 是（`juce::SpinLock`）

### void RenderCache::completeChunkRender(double startSeconds, uint64_t revision, CompletionResult result)

**描述**: 渲染完成回调。根据结果和版本匹配决定状态转移。

**状态转移规则**:
- `Succeeded` + 版本匹配 → Idle，发布 `publishedRevision`
- `Succeeded` + 版本过期 → Pending（重新入队）
- `RetryableFailure` → Pending（重试）
- `TerminalFailure` → Idle（放弃）

### bool RenderCache::addChunk()

**描述**: 添加渲染完成的音频 Chunk。执行版本匹配检查和全局内存限制驱逐。

**线程安全**: 是（`juce::SpinLock`）

| 参数 | 类型 | 说明 |
|------|------|------|
| `startSeconds` | `double` | Chunk 起始时间 |
| `endSeconds` | `double` | Chunk 结束时间 |
| `audio` | `std::vector<float>&&` | 渲染音频（move） |
| `targetRevision` | `uint64_t` | 目标版本号 |

### bool RenderCache::addResampledChunk()

**描述**: 添加重采样后的 Chunk（为非 44100Hz 设备率缓存）

### int RenderCache::readAtTimeForRate()

**描述**: 音频线程读取接口。支持非阻塞模式（`ScopedTryLock`），带线性插值。

**线程安全**: 是（非阻塞模式使用 `ScopedTryLock`，避免音频线程阻塞）

| 参数 | 类型 | 说明 |
|------|------|------|
| `dest` | `float*` | 输出缓冲区 |
| `numSamples` | `int` | 请求采样数 |
| `timeSeconds` | `double` | 读取时间位置 |
| `targetSampleRate` | `int` | 目标采样率 |
| `nonBlocking` | `bool` | 非阻塞模式（音频线程用） |

**返回**: 实际读取的采样数（0 = 未命中或锁竞争）

### 其他查询方法

| 方法 | 说明 |
|------|------|
| `isRevisionPublished()` | 检查指定版本是否已发布 |
| `getPublishedChunks()` | 获取所有已发布 Chunk 的视图 |
| `getPendingCount()` | Pending 任务数 |
| `getChunkStats()` | Chunk 状态统计（Idle/Pending/Running/Blank） |
| `markChunkAsBlank()` | 标记无有效 F0 的 Chunk |
| `clearAllPending()` | 清理所有 Pending 状态 |
| `clear()` / `clearResampledCache()` | 清空缓存 |
| `getTotalMemoryUsage()` | 当前实例内存使用量 |
| `setMemoryLimit()` | 设置全局缓存内存上限 |
| `getGlobalMemoryStats()` | 获取全局内存统计 |

---

## ⚠️ 待确认

### 接口歧义

1. **`VocoderInferenceService::doSynthesizeAudioWithEnergy` 为 `virtual`**：这是公开的虚方法，但 `synthesizeAudioWithEnergy` 内部通过 `lock_guard` + 调用 `doSynthesizeAudioWithEnergy` 实现。子类覆写 `doSynthesizeAudioWithEnergy` 时不受 `runMutex_` 保护 — 待确认是否为设计意图（如测试 mock）还是潜在线程安全问题。

2. **`VocoderInferenceService::synthesizeAudioWithEnergy` 的 `energy` 参数**：`energy` 被传入但在 Impl 中未使用（直接传给 `vocoder_->synthesize(f0, mel, melSize)`，VocoderInterface 签名中无 energy）。待确认是预留参数还是遗漏。

3. **`RenderCache::readAtTimeForRate` 的线性插值**：对非整数偏移使用线性插值（`s0 + (s1 - s0) * baseFraction`），但 `baseFraction` 只在首次计算，后续帧复用同一个 fraction — 待确认这是精度简化（忽略累积漂移）还是 bug。

### 缺失契约

4. **`IF0Extractor` 接口无异常规范**：接口注释未约定异常行为，但 `RMVPEExtractor::extractF0` 会抛出多种异常。待确认接口契约是否应标注异常保证。
