---
spec_version: 1.0.0
status: draft
doc_type: constitution
generated_by: orchestrator-agent
generated_at: 2026-04-10
last_updated: 2026-04-10
---

# 项目宪法

## 项目概述

OpenTune 是一款基于 AI 的开源智能修音独立应用程序，由 DAYA 公司开发。它使用 NSF-HiFiGAN 神经声码器和 RMVPE 音高提取（通过 ONNX Runtime），在保留共振峰的前提下实现人声音高修正，支持极端音高调整而不失真。

## 技术栈

| 维度 | 选型 | 版本 |
|------|------|------|
| 语言 | C++ | C++17 |
| 框架 | JUCE | master (vendored) |
| AI 推理引擎 | ONNX Runtime | 1.24.4 |
| 重采样库 | r8brain-free-src | vendored |
| 构建系统 | CMake | 3.22+ |
| GPU 加速 (Windows) | DirectML | 1.15.4 |
| GPU 加速 (Windows) | DirectX Agility SDK | 1.619.1 |
| GPU 加速 (macOS) | CoreML | (系统内置) |
| 音频格式 | FLAC, OGG Vorbis, MP3 | JUCE 内置 |
| 字体 | HONOR Sans CN Medium | bundled |
| 许可证 | AGPL-3.0 | - |

## 核心原则

### I. 共振峰保留音高修正

与传统 DSP 移调不同，OpenTune 使用神经声码器（NSF-HiFiGAN）在保留原始共振峰的前提下重新合成人声。即使进行极端音高调整，也能保持自然音质，避免"鸭子叫"失真。

### II. 音频线程永不阻塞

音频线程（`processBlock`）中所有数据访问均为非阻塞：`ScopedTryLock` 用于 RenderCache，`std::atomic_load` 用于 PitchCurve 快照（COW 模式），`ScopedNoDenormals` 用于 CPU 安全。任何锁竞争时回退到干信号。

### III. 源码扫描主导的 AI 推理

音高修正算法本身为纯数学/DSP（无 ML 模型）。ML 模型仅用于两个端点：RMVPE 提取 F0（输入端）和 NSF-HiFiGAN 合成音频（输出端）。

### IV. Copy-on-Write 不可变快照

PitchCurve 通过 `shared_ptr<const PitchCurveSnapshot>` + `std::atomic_store/load` 实现 COW 模式，UI 线程写入时创建新快照，音频线程读取时无锁。

## 硬性约束（不可修改）

- 项目代码不抛出异常，仅在 ONNX Runtime 外部 API 边界捕获异常
- 音频线程中禁止内存分配、阻塞锁、系统调用
- 所有 JUCE 模块使用严格引用计数指针（`JUCE_STRICT_REFCOUNTEDPOINTER=1`）
- ONNX Runtime 使用手动初始化模式（`ORT_API_MANUAL_INIT`）
- Windows 上 MSVC 运行时使用静态链接（`/MT`）
- macOS 最低系统版本：12.0
- 音频采样率标准：44100 Hz（内部处理），16000 Hz（RMVPE 推理）

## 构建与部署

| 平台 | 命令 | 说明 |
|------|------|------|
| Windows (Release) | `cmake .. -G "Visual Studio 17 2022" -A x64 && cmake --build . --config Release` | MSVC 2022, x64 |
| macOS (Release) | `cmake .. && cmake --build . --config Release` | arm64 only |
| 输出 (Windows) | `build/OpenTune_artefacts/Release/Standalone/OpenTune.exe` | 自动复制 DLL 和模型 |
| 输出 (macOS) | `build/OpenTune_artefacts/Release/Standalone/OpenTune.app` | 自动嵌入 dylib 和模型 |

## AI 模型文件

| 模型 | 用途 | 路径 | 管理方式 |
|------|------|------|----------|
| RMVPE | F0 音高提取 | `models/rmvpe.onnx` | Git LFS |
| PC-NSF-HiFiGAN | 神经声码器 | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` | Git LFS |

## 编译宏定义

| 宏 | 说明 |
|----|------|
| `JUCE_STRICT_REFCOUNTEDPOINTER=1` | 严格 JUCE 指针安全 |
| `ORT_API_MANUAL_INIT` | ONNX Runtime 手动初始化 |
| `NOMINMAX` / `WIN32_LEAN_AND_MEAN` | Windows 标准定义 |
| `OPENTUNE_VERSION` | 版本号字符串 |
| `OPENTUNE_D3D12_AGILITY_SDK_VERSION` | DirectX Agility SDK 版本 |
| `JUCE_USE_FLAC=1` / `JUCE_USE_OGGVORBIS=1` / `JUCE_USE_MP3AUDIOFORMAT=1` | 音频格式支持 |
