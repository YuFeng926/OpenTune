---
module: utils
type: overview
generated: 2026-04-10
source_scan: true
---

# utils -- Module Overview

## 模块概述

`utils` 是 OpenTune 的基础设施工具层，提供跨模块共享的基础能力。本模块不含业务逻辑，仅为上层模块（Inference、DSP、UI、PluginProcessor）提供日志、撤销框架、无锁数据结构、SIMD 加速、GPU 检测、平台适配、配置管理和本地化等基础设施服务。

## 子系统划分

| 子系统 | 核心文件 | 职责 |
|---|---|---|
| 日志 | AppLogger.h/.cpp | 分级日志输出，文件记录 |
| 撤销/重做 | UndoAction.h/.cpp | Command 模式可逆操作体系，12 种具体 Action |
| 无锁队列 | LockFreeQueue.h | MPMC 有界无锁环形队列 |
| 预设管理 | PresetManager.h/.cpp | XML 预设文件的读写和状态捕获 |
| 错误处理 | Error.h | ErrorCode 枚举 + Error 结构 + Result<T> 模板 |
| CPU 检测 | CpuFeatures.h/.cpp | SIMD 指令集和 CPU 核心数检测 |
| SIMD 加速 | SimdAccelerator.h/.cpp | 运行时函数指针分派 (Scalar/AVX/AVX-512/NEON/Accelerate) |
| GPU 检测 | AccelerationDetector.h/.cpp | DirectML/CoreML 后端自动选择 |
| DML 验证 | DmlRuntimeVerifier.h/.cpp | 8 阶段 DirectML 运行时完整性验证 |
| CPU 预算 | CpuBudgetManager.h/.cpp | ONNX Runtime 线程数配置 |
| 静息检测 | SilentGapDetector.h/.cpp | 音频静息处检测与查找 |
| 坐标转换 | TimeCoordinate.h | 采样数/秒数转换，44.1kHz 全局常量 |
| 片段快照 | ClipSnapshot.h | 片段完整状态快照（用于撤销恢复） |
| 模型路径 | ModelPathResolver.h | ONNX Runtime/模型文件路径解析 |
| 本地化 | LocalizationManager.h | 5 语言 UI 翻译（观察者模式通知） |
| 快捷键配置 | KeyShortcutConfig.h | 键盘快捷键绑定管理（支持多绑定/冲突检测） |
| 缩放配置 | ZoomSensitivityConfig.h | 水平/垂直缩放灵敏度和滚动速度 |
| 音高配置 | PitchControlConfig.h | 默认重调速度、颤音、音符分割阈值 |
| 鼠标轨迹 | MouseTrailConfig.h | 8 种鼠标轨迹主题样式 |
| 平台适配 | D3D12AgilityBootstrap.cpp, WindowsDllSearchPath.cpp, OnnxRuntimeDelayLoadHook.cpp | Windows DLL 加载策略 |

## 关键设计决策

1. **全静态日志接口**: `AppLogger` 全部为 `static` 方法，避免依赖注入复杂性，任何位置可直接调用
2. **Command 模式撤销**: 12 种具体 `UndoAction` 子类覆盖音符、F0 校正、片段操作、轨道操作、调式变更，支持 `CompoundUndoAction` 原子组合
3. **运行时 SIMD 分派**: 通过函数指针而非模板特化，避免编译时 ISA 绑定，支持单一二进制文件在不同 CPU 上最优运行
4. **多层 DLL 加载防护**: 全局静态初始化器 + 延迟加载钩子 + 模型路径解析器，三层防护确保 `onnxruntime.dll` 在各种部署场景下都能正确加载
5. **Rust-style Result**: `Result<T>` 基于 `std::variant` 实现，项目约定不抛异常（仅在外部 API 边界捕获），统一使用 `Result` 传播错误

## 文件清单

| 文件 | 行数 | 说明 |
|---|---|---|
| AppLogger.h | 68 | 日志接口 + PerfTimer |
| AppLogger.cpp | 136 | 日志实现 |
| UndoAction.h | 679 | 撤销基类 + 12 种子类 + UndoManager |
| UndoAction.cpp | 251 | 撤销操作实现 |
| LockFreeQueue.h | 156 | MPMC 无锁队列模板 |
| PresetManager.h | 106 | 预设管理接口 |
| PresetManager.cpp | 142 | 预设管理实现 |
| Error.h | 187 | 错误码 + Error + Result<T> |
| CpuFeatures.h | 91 | CPU 特性检测接口 |
| CpuFeatures.cpp | 144 | CPU 特性检测实现 |
| SimdAccelerator.h | 287 | SIMD 加速器接口 |
| SimdAccelerator.cpp | 1019 | SIMD 多平台实现 |
| AccelerationDetector.h | 161 | GPU 加速检测接口 |
| AccelerationDetector.cpp | 385 | GPU 加速检测实现 |
| CpuBudgetManager.h | 23 | CPU 预算接口 |
| CpuBudgetManager.cpp | 32 | CPU 预算实现 |
| DmlRuntimeVerifier.h | 36 | DML 验证接口 |
| DmlRuntimeVerifier.cpp | 457 | DML 验证实现 |
| SilentGapDetector.h | 249 | 静息处检测接口 |
| SilentGapDetector.cpp | 384 | 静息处检测实现 |
| KeyShortcutConfig.h | 363 | 快捷键配置 |
| TimeCoordinate.h | 32 | 坐标转换 |
| ClipSnapshot.h | 37 | 片段快照 |
| LocalizationManager.h | 332 | 本地化管理 + 翻译表 |
| ModelPathResolver.h | 144 | 模型路径解析 |
| ZoomSensitivityConfig.h | 40 | 缩放配置 |
| PitchControlConfig.h | 15 | 音高控制配置 |
| MouseTrailConfig.h | 73 | 鼠标轨迹配置 |
| D3D12AgilityBootstrap.cpp | 14 | D3D12 Agility SDK 引导 |
| WindowsDllSearchPath.cpp | 69 | DLL 搜索路径设置 |
| OnnxRuntimeDelayLoadHook.cpp | 140 | ONNX Runtime 延迟加载钩子 |

**总计**: 31 个文件，约 5,600 行代码。

## Spec 文档索引

| 文档 | 内容 |
|---|---|
| [api.md](./api.md) | 所有公开类/函数的接口契约 |
| [data-model.md](./data-model.md) | 枚举、结构体、继承体系、模板参数 |
| [business.md](./business.md) | 基础设施规约、设计时序、关键方法说明 |
