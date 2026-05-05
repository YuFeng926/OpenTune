---
spec_version: 1.0.0
status: draft
module: utils
doc_type: api
generated_by: module-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# Utils 模块 API 契约

> 本文按子系统分组列出所有公共接口的签名、契约、线程安全说明。所有类型位于 `OpenTune` 命名空间（部分嵌套命名空间如 `OpenTune::TimeCoordinate` / `OpenTune::KeyShortcutConfig` / `OpenTune::MouseTrailConfig` / `OpenTune::PitchControlConfig` / `OpenTune::ZoomSensitivityConfig` / `OpenTune::Loc` / `OpenTune::Loc::Keys`）。

## 1. 日志 AppLogger (`AppLogger.h`)

### 枚举 `LogLevel`
```cpp
enum class LogLevel { Debug, Info, Warning, Error };
```
默认级别 `Info`。优先级低于当前级别的日志被丢弃。

### 类 `AppLogger`（全静态接口）
| 方法 | 契约 |
|---|---|
| `static void initialize()` | 幂等；在用户 AppData/OpenTune/Logs 创建日期分文件，失败回退到临时目录；设置为 JUCE 全局 logger |
| `static void shutdown()` | 幂等；取消 JUCE logger 绑定，释放 FileLogger |
| `static void log(const juce::String&)` | 原始写入。内部会 `initialize()`（lazy） |
| `static void debug(msg)` / `info(msg)` / `warn(msg)` / `error(msg)` | 按级别过滤后调用 `log()`，前缀 `[DEBUG]/[INFO]/[WARN]/[ERROR]` |
| `static void setLogLevel(LogLevel)` / `static LogLevel getLogLevel()` | 线程安全（内部 `juce::CriticalSection`） |
| `static juce::File getCurrentLogFile()` | 返回当前日志文件路径（若未初始化返回空 `juce::File{}`） |

线程安全：**全部方法线程安全**。内部用 `juce::CriticalSection` 保护 logger 指针与级别状态。

## 2. 撤销/重做 UndoManager (`UndoManager.h/.cpp`)

### 抽象基类 `UndoAction`
```cpp
class UndoAction {
public:
    virtual ~UndoAction() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual juce::String getDescription() const = 0;
};
```
子类由业务方提供（当前有 `PianoRollEditAction`、`PlacementActions` 等）。`undo()`/`redo()` 必须对称、幂等。

### 类 `UndoManager`
| 方法 | 契约 |
|---|---|
| `void addAction(std::unique_ptr<UndoAction>)` | 1) 若当前正在 undo/redo（`isPerformingUndoRedo_=true`）或 action 为空，直接 return；2) 丢弃 cursor 之后的 redo 历史；3) push；4) 若超过 500 条，erase 首元素；5) `cursor_ = actions_.size()`；6) 触发 `onChange` |
| `UndoAction* undo()` | 若 `canUndo()` 为假，返回 `nullptr`；否则在 `isPerformingUndoRedo_=true` 护栏下调用 `actions_[--cursor_]->undo()` 并触发 `onChange`，返回该 action 指针 |
| `UndoAction* redo()` | 对称：调用 `actions_[cursor_++]->redo()` |
| `bool canUndo() const` | `cursor_ > 0` |
| `bool canRedo() const` | `cursor_ < actions_.size()` |
| `void clear()` | 清空栈，`cursor_=0`，触发 `onChange` |
| `juce::String getUndoDescription() const` | 返回 `actions_[cursor_-1]->getDescription()`；不可 undo 时返回空字符串 |
| `juce::String getRedoDescription() const` | 对称 |
| `void setOnChange(ChangeCallback)` | 栈结构变化时回调（undo/redo/add/clear 均触发） |

线程安全：**非线程安全**，约定仅 message thread 调用。护栏字段 `isPerformingUndoRedo_` 用于防止 `undo()/redo()` 执行期间 action 内部再次 `addAction`（避免栈被破坏）。

## 3. 无锁队列 LockFreeQueue (`LockFreeQueue.h`)

