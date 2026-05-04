# Phase 06-04 Build Comparison Report

## 1) 主二进制大小对比

| 格式 | Debug (MB) | Release (MB) |
|---|---:|---:|
| Standalone (`OpenTune.exe`) | 41.12 | 16.03 |
| VST3 (`OpenTune.vst3`) | 42.40 | 16.37 |
| 差异 (VST3-Standalone) | +1.28 | +0.34 |

结论：VST3 主二进制略大，符合包含 ARA/VST3 客户端代码的预期。

## 2) 运行时依赖对比（Debug）

| 文件 | Standalone (MB) | VST3 (MB) | 哈希一致 |
|---|---:|---:|---|
| onnxruntime.dll | 16.53 | 16.53 | ✅ |
| DirectML.dll | 17.67 | 17.67 | ✅ |
| D3D12Core.dll | 4.78 | 4.78 | ✅ |
| D3D12SDKLayers.dll | 4.73 | 4.73 | ✅ |

结论：两个格式依赖 DLL 完全一致。

## 3) 模型文件对比（Debug）

| 文件 | Standalone (bytes) | VST3 (bytes) | 哈希一致 |
|---|---:|---:|---|
| hifigan.onnx | 133 | 133 | ✅ |
| rmvpe.onnx | 134 | 134 | ✅ |

说明：当前模型文件为极小占位文件（非完整模型），但两格式一致。

## 4) 总部署体积对比

| 格式 | Debug 总大小 (MB) | Release 总大小 (MB) | 文件数（Debug） |
|---|---:|---:|---:|
| Standalone 输出目录 | 233.91 | 59.79 | 10 |
| VST3 Bundle | 86.14 | 60.11 | 11 |

说明：Standalone Debug 包含较大的 `OpenTune.pdb`，导致 Debug 体积显著高于 VST3 bundle。

## 5) 结构差异

### Standalone
- 目录：`build-standalone/OpenTune_artefacts/{Config}/Standalone/`
- 主程序：`OpenTune.exe`
- 依赖与模型：与主程序同级（含 `D3D12/`、`models/`）

### VST3
- 目录：`build-vst3/OpenTune_artefacts/{Config}/VST3/OpenTune.vst3/`
- 主插件：`Contents/x86_64-win/OpenTune.vst3`
- 依赖与模型：当前也位于 `Contents/x86_64-win/` 下（含 `D3D12/`、`models/`）

## 6) 部署建议

### Standalone
复制整个 `Standalone` 目录即可运行，必须包含：
- `OpenTune.exe`
- `onnxruntime.dll` / `DirectML.dll`
- `D3D12/` 子目录
- `models/` 子目录

### VST3
复制整个 `OpenTune.vst3` bundle 到 VST3 插件目录（Windows 常见目录：`C:\Program Files\Common Files\VST3\`）。

### 共同注意事项
- 两格式当前依赖版本一致，便于统一升级。
- 当前 `models/*.onnx` 体积异常小（占位文件），若用于真实运行需替换为正式模型。
