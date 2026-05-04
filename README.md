# OpenTune

AI-powered real-time pitch correction application. Supports Standalone and VST3 (with ARA extension) dual-format builds on Windows and macOS.

## Features

- **AI Pitch Detection** — RMVPE-based F0 extraction for accurate pitch tracking
- **AI Vocoder** — PC-NSF HifiGAN neural vocoder with LR4 crossover mixing
- **Piano Roll Editor** — Visual pitch editing with hand-draw, line, and selection tools
- **Multi-scale Correction** — Chromatic, Major, Minor, Pentatonic, Dorian, Mixolydian, Harmonic Minor
- **Dual Format** — Standalone application + VST3 plugin with ARA extension
- **GPU Acceleration** — DirectML (Windows) / CoreML (macOS) for inference
- **Multi-language** — English, Chinese, Japanese, Russian, Spanish

## Project Structure

```
OpenTune/
├── Source/
│   ├── ARA/                    # ARA document controller & playback renderer
│   ├── Audio/                  # Audio format registry & async loader
│   ├── DSP/                    # Resampling, mel spectrogram, crossover mixer
│   ├── Editor/                 # Editor factory, preferences, shared components
│   ├── Inference/              # ONNX Runtime inference (F0, vocoder, DML)
│   ├── Plugin/                 # VST3 plugin editor
│   ├── Services/               # F0 extraction service
│   ├── Standalone/             # Standalone editor & UI components
│   │   └── UI/
│   │       └── PianoRoll/      # Piano roll renderer, tools, correction worker
│   └── Utils/                  # Pitch curves, undo, preferences, CPU features
├── Resources/
│   ├── Fonts/                  # HONORSansCN-Medium.ttf
│   ├── PianoSamples-mp3/       # Piano audition samples (MIDI 21–108)
│   └── macOS/                  # Entitlements for code signing
├── Tests/                      # Unit tests
├── docs/                       # Multi-language user guides
├── scripts/                    # macOS signing & packaging
└── CMakeLists.txt              # Build configuration
```

## Prerequisites

| Requirement | Windows | macOS |
|-------------|---------|-------|
| **OS** | Windows 10+ | macOS 12.0+ (Monterey) |
| **Architecture** | x64 | arm64 (Apple Silicon) |
| **Compiler** | Visual Studio 2022 (MSVC 17+) | Xcode 14+ / Apple Clang |
| **CMake** | 3.22+ | 3.22+ |
| **C++ Standard** | C++17 | C++17 |

## Dependencies Setup

Clone the repository and prepare third-party dependencies:

```bash
git clone -b standalone https://github.com/YuFeng926/OpenTune.git
cd OpenTune
```

### 1. JUCE Framework

```bash
git clone https://github.com/juce-framework/JUCE.git JUCE-master
```

### 2. ARA SDK

```bash
cd ThirdParty
git clone --recursive --branch releases/2.2.0 https://github.com/Celemony/ARA_SDK.git ARA_SDK-releases-2.2.0
cd ..
```

### 3. r8brain Resampler

```bash
cd ThirdParty
git clone https://github.com/avaneev/r8brain-free-src.git r8brain-free-src-master
cd ..
```

### 4. ONNX Runtime (v1.24.4)

#### Windows

Download and extract to `ThirdParty/`:

| Package | URL | Extract to |
|---------|-----|------------|
| ONNX Runtime CPU | [onnxruntime-win-x64-1.24.4.zip](https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-win-x64-1.24.4.zip) | `ThirdParty/onnxruntime-win-x64-1.24.4/` |
| ONNX Runtime DirectML | [Microsoft.ML.OnnxRuntime.DirectML.1.24.4.nupkg](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.24.4) | `ThirdParty/onnxruntime-dml-1.24.4/` |

> **Note:** Rename `.nupkg` to `.zip` and extract. The DML package provides `runtimes/win-x64/native/onnxruntime.dll` with built-in DirectML support.

