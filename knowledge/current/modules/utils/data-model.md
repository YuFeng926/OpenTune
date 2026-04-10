---
module: utils
type: data-model
generated: 2026-04-10
source_scan: true
---

# utils -- Data Model Reference

> **本文档基于源码扫描自动生成，标注 `[待确认]` 处需人工复核。**

---

## 1. 枚举类型

### LogLevel

**文件**: `Source/Utils/AppLogger.h:14`

```cpp
enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };
```

内部使用整数优先级进行级别过滤：`Debug(0) < Info(1) < Warning(2) < Error(3)`。

### ErrorCode

**文件**: `Source/Utils/Error.h:11`

```cpp
enum class ErrorCode : int {
    Success = 0,
    // 模型错误 100-104
    ModelNotFound = 100, ModelLoadFailed = 101, ModelInferenceFailed = 102,
    InvalidModelType = 103, SessionCreationFailed = 104,
    // 初始化错误 200-202
    NotInitialized = 200, AlreadyInitialized = 201, InitializationFailed = 202,
    // 音频输入错误 300-303
    InvalidAudioInput = 300, InvalidSampleRate = 301,
    InvalidAudioLength = 302, AudioTooShort = 303,
    // F0/声码器错误 400-402
    InvalidF0Input = 400, F0ExtractionFailed = 401, VocoderSynthesisFailed = 402,
    // Mel 频谱错误 500-502
    MelConfigInvalid = 500, MelFFTSizeInvalid = 501, MelNotConfigured = 502,
    // 通用错误 600-602
    InvalidParameter = 600, OutOfMemory = 601, OperationCancelled = 602,
    UnknownError = 999
};
```

### OriginalF0State

**文件**: `Source/Utils/ClipSnapshot.h:15`

```cpp
enum class OriginalF0State : uint8_t { NotRequested = 0, Extracting, Ready, Failed };
```

状态机：`NotRequested → Extracting → Ready | Failed`。

### CpuFeatures::SimdLevel

**文件**: `Source/Utils/CpuFeatures.h:25`

```cpp
enum class SimdLevel { None, SSE2, SSE41, AVX, AVX2, AVX512, NEON };
```

### SimdAccelerator::SimdLevel

**文件**: `Source/Utils/SimdAccelerator.h:26`

```cpp
enum class SimdLevel { None, SSE2, AVX, AVX2, AVX512, NEON, Accelerate };
```

注意：与 `CpuFeatures::SimdLevel` 不同，增加了 `Accelerate` 级别（Apple vDSP/vForce）。

### AccelerationDetector::AccelBackend

**文件**: `Source/Utils/AccelerationDetector.h:53`

```cpp
enum class AccelBackend { CPU, DirectML, CoreML };
```

### KeyShortcutConfig::ShortcutId

**文件**: `Source/Utils/KeyShortcutConfig.h:10`

```cpp
enum class ShortcutId {
    PlayPause, Stop, PlayFromStart, Undo, Redo,
    Cut, Copy, Paste, SelectAll, Delete, Count
};
```

### MouseTrailConfig::TrailTheme

**文件**: `Source/Utils/MouseTrailConfig.h:7`

```cpp
enum class TrailTheme { None = 0, Classic, Neon, Fire, Ocean, Galaxy, CherryBlossom, Matrix };
```

### Language

**文件**: `Source/Utils/LocalizationManager.h:9`

```cpp
enum class Language { English = 0, Chinese, Japanese, Russian, Spanish, Count };
```

---

## 2. 核心数据结构

### Error

**文件**: `Source/Utils/Error.h:79`

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `code` | `ErrorCode` | `Success` | 错误码 |
| `message` | `std::string` | `""` | 人可读错误消息 |
| `context` | `std::string` | `""` | 调用上下文 |

### Result<T>

**文件**: `Source/Utils/Error.h:102`

基于 `std::variant<T, Error>` 的 Rust-style Result 类型。支持 `map()` 和 `andThen()` 函数式组合。

特化 `Result<void>` 基于 `std::optional<Error>`。

### PresetData