### 模板 `LockFreeQueue<T>`
```cpp
template<typename T> class LockFreeQueue {
    explicit LockFreeQueue(size_t capacity = 1024);
    bool try_enqueue(const T& item);
    bool try_enqueue(T&& item);
    bool try_dequeue(T& item);
    size_t size() const;        // 近似值
    bool empty() const;
    size_t capacity() const;
    void clear();
};
```
| 方法 | 契约 |
|---|---|
| 构造 | `capacity` 必须为 2 的幂（`assert` 检查）。默认 1024。使用 `Cell` 数组 + 序号约定实现 MPMC bounded queue（Vyukov 算法） |
| `try_enqueue(const T&)` / `try_enqueue(T&&)` | CAS 竞争写入。队满返回 `false`。不阻塞 |
| `try_dequeue(T& out)` | CAS 竞争读取。队空返回 `false`。通过 `std::move` 将 cell 中的数据转出 |
| `size()` | `enqueuePos - dequeuePos`，**近似值**（并发下可能短暂不精确） |
| `clear()` | 循环 `try_dequeue` 直到空（非原子，不要在有并发读/写时使用） |

线程安全：MPMC 无锁，`enqueuePos_` / `dequeuePos_` 以 `alignas(64)` 隔离 cache line。`Cell::sequence` 用 acquire/release 排序保证数据可见。

## 4. 预设管理 PresetManager (`PresetManager.h/.cpp`)

### 结构 `PresetData`
```cpp
struct PresetData {
    juce::String name, author, description;
    int64_t timestamp;                  // 构造时 = Time::currentTimeMillis()
    int version{1};
    float retuneSpeed{15.0f};
    int scaleRoot{0}; bool isMinorScale{false};
    float vibratoDepth{0.0f}; float vibratoRate{6.0f};
    bool showWaveform{true}; bool showLanes{true};
    double zoomLevel{1.0}; double bpm{120.0};
    juce::ValueTree toValueTree() const;
    static PresetData fromValueTree(const juce::ValueTree&);
};
```

### 类 `PresetManager`
| 方法 | 契约 |
|---|---|
| 构造 | 创建 `~/Documents/OpenTune/Presets/` 作为默认目录（不存在则创建） |
| `bool savePreset(const PresetData&, const juce::File&)` | 序列化为 XML，写入指定文件。失败时返回 `false` 并 `DBG` 输出 |
| `PresetData loadPreset(const juce::File&)` | 读取并解析；失败返回默认构造的 `PresetData` |
| `std::vector<juce::File> getPresetFiles(const juce::File& dir)` | 按 `*.otpreset` 枚举（不递归）。目录不存在返回空 |
| `juce::File getDefaultPresetDirectory()` | 构造时设定的 Documents/OpenTune/Presets |
| `PresetData captureCurrentState(OpenTuneAudioProcessor&)` | **仅捕获 `zoomLevel/bpm`**，其余字段为默认值（见 ⚠️） |
| `void applyPreset(const PresetData&, OpenTuneAudioProcessor&)` | **仅应用 `zoomLevel/bpm`** |

线程安全：无内部同步，约定在 message thread 调用。

## 5. 错误处理 Error.h

### 枚举 `ErrorCode`
按范围划分：
- `Success = 0`
- 模型相关 100–104：`ModelNotFound / ModelLoadFailed / ModelInferenceFailed / InvalidModelType / SessionCreationFailed`
- 初始化 200–202：`NotInitialized / AlreadyInitialized / InitializationFailed`
- 音频输入 300–303：`InvalidAudioInput / InvalidSampleRate / InvalidAudioLength / AudioTooShort`
- F0/Vocoder 400–402
- Mel 500–502
- 通用 600–602
- `UnknownError = 999`

### 结构 `Error`
```cpp
struct Error {
    ErrorCode code{ErrorCode::Success};
    std::string message;
    std::string context;
    static Error fromCode(ErrorCode, const std::string& ctx = "");
    bool ok() const; explicit operator bool() const;
    std::string fullMessage() const;        // "ctx: message"
};
```

### 模板 `Result<T>` / 特化 `Result<void>`
```cpp
template<typename T> class Result {
    Result(T);
    Result(Error);
    static Result success(T); static Result failure(ErrorCode, ctx = "");
    bool ok() const; explicit operator bool() const;
    const T& value() const&; T&& value() &&;        // 错误时抛 std::runtime_error
    const T& operator*() const&; const T* operator->() const;
    const Error& error() const&;
    T valueOr(T) const&; T valueOr(T) &&;
    template<F> auto map(F) -> Result<U>;            // T -> U
    template<F> auto andThen(F) -> Result<U>;        // T -> Result<U>
};
```
项目约定**不跨 API 边界抛异常**，调用者统一消费 `Result::ok()` / `error()`。`value()` 仅在已确认 `ok()` 时调用。

## 6. CPU 特性 CpuFeatures (`CpuFeatures.h/.cpp`)

