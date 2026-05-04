<div align="center">

## OpenTune – AI 智能修音软件
<img width="512" alt="1" src="https://github.com/user-attachments/assets/5f018b53-e78c-4eec-a2da-e71b0724bc9b" />


保留共振峰，移调不失真


OpenTune 是一款基于神经声码器的开源修音工具。与传统 DSP 算法直接移调不同，它使用 NSF-HiFiGAN 声码器在保留原始共振峰的前提下重新生成人声，即使进行极端音高调整，也能保持自然、扎实的音质。

</div>

<div align="center">

## ✨ 亮点特性

<img width="512" alt="2" src="https://github.com/user-attachments/assets/55f7136e-fc8b-4576-af0b-2cbeea3c2f6e" />




共振峰不变：改变音高时不影响声音的底色，告别"鸭子叫"或"换人唱"的失真感


大范围移调：支持夸张的音高修正与转调，音质依然清晰稳定


AI 重合成：基于深度学习的声码器，而非传统移调




<img width="512" alt="3" src="https://github.com/user-attachments/assets/d99c088a-46f6-4177-a761-0af737416b41" />




类Auto-Tune工作流：手绘、音符、锚点工具一应俱全，助你快速进入心流状态



<img width="512" alt="4" src="https://github.com/user-attachments/assets/c95636d1-2eb8-4cad-8c2c-61b208adb961" />




内置类Auto-Key自动检测调式：上手即修，不修也准，修了更准



双格式输出：Standalone 独立运行 + VST3 插件（支持 ARA2 扩展）


GPU 加速推理：Windows DirectML / macOS CoreML


多语言界面：中 / 英 / 日 / 俄 / 西


开源免费：永久免费，社区驱动，持续迭代



</div>



## 🖥️ 当前状态
- 平台：Windows + macOS（Standalone 独立运行 + VST3 插件 + ARA2 扩展）
- GPU 加速：DirectML (Windows) / CoreML (macOS)
- 多音阶校正：Chromatic、Major、Minor、Pentatonic、Dorian、Mixolydian、Harmonic Minor
- AI 引擎：RMVPE 音高提取 + PC-NSF HifiGAN 声码器（ONNX Runtime 推理）
- 音频格式：WAV、FLAC、OGG、MP3 导入
- 界面语言：中文 / English / 日本語 / Русский / Español


## 🧪 测试版说明
这是早期测试版本，可能存在少量 bug，性能也尚未完全优化。欢迎下载试用，并通过 Issues 或 Discussion 提出宝贵意见、功能需求或使用中遇到的问题。我们会根据反馈积极改进。

已知限制：
- 首次加载模型时会有短暂延迟（GPU 初始化）
- 超长音频（>10 分钟）的处理时间较长
- VST3 ARA 模式需要宿主支持 ARA2（如 Studio One、Logic Pro）


## 🧠 项目理念
AI 的存在是为了帮助人，以人为本，带来更好的创作体验。

在 AI 已经深度介入音乐制作的今天，混音和音频编辑却仍常受限于录音质量，耗费大量时间。OpenTune 希望通过开放的技术，让每一位创作者都能更自由地处理人声，把精力放回音乐本身。


