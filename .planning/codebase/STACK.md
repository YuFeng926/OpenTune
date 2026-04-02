# Technology Stack

**Generated:** 2026-04-02
**Project:** OpenTune - AI Pitch Correction Application

## Languages

**Primary:**
- **C++17** - Core application language for all components
  - Standard enforced via `CMAKE_CXX_STANDARD 17` in `CMakeLists.txt:19`
  - Used throughout `Source/` directory for audio processing, UI, and ML inference

**Secondary:**
- **C** - FFT implementation in PFFFT library
  - File: `ThirdParty/r8brain-free-src-master/fft/pffft_double.c`
  - Used for high-performance FFT in sample rate conversion

## Runtime

**Environment:**
- **Windows x64** - Primary and only target platform
- **MSVC 2022** - Recommended compiler (Visual Studio 17 2022 generator)
- **DirectX 12** - GPU compute runtime for DirectML inference

**Package Manager:**
- **NuGet** (via CMake `file(DOWNLOAD)`) - For DirectML and DirectX Agility SDK
  - DirectML 1.15.4 from nuget.org
  - DirectX Agility SDK 1.619.1 from nuget.org
  - Downloaded at CMake configure time to `<build>/nuget/`

**Build System:**
- **CMake 3.22+** - Minimum required version
- compile_commands.json exported for LSP/clangd support (`CMakeLists.txt:14`)

## Frameworks

**Core:**
- **JUCE 8.0.12** - Cross-platform audio application framework
  - Source: `JUCE-master/` (vendored, AGPLv3/Commercial license)
  - Version defined in `JUCE-master/CMakeLists.txt:35`
  - Purpose: Audio I/O, GUI, DSP primitives, event handling

**ML Inference:**
- **ONNX Runtime 1.24.4** - Neural network inference engine
  - CPU build: `onnxruntime-win-x64-1.24.4/`
  - DirectML build: `onnxruntime-dml-1.24.4/`
  - API version: 24
  - Purpose: Run RMVPE F0 extraction and PC-NSF-HifiGAN vocoder
  - Execution providers: CPU, DirectML (GPU builtin)

**Audio Processing:**
- **r8brain-free-src** - High-quality professional sample rate converter
  - Source: `ThirdParty/r8brain-free-src-master/`
  - License: MIT
  - Author: Aleksey Vaneev (Voxengo)
  - Purpose: Sample rate conversion for model input preparation

## JUCE Modules Used

| Module | Purpose | Link Target |
|--------|---------|-------------|
| `juce_audio_utils` | High-level audio utilities, device management | PRIVATE |
| `juce_audio_processors` | AudioProcessor base class | PRIVATE |
| `juce_dsp` | FFT, filters, windowing functions | PRIVATE |
| `juce_opengl` | GPU-accelerated graphics | PRIVATE |
| `juce_graphics` | 2D graphics, fonts, images | PRIVATE |
| `juce_gui_basics` | UI components (buttons, sliders, windows) | PRIVATE |
| `juce_gui_extra` | Extended UI features | PRIVATE |

## Key Dependencies

**Critical:**

| Package | Version | Purpose | Location |
|---------|---------|---------|----------|
| ONNX Runtime | 1.24.4 | ML model inference | `onnxruntime-win-x64-1.24.4/`, `onnxruntime-dml-1.24.4/` |
| DirectML | 1.15.4 | GPU ML acceleration | NuGet download at build time |
| DirectX Agility SDK | 1.619.1 | Modern D3D12 runtime | NuGet download at build time |
| JUCE | 8.x | Audio framework, UI | `JUCE-master/` |
| r8brain-free-src | 7.1 | Audio resampling | `ThirdParty/r8brain-free-src-master/` |
| PFFFT | bundled | FFT implementation | `ThirdParty/r8brain-free-src-master/fft/` |

**ONNX Runtime Headers:**
- `onnxruntime_cxx_api.h` - C++ API for inference
- `dml_provider_factory.h` - DirectML GPU execution provider (from DML package)
- `onnxruntime_float16.h` - Float16 tensor support

## AI Models

**F0 Extraction - RMVPE:**
- File: `models/rmvpe.onnx` (345 MB)
- Source: `pc_nsf_hifigan_44.1k_ONNX/` copied during build
- Architecture: Robust Multi-scale Vocal Pitch Estimator
- Input: Waveform `[1, num_samples]` at 16 kHz
- Output: F0 curve `[1, num_frames]`, UV (unvoiced) detection
- Hop size: 160 samples (10ms frames)
- Implementation: `Source/Inference/RMVPEExtractor.cpp`

**Neural Vocoder - PC-NSF-HiFiGAN:**
- File: `models/hifigan.onnx` (54 MB)
- Source: `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx`
- Architecture: Pitch Controllable NSF-HiFiGAN
- Input: Mel spectrogram `[1, 128, num_frames]`, F0 curve `[1, num_frames]`
- Output: Audio waveform at 44.1 kHz
- Hop size: 512 samples (~11.6ms frames)
- Mel bins: 128
- Implementation: `Source/Inference/DmlVocoder.cpp`

## Configuration

