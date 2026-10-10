# 下载与安装

> 安装发布包遇到问题看这里；从源码构建请看 [BUILDING.md](BUILDING.md)，项目介绍见 [README](README.md)。

请前往 [Releases](https://github.com/YuFeng926/OpenTune/releases) 页面下载最新安装包。

## Windows
解压 ZIP 包后直接运行 `OpenTune.exe`，需保持 `models/`、`D3D12/`、DLL 文件与 exe 同目录。
将 `OpenTune.vst3` 文件夹复制到 `C:\Program Files\Common Files\VST3\` 即可在 DAW 中加载插件。

## macOS
发布包只有一个 `Universal2` 架构，适用于 Intel Mac、Apple Silicon 原生运行，以及 Apple Silicon 上通过 Rosetta 运行的 DAW。优先下载 `OpenTune-<version>-macOS-Universal2.pkg`，双击并按提示安装：Standalone 安装到 `/Applications`，VST3 安装到系统级 `/Library/Audio/Plug-Ins/VST3/`。

也可下载 `OpenTune-<version>-macOS-Universal2.dmg`。挂载后双击 **安装 OpenTune.command** 或 **Install OpenTune.command**；脚本将 Standalone 放入 `/Applications`，并将 VST3 放入当前用户的 `~/Library/Audio/Plug-Ins/VST3/`。DMG 也包含 `.app` 和 `.vst3`，可手动复制安装。

> ⚠️ Standalone 和 VST3 目前为 ad-hoc 签名；PKG 本身未做 Developer ID Installer 签名或 Apple 公证。首次安装或启动可能被 Gatekeeper 拦截。若 macOS 阻止打开，可在「系统设置 → 隐私与安全性」选择仍要打开，或清除应用/插件的隔离标志：
>
> ```bash
> xattr -rd com.apple.quarantine /Applications/OpenTune.app
> xattr -rd com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/OpenTune.vst3"
> xattr -rd com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/VST3/OpenTune.vst3"
> ```

## 运行时文件结构（Windows）

```
OpenTune/
├── OpenTune.exe
├── OpenTuneOnnxRuntime_1_24_4.dll ← ONNX Runtime (内置 DirectML)
├── DirectML.dll             ← DirectML 运行时
├── D3D12/
│   ├── D3D12Core.dll        ← DirectX Agility SDK
│   └── D3D12SDKLayers.dll
├── models/
│   ├── fcpe.onnx            ← 音高提取模型 (FCPE)
│   └── hifigan.onnx         ← 声码器模型
└── docs/
    └── UserGuide.html
```

> 上表为 ZIP 便携版布局（模型与 exe 同目录）。用安装器安装时，模型统一放在
> `%ProgramData%\OpenTune\Models`，由独立版与 VST3/ARA 共用一份；其余文件结构相同。
> 自定义安装目录不影响模型解析（按"自带目录优先、共享目录兜底"顺序查找）。
