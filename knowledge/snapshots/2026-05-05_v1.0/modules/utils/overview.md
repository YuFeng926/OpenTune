---
spec_version: 1.0.0
status: draft
module: utils
doc_type: overview
generated_by: module-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# Utils 模块概览

## 定位

`Source/Utils/` 是 OpenTune 的**基础设施工具层**，为上层所有模块（Inference / DSP / PitchCorrection / CoreProcessor / UI / ARA-VST3）提供通用基础能力。本层**不含业务逻辑**，所有类型与函数均为被动式服务：

- 横向被几乎所有模块直接包含，处于依赖金字塔底层
- 下游依赖：JUCE（juce_core / juce_audio_basics / juce_gui_basics / juce_dsp / juce_data_structures）、ONNX Runtime（仅 `AccelerationDetector` / `ModelPathResolver`）、平台 API（Win32、macOS sysctl、DXGI、D3D12、DirectML）
- 上游调用者：Processor / Inference / UI / ARA-VST3 等几乎全部业务模块

> 与 v1.2 相比，v1.3 做了重大简化：
> - 新增 `UndoManager.{h,cpp}`，以单一线性栈 + `UndoAction` 抽象基类取代旧 `UndoAction.{h,cpp}` 的 12 种具体子类和 `PianoRollUndoSupport`
> - 移除 `ClipSnapshot.h`（由 `Utils/PlacementActions.h` 等业务级撤销载荷接管）
> - 移除 `SimdAccelerator.{h,cpp}`（SIMD 运行时分派）与 `DmlRuntimeVerifier.{h,cpp}`（DML 8 阶段验证）—— 相关能力下沉到调用方或已由 ORT 自身的 EP 机制替代
> - `CpuBudgetManager` 大幅精简：仅保留 `totalBudget / onnxIntra / onnxInter / onnxSequential / allowSpinning` 五字段，线程预算在初始化时按 `gpuMode` 固定

## 模块边界

| 方向 | 内容 |
|---|---|
| 上游调用者 | `PluginProcessor`（AppLogger 初始化、AccelerationDetector、CpuBudgetManager、PresetManager）、`Inference/*`（ModelPathResolver、CpuBudgetManager、AccelerationDetector、LockFreeQueue）、`Standalone/UI/*` + `Plugin/*`（UndoManager、Loc、KeyShortcutConfig、ZoomSensitivityConfig、MouseTrailConfig、SilentGapDetector）、`SourceStore` / `MaterializationStore` / `StandaloneArrangement`（UndoManager） |
| 下游依赖 | JUCE 框架（日志/线程/XML/GUI/DSP）、ONNX Runtime C++ API、Windows SDK（dxgi/d3d12/delayimp）、Apple sysctl |
| 不应依赖 | 业务层：`Source/Inference/*`、`Source/DSP/*`、`Source/UI/*`、`Source/PianoRoll/*`、`Source/PluginProcessor.{h,cpp}` 均不应被 utils 反向依赖。唯一例外是 `PresetManager.h` 当前 `#include "../PluginProcessor.h"`（反向耦合，已在 `⚠️ 待确认` 标注） |

## 子系统一览（26 个文件）

| 子系统 | 核心文件 | 文件数 | 职责 |
|---|---|---:|---|
| 日志 | `AppLogger.h/.cpp` | 2 | 4 级日志、JUCE FileLogger 包装、日期分文件 |
| 撤销/重做 | `UndoManager.h/.cpp` | 2 | 线性 Undo 栈（上限 500 层）、`UndoAction` 抽象基类 |
| 无锁队列 | `LockFreeQueue.h` | 1 | MPMC 有界环形队列模板（bounded MPMC） |
| 预设 | `PresetManager.h/.cpp` | 2 | `.otpreset` XML 预设读写 |
| 错误 | `Error.h` | 1 | `ErrorCode` / `Error` / `Result<T>` / `Result<void>` |
| CPU 特性 | `CpuFeatures.h/.cpp` | 2 | SIMD 指令集 + 核心数检测（单例） |
| GPU 加速检测 | `AccelerationDetector.h/.cpp` | 2 | DXGI GPU 枚举 + ORT DML EP 可用性 + CoreML 可用性 |
| CPU 预算 | `CpuBudgetManager.h/.cpp` | 2 | ORT `intra/inter_op_num_threads` 计算 |
| 静音检测 | `SilentGapDetector.h/.cpp` | 2 | 静息处检测 + 频域约束 + 区间查询 |
| 时间坐标 | `TimeCoordinate.h` | 1 | `kRenderSampleRate=44100.0` 单一真相、秒↔样本转换 |
| 本地化 | `LocalizationManager.h` | 1 | 5 语言翻译表 + 观察者 + `ScopedLanguageBinding` |
| 模型路径 | `ModelPathResolver.h` | 1 | ONNX 运行时 DLL 主动加载 + 模型目录解析 |
| 快捷键配置 | `KeyShortcutConfig.h` | 1 | 10 种 `ShortcutId`、多绑定、冲突检测、规范字符串 |
| 缩放灵敏度 | `ZoomSensitivityConfig.h` | 1 | 水平/垂直缩放因子、滚动速度常量 |
| 音高控制 | `PitchControlConfig.h` | 1 | 默认 retune speed / vibrato / noteSplit 阈值 |
| 鼠标轨迹 | `MouseTrailConfig.h` | 1 | 8 种主题样式（颜色/粗细/褪色速度） |
| 平台 shim（Windows） | `D3D12AgilityBootstrap.cpp` / `WindowsDllSearchPath.cpp` / `OnnxRuntimeDelayLoadHook.cpp` | 3 | D3D12 Agility SDK 导出、DLL 搜索路径补丁、ONNX 延迟加载钩子 |

