# Build Instructions

> This document is for developers who want to build OpenTune from source. For installing release packages, see [INSTALL.en.md](INSTALL.en.md); for project introduction, see [README.en.md](README.en.md).

## System Requirements

| Requirement | Windows | macOS |
|-------------|---------|-------|
| **System** | Windows 10 1903+ | macOS 13.4+ (Intel) / 14.0+ (Apple Silicon) |
| **Architecture** | x64 | x86_64 (Intel) / arm64 (Apple Silicon) |
| **Compiler** | Visual Studio 2022 (MSVC 17+) | Xcode Command Line Tools / Apple Clang |
| **CMake** | 3.22+ | 3.22+ |
| **C++ Standard** | C++17 | C++17 |
| **Build System** | MSBuild (VS Generator) / Ninja | Ninja |

> **Note:** Windows builds support both Visual Studio Generator + MSBuild and Ninja. Visual Studio Generator is recommended for full development, while Ninja is suitable for quick builds.
> macOS Release and DMG packaging use the `macos-silicon-ara-ninja` (Apple Silicon) or `macos-intel-ara-ninja` (Intel) preset, so Ninja must be installed.

## Dependency Preparation

```bash
git clone -b standalone https://github.com/YuFeng926/OpenTune.git
cd OpenTune
```

> **Important: All third-party dependencies are not included in the Git repository** (`JUCE-master/` and `ThirdParty/` are added to `.gitignore` to avoid hundreds of MB of binary blobs in history). Please follow the steps below to obtain and place them in the specified paths.
>
> **CMake auto-detection:** After dependencies are placed according to the specifications below, no `-D` overrides are needed during configuration. CMakeLists.txt will automatically fall back to the bundled default paths under `ThirdParty/<dep>/` when cache paths are invalid.

### 1. JUCE Framework

Clone to the project root directory (**JUCE is an exception, not in `ThirdParty/`**), the folder name must be `JUCE-master`:

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

### 4. ONNX Runtime (Windows v1.24.4 / macOS Intel v1.23.0 / macOS Apple Silicon v1.24.4)

This project requires **two** ONNX Runtime packages (Windows): the CPU version provides headers, and the DML version provides the original `onnxruntime.dll` (with built-in DirectML support). The build system generates a dedicated import library and outputs the runtime DLL as `OpenTuneOnnxRuntime_1_24_4.dll`.

**Windows** — Download and extract to `ThirdParty/`:

| Package | Link | Extract to |
|---------|------|------------|
| ONNX Runtime CPU | [onnxruntime-win-x64-1.24.4.zip](https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-win-x64-1.24.4.zip) | `ThirdParty/onnxruntime-win-x64-1.24.4/` |
| ONNX Runtime DirectML | [Microsoft.ML.OnnxRuntime.DirectML.1.24.4.nupkg](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.24.4) | `ThirdParty/onnxruntime-dml-1.24.4/` |

> **Tip:** Rename `.nupkg` to `.zip` before extracting.

**Directory structure (key paths):**

```
ThirdParty/
├── onnxruntime-win-x64-1.24.4/       ← CPU version (for compilation linking)
│   ├── include/
│   │   └── onnxruntime_cxx_api.h      ← CMake checks for this file
│   └── lib/
│       └── onnxruntime.lib            ← Upstream package file; OpenTune doesn't link directly
│
└── onnxruntime-dml-1.24.4/            ← DML version (runtime DLL + DML provider headers)
    ├── build/native/include/
    │   └── dml_provider_factory.h     ← DirectML EP registration header
    └── runtimes/win-x64/native/
        └── onnxruntime.dll             ← Upstream source DLL; renamed and copied during build
```

**Why are two packages needed?**
- **CPU package** (`onnxruntime-win-x64-1.24.4`): Provides C++ API headers (`onnxruntime_cxx_api.h`).
- **DML package** (`onnxruntime-dml-1.24.4`): Provides the original `onnxruntime.dll` with compiled DirectML Execution Provider and `dml_provider_factory.h`. CMake generates `OpenTuneOnnxRuntime_1_24_4.lib` based on the project's `.def` file and renames the source DLL to `OpenTuneOnnxRuntime_1_24_4.dll` for deployment.