### 类 `CpuFeatures`（单例 Meyers singleton）
```cpp
enum class SimdLevel { None, SSE2, SSE41, AVX, AVX2, AVX512, NEON };
static CpuFeatures& getInstance();
void detect();                           // 幂等
SimdLevel getSimdLevel() const;
bool hasSSE2/SSE41/AVX/AVX2/FMA/AVX512/NEON() const;
std::string getCpuInfoString() const;   // "Brand | SIMD: AVX2+FMA | Cores: 8P/16L"
```
检测路径：
- `__aarch64__`：直接 `SimdLevel::NEON`，`sysctl machdep.cpu.brand_string`
- x86 Windows：`__cpuid` 检 FMA (CPUID.1:ECX.12) + AVX-512F (CPUID.7:EBX.16)；AVX-512 要求同时有 AVX2
- x86 macOS：依赖 JUCE + `hasFMA_ = hasAVX2_`（保守），`hasAVX512_ = false`
- x86 Linux：同 macOS 保守规则

线程安全：`detect()` 不可并发调用；检测完成后只读取属线程安全。日志经 `AppLogger::info` 输出。

## 7. CPU 预算 CpuBudgetManager (`CpuBudgetManager.h/.cpp`)

### 结构 `BudgetConfig`
```cpp
struct BudgetConfig {
    int totalBudget{4};
    int onnxIntra{1};
    int onnxInter{1};
    bool onnxSequential{true};
    bool allowSpinning{false};
};
```

### 静态方法
| 方法 | 契约 |
|---|---|
| `static int computeTotalBudget(unsigned int hw)` | `max(4, floor(hw * 0.60))`；`hw==0` 按 4 处理 |
| `static BudgetConfig buildConfig(bool gpuMode, unsigned int hw = hardware_concurrency())` | GPU 模式：`onnxIntra=2`；CPU 模式：`onnxIntra=0`（让 ORT 自适应）。始终 `onnxInter=1`、`onnxSequential=true`、`allowSpinning=false` |

使用时机：**初始化时计算一次**，后续不随 playback 状态变化。由 `PluginProcessor` / `ModelFactory` 在创建 ORT session 时读取。

## 8. 加速检测 AccelerationDetector (`AccelerationDetector.h/.cpp`)

### 结构 `GpuDeviceInfo`
```cpp
struct GpuDeviceInfo {
    std::string name;
    size_t dedicatedVideoMemory{0};
    size_t sharedSystemMemory{0};
    size_t vendorId{0};                  // 0x10DE=NVIDIA / 0x1002=AMD / 0x8086=Intel
    size_t deviceId{0};
    uint32_t adapterIndex{0};            // DXGI 枚举顺序
    bool isIntegrated{false};
};
```

### 类 `AccelerationDetector`（单例）
```cpp
enum class AccelBackend { CPU, DirectML, CoreML };
static AccelerationDetector& getInstance();
void detect(bool forceCpu = false);     // 幂等（已检测直接返回）
void reset();                            // 允许重新检测；调用者须保证无并发
void overrideBackend(AccelBackend);     // 由 VocoderFactory 在 DML session 创建失败时回退用
AccelBackend getSelectedBackend() const;
bool isDirectMLAvailable() const;
bool isCoreMLAvailable() const;
std::string getBackendName() const;
std::string getGpuInfoString() const;
std::string getAccelerationReport() const;    // 含 UI 展示用多行文本
int getDirectMLDeviceId() const;              // = dmlAdapterIndex_
const std::vector<GpuDeviceInfo>& getGpuDevices() const;
const GpuDeviceInfo& getSelectedGpu() const;
```

检测策略：
- **macOS**：`coreMLAvailable_ = true` 无条件（EP 兼容性留给 session 创建时捕获）
- **Windows**：DXGI 枚举 → 过滤软件渲染器 + `Microsoft Basic Render Driver` → 按「独显优先 + VRAM 降序」排序选 `gpuDevices_[0]` → `Ort::InitApi()` + `GetExecutionProviderApi("DML", ...)` 验证 DML EP 是否编译进 ORT
- **Linux/other**：恒 `CPU`

线程安全：单例 + 单次初始化约定。`overrideBackend` 是写操作，应由单一持有者（VocoderFactory）调用。

## 9. 时间坐标 TimeCoordinate (`TimeCoordinate.h`)