**合计**：26 个文件（arch_layers 声明），其中 6 个 `.cpp` + 20 个 `.h`。

## 关键约束

1. **全局单例多**：`AppLogger`（静态）、`CpuFeatures::getInstance()`、`AccelerationDetector::getInstance()`、`LocalizationManager::getInstance()` 均为 Meyers singleton。初始化顺序由 Processor 控制：先 `AppLogger::initialize()` → `CpuFeatures::detect()` → `AccelerationDetector::detect()`。
2. **线程安全契约差异**：`AppLogger` 内部用 `juce::CriticalSection` 保护；`SilentGapDetector::getConfig/setConfig` 用 `std::mutex`；`LockFreeQueue` 使用 `std::atomic` + cache-line 对齐；`UndoManager` **无锁**（仅 message thread 调用）；`PresetManager` 无同步保护。
3. **Header-only 偏好**：多数 config 类（`KeyShortcutConfig` / `ZoomSensitivityConfig` / `PitchControlConfig` / `MouseTrailConfig` / `TimeCoordinate` / `Error` / `LocalizationManager` / `ModelPathResolver`）全部实现在 `.h`，仅 `inline` 函数或 `constexpr` 常量。
4. **平台条件编译**：`WindowsDllSearchPath.cpp` / `OnnxRuntimeDelayLoadHook.cpp` / `D3D12AgilityBootstrap.cpp` 全文件用 `#if defined(_WIN32)` 包裹，非 Windows 平台编译为空翻译单元。`CpuFeatures` 在 `__aarch64__` 分支直接设 `SimdLevel::NEON`。
5. **采样率单一真相**：`TimeCoordinate::kRenderSampleRate = 44100.0` 是全项目内部音频的**唯一采样率常量**，`SilentGapDetector::kInternalSampleRate` / `RenderCache::kSampleRate` / `AudioConstants::StoredAudioSampleRate` 均引用此值。
6. **DLL 加载三重防护（Windows）**：
   - `WindowsDllSearchPath.cpp` 全局 static initializer：通过 `SetDefaultDllDirectories` + `AddDllDirectory` 把模块目录加入搜索
   - `OnnxRuntimeDelayLoadHook.cpp` 延迟加载钩子 `__pfnDliNotifyHook2`：在 ORT 首次被触发加载时拦截，按 module dir → Program Files → ProgramData 顺序尝试
   - `ModelPathResolver::ensureOnnxRuntimeLoaded()` 运行时主动调用：作为最后保险
7. **`UndoManager` v1.3 重构**：旧 `UndoAction.h` 里的 `CompoundUndoAction` / 12 种具体 Action 不再在 utils 层；新 `UndoAction` 是纯虚基类，业务方（`PianoRollEditAction` / `PlacementActions`）自行实现子类后 `addAction`。栈上限 500 层，溢出时丢弃最早一条。

## Spec 文档索引

| 文档 | 内容摘要 |
|---|---|
| [api.md](./api.md) | 各子系统公共接口契约（按子系统分组） |
| [data-model.md](./data-model.md) | 配置 struct 字段表、LockFreeQueue 内存布局、UndoManager 栈结构、TimeCoordinate 常量、CpuBudgetManager 状态字段 |
| [business.md](./business.md) | 基础设施使用约定、线程安全契约、平台差异点、加载顺序、时序图 |

## ⚠️ 待确认

1. `PresetManager.h` 反向 `#include "../PluginProcessor.h"` 违反分层原则，`captureCurrentState/applyPreset` 只读写 `zoomLevel/bpm` 两个字段 —— 需确认这是过渡状态还是最终设计
2. `SimdAccelerator.{h,cpp}` / `DmlRuntimeVerifier.{h,cpp}` / `ClipSnapshot.h` 在 v1.2 存在，v1.3 被移除；替换路径在代码中未留注释（尤其 SIMD 相关被哪些地方替代需确认）
3. `UndoManager` 的 `maxSize_ = 500` 硬编码在类内 `constexpr`，无运行时配置入口 —— 确认是产品决策还是可放宽
