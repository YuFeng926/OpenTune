# Build Instructions

> This document is for developers who want to build OpenTune from source. For installing release packages, see [INSTALL.en.md](INSTALL.en.md); for project introduction, see [README.en.md](README.en.md).

## System Requirements

| Requirement | Windows | macOS |
|-------------|---------|-------|
| **System** | Windows 10 1903+ | macOS 12.0+ (Intel / Apple Silicon) |
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
git clone --recursive --branch releases/2.3.0 https://github.com/Celemony/ARA_SDK.git ARA_SDK-releases-2.3.0
cd ..
```

> ⚠️ **ARA 2.3.0 requires a matching patch to your local JUCE checkout**: `JUCE-master/` (gitignored,
> not shipped with this repository) still has
> `modules/juce_audio_plugin_client/juce_audio_plugin_client_ARA.cpp` including the 2.2.0 file
> `ARA_Library/Utilities/ARAChannelArrangement.cpp`, which ARA 2.3.0 renamed to `ARAChannelFormat.cpp`.
> After cloning the SDK, change that line to `#include <ARA_Library/Utilities/ARAChannelFormat.cpp>`,
> otherwise compiling `juce_audio_plugin_client_ARA.cpp` fails with C1083 (include file not found).

### 3. r8brain Resampler

```bash
cd ThirdParty
git clone https://github.com/avaneev/r8brain-free-src.git r8brain-free-src-master
cd ..
```

### 4. ONNX Runtime (Windows v1.24.4 / macOS v1.19.2 universal2)

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

**Runtime resolution order (Windows)**: the app loads `OpenTuneOnnxRuntime_<version>.dll` from its own directory first (next to the Standalone executable, or in the VST3 bundle's `Contents\x86_64-win`); the shared `Program Files\OpenTune` install is only a fallback, so a stale installation can never shadow the copy shipped with the binary. The ORT version in the file name is an ABI contract: bumping ONNX Runtime requires updating `OPENTUNE_ORT_DLL_NAME` and the source file name in the installer's `[Files]` section (if they diverge, ISCC fails the compile instead of packaging the wrong file); stale files are cleaned by the installer's `OpenTuneOnnxRuntime_*.dll` wildcard in `[InstallDelete]`, and `scripts/validate-windows-release.ps1` reads the expected name from CMakeLists and asserts that exactly one copy exists. The installer places models in `%ProgramData%\OpenTune\Models`, shared by the Standalone and the VST3 (the portable ZIP still ships them next to the executable).

**macOS (Intel / Apple Silicon, shared)**:

Both macOS architectures share the same **universal2** package (x86_64 + arm64 slices, CoreML EP built in), pinned to version **1.19.2**. The runtime's Mach-O minimum OS is macOS 11.0, so together with `CMAKE_OSX_DEPLOYMENT_TARGET = 12.0` it runs on macOS 12 and later. **Download and extract it to the specified directory; do not replace it with a newer ORT release** (newer releases raise the minimum OS requirement):

```bash
cd ThirdParty
curl -L https://github.com/microsoft/onnxruntime/releases/download/v1.19.2/onnxruntime-osx-universal2-1.19.2.tgz | tar xz
cd ..
```

Extracted structure:
```
ThirdParty/onnxruntime-osx-universal2-1.19.2/
├── include/
│   └── onnxruntime_cxx_api.h
└── lib/
    └── libonnxruntime.1.19.2.dylib
```

> **CoreML registration path**: ORT 1.19.2 does not recognise the string-based `AppendExecutionProvider("CoreML", ...)` (it throws `Unknown provider name`). The project uses `Source/Inference/OnnxRuntimeProviderCompat.h` to try the newer provider-options API first and fall back to the flag-based C API (`COREML_FLAG_CREATE_MLPROGRAM`). Re-verify this file and confirm the log shows `CoreML EP added` (CoreML actually active) before upgrading the macOS ORT version.

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
│   ├── ARA_SDK-releases-2.3.0/           ← ARA SDK
│   ├── r8brain-free-src-master/          ← Resampling library
│   ├── onnxruntime-win-x64-1.24.4/      ← ONNX Runtime CPU (Windows)
│   ├── onnxruntime-dml-1.24.4/           ← ONNX Runtime DML (Windows)
│   ├── onnxruntime-osx-universal2-1.19.2/ ← ONNX Runtime (macOS, shared Intel + Apple Silicon)
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

Before packaging a Release build, validate the Standalone artifact. The
validation and packaging scripts are maintained locally and are not
distributed with the repository (`scripts/build-ninja.ps1`,
`scripts/validate-windows-release.ps1`, `scripts/package-windows.ps1`,
`Installer/`). The `scripts/build-ninja.ps1` script runs the same validation
automatically after a Release build; it can also be run directly:

```powershell
.\scripts\validate-windows-release.ps1
```

The validation checks that `OpenTune.exe` matches the version in
`CMakeLists.txt` and that the ONNX Runtime, DirectML, D3D12, FCPE, and HifiGAN
files are present in the same Release artifact. Build the Inno Setup installer
only through `scripts/package-windows.ps1`; it runs the validation again before
calling Inno Setup, preventing stale `build-ara-ninja` files from being packed
into a new installer. The installer also validates the installed files and
executable version after installation and aborts on failure.

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
# Apple Silicon (DMG)
./scripts/package-macos.sh --arch silicon

# Intel (DMG)
./scripts/package-macos.sh --arch intel

# Build a PKG installer (welcome page shows the anti-fraud and license
# notice sourced from Installer/NOTICE.md)
./scripts/package-macos.sh --arch silicon --format pkg

# Produce both DMG and PKG
./scripts/package-macos.sh --arch intel --format both
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
| `ONNX Runtime dylib not found` | macOS universal2 package missing or version mismatch | Verify `ThirdParty/onnxruntime-osx-universal2-1.19.2/lib/libonnxruntime.1.19.2.dylib` exists and has not been replaced with a newer ORT version |
| `ARA SDK not found` | ARA SDK not cloned | Execute step 2 git clone command |
| MSVC link error LNK2019 | MSVC runtime mismatch | This project uses static CRT (`/MT`), ensure dependency libraries are consistent |
| Ninja build failure | Ninja not installed or not in PATH | Ensure Ninja is installed and in system PATH, or use Visual Studio Generator |

## Development Guidelines

- **Code Style**: C++17, JUCE naming conventions
- **Commit Guidelines**: Concise description of changes (Chinese or English acceptable)
- **UI Isolation**: `Source/Standalone/` for Standalone UI, `Source/Plugin/` for VST3 UI, isolated via `JucePlugin_Build_Standalone` macro
- **Shared Processor**: `Source/PluginProcessor.*` is shared between both formats; modifications must consider both build targets
