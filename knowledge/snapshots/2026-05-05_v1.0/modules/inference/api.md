---
spec_version: 1.0.0
status: draft
module: inference
doc_type: api
generated_by: arch-doc-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# Inference 模块 API 契约

本文档汇总 `Source/Inference/` 所有公共接口、抽象基类及服务契约。函数签名以源码为准。

---

## 1. IF0Extractor（`IF0Extractor.h`）

F0 提取抽象接口，所有实现必须接受任意采样率、内部处理重采样，返回 Hz 为单位的 F0 曲线（一帧一值）。

```cpp
class IF0Extractor {
public:
    virtual ~IF0Extractor() = default;

    virtual std::vector<float> extractF0(
        const float* audio,
        size_t length,
        int sampleRate,
        std::function<void(float)> progressCallback = nullptr,
        std::function<void(const std::vector<float>&, int)> partialCallback = nullptr
    ) = 0;

    virtual int         getHopSize() const = 0;
    virtual int         getTargetSampleRate() const = 0;
    virtual F0ModelType getModelType() const = 0;
    virtual std::string getName() const = 0;
    virtual size_t      getModelSize() const = 0;

    virtual void  setConfidenceThreshold(float threshold) = 0;
    virtual void  setF0Min(float minHz) = 0;
    virtual void  setF0Max(float maxHz) = 0;
    virtual float getConfidenceThreshold() const = 0;
    virtual float getF0Min() const = 0;
    virtual float getF0Max() const = 0;
};
```

**契约**

| 字段 | 约定 |
|------|------|
| `audio` | 非空 PCM float 指针；为空则实现可抛 `std::invalid_argument` |
| `length` | 样本数；必须 > 0 |
| `sampleRate` | 任意正整数；实现内部负责重采样至 `getTargetSampleRate()` |
| 返回值 | 一帧一值的 F0 曲线（Hz）；unvoiced = 0.0；失败返回空 vector 或抛异常 |
| `progressCallback(p)` | `p ∈ [0, 1]`；实现需保证调用频率合理 |
| `partialCallback(f0, offset)` | 可选分段结果回调 |
| 线程安全 | 实现不保证并发调用安全；使用方通过 `F0InferenceService::extractF0` 的 `shared_mutex` 协调 |

**支持的模型类型**

```cpp
enum class F0ModelType {
    RMVPE = 0    // Robust Multi-scale Vocal Pitch Estimator (361MB)
};
```

---

## 2. RMVPEExtractor（`RMVPEExtractor.h` / `.cpp`）

```cpp
class RMVPEExtractor : public IF0Extractor {
public:
    struct PreflightResult {
        bool        success = false;
        std::string errorMessage;
        std::string errorCategory;      // "MEMORY" | "DURATION" | "MODEL" | "SYSTEM"
        size_t      estimatedMemoryMB = 0;
        size_t      availableMemoryMB = 0;
        double      audioDurationSec = 0.0;
    };

    RMVPEExtractor(std::unique_ptr<Ort::Session> session,
                   std::shared_ptr<ResamplingManager> resampler);

    std::vector<float> extractF0(const float* audio, size_t length, int sampleRate,
                                 std::function<void(float)> progressCallback = nullptr,
                                 std::function<void(const std::vector<float>&, int)> partialCallback = nullptr
                                ) override;

    int         getHopSize()           const override { return 160; }   // 10 ms @ 16 kHz
    int         getTargetSampleRate()  const override { return 16000; }
    F0ModelType getModelType()         const override { return F0ModelType::RMVPE; }
    std::string getName()              const override { return "RMVPE"; }
    size_t      getModelSize()         const override { return 0; }     // 变动，工厂内部记录

    PreflightResult preflightCheck(size_t audioLength, int sampleRate) const;
};
```

**关键契约**

- **异常**
  - `std::invalid_argument` — audio == nullptr / length == 0 / sampleRate ≤ 0
  - `std::logic_error` — session 或 resampler 未初始化
  - `std::runtime_error` — 预检失败（"DURATION" / "MEMORY"）或 ONNX 推理失败
- **预检查**（`preflightCheck`）
  - 时长闸门：`durationSec > 600 s` → category `DURATION`
  - 内存预算：`estimated > systemMem - 512 MB` → category `MEMORY`
  - 会话检查：`session_ == nullptr` → 错误信息（无 category 字段）
  - VST3 构建下会减去已驻留的 350 MB 模型占用
