---
spec_version: 1.0.0
status: draft
module: utils
doc_type: data-model
generated_by: module-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# Utils 模块数据模型

> 本文覆盖 utils 模块内部的数据结构：内存布局、配置结构、单例状态、枚举。所有结构位于 `OpenTune` 命名空间或其嵌套命名空间。

## 1. UndoManager 内部栈

### `UndoManager` 成员布局
```cpp
class UndoManager {
    std::vector<std::unique_ptr<UndoAction>> actions_;  // 线性栈
    int cursor_{0};                                      // 指向"下一个可 redo 的位置"
    static constexpr int maxSize_{500};                  // 历史上限
    bool isPerformingUndoRedo_{false};                   // 递归防护
    ChangeCallback onChange_;                            // std::function<void()>
};
```

### 游标不变量

| 状态 | cursor | `canUndo()` | `canRedo()` |
|---|---:|:---:|:---:|
| 空栈 | 0 | false | false |
| 刚 add 一次 | 1 | true | false |
| 连续 add 到上限 | 500 | true | false |
| 从上限 undo 一次 | 499 | true | true |

`addAction` 执行后 `cursor_ = actions_.size()`（始终指向末尾），无 redo 历史。
`undo` 前自减再取：`actions_[--cursor_]`；`redo` 取后自增：`actions_[cursor_++]`。

### 溢出策略
`actions_.size() > maxSize_`（501）时，`erase(begin)`（丢弃最早的一条），**不增加 cursor**（因为已满，末端是新 push）。

## 2. LockFreeQueue<T> 内存布局

### 组成
```cpp
template<typename T> class LockFreeQueue {
    struct Cell {
        std::atomic<size_t> sequence;    // 序号约定
        T data;
    };
    const size_t capacity_;              // 必须为 2 的幂
    const size_t mask_;                  // capacity_ - 1
    std::vector<Cell> buffer_;           // 大小 = capacity_

    alignas(64) std::atomic<size_t> enqueuePos_;   // 生产者游标（独占 cache line）
    alignas(64) std::atomic<size_t> dequeuePos_;   // 消费者游标（独占 cache line）
};
```

### Vyukov 序号规则
- 初始化：`buffer_[i].sequence = i`
- 入队：生产者要求 `cell.sequence == pos`，写完数据后 `sequence = pos + 1`
- 出队：消费者要求 `cell.sequence == pos + 1`，读完数据后 `sequence = pos + capacity_`（表示此 cell 下一轮可用）

### 并发约束

| 项目 | 值/说明 |
|---|---|
| 模型 | MPMC bounded |
| 阻塞 | 从不（`try_enqueue/try_dequeue` 非阻塞） |
| 容量策略 | 队满/队空返回 `false`，由调用方重试或降级 |
| 内存序 | `sequence.load(acquire)` + `sequence.store(release)`；`pos.cas_weak(relaxed)`（ABA 由 sequence 规避） |
| 错共享防护 | `enqueuePos_` / `dequeuePos_` `alignas(64)`；MSVC 关闭 `4324` 警告 |
| 数据传输 | 入队拷贝 `= item` 或 `std::move(item)`，出队 `= std::move(cell->data)` |

## 3. TimeCoordinate 常量与转换

```cpp
constexpr double kRenderSampleRate = 44100.0;   // 全局唯一采样率
```

所有转换函数为 `inline`：

| 函数 | 实现 | 溢出/边界 |
|---|---|---|
| `samplesToSeconds(s, sr)` | `double(s)/sr` | `sr<=0` 返回 0.0 |
| `secondsToSamplesExact(sec, sr)` | `sec*sr` | `sr<=0` 返回 0.0 |
| `secondsToSamples(sec, sr)` | 向 0 截断 | `sr<=0` 返回 0 |
| `secondsToSamplesFloor(sec, sr)` | `std::floor(sec*sr)` | `sr<=0` 返回 0 |
| `secondsToSamplesCeil(sec, sr)` | `std::ceil(sec*sr)` | `sr<=0` 返回 0 |

## 4. CpuBudgetManager::BudgetConfig 字段

