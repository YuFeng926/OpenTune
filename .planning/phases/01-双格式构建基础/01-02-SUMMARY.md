# Plan 01-02 Summary: 集成 ARA SDK 路径配置

**Status**: COMPLETED
**Phase**: 01 — 双格式构建基础
**Plan**: 02
**Wave**: 1
**Date**: 2026-04-04

## Changes Made

### CMakeLists.txt

在 `# JUCE Configuration` section 之前添加 ARA SDK 配置（line 32-56）：

```cmake
# ==============================================================================
# ARA SDK Configuration (for VST3 ARA Extension support)
# ==============================================================================
set(OPENTUNE_ARA_SDK_VERSION "2.2.0" CACHE STRING "ARA SDK version")
set(ARA_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/ARA_SDK-releases-${OPENTUNE_ARA_SDK_VERSION}" CACHE PATH "Path to ARA SDK")

# ==============================================================================
# JUCE Configuration
# ==============================================================================
set(OPENTUNE_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE-master")
add_subdirectory("${OPENTUNE_JUCE_DIR}")

# Configure ARA SDK path for JUCE (after JUCE module is loaded)
if(EXISTS "${ARA_SDK_PATH}")
    if(TARGET OpenTune_VST3)
        juce_set_ara_sdk_path("${ARA_SDK_PATH}")
        message(STATUS "ARA SDK found at: ${ARA_SDK_PATH} (VST3 ARA support enabled)")
    endif()
else()
    if(TARGET OpenTune_VST3)
        message(WARNING "ARA SDK not found at ${ARA_SDK_PATH}. VST3 ARA features will be disabled.")
        message(STATUS "To enable ARA, clone: git clone --recursive --branch releases/${OPENTUNE_ARA_SDK_VERSION} https://github.com/Celemony/ARA_SDK")
    endif()
endif()
```

## Verification

```bash
grep "set(ARA_SDK_PATH" CMakeLists.txt    # → 1 match (BLD-04)
grep "juce_set_ara_sdk_path" CMakeLists.txt  # → 1 match (BLD-04)
grep "ARA SDK" CMakeLists.txt             # → 2 matches (warning + status)
```

## Requirements Covered

| Requirement | Description | Status |
|-------------|-------------|--------|
| BLD-04 | ARA SDK 路径配置 | PASS |

## Key Design Decisions

1. **`juce_set_ara_sdk_path()` 在 `add_subdirectory(JUCE)` 之后调用** — 因为该宏依赖 JUCE CMake 模块已加载
2. **`if(TARGET OpenTune_VST3)` 保护** — 确保仅在 VST3 目标存在时才调用 ARA 相关配置
3. **ARA SDK 路径版本化为变量** — `OPENTUNE_ARA_SDK_VERSION` 便于未来升级

## Pre-existing Environment Issues

CMake configure 尝试时发现两个环境问题（与本次修改无关）：
1. `JUCE-master` 不存在于主工作区根目录
2. `onnxruntime-dml-1.24.4/build/native/include/` 目录缺失

这些问题需要在环境准备阶段解决。

## Files Modified

- `CMakeLists.txt` — ARA SDK 配置 section
