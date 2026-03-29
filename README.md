## OpenTune – AI 智能修音软件
<img width="2211" height="1233" alt="1" src="https://github.com/user-attachments/assets/5f018b53-e78c-4eec-a2da-e71b0724bc9b" />


保留共振峰，移调不失真


OpenTune 是一款基于神经声码器的开源修音工具。与传统 DSP 算法直接移调不同，它使用 NSF-HiFiGAN 声码器在保留原始共振峰的前提下重新生成人声，即使进行极端音高调整，也能保持自然、扎实的音质。

## ✨ 亮点特性

<img width="2571" height="1435" alt="2" src="https://github.com/user-attachments/assets/55f7136e-fc8b-4576-af0b-2cbeea3c2f6e" />


共振峰不变：改变音高时不影响声音的底色，告别“鸭子叫”或“换人唱”的失真感


大范围移调：支持夸张的音高修正与转调，音质依然清晰稳定


AI 重合成：基于深度学习的声码器，而非传统移调




<img width="2048" height="2048" alt="3" src="https://github.com/user-attachments/assets/d99c088a-46f6-4177-a761-0af737416b41" />


类Auto-Tune工作流：手绘、音符、锚点工具一应俱全，助你快速进入心流状态



<img width="2571" height="1435" alt="4" src="https://github.com/user-attachments/assets/c95636d1-2eb8-4cad-8c2c-61b208adb961" />


内置类Auto-Key自动检测调式：上手即修，不修也准，修了更准



开源免费：永久免费，社区驱动，持续迭代



## 🖥️ 当前状态
平台：Windows（Standalone 独立运行版本）

即将推出：VST3 插件、ARA2 支持（可直接在 DAW 中运行）

性能提示：目前 CPU/内存有一定占用，建议配备独立显卡以获得更流畅体验


## 🧪 测试版说明
这是早期测试版本，可能存在少量 bug，性能也尚未完全优化。欢迎下载试用，并通过 Issues 或 Discussion 提出宝贵意见、功能需求或使用中遇到的问题。我们会根据反馈积极改进。


## 🧠 项目理念
AI 的存在是为了帮助人，以人为本，带来更好的创作体验。

在 AI 已经深度介入音乐制作的今天，混音和音频编辑却仍常受限于录音质量，耗费大量时间。OpenTune 希望通过开放的技术，让每一位创作者都能更自由地处理人声，把精力放回音乐本身。


## 🙏 鸣谢
特别感谢 DiffSinger 开源社区及所有贡献者。正是他们在神经声码器与歌声合成领域的持续探索，才为传统音乐制作带来了这份礼物。


## 📥 下载与使用
请前往 Releases 页面下载最新 Windows 独立运行包。
解压后直接运行 .exe 文件，加载音频并设置目标音高即可开始处理。


## Build Instructions
## Build Requirements

- CMake 3.22+
- C++17 compiler (MSVC 2022 recommended on Windows)
- Visual Studio 2022 (Windows)

### Windows (MSVC)

```bash
# Create build directory
mkdir build
cd build

# Generate project files
cmake .. -G "Visual Studio 17 2022" -A x64

# Build
cmake --build . --config Release

# Output: build/OpenTune_artefacts/Release/Standalone/OpenTune.exe
```

### Build Output

After successful build, the Standalone executable will be located at:
```
build/OpenTune_artefacts/Release/Standalone/OpenTune.exe
```

Required DLLs and models will be automatically copied to the same directory.

## Dependencies (Included)

- **JUCE** - Cross-platform audio framework
- **ONNX Runtime 1.17.3** - ML inference engine
- **r8brain-free-src** - High-quality audio resampling library

## AI Models

The application requires two ONNX models (included):
- `models/rmvpe.onnx` - Pitch extraction model
- `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` - Vocoder model


## 🤝 参与贡献
欢迎提交 PR、翻译文档、报告 bug 或提出新功能建议。让我们一起把 OpenTune 打磨得更好。


## 🙏 致谢

- **[OpenVPI 团队 / Diffsinger 社区声码器](https://github.com/openvpi/vocoders)** - 高质量的声码器实现，本项目的核心部分。
- **[yxlllc / RMVPE](https://github.com/yxlllc/RMVPE)** - 使用了大佬训练的 RMVPE 权重，显著提升了音高提取的准确性与鲁棒性。
- **[吃土大佬 (CNChTu) / FCPE](https://github.com/CNChTu/FCPE)** - 参考了 FCPE ，可能后续会尝试实装。
- **[JUCE 框架](https://github.com/juce-framework/JUCE)** - 
- **[avaneev / r8brain-free-src](https://github.com/avaneev/r8brain-free-src)** - 高效的重采样算法。
各项目均遵循其自身的开源许可协议。
感谢以上项目作者与团队的开放共享精神，他们的工作让本项目的实现成为可能。


## License
AGPL v3.0

