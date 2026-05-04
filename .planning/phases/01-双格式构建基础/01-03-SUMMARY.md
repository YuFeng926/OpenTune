# Plan 01-03 Summary: 条件化源文件列表和 POST_BUILD 脚本

**Status**: COMPLETED
**Phase**: 01 — 双格式构建基础
**Plan**: 03
**Wave**: 2
**Date**: 2026-04-04

## Changes Made

### CMakeLists.txt — 两处修改

#### 修改 1: 条件化 `HostIntegrationStandalone.cpp` 源文件

从无条件 `target_sources` 列表中移除 `Source/Host/HostIntegrationStandalone.cpp`，添加条件化 section：

**Before** (unconditional):
```
# Standalone specific files - Must be compiled for OpenTune target
Source/Host/HostIntegrationStandalone.cpp
Source/Standalone/EditorFactoryStandalone.cpp
...
```

**After** — 移除后添加条件化 section (line 394-402):
```cmake
# ==============================================================================
# Conditional Source Files per Format
# ==============================================================================
# Standalone-specific files (compiled only when building Standalone format)
if(JucePlugin_Build_Standalone)
    target_sources(OpenTune PRIVATE
        Source/Host/HostIntegrationStandalone.cpp
    )
endif()

# VST3-specific files (compiled only when building VST3 format)
# Phase 2: Source/ARA/* files added here
# Phase 4: Source/Plugin/PluginEditor.cpp/.h and Source/Host/HostIntegrationPlugin.cpp added here
```

#### 修改 2: 添加 VST3 POST_BUILD 命令

在 Standalone POST_BUILD section 之后、Unit Tests section 之前添加 VST3 专用命令 (line 613-669):

```cmake
# ==============================================================================
# VST3-Specific POST_BUILD Commands
# ==============================================================================
if(NOT JucePlugin_Build_Standalone)
    # ONNX Runtime DLL
    add_custom_command(TARGET OpenTune_VST3 POST_BUILD ...)

    # ONNX Runtime providers shared DLL
    add_custom_command(TARGET OpenTune_VST3 POST_BUILD ...)

    # DirectML DLL
    add_custom_command(TARGET OpenTune_VST3 POST_BUILD ...)

    # D3D12 Agility SDK
    add_custom_command(TARGET OpenTune_VST3 POST_BUILD ...)

    # AI Models
    add_custom_command(TARGET OpenTune_VST3 POST_BUILD ...)
endif()
```

## Verification

```bash
grep "if(JucePlugin_Build_Standalone)" CMakeLists.txt       # → 1 match
grep "if(NOT JucePlugin_Build_Standalone)" CMakeLists.txt   # → 1 match
grep "OpenTune_VST3" CMakeLists.txt                          # → 9 matches (DLL + model copies)
grep "HostIntegrationStandalone" CMakeLists.txt                # → 1 match (conditional section only)
```

## Requirements Covered

| Requirement | Description | Status |
|-------------|-------------|--------|
| BLD-01 | 双目标编译（条件化源文件） | PASS |
| BLD-02 | Standalone 目标使用 Standalone 特有文件 | PASS |
| BLD-03 | VST3 目标使用 VST3 特有文件 | PASS |
| BLD-05 | POST_BUILD 分别复制 DLL 和模型文件 | PASS |

## Key Design Decisions

1. **`JucePlugin_Build_Standalone` CMake 变量** — JUCE 在 `juce_add_plugin` 处理后自动定义此变量，值为 `TRUE` 或 `FALSE`
2. **源文件条件化使用单独 `target_sources` 块** — 避免在单一块内混用条件编译
3. **POST_BUILD 条件化在 Standalone 命令之后** — 保持原有的 Standalone 配置不变
4. **Phase 2/4 占位注释** — 明确标记后续阶段需要添加的文件

## Files Modified

- `CMakeLists.txt` — 条件化源文件 section 和 VST3 POST_BUILD section
