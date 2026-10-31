# Build Instructions

> This document is for developers who want to build OpenTune from source. For installing release packages, see [INSTALL.en.md](INSTALL.en.md); for project introduction, see [README.en.md](README.en.md).

## System Requirements

| Requirement | Windows | macOS |
|-------------|---------|-------|
| **System** | Windows 10 1903+ | macOS 12.0+ (Intel / Apple Silicon) |
| **Architecture** | x64 | universal2 (x86_64 + arm64) |
| **Compiler** | Visual Studio 2022 (MSVC 17+) | Xcode Command Line Tools / Apple Clang |
| **CMake** | 3.22+ | 3.22+ |
| **Python** | Python 3 (`python`) | Python 3 (`python3`) |
| **C++ Standard** | C++17 | C++17 |
| **Build System** | MSBuild (VS Generator) / Ninja | Ninja |

> **Note:** Windows builds support both Visual Studio Generator + MSBuild and Ninja. Visual Studio Generator is recommended for full development, while Ninja is suitable for quick builds.
> macOS Release uses the single `macos-universal2-ara-ninja` preset. The same artifact supports Intel, native Apple Silicon, and DAWs running under Rosetta on Apple Silicon; Ninja is required.

## Dependency Preparation

```bash
git clone -b standalone https://github.com/YuFeng926/OpenTune.git
cd OpenTune
```

> **Important: Except for the r8brain source included in this repository, build dependencies and models must be obtained from the third-party sources linked below** and placed at the specified paths.
>
> **CMake auto-detection:** After dependencies are placed according to the specifications below, no `-D` overrides are needed during configuration. CMakeLists.txt will automatically fall back to the bundled default paths under `ThirdParty/<dep>/` when cache paths are invalid.

### 1. JUCE Framework

Clone to the project root directory (**JUCE is an exception, not in `ThirdParty/`**), the folder name must be `JUCE-master`:

```bash
git clone --depth 1 --branch 8.0.12 https://github.com/juce-framework/JUCE.git JUCE-master
```

Windows:
```powershell
python scripts/github_actions/patch-juce-ara.py
```

macOS:
```bash
python3 scripts/github_actions/patch-juce-ara.py
```

> The script also adds the maximise button to the `juce_StandaloneFilterWindow.h` title bar
> (JUCE 8.0.12 upstream ships minimise and close only). Without it the standalone window shows
> only — and × in the top right.

### 2. ARA SDK

```bash
cd ThirdParty
git clone --recursive --branch releases/2.3.0 https://github.com/Celemony/ARA_SDK.git ARA_SDK-releases-2.3.0
cd ..
```

> ⚠️ **ARA 2.3.0 requires a matching patch to your local JUCE checkout**: `JUCE-master/` (gitignored,
> not shipped with this repository) still includes the 2.2.0 file
> `ARA_Library/Utilities/ARAChannelArrangement.cpp`, which ARA 2.3.0 renamed to `ARAChannelFormat.cpp`.
> Do not edit the file by hand; after cloning JUCE 8.0.12 run the platform-specific command above.
> The script applies the ARA include rename, the AudioDeviceManager temporary-setup patch,
> and the standalone title-bar maximise button patch.

### 3. r8brain and SoundTouch

`ThirdParty/r8brain-free-src-master/` is tracked in the Git repository; no separate download or clone is needed.