- **后处理**（`extractF0` 内部调用）
  - `fixOctaveErrors`：在连续浊音段内，若相邻比值 ∈ (0.45, 0.55) 则 ×2
  - `fillF0Gaps`：在相邻浊音段之间、空隙 ≤ 8 帧（≈80 ms）时，对 log₂(F0) 做线性插值
- **私有常量**（`RMVPEExtractor.h`）

| 常量 | 值 | 用途 |
|------|----|------|
| `kMaxAudioDurationSec` | 600 | 时长闸门上限 |
| `kMemoryOverheadFactor` | 6.0 | 中间张量内存估算倍数 |
| `kMinReservedMemoryMB` | 512 | 系统预留内存 |
| `kModelMemoryMB` | 350 | 模型内存占用估算 |
| `kMaxGapFramesDefault` | 8 | 空隙填补最大帧数 |
| `fftOrder_` / `fftSize_` | 10 / 1024 | （已声明，当前路径不使用 FFT 推理） |

---

## 3. VocoderInterface（`VocoderInterface.h`）

```cpp
class VocoderInterface {
public:
    virtual ~VocoderInterface() = default;

    virtual std::vector<float> synthesize(
        const std::vector<float>& f0,
        const float* mel,
        size_t melSize
    ) = 0;

    virtual int getHopSize()    const { return 512; }
    virtual int getSampleRate() const { return 44100; }
    virtual int getMelBins()    const { return 128; }
};
```

**契约**

| 字段 | 约定 |
|------|------|
| `f0` | 一帧一值的 F0 曲线；长度决定 num_frames |
| `mel` | mel-spectrogram 展平数组；布局由模型决定（基类自动探测转置） |
| `melSize` | 必须等于 `melBins × numFrames`；不一致抛 `std::runtime_error` |
| 返回值 | `numFrames * hopSize` 个 float 样本 |
| 线程安全 | **非线程安全**：实现只允许被 `VocoderRenderScheduler` 单 worker 调用 |

---

## 4. OnnxVocoderBase（`OnnxVocoderBase.h` / `.cpp`，v1.3 新增）

Onnx 声码器的共享基类，实现了 I/O tensor 的通用准备流程；子类仅需实现 `runSession`。

```cpp
struct VocoderScratchBuffers {
    std::vector<float>              melOwned;
    std::vector<float>              uvData;
    std::vector<float>              melTransposed;
    std::vector<const char*>        inputNamesC;
    std::vector<Ort::Value>         inputTensors;
    std::vector<std::string>        inputNameStorage;
    std::vector<std::vector<float>>   extraFloatBuffers;
    std::vector<std::vector<int64_t>> extraInt64Buffers;

    void resetForRun(size_t frameCount, size_t inputCount);
};

class OnnxVocoderBase : public VocoderInterface {
public:
    std::vector<float> synthesize(const std::vector<float>& f0,
                                  const float* mel, size_t melSize) override;
    int getHopSize()    const override { return 512; }
    int getSampleRate() const override { return 44100; }
    int getMelBins()    const override { return static_cast<int>(melBinsHint_); }

protected:
    void detectInputOutputNames();
    void prepareInputTensors(VocoderScratchBuffers& scratch,
                             const std::vector<float>& f0,
                             const float* mel, size_t melSize,
                             Ort::MemoryInfo& memoryInfo);
    virtual std::vector<float> runSession(VocoderScratchBuffers& scratch,
                                          size_t numFrames) = 0;

    std::unique_ptr<Ort::Session>          session_;
    std::vector<std::string>               inputNames_;
    std::vector<std::string>               outputNames_;
    std::vector<std::vector<int64_t>>      inputShapes_;
    std::vector<ONNXTensorElementDataType> inputElemTypes_;

    int     melIndex_          = -1;
    int     f0Index_           = -1;
    int     uvIndex_           = -1;
    int64_t melBinsHint_       = 128;
    bool    melNeedsTranspose_ = false;
};
```

**I/O 自动识别规则**（`detectInputOutputNames`，大小写不敏感子串匹配）

