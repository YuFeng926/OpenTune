<div align="center">

[English](README.en.md) | 中文

## OpenTune – AI 智能修音软件
<img width="1672" height="941" alt="OpenTune主图" src="https://github.com/user-attachments/assets/822c7d23-9e12-4f26-afd7-5ae1587d46c7" />

保留共振峰，移调不失真

OpenTune 是一款基于神经声码器的开源修音工具。与传统 DSP 算法直接移调不同，它使用 NSF-HiFiGAN 声码器在保留原始共振峰的前提下重新生成人声，即使进行极端音高调整，也能保持自然、扎实的音质。

</div>

<div align="center">

## ✨ 亮点特性

<img width="1672" height="941" alt="OpenTune特性" src="https://github.com/user-attachments/assets/37c38b4a-040e-4ce2-96fc-053e46c35d05" />

**Hybrid移调：通过与DSP算法结合，它可在一个八度内实现更自然的移调，在保证小幅度修音拥有第一梯队音质的同时，在大幅度修音时获得业界领先的自然效果。**

共振峰不变：改变音高时尽量不影响音色，一耳朵差距

AI 重合成：基于深度学习的声码器，而非传统移调

类Auto-Tune/Melodyne工作流：手绘、音符、锚点工具一应俱全，助你快速进入心流状态

内置自动检测调式：上手即修，不修也准，修了更准

双格式输出：Standalone 独立运行 + VST3 插件（支持 ARA2 扩展）

GPU 加速推理：Windows DirectML / macOS CoreML

多语言界面：中 / 英 / 日 / 俄 / 西

开源免费：永久免费，社区驱动，持续迭代

</div>

## 🚀 项目进展
- **逐个音符 EQ 处理**：可对每个音符单独施加 EQ，这是强大的音频编辑能力，许多商业软件都无法做到
- AI 引擎：FCPE 音高提取 + PC-NSF HifiGAN 声码器（ONNX Runtime 推理）
- 多音阶校正：Chromatic、Major、Minor、Pentatonic、Dorian、Mixolydian、Harmonic Minor
- 音频格式：WAV、FLAC、OGG、MP3 导入


## 🧠 项目理念
深度学习技术、大中小模型的发展始终是为了帮助人类，以人为本，为音乐人带来更好的创作体验。

在 AI 已经深度介入音乐制作的今天，混音和音频编辑却仍常受限于录音质量，耗费大量时间。OpenTune 希望通过开源的技术，精心设计的交互，让每一位创作者都能更自由地处理人声，把精力放回音乐语言本身。


## 📥 下载与使用
请前往 [Releases](https://github.com/YuFeng926/OpenTune/releases) 页面下载最新安装包。

各平台详细安装步骤（Windows 解压运行、macOS DMG 安装脚本与 Gatekeeper 处理、VST3 安装位置）及运行时文件结构，见 [INSTALL.md](INSTALL.md)。


## 🔨 构建说明
环境要求、三方依赖准备、构建命令、构建产物与常见问题排查，见 [BUILDING.md](BUILDING.md)。


## 🙏 致谢

- **[OpenVPI 团队 / Diffsinger 社区声码器](https://github.com/openvpi/vocoders)** — 高质量的声码器实现，本项目的核心部分。
- **[yxlllc / RMVPE](https://github.com/yxlllc/RMVPE)** — RMVPE 权重，显著提升音高提取的准确性与鲁棒性。
- **[吃土大佬 (CNChTu) / FCPE](https://github.com/CNChTu/FCPE)** — FCPE 音高提取模型，当前版本的 F0 提取引擎。
- **[JUCE 框架](https://github.com/juce-framework/JUCE)** — 跨平台音频框架。
- **[avaneev / r8brain-free-src](https://github.com/avaneev/r8brain-free-src)** — 高效的重采样算法。
- **[Celemony / ARA SDK](https://github.com/Celemony/ARA_SDK)** — ARA2 扩展支持。

各项目均遵循其自身的开源许可协议。感谢以上项目作者与团队的开放共享精神。


## ☕ 支持本项目

本项目由风语 @ DAYA STUDIO 作为主力开发，目前我们正致力于探索声学合成模型的上限和前沿音频编辑工具的开发。
如果 OpenTune 对你的工作有帮助，欢迎请我们喝杯咖啡！

<p align="center">
  <img src="assets/wechat-pay.png" alt="微信支付" width="200" />
  <img src="assets/alipay.png" alt="支付宝" width="200" />
</p>

> 捐赠是自愿的支持，不购买功能优先级，也不影响 Issue 的处理顺序。


## License
AGPL V3.0
