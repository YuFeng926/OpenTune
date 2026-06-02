# Technology Stack

**Analysis Date:** 2026-06-02

## Languages

**Primary:**
- C++17 - Entire application core: DSP, inference, UI, plugin processor, ARA integration
- C - ONNX Runtime C API bindings

**Secondary:**
- Python 3 - Auxiliary tooling: pitch comparison benchmark (`Python/pitch_compare.py`) and installer wizard image generator (`Installer/gen_wizard_images.py`)
- Bash - macOS code signing & DMG packaging (`scripts/sign-and-package.sh`), macOS installer script (`scripts/install.command`)
- Pascal (Inno Setup) - Windows installer script (`Installer/OpenTune_Installer.iss`)

## Runtime

**Environment:**
- Native desktop application (Standalone `.exe` / `.app`)
- VST3 audio plugin (`.vst3` bundle)
- Windows 10 1903+ (x64) / macOS 14.0+ Sonoma (arm64 Apple Silicon)

**Package Manager:**
- No C++ package manager — all third-party dependencies are vendored via `ThirdParty/` directory
- pip (Python): `Python/requirements.txt`

## Frameworks

**Core:**
- JUCE (vendored `JUCE-master/`) — Cross-platform audio application framework
  - Modules linked: `juce_audio_utils`, `juce_audio_processors`, `juce_dsp`, `juce_opengl`, `juce_graphics`, `juce_gui_basics`, `juce_gui_extra`
  - Plugin formats: Standalone, VST3 (with optional ARA2 extension)
  - Audio format support: FLAC (`JUCE_USE_FLAC=1`), OGG/Vorbis (`JUCE_USE_OGGVORBIS=1`), MP3 (`JUCE_USE_MP3AUDIOFORMAT=1`)
  - Windows: ASIO enabled (`JUCE_ASIO=1`), Windows Media Format (`JUCE_USE_WINDOWS_MEDIA_FORMAT=1`)

**Testing:**
- CTest (CMake's built-in test driver)
  - Test executable: `OpenTuneTests`
  - Test files in `Tests/` directory
  - No external test framework (tests use JUCE's test infrastructure or custom assertions)

**Build/Dev:**
- CMake 3.22+ — Build system generator; config in `CMakeLists.txt` and `CMakePresets.json`
- Visual Studio 2022 (MSVC 17+) — Windows compiler, MSBuild generator (Ninja unsupported for production builds)
- Xcode 14+ / Apple Clang — macOS compiler
- Ninja — Used only for LSP/clangd `compile_commands.json` generation (not production builds)
- clangd — LSP configuration in `.clangd`
- Inno Setup 6 — Windows installer compiler (`Installer/OpenTune_Installer.iss`)
- Git LFS — For `.onnx` model files (`.gitattributes`)

## Key Dependencies

**Critical (vendored in `ThirdParty/`):**
- ONNX Runtime v1.24.4 — AI model inference engine
  - Windows: two packages — CPU (`onnxruntime-win-x64-1.24.4/`) for headers/libs + DirectML (`onnxruntime-dml-1.24.4/`) for runtime DLL
  - macOS: single package (`onnxruntime-osx-arm64-1.24.4/`) with built-in CoreML EP
- Microsoft.AI.DirectML v1.15.4 — GPU-accelerated ML on Windows (DirectX 12 compute)
- Microsoft.Direct3D.D3D12 v1.619.1 (Agility SDK) — Latest D3D12 runtime for DirectML compatibility
- ARA SDK v2.2.0 — ARA2 extension for VST3 plugin (`ThirdParty/ARA_SDK-releases-2.2.0/`)
  - Optional: controlled by `OPENTUNE_ENABLE_ARA` CMake option
- SoundTouch v2.3.3 — WSOLA time-stretching for vocal Stage 2 processing
  - License: LGPL-2.1 (see `LICENSES/soundtouch.LGPL-2.1.txt`)
- r8brain-free-src (master) — High-quality sample rate conversion (`ThirdParty/r8brain-free-src-master/`)

**Infrastructure:**
- Windows Kits 10 — System headers/libs for D3D12, DXGI, DirectML (`d3d12.lib`, `dxgi.lib`)
- macOS Accelerate framework — Apple's vector math library for macOS builds

**AI Models (bundled at build time, not in git):**
- RMVPE ONNX model — F0/pitch extraction (`models/rmvpe.onnx`)
- PC-NSF HiFiGAN ONNX model — Neural vocoder (`pc_nsf_hifigan_44.1k_ONNX/*.onnx`)
  - Community version: `pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx`
  - Coulin9 fine-tuned: `pc_nsf_hifigan_44k_hop512_128bin_opentune_fmax22050_v4_user_zh_female_step20000.onnx`
- GAME model bundle — AI note generator (`models/GAME/`)
  - Optional: falls back to legacy note generator if missing

**Python Tooling Dependencies:**
- torch>=1.13.0
- onnxruntime>=1.14.0
- librosa>=0.9.0
- soundfile>=0.12.0
- parselmouth>=0.4.0
- numpy>=1.21.0
- scipy>=1.7.0
- Pillow (for installer image generation)

## Configuration

**Build:**
- `CMakePresets.json` — Three presets: `windows-ara-vs2022` (VS2022, ARA on), `windows-nonara-vs2022` (VS2022, ARA off), `windows-ara-lsp` (Ninja, clangd only)
- `CMakeLists.txt` — All build logic, dependency discovery, platform branching (Windows/macOS)

**IDE:**
- `.clangd` — clangd/LSP configuration: strict unused includes, disabled trailing-return-type + identifier-length warnings, inlay hints enabled

**Installer:**
- `Installer/OpenTune_Installer.iss` — Inno Setup 6 script for Windows installer
  - Multi-language: en, zh, ja, ru, es
  - Component-based: Standalone + VST3
  - Admin privileges required
  - LZMA2/ultra64 compression

## Platform Requirements

**Development:**
- **Windows:** Visual Studio 2022 (MSVC 17+), CMake 3.22+, Windows 10 1903+, Windows Kits 10
- **macOS:** Xcode 14+, CMake 3.22+, macOS 14.0+ (Sonoma), Apple Silicon (arm64)

**Production (Runtime):**
- **Windows:** Windows 10 1903+ x64
  - Filesystem: `models/`, `D3D12/`, `docs/`, DLLs alongside executable
  - VST3 path: `C:\Program Files\Common Files\VST3\OpenTune.vst3\`
- **macOS:** macOS 14.0+ (Sonoma) arm64 (Apple Silicon)
  - App bundle: `/Applications/OpenTune.app`
  - VST3 path: `/Library/Audio/Plug-Ins/VST3/OpenTune.vst3`

---

*Stack analysis: 2026-06-02*