| 字段 | 类型 | 默认 | 计算规则 |
|---|---|---:|---|
| `totalBudget` | int | 4 | `max(4, floor(hw * 0.60))` |
| `onnxIntra` | int | 1 | GPU 模式 2；CPU 模式 0（ORT 自适应） |
| `onnxInter` | int | 1 | 恒为 1（串行 session.Run） |
| `onnxSequential` | bool | true | 恒 true（`ORT_SEQUENTIAL` 执行模式） |
| `allowSpinning` | bool | false | 恒 false（避免与 DAW 线程争抢） |

## 5. CpuFeatures 内部状态

| 字段 | 默认 | 说明 |
|---|---:|---|
| `detected_` | false | 首次 `detect()` 后置 true，避免重复检测 |
| `simdLevel_` | `SSE2` | x86 至少为 SSE2；ARM64 恒为 NEON |
| `hasSSE2_` | true | x64 默认支持 |
| `hasSSE41_ / hasAVX_ / hasAVX2_ / hasFMA_ / hasAVX512_` | false | 检测后置位 |
| `hasNEON_` | false | ARM64 置 true |
| `physicalCores_ / logicalCores_` | 1 | 从 `juce::SystemStats::getNumPhysicalCpus()` / `std::thread::hardware_concurrency()` 取 |
| `cpuBrand_` | "" | `__cpuid` 品牌字符串 / macOS `sysctl machdep.cpu.brand_string` |

## 6. AccelerationDetector 状态

### 枚举 `AccelBackend`
`CPU / DirectML / CoreML`

### 单例字段

| 字段 | 默认 | 说明 |
|---|---:|---|
| `detected_` | false | 检测完成标志 |
| `selectedBackend_` | `CPU` | 最终选择的后端 |
| `directMLAvailable_` | false | DML EP 可用 |
| `coreMLAvailable_` | false | macOS 无条件 true |
| `dmlAdapterIndex_` | 0 | 选中 GPU 的 DXGI 枚举索引 |
| `gpuDevices_` | `{}` | 过滤软件渲染器后的全部 GPU |
| `selectedGpu_` | `{}` | 排序后的 `gpuDevices_[0]` |

### `GpuDeviceInfo` 字段
| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | std::string | UTF-8 GPU 名称 |
| `dedicatedVideoMemory` | size_t | 字节 |
| `sharedSystemMemory` | size_t | 字节 |
| `vendorId` | size_t | DXGI Desc.VendorId（通常 16 位） |
| `deviceId` | size_t | DXGI Desc.DeviceId |
| `adapterIndex` | uint32_t | DXGI 枚举顺序（0 起） |
| `isIntegrated` | bool | 基于厂商 + 显存规则判定 |

### 集成显卡判定规则
- Intel（`VendorId=0x8086`）：排除 Arc 系列（`DeviceId 0x5690-0x56C0`），若 `DedicatedVideoMemory < 512MB` 视为集成
- AMD（`VendorId=0x1002`）：`DedicatedVideoMemory < 1GB && SharedSystemMemory > 0` 视为集成（覆盖 APU/Radeon 760M 等）
- NVIDIA / 其他：无集成判定（保守当独显）

### GPU 排序规则
```cpp
sort([](a, b) {
    if (a.isIntegrated != b.isIntegrated) return !a.isIntegrated;   // 独显在前
    return a.dedicatedVideoMemory > b.dedicatedVideoMemory;          // VRAM 降序
});
```

## 7. SilentGapDetector 配置与数据结构

### `DetectionConfig` 字段

| 字段 | 默认 | sanitize 范围 | 说明 |
|---|---:|---|---|
| `strictThreshold_dB` | -40 | `[-120, 0]` | 严格：总电平低于此直接判静息 |
| `relaxedTotalThreshold_dB` | -30 | `[-120, 0]`，不小于 strict | 放宽：总电平上限 |
| `lowBandThreshold_dB` | -40 | `[-120, 0]` | 放宽：低频带能量上限 |
| `highPassCutoffHz` | 60 | `[1, 500]` | 检测前高通截止 |
| `lowBandUpperHz` | 3000 | `[200, 20000]`，必须 > `highPassCutoffHz` | 低频带上限 |
| `minGapDurationMs` | 100 | `[1, 200]` | 最小静息持续时长 |

### `SilentGap` 字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `startSample` | int64_t | 起始（含） |
| `endSampleExclusive` | int64_t | 终止（不含） |
| `minLevel_dB` | float | 区域最低电平（dB） |

