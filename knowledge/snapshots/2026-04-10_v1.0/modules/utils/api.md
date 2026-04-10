---
module: utils
type: api
generated: 2026-04-10
source_scan: true
---

# utils -- API Reference

> 本模块无 HTTP Controller，此文档记录基础设施工具层对外暴露的编程接口契约。

> **本文档基于源码扫描自动生成，标注 `[待确认]` 处需人工复核。**

---

## 1. AppLogger (日志)

**文件**: `Source/Utils/AppLogger.h` / `AppLogger.cpp`

全静态接口，线程安全（内部 `juce::CriticalSection`）。

| 方法 | 签名 | 说明 |
|---|---|---|
| `initialize` | `static void initialize()` | 初始化日志系统，创建带日期戳的日志文件到 `./Logs/` 目录。幂等——重复调用无操作 |
| `shutdown` | `static void shutdown()` | 关闭日志系统，释放 `FileLogger` |
| `log` | `static void log(const juce::String& message)` | 直接写日志（无级别前缀）。首次调用会自动 `initialize()` |
| `debug` | `static void debug(const juce::String& message)` | DEBUG 级别日志 |
| `info` | `static void info(const juce::String& message)` | INFO 级别日志 |
| `warn` | `static void warn(const juce::String& message)` | WARN 级别日志 |
| `error` | `static void error(const juce::String& message)` | ERROR 级别日志 |
| `setLogLevel` | `static void setLogLevel(LogLevel level)` | 设置全局日志级别过滤 |
| `getLogLevel` | `static LogLevel getLogLevel()` | 获取当前日志级别 |
| `getCurrentLogFile` | `static juce::File getCurrentLogFile()` | 获取当前日志文件路径 |
| `logPerf` | `static void logPerf(const juce::String& operation, double durationMs)` | 输出 `[PERF]` 标签的性能日志 |

### PerfTimer (RAII 性能计时器)

```cpp
explicit PerfTimer(const juce::String& operation)
```

构造时记录起始时间，析构时调用 `AppLogger::logPerf`。**注意：当前析构函数中 `return;` 语句提前返回，性能日志实际被禁用。**

---

## 2. UndoAction / UndoManager (撤销/重做)

**文件**: `Source/Utils/UndoAction.h` / `UndoAction.cpp`

### 2.1 UndoAction (抽象基类)

```cpp
class UndoAction {
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual juce::String getDescription() const = 0;
    virtual uint64_t getClipId() const = 0;
};
```

### 2.2 具体 Action 子类

| 类名 | 描述 | 关键构造参数 |
|---|---|---|
| `CompoundUndoAction` | 组合多个 Action 为原子操作 | `description` |
| `NotesChangeAction` | 音符编辑 | `processor, trackId, clipId, oldNotes, newNotes` |
| `CorrectedSegmentsChangeAction` | F0 校正段修改 | `clipId, oldSegments, newSegments, applier, info` |
| `ClipSplitAction` | 片段分割 | `processor, trackId, clipId, result, indices` |
| `ClipGainChangeAction` | 片段增益修改 | `processor, trackId, clipId, oldGain, newGain` |
| `ClipMoveAction` | 片段移动 | `processor, trackId, clipId, oldStart, newStart` |
| `ClipDeleteAction` | 片段删除 | `processor, trackId, clipId, deletedIndex, snapshot` |
| `ClipCreateAction` | 导入音频 | `processor, trackId, clipId` |
| `ScaleKeyChangeAction` | 全局调式变更 | `oldRoot, oldScale, newRoot, newScale, applier` |
| `ClipScaleKeyChangeAction` | 片段级调式变更 | `trackId, clipId, old/new root/scale, applier` |
| `TrackMuteAction` | 轨道静音 | `processor, trackId, old/new muted, uiUpdater` |
| `TrackSoloAction` | 轨道独奏 | `processor, trackId, old/new solo, uiUpdater` |
| `TrackVolumeAction` | 轨道音量 | `processor, trackId, old/new volume, uiUpdater` |
| `ClipCrossTrackMoveAction` | 跨轨道移动片段 | `processor, source/target trackId, clipId, old/new start` |

### 2.3 CorrectedSegmentsChangeAction 静态工厂方法