**文件**: `Source/Utils/PresetManager.h:20`

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `name` | `juce::String` | | 预设名称 |
| `author` | `juce::String` | | 作者 |
| `description` | `juce::String` | | 描述 |
| `timestamp` | `int64_t` | 当前时间 | 创建时间戳 |
| `version` | `int` | 1 | 版本号 |
| `retuneSpeed` | `float` | 15.0f | 重调速度 |
| `scaleRoot` | `int` | 0 | 调式主音 (0=C) |
| `isMinorScale` | `bool` | false | 是否小调 |
| `vibratoDepth` | `float` | 0.0f | 颤音深度 |
| `vibratoRate` | `float` | 6.0f | 颤音速率 |
| `showWaveform` | `bool` | true | 显示波形 |
| `showLanes` | `bool` | true | 显示轨道 |
| `zoomLevel` | `double` | 1.0 | 缩放级别 |
| `bpm` | `double` | 120.0 | BPM |

序列化方式：`juce::ValueTree` → XML。

### SilentGap

**文件**: `Source/Utils/SilentGapDetector.h:36`

| 字段 | 类型 | 说明 |
|---|---|---|
| `startSeconds` | `double` | 起始时间（秒，含） |
| `endSeconds` | `double` | 结束时间（秒，不含） |
| `minLevel_dB` | `float` | 区间最低电平 (dB) |

辅助方法：`length()`, `midpoint()`, `contains(seconds)`。

### SilentGapDetector::DetectionConfig

**文件**: `Source/Utils/SilentGapDetector.h:63`

| 字段 | 类型 | 默认值 | 合法范围 |
|---|---|---|---|
| `strictThreshold_dB` | `float` | -40.0 | [-120, 0] |
| `relaxedTotalThreshold_dB` | `float` | -30.0 | [-120, 0]，不低于 strict |
| `lowBandThreshold_dB` | `float` | -40.0 | [-120, 0] |
| `highPassCutoffHz` | `double` | 60.0 | [1, 500] |
| `lowBandUpperHz` | `double` | 3000.0 | [200, 20000]，大于 highPass |
| `minGapDurationMs` | `double` | 100.0 | [1, 200] |

### ClipSnapshot

**文件**: `Source/Utils/ClipSnapshot.h:22`

| 字段 | 类型 | 说明 |
|---|---|---|
| `audioBuffer` | `shared_ptr<const AudioBuffer<float>>` | 音频数据 |
| `startSeconds` | `double` | 起始位置 |
| `gain` | `float` | 增益 (默认 1.0) |
| `fadeInDuration` / `fadeOutDuration` | `double` | 淡入/淡出时长 |
| `name` | `juce::String` | 片段名称 |
| `colour` | `juce::Colour` | 片段颜色 |
| `pitchCurve` | `shared_ptr<PitchCurve>` | 音高曲线 |
| `originalF0State` | `OriginalF0State` | F0 提取状态 |
| `detectedKey` | `DetectedKey` | 检测到的调式 |
| `renderCache` | `shared_ptr<RenderCache>` | 渲染缓存 |
| `silentGaps` | `vector<SilentGap>` | 预计算的静息处 |

### GpuDeviceInfo

**文件**: `Source/Utils/AccelerationDetector.h:33`

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | `std::string` | GPU 名称 |
| `dedicatedVideoMemory` | `size_t` | 专用显存 (字节) |
| `sharedSystemMemory` | `size_t` | 共享系统内存 (字节) |
| `vendorId` | `size_t` | 厂商 ID (0x10DE=NVIDIA, 0x1002=AMD, 0x8086=Intel) |
| `deviceId` | `size_t` | 设备 ID |
| `adapterIndex` | `uint32_t` | DXGI 枚举索引 |
| `isIntegrated` | `bool` | 是否集成显卡 |

### CpuBudgetManager::BudgetConfig

**文件**: `Source/Utils/CpuBudgetManager.h:10`

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `totalBudget` | `int` | 4 | 总线程预算 |
| `onnxIntra` | `int` | 1 | ONNX 算子内线程数 |
| `onnxInter` | `int` | 1 | ONNX 算子间线程数 |
| `onnxSequential` | `bool` | true | 是否顺序执行 |
| `allowSpinning` | `bool` | false | 是否允许自旋等待 |

### DmlDiagnosticReport / DmlDiagnosticIssue

**文件**: `Source/Utils/DmlRuntimeVerifier.h`

```
DmlDiagnosticReport
├── ok: bool
├── adapterName: string
├── adapterIndex: uint32_t
├── windowsBuild: uint32_t
├── directMLDllPath: string
├── directMLVersion: string
└── issues: vector<DmlDiagnosticIssue>
        ├── stage: string       // 验证阶段标识
        ├── hresult: long       // Windows HRESULT
        ├── detail: string      // 问题描述
        └── remediation: string // 修复建议
```