派生量：`sampleCount()` / `midpointSample()` / `startSeconds()` / `endSeconds()` / `midpointSeconds()`。时间永远以 `kRenderSampleRate=44100` 投影。

### 全局配置存储
- 匿名命名空间 `globalConfig()` 返回 `static DetectionConfig` 单例引用
- `configMutex()` 返回 `static std::mutex` 单例引用
- `getConfig/setConfig/resetConfig` 均在 `std::lock_guard` 内执行

## 8. PresetData 全字段表

| 字段 | 类型 | 默认 | XML 键 |
|---|---|---:|---|
| `name` | juce::String | "" | `name` |
| `author` | juce::String | "" | `author` |
| `description` | juce::String | "" | `description` |
| `timestamp` | int64_t | 构造时 `Time::currentTimeMillis()` | `timestamp` |
| `version` | int | 1 | `version` |
| `retuneSpeed` | float | 15.0 | `retuneSpeed` |
| `scaleRoot` | int | 0 | `scaleRoot` |
| `isMinorScale` | bool | false | `isMinorScale` |
| `vibratoDepth` | float | 0.0 | `vibratoDepth` |
| `vibratoRate` | float | 6.0 | `vibratoRate` |
| `showWaveform` | bool | true | `showWaveform` |
| `showLanes` | bool | true | `showLanes` |
| `zoomLevel` | double | 1.0 | `zoomLevel` |
| `bpm` | double | 120.0 | `bpm` |

XML 根元素名：`Preset`；文件扩展名：`.otpreset`。

## 9. Error / Result<T> 内部存储

### `Error`
```cpp
struct Error {
    ErrorCode code{Success};
    std::string message;      // 一般为 errorCodeMessage(code)
    std::string context;      // 调用方补充的上下文
};
```
`fullMessage()` = `context.empty() ? message : (context + ": " + message)`。

### `Result<T>`
- 内部：`std::variant<T, Error>`
- `Result<T>::value()` 在 `holds<Error>` 时抛 `std::runtime_error(fullMessage)`
- `Result<void>` 特化：内部用 `std::optional<Error>`（`nullopt` 表示成功）

### `ErrorCode` 分段

| 段 | 起始 | 用途 |
|---|---:|---|
| Success | 0 | 成功 |
| Model | 100 | 模型加载/推理 |
| Init | 200 | 初始化生命周期 |
| Audio | 300 | 音频输入校验 |
| F0/Vocoder | 400 | 音高/合成 |
| Mel | 500 | Mel 谱配置 |
| General | 600 | 通用（参数/OOM/取消） |
| Unknown | 999 | 兜底 |

## 10. LocalizationManager 状态

| 字段 | 类型 | 说明 |
|---|---|---|
| `languageBindings_` | `std::vector<std::weak_ptr<LanguageState>>` | 绑定栈。`resolveLanguage` 从末尾向前取首个非过期 |
| `listeners_` | `juce::ListenerList<LanguageChangeListener>` | 观察者列表 |

### `LanguageState`
```cpp
struct LanguageState { Language language = Chinese; };
```
通常由 `AppPreferences` 持有，通过 `ScopedLanguageBinding` RAII 绑定到管理器。

### 翻译表结构（`Loc::get` 内嵌）
```cpp
struct Entry { const char* key; const char* en; const char* zh; const char* ja; const char* ru; const char* es; };
static const Entry translations[] = { ... };  // ~130 条
```
`Loc::get(lang, key)` 做线性 `strcmp` 查找，未命中返回 `key` 字符串本身。

## 11. KeyShortcutConfig 数据结构

### `KeyShortcutSettings`
```cpp
struct KeyShortcutSettings {
    std::array<ShortcutBinding, ShortcutId::Count> bindings;   // size = 10
};
```
每个 `ShortcutBinding` 持有一个 `std::vector<KeyBinding>`（0 到 N 绑定）。

### 默认绑定表 `kShortcutInfos`