## 📥 下载与使用
请前往 [Releases](https://github.com/YuFeng926/OpenTune/releases) 页面下载最新安装包。

- **Windows**：解压后直接运行 `OpenTune.exe`（需保持 `models/`、`D3D12/`、DLL 文件与 exe 同目录）
- **macOS**：打开 `OpenTune.app`（模型已内嵌在 app bundle 中）
- **VST3 插件**：将 `OpenTune.vst3` 文件夹完整复制到 DAW 的 VST3 目录（Windows: `C:\Program Files\Common Files\VST3\`，macOS: `/Library/Audio/Plug-Ins/VST3/`）

### 运行时文件结构（Windows）

```
OpenTune/
├── OpenTune.exe
├── onnxruntime.dll          ← ONNX Runtime (内置 DirectML)
├── DirectML.dll             ← DirectML 运行时
├── D3D12/
│   ├── D3D12Core.dll        ← DirectX Agility SDK
│   └── D3D12SDKLayers.dll
├── models/
│   ├── rmvpe.onnx           ← 音高提取模型
│   └── hifigan.onnx         ← 声码器模型
└── docs/
    └── UserGuide.html
```


## 🔨 构建说明

### 环境要求

| 需求 | Windows | macOS |
|------|---------|-------|
| **系统** | Windows 10 1903+ | macOS 12.0+ (Monterey) |
| **架构** | x64 | arm64 (Apple Silicon) |
| **编译器** | Visual Studio 2022 (MSVC 17+) | Xcode 14+ / Apple Clang |
| **CMake** | 3.22+ | 3.22+ |
| **C++ 标准** | C++17 | C++17 |
| **构建系统** | MSBuild (VS Generator) | Xcode |

> ⚠️ **Windows 构建必须使用 Visual Studio Generator + MSBuild**，不支持 Ninja 或其他生成器。

### 依赖准备

```bash
git clone -b standalone https://github.com/YuFeng926/OpenTune.git
cd OpenTune
```

#### 1. JUCE Framework

克隆到项目根目录，文件夹名必须为 `JUCE-master`：

```bash
git clone https://github.com/juce-framework/JUCE.git JUCE-master
```

#### 2. ARA SDK

```bash
cd ThirdParty
git clone --recursive --branch releases/2.2.0 https://github.com/Celemony/ARA_SDK.git ARA_SDK-releases-2.2.0
cd ..
```

#### 3. r8brain Resampler

```bash
cd ThirdParty
git clone https://github.com/avaneev/r8brain-free-src.git r8brain-free-src-master
cd ..
```

#### 4. ONNX Runtime (v1.24.4)

本项目需要 **两个** ONNX Runtime 包（Windows）：CPU 版提供头文件和 `.lib`，DML 版提供运行时 `onnxruntime.dll`（内置 DirectML 支持）。

**Windows** — 下载并解压到 `ThirdParty/`：

| 包 | 链接 | 解压到 |
|----|------|--------|
| ONNX Runtime CPU | [onnxruntime-win-x64-1.24.4.zip](https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-win-x64-1.24.4.zip) | `ThirdParty/onnxruntime-win-x64-1.24.4/` |
| ONNX Runtime DirectML | [Microsoft.ML.OnnxRuntime.DirectML.1.24.4.nupkg](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.24.4) | `ThirdParty/onnxruntime-dml-1.24.4/` |

> **提示：** 将 `.nupkg` 改名为 `.zip` 后解压。

**目录结构说明（关键路径）：**

```
ThirdParty/
├── onnxruntime-win-x64-1.24.4/       ← CPU 版（编译时链接用）
│   ├── include/
│   │   └── onnxruntime_cxx_api.h      ← CMake 检测此文件是否存在
│   └── lib/
│       ├── onnxruntime.lib            ← 链接用
│       └── onnxruntime_providers_shared.dll
│
└── onnxruntime-dml-1.24.4/            ← DML 版（运行时 DLL + DML provider 头文件）
    ├── build/native/include/
    │   └── dml_provider_factory.h     ← DirectML EP 注册头文件
    └── runtimes/win-x64/native/
        └── onnxruntime.dll            ← 运行时使用的 DLL（内置 DML）
```

**为什么需要两个包？**
- **CPU 包** (`onnxruntime-win-x64-1.24.4`)：提供 C++ API 头文件（`onnxruntime_cxx_api.h`）和 `.lib` 导入库用于编译链接。
- **DML 包** (`onnxruntime-dml-1.24.4`)：提供编译了 DirectML Execution Provider 的 `onnxruntime.dll`，运行时通过 delay-load 加载。同时提供 `dml_provider_factory.h` 头文件用于注册 DML EP。

**macOS (Apple Silicon)**：

macOS 只需一个包，CoreML EP 已内置：

```bash
cd ThirdParty
curl -L https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-osx-arm64-1.24.4.tgz | tar xz
cd ..
```

解压后结构：
```
ThirdParty/onnxruntime-osx-arm64-1.24.4/
├── include/
│   └── onnxruntime_cxx_api.h
└── lib/
    └── libonnxruntime.1.24.4.dylib
```

#### 5. DirectML & DirectX Agility SDK（仅 Windows）

这两个 NuGet 包提供 GPU 加速推理所需的 DirectML 运行时和最新版 D3D12 支持。

| 包 | 版本 | 下载 | 解压到 |
|----|------|------|--------|
| Microsoft.AI.DirectML | 1.15.4 | [NuGet](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4) | `ThirdParty/microsoft.ai.directml.1.15.4/` |
| Microsoft.Direct3D.D3D12 | 1.619.1 | [NuGet](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.1) | `ThirdParty/microsoft.direct3d.d3d12.1.619.1/` |

> 下载 `.nupkg` 后改名为 `.zip` 解压。

**CMake 会检测以下关键文件：**

```
ThirdParty/microsoft.ai.directml.1.15.4/
├── include/
│   └── DirectML.h
└── bin/x64-win/
    ├── DirectML.dll
    └── DirectML.lib

ThirdParty/microsoft.direct3d.d3d12.1.619.1/
└── build/native/
    ├── include/
    │   ├── d3d12.h
    │   └── d3dx12/          ← 辅助头文件目录
    └── bin/x64/
        ├── D3D12Core.dll
        └── D3D12SDKLayers.dll
```

#### 6. AI 模型

模型文件需放在项目根目录下的指定位置，构建时会自动复制到输出目录：

| 模型 | 源路径 | 构建后位置 |
|------|--------|-----------|
| RMVPE (F0 提取) | `models/rmvpe.onnx` | `<output>/models/rmvpe.onnx` |
| PC-NSF HifiGAN (声码器) | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` | `<output>/models/hifigan.onnx` |

> 模型文件不包含在 Git 仓库中，请从 [Releases](https://github.com/YuFeng926/OpenTune/releases) 页面下载或联系维护者获取。

### 完整目录结构概览

配置完所有依赖后，项目根目录应如下所示：

```
OpenTune/
├── CMakeLists.txt
├── JUCE-master/                          ← JUCE 框架
├── ThirdParty/
│   ├── ARA_SDK-releases-2.2.0/           ← ARA SDK
│   ├── r8brain-free-src-master/          ← 重采样库
│   ├── onnxruntime-win-x64-1.24.4/      ← ONNX Runtime CPU (Windows)
│   ├── onnxruntime-dml-1.24.4/           ← ONNX Runtime DML (Windows)
│   ├── onnxruntime-osx-arm64-1.24.4/    ← ONNX Runtime (macOS)
│   ├── microsoft.ai.directml.1.15.4/    ← DirectML SDK (Windows)
│   └── microsoft.direct3d.d3d12.1.619.1/ ← D3D12 Agility SDK (Windows)
├── models/
│   └── rmvpe.onnx
├── pc_nsf_hifigan_44.1k_ONNX/
│   └── pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx
├── Source/
├── Resources/
└── docs/
```

### 构建命令

**Windows (Visual Studio + CMake)**

```powershell
# 生成 VS 解决方案（必须使用 "Visual Studio 17 2022" 生成器）
cmake -B build -G "Visual Studio 17 2022" -A x64

# 编译 Release 版本
cmake --build build --config Release
```

如需在 Visual Studio IDE 中开发：
1. 执行上述 `cmake -B build` 命令
2. 打开 `build/OpenTune.sln`
3. 将 `OpenTune_Standalone` 或 `OpenTune_VST3` 设为启动项目
4. 选择 Release/x64 配置，编译运行

**macOS (Xcode + CMake)**

```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

或在 Xcode 中打开 `build/OpenTune.xcodeproj` 进行开发调试。

### 构建产物

| 格式 | Windows | macOS |
|------|---------|-------|
| Standalone | `build/OpenTune_artefacts/Release/Standalone/OpenTune.exe` | `build/OpenTune_artefacts/Release/Standalone/OpenTune.app` |
| VST3 | `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` | `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` |

构建完成后，运行时 DLL、模型文件、D3D12 目录会自动复制到产物目录旁，无需手动操作。

### 常见构建问题

| 症状 | 原因 | 解决方法 |
|------|------|----------|
| `ONNX Runtime C++ API header not found` | CPU 版 ONNX Runtime 未放对位置 | 确认 `ThirdParty/onnxruntime-win-x64-1.24.4/include/onnxruntime_cxx_api.h` 存在 |
| `DirectML provider header missing` | DML 版 NuGet 包未正确解压 | 确认 `ThirdParty/onnxruntime-dml-1.24.4/build/native/include/dml_provider_factory.h` 存在 |
| `DirectML header missing` | DirectML NuGet 包未解压 | 确认 `ThirdParty/microsoft.ai.directml.1.15.4/include/DirectML.h` 存在 |
| `D3D12 header missing from Agility SDK` | D3D12 NuGet 包未解压 | 确认 `ThirdParty/microsoft.direct3d.d3d12.1.619.1/build/native/include/d3d12.h` 存在 |
| `ONNX Runtime DirectML DLL missing` | DML 版运行时 DLL 缺失 | 确认 `ThirdParty/onnxruntime-dml-1.24.4/runtimes/win-x64/native/onnxruntime.dll` 存在 |
| `ARA SDK not found` | ARA SDK 未克隆 | 执行步骤 2 的 git clone 命令 |
| MSVC 链接错误 LNK2019 | MSVC 运行时不匹配 | 本项目使用静态 CRT (`/MT`)，确保依赖库一致 |
| Ninja 生成器报错 | 不支持 Ninja | 只能使用 `"Visual Studio 17 2022"` 生成器 |


## 🤝 参与贡献

欢迎提交 PR、翻译文档、报告 bug 或提出新功能建议。让我们一起把 OpenTune 打磨得更好。

### 开发指引

- **代码风格**：C++17，JUCE 命名规范
- **分支策略**：`main` 为稳定版，`standalone` 为开发分支
- **提交规范**：简洁描述变更目的（中英文均可）
- **UI 隔离**：`Source/Standalone/` 为独立版 UI，`Source/Plugin/` 为 VST3 版 UI，通过 `JucePlugin_Build_Standalone` 宏隔离
- **Processor 共享**：`Source/PluginProcessor.*` 为两种格式共用，修改时需兼顾两个构建目标


## 🙏 致谢

- **[OpenVPI 团队 / Diffsinger 社区声码器](https://github.com/openvpi/vocoders)** — 高质量的声码器实现，本项目的核心部分。
- **[yxlllc / RMVPE](https://github.com/yxlllc/RMVPE)** — RMVPE 权重，显著提升音高提取的准确性与鲁棒性。
- **[吃土大佬 (CNChTu) / FCPE](https://github.com/CNChTu/FCPE)** — 参考了 FCPE，可能后续会尝试实装。
- **[JUCE 框架](https://github.com/juce-framework/JUCE)** — 跨平台音频框架。
- **[avaneev / r8brain-free-src](https://github.com/avaneev/r8brain-free-src)** — 高效的重采样算法。
- **[Celemony / ARA SDK](https://github.com/Celemony/ARA_SDK)** — ARA2 扩展支持。

各项目均遵循其自身的开源许可协议。感谢以上项目作者与团队的开放共享精神。


## License
Copyright © 2024-2026 DAYA. All rights reserved.