#### macOS (Apple Silicon)

```bash
cd ThirdParty
curl -L https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-osx-arm64-1.24.4.tgz | tar xz
cd ..
```

### 5. DirectML & DirectX Agility SDK (Windows only)

Download NuGet packages and extract to `ThirdParty/`:

| Package | Version | Extract to |
|---------|---------|------------|
| [Microsoft.AI.DirectML](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4) | 1.15.4 | `ThirdParty/microsoft.ai.directml.1.15.4/` |
| [Microsoft.Direct3D.D3D12](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.1) | 1.619.1 | `ThirdParty/microsoft.direct3d.d3d12.1.619.1/` |

### 6. AI Models

Place ONNX model files in the project root:

| Model | Path |
|-------|------|
| RMVPE (F0 extractor) | `models/rmvpe.onnx` |
| PC-NSF HifiGAN (vocoder) | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` |

### Final Directory Layout

After setup, your `ThirdParty/` directory should look like:

```
ThirdParty/
├── ARA_SDK-releases-2.2.0/
├── r8brain-free-src-master/
├── onnxruntime-win-x64-1.24.4/          # Windows only
├── onnxruntime-dml-1.24.4/              # Windows only
├── onnxruntime-osx-arm64-1.24.4/        # macOS only
├── microsoft.ai.directml.1.15.4/        # Windows only
└── microsoft.direct3d.d3d12.1.619.1/    # Windows only
```

## Build Instructions

### Windows (Visual Studio + CMake)

```powershell
# Configure (generates Visual Studio solution)
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build Release
cmake --build build --config Release

# Build Debug
cmake --build build --config Debug
```

**Build outputs:**
- Standalone: `build/OpenTune_artefacts/Release/Standalone/OpenTune.exe`
- VST3: `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3/`

Post-build automatically copies ONNX Runtime DLLs, DirectML DLL, D3D12 Agility SDK, and AI models alongside the executables.

### macOS (Xcode + CMake)

```bash
# Configure (generates Xcode project)
cmake -B build -G Xcode

# Build Release
cmake --build build --config Release

# Or open in Xcode
open build/OpenTune.xcodeproj
```

**Build outputs:**
- Standalone: `build/OpenTune_artefacts/Release/Standalone/OpenTune.app`
- VST3: `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3/`

Post-build automatically:
- Copies `libonnxruntime.1.24.4.dylib` to `OpenTune.app/Contents/Frameworks/`
- Copies AI models to `OpenTune.app/Contents/Resources/models/`
- Copies user guide translations to `OpenTune.app/Contents/Resources/docs/`
- Merges `Info.plist` with microphone permission and app category metadata

### Running Tests

```bash
cmake --build build --config Release --target OpenTuneTests
ctest --test-dir build -C Release
```

## macOS Code Signing & Notarization

For distribution, use the included signing script:

```bash
# Set environment variables
export APPLE_TEAM_ID="YOUR_TEAM_ID"
export APPLE_SIGNING_ID="Developer ID Application: Your Name (TEAM_ID)"
export APPLE_ID="your@apple.id"
export APPLE_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"

# Sign and package
bash scripts/sign-and-package.sh
```

The entitlements file at `Resources/macOS/OpenTune.entitlements` grants:
- `com.apple.security.cs.disable-library-validation` — required for loading ONNX Runtime dylib
- `com.apple.security.device.audio-input` — microphone access

## VST3 Plugin Installation

| Platform | VST3 Install Path |
|----------|--------------------|
| Windows | `C:\Program Files\Common Files\VST3\` |
| macOS | `~/Library/Audio/Plug-Ins/VST3/` |

Copy the `OpenTune.vst3` bundle to the appropriate directory. The VST3 plugin supports ARA extension for DAWs with ARA2 support (e.g., REAPER, Studio One, Cubase).

## License

Copyright © 2024-2026 DAYA. All rights reserved.
