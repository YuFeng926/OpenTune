# 构建说明

> 本文档面向希望从源码构建 OpenTune 的开发者。安装发布包请看 [INSTALL.md](INSTALL.md)，项目介绍见 [README](README.md)。

## 环境要求

| 需求 | Windows | macOS |
|------|---------|-------|
| **系统** | Windows 10 1903+ | macOS 12.0+ (Intel / Apple Silicon) |
| **架构** | x64 | universal2 (x86_64 + arm64) |
| **编译器** | Visual Studio 2022 (MSVC 17+) | Xcode Command Line Tools / Apple Clang |
| **CMake** | 3.22+ | 3.22+ |
| **C++ 标准** | C++17 | C++17 |
| **构建系统** | MSBuild (VS Generator) / Ninja | Ninja |

> **注意：** Windows 构建支持 Visual Studio Generator + MSBuild 或 Ninja 两种方式。推荐使用 Visual Studio Generator 进行完整开发，Ninja 适合快速构建。
> macOS Release 使用唯一的 `macos-universal2-ara-ninja` 预设；同一产物支持 Intel、Apple Silicon 原生和 Apple Silicon 上以 Rosetta 运行的 DAW，因此需要安装 Ninja。

## 依赖准备

```bash
git clone -b standalone https://github.com/YuFeng926/OpenTune.git
cd OpenTune
```

> **重要：所有三方依赖均不在 Git 仓库中**（`JUCE-master/` 与 `ThirdParty/` 已加入 `.gitignore`，避免数百 MB 二进制 blob 进入历史）。请按下列步骤分别获取并放置到指定路径。
>
> **CMake 自动检测：** 依赖按下方规范放置后，配置时**无需任何 `-D` 覆盖**。CMakeLists.txt 在缓存路径失效时会自动 fallback 到 `ThirdParty/<dep>/` 的 bundled 默认路径。

### 1. JUCE Framework

克隆到项目根目录（**JUCE 例外，不在 `ThirdParty/`**），文件夹名必须为 `JUCE-master`：

```bash
git clone https://github.com/juce-framework/JUCE.git JUCE-master
```

### 2. ARA SDK

```bash
cd ThirdParty
git clone --recursive --branch releases/2.3.0 https://github.com/Celemony/ARA_SDK.git ARA_SDK-releases-2.3.0
cd ..
```

> ⚠️ **ARA 2.3.0 需要同步修补本地 JUCE 分支**：`JUCE-master/`（被 gitignore，不随仓库分发）的
> `modules/juce_audio_plugin_client/juce_audio_plugin_client_ARA.cpp` 仍 include 2.2.0 的
> `ARA_Library/Utilities/ARAChannelArrangement.cpp`，而 2.3.0 已将该文件改名为 `ARAChannelFormat.cpp`。
> 克隆 SDK 后必须把该行改为 `#include <ARA_Library/Utilities/ARAChannelFormat.cpp>`，
> 否则编译 `juce_audio_plugin_client_ARA.cpp` 时报 C1083 找不到包含文件。

### 3. r8brain Resampler

```bash
cd ThirdParty
git clone https://github.com/avaneev/r8brain-free-src.git r8brain-free-src-master
cd ..
```

### 4. ONNX Runtime（Windows v1.24.4 / macOS v1.20.0 universal2）

本项目需要 **两个** ONNX Runtime 包（Windows）：CPU 版提供头文件，DML 版提供原始 `onnxruntime.dll`（内置 DirectML 支持）。构建系统生成专用导入库，并把运行时 DLL 输出为 `OpenTuneOnnxRuntime_1_24_4.dll`。

**Windows** — 下载并解压到 `ThirdParty/`：

| 包 | 链接 | 解压到 |
|----|------|--------|
| ONNX Runtime CPU | [onnxruntime-win-x64-1.24.4.zip](https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-win-x64-1.24.4.zip) | `ThirdParty/onnxruntime-win-x64-1.24.4/` |
| ONNX Runtime DirectML | [Microsoft.ML.OnnxRuntime.DirectML.1.24.4.nupkg](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.24.4) | `ThirdParty/onnxruntime-dml-1.24.4/` |

