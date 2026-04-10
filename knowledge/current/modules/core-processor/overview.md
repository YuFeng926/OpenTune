---
spec_version: 1.0.0
status: draft
module: core-processor
doc_type: overview
generated_by: module-agent
generated_at: 2026-04-10
last_updated: 2026-04-10
---

# core-processor — 模块总览

## 职责

核心音频处理器模块，负责多轨管理、音频播放、混音输出、音频文件导入/导出、F0 提取服务调度、chunk 渲染管线协调、项目序列化/反序列化、全局 Undo/Redo 管理。是整个 OpenTune 应用的中心枢纽。

## 模块结构

| 类/文件 | 层级 | 职责 |
|---------|------|------|
| `OpenTuneAudioProcessor` (`Source/PluginProcessor.h/.cpp`) | 核心 | JUCE AudioProcessor 子类。多轨道数据管理、实时音频混音、两阶段导入、chunk 渲染调度、音频导出、项目序列化、Undo/Redo |
| `F0ExtractionService` (`Source/Services/F0ExtractionService.h/.cpp`) | 服务层 | 多线程异步 F0 提取任务管理器。Lock-free queue + token 取消机制 |
| `AsyncAudioLoader` (`Source/Audio/AsyncAudioLoader.h`) | 基础设施 | 后台线程异步音频文件加载器。Validity token 防悬空回调 |
| `HostIntegration` (`Source/Host/HostIntegration.h`) | 抽象接口 | 宿主集成 ABC，定义 Standalone/Plugin 两种模式的行为契约 |
| `HostIntegrationStandalone` (`Source/Host/HostIntegrationStandalone.cpp`) | 实现 | Standalone 模式实现。configureInitialState 为空操作，processIfApplicable 返回 false，audioSettingsRequested 打开设备选择对话框 |
| `EditorFactory` (`Source/Editor/EditorFactory.h`) | 工厂 | 编辑器创建工厂接口 |
| `EditorFactoryStandalone` (`Source/Standalone/EditorFactoryStandalone.cpp`) | 实现 | Standalone 模式下创建 OpenTuneAudioProcessorEditor |

## 对外依赖

| 依赖模块/类 | 类型 | 用途 |
|------------|------|------|
| `PitchCurve` / `PitchCurveSnapshot` (Utils) | 数据 | 音高曲线 COW 快照，processBlock 和渲染流程核心依赖 |
| `RenderCache` (Inference) | 数据 | AI 渲染结果缓存，音频线程读取、渲染线程写入 |
| `F0InferenceService` (Inference) | 服务 | F0 推理引擎（RMVPE ONNX），懒初始化 |
| `VocoderDomain` (Inference) | 服务 | Vocoder 推理域（NSF-HiFiGAN），chunk 渲染提交 |
| `ResamplingManager` (DSP) | 工具 | 音频重采样（r8brain），导入和 drySignal 重采样 |
| `MelSpectrogram` (DSP) | 算法 | Log-mel spectrogram 计算，渲染管线使用 |
| `SilentGapDetector` (Utils) | 算法 | 静音间隔检测，用于 chunk 边界计算 |
| `Note` / `NoteSequence` (Utils) | 数据 | 音符数据结构 |
| `ClipSnapshot` (Utils) | 数据 | Clip 快照，用于 Undo/Redo |
| `UndoManager` / `UndoAction` (Utils) | 框架 | 全局 Undo/Redo 管理 |
| `ScaleInference` / `DetectedKey` (DSP) | 数据 | 调性检测结果 |
| `ModelPathResolver` (Utils) | 工具 | ONNX 模型文件路径解析 |
| `AccelerationDetector` (Utils) | 工具 | 硬件加速检测（构造时调用） |
| `TimeCoordinate` (Utils) | 工具 | 时间-采样坐标转换 |
| JUCE Framework | 框架 | AudioProcessor, AudioBuffer, ReadWriteLock, MessageManager 等 |

## 被调用方

| 调用方 | 说明 |
|--------|------|
| `OpenTuneAudioProcessorEditor` (Standalone/PluginEditor) | **主要调用方**。通过 processor 引用调用所有公共 API（轨道管理、播放控制、导入/导出、Undo/Redo） |
| `PianoRollComponent` (UI) | 调用 clip 音符读写、PitchCurve 读写、enqueuePartialRender |
| `MainControlPanel` (UI) | 调用播放控制、轨道控制（Solo/Mute/Volume） |
| JUCE Host / Standalone Wrapper | 调用 prepareToPlay、processBlock、releaseResources、createEditor |
| `createPluginFilter()` | 全局工厂函数，由 JUCE 框架调用创建 processor 实例 |

## 关键约束

1. **音频线程永不阻塞**：processBlock 使用 ScopedReadLock（非独占）+ ScopedTryLock（非阻塞）+ 原子操作，确保零优先级反转。
2. **固定 44100Hz 存储**：所有音频数据和渲染结果以 44100Hz 为基准存储，设备采样率差异通过 drySignalBuffer 和 resampled cache 适配。
3. **两阶段导入**：重计算在后台线程完成，写锁内仅做轻量对象挂载，最小化写锁持有时间。