| 字段 | 匹配规则 |
|------|----------|
| `melIndex_` | 输入名包含 `"mel"`，或名称严格等于 `"c"` |
| `f0Index_`  | 输入名等于 `"f0"`、或包含 `"f0"` / `"pitch"` |
| `uvIndex_`  | 输入名包含 `"uv"` 或 `"voiced"` |
| `melBinsHint_` | 从 mel 输入 shape 中取第一个大于 1 且不等于 128 的维；否则保持默认 128 |
| `melNeedsTranspose_` | mel 输入 shape 为三维且最后一维为 128 → true |

**prepareInputTensors 行为**

- 对 f0、mel 写入 tensor 引用原始缓冲（无拷贝）
- 自动构造 `uvData[i] = (f0[i] > 0 ? 1.0f : 0.0f)` 作为 uv 输入
- 对其他未识别输入：按元素类型填 0（int64 / float），形状按 `resolveShape`（将 ≤ 0 的维替换为 `numFrames`，将 128 替换为 `melBinsHint_`）
- `melSize ≠ melBins * numFrames` → 抛 `std::runtime_error`；若 `mel == nullptr` 则自动构造全 0 mel

**synthesize 流程**

1. `session_` 空 → 抛 `std::runtime_error`
2. `f0` 空 → 抛 `std::runtime_error`
3. 复用 `thread_local VocoderScratchBuffers`
4. `prepareInputTensors` 填充输入
5. 调用子类 `runSession(scratch, f0.size())` 返回音频

---

## 5. PCNSFHifiGANVocoder（`PCNSFHifiGANVocoder.h` / `.cpp`）

CPU / CoreML 后端的 NSF-HiFiGAN 声码器（继承 `OnnxVocoderBase`）。

```cpp
class PCNSFHifiGANVocoder : public OnnxVocoderBase {
public:
    explicit PCNSFHifiGANVocoder(std::unique_ptr<Ort::Session> session);
    ~PCNSFHifiGANVocoder() override;

protected:
    std::vector<float> runSession(VocoderScratchBuffers& scratch,
                                  size_t numFrames) override;
};
```

**runSession 行为**

- 若 `outputNames_` 为空，默认使用 `"audio"` 作为输出名
- 输出元素类型必须为 `ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT`
- 输出形状允许一维 `[N]` 或二维 `[1, N]`（batch 必须为 1）
- 各种校验失败均抛 `std::runtime_error("PCNSFHifiGANVocoder: ...")`

---

## 6. DmlVocoder（`DmlVocoder.h` / `.cpp`，仅 `_WIN32`）

```cpp
class DmlVocoder : public OnnxVocoderBase {
public:
    explicit DmlVocoder(const std::string& modelPath,
                        Ort::Env& env,
                        const DmlConfig& config);
    ~DmlVocoder() override;

protected:
    std::vector<float> runSession(VocoderScratchBuffers& scratch,
                                  size_t numFrames) override;
};

struct DmlConfig {
    int adapterIndex = 0;       // DXGI adapter index for DML1 API
};
```

**初始化阶段约束**

- 通过 `OrtApi::GetExecutionProviderApi("DML", ...)` 获取 DML API；失败抛 `std::runtime_error`，带结构化诊断（stage / hr / detail / remediation）
- 会话选项：
  - `IntraOpNumThreads = 1`、`InterOpNumThreads = 1`
  - `ExecutionMode = ORT_SEQUENTIAL`
  - `DisableMemPattern()` + `DisableCpuMemArena()`
  - `session.intra_op.allow_spinning = 0`、`session.inter_op.allow_spinning = 0`
  - `GraphOptimizationLevel = ORT_ENABLE_ALL`
- Windows 路径使用 `wchar_t*`；其他平台不会进入该类（`#ifdef _WIN32` 包裹）

**runSession 行为**

- 懒加载 `Ort::IoBinding`（第一次调用时初始化）
- 预分配输出 buffer：`numFrames * 512` 个 float，按 `[1, expectedAudioLength]` 形状绑定
- 若 `numFrames` 变化则重新分配输出缓冲
- 执行后调用 `SynchronizeOutputs()` 等待 GPU
- 返回 `outputBuffer_`（内部缓冲的拷贝）

---

## 7. ModelFactory（`ModelFactory.h` / `.cpp`）