```cpp
namespace OpenTune::TimeCoordinate {
    constexpr double kRenderSampleRate = 44100.0;  // 单一真相源
    inline double samplesToSeconds(int64_t samples, double sr);
    inline double secondsToSamplesExact(double sec, double sr);   // double
    inline int64_t secondsToSamples(double sec, double sr);        // 截断
    inline int64_t secondsToSamplesFloor(double sec, double sr);
    inline int64_t secondsToSamplesCeil(double sec, double sr);
}
```
所有函数均 `sr <= 0` 时返回 0。

## 10. 本地化 LocalizationManager (`LocalizationManager.h`)

### 枚举 `Language`
`English / Chinese / Japanese / Russian / Spanish / Count` —— `Chinese` 为默认值。

### 工具函数
- `juce::String getLanguageName(Language)` —— 显示名（"English" / "中文" / ...）
- `juce::String getLanguageNativeName(Language)` —— 本地化名

### 观察者接口
```cpp
class LanguageChangeListener {
public:
    virtual void languageChanged(Language) = 0;
};
```

### 类 `LocalizationManager`（单例）
| 方法 | 契约 |
|---|---|
| `getInstance()` | Meyers singleton |
| `bindLanguageState(shared_ptr<LanguageState>)` | 将可观察状态压入绑定栈（支持嵌套作用域），重复绑定同一 state 先移除旧条目再 push |
| `unbindLanguageState(shared_ptr<LanguageState>)` | 从绑定栈移除 |
| `Language resolveLanguage()` | 从栈顶到栈底找首个非过期 weak_ptr 返回其 `language`；全空时 `jassertfalse` 返回 `Chinese` |
| `notifyLanguageChanged(Language)` | 触发所有 `LanguageChangeListener::languageChanged(lang)` |
| `addListener / removeListener` | 内部 `juce::ListenerList` 管理 |

### RAII `ScopedLanguageBinding`
`LocalizationManager::ScopedLanguageBinding`：构造时 bind、析构时 unbind。不可拷贝。

### 翻译入口 `Loc::get`
```cpp
namespace Loc {
    inline juce::String get(Language, const char* key);
    inline juce::String get(const char* key);                    // 使用 resolveLanguage()
    inline juce::String format(pattern, arg0 [, arg1]);          // 替换 {0} / {1}
    template<typename...> juce::String tr(const char* key, ...); // ⚠️ 实际不翻译（直接返回 key）
}
#define LOC(key)       OpenTune::Loc::get(OpenTune::Loc::Keys::key)
#define LOC_KEY(key)   OpenTune::Loc::get(key)
#define LOC_RAW(key)   OpenTune::Loc::get(resolveLanguage(), key)
```

`Loc::Keys::*`：约 130+ `constexpr const char*` 键常量（菜单/工具/对话框/tooltip），见源文件。

## 11. 静音检测 SilentGapDetector (`SilentGapDetector.h/.cpp`)

### 结构 `SilentGap`
```cpp
struct SilentGap {
    int64_t startSample{0};             // 真相源为 sample span
    int64_t endSampleExclusive{0};
    float minLevel_dB{0.0f};
    int64_t sampleCount() const;
    bool isValid() const;
    double startSeconds() const;         // 基于 TimeCoordinate::kRenderSampleRate
    double endSeconds() const;
    int64_t midpointSample() const; double midpointSeconds() const;
    bool containsSample(int64_t) const; bool contains(double sec) const;
};
```

### 结构 `DetectionConfig`
```cpp
struct DetectionConfig {
    float strictThreshold_dB = -40.0f;
    float relaxedTotalThreshold_dB = -30.0f;
    float lowBandThreshold_dB = -40.0f;
    double highPassCutoffHz = 60.0;
    double lowBandUpperHz = 3000.0;
    double minGapDurationMs = 100.0;
};
```

### 类 `SilentGapDetector`（全静态）
| 方法 | 契约 |
|---|---|
| `static DetectionConfig getConfig()` | 线程安全（`std::mutex`）读取全局配置 |
| `static void setConfig(DetectionConfig)` | 内部 `sanitizeConfig`（限幅 + 保证 relaxed 不比 strict 更严、lowBand 上限高于高通截止） |
| `static void resetConfig()` | 恢复默认 |
| `static std::vector<SilentGap> detectAllGaps(buffer, threshold_dB=NaN)` | 全量检测（传 NaN 用全局配置）。要求 44.1kHz |
| `static std::vector<SilentGap> detectAllGapsAdaptive(buffer, maxSearchSec=-1)` | 使用频域约束（见 ⚠️ 当前实现） |
| `static std::optional<SilentGap> findNearestGap(gaps, posSec, maxDistSec, forward)` | 线性/二分查找最近静息 |
| `static std::optional<SilentGap> findGapContaining(gaps, posSec)` | 查找包含该位置的静息 |
| `static bool hasGapBetween(gaps, start, end)` / `getGapsBetween(...)` | 区间查询 |
| `static float linearToDb(float)` / `dbToLinear(float)` | 便捷工具 |

