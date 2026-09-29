# Third-Party Credits

OpenTune is built on top of several open-source libraries and AI models. We
gratefully acknowledge the work of their authors.

## Core Frameworks

### JUCE
- **License**: GPL-3.0 / Commercial dual license
- **Project**: https://juce.com
- **Use**: Application framework, audio processing, GUI

### ARA SDK 2.2.0
- **License**: ARA SDK license (see ThirdParty/ARA_SDK-releases-2.2.0/LICENSE)
- **Project**: https://github.com/Celemony/ARA_SDK
- **Use**: Audio Random Access integration for VST3 plugin

## Audio Processing

### r8brain-free-src
- **License**: MIT
- **Project**: https://github.com/avaneev/r8brain-free-src
- **Use**: Polyphase 24-bit sample rate conversion

### SoundTouch 2.3.3
- **License**: GNU LGPL v2.1 (see LICENSES/soundtouch.LGPL-2.1.txt)
- **Project**: https://www.surina.net/soundtouch/
- **Author**: Olli Parviainen
- **Use**: WSOLA time-stretching engine driving Stage 2 of the two-stage
  render pipeline (vocal-friendly retiming).

## AI Models

### RMVPE (Robust Model for Vocal Pitch Estimation)
- **License**: BSD-3-Clause (model only; please verify upstream)
- **Project**: https://github.com/Mai-Hiyori/RMVPE
- **Use**: Fundamental frequency (F0) extraction from vocal audio.
  Runs at 16 kHz / 100 fps via ONNX Runtime (CPU on Windows; CoreML on macOS).

### PC-NSF-HiFiGAN
- **License**: MIT (please verify upstream)
- **Project**: https://github.com/openvpi/SingingVocoders
- **Use**: F0-conditioned neural vocoder. Synthesises 44.1 kHz audio from
  log-mel spectrogram + corrected F0, preserving formants under extreme
  pitch correction. Runs via ONNX Runtime (CPU/DirectML on Windows;
  CoreML on macOS).

## ONNX Runtime
- **License**: MIT
- **Project**: https://onnxruntime.ai/
- **Version**: Windows 1.24.4; macOS Intel 1.23.0; macOS Apple Silicon 1.24.4
- **Use**: Cross-platform AI inference (CPU on all platforms; DirectML
  on Windows; CoreML on macOS).

## Resources

### HONOR Sans CN Medium
- **License**: HONOR font license (commercial / personal use within OpenTune)
- **Use**: Default sans-serif typeface for UI

### Piano Samples
- **License**: CC0 / public domain (verify per sample)
- **Use**: 88-key piano audition feature for the piano-roll editor