> **提示：** 将 `.nupkg` 改名为 `.zip` 后解压。

**目录结构说明（关键路径）：**

```
ThirdParty/
├── onnxruntime-win-x64-1.24.4/       ← CPU 版（编译时链接用）
│   ├── include/
│   │   └── onnxruntime_cxx_api.h      ← CMake 检测此文件是否存在
│   └── lib/
│       └── onnxruntime.lib            ← 上游包文件；OpenTune 不直接链接
│
└── onnxruntime-dml-1.24.4/            ← DML 版（运行时 DLL + DML provider 头文件）
    ├── build/native/include/
    │   └── dml_provider_factory.h     ← DirectML EP 注册头文件
    └── runtimes/win-x64/native/
        └── onnxruntime.dll             ← 上游源 DLL；构建时改名复制
```

**为什么需要两个包？**
- **CPU 包** (`onnxruntime-win-x64-1.24.4`)：提供 C++ API 头文件（`onnxruntime_cxx_api.h`）。
- **DML 包** (`onnxruntime-dml-1.24.4`)：提供编译了 DirectML Execution Provider 的原始 `onnxruntime.dll` 和 `dml_provider_factory.h`。CMake 根据项目内 `.def` 生成 `OpenTuneOnnxRuntime_1_24_4.lib`，并把源 DLL 改名部署为 `OpenTuneOnnxRuntime_1_24_4.dll`。

**运行时解析顺序（Windows）**：应用优先加载自身目录中的 `OpenTuneOnnxRuntime_<版本>.dll`（Standalone 为 exe 同目录，VST3 为 bundle 的 `Contents\x86_64-win`），`Program Files\OpenTune` 共享安装只作兜底——避免旧安装静默遮蔽随包分发的那一份。文件名中的 ORT 版本号即 ABI 契约：升级 ONNX Runtime 必须同步修改 `OPENTUNE_ORT_DLL_NAME` 与安装器 `[Files]` 中的源文件名（两处不一致时 ISCC 会直接编译失败，不会静默打错包）；旧版本文件由安装器 `[InstallDelete]` 的 `OpenTuneOnnxRuntime_*.dll` 通配符清理，`scripts/validate-windows-release.ps1` 从 CMakeLists 读取期望名并断言产物中恰好一份。模型由安装器统一装到 `%ProgramData%\OpenTune\Models`，独立版与 VST3 共用一份（便携 ZIP 仍随包放在 exe 同目录）。

**macOS（Intel / Apple Silicon 通用）**：

macOS 两个架构共用同一个 **universal2** 包（x86_64 + arm64 双架构，CoreML EP 已内置），版本固定为 **1.20.0**。项目不直接使用官方 macOS 预编译包，而是从 ORT v1.20.0 源码分别构建 arm64/x86_64，并以 `CMAKE_OSX_DEPLOYMENT_TARGET = 12.0` 合并为 universal2；这样保留 macOS 12 兼容性，同时包含 macOS 15 CoreML 配置初始化修复。

```bash
./scripts/build-onnxruntime-macos.sh
```

首次构建需要下载 ORT 源码及子模块，耗时可能较长。若需清理该脚本创建的源码、构建和输出目录后重建：

```bash
./scripts/build-onnxruntime-macos.sh --clean
```

解压后结构：
```
ThirdParty/onnxruntime-osx-universal2-1.20.0/
├── include/
│   └── onnxruntime_cxx_api.h
└── lib/
    └── libonnxruntime.1.20.0.dylib
```

> **CoreML 注册路径**：macOS 12 使用 `NeuralNetwork` 格式，macOS 13+ 使用 `MLProgram` 格式；两者都使用 `MLComputeUnits=ALL`。项目通过 `Source/Inference/OnnxRuntimeProviderCompat.h` 直接调用 ORT 的 CoreML flag API 注册 provider。日志中会明确显示 `NeuralNetwork/macOS 12` 或 `MLProgram/macOS 13+`。

### 5. DirectML & DirectX Agility SDK（仅 Windows）

这两个 NuGet 包提供 GPU 加速推理所需的 DirectML 运行时和最新版 D3D12 支持。