```cpp
class ModelFactory {
public:
    using F0ExtractorResult = Result<std::unique_ptr<IF0Extractor>>;

    static F0ExtractorResult createF0Extractor(
        F0ModelType type,
        const std::string& modelDir,
        Ort::Env& env,
        std::shared_ptr<ResamplingManager> resampler);

    static std::string              getModelPath(F0ModelType type, const std::string& modelDir);
    static bool                     isModelAvailable(F0ModelType type, const std::string& modelDir);
    static std::vector<F0ModelInfo> getAvailableF0Models(const std::string& modelDir);
    static Ort::SessionOptions      createF0SessionOptions(bool& outGpuMode);

private:
    static std::unique_ptr<Ort::Session> loadF0Session(
        const std::string& modelPath, Ort::Env& env, bool& outGpuMode);
};
```

**错误路径**

- `ModelNotFound` — 模型文件不存在
- `SessionCreationFailed` — `loadF0Session` 返回 nullptr
- `InvalidModelType` — 未知枚举
- `ModelLoadFailed` — `Ort::Exception` 或 `std::exception` 被捕获

**`createF0SessionOptions` 行为**

- 始终禁用 MemPattern 和 CPU Arena
- macOS：尝试附加 CoreML EP（`ModelFormat=MLProgram`、`MLComputeUnits=CPUAndGPU`），失败回退 CPU
- 线程数来源于 `CpuBudgetManager::buildConfig(gpuMode)`
- `GraphOptimizationLevel = ORT_ENABLE_ALL`
- Debug 构建下若环境变量 `OPENTUNE_ORT_PROFILE ∈ {1,true,on,yes}` 启用 profiling

**`getAvailableF0Models` 硬编码**

```cpp
F0ModelInfo rmvpe {
    .type = F0ModelType::RMVPE,
    .name = "rmvpe",
    .displayName = "RMVPE (Robust)",
    .modelSizeBytes = 361 * 1024 * 1024,
    .isAvailable = <文件存在性>
};
```

模型文件路径：`modelDir + "/rmvpe.onnx"`。

---

## 8. VocoderFactory（`VocoderFactory.h` / `.cpp`）

```cpp
enum class VocoderBackend { CPU, DML, CoreML };

struct VocoderCreationResult {
    std::unique_ptr<VocoderInterface> vocoder;
    VocoderBackend backend;
    std::string    errorMessage;

    static VocoderCreationResult success(std::unique_ptr<VocoderInterface>, VocoderBackend);
    static VocoderCreationResult failure(VocoderBackend, const std::string&);
    bool success() const { return vocoder != nullptr; }
};

class VocoderFactory {
public:
    static VocoderCreationResult create(const std::string& modelPath, Ort::Env& env);
private:
    VocoderFactory() = delete;
};
```

**后端选择逻辑**（`VocoderFactory::create`）

1. `_WIN32` 且 `AccelerationDetector::getSelectedBackend() == DirectML`：
   - 创建 `DmlVocoder`；抛异常则记录错误、调用 `gpu.overrideBackend(CPU)`，进入下一步
2. 其他平台或回退：
   - 创建 `Ort::Session`，并附加 EP：
     - macOS：优先 CoreML（`MLProgram` + `CPUAndGPU`），失败回退 CPU
     - Windows / Linux：CPU
   - 构造 `PCNSFHifiGANVocoder`
3. 任意未捕获异常 → `VocoderCreationResult::failure(CPU, ...)`

**模型文件约定**：`modelDir + "/hifigan.onnx"`（由 `VocoderInferenceService::initialize` 拼接）。

---

## 9. F0InferenceService（`F0InferenceService.h` / `.cpp`）

CPU / CoreML 的 F0 提取服务，内部使用 Pimpl + `shared_mutex` 支持并发。

```cpp
class F0InferenceService {
public:
    F0InferenceService(std::shared_ptr<Ort::Env> env);
    ~F0InferenceService();

    bool initialize(const std::string& modelDir);
    void shutdown();

    Result<std::vector<float>> extractF0(
        const float* audio, size_t length, int sampleRate,
        std::function<void(float)> progressCallback = nullptr,
        std::function<void(const std::vector<float>&, int)> partialCallback = nullptr);

    bool        setF0Model(F0ModelType type);
    F0ModelType getCurrentF0Model() const;
    std::vector<F0ModelInfo> getAvailableF0Models() const;

    void  setConfidenceThreshold(float);
    void  setF0Min(float);
    void  setF0Max(float);
    float getConfidenceThreshold() const;
    float getF0Min() const;
    float getF0Max() const;

    int  getF0HopSize()    const;
    int  getF0SampleRate() const;
    bool isInitialized()   const;

    // 外部定期调用：释放超过 30 秒未使用的 F0 模型
    void releaseIdleModelIfNeeded();
};
```