**macOS (Intel, x86_64)**:

macOS Intel uses the existing universal2 package from the repository (built as x86_64), with CoreML EP built in. This runtime requires macOS 13.4 or later:

```
ThirdParty/onnxruntime-osx-universal2-1.23.0/   ← Already included in the repository, no download needed
├── include/
│   └── onnxruntime_cxx_api.h
└── lib/
    └── libonnxruntime.1.23.0.dylib
```

**macOS (Apple Silicon, arm64)**:

macOS Apple Silicon uses the arm64 v1.24.4 package with CoreML EP built in. This runtime requires macOS 14.0 or later. **Please download and extract to the specified directory manually** (not included in the repository):

```bash
cd ThirdParty
curl -L https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-osx-arm64-1.24.4.tgz | tar xz
cd ..
```

Extracted structure:
```
ThirdParty/onnxruntime-osx-arm64-1.24.4/
├── include/
│   └── onnxruntime_cxx_api.h
└── lib/
    └── libonnxruntime.1.24.4.dylib
```

### 5. DirectML & DirectX Agility SDK (Windows only)

These two NuGet packages provide the DirectML runtime and latest D3D12 support required for GPU-accelerated inference.

| Package | Version | Download | Extract to |
|---------|---------|----------|------------|
| Microsoft.AI.DirectML | 1.15.4 | [NuGet](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4) | `ThirdParty/microsoft.ai.directml.1.15.4/` |
| Microsoft.Direct3D.D3D12 | 1.619.5 | [NuGet](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.5) | `ThirdParty/microsoft.direct3d.d3d12.1.619.5/` |

> After downloading `.nupkg`, rename to `.zip` and extract.

**CMake will detect these key files:**

```
ThirdParty/microsoft.ai.directml.1.15.4/
├── include/
│   └── DirectML.h
└── bin/x64-win/
    ├── DirectML.dll
    └── DirectML.lib

ThirdParty/microsoft.direct3d.d3d12.1.619.5/
└── build/native/
    ├── include/
    │   ├── d3d12.h
    │   └── d3dx12/          ← Helper header directory
    └── bin/x64/
        ├── D3D12Core.dll
        └── D3D12SDKLayers.dll
```

### 6. AI Models

Model files need to be placed in specified locations under the project root directory and will be automatically copied to the output directory during build:

| Model | Source Path | Build Location |
|-------|-------------|----------------|
| FCPE (F0 extraction) | `models/fcpe.onnx` | `<output>/models/fcpe.onnx` |
| PC-NSF HifiGAN (vocoder) | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` | `<output>/models/hifigan.onnx` |
| GAME note generator (optional) | `models/GAME/` | `<output>/models/GAME/` |

> Model files are not included in the Git repository. Please download from the [Releases](https://github.com/YuFeng926/OpenTune/releases) page or contact the maintainer. The GAME model bundle is optional; when absent, the runtime falls back to the Legacy workflow.

## Complete Directory Structure Overview

After configuring all dependencies, the project root directory should look like this:

```
OpenTune/
├── CMakeLists.txt
├── JUCE-master/                          ← JUCE framework
├── ThirdParty/
│   ├── ARA_SDK-releases-2.2.0/           ← ARA SDK
│   ├── r8brain-free-src-master/          ← Resampling library
│   ├── onnxruntime-win-x64-1.24.4/      ← ONNX Runtime CPU (Windows)
│   ├── onnxruntime-dml-1.24.4/           ← ONNX Runtime DML (Windows)
│   ├── onnxruntime-osx-universal2-1.23.0/ ← ONNX Runtime (macOS Intel, universal2)
│   ├── onnxruntime-osx-arm64-1.24.4/     ← ONNX Runtime (macOS Apple Silicon, arm64)
│   ├── microsoft.ai.directml.1.15.4/    ← DirectML SDK (Windows)
│   └── microsoft.direct3d.d3d12.1.619.5/ ← D3D12 Agility SDK (Windows)
├── models/
│   └── fcpe.onnx
├── pc_nsf_hifigan_44.1k_ONNX/
│   └── pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx
├── Source/
├── Resources/
└── docs/
```

## Build Commands

**Windows (Visual Studio + CMake)**

```powershell
# Generate ARA2 VST3 VS solution (must use "Visual Studio 17 2022" generator).
# Use PATH workaround in Codex/desktop shell for configuration and compilation to avoid Path/PATH conflicts affecting MSBuild.
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --preset windows-ara-vs2022"