| 方法 | 说明 |
|---|---|
| `captureSegments(curve)` | 从 PitchCurve 捕获当前校正段快照 |
| `snapshotsEquivalent(a, b, epsilon)` | 比较两组快照是否等价（浮点容差） |
| `makeCurveApplier(curve)` | 创建应用快照到 PitchCurve 的闭包 |
| `createForCurve(...)` | 完整工厂方法，返回 `unique_ptr<CorrectedSegmentsChangeAction>` |

### 2.4 UndoManager

```cpp
class UndoManager {
    explicit UndoManager(int maxHistorySize = 100);
    void addAction(std::unique_ptr<UndoAction> action);
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    void clear();
    juce::String getUndoDescription() const;
    juce::String getRedoDescription() const;
    int getHistorySize() const;
};
```

**特殊行为**: `addAction` 中检测"Scale Change + Auto Tune"连续操作时，自动合并为 `CompoundUndoAction`。

---

## 3. LockFreeQueue (无锁队列)

**文件**: `Source/Utils/LockFreeQueue.h`

模板类 `LockFreeQueue<T>`，MPMC 无锁有界队列。

```cpp
template<typename T>
class LockFreeQueue {
    explicit LockFreeQueue(size_t capacity = 1024); // capacity 必须是 2 的幂
    bool try_enqueue(const T& item);
    bool try_enqueue(T&& item);
    bool try_dequeue(T& item);
    size_t size() const;
    bool empty() const;
    size_t capacity() const;
    void clear();
};
```

**实现**: 基于环形缓冲区 + `sequence` 原子变量，`enqueuePos_` / `dequeuePos_` 对齐到 64 字节缓存行（`alignas(64)`）。

---

## 4. PresetManager (预设管理)

**文件**: `Source/Utils/PresetManager.h` / `PresetManager.cpp`

| 方法 | 签名 | 说明 |
|---|---|---|
| `savePreset` | `bool savePreset(const PresetData&, const juce::File&)` | 序列化为 XML 写入文件 |
| `loadPreset` | `PresetData loadPreset(const juce::File&)` | 从 XML 文件反序列化 |
| `getPresetFiles` | `vector<File> getPresetFiles(const File& directory)` | 枚举 `*.otpreset` 文件 |
| `getDefaultPresetDirectory` | `juce::File getDefaultPresetDirectory()` | 返回 `~/Documents/OpenTune/Presets/` |
| `captureCurrentState` | `PresetData captureCurrentState(processor)` | 从处理器捕获当前状态 |
| `applyPreset` | `void applyPreset(const PresetData&, processor)` | 应用预设到处理器 |

**预设文件格式**: XML，扩展名 `.otpreset`。

---

## 5. CpuFeatures (CPU 特性检测)

**文件**: `Source/Utils/CpuFeatures.h` / `CpuFeatures.cpp`

单例，程序启动时调用 `detect()` 一次。

| 方法 | 说明 |
|---|---|
| `getInstance()` | 获取单例 |
| `detect()` | 检测并缓存 CPU 特性（幂等） |
| `getSimdLevel()` | 返回最高 SIMD 级别 (`SimdLevel` 枚举) |
| `hasSSE2()` / `hasSSE41()` / `hasAVX()` / `hasAVX2()` / `hasFMA()` / `hasAVX512()` / `hasNEON()` | 各指令集查询 |
| `getCpuInfoString()` | 格式化日志字符串 (品牌 + SIMD 级别 + 核心数) |

**SimdLevel 枚举**: `None < SSE2 < SSE41 < AVX < AVX2 < AVX512 < NEON`

---

## 6. SimdAccelerator (SIMD 加速器)

**文件**: `Source/Utils/SimdAccelerator.h` / `SimdAccelerator.cpp`

单例，通过函数指针动态分派到最优 SIMD 实现。

### 基础算术

| 方法 | 签名 | 说明 |
|---|---|---|
| `sumOfSquares` | `float (const float*, size_t)` | 平方和 |
| `dotProduct` | `float (const float*, const float*, size_t)` | 点积 |
| `multiplyAdd` | `void (float*, const float*, const float*, float, size_t)` | `result = a * b + c` |
| `multiply` | `void (float*, const float*, const float*, size_t)` | 逐元素乘 |
| `add` | `void (float*, const float*, const float*, size_t)` | 逐元素加 |
| `absMax` | `float (const float*, size_t)` | 绝对值最大值 |
| `findMinMax` | `void (const float*, size_t, float&, float&)` | 最小/最大值 |

### 向量化数学