常量：`kDefaultThreshold_dB=-40 / kRelaxedTotalThreshold_dB=-30 / kLowBandThreshold_dB=-40 / kHighPassCutoffHz=60 / kLowBandUpperHz=3000 / kMinGapDurationMs=100 / kInternalSampleRate=44100 / kMaxSearchDistanceSec=20`。

## 12. 模型路径 ModelPathResolver (`ModelPathResolver.h`)

全静态接口：
| 方法 | 契约 |
|---|---|
| `static bool ensureOnnxRuntimeLoaded()` | Windows：已加载返回 true；否则按 `Program Files/OpenTune/` → `ProgramData/OpenTune/` → `当前模块目录/` 顺序 `LoadLibraryExW(.., LOAD_WITH_ALTERED_SEARCH_PATH)`。非 Windows 恒返回 true |
| `static std::string getModelsDirectory()` | 按以下顺序寻找 `models/` 目录：Program Files/OpenTune/models → ProgramData/OpenTune/models → 模块同级目录 → 模块上级的 `Resources/models` → 注册表 `HKLM\Software\MakediffVST\OpenTune\ModelsPath` → CWD → 可执行文件同级。找不到返回 `"./models"` |

## 13. 快捷键配置 KeyShortcutConfig (`KeyShortcutConfig.h`)

### 枚举 `ShortcutId`
`PlayPause, Stop, PlayFromStart, Undo, Redo, Cut, Copy, Paste, SelectAll, Delete, Count`（10 项 + 哨兵）。

### 结构
```cpp
struct KeyBinding { int keyCode; juce::ModifierKeys modifiers;
    bool operator==(...) const;    // Ctrl/Cmd 视为等价
};
struct ShortcutInfo { ShortcutId id; const char* displayNameKey; std::vector<KeyBinding> defaultBindings; };
struct ShortcutBinding {
    std::vector<KeyBinding> bindings;
    void addBinding(const KeyBinding&);      // 去重
    void removeBinding(const KeyBinding&);
    bool hasBinding(const KeyBinding&) const;
    juce::String getDisplayNames() const;     // macOS ⌃⌥⇧⌘ 符号，其他平台 "Ctrl+Shift+..."
};
struct KeyShortcutSettings {
    std::array<ShortcutBinding, ShortcutId::Count> bindings;
    KeyShortcutSettings();                    // 用 kShortcutInfos 填充默认
    static KeyShortcutSettings getDefault();
};
```

### 全局数据与工具函数（均 `inline`）
- `const ShortcutInfo kShortcutInfos[]` / `const size_t kShortcutCount` —— 内置默认绑定表
- `juce::String getShortcutDisplayName(ShortcutId)` —— 通过 `Loc::get`
- `const ShortcutInfo& getShortcutInfo(ShortcutId)`
- `const ShortcutBinding& getShortcutBinding(const KeyShortcutSettings&, ShortcutId)`
- `juce::String toCanonicalString(const KeyBinding&)` / `toCanonicalString(const ShortcutBinding&)` —— 用 `|` 连接多绑定
- `bool parseKeyBinding(text, out)` / `bool parseShortcutBinding(text, out)` —— 从规范字符串反序列化
- `bool matchesShortcut(settings, id, juce::KeyPress)` —— 运行时匹配
- `void setShortcutBinding(settings, id, KeyBinding)` —— 清空后只保留该绑定
- `void resetShortcutBinding(settings, id)` —— 恢复默认绑定
- `void resetAllShortcutBindings(settings)`
- `ShortcutId findConflictingShortcut(settings, excludeId, binding)` —— 返回冲突项或 `ShortcutId::Count` 表示无冲突

## 14. 缩放灵敏度 ZoomSensitivityConfig (`ZoomSensitivityConfig.h`)

