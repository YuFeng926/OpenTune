<div align="center">

[中文](README.md) | English

## OpenTune – AI Pitch Correction Software
<img width="1672" height="941" alt="OpenTune main image" src="https://github.com/user-attachments/assets/822c7d23-9e12-4f26-afd7-5ae1587d46c7" />

Preserving formants, pitch shifting without distortion

OpenTune is an open-source pitch correction tool based on neural vocoders. Unlike traditional DSP algorithms that directly shift pitch, it uses the NSF-HiFiGAN vocoder to regenerate vocals while preserving the original formants, maintaining natural, solid sound quality even with extreme pitch adjustments.

</div>

<div align="center">

## ✨ Key Features

<img width="1672" height="941" alt="OpenTune features" src="https://github.com/user-attachments/assets/37c38b4a-040e-4ce2-96fc-053e46c35d05" />

Formant preservation: Changing pitch doesn't affect the voice's tonal color, eliminating "duck voice" or "different singer" distortion

Wide-range pitch shifting: Supports extreme pitch correction and transposition with clear, stable sound quality

AI resynthesis: Deep learning-based vocoder, not traditional pitch shifting

Auto-Tune-like workflow: Hand-drawing, note, and anchor tools available to help you quickly enter a flow state

Built-in Auto-Key style key detection: Accurate detection before correction, even more accurate after correction

Dual-format output: Standalone executable + VST3 plugin (with ARA2 extension support)

GPU-accelerated inference: Windows DirectML / macOS CoreML

Multi-language interface: Chinese / English / Japanese / Russian / Spanish

Open source and free: Permanently free, community-driven, continuously iterating

</div>

## 🖥️ Current Status
- AI engine: RMVPE pitch extraction + PC-NSF HifiGAN vocoder (ONNX Runtime inference)
- Multi-scale correction: Chromatic, Major, Minor, Pentatonic, Dorian, Mixolydian, Harmonic Minor
- Audio formats: WAV, FLAC, OGG, MP3 import


## 🧪 Beta Version Notice
This is an early test version, which may have some bugs and performance not yet fully optimized. Feel free to download and try it out, and share your valuable feedback, feature requests, or issues encountered through Issues or Discussion. We will actively improve based on feedback.

Known limitations:
- VST3 ARA mode requires host support for ARA2 (e.g., Studio One, Logic Pro)


## 🧠 Project Philosophy
AI exists to help people, with a human-centered approach to provide a better creative experience.

In today's music production where AI is deeply involved, mixing and audio editing are still often limited by recording quality, consuming significant time. OpenTune aims to use open technology to enable every creator to process vocals more freely, focusing energy back on the music itself.


## 📥 Download and Usage
Please visit the [Releases](https://github.com/YuFeng926/OpenTune/releases) page to download the latest installation package.

For detailed per-platform installation steps (Windows ZIP, macOS DMG install script and Gatekeeper handling, VST3 install location) and the runtime file structure, see [INSTALL.en.md](INSTALL.en.md).


## 🔨 Build Instructions
For system requirements, third-party dependency preparation, build commands, build artifacts, and troubleshooting, see [BUILDING.en.md](BUILDING.en.md).


## 🤝 Contributing
We welcome PRs, documentation translations, bug reports, and new feature suggestions. For the development environment and code conventions (code style, UI isolation, shared processor, etc.), see [BUILDING.en.md](BUILDING.en.md).


## 🙏 Acknowledgments

- **[OpenVPI Team / Diffsinger Community Vocoder](https://github.com/openvpi/vocoders)** — High-quality vocoder implementation, a core component of this project.
- **[yxlllc / RMVPE](https://github.com/yxlllc/RMVPE)** — RMVPE weights, significantly improving pitch extraction accuracy and robustness.
- **[吃土大佬 (CNChTu) / FCPE](https://github.com/CNChTu/FCPE)** — Referenced FCPE, may attempt implementation in the future.
- **[JUCE Framework](https://github.com/juce-framework/JUCE)** — Cross-platform audio framework.
- **[avaneev / r8brain-free-src](https://github.com/avaneev/r8brain-free-src)** — Efficient resampling algorithm.
- **[Celemony / ARA SDK](https://github.com/Celemony/ARA_SDK)** — ARA2 extension support.

All projects follow their own open-source license agreements. Thanks to the open and sharing spirit of these project authors and teams.


## ☕ Support This Project

This project is primarily developed by 风语 @ DAYA STUDIO. We are currently dedicated to exploring the limits of acoustic synthesis models and building cutting-edge audio editing tools.
If this project has been helpful to you, feel free to buy me a coffee!

<p align="center">
  <img src="assets/wechat-pay.png" alt="WeChat Pay" width="200" />
  <img src="assets/alipay.png" alt="Alipay" width="200" />
</p>

> Donations are voluntary support. They do not purchase feature priority and do not affect the handling order of issues.


## License
AGPL V3.0