**线程安全**

| 调用 | 锁 |
|------|----|
| `extractF0` | `std::shared_lock`（允许并发读取） |
| `setF0Model` / `set*` / `shutdown` | `std::unique_lock`（独占） |
| getter（`getF0Min` 等） | `std::shared_lock` |
| `releaseIdleModelIfNeeded` | 内部 `shutdown` 时使用 `unique_lock` |
| `initialized_` | `std::atomic<bool>` + `release/acquire` |
| `lastExtractionTimeMs_` | `std::atomic<uint64_t>` |

**生命周期**

- `extractF0`：若未初始化则尝试 `initialize(modelDir_)` 重建
- 每次 `extractF0` 成功/失败后更新 `lastExtractionTimeMs_`
- `releaseIdleModelIfNeeded`：若距离上次提取 ≥ 30000 ms → `shutdown()`
- 错误路径：`NotInitialized` / `ModelInferenceFailed`（封装底层 `std::exception`）

**参数默认值**（无 extractor 时的 getter 回退）

| Getter | 默认返回 |
|--------|----------|
| `getConfidenceThreshold` | `0.03f` |
| `getF0Min` | `30.0f` |
| `getF0Max` | `2000.0f` |
| `getF0HopSize` | `160` |
| `getF0SampleRate` | `16000` |

---

## 10. VocoderInferenceService（`VocoderInferenceService.h` / `.cpp`）

专供调度器单线程调用的声码器推理服务。

```cpp
class VocoderInferenceService {
public:
    VocoderInferenceService(std::shared_ptr<Ort::Env> env);
    ~VocoderInferenceService();

    bool initialize(const std::string& modelDir);   // 加载 modelDir + "/hifigan.onnx"
    void shutdown();

    Result<std::vector<float>> synthesize(
        const std::vector<float>& f0, const float* mel, size_t melSize);

    int getVocoderHopSize() const;   // 默认 512
    int getMelBins()        const;   // 默认 128
};
```

**契约**

- `synthesize` 未初始化 → `Result::failure(NotInitialized, ...)`
- 捕获底层 `std::exception` → `Result::failure(ModelInferenceFailed, what())`
- **不保证多线程安全**：`initialize` / `shutdown` 与 `synthesize` 不得重叠调用

---

## 11. VocoderRenderScheduler（`VocoderRenderScheduler.h` / `.cpp`）

串行 FIFO 调度器 + 单 worker 线程，保证 DML session 不被并发调用。

```cpp
class VocoderRenderScheduler {
public:
    struct Job {
        uint64_t           chunkKey{0};
        std::vector<float> f0;
        std::vector<float> mel;
        std::function<void(bool ok, const juce::String& err,
                           const std::vector<float>& audio)> onComplete;
    };

    VocoderRenderScheduler();
    ~VocoderRenderScheduler();                      // 自动 shutdown

    bool initialize(VocoderInferenceService* service); // service 必须 outlive scheduler
    void shutdown();                                // 停止 + drain queue（未处理 job 回调 false）
    void submit(Job job);

    static constexpr int kMaxQueueDepth = 50;
};
```

**队列语义**

| 情况 | 动作 |
|------|------|
| `acceptingJobs_` 为 false | 直接丢弃 submit（不回调） |
| 新 job 的 `chunkKey` 与队列中某 job 相同 | **替换** 旧 job；旧 job 的 `onComplete(false, "Superseded by newer revision", {})` 在锁外调用 |
| 队列已满（≥ 50）且未匹配到替换目标 | 丢弃队首（调用 `onComplete(false, "Queue overflow: job discarded", {})`） |
| worker 正常处理 | 成功 `onComplete(true, "", audio)`；失败 `onComplete(false, err, {})` |
| worker shutdown 期间 | `onComplete(false, "Scheduler shutdown", {})` |

**线程安全**

- `jobQueue_` 受 `queueMutex_` 保护
- `condition_variable queueCV_` 用于唤醒 worker
- `acceptingJobs_` 为 `std::atomic<bool>`
- Supersede 回调在锁外执行以避免潜在死锁

---

## 12. VocoderDomain（`VocoderDomain.h` / `.cpp`）

领域聚合层，将 Service 与 Scheduler 绑定为统一生命周期对象。