```cpp
namespace OpenTune::ZoomSensitivityConfig {
    constexpr float kDefaultHorizontalZoomFactor = 0.35f;
    constexpr float kMinHorizontalZoomFactor = 0.1f, kMaxHorizontalZoomFactor = 1.0f;
    constexpr float kDefaultVerticalZoomFactor = 0.35f;
    constexpr float kMinVerticalZoomFactor = 0.1f, kMaxVerticalZoomFactor = 1.0f;
    constexpr float kDefaultScrollSpeed = 90.0f;
    constexpr float kMinScrollSpeed = 30.0f, kMaxScrollSpeed = 300.0f;
    struct ZoomSensitivitySettings {
        float horizontalZoomFactor = 0.35f;
        float verticalZoomFactor = 0.35f;
        float scrollSpeed = 90.0f;
        static ZoomSensitivitySettings getDefault();
    };
}
```

## 15. 音高控制配置 PitchControlConfig (`PitchControlConfig.h`)

```cpp
namespace OpenTune::PitchControlConfig {
    constexpr float kDefaultRetuneSpeedPercent = 15.0f;
    constexpr float kDefaultRetuneSpeedNormalized = 0.15f;
    constexpr float kDefaultVibratoDepth = 0.0f;
    constexpr float kDefaultVibratoRateHz = 7.5f;
    constexpr float kDefaultNoteSplitCents = 80.0f;
    constexpr float kMinNoteSplitCents = 0.0f, kMaxNoteSplitCents = 200.0f;
}
```
纯常量定义，不提供结构体。

## 16. 鼠标轨迹 MouseTrailConfig (`MouseTrailConfig.h`)

```cpp
namespace OpenTune::MouseTrailConfig {
    enum class TrailTheme { None, Classic, Neon, Fire, Ocean, Galaxy, CherryBlossom, Matrix };
    inline bool isEnabled(TrailTheme t);     // t != None
    struct TrailThemeStyle {
        juce::Colour baseColor, accentColor;
        float thickness;   // 像素
        float fadeSpeed;   // 每帧衰减
        bool useGradient;
        float hueShift;    // 0~1 色相偏移
    };
    inline TrailThemeStyle getThemeStyle(TrailTheme);    // 8 分支 switch
}
```

## 17. 平台 shim（Windows-only）

### `WindowsDllSearchPath.cpp`
- 文件作用域静态对象 `WindowsDllSearchPathInitializer` 在 `main` 前执行
- 调用 `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS)`
- `AddDllDirectory(<当前模块目录>)`
- 不提供任何外部符号

### `OnnxRuntimeDelayLoadHook.cpp`
- 导出 `extern "C" const PfnDliHook __pfnDliNotifyHook2 = OpenTune::onnxRuntimeDelayLoadHook;`
- 钩子仅处理 `dliNotePreLoadLibrary + "onnxruntime.dll"`
- 候选顺序：**模块目录** > `ProgramW6432/OpenTune/` > `ProgramData/OpenTune/`
- 依赖 MSVC `delayimp.h`，非 MSVC 构建文件为空

### `D3D12AgilityBootstrap.cpp`
- 导出两个 DirectX 12 Agility SDK 要求的全局符号：
  - `__declspec(dllexport) const unsigned int D3D12SDKVersion = OPENTUNE_D3D12_AGILITY_SDK_VERSION;`（由 CMake 注入）
  - `__declspec(dllexport) const char* D3D12SDKPath = ".\\D3D12\\";`
- 仅 Windows

## ⚠️ 待确认

1. `AppLogger::log()` 内部会调用 `initialize()`（lazy），但 `initialize()` 本身不是严格幂等（double checked 仅靠 `logger != nullptr`，无内存屏障保证）—— 高并发下是否可能重复初始化需确认
2. `LockFreeQueue::size()` 在并发下短暂返回 `(enq - deq)` 可能略大/略小真实值 —— 是否仅用于监控/断言需确认
3. `PresetManager::captureCurrentState` / `applyPreset` 只处理 `zoomLevel/bpm`，但 `PresetData` 有 12 个字段 —— 是已知 TODO 还是刻意只暴露部分
4. `Loc::tr()` 模板实现实际**返回 key 而非翻译**（与 `Loc::get` 行为不一致）—— 确认是占位实现还是本就预期
5. `AccelerationDetector::reset()` 文档写「调用者必须确保无并发」但未加同步，实际只在 UI 设置页"恢复默认"时调用 —— 确认是否足够
6. `KeyShortcutConfig::parseKeyBinding` 未处理数字键以外的符号键（`-`, `=`, `[`, `]` 等），也不支持功能键（F1-F12）—— 是否刻意限制
