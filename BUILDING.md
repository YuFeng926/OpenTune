# 构建说明

> 本文档面向希望从源码构建 OpenTune 的开发者。安装发布包请看 [INSTALL.md](INSTALL.md)，项目介绍见 [README](README.md)。

## 环境要求

| 需求 | Windows | macOS |
|------|---------|-------|
| **系统** | Windows 10 1903+ | macOS 13.4+ (Intel) / 14.0+ (Apple Silicon) |
| **架构** | x64 | x86_64 (Intel) / arm64 (Apple Silicon) |
| **编译器** | Visual Studio 2022 (MSVC 17+) | Xcode Command Line Tools / Apple Clang |
| **CMake** | 3.22+ | 3.22+ |
| **C++ 标准** | C++17 | C++17 |
| **构建系统** | MSBuild (VS Generator) / Ninja | Ninja |

> **注意：** Windows 构建支持 Visual Studio Generator + MSBuild 或 Ninja 两种方式。推荐使用 Visual Studio Generator 进行完整开发，Ninja 适合快速构建。
> macOS Release 与 DMG 打包使用 `macos-silicon-ara-ninja`（Apple Silicon）或 `macos-intel-ara-ninja`（Intel）预设，因此需要安装 Ninja。

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
git clone --recursive --branch releases/2.2.0 https://github.com/Celemony/ARA_SDK.git ARA_SDK-releases-2.2.0
cd ..
```

### 3. r8brain Resampler

```bash
cd ThirdParty
git clone https://github.com/avaneev/r8brain-free-src.git r8brain-free-src-master
cd ..
```

### 4. ONNX Runtime（Windows v1.24.4 / macOS Intel v1.23.0 / macOS Apple Silicon v1.24.4）

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

**macOS (Intel, x86_64)**：

macOS Intel 使用仓库中现有的 universal2 包（实际按 x86_64 构建），CoreML EP 已内置。该运行库最低支持 macOS 13.4：

```
ThirdParty/onnxruntime-osx-universal2-1.23.0/   ← 已包含在仓库中，无需额外下载
├── include/
│   └── onnxruntime_cxx_api.h
└── lib/
    └── libonnxruntime.1.23.0.dylib
```

**macOS (Apple Silicon, arm64)**：

macOS Apple Silicon 使用 arm64 v1.24.4 包，CoreML EP 已内置。该运行库最低支持 macOS 14.0。**请手动下载并解压到指定目录**（仓库不包含此文件）：

```bash
cd ThirdParty
curl -L https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-osx-arm64-1.24.4.tgz | tar xz
cd ..
```

解压后结构：
```
ThirdParty/onnxruntime-osx-arm64-1.24.4/
├── include/
│   └── onnxruntime_cxx_api.h
└── lib/
    └── libonnxruntime.1.24.4.dylib
```

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
| RMVPE (F0 提取) | `models/rmvpe.onnx` | `<output>/models/rmvpe.onnx` |
| PC-NSF HifiGAN (声码器) | `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` | `<output>/models/hifigan.onnx` |

> 模型文件不包含在 Git 仓库中，请从 [Releases](https://github.com/YuFeng926/OpenTune/releases) 页面下载或联系维护者获取。

## 完整目录结构概览

配置完所有依赖后，项目根目录应如下所示：

```
OpenTune/
├── CMakeLists.txt
├── JUCE-master/                          ← JUCE 框架
├── ThirdParty/
│   ├── ARA_SDK-releases-2.2.0/           ← ARA SDK
│   ├── r8brain-free-src-master/          ← 重采样库
│   ├── onnxruntime-win-x64-1.24.4/      ← ONNX Runtime CPU (Windows)
│   ├── onnxruntime-dml-1.24.4/           ← ONNX Runtime DML (Windows)
│   ├── onnxruntime-osx-universal2-1.23.0/ ← ONNX Runtime (macOS Intel, universal2)
│   ├── onnxruntime-osx-arm64-1.24.4/     ← ONNX Runtime (macOS Apple Silicon, arm64)
│   ├── microsoft.ai.directml.1.15.4/    ← DirectML SDK (Windows)
│   └── microsoft.direct3d.d3d12.1.619.5/ ← D3D12 Agility SDK (Windows)
├── models/
│   └── rmvpe.onnx
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
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build --preset windows-ara-release --target OpenTune_Standalone"
```

**Windows (Ninja + CMake)**

```powershell
# 使用 Ninja 快速构建（需要 Ninja 在 PATH 中）
cmake --preset windows-ara-ninja
cmake --build --preset windows-ara-ninja-release
```

同一个 ARA2 VST3 二进制在未绑定 ARA 的普通 VST3 宿主中会自然回退到 Capture 流程，
不再单独生成 non-ARA 插件。

如需在 Visual Studio IDE 中开发：
1. 执行上述 `cmake --preset ...` 命令
2. 打开 `build-ara-overlay-vs18-clean/OpenTune.sln`
3. 将 `OpenTune_Standalone` 或 `OpenTune_VST3` 设为启动项目
4. 选择 Release/x64 配置，编译运行

**macOS (Ninja + CMake)**

```bash
# Apple Silicon (arm64)
cmake --preset macos-silicon-ara-ninja
cmake --build --preset macos-silicon-ara-release

