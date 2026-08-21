<div align="center">

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
- Platforms: Windows + macOS (Standalone executable + VST3 plugin + ARA2 extension)
- GPU acceleration: DirectML (Windows) / CoreML (macOS)
- Multi-scale correction: Chromatic, Major, Minor, Pentatonic, Dorian, Mixolydian, Harmonic Minor
- AI engine: RMVPE pitch extraction + PC-NSF HifiGAN vocoder (ONNX Runtime inference)
- Audio formats: WAV, FLAC, OGG, MP3 import
- Interface languages: Chinese / English / Japanese / Russian / Spanish


## 🧪 Beta Version Notice
This is an early test version, which may have some bugs and performance not yet fully optimized. Feel free to download and try it out, and share your valuable feedback, feature requests, or issues encountered through Issues or Discussion. We will actively improve based on feedback.

Known limitations:
- VST3 ARA mode requires host support for ARA2 (e.g., Studio One, Logic Pro)


## 🧠 Project Philosophy
AI exists to help people, with a human-centered approach to provide a better creative experience.

In today's music production where AI is deeply involved, mixing and audio editing are still often limited by recording quality, consuming significant time. OpenTune aims to use open technology to enable every creator to process vocals more freely, focusing energy back on the music itself.


## 📥 Download and Usage
Please visit the [Releases](https://github.com/YuFeng926/OpenTune/releases) page to download the latest installation package.

### Windows
Extract the ZIP package and run `OpenTune.exe` directly. Keep the `models/`, `D3D12/`, and DLL files in the same directory as the executable.
Copy the `OpenTune.vst3` folder to `C:\Program Files\Common Files\VST3\` to load the plugin in your DAW.

### macOS
1. Download `OpenTune-<version>-macOS-arm64.dmg` and double-click to mount.
2. Double-click **install.command** in the mounted disk window (if blocked by Gatekeeper on first run, go to "System Settings → Privacy & Security" and click *Open Anyway*; or right-click the script in the DMG window → *Open*).
3. The terminal will automatically execute:
    - Copy `OpenTune.app` to `/Applications/` and remove the quarantine attribute;
    - Ask whether to install VST3 as well. If agreed, it will request administrator password to install `OpenTune.vst3` to the system-level `/Library/Audio/Plug-Ins/VST3/` (globally visible in DAWs);
    - Ask whether to launch the Standalone immediately.
4. After installation is complete, you can eject and discard the DMG. To update, re-running the script will overwrite the old version.

> ⚠️ Release packages currently use ad-hoc signing (no Apple Notarization), so you may see a "Cannot verify developer" prompt on first launch. `install.command` automatically removes the quarantine flag. If you manually drag the `.app` bypassing the script, you need to run:
>
> ```bash
> xattr -rd com.apple.quarantine /Applications/OpenTune.app
> xattr -rd com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/OpenTune.vst3"
> ```

### Runtime File Structure (Windows)

```
OpenTune/
├── OpenTune.exe
├── OpenTuneOnnxRuntime_1_23_0.dll ← ONNX Runtime (built-in DirectML)
├── DirectML.dll             ← DirectML runtime
├── D3D12/
│   ├── D3D12Core.dll        ← DirectX Agility SDK
│   └── D3D12SDKLayers.dll
├── models/
│   ├── rmvpe.onnx           ← Pitch extraction model
│   └── hifigan.onnx         ← Vocoder model
└── docs/
    └── UserGuide.html
```


## 🔨 Build Instructions

### System Requirements

| Requirement | Windows | macOS |
|-------------|---------|-------|
| **System** | Windows 10 1903+ | macOS 14.0+ (Sonoma) |
| **Architecture** | x64 | arm64 (Apple Silicon) |
| **Compiler** | Visual Studio 2022 (MSVC 17+) | Xcode 14+ / Apple Clang |
| **CMake** | 3.22+ | 3.22+ |
| **C++ Standard** | C++17 | C++17 |
| **Build System** | MSBuild (VS Generator) / Ninja | Xcode |

> **Note:** Windows builds support both Visual Studio Generator + MSBuild and Ninja. Visual Studio Generator is recommended for full development, while Ninja is suitable for quick builds.

### Dependency Preparation

```bash
git clone -b standalone https://github.com/YuFeng926/OpenTune.git
cd OpenTune
```

> **Important: All third-party dependencies are not included in the Git repository** (`JUCE-master/` and `ThirdParty/` are added to `.gitignore` to avoid hundreds of MB of binary blobs in history). Please follow the steps below to obtain and place them in the specified paths.
>
> **CMake auto-detection:** After dependencies are placed according to the specifications below, no `-D` overrides are needed during configuration. CMakeLists.txt will automatically fall back to the bundled default paths under `ThirdParty/<dep>/` when cache paths are invalid.

#### 1. JUCE Framework

Clone to the project root directory (**JUCE is an exception, not in `ThirdParty/`**), the folder name must be `JUCE-master`:

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

#### 4. ONNX Runtime (v1.23.0)

This project requires **two** ONNX Runtime packages (Windows): the CPU version provides headers, and the DML version provides the original `onnxruntime.dll` (with built-in DirectML support). The build system generates a dedicated import library and outputs the runtime DLL as `OpenTuneOnnxRuntime_1_23_0.dll`.

**Windows** — Download and extract to `ThirdParty/`:

| Package | Link | Extract to |
|---------|------|------------|
| ONNX Runtime CPU | [onnxruntime-win-x64-1.23.0.zip](https://github.com/microsoft/onnxruntime/releases/download/v1.23.0/onnxruntime-win-x64-1.23.0.zip) | `ThirdParty/onnxruntime-win-x64-1.23.0/` |
| ONNX Runtime DirectML | [Microsoft.ML.OnnxRuntime.DirectML.1.23.0.nupkg](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.23.0) | `ThirdParty/onnxruntime-dml-1.23.0/` |

> **Tip:** Rename `.nupkg` to `.zip` before extracting.

**Directory structure (key paths):**

```
ThirdParty/
├── onnxruntime-win-x64-1.23.0/       ← CPU version (for compilation linking)
│   ├── include/
│   │   └── onnxruntime_cxx_api.h      ← CMake checks for this file
│   └── lib/
│       └── onnxruntime.lib            ← Upstream package file; OpenTune doesn't link directly
│
└── onnxruntime-dml-1.23.0/            ← DML version (runtime DLL + DML provider headers)
    ├── build/native/include/
    │   └── dml_provider_factory.h     ← DirectML EP registration header
    └── runtimes/win-x64/native/
        └── onnxruntime.dll             ← Upstream source DLL; renamed and copied during build
```

**Why are two packages needed?**
- **CPU package** (`onnxruntime-win-x64-1.23.0`): Provides C++ API headers (`onnxruntime_cxx_api.h`).
- **DML package** (`onnxruntime-dml-1.23.0`): Provides the original `onnxruntime.dll` with compiled DirectML Execution Provider and `dml_provider_factory.h`. CMake generates `OpenTuneOnnxRuntime_1_23_0.lib` based on the project's `.def` file and renames the source DLL to `OpenTuneOnnxRuntime_1_23_0.dll` for deployment.

**macOS (Apple Silicon + Intel)**:

macOS only requires one universal2 package (supports arm64 + x86_64), with CoreML EP built-in:

```bash
cd ThirdParty
curl -L https://github.com/microsoft/onnxruntime/releases/download/v1.23.0/onnxruntime-osx-universal2-1.23.0.tgz | tar xz
cd ..
```

Extracted structure:
```
ThirdParty/onnxruntime-osx-universal2-1.23.0/
├── include/
│   └── onnxruntime_cxx_api.h
└── lib/
    └── libonnxruntime.1.23.0.dylib
```

#### 5. DirectML & DirectX Agility SDK (Windows only)

These two NuGet packages provide the DirectML runtime and latest D3D12 support required for GPU-accelerated inference.

| Package | Version | Download | Extract to |
|---------|---------|----------|------------|
| Microsoft.AI.DirectML | 1.15.4 | [NuGet](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4) | `ThirdParty/microsoft.ai.directml.1.15.4/` |
| Microsoft.Direct3D.D3D12 | 1.619.1 | [NuGet](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.1) | `ThirdParty/microsoft.direct3d.d3d12.1.619.1/` |

> After downloading `.nupkg`, rename to `.zip` and extract.

**CMake will detect these key files:**

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
    │   └── d3dx12/          ← Helper header directory
    └── bin/x64/
        ├── D3D12Core.dll
        └── D3D12SDKLayers.dll
```

#### 6. AI Models

Model files need to be placed in specified locations under the project root directory and will be automatically copied to the output directory during build:

| Model | Source Path | Build Location |
|-------|-------------|----------------|
| RMVPE (F0 extraction) | `models/rmvpe.onnx` | `<output>/models/rmvpe.onnx` |
| PC-NSF HifiGAN (vocoder) | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` | `<output>/models/hifigan.onnx` |

> Model files are not included in the Git repository. Please download from the [Releases](https://github.com/YuFeng926/OpenTune/releases) page or contact the maintainer.

### Complete Directory Structure Overview

After configuring all dependencies, the project root directory should look like this:

```
OpenTune/
├── CMakeLists.txt
├── JUCE-master/                          ← JUCE framework
├── ThirdParty/
│   ├── ARA_SDK-releases-2.2.0/           ← ARA SDK
│   ├── r8brain-free-src-master/          ← Resampling library
│   ├── onnxruntime-win-x64-1.23.0/      ← ONNX Runtime CPU (Windows)
│   ├── onnxruntime-dml-1.23.0/           ← ONNX Runtime DML (Windows)
│   ├── onnxruntime-osx-universal2-1.23.0/ ← ONNX Runtime (macOS, universal2)
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

### Build Commands

**Windows (Visual Studio + CMake)**

```powershell
# Generate ARA2 VST3 VS solution (must use "Visual Studio 17 2022" generator).
# Use PATH workaround in Codex/desktop shell for configuration and compilation to avoid Path/PATH conflicts affecting MSBuild.
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --preset windows-ara-vs2022"

# Compile Release version
cmd /v/on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build --preset windows-ara-release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build --preset windows-ara-release --target OpenTune_Standalone"
```

**Windows (Ninja + CMake)**

```powershell
# Quick build with Ninja (requires Ninja in PATH)
cmake --preset windows-ara-ninja
cmake --build --preset windows-ara-ninja-release
```

The same ARA2 VST3 binary will naturally fall back to the Capture workflow in standard VST3 hosts without ARA binding, and no separate non-ARA plugin is generated.

For development in Visual Studio IDE:
1. Run the `cmake --preset ...` command above
2. Open `build-ara-overlay-vs18-clean/OpenTune.sln`
3. Set `OpenTune_Standalone` or `OpenTune_VST3` as the startup project
4. Select Release/x64 configuration, compile and run

**macOS (Ninja + CMake)**

```bash
cmake --preset macos-ara-ninja
cmake --build --preset macos-ara-release
```

**macOS (Xcode + CMake)**

```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

Or open `build/OpenTune.xcodeproj` in Xcode for development and debugging.

### Build Artifacts

| Format | Windows | macOS |
|--------|---------|-------|
| Standalone | `build-ara-overlay-vs18-clean/OpenTune_artefacts/Release/Standalone/OpenTune.exe` | `build/OpenTune_artefacts/Release/Standalone/OpenTune.app` |
| VST3 ARA2 | `build-ara-overlay-vs18-clean/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` | `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` |

After build completion, runtime DLLs, model files, and D3D12 directory will be automatically copied to the artifact directory, requiring no manual operation.

### Common Build Issues

| Symptom | Cause | Solution |
|---------|-------|----------|
| `ONNX Runtime C++ API header not found` | CPU version ONNX Runtime not placed correctly | Verify `ThirdParty/onnxruntime-win-x64-1.23.0/include/onnxruntime_cxx_api.h` exists |
| `DirectML provider header missing` | DML version NuGet package not extracted correctly | Verify `ThirdParty/onnxruntime-dml-1.23.0/build/native/include/dml_provider_factory.h` exists |
| `DirectML header missing` | DirectML NuGet package not extracted | Verify `ThirdParty/microsoft.ai.directml.1.15.4/include/DirectML.h` exists |
| `D3D12 header missing from Agility SDK` | D3D12 NuGet package not extracted | Verify `ThirdParty/microsoft.direct3d.d3d12.1.619.1/build/native/include/d3d12.h` exists |
| `ONNX Runtime DirectML DLL missing` | DML version runtime DLL missing | Verify `ThirdParty/onnxruntime-dml-1.23.0/runtimes/win-x64/native/onnxruntime.dll` exists |
| `ARA SDK not found` | ARA SDK not cloned | Execute step 2 git clone command |
| MSVC link error LNK2019 | MSVC runtime mismatch | This project uses static CRT (`/MT`), ensure dependency libraries are consistent |
| Ninja build failure | Ninja not installed or not in PATH | Ensure Ninja is installed and in system PATH, or use Visual Studio Generator |


## 🤝 Contributing

We welcome PRs, documentation translations, bug reports, and new feature suggestions. Let's work together to make OpenTune even better.

### Development Guidelines

- **Code Style**: C++17, JUCE naming conventions
- **Commit Guidelines**: Concise description of changes (Chinese or English acceptable)
- **UI Isolation**: `Source/Standalone/` for Standalone UI, `Source/Plugin/` for VST3 UI, isolated via `JucePlugin_Build_Standalone` macro
- **Shared Processor**: `Source/PluginProcessor.*` is shared between both formats; modifications must consider both build targets


## 🙏 Acknowledgments

- **[OpenVPI Team / Diffsinger Community Vocoder](https://github.com/openvpi/vocoders)** — High-quality vocoder implementation, a core component of this project.
- **[yxlllc / RMVPE](https://github.com/yxlllc/RMVPE)** — RMVPE weights, significantly improving pitch extraction accuracy and robustness.
- **[吃土大佬 (CNChTu) / FCPE](https://github.com/CNChTu/FCPE)** — Referenced FCPE, may attempt implementation in the future.
- **[JUCE Framework](https://github.com/juce-framework/JUCE)** — Cross-platform audio framework.
- **[avaneev / r8brain-free-src](https://github.com/avaneev/r8brain-free-src)** — Efficient resampling algorithm.
- **[Celemony / ARA SDK](https://github.com/Celemony/ARA_SDK)** — ARA2 extension support.

All projects follow their own open-source license agreements. Thanks to the open and sharing spirit of these project authors and teams.


## License
AGPL V3.0