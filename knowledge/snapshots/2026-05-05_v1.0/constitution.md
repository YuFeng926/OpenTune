---
spec_version: 1.0.0
status: draft
doc_type: constitution
generated_by: orchestrator-agent
generated_at: 2026-05-05
last_updated: 2026-05-05
---

# 项目宪法

## 项目概述

OpenTune 是一款基于 AI 的开源智能修音应用，由 DAYA 公司开发。自 v1.3 起同时以 **Standalone** 与 **VST3（含 ARA 扩展）** 双格式提供：既可作为独立应用运行，也能作为 ARA 插件内嵌于宿主 DAW。核心使用 NSF-HiFiGAN 神经声码器 + RMVPE 音高提取（ONNX Runtime），在保留共振峰前提下实现人声音高修正，支持极端音高调整而不失真。

## 技术栈

| 维度 | 选型 | 版本 |
|------|------|------|
| 语言 | C++ | C++17 |
| 应用框架 | JUCE | master（vendored，位于 `JUCE-master/`） |
| 插件 SDK | ARA SDK | 2.2.0（vendored，位于 `ThirdParty/ARA_SDK-releases-2.2.0`） |
| AI 推理引擎 | ONNX Runtime | 1.24.4（macOS arm64 + Windows x64+DML 双包） |
| 重采样库 | r8brain-free-src | vendored |
| 构建系统 | CMake | 3.22+ |
| GPU 加速（Windows） | DirectML | 1.15.4 |
| GPU 加速（Windows） | DirectX Agility SDK | 1.619.1 |
| GPU 加速（macOS） | CoreML | 系统内置 |
| 音频格式 | FLAC / OGG Vorbis / MP3 / WAV | JUCE 内置 |
| 字体 | HONOR Sans CN Medium | bundled |
| 许可证 | AGPL-3.0 | - |

## 核心原则

### I. 共振峰保留的神经声码修音

不依赖传统 DSP 移调。RMVPE 提取 F0 → 用户在钢琴卷帘上修正曲线 → NSF-HiFiGAN 按修正曲线重新合成音频。极端音高调整下仍保持自然共振峰（避免"鸭子叫"）。

### II. 音频线程永不阻塞

`processBlock` 中的所有数据访问必须非阻塞：`ScopedTryLock`（RenderCache 读取）、`std::atomic_load`（PitchCurve COW 快照）、`ScopedNoDenormals`。任何潜在锁竞争立即回退到干信号输出。

### III. Copy-on-Write 不可变快照

`PitchCurve` 通过 `shared_ptr<const PitchCurveSnapshot>` + `std::atomic_store/load` 实现 COW：UI 线程改动创建新快照，音频线程/渲染线程无锁读。

### IV. Dual-Format Seam

Standalone 与 VST3/ARA 共享同一 `PluginProcessor` 内核，差异通过 `Source/Editor/EditorFactory.h` 工厂接口隔离。Standalone 走 `EditorFactoryStandalone.cpp`，VST3 插件走 `EditorFactoryPlugin.cpp`。ARA 访问通过 `Source/ARA/OpenTuneDocumentController` + `OpenTunePlaybackRenderer` + `VST3AraSession` 桥接。

### V. Materialization Pipeline（v1.3+ 新引入）

音频导入后经 `MaterializationStore` 存储原始 PCM，`SourceStore` 管理源窗口，`StandaloneArrangement` 在离线线程上 materialize（渲染）修正结果；播放线程消费已 materialized 的 snapshot region。统一失效机制（unified invalidation）使 UI 编辑→重新 materialize→UI 感知形成单向闭环。

### VI. 源码扫描主导（无 HyperWiki 依赖）

本项目无内部文档系统。知识库完全来自源码（Glob/Grep/Read）。缺失信息标注 `⚠️ 待补充`，不推断。

## 硬性约束（不可修改）

- 项目代码**不抛异常**；仅在 ONNX Runtime / JUCE 外部 API 边界捕获异常
- 音频线程中**禁止**内存分配、阻塞锁、系统调用
- 所有 JUCE 模块启用严格引用计数指针：`JUCE_STRICT_REFCOUNTEDPOINTER=1`
- ONNX Runtime 使用手动初始化：`ORT_API_MANUAL_INIT`
- Windows MSVC 运行时静态链接：`/MT` / `/MTd`
- macOS 最低系统版本：12.0（Monterey）
- 采样率标准：内部 44100 Hz；RMVPE 推理 16000 Hz
- 共用 `EditorFactory` 接口：Standalone 与 VST3 各自实现一份 `.cpp`，不允许任一方直接引用对方符号

## 构建与部署

| 平台 | 命令 | 产物 |
|------|------|------|
| Windows Release | `cmake .. -G "Visual Studio 17 2022" -A x64 && cmake --build . --config Release` | `build/OpenTune_artefacts/Release/Standalone/OpenTune.exe` + `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3` |
| macOS Release (arm64) | `cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build .` | `build/OpenTune_artefacts/Release/Standalone/OpenTune.app` + `.../VST3/OpenTune.vst3` |
| macOS 签名打包 | `scripts/sign-and-package.sh` | `OpenTune-<ver>-arm64.dmg`（含 Install 命令脚本） |

## AI 模型文件（Git LFS 管理）

| 模型 | 用途 | 路径 |
|------|------|------|
| RMVPE | F0 音高提取 | `models/rmvpe.onnx` |
| PC-NSF-HiFiGAN | 神经声码器 | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` |

## 关键编译宏

| 宏 | 说明 |
|----|------|
| `JUCE_STRICT_REFCOUNTEDPOINTER=1` | 严格 JUCE 指针安全 |
| `JUCE_VST3_CAN_REPLACE_VST2=0` | 不兼容 VST2 替换 |
| `ORT_API_MANUAL_INIT` | 手动初始化 ONNX Runtime（延迟加载） |
| `ARA_ENABLE_DRAFT_NO_DEBUG` | ARA 非调试编译 |

## ThirdParty 依赖根目录

- `JUCE-master/` — 应用框架源码（vendored）
- `ThirdParty/ARA_SDK-releases-2.2.0/` — ARA SDK
- `onnxruntime-osx-arm64-1.24.4/` — macOS arm64 运行时
- `onnxruntime-win-x64-1.24.4/` + `onnxruntime-dml-1.24.4/` — Windows x64 双包
- `r8brain-free-src`（随 JUCE 编译） — 高质量重采样