| 方法 | 说明 | macOS 实现 | 其他平台 |
|---|---|---|---|
| `vectorLog` | 自然对数 | `vvlogf` (Accelerate) | 标量回退 |
| `vectorExp` | 指数 | `vvexpf` | 标量回退 |
| `vectorSqrt` | 平方根 | `vvsqrtf` | 标量回退 |
| `complexMagnitude` | 复数模长 | `vDSP_vdist` | 标量回退 |

**SimdLevel 枚举**: `None < SSE2 < AVX < AVX2 < AVX512 < NEON < Accelerate`

**分派层级**: macOS → Accelerate; ARM64 → NEON; x86 → AVX512 > AVX2 > AVX > SSE2 > Scalar

---

## 7. AccelerationDetector (加速后端检测)

**文件**: `Source/Utils/AccelerationDetector.h` / `AccelerationDetector.cpp`

单例，检测 GPU 加速后端可用性。

| 方法 | 说明 |
|---|---|
| `detect()` | 检测并选择最佳后端（幂等） |
| `getSelectedBackend()` | 返回 `AccelBackend::CPU / DirectML / CoreML` |
| `isDirectMLAvailable()` | DirectML 是否可用 |
| `isCoreMLAvailable()` | CoreML 是否可用 |
| `getBackendName()` | 后端名称字符串 |
| `getGpuInfoString()` | GPU 信息摘要 |
| `getAccelerationReport()` | 完整加速状态报告（支持中文） |
| `getDirectMLDeviceId()` | DML 适配器索引 |
| `getGpuDevices()` | 枚举到的所有 GPU 列表 |
| `getSelectedGpu()` | 选中的 GPU 信息 |
| `getRecommendedGpuMemoryLimit()` | 推荐显存限制（VRAM 的 60%） |
| `isDmlProviderDllAvailable()` | DML Provider DLL 是否可用 |

**GPU 选择策略**: 独立显卡优先 → 显存最大优先 → 有效显存 < 512MB 回退 CPU。

---

## 8. CpuBudgetManager (CPU 线程预算)

**文件**: `Source/Utils/CpuBudgetManager.h` / `CpuBudgetManager.cpp`

纯静态工具类。

| 方法 | 签名 | 说明 |
|---|---|---|
| `computeTotalBudget` | `static int computeTotalBudget(unsigned int hardwareThreads)` | `max(4, floor(threads * 0.6))` |
| `buildConfig` | `static BudgetConfig buildConfig(bool gpuMode, unsigned int hardwareThreads)` | GPU 模式: `onnxIntra=2`; CPU 模式: `onnxIntra=0` |

**BudgetConfig 结构**:
- `totalBudget`: 总线程预算
- `onnxIntra`: ONNX 内线程数
- `onnxInter`: ONNX 间线程数（固定为 1）
- `onnxSequential`: 顺序执行（固定为 true）
- `allowSpinning`: 禁止自旋（固定为 false）

---

## 9. DmlRuntimeVerifier (DirectML 运行时验证)

**文件**: `Source/Utils/DmlRuntimeVerifier.h` / `DmlRuntimeVerifier.cpp`

| 方法 | 签名 | 说明 |
|---|---|---|
| `verify` | `static DmlDiagnosticReport verify(uint32_t adapterIndex, const string& adapterName)` | 完整验证 DirectML 可用性 |

**验证阶段**:
1. Windows 版本 >= 19044 (21H2)
2. DirectML.dll 存在且版本 >= 1.15.4
3. D3D12Core.dll (Agility SDK) 存在
4. DXGI Factory 创建
5. DXGI Adapter 枚举
6. D3D12 设备创建
7. D3D12 命令队列创建
8. DML 设备创建 (Feature Level 5.0)

---

## 10. SilentGapDetector (静息处检测)

**文件**: `Source/Utils/SilentGapDetector.h` / `SilentGapDetector.cpp`

| 方法 | 说明 |
|---|---|
| `detectAllGaps(audio, threshold_dB)` | 检测所有静息处（44.1kHz 音频） |
| `detectAllGapsAdaptive(audio, maxSearchDistance)` | 自适应检测（当前委托到 `detectAllGaps`） |
| `findNearestGap(gaps, pos, maxDist, forward)` | 查找最近静息处 |
| `findGapContaining(gaps, pos)` | 查找包含指定位置的静息处 |
| `hasGapBetween(gaps, start, end)` | 两位置间是否有静息处 |
| `getGapsBetween(gaps, start, end)` | 获取区间内所有静息处 |
| `getConfig()` / `setConfig()` / `resetConfig()` | 线程安全的配置管理 |