```cpp
class VocoderDomain {
public:
    struct Job {
        uint64_t           chunkKey{0};
        std::vector<float> f0;
        std::vector<float> mel;
        std::function<void(bool, const juce::String&, const std::vector<float>&)> onComplete;
    };

    VocoderDomain(std::shared_ptr<Ort::Env> env);
    ~VocoderDomain();

    bool initialize(const std::string& modelDir);   // 先 service.initialize，再 scheduler.initialize(service)
    void shutdown();
    void submit(Job job);                           // 透传给 scheduler
    int  getVocoderHopSize() const;
    int  getMelBins()         const;
};
```

**声明顺序契约**（关键）

成员声明顺序：`inferenceService_` 在前、`scheduler_` 在后。C++ 析构反序：先 scheduler 再 service，保证 scheduler 停止前 service 仍在，避免 worker 使用悬空指针。

---

## 13. RenderCache（`RenderCache.h` / `.cpp`）

带状态机与版本协议的渲染缓存，持有 LR4 `CrossoverMixer` 供播放侧使用。

```cpp
class RenderCache {
public:
    static constexpr size_t kDefaultGlobalCacheLimitBytes = 256 * 1024 * 1024;
    static constexpr double kSampleRate = TimeCoordinate::kRenderSampleRate;

    struct Chunk {
        double  startSeconds{0.0};
        double  endSeconds{0.0};
        int64_t startSample{0};
        int64_t endSampleExclusive{0};
        std::vector<float> audio;
        enum class Status : uint8_t { Idle, Pending, Running, Blank };
        Status   status{Status::Idle};
        uint64_t desiredRevision{0};
        uint64_t publishedRevision{0};
    };

    struct PendingJob {
        double   startSeconds{0.0};
        double   endSeconds{0.0};
        int64_t  startSample{0};
        int64_t  endSampleExclusive{0};
        uint64_t targetRevision{0};
    };

    enum class CompletionResult : uint8_t { Succeeded, TerminalFailure };

    struct ChunkStats {
        int idle{0}, pending{0}, running{0}, blank{0};
        int  total() const;
        bool hasActiveWork() const;
    };

    struct StateSnapshot {
        ChunkStats chunkStats;
        bool       hasPublishedAudio{false};
        bool       hasNonBlankChunks{false};
    };

    RenderCache();
    ~RenderCache();

    // 调度状态管理
    void requestRenderPending(double startSeconds, double endSeconds,
                              int64_t startSample, int64_t endSampleExclusive);
    bool getNextPendingJob(PendingJob& outJob);
    void completeChunkRender(double startSeconds, uint64_t revision, CompletionResult result);
    void markChunkAsBlank(double startSeconds);

    int           getPendingCount() const;
    ChunkStats    getChunkStats()   const;
    StateSnapshot getStateSnapshot()const;

    // 渲染结果写入
    bool addChunk(int64_t startSample, int64_t endSampleExclusive,
                  std::vector<float>&& audio, uint64_t targetRevision);

    // 非阻塞播放读取
    void overlayPublishedAudioForRate(juce::AudioBuffer<float>& destination,
                                      int destStartSample, int numSamples,
                                      double timeSeconds, int targetSampleRate) const;

    void clear();
    void prepareCrossoverMixer(double sampleRate, int maxBlockSize, int numChannels = 2);
    CrossoverMixer& getCrossoverMixer() const;
};
```

**锁与并发**

- 所有读写方法内使用 `juce::SpinLock::ScopedLockType`（`lock_`）
- 全局静态原子：`globalCacheLimitBytes()` / `globalCacheCurrentBytes()` / `globalCachePeakBytes()`
- 非阻塞读取路径（`overlayPublishedAudioForRate`）同样持锁；实现通过快速跳过未发布 / 空 chunk 降低临界区时长

**addChunk 拒收条件**（所有情况下日志打印 REJECT）

1. `audio.empty()` 或 `targetRevision == 0` 或 `endSampleExclusive <= startSample` → early-check 拒收
2. `audio.size() != (endSampleExclusive - startSample)` → sample-span-mismatch 拒收
3. 存量 chunk 已有非空 sample 范围、但与入参边界不一致 → sample-boundary-mismatch 拒收
4. `targetRevision != chunk.desiredRevision` → revision-mismatch 拒收