**Build Configuration:**
- Project: `OpenTune` version 1.0.0 (`CMakeLists.txt:11`)
- Company: DAYA (https://daya.audio)
- Bundle ID: `com.daya.opentune`
- Plugin formats: Standalone only
- Manufacturer code: DAYA, Plugin code: De77

**Compile Definitions (CMakeLists.txt:271-289):**
```cmake
# JUCE Configuration
JUCE_WEB_BROWSER=0
JUCE_USE_CURL=0
JUCE_VST3_CAN_REPLACE_VST2=0
JUCE_REPORT_APP_USAGE=0
JUCE_STRICT_REFCOUNTEDPOINTER=1
JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
JUCE_ASIO=1

# Audio Format Support
JUCE_USE_FLAC=1
JUCE_USE_OGGVORBIS=1
JUCE_USE_MP3AUDIOFORMAT=1
JUCE_USE_WINDOWS_MEDIA_FORMAT=1

# ONNX Runtime
ORT_API_MANUAL_INIT
```

**Compiler Flags (MSVC, CMakeLists.txt:300):**
- `/utf-8` - Source file encoding
- `/MP` - Multi-processor compilation
- `/wd4100` - Suppress unreferenced formal parameter warning
- `/wd4127` - Suppress conditional expression is constant warning

**Linker Options (CMakeLists.txt:304):**
- `/DELAYLOAD:onnxruntime.dll` - Delay load for faster startup
- Links: `delayimp.lib` for delay-load support

## Platform Requirements

**Development:**
- Windows 10/11 x64
- Visual Studio 2022 with C++ workload
- CMake 3.22+
- DirectX 12 SDK (for DML development)
- ~4GB disk space (JUCE, ONNX Runtime, models)

**Production:**
- Windows 10/11 x64
- DirectX 12 compatible GPU recommended (for DML acceleration)
- Minimum 4GB RAM (8GB+ for long audio files)
- ASIO driver support for low-latency audio

## Audio Format Support

**Import:**
| Format | Extension | Implementation |
|--------|-----------|----------------|
| WAV | .wav | JUCE Windows Media |
| FLAC | .flac | JUCE FLAC module |
| OGG Vorbis | .ogg | JUCE OGG module |
| MP3 | .mp3 | JUCE MP3 module |

**Export:**
- WAV (32-bit float, 44.1kHz)

## GPU Acceleration

**DirectML Integration:**
- NuGet Package: `Microsoft.AI.DirectML` 1.15.4
- Download: CMake `file(DOWNLOAD)` from `api.nuget.org` at configure time
- Headers: `<build>/nuGet/microsoft.ai.directml.1.15.4/include/DirectML.h`
- DLL: `<build>/nuget/microsoft.ai.directml.1.15.4/bin/x64-win/DirectML.dll`
- Implementation: `Source/Inference/DmlVocoder.cpp`
- GPU Detection: `Source/Utils/GpuDetector.cpp`
- Runtime Verification: `Source/Utils/DmlRuntimeVerifier.cpp`
- D3D12 Bootstrap: `Source/Utils/D3D12AgilityBootstrap.cpp`
- Supported vendors: NVIDIA (0x10DE), AMD (0x1002), Intel (0x8086), Microsoft (0x1414)
- Device filter: GPU only (`OrtDmlDeviceFilter::Gpu`)

**DirectX Agility SDK:**
- NuGet Package: `Microsoft.Direct3D.D3D12` 1.619.1
- SDK Version Token: 619 (extracted from version)
- Runtime files: `D3D12/D3D12Core.dll`, `D3D12/D3D12SDKLayers.dll`
- Deployed to: `<executable_dir>/D3D12/`

**DirectX Libraries Linked:**
- `d3d12.lib` - Direct3D 12 API
- `dxgi.lib` - DirectX Graphics Infrastructure
- `version.lib` - Windows version API

## SIMD Acceleration

**Implementation:** `Source/Utils/SimdAccelerator.cpp`

| Level | Vector Width | Detection Method |
|-------|--------------|------------------|
| AVX-512 | 16 floats | `cpu.hasAVX512()` |
| AVX2 | 8 floats | `cpu.hasAVX2()` |
| AVX | 8 floats | `cpu.hasAVX()` |
| SSE2 | 4 floats | `cpu.hasSSE2()` |
| Scalar | 1 float | Fallback |

**Accelerated Operations:**
- Sum of squares (energy calculation)
- Dot product (correlation)
- Multiply-add (filtering)

## Build Output

**Executable:**
```
build/OpenTune_artefacts/Release/Standalone/OpenTune.exe
```

**Bundled Dependencies (copied by CMake POST_BUILD):**
```
OpenTune.exe
onnxruntime.dll                    # Core inference (DML build with builtin DirectML)
DirectML.dll                       # Microsoft DirectML runtime
D3D12/D3D12Core.dll                # DirectX Agility runtime
D3D12/D3D12SDKLayers.dll           # DirectX debug layers
models/hifigan.onnx                # Neural vocoder model
models/rmvpe.onnx                  # Pitch extraction model
docs/UserGuide.html                # User documentation
```

## Build Commands

```bash
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build Release
cmake --build build --config Release

# Build with tests disabled
cmake -B build -DOPENTUNE_BUILD_TESTS=OFF

# Run tests
cmake --build build --config Release && build/OpenTuneTests.exe
```

---

*Stack analysis: 2026-04-01*
