# 下载与安装

> 安装发布包遇到问题看这里；从源码构建请看 [BUILDING.md](BUILDING.md)，项目介绍见 [README](README.md)。

请前往 [Releases](https://github.com/YuFeng926/OpenTune/releases) 页面下载最新安装包。

## Windows
解压 ZIP 包后直接运行 `OpenTune.exe`，需保持 `models/`、`D3D12/`、DLL 文件与 exe 同目录。
将 `OpenTune.vst3` 文件夹复制到 `C:\Program Files\Common Files\VST3\` 即可在 DAW 中加载插件。

## macOS
1. 根据 Mac 架构下载对应的 `OpenTune-<version>-macOS-Intel.dmg` 或 `OpenTune-<version>-macOS-Apple-Silicon.dmg`，双击挂载。
2. 在挂载出的磁盘窗口中双击 **install.command**（如首次执行被 Gatekeeper 拦截，请到「系统设置 → 隐私与安全性」点击 *仍要打开*；或在 DMG 窗口右键脚本 → *打开*）。
3. 终端会自动执行：
    - 把 `OpenTune.app` 拷贝到 `/Applications/` 并去除 quarantine 属性；
    - 询问是否同时安装 VST3。同意后会请求管理员密码，把 `OpenTune.vst3` 安装到系统级 `/Library/Audio/Plug-Ins/VST3/`（DAW 全局可见）；
    - 询问是否立即启动 Standalone。
4. 安装完成后即可弹出并丢弃 DMG。如需更新，重新执行该脚本会覆盖旧版本。

> ⚠️ Release 包目前为 ad-hoc 签名（未做 Apple Notarization），首次启动可能弹出「无法验证开发者」提示。`install.command` 会自动剥离 quarantine 标志，若你绕过脚本手工拖拽 `.app`，则需要执行：
>
> ```bash
> xattr -rd com.apple.quarantine /Applications/OpenTune.app
> xattr -rd com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/OpenTune.vst3"
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
