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


## 🧪 测试版说明
这是早期测试版本，可能存在少量 bug，性能也尚未完全优化。欢迎下载试用，并通过 Issues 或 Discussion 提出宝贵意见、功能需求或使用中遇到的问题。我们会根据反馈积极改进。


## 🧠 项目理念
AI 的存在是为了帮助人，以人为本，带来更好的创作体验。

在 AI 已经深度介入音乐制作的今天，混音和音频编辑却仍常受限于录音质量，耗费大量时间。OpenTune 希望通过开放的技术，让每一位创作者都能更自由地处理人声，把精力放回音乐本身。


## 📥 下载与使用
请前往 [Releases](https://github.com/YuFeng926/OpenTune/releases) 页面下载最新安装包。

- **Windows**：解压后直接运行 `OpenTune.exe`
- **macOS**：打开 `OpenTune.app`
- **VST3 插件**：将 `OpenTune.vst3` 复制到 DAW 的 VST3 目录即可


## 🔨 构建说明

### 环境要求

| 需求 | Windows | macOS |
|------|---------|-------|
| **系统** | Windows 10+ | macOS 12.0+ (Monterey) |
| **架构** | x64 | arm64 (Apple Silicon) |
| **编译器** | Visual Studio 2022 (MSVC 17+) | Xcode 14+ / Apple Clang |
| **CMake** | 3.22+ | 3.22+ |
| **C++ 标准** | C++17 | C++17 |

### 依赖准备

```bash
git clone -b standalone https://github.com/YuFeng926/OpenTune.git
cd OpenTune
```

#### 1. JUCE Framework
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

**Windows** — 下载并解压到 `ThirdParty/`：

| 包 | 链接 | 解压到 |
|----|------|--------|
| ONNX Runtime CPU | [onnxruntime-win-x64-1.24.4.zip](https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-win-x64-1.24.4.zip) | `ThirdParty/onnxruntime-win-x64-1.24.4/` |
| ONNX Runtime DirectML | [Microsoft.ML.OnnxRuntime.DirectML.1.24.4.nupkg](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.24.4) | `ThirdParty/onnxruntime-dml-1.24.4/` |

> **提示：** 将 `.nupkg` 改名为 `.zip` 后解压。DML 包提供内置 DirectML 支持的 `onnxruntime.dll`。

**macOS (Apple Silicon)**：
```bash
cd ThirdParty
curl -L https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-osx-arm64-1.24.4.tgz | tar xz
cd ..
```

#### 5. DirectML & DirectX Agility SDK（仅 Windows）

| 包 | 版本 | 解压到 |
|----|------|--------|
| [Microsoft.AI.DirectML](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4) | 1.15.4 | `ThirdParty/microsoft.ai.directml.1.15.4/` |
| [Microsoft.Direct3D.D3D12](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.1) | 1.619.1 | `ThirdParty/microsoft.direct3d.d3d12.1.619.1/` |

#### 6. AI 模型

| 模型 | 路径 |
|------|------|
| RMVPE (F0 提取) | `models/rmvpe.onnx` |
| PC-NSF HifiGAN (声码器) | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` |

### 构建命令

**Windows (Visual Studio + CMake)**
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**macOS (Xcode + CMake)**
```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

### 构建产物

| 格式 | Windows | macOS |
|------|---------|-------|
| Standalone | `build/OpenTune_artefacts/Release/Standalone/OpenTune.exe` | `build/OpenTune_artefacts/Release/Standalone/OpenTune.app` |
| VST3 | `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` | `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` |


## 🤝 参与贡献
欢迎提交 PR、翻译文档、报告 bug 或提出新功能建议。让我们一起把 OpenTune 打磨得更好。


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