| ShortcutId | 默认 KeyBinding |
|---|---|
| `PlayPause` | Space |
| `Stop` | Enter |
| `PlayFromStart` | A |
| `Undo` | Cmd/Ctrl+Z |
| `Redo` | Cmd/Ctrl+Y **和** Cmd/Ctrl+Shift+Z |
| `Cut` | Cmd/Ctrl+X |
| `Copy` | Cmd/Ctrl+C |
| `Paste` | Cmd/Ctrl+V |
| `SelectAll` | Cmd/Ctrl+A |
| `Delete` | Delete **和** Backspace |

### `KeyBinding::operator==`
Ctrl 和 Cmd 视为等价（`isCtrlDown() || isCommandDown()` 比较），Shift/Alt 严格比较。

## 12. MouseTrailConfig 主题样式表

| TrailTheme | baseColor | accentColor | thickness | fadeSpeed | useGradient | hueShift |
|---|---|---|---:|---:|:---:|---:|
| None | (默认分支，等同 Classic) | | | | | |
| Classic | 0xFF3B82F6 | 0xFF60A5FA | 2.5 | 0.083 | false | 0.00 |
| Neon | 0xFFFF00FF | 0xFF00FFFF | 3.0 | 0.100 | true | 0.50 |
| Fire | 0xFFFF4500 | 0xFFFFD700 | 3.5 | 0.120 | true | 0.08 |
| Ocean | 0xFF0077BE | 0xFF00CED1 | 2.0 | 0.060 | true | 0.55 |
| Galaxy | 0xFF8B5CF6 | 0xFFEC4899 | 2.8 | 0.090 | true | 0.75 |
| CherryBlossom | 0xFFFFB7C5 | 0xFFFF69B4 | 2.2 | 0.070 | true | 0.92 |
| Matrix | 0xFF00FF00 | 0xFF00FF00 | 1.8 | 0.150 | false | 0.33 |

## 13. 平台 shim 数据

### `WindowsDllSearchPath.cpp`
- `WindowsDllSearchPathInitializer`：唯一文件作用域静态对象，无字段，仅构造函数副作用
- 调用顺序：`SetDefaultDllDirectories(SEARCH_DEFAULT_DIRS | SEARCH_USER_DIRS)` → `AddDllDirectory(moduleDir)`

### `OnnxRuntimeDelayLoadHook.cpp`
- 候选路径顺序（按优先级降序）：
  1. 当前模块目录（VST/exe 同级，部署标准位置）
  2. `%ProgramW6432% | %ProgramFiles% /OpenTune/onnxruntime.dll`
  3. `%ProgramData%/OpenTune/onnxruntime.dll`
- 钩子仅对 `dliNotePreLoadLibrary + "onnxruntime.dll"` 生效
- 全局符号 `extern "C" const PfnDliHook __pfnDliNotifyHook2`（MSVC 链接器识别）

### `D3D12AgilityBootstrap.cpp`
| 符号 | 类型 | 值来源 |
|---|---|---|
| `D3D12SDKVersion` | `const unsigned int` | `OPENTUNE_D3D12_AGILITY_SDK_VERSION`（CMake 宏） |
| `D3D12SDKPath` | `const char*` | `".\\D3D12\\"`（相对路径） |

D3D12 Runtime 在加载时会查找这两个符号决定是否启用 Agility SDK。

## ⚠️ 待确认

1. `UndoManager` 栈上限 500 写在 `constexpr int maxSize_{500}`，对记忆占用影响不大（unique_ptr + 短 action），但无 UI 显示剩余步数 —— 是否需要暴露
2. `LockFreeQueue<T>` 当前使用场景仅 `VocoderRenderScheduler` 内部（生产者=UI、消费者=渲染线程），实际是 SPSC 语义；用 MPMC 实现略有过度设计 —— 是否未来预留
3. `PresetData::version=1` 无迁移逻辑，`fromValueTree` 用默认值填充缺失字段 —— 当字段增加时老预设行为是否正确
4. `SilentGapDetector::DetectionConfig` 全局单例（`configMutex + globalConfig`）非 per-project，多个文档同时打开时配置共享 —— 是否预期
5. `Loc::get` 线性查找 130 条翻译（每次调用 `strcmp`），UI 渲染时每帧可能调用数十次 —— 性能是否已测
6. `KeyShortcutConfig` 的默认 Redo 绑定同时包含 `Cmd+Y` 和 `Cmd+Shift+Z`（多绑定设计），但 `setShortcutBinding` 会先 clear —— 用户改单绑定后另一条丢失是否预期