**DetectionConfig 参数**:
- `strictThreshold_dB`: -40 dBFS（严格判定）
- `relaxedTotalThreshold_dB`: -30 dBFS（放宽总电平）
- `lowBandThreshold_dB`: -40 dBFS（<=3kHz 低频带）
- `highPassCutoffHz`: 60 Hz
- `lowBandUpperHz`: 3000 Hz
- `minGapDurationMs`: 100 ms

---

## 11. Error / Result (错误处理)

**文件**: `Source/Utils/Error.h`

### ErrorCode 枚举

| 范围 | 类别 |
|---|---|
| 100-104 | 模型错误 (ModelNotFound, ModelLoadFailed, ...) |
| 200-202 | 初始化错误 |
| 300-303 | 音频输入错误 |
| 400-402 | F0/声码器错误 |
| 500-502 | Mel 频谱错误 |
| 600-602 | 通用错误 (InvalidParameter, OutOfMemory, OperationCancelled) |

### Error 结构

```cpp
struct Error {
    ErrorCode code;
    std::string message;
    std::string context;
    static Error fromCode(ErrorCode c, const std::string& ctx = "");
    bool ok() const;
    std::string fullMessage() const;
};
```

### Result<T> 模板

Rust-style `Result` 类型，基于 `std::variant<T, Error>`。

| 方法 | 说明 |
|---|---|
| `ok()` | 是否成功 |
| `value()` / `operator*()` / `operator->()` | 获取值（失败时抛 `runtime_error`） |
| `error()` | 获取错误 |
| `valueOr(default)` | 安全取值 |
| `map(f)` | 函数式映射 |
| `andThen(f)` | 函数式链式调用 |

特化 `Result<void>` 用于无返回值操作。

---

## 12. 配置结构体

### KeyShortcutConfig

**文件**: `Source/Utils/KeyShortcutConfig.h`  
**命名空间**: `OpenTune::KeyShortcutConfig`

| 函数 | 说明 |
|---|---|
| `getSettings()` / `setSettings()` | 全局快捷键配置读写 |
| `getShortcutBinding(id)` | 获取指定快捷键绑定 |
| `addShortcutBinding(id, binding)` | 添加绑定 |
| `removeShortcutBinding(id, binding)` | 移除绑定 |
| `setShortcutBinding(id, binding)` | 替换绑定 |
| `resetShortcutBinding(id)` / `resetAllShortcutBindings()` | 重置默认 |
| `matchesShortcut(id, keyPress)` | 匹配快捷键 |
| `findConflictingShortcut(excludeId, binding)` | 冲突检测 |
| `parseKeyBinding(text, outBinding)` | 文本解析快捷键 |

**ShortcutId 枚举**: `PlayPause, Stop, PlayFromStart, Undo, Redo, Cut, Copy, Paste, SelectAll, Delete`

### ZoomSensitivityConfig

**文件**: `Source/Utils/ZoomSensitivityConfig.h`  
**命名空间**: `OpenTune::ZoomSensitivityConfig`

| 常量 | 默认值 | 范围 |
|---|---|---|
| `kDefaultHorizontalZoomFactor` | 0.35 | [0.1, 1.0] |
| `kDefaultVerticalZoomFactor` | 0.35 | [0.1, 1.0] |
| `kDefaultScrollSpeed` | 90.0 | [30.0, 300.0] |

### PitchControlConfig

**文件**: `Source/Utils/PitchControlConfig.h`  
**命名空间**: `OpenTune::PitchControlConfig`

| 常量 | 默认值 |
|---|---|
| `kDefaultRetuneSpeedPercent` | 15.0% |
| `kDefaultVibratoDepth` | 0.0 |
| `kDefaultVibratoRateHz` | 7.5 Hz |
| `kDefaultNoteSplitCents` | 80 cents (range: 0-200) |

### MouseTrailConfig

**文件**: `Source/Utils/MouseTrailConfig.h`  
**命名空间**: `OpenTune::MouseTrailConfig`

| 函数 | 说明 |
|---|---|
| `isEnabled()` | 是否启用鼠标轨迹 |
| `setTheme(theme)` / `getTheme()` | 设置/获取轨迹主题 |
| `getThemeStyle(theme)` | 获取主题样式参数 |

**TrailTheme**: `None, Classic, Neon, Fire, Ocean, Galaxy, CherryBlossom, Matrix`

---

## 13. TimeCoordinate (坐标转换)