| 包 | 版本 | 下载 | 解压到 |
|----|------|------|--------|
| Microsoft.AI.DirectML | 1.15.4 | [NuGet](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4) | `ThirdParty/microsoft.ai.directml.1.15.4/` |
| Microsoft.Direct3D.D3D12 | 1.619.5 | [NuGet](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.5) | `ThirdParty/microsoft.direct3d.d3d12.1.619.5/` |

> 下载 `.nupkg` 后改名为 `.zip` 解压。

**CMake 会检测以下关键文件：**

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
    │   └── d3dx12/          ← 辅助头文件目录
    └── bin/x64/
        ├── D3D12Core.dll
        └── D3D12SDKLayers.dll
```

### 6. AI 模型

模型文件需放在项目根目录下的指定位置，构建时会自动复制到输出目录：

| 模型 | 源路径 | 构建后位置 |
|------|--------|-----------|
| FCPE (F0 提取) | `models/fcpe.onnx` | `<output>/models/fcpe.onnx` |
| PC-NSF HifiGAN (声码器) | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` | `<output>/models/hifigan.onnx` |
| GAME 音符生成（可选） | `models/GAME/` | `<output>/models/GAME/` |

> 模型文件不包含在 Git 仓库中，请从 [Releases](https://github.com/YuFeng926/OpenTune/releases) 页面下载或联系维护者获取。GAME 模型包为可选，缺失时运行时回退到 Legacy 流程。

## 完整目录结构概览

配置完所有依赖后，项目根目录应如下所示：

```
OpenTune/
├── CMakeLists.txt
├── JUCE-master/                          ← JUCE 框架
├── ThirdParty/
│   ├── ARA_SDK-releases-2.3.0/           ← ARA SDK
│   ├── r8brain-free-src-master/          ← 重采样库
│   ├── onnxruntime-win-x64-1.24.4/      ← ONNX Runtime CPU (Windows)
│   ├── onnxruntime-dml-1.24.4/           ← ONNX Runtime DML (Windows)
│   ├── onnxruntime-osx-universal2-1.20.0/ ← ONNX Runtime (macOS，Intel + Apple Silicon 通用)
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

## 构建命令

**Windows (Visual Studio + CMake)**

```powershell
# 生成 ARA2 VST3 VS 解决方案（必须使用 "Visual Studio 17 2022" 生成器）。
# Codex/桌面 shell 下配置和编译都使用 PATH workaround，避免 Path/PATH 撞键影响 MSBuild。
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --preset windows-ara-vs2022"

# 编译 Release 版本
cmd /v/on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build --preset windows-ara-release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build --preset windows-ara-release --target OpenTuneStandalone_Standalone"
```

**Windows (Ninja + CMake)**

```powershell
# 使用 Ninja 快速构建（需要 Ninja 在 PATH 中）
cmake --preset windows-ara-ninja
cmake --build --preset windows-ara-ninja-release
```

Release 构建完成后，发布前必须校验 Standalone 产物。校验与打包脚本为本地维护，
不随仓库分发（`scripts/build-ninja.ps1`、`scripts/validate-windows-release.ps1`、
`scripts/package-windows.ps1`、`Installer/`），需从本机获取。
`scripts/build-ninja.ps1` 在 Release 构建结束时会自动执行同一校验；也可以单独执行：

```powershell
.\scripts\validate-windows-release.ps1
```

校验会确认 `OpenTune.exe` 为当前 `CMakeLists.txt` 中的版本，并确认
ONNX Runtime、DirectML、D3D12 和 FCPE/HifiGAN 模型都位于同一套 Release 产物中。
校验通过后使用 `scripts/package-windows.ps1` 编译 Inno Setup 安装包；该脚本会在
调用 Inno Setup 前再次执行校验，避免直接把旧的 `build-ara-ninja` 产物打进新版安装包。
安装器完成后还会验证安装目录中的文件和 exe 版本；验证失败会中止安装流程。

同一个 ARA2 VST3 二进制在未绑定 ARA 的普通 VST3 宿主中会自然回退到 Capture 流程，
不再单独生成 non-ARA 插件。

如需在 Visual Studio IDE 中开发：
1. 执行上述 `cmake --preset ...` 命令
2. 打开 `build-ara-overlay-vs18-clean/OpenTune.sln`
3. 将 `OpenTuneStandalone_Standalone` 或 `OpenTune_VST3` 设为启动项目
4. 选择 Release/x64 配置，编译运行

**macOS (Ninja + CMake)**

```bash
# macOS Universal2 Release (x86_64 + arm64, minimum macOS 12.0)
cmake --preset macos-universal2-ara-ninja
cmake --build --preset macos-universal2-ara-release
```

打包命令：
```bash
# 默认生成 Universal2 PKG
./scripts/package-macos.sh

# 可选：生成 Universal2 DMG
./scripts/package-macos.sh --format dmg

# 同时生成 PKG 与 DMG
./scripts/package-macos.sh --format both
```

PKG 双击安装到 `/Applications`，并将 VST3 安装到系统级 `/Library/Audio/Plug-Ins/VST3/`；DMG 内含 Standalone、VST3 和中英文安装命令，命令脚本将 VST3 放入当前用户的插件目录。PKG 欢迎页展示 `Installer/NOTICE.md` 中的防诈与许可声明。

## 构建产物

| 格式 | Windows | macOS |
|------|---------|-------|
| Standalone | `build-ara-overlay-vs18-clean/OpenTuneStandalone_artefacts/Release/Standalone/OpenTune.exe` | `build-macos-universal2-ninja/OpenTuneStandalone_artefacts/Release/Standalone/OpenTune.app` |
| VST3 ARA2 | `build-ara-overlay-vs18-clean/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` | `build-macos-universal2-ninja/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` |

构建完成后，运行时 DLL、模型文件、D3D12 目录会自动复制到产物目录旁，无需手动操作。

## 常见构建问题

| 症状 | 原因 | 解决方法 |
|------|------|----------|
| `ONNX Runtime C++ API header not found` | CPU 版 ONNX Runtime 未放对位置 | 确认 `ThirdParty/onnxruntime-win-x64-1.24.4/include/onnxruntime_cxx_api.h` 存在 |
| `DirectML provider header missing` | DML 版 NuGet 包未正确解压 | 确认 `ThirdParty/onnxruntime-dml-1.24.4/build/native/include/dml_provider_factory.h` 存在 |
| `DirectML header missing` | DirectML NuGet 包未解压 | 确认 `ThirdParty/microsoft.ai.directml.1.15.4/include/DirectML.h` 存在 |
| `D3D12 header missing from Agility SDK` | D3D12 NuGet 包未解压 | 确认 `ThirdParty/microsoft.direct3d.d3d12.1.619.5/build/native/include/d3d12.h` 存在 |
| `ONNX Runtime DirectML DLL missing` | DML 版运行时 DLL 缺失 | 确认 `ThirdParty/onnxruntime-dml-1.24.4/runtimes/win-x64/native/onnxruntime.dll` 存在 |
| `ONNX Runtime dylib not found` | macOS universal2 包未构建或版本不符 | 执行 `./scripts/build-onnxruntime-macos.sh`，确认 `ThirdParty/onnxruntime-osx-universal2-1.20.0/lib/libonnxruntime.1.20.0.dylib` 存在 |
| `ARA SDK not found` | ARA SDK 未克隆 | 执行步骤 2 的 git clone 命令 |
| MSVC 链接错误 LNK2019 | MSVC 运行时不匹配 | 本项目使用静态 CRT (`/MT`)，确保依赖库一致 |
| Ninja 构建失败 | Ninja 未安装或不在 PATH 中 | 确保 Ninja 已安装并在系统 PATH 中，或使用 Visual Studio Generator |

## 开发指引

- **代码风格**：C++17，JUCE 命名规范
- **提交规范**：简洁描述变更目的（中英文均可）
- **UI 隔离**：`Source/Standalone/` 为独立版 UI，`Source/Plugin/` 为 VST3 版 UI，通过 `JucePlugin_Build_Standalone` 宏隔离
- **Processor 共享**：`Source/PluginProcessor.*` 为两种格式共用，修改时需兼顾两个构建目标