Obtain SoundTouch 2.3.2 from the [official source repository](https://codeberg.org/soundtouch/soundtouch) and clone it to the path expected by CMake:

```bash
git clone --branch 2.3.2 https://codeberg.org/soundtouch/soundtouch.git ThirdParty/soundtouch-2.3.2
```

### 4. ONNX Runtime (Windows v1.24.4 / macOS v1.20.0 universal2)

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

Both architectures use the same **universal2** package (x86_64 + arm64 slices, CoreML EP built in), pinned to **1.20.0** with a macOS 12.0 deployment target. This project-built runtime includes the CoreML initialization fix required by macOS 15+. Keep using this version; do not replace it with another ORT release.

The package is located at `ThirdParty/onnxruntime-osx-universal2-1.20.0/`. The repository's `scripts/build-onnxruntime-macos.sh` describes how to recreate it if the dependency is missing.

Extracted structure:
```
ThirdParty/onnxruntime-osx-universal2-1.20.0/
├── include/
│   └── onnxruntime_cxx_api.h
└── lib/
    └── libonnxruntime.1.20.0.dylib
```

> **CoreML registration path**: the project uses `Source/Inference/OnnxRuntimeProviderCompat.h` to register CoreML through ORT's flag-based C API. macOS 12 uses `NeuralNetwork`; macOS 13+ uses `MLProgram` (both with `MLComputeUnits=ALL`).

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

| Model | Official weight source | Local source path → build location |
|-------|------------------------|-----------------------------------|
| FCPE (F0 extraction) | [KCKT0112/HachiTune: `fcpe.onnx` (pinned commit)](https://github.com/KCKT0112/HachiTune/blob/d68808c37dede264d80d2d1050dce27af34ad654/Resources/models/fcpe.onnx); upstream model author: [CNChTu/FCPE](https://github.com/CNChTu/FCPE) | `models/fcpe.onnx` → `<output>/models/fcpe.onnx` |
| PC-NSF HifiGAN (vocoder) | [OpenVPI official 2025.02 `.oudep` package](https://github.com/openvpi/vocoders/releases/download/pc-nsf-hifigan-44.1k-hop512-128bin-2025.02/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.oudep). The file is a zip; the ONNX is at the package root. The `.zip` on the same release page contains only the PyTorch `model.ckpt`, not the ONNX. | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` → `<output>/models/hifigan.onnx` |

> HifiGAN uses OpenVPI's official release. OpenTune's current `models/fcpe.onnx` is byte-for-byte identical to the HachiTune file linked above (SHA-256: `013b507acd406de122b61ee497300ac6be082511101bc2136b886e48d9737e5d`). CNChTu is the upstream FCPE author and provides a `.pt` checkpoint, but HachiTune does not document how that checkpoint was exported to this ONNX file.

### External Dependencies Required by a macOS Buildbot

Except for the r8brain source included in the repository, obtain the following dependencies and models from the third-party sources listed in the table. ONNX Runtime is built from its official source by the script:

| Path | Required? | Setup / purpose |
|------|----------|-----------------|
| `JUCE-master/` | Yes | Clone JUCE 8.0.12 at the repository root, then run `python3 scripts/github_actions/patch-juce-ara.py` (macOS command; the same script handles the ARA include, AudioDeviceManager and standalone title-bar maximise button patches). |
| `ThirdParty/ARA_SDK-releases-2.3.0/` | Yes | Clone the ARA SDK `releases/2.3.0` branch with submodules. |
| `ThirdParty/r8brain-free-src-master/` | Yes | Included in the Git repository; no separate download is needed. |
| `ThirdParty/soundtouch-2.3.2/` | Yes | Clone the `2.3.2` tag from the official SoundTouch repository linked above. |
| `ThirdParty/onnxruntime-osx-universal2-1.20.0/` | Yes | Generate it with `./scripts/build-onnxruntime-macos.sh`. The script builds from [Microsoft ONNX Runtime v1.20.0](https://github.com/microsoft/onnxruntime/tree/v1.20.0) and fetches Eigen at [this official repository revision](https://gitlab.com/libeigen/eigen/-/archive/e7248b26a1ed53fa030c5c459f7ea095dfd276ac/eigen-e7248b26a1ed53fa030c5c459f7ea095dfd276ac.zip), then builds arm64 and x86_64 separately. The packaging script does not invoke this builder. |
| `models/fcpe.onnx` | Yes | Download from this [pinned KCKT0112/HachiTune commit](https://raw.githubusercontent.com/KCKT0112/HachiTune/d68808c37dede264d80d2d1050dce27af34ad654/Resources/models/fcpe.onnx); verify SHA-256 `013b507acd406de122b61ee497300ac6be082511101bc2136b886e48d9737e5d`. The upstream model author is [CNChTu/FCPE](https://github.com/CNChTu/FCPE). |
| `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` | Yes | Download the `.oudep` linked above and unzip it. Place its `pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` at this path. The build copies it to `models/hifigan.onnx`. |
Recommended macOS Buildbot order: obtain the required dependencies and models above from their third-party sources; run `./scripts/build-onnxruntime-macos.sh` if the ORT package is not cached; then run `./scripts/package-macos.sh --format pkg`. The packaging script configures and builds Universal2, validates the artifacts, and creates the PKG. `Installer/NOTICE.md` is included in the repository.

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
│   ├── onnxruntime-osx-universal2-1.20.0/ ← ONNX Runtime (macOS, shared Intel + Apple Silicon)
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
# macOS Universal2 Release (x86_64 + arm64, minimum macOS 12.0)
cmake --preset macos-universal2-ara-ninja
cmake --build --preset macos-universal2-ara-release
```

Packaging commands:
```bash
# Generate a Universal2 PKG by default
./scripts/package-macos.sh

# Optional: generate a Universal2 DMG
./scripts/package-macos.sh --format dmg

# Generate both PKG and DMG
./scripts/package-macos.sh --format both
```

The PKG installs the app in `/Applications` and the VST3 and AU in the system-wide `/Library/Audio/Plug-Ins/VST3/` and `/Library/Audio/Plug-Ins/Components/` directories. The DMG contains the Standalone, VST3, AU, and bilingual install commands, which place both plug-ins in the current user's VST3 and Components directories. The PKG welcome page uses the anti-fraud and license notice from `Installer/NOTICE.md`.

## Build Artifacts

| Format | Windows | macOS |
|--------|---------|-------|
| Standalone | `build-ara-overlay-vs18-clean/OpenTuneStandalone_artefacts/Release/Standalone/OpenTune.exe` | `build-macos-universal2-ninja/OpenTuneStandalone_artefacts/Release/Standalone/OpenTune.app` |
| VST3 ARA2 | `build-ara-overlay-vs18-clean/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` | `build-macos-universal2-ninja/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` |
| AUv2 | — | `build-macos-universal2-ninja/OpenTuneAU_artefacts/Release/AU/OpenTune.component/` |

After build completion, runtime DLLs, model files, and D3D12 directory will be automatically copied to the artifact directory, requiring no manual operation.

## Common Build Issues

| Symptom | Cause | Solution |
|---------|-------|----------|
| `ONNX Runtime C++ API header not found` | CPU version ONNX Runtime not placed correctly | Verify `ThirdParty/onnxruntime-win-x64-1.24.4/include/onnxruntime_cxx_api.h` exists |
| `DirectML provider header missing` | DML version NuGet package not extracted correctly | Verify `ThirdParty/onnxruntime-dml-1.24.4/build/native/include/dml_provider_factory.h` exists |
| `DirectML header missing` | DirectML NuGet package not extracted | Verify `ThirdParty/microsoft.ai.directml.1.15.4/include/DirectML.h` exists |
| `D3D12 header missing from Agility SDK` | D3D12 NuGet package not extracted | Verify `ThirdParty/microsoft.direct3d.d3d12.1.619.5/build/native/include/d3d12.h` exists |
| `ONNX Runtime DirectML DLL missing` | DML version runtime DLL missing | Verify `ThirdParty/onnxruntime-dml-1.24.4/runtimes/win-x64/native/onnxruntime.dll` exists |
| `ONNX Runtime dylib not found` | macOS universal2 package missing or version mismatch | Verify `ThirdParty/onnxruntime-osx-universal2-1.20.0/lib/libonnxruntime.1.20.0.dylib` exists |
| `ARA SDK not found` | ARA SDK not cloned | Execute step 2 git clone command |
| MSVC link error LNK2019 | MSVC runtime mismatch | This project uses static CRT (`/MT`), ensure dependency libraries are consistent |
| Ninja build failure | Ninja not installed or not in PATH | Ensure Ninja is installed and in system PATH, or use Visual Studio Generator |

## Development Guidelines

- **Code Style**: C++17, JUCE naming conventions
- **Commit Guidelines**: Concise description of changes (Chinese or English acceptable)
- **UI Isolation**: `Source/Standalone/` for Standalone UI, `Source/Plugin/` for VST3 UI, isolated via `JucePlugin_Build_Standalone` macro
- **Shared Processor**: `Source/PluginProcessor.*` is shared between both formats; modifications must consider both build targets