---

## 3. UndoAction 继承体系

```
UndoAction (abstract)
├── CompoundUndoAction           -- 组合多个子 Action
├── NotesChangeAction            -- 音符编辑
├── CorrectedSegmentsChangeAction -- F0 校正段修改
│   ├── SegmentSnapshot (inner)  -- 校正段快照
│   └── ChangeInfo (inner)       -- 变更元信息
├── ClipSplitAction              -- 片段分割
│   └── SplitResult (inner)
├── ClipGainChangeAction         -- 片段增益
├── ClipMoveAction               -- 片段移动
├── ClipDeleteAction             -- 片段删除
├── ClipCreateAction             -- 导入音频
├── ScaleKeyChangeAction         -- 全局调式变更
├── ClipScaleKeyChangeAction     -- 片段级调式变更
├── TrackMuteAction              -- 轨道静音
├── TrackSoloAction              -- 轨道独奏
├── TrackVolumeAction            -- 轨道音量
└── ClipCrossTrackMoveAction     -- 跨轨道移动
```

### CorrectedSegmentsChangeAction::SegmentSnapshot

| 字段 | 类型 | 说明 |
|---|---|---|
| `startFrame` | `int` | 起始帧 |
| `endFrame` | `int` | 结束帧 |
| `f0Data` | `vector<float>` | 校正后 F0 值 |
| `source` | `CorrectedSegment::Source` | 来源 (NoteBased/HandDraw/LineAnchor) |
| `retuneSpeed` | `float` | 重调速度 |
| `vibratoDepth` | `float` | 颤音深度 |
| `vibratoRate` | `float` | 颤音速率 |

---

## 4. LockFreeQueue 模板参数

```
LockFreeQueue<T>
├── capacity_: const size_t          -- 容量 (必须是 2 的幂)
├── mask_: const size_t              -- capacity - 1 (位掩码)
├── buffer_: vector<Cell>            -- 环形缓冲区
├── enqueuePos_: atomic<size_t>      -- 入队位置 (alignas(64))
└── dequeuePos_: atomic<size_t>      -- 出队位置 (alignas(64))

Cell
├── sequence: atomic<size_t>         -- 序列号 (用于 CAS 协调)
└── data: T                          -- 元素数据
```

---

## 5. 配置结构体

### KeyShortcutConfig::KeyBinding

| 字段 | 类型 | 说明 |
|---|---|---|
| `keyCode` | `int` | JUCE 键码 |
| `modifiers` | `juce::ModifierKeys` | 修饰键 |

比较逻辑：Ctrl 和 Command 视为等价（跨平台兼容）。

### KeyShortcutConfig::ShortcutBinding

多绑定容器，支持同一快捷键功能绑定到多个键组合。

### ZoomSensitivityConfig::ZoomSensitivitySettings

| 字段 | 默认值 | 范围 |
|---|---|---|
| `horizontalZoomFactor` | 0.35 | [0.1, 1.0] |
| `verticalZoomFactor` | 0.35 | [0.1, 1.0] |
| `scrollSpeed` | 90.0 | [30.0, 300.0] |

### MouseTrailConfig::TrailThemeStyle

| 字段 | 类型 | 说明 |
|---|---|---|
| `baseColor` | `juce::Colour` | 基础颜色 |
| `accentColor` | `juce::Colour` | 强调颜色 |
| `thickness` | `float` | 线宽 |
| `fadeSpeed` | `float` | 淡出速度 |
| `useGradient` | `bool` | 是否使用渐变 |
| `hueShift` | `float` | 色相偏移量 |

---

## [待确认]

1. **SegmentSnapshot 与 CorrectedSegment 的字段同步**: `SegmentSnapshot` 是 `CorrectedSegment` 的值拷贝，若 `CorrectedSegment` 新增字段需同步更新。
2. **PresetData 版本迁移**: `version` 字段存在但无迁移逻辑，旧版本预设的兼容性未明确。
3. **GpuDeviceInfo.vendorId/deviceId 类型**: 使用 `size_t` 而非 `uint32_t`，在 32 位平台上可能存在差异。[待确认] 是否有 32 位平台支持意图。
