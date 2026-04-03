# OpenTune macOS 移植计划

## 概述

本文档描述将 OpenTune 从 Windows 专属移植至 macOS 平台的技术方案。目标是同时支持 Apple Silicon (arm64) 和 Intel Mac (x86_64)，分别构建和分发标准 `.app` 包（不上 Mac App Store）。

---

## 目录

1. [当前平台依赖分析](#1-当前平台依赖分析)
2. [构建系统方案](#2-构建系统方案)
3. [ONNX Runtime 集成](#3-onnx-runtime-集成)
4. [SIMD 加速方案](#4-simd-加速方案)
5. [GPU 加速策略](#5-gpu-加速策略)
6. [系统 API 替代](#6-系统-api-替代)
7. [App Bundle 打包与模型分发](#7-app-bundle-打包与模型分发)
8. [代码签名与公证](#8-代码签名与公证)
9. [风险评估](#9-风险评估)
10. [实施路线图](#10-实施路线图)
11. [验证步骤 (Spike)](#11-验证步骤-spike)

---

## 1. 当前平台依赖分析

### 架构优势：音频线程与推理完全解耦

OpenTune 的实时音频管线对 macOS 移植非常友好。ONNX 推理**不在音频回调线程上运行**，架构采用经典的预计算+缓存模式：

```
┌─────────────────────────────────────────────────────────────────┐
│                  OpenTune 实时音频管线架构                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────┐                                            │
│  │  UI Thread       │──── enqueuePartialRender()                │
│  │  (用户编辑pitch) │              │                             │
│  └─────────────────┘              ▼                             │
│                          ┌────────────────┐                     │
│                          │ Chunk Worker   │ (单线程)             │
│                          │ Mel谱 + F0插值  │                     │
│                          └───────┬────────┘                     │
│                                  ▼                              │
│                       ┌──────────────────┐                      │
│                       │ LockFreeQueue    │                      │
│                       └────────┬─────────┘                      │
│                                ▼                                │
│               ┌────────────────────────────────┐                │
│               │  RenderingManager Workers (N/2) │               │
│               │  ★ ONNX HiFiGAN 推理在这里 ★     │               │
│               │  vocoder->synthesize()          │               │
│               └────────────────┬───────────────┘                │
│                                ▼                                │
│                       ┌────────────────┐                        │
│                       │  RenderCache   │ SpinLock (try-lock)    │
│                       └────────┬───────┘                        │
│                                ▼                                │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Audio Thread (processBlock)                              │   │
│  │  - 只做内存读取 + float 乘加                               │   │
│  │  - 非阻塞读缓存：命中->渲染音频，未命中->原始干信号           │   │
│  │  - 无 ONNX，无 FFT，无重采样                               │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

这意味着 macOS 移植的性能风险较低。音频线程极其轻量，真正的性能敏感点在后台渲染线程。

### Windows 特定代码集中在 6 个关键区域

| 区域 | 关键文件 | 问题描述 |
|------|---------|---------|
| ONNX Runtime 链接 | `CMakeLists.txt:32,255` | 硬编码 `onnxruntime-win-x64-1.17.3`，链接 `.lib` |
| DLL 加载机制 | `OnnxRuntimeDelayLoadHook.cpp`, `WindowsDllSearchPath.cpp` | MSVC delay-load，Win32 DLL 搜索 |
| GPU 检测 | `GpuDetector.cpp` | DXGI/DirectML，Windows 专属 |
| 系统内存查询 | `RMVPEExtractor.cpp:9-26` | `GlobalMemoryStatusEx()` |
| CPU 特性检测 | `CpuFeatures.cpp:5-62` | `__cpuid` / `__cpuidex` (MSVC intrinsics) |
| SIMD 加速 | `SimdAccelerator.cpp` | AVX/AVX-512 路径，无 NEON |

### 已有跨平台保护（基本 OK）

| 文件 | 保护方式 | 状态 |
|------|---------|------|
| `ModelFactory.cpp:331-346` | `#ifdef _WIN32` 宽字符/窄字符分支 | 已跨平台 |
| `WindowsDllSearchPath.cpp` | `#if JUCE_WINDOWS` 整文件保护 | macOS 上 no-op |
| `OnnxRuntimeDelayLoadHook.cpp` | `#if defined(_MSC_VER)` 整文件保护 | macOS 上 no-op |
| `ModelPathResolver.h:89-93` | 搜索路径包含 `../Resources/models/` | 已兼容 macOS bundle |

### 完全跨平台（无需修改）

- `PluginProcessor.cpp/h` — JUCE 抽象
- 所有 `Source/Standalone/UI/` 组件 — JUCE 组件
- `HifiGANVocoder.cpp/h` — 纯 ONNX C++ API
- `InferenceManager.cpp/h` — ONNX 编排层
- `PitchCurve.cpp/h` — 标准 C++ COW 模式
- `LockFreeQueue.h` — 标准 C++ atomics
- `AppLogger.cpp` — JUCE FileLogger
- `ThirdParty/r8brain-free-src-master/` — 已支持 SSE2/AVX/NEON

### 平台特定预处理指令完整索引

| 文件 | 行号 | 指令 | 用途 |
|------|------|------|------|
| `WindowsDllSearchPath.cpp` | 1 | `#if JUCE_WINDOWS` | 整文件保护 |
| `OnnxRuntimeDelayLoadHook.cpp` | 1, 8 | `#if defined(_WIN32)`, `#if defined(_MSC_VER)` | Windows.h + MSVC delay-load |
| `ModelPathResolver.h` | 10, 31, 95, 122 | `#if defined(_WIN32)` | DLL 加载/注册表/模块路径 |
| `GpuDetector.cpp` | 5, 22, 100 | `#ifdef _WIN32` | DXGI 枚举/DirectML 检测 |
| `CpuFeatures.cpp` | 5, 27 | `#ifdef _WIN32` | `<intrin.h>` + CPUID |
| `SimdAccelerator.cpp` | 7, 181, 357 | `#ifdef _MSC_VER`, `#ifdef __AVX__`, `#ifdef __AVX512F__` | SIMD intrinsics |
| `ModelFactory.cpp` | 331, 339 | `#ifdef _WIN32` | 宽字符路径 |
| `RMVPEExtractor.cpp` | 9, 18 | `#ifdef _WIN32` | Psapi/内存查询 |

---

## 2. 构建系统方案

### 双架构分别构建

分别为 Apple Silicon 和 Intel Mac 构建独立的 `.app` 包：

```
  ┌────────────────────┐     ┌────────────────────┐
  │  macOS x86_64 构建  │     │  macOS arm64 构建   │
  │                    │     │                    │
  │  ort-osx-x86_64   │     │  ort-osx-arm64     │
  │  -mavx2 -mfma     │     │  NEON (内建)        │
  │  SimdAccelerator   │     │  SimdAccelerator   │
  │   -> AVX 路径      │     │   -> NEON 路径      │
  │                    │     │                    │
  │  OpenTune-x86_64   │     │  OpenTune-arm64    │
  │  .app              │     │  .app              │
  └────────────────────┘     └────────────────────┘
```

**选择分别构建而非 Universal Binary 的原因：**

1. ONNX Runtime 官方分别提供 x86_64 和 arm64 的 dylib，合并为 fat binary 需要额外 `lipo` 步骤且增加包体积
2. 编译器 flag 不同 — x86_64 需要 `-mavx2 -mfma` 启用 AVX SIMD 路径，arm64 的 NEON 默认启用
3. 分开构建更简洁、更容易调试

### CMakeLists.txt 改造要点

```cmake
# === 平台检测 (伪代码) ===

if(APPLE)
    # ONNX Runtime macOS 路径
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/onnxruntime-osx-arm64-1.17.3")
    else()
        set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/onnxruntime-osx-x86_64-1.17.3")
    endif()
    set(ONNXRUNTIME_LIB "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.dylib")

    # macOS 编译器 flags
    target_compile_options(OpenTune PRIVATE -Wall -Wextra)
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        target_compile_options(OpenTune PRIVATE -mavx2 -mfma)
    endif()

    # 不设 JUCE_ASIO (Windows-only)
    # CoreAudio 由 JUCE 自动处理

elseif(WIN32)
    # 保持现有 Windows 逻辑不变
    set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/onnxruntime-win-x64-1.17.3")
    set(ONNXRUNTIME_LIB "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
    target_compile_definitions(OpenTune PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(OpenTune PRIVATE JUCE_ASIO=1)
    # ... MSVC flags, delay-load, DLL 复制 ...
endif()
```

**JUCE_ASIO 条件化** — 当前 `CMakeLists.txt:268` 无条件设置 `JUCE_ASIO=1`，这在 macOS 上会导致编译错误。必须移入 `if(WIN32)` 块。

---

## 3. ONNX Runtime 集成

### 库文件获取

从 [ONNX Runtime Releases](https://github.com/microsoft/onnxruntime/releases/tag/v1.17.3) 下载：

- `onnxruntime-osx-x86_64-1.17.3.tgz` (Intel Mac)
- `onnxruntime-osx-arm64-1.17.3.tgz` (Apple Silicon)

解压后放置在项目根目录，与 Windows 版本平行：

```
OpenTune/
├── onnxruntime-win-x64-1.17.3/       # 现有
├── onnxruntime-osx-x86_64-1.17.3/    # 新增
└── onnxruntime-osx-arm64-1.17.3/     # 新增
```

### Execution Provider 策略

```
┌──────────────────────────────────────────────────────┐
│           macOS Execution Provider 决策树              │
├──────────────────────────────────────────────────────┤
│                                                      │
│  Phase 1 (初始移植):                                  │
│     CPU EP only                                      │
│     ├── x86_64: MLAS 后端 (ONNX RT 内建，自动用 AVX) │
│     └── arm64: MLAS 后端 (自动用 NEON)               │
│                                                      │
│  Phase 2 (性能优化，可选):                             │
│     CoreML EP                                        │
│     ├── Apple Neural Engine 加速                     │
│     ├── 需测试模型兼容性 (算子覆盖)                     │
│     └── sessionOptions.AppendExecutionProvider(       │
│         "CoreML", {{"MLComputeUnits", "ALL"}})       │
│                                                      │
│  注意: ONNX RT 的 CPU EP 内部 MLAS 库已经              │
│  针对 ARM NEON 做了深度优化。在 M 系列芯片上            │
│  CPU EP 的性能可能已经足够好，不一定需要 CoreML。        │
└──────────────────────────────────────────────────────┘
```

**关键认知**：ONNX Runtime 的 CPU Execution Provider 内部包含 MLAS (Microsoft Learning Acceleration Service)，它在 ARM64 上自动使用 NEON 优化。即使不加 CoreML EP，ONNX 推理本身在 Apple Silicon 上也有合理的性能。

### dylib 加载方案

macOS 不使用 Windows 的 delay-load 机制。改用 `@rpath` 嵌入：

```
链接时: -rpath @executable_path/../Frameworks
加载时: dyld 在 .app/Contents/Frameworks/ 中找到 libonnxruntime.dylib
```

现有的 `OnnxRuntimeDelayLoadHook.cpp` 和 `WindowsDllSearchPath.cpp` 在 macOS 上自动跳过（已有 `#if` 保护）。

### ModelFactory 改造

`ModelFactory.cpp` 中的 Execution Provider 逻辑需扩展：

```
InferenceProvider::Auto 行为:
├── Windows: 尝试 DirectML -> 回退 CPU
└── macOS:   Phase 1: 直接 CPU
             Phase 2: 尝试 CoreML -> 回退 CPU

GpuBackend enum 扩展:
├── CPU       (现有)
├── DirectML  (现有, Windows)
└── CoreML    (新增, macOS)
```

---

## 4. SIMD 加速方案

### 需要 NEON 化的函数

`SimdAccelerator` 共 7 个 SIMD 函数，其中 3 个在代码中实际被调用：

| 函数 | 调用位置 | 热度 | 说明 |
|------|---------|------|------|
| `dotProduct()` | `MelSpectrogram.cpp:168,197` | **最热** (128 x numFrames 次) | Mel filterbank 内循环 |
| `sumOfSquares()` | `SilentGapDetector.cpp:91-92` | 中等 | RMS 计算 |
| `multiply()` | `SilentGapDetector.cpp:172-176` | 中等 | 预平方运算 |
| `multiplyAdd()` | (无调用处) | — | 预留 |
| `add()` | (无调用处) | — | 预留 |
| `absMax()` | (无调用处) | — | 预留 |
| `findMinMax()` | (无调用处) | — | 预留 |

建议全部 7 个函数都提供 NEON 实现以保持完整性。

### AVX -> NEON 内在函数对应关系

```
AVX (256-bit, 8 floats)              ARM NEON (128-bit, 4 floats)
────────────────────────             ─────────────────────────────
_mm256_loadu_ps()               ->   vld1q_f32()
_mm256_setzero_ps()             ->   vdupq_n_f32(0.0f)
_mm256_fmadd_ps()               ->   vfmaq_f32()        <- A64 原生 FMA
_mm256_mul_ps()                 ->   vmulq_f32()
_mm256_add_ps()                 ->   vaddq_f32()
_mm256_storeu_ps()              ->   vst1q_f32()
_mm256_max_ps()                 ->   vmaxq_f32()
_mm256_min_ps()                 ->   vminq_f32()
_mm256_set1_ps()                ->   vdupq_n_f32()
_mm256_andnot_ps() (for abs)    ->   vabsq_f32()        <- 直接绝对值指令

水平归约:
  AVX: _mm256_extractf128_ps + _mm_hadd_ps (多步)
  NEON: vaddvq_f32() (单指令，AArch64 专有)
```

**关键差异**：
- NEON 是 128-bit (4 floats)，AVX 是 256-bit (8 floats)
- 但 Apple Silicon 有 2 个 128-bit NEON 管线，吞吐量接近 AVX
- `vfmaq_f32()` 是 AArch64 原生 FMA，延迟 4 cycle
- `vaddvq_f32()` 单指令完成水平求和，比 AVX 的多步归约更简洁

### SimdAccelerator 目标结构

```
SimdAccelerator 分派架构:

    ┌─────────────────────────────────────┐
    │         SimdAccelerator API          │
    │   (运行时函数指针分派, detect())      │
    ├─────────┬──────────┬────────────────┤
    │  AVX    │ AVX-512  │   ARM NEON     │
    │  (x86)  │ (x86)    │ (Apple Silicon)│
    ├─────────┴──────────┴────────────────┤
    │         Scalar Fallback             │
    └─────────────────────────────────────┘

detect() 逻辑:
  if ARM (aarch64)  -> NEON 路径 (始终可用)
  if AVX-512        -> AVX-512 路径
  if AVX2/AVX       -> AVX 路径
  else              -> Scalar 回退
```

### 编译保护结构

```cpp
// SimdAccelerator.cpp 中的条件编译:

#if defined(__ARM_NEON) || defined(__aarch64__)
  // ARM NEON 实现 (Apple Silicon)
  float dotProduct_NEON(const float* a, const float* b, size_t count);
  float sumOfSquares_NEON(const float* data, size_t count);
  // ... 其余 5 个函数
#endif

#ifdef __AVX__
  // 现有 AVX 实现 (Intel)
#endif

#ifdef __AVX512F__
  // 现有 AVX-512 实现 (Intel 高端)
#endif

// Scalar 回退 (始终存在)
```

### 性能预期

Apple M 系列芯片的 NEON FMA 吞吐量约 8 FLOPS/cycle（双发射），与 AVX2 的 8 FLOPS/cycle 相当。MelSpectrogram 的 `dotProduct` 内循环性能不会因为从 AVX 切换到 NEON 而降低。

---

## 5. GPU 加速策略

### GpuDetector 改造

```
现有 GpuBackend enum:            macOS 扩展后:
┌──────────────────┐            ┌──────────────────┐
│ ├── CPU          │            │ ├── CPU          │
│ └── DirectML     │            │ ├── DirectML     │
└──────────────────┘            │ └── CoreML       │ <- 新增
                                └──────────────────┘
```

### macOS GPU 检测

`GpuDetector.cpp` 的 `#else` 分支当前返回 `false`（CPU-only），功能上可用。Phase 2 若要支持 CoreML EP，需要添加 macOS GPU 枚举（通过 Metal/IOKit）。

### 推荐方案

Phase 1 直接使用 CPU EP，不做 GPU 检测。理由：

1. ONNX RT CPU EP + MLAS 在 M 系列芯片上已足够高效
2. 推理在后台线程，不影响音频实时性
3. CoreML EP 需要模型兼容性测试，增加移植复杂度

---

## 6. 系统 API 替代

| Windows API | macOS 替代 | 所在文件 | 优先级 |
|-------------|-----------|---------|--------|
| `GlobalMemoryStatusEx()` | `sysctl(HW_MEMSIZE)` + `host_statistics64()` | `RMVPEExtractor.cpp:17-26` | P1 - 影响内存预检 |
| `__cpuid` / `__cpuidex` | `juce::SystemStats` (已有!) + ARM 无需检测 AVX | `CpuFeatures.cpp:27-57` | P1 - 影响 SIMD 分派 |
| Windows Registry | 不需要 (macOS 用 bundle 内路径) | `ModelPathResolver.h:95-104` | P2 - 已有 `#ifdef` 保护 |
| `GetModuleHandleExW` / `GetModuleFileNameW` | `juce::File::getSpecialLocation(currentExecutableFile)` (已有 fallback) | `ModelPathResolver.h:122-139` | P2 - 已有 fallback |
| `LoadLibraryExW` (DLL 加载) | 不需要 (macOS 用 @rpath 直接链接) | `ModelPathResolver.h:31-64` | P2 - 非 Win 返回 true |
| DXGI GPU 枚举 | Phase 1 不需要 / Phase 2 用 Metal API | `GpuDetector.cpp:22-92` | P2 |

### CpuFeatures ARM 路径

Apple Silicon 上不存在 AVX/SSE，`CpuFeatures` 需要：

```
ARM64 平台:
  hasSSE2_ = false
  hasSSE41_ = false
  hasAVX_ = false
  hasAVX2_ = false
  hasFMA_ = false
  hasAVX512_ = false
  hasNEON_ = true  <- 新增字段

  detect() 中:
  #if defined(__aarch64__) || defined(__ARM_NEON)
    hasNEON_ = true;
    // 其余 x86 标志均 false
  #else
    // 现有 JUCE SystemStats 检测逻辑
  #endif
```

---

## 7. App Bundle 打包与模型分发

### 目标 Bundle 结构

```
OpenTune.app/
└── Contents/
    ├── MacOS/
    │   └── OpenTune                          <- 可执行文件
    ├── Frameworks/
    │   └── libonnxruntime.1.17.3.dylib       <- ONNX Runtime
    │       (+ symlink libonnxruntime.dylib)
    ├── Resources/
    │   ├── models/
    │   │   ├── rmvpe.onnx                    <- F0 提取模型
    │   │   └── hifigan.onnx                  <- 声码器模型
    │   ├── Fonts/
    │   │   └── HONORSansCN-Medium.ttf
    │   └── AppIcon.icns
    └── Info.plist
```

### CMake Post-Build 步骤 (macOS)

```cmake
if(APPLE)
    # 1. 复制 dylib 到 Frameworks/
    add_custom_command(TARGET OpenTune POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:OpenTune>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.1.17.3.dylib"
            "$<TARGET_BUNDLE_DIR:OpenTune>/Contents/Frameworks/"
    )

    # 2. 修正 dylib 加载路径
    add_custom_command(TARGET OpenTune POST_BUILD
        COMMAND install_name_tool -change
            "@rpath/libonnxruntime.1.17.3.dylib"
            "@executable_path/../Frameworks/libonnxruntime.1.17.3.dylib"
            "$<TARGET_BUNDLE_DIR:OpenTune>/Contents/MacOS/OpenTune"
    )

    # 3. 复制模型文件到 Resources/models/
    add_custom_command(TARGET OpenTune POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:OpenTune>/Contents/Resources/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/models/rmvpe.onnx"
            "${CMAKE_SOURCE_DIR}/models/hifigan.onnx"
            "$<TARGET_BUNDLE_DIR:OpenTune>/Contents/Resources/models/"
    )
endif()
```

### 模型路径解析

`ModelPathResolver.h` 的 `getModelsDirectory()` 搜索顺序已包含 macOS bundle 路径：

1. `{globalApplicationsDirectory}/OpenTune/models` -> `/Applications/OpenTune/models`
2. `{commonApplicationDataDirectory}/OpenTune/models` -> `/Library/Application Support/OpenTune/models`
3. `{executableParent}/models` -> `.app/Contents/MacOS/models`
4. **`{executableParent}/../Resources/models`** -> `.app/Contents/Resources/models/` (正确匹配!)
5. 当前工作目录 `/models`

第 4 条路径恰好匹配 macOS bundle 结构，无需修改路径解析逻辑。

---

## 8. 代码签名与公证

macOS 分发 `.app` 必须经过代码签名和公证，否则 Gatekeeper 会阻止用户运行。

### 签名流程

```
构建完成
    │
    ▼
codesign --force --deep --sign "Developer ID Application: ..." OpenTune.app
    │
    ├── 签名主程序: Contents/MacOS/OpenTune
    ├── 签名 dylib: Contents/Frameworks/libonnxruntime.1.17.3.dylib
    └── 签名整个 bundle
    │
    ▼
创建 DMG (hdiutil create / create-dmg)
    │
    ▼
xcrun notarytool submit OpenTune.dmg --apple-id ... --password ... --team-id ...
    │
    ▼
xcrun stapler staple OpenTune.dmg
    │
    ▼
分发
```

### 注意事项

- 需要 Apple Developer 证书 (Developer ID Application)
- `libonnxruntime.dylib` 也必须签名，否则 Hardened Runtime 拒绝加载
- Hardened Runtime 需要在 entitlements 中声明 `com.apple.security.cs.disable-library-validation`（加载非 Apple 签名的 dylib）

---

## 9. 风险评估

| 风险 | 严重程度 | 说明 | 缓解措施 |
|------|---------|------|---------|
| ONNX 模型兼容性 | ★★★☆☆ | rmvpe.onnx 和 hifigan.onnx 是否包含 CPU EP 不支持的算子 | Spike 阶段提前验证 |
| 音频延迟特性差异 | ★★☆☆☆ | CoreAudio 与 WASAPI 延迟不同 | JUCE 抽象了此层，风险较低 |
| Gatekeeper 和签名 | ★★★★☆ | 未签名/未公证的 .app 被 macOS 阻止 | 需 Apple Developer 证书 + notarytool |
| dylib 签名 | ★★★☆☆ | libonnxruntime.dylib 需要签名 | Hardened Runtime entitlements |
| Intel Mac 实际性能 | ★★☆☆☆ | x86_64 Mac 上 ONNX CPU EP 可能较慢 | 需实际硬件测试 |
| SIMD NEON 实现 | ★★☆☆☆ | 7 个函数的 NEON 实现需要测试正确性和性能 | 单元测试 + benchmark |
| CpuFeatures ARM 路径 | ★☆☆☆☆ | Apple Silicon 检测 AVX 返回 false，需确保走 NEON | 添加 `hasNEON_` 字段 |

---

## 10. 实施路线图

### 分层实施

```
┌─────────────────────────────────────────────────────┐
│              macOS 移植工作量分层                      │
├─────────────────────────────────────────────────────┤
│                                                     │
│  Layer 0: 能编译通过 ✅ 已完成                        │
│  ├── CMakeLists.txt 平台条件化                       │
│  ├── 下载/集成 macOS 版 ONNX Runtime                 │
│  ├── JUCE_ASIO 条件化                               │
│  ├── 修复 MSVC-specific 编译器 flag                   │
│  ├── App bundle 打包 (模型 + dylib)                  │
│  └── @rpath 配置 + dylib 嵌入 Frameworks/            │
│                                                     │
│  Layer 1: 能正确运行 (约 1-2 天)                      │
│  ├── macOS 内存查询 (替换 GlobalMemoryStatusEx)       │
│  ├── CpuFeatures macOS/ARM 实现 (hasNEON_)          │
│  ├── SimdAccelerator NEON 路径 (7 个函数)            │
│  └── E2E 验证 (启动 + 导入 + F0 + 播放)             │
│                                                     │
│  Layer 2: 性能优化 ✅ 已完成                          │
│  ├── CoreML EP 集成 ✅ 已完成 (RMVPE + vocoder)      │
│  ├── AccelerationDetector CoreML 后端 ✅ 已完成       │
│  ├── ModelFactory CoreML EP 逻辑 ✅ 已完成            │
│  └── Accelerate 框架集成 ✅ 已完成                    │
│                                                     │
│  Layer 3: 发布级品质 (约 3-5 天)                      │
│  ├── 代码签名 + 公证 (notarization)                  │
│  ├── DMG 打包                                       │
│  ├── Gatekeeper 兼容                                │
│  ├── macOS UI 细节 (菜单栏、全屏等)                    │
│  └── CI/CD for macOS                               │
│                                                     │
│  总计估算: 8-13 天 (1人)                             │
└─────────────────────────────────────────────────────┘
```

### Layer 1 详细设计

Layer 0 完成后，arm64 版本可以编译并启动，但运行时存在以下问题：

| # | 问题 | 现象 | 严重度 |
|---|------|------|--------|
| 1 | `getAvailableSystemMemoryMB()` 返回硬编码 2048MB | M 系列 Mac 实际可能有 16-192GB 内存，preflight 可能误报"内存不足"拒绝长音频 | 中 |
| 2 | `CpuFeatures` 无 NEON 感知 | 日志显示 "SIMD: None"，无 cpuBrand 信息 | 低-中 |
| 3 | `SimdAccelerator` 走 Scalar 路径 | MelSpectrogram dotProduct 内循环无 NEON 加速 | 中 |
| 4 | `GpuDetector` 无 CoreML | Phase 1 使用 CPU EP，行为正确，不需要改动 | 低 (不阻塞) |

**原始规划中 NEON SIMD 放在 Layer 2，现调整到 Layer 1，理由：**

1. `CpuFeatures` 的唯一消费者是 `SimdAccelerator` 和日志。给 CpuFeatures 加 `hasNEON_` 字段但不让 SimdAccelerator 使用它，是一个死字段
2. NEON 实现比 AVX 简单得多 — `vaddvq_f32()` 单指令水平求和，`vabsq_f32()` 直接绝对值，无需多步归约
3. 7 个 NEON 函数共约 150 行，与 CpuFeatures 改动合计约 200 行，属于小改动
4. 所有改动都是 `#if __aarch64__` 新增分支，零回归风险

**原始规划中 @rpath/dylib 签名放在 Layer 1，现移至 Layer 0 (已完成) 和 Layer 3，理由：**

1. @rpath 配置已在 Layer 0 的 CMakeLists.txt 中完成（`BUILD_RPATH "@executable_path/../Frameworks"`）
2. dylib 复制到 `Contents/Frameworks/` 已在 post-build 步骤中完成
3. E2E 验证中应用正常启动，说明 dylib 加载路径已经正确
4. codesign 签名属于 Layer 3 发布品质，开发阶段无需签名

#### Layer 1 运行时行为分析

```
启动链: InferenceManager::Impl::initialize()
  │
  ├── CpuFeatures::detect()
  │     当前 (Layer 0 后):                 Layer 1 目标:
  │     hasSSE2-AVX512 = false ✓           不变 ✓
  │     hasNEON_ = (不存在)                hasNEON_ = true (新增)
  │     simdLevel_ = None ⚠️               simdLevel_ = NEON ✓
  │     cpuBrand_ = "" ⚠️                  cpuBrand_ = "Apple M5 Max" ✓
  │
  ├── SimdAccelerator::detect()
  │     当前: simdLevel_ = None            Layer 1: simdLevel_ = NEON
  │     当前: 所有 -> Scalar ⚠️             Layer 1: 所有 -> NEON 实现 ✓
  │
  ├── GpuDetector::detect()
  │     当前: CPU 回退 ✓                    不变 ✓
  │
  └── RMVPEExtractor::preflightCheck()
        └── getAvailableSystemMemoryMB()
              当前: return 2048 (硬编码) ⚠️  Layer 1: sysctl 真实值 ✓
```

#### Layer 1 文件改动矩阵

| 文件 | 改动类型 | 改动量 | ARM 特有 | x86 影响 |
|------|---------|--------|---------|---------|
| `Source/Utils/CpuFeatures.h` | 新增 `SimdLevel::NEON` 枚举值、`hasNEON_` 字段和 getter | ~10 行 | 是 | 无 |
| `Source/Utils/CpuFeatures.cpp` | `#if __aarch64__` 分支：NEON 检测、`sysctl` 获取 cpuBrand | ~30 行 | 是 | 无 |
| `Source/Utils/SimdAccelerator.h` | 新增 `SimdLevel::NEON`、声明 7 个 `_NEON` 静态函数 | ~15 行 | 是 | 无 |
| `Source/Utils/SimdAccelerator.cpp` | 7 个 NEON 实现 + `detect()` NEON 分支 + `#include <arm_neon.h>` | ~150 行 | 是 | 无 |
| `Source/Inference/RMVPEExtractor.cpp` | `#elif __APPLE__` 分支：`sysctl(HW_MEMSIZE)` 内存查询 | ~15 行 | 是 | 无 |

所有改动均为新增 `#if` 条件分支，不修改任何现有 x86/Windows 代码路径。

#### NEON 实现要点

```
AVX (256-bit, 8 floats)              ARM NEON (128-bit, 4 floats)
────────────────────────             ─────────────────────────────
_mm256_loadu_ps()               ->   vld1q_f32()
_mm256_setzero_ps()             ->   vdupq_n_f32(0.0f)
_mm256_fmadd_ps()               ->   vfmaq_f32()        <- A64 原生 FMA
_mm256_mul_ps()                 ->   vmulq_f32()
_mm256_add_ps()                 ->   vaddq_f32()
_mm256_storeu_ps()              ->   vst1q_f32()
_mm256_max_ps()                 ->   vmaxq_f32()
_mm256_min_ps()                 ->   vminq_f32()
_mm256_set1_ps()                ->   vdupq_n_f32()
_mm256_andnot_ps() (for abs)    ->   vabsq_f32()        <- 直接绝对值指令

水平归约:
  AVX: _mm256_extractf128_ps + _mm_hadd_ps (多步)
  NEON: vaddvq_f32() (单指令，AArch64 专有)
```

NEON 是 128-bit (4 floats)，但 Apple Silicon 有 2 个 128-bit NEON 管线，FMA 吞吐量约 8 FLOPS/cycle，与 AVX2 相当。

### 需修改文件清单

#### Layer 0 - 构建基础 ✅ 已完成

| 文件 | 改动内容 | 状态 |
|------|---------|------|
| `CMakeLists.txt` | 平台检测、ONNX RT 路径、链接库、ASIO 条件化、编译器 flag、post-build 步骤、@rpath | ✅ |
| `Source/Utils/SimdAccelerator.cpp:7-13` | `#if defined(__AVX__) \|\| defined(__AVX512F__)` immintrin.h 保护 | ✅ |

#### Layer 1 - 运行时正确性

| 文件 | 改动内容 |
|------|---------|
| `Source/Inference/RMVPEExtractor.cpp` | macOS 内存查询 (`sysctl(HW_MEMSIZE)`) 替换硬编码 2048MB |
| `Source/Utils/CpuFeatures.h` | 新增 `SimdLevel::NEON`、`hasNEON_` 字段和 `hasNEON()` getter |
| `Source/Utils/CpuFeatures.cpp` | `#if __aarch64__` 分支：NEON 标志、sysctl 获取 CPU 品牌字符串 |
| `Source/Utils/SimdAccelerator.h` | 新增 `SimdLevel::NEON`、声明 7 个 NEON 静态函数 |
| `Source/Utils/SimdAccelerator.cpp` | 7 个 NEON 实现 (`<arm_neon.h>`) + `detect()` ARM 分支 |

#### Layer 2 - 性能优化 (可选)

| 文件 | 改动内容 |
|------|---------|
| `Source/Utils/GpuDetector.h` | `GpuBackend` enum 添加 `CoreML` |
| `Source/Utils/GpuDetector.cpp` | macOS CoreML 后端检测 |
| `Source/Inference/ModelFactory.cpp` | macOS CoreML Execution Provider 逻辑 |

#### 已有保护，无需修改

| 文件 | 状态 |
|------|------|
| `Source/Utils/ModelPathResolver.h` | 已兼容 macOS bundle 路径 |
| `Source/Utils/WindowsDllSearchPath.cpp` | `#if JUCE_WINDOWS` 整文件保护 |
| `Source/Utils/OnnxRuntimeDelayLoadHook.cpp` | `#if defined(_MSC_VER)` 整文件保护 |
| `Source/Inference/ModelFactory.cpp:331-346` | 已有 `#ifdef _WIN32` / `#else` 跨平台分支 |

---

## 11. 验证步骤 (Spike)

### Layer 0 Spike ✅ 已通过

以下验证项已在 Layer 0 实施中完成：

1. ✅ **下载 macOS arm64 / x86_64 的 ONNX Runtime 1.17.3** — 已集成到项目根目录
2. ✅ **CMakeLists.txt 改造** — 平台检测、ONNX RT 选择、编译器 flag、app bundle 打包
3. ✅ **arm64 原生编译通过** — Apple Silicon (M5 Max) 上 `cmake -DCMAKE_OSX_ARCHITECTURES=arm64`
4. ✅ **x86_64 交叉编译通过** — Apple Silicon 上 `cmake -DCMAKE_OSX_ARCHITECTURES=x86_64`
5. ✅ **E2E 启动验证** — arm64 版本在 Apple Silicon 上原生启动，进程稳定运行无 crash
6. ✅ **App bundle 结构正确** — dylib 在 `Contents/Frameworks/`，模型在 `Contents/Resources/models/`

### Layer 1 Spike (待执行)

Layer 1 实施后需验证：

1. **启动日志确认** — 日志应显示 "SIMD: NEON" 而非 "SIMD: None"，cpuBrand 应显示实际芯片型号
2. **内存 preflight** — 确认 `getAvailableSystemMemoryMB()` 返回真实系统内存值
3. **端到端推理** — 导入音频 → F0 提取 → pitch 编辑 → 渲染播放
4. **双架构验证** — arm64 和 x86_64 构建均应正常运行（x86_64 走 AVX 路径，arm64 走 NEON 路径）