成功写入时：更新 `audio` 与 `publishedRevision = targetRevision`；更新全局字节计数；若超过 `kDefaultGlobalCacheLimitBytes` 触发 LRU 驱逐（保留 `currentChunk` 自身，遍历 `chunks_` 找到首个非空 chunk 清空）。

**状态转移由下述方法触发**（详见 `data-model.md` 的状态机图）：

- `requestRenderPending`：Idle/Blank → Pending；其他状态保持但 `desiredRevision++`
- `getNextPendingJob`：Pending → Running（返回 job）
- `completeChunkRender(Succeeded)`：Running → Idle（若 revision 匹配） / Pending（若已 stale）
- `completeChunkRender(TerminalFailure)`：Running → Idle
- `markChunkAsBlank`：Pending/Running → Blank（清空 audio）；其他状态忽略
- `addChunk` 本身不触发状态转移，但若 revision 匹配则更新 `publishedRevision`

---

## 14. 数据类型回顾

| 类型 | 定义位置 | 说明 |
|------|----------|------|
| `F0ModelType` | `IF0Extractor.h` | `enum class`，仅 `RMVPE = 0` |
| `F0ModelInfo` | `IF0Extractor.h` | 模型元信息（name / displayName / size / availability） |
| `RMVPEExtractor::PreflightResult` | `RMVPEExtractor.h` | 预检结果（success / message / category / memory） |
| `VocoderScratchBuffers` | `OnnxVocoderBase.h` | `thread_local` 缓冲，跨推理复用 |
| `VocoderBackend` | `VocoderFactory.h` | `CPU` / `DML` / `CoreML` |
| `VocoderCreationResult` | `VocoderFactory.h` | `{ vocoder, backend, errorMessage }` |
| `VocoderDomain::Job` | `VocoderDomain.h` | 等同于 `VocoderRenderScheduler::Job` 的结构 |
| `RenderCache::Chunk` | `RenderCache.h` | 渲染缓存单元 + 状态机字段 |
| `RenderCache::PendingJob` | `RenderCache.h` | 调度器拉取的作业描述 |
| `RenderCache::CompletionResult` | `RenderCache.h` | Succeeded / TerminalFailure |
| `Result<T>` | `../Utils/Error.h` | 通用结构化错误封装 |

---

## ⚠️ 待确认

1. **`F0InferenceService` 并发实际上限**：`shared_mutex` 允许并发 `extractF0`，但 RMVPEExtractor 内部使用同一 `Ort::Session`。Onnx session 是否完全支持并发 Run？若不支持，Service 层的 shared_lock 能力可能引入未被发现的竞态（现有代码没有显式互斥 RMVPEExtractor 的状态）。
2. **`VocoderDomain::Job` 的实际投递方**：代码中 `VocoderDomain::submit` 没有被 Inference 模块内部调用，需追溯调用方（PluginProcessor / 某个 Service）才能给出“调度→完成回调→RenderCache::addChunk”的完整链路。
3. **`OnnxVocoderBase::detectInputOutputNames` 中 `uv` 输入匹配**：当前实现会同时匹配 `"uv"` 和 `"voiced"`，但 `PCNSFHifiGAN` 模型是否实际需要 uv 输入（而非由基类自动导出）未从模型元数据侧验证。若模型本身已包含 uv 计算节点，额外 feed uv 可能被忽略或导致错误。
4. **`VocoderInterface::getMelBins()` 默认 128 vs `melBinsHint_` 回退**：`OnnxVocoderBase::getMelBins()` 返回 `melBinsHint_`（默认 128，由探测调整）；若模型输入 shape 中首个非 1 维恰好等于 128，探测会跳过，行为正确；但若模型实际用 80 bins 且写死在 shape 首位，则 `getMelBins()` 会返回 80。调用方（mel 构造器）需与实际模型保持一致。
5. **RMVPE 置信度阈值的双重身份**：`extractF0` 中 `threshold` 作为 tensor 直接传入模型（名为 `threshold`），同时又在 `enableUvCheck_ == true` 时被用作 uv 过滤阈值。当前 `enableUvCheck_` 默认 false，该路径是否被 Service 层开启未知。
6. **`DmlVocoder::runSession` 的 `outputBuffer_` 返回方式**：函数返回 `outputBuffer_`（成员变量的拷贝）。由于 session 是串行的，这是安全的；但若未来接入并发，必须改为返回值构造或显式复制。