**文件**: `Source/Utils/TimeCoordinate.h`  
**命名空间**: `OpenTune::TimeCoordinate`

| 函数 | 说明 |
|---|---|
| `samplesToSeconds(samples, sampleRate)` | 采样数 → 秒 |
| `secondsToSamplesExact(seconds, sampleRate)` | 秒 → 采样数 (精确 double) |
| `secondsToSamples(seconds, sampleRate)` | 秒 → 采样数 (int64_t 截断) |

**核心常量**: `kRenderSampleRate = 44100.0` — 全局渲染采样率单一真实来源。

---

## 14. ClipSnapshot (片段快照)

**文件**: `Source/Utils/ClipSnapshot.h`

```cpp
struct ClipSnapshot {
    shared_ptr<const AudioBuffer<float>> audioBuffer;
    double startSeconds;
    float gain;
    double fadeInDuration / fadeOutDuration;
    juce::String name;
    juce::Colour colour;
    shared_ptr<PitchCurve> pitchCurve;
    OriginalF0State originalF0State;
    DetectedKey detectedKey;
    shared_ptr<RenderCache> renderCache;
    vector<SilentGap> silentGaps;
};
```

**OriginalF0State**: `NotRequested → Extracting → Ready / Failed`

---

## 15. LocalizationManager (本地化)

**文件**: `Source/Utils/LocalizationManager.h`

| 类/函数 | 说明 |
|---|---|
| `LocalizationManager::getInstance()` | 单例 |
| `setLanguage(lang)` / `getLanguage()` | 设置/获取当前语言 |
| `addListener` / `removeListener` | 语言变更观察者 |
| `Loc::get(key)` | 根据当前语言获取翻译文本 |
| `Loc::get(lang, key)` | 指定语言获取翻译 |
| `Loc::format(pattern, arg0, ...)` | 占位符替换 |

**支持语言**: English, 中文, 日本語, Русский, Español

**宏**: `LOC(key)`, `LOC_KEY(key)`, `LOC_RAW(key)`

---

## 16. ModelPathResolver (模型路径解析)

**文件**: `Source/Utils/ModelPathResolver.h`

| 方法 | 说明 |
|---|---|
| `ensureOnnxRuntimeLoaded()` | 确保 `onnxruntime.dll` 已加载 |
| `getModelsDirectory()` | 解析模型目录路径 |

**搜索顺序**: Program Files/OpenTune → ProgramData/OpenTune → 模块同目录 → Resources/models → 注册表 → CWD → 可执行文件目录

---

## 17. 平台适配 (Windows 专用)

### D3D12AgilityBootstrap

**文件**: `Source/Utils/D3D12AgilityBootstrap.cpp`

导出 `D3D12SDKVersion` 和 `D3D12SDKPath` 符号，启用 D3D12 Agility SDK 运行时加载。

### WindowsDllSearchPath

**文件**: `Source/Utils/WindowsDllSearchPath.cpp`

全局静态初始化器 `WindowsDllSearchPathInitializer`，在程序加载时自动将模块所在目录添加到 DLL 搜索路径。

### OnnxRuntimeDelayLoadHook

**文件**: `Source/Utils/OnnxRuntimeDelayLoadHook.cpp`

MSVC 延迟加载钩子 `__pfnDliNotifyHook2`，拦截 `onnxruntime.dll` 的延迟加载请求，按优先级搜索候选路径：模块目录 → Program Files → ProgramData。

---

## [待确认]

1. **PerfTimer 实际禁用**: `~PerfTimer()` 中有 `return;` 语句，性能日志永远不会输出。是有意为之还是遗留 debug 代码？
2. **PresetManager 状态捕获不完整**: `captureCurrentState` 仅捕获 `zoomLevel` 和 `bpm`，未捕获 `retuneSpeed`、`scaleRoot` 等 `PresetData` 中定义的处理参数。`applyPreset` 同理仅应用 zoom 和 bpm。
3. **detectAllGapsAdaptive 无差异**: 当前直接委托 `detectAllGaps`，`maxSearchDistanceSec` 参数未使用。
4. **LockFreeQueue::size() 近似值**: 非原子读取 `enqueuePos_` 和 `dequeuePos_`，返回值为近似值而非精确值。
5. **LocalizationManager 翻译方式**: 当前使用静态数组线性查找，`Loc::tr()` 函数实际未做翻译（直接返回 key 的 UTF-8）。仅 `Loc::get()` 真正查表。