# Intel (x86_64)
cmake --preset macos-intel-ara-ninja
cmake --build --preset macos-intel-ara-release
```

打包命令：
```bash
# Apple Silicon
./scripts/package-macos.sh --arch silicon

# Intel
./scripts/package-macos.sh --arch intel
```

## 构建产物

| 格式 | Windows | macOS |
|------|---------|-------|
| Standalone | `build-ara-overlay-vs18-clean/OpenTune_artefacts/Release/Standalone/OpenTune.exe` | `build/OpenTune_artefacts/Release/Standalone/OpenTune.app` |
| VST3 ARA2 | `build-ara-overlay-vs18-clean/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` | `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3/` |

构建完成后，运行时 DLL、模型文件、D3D12 目录会自动复制到产物目录旁，无需手动操作。

## 常见构建问题

| 症状 | 原因 | 解决方法 |
|------|------|----------|
| `ONNX Runtime C++ API header not found` | CPU 版 ONNX Runtime 未放对位置 | 确认 `ThirdParty/onnxruntime-win-x64-1.24.4/include/onnxruntime_cxx_api.h` 存在 |
| `DirectML provider header missing` | DML 版 NuGet 包未正确解压 | 确认 `ThirdParty/onnxruntime-dml-1.24.4/build/native/include/dml_provider_factory.h` 存在 |
| `DirectML header missing` | DirectML NuGet 包未解压 | 确认 `ThirdParty/microsoft.ai.directml.1.15.4/include/DirectML.h` 存在 |
| `D3D12 header missing from Agility SDK` | D3D12 NuGet 包未解压 | 确认 `ThirdParty/microsoft.direct3d.d3d12.1.619.5/build/native/include/d3d12.h` 存在 |
| `ONNX Runtime DirectML DLL missing` | DML 版运行时 DLL 缺失 | 确认 `ThirdParty/onnxruntime-dml-1.24.4/runtimes/win-x64/native/onnxruntime.dll` 存在 |
| `ARA SDK not found` | ARA SDK 未克隆 | 执行步骤 2 的 git clone 命令 |
| MSVC 链接错误 LNK2019 | MSVC 运行时不匹配 | 本项目使用静态 CRT (`/MT`)，确保依赖库一致 |
| Ninja 构建失败 | Ninja 未安装或不在 PATH 中 | 确保 Ninja 已安装并在系统 PATH 中，或使用 Visual Studio Generator |

## 开发指引

- **代码风格**：C++17，JUCE 命名规范
- **提交规范**：简洁描述变更目的（中英文均可）
- **UI 隔离**：`Source/Standalone/` 为独立版 UI，`Source/Plugin/` 为 VST3 版 UI，通过 `JucePlugin_Build_Standalone` 宏隔离
- **Processor 共享**：`Source/PluginProcessor.*` 为两种格式共用，修改时需兼顾两个构建目标