# Compile Release version
cmd /v/on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build --preset windows-ara-release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build --preset windows-ara-release --target OpenTuneStandalone_Standalone"
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
3. Set `OpenTuneStandalone_Standalone` or `OpenTune_VST3` as the startup project
4. Select Release/x64 configuration, compile and run

**macOS (Ninja + CMake)**

```bash
# Apple Silicon (arm64)
cmake --preset macos-silicon-ara-ninja
cmake --build --preset macos-silicon-ara-release

# Intel (x86_64)
cmake --preset macos-intel-ara-ninja
cmake --build --preset macos-intel-ara-release
```

Packaging commands:
```bash
# Apple Silicon
./scripts/package-macos.sh --arch silicon

# Intel
./scripts/package-macos.sh --arch intel
```

## Build Artifacts

| Format | Windows | macOS |
|--------|---------|-------|
| Standalone | `build-ara-overlay-vs18-clean/OpenTuneStandalone_artefacts/Release/Standalone/OpenTune.exe` | `build/OpenTuneStandalone_artefacts/Release/Standalone/OpenTune.app` |
| VST3 ARA2 | `build-ara-overlay-vs18-clean/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` | `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` |

After build completion, runtime DLLs, model files, and D3D12 directory will be automatically copied to the artifact directory, requiring no manual operation.

## Common Build Issues

| Symptom | Cause | Solution |
|---------|-------|----------|
| `ONNX Runtime C++ API header not found` | CPU version ONNX Runtime not placed correctly | Verify `ThirdParty/onnxruntime-win-x64-1.24.4/include/onnxruntime_cxx_api.h` exists |
| `DirectML provider header missing` | DML version NuGet package not extracted correctly | Verify `ThirdParty/onnxruntime-dml-1.24.4/build/native/include/dml_provider_factory.h` exists |
| `DirectML header missing` | DirectML NuGet package not extracted | Verify `ThirdParty/microsoft.ai.directml.1.15.4/include/DirectML.h` exists |
| `D3D12 header missing from Agility SDK` | D3D12 NuGet package not extracted | Verify `ThirdParty/microsoft.direct3d.d3d12.1.619.5/build/native/include/d3d12.h` exists |
| `ONNX Runtime DirectML DLL missing` | DML version runtime DLL missing | Verify `ThirdParty/onnxruntime-dml-1.24.4/runtimes/win-x64/native/onnxruntime.dll` exists |
| `ARA SDK not found` | ARA SDK not cloned | Execute step 2 git clone command |
| MSVC link error LNK2019 | MSVC runtime mismatch | This project uses static CRT (`/MT`), ensure dependency libraries are consistent |
| Ninja build failure | Ninja not installed or not in PATH | Ensure Ninja is installed and in system PATH, or use Visual Studio Generator |

## Development Guidelines

- **Code Style**: C++17, JUCE naming conventions
- **Commit Guidelines**: Concise description of changes (Chinese or English acceptable)
- **UI Isolation**: `Source/Standalone/` for Standalone UI, `Source/Plugin/` for VST3 UI, isolated via `JucePlugin_Build_Standalone` macro
- **Shared Processor**: `Source/PluginProcessor.*` is shared between both formats; modifications must consider both build targets
