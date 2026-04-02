# External Integrations

**Generated:** 2026-04-02
**Project:** OpenTune - AI Pitch Correction Application

## ML/AI Inference

### ONNX Runtime Integration

**SDK:** `onnxruntime_cxx_api.h` (C++ API)
**Version:** 1.24.4
**API Version:** 24
**Locations:** 
- CPU build: `onnxruntime-win-x64-1.24.4/`
- DirectML build: `onnxruntime-dml-1.24.4/`

**Purpose:** Neural network inference for pitch extraction and audio synthesis

**Key Components:**
| Component | File | Purpose |
|-----------|------|---------|
| RMVPEExtractor | `Source/Inference/RMVPEExtractor.cpp` | F0 pitch extraction from audio |
| DmlVocoder | `Source/Inference/DmlVocoder.cpp` | Neural vocoder using DirectML GPU |
| VocoderFactory | `Source/Inference/VocoderFactory.cpp` | Creates vocoder instances |
| ModelFactory | `Source/Inference/ModelFactory.cpp` | Creates ONNX sessions |
| F0InferenceService | `Source/Inference/F0InferenceService.cpp` | Service layer for F0 extraction |
| VocoderInferenceService | `Source/Inference/VocoderInferenceService.cpp` | Service layer for vocoding |

**Execution Providers:**
| Provider | Priority | Configuration |
|----------|----------|---------------|
| DirectML | First (if GPU available) | GPU memory limit at 60% VRAM |
| CPU | Fallback | Thread count from CpuBudgetManager |

**Session Creation Pattern:**
```cpp
// From DmlVocoder.cpp
Ort::SessionOptions sessionOptions;
sessionOptions.SetIntraOpNumThreads(1);
sessionOptions.SetGraphOptimizationLevel(
    GraphOptimizationLevel::ORT_ENABLE_ALL);

// DirectML provider configuration
OrtDmlApiOptions dmlOptions;
dmlOptions.device_id = config.deviceId;
Ort::GetApi().AddExecutionProvider_Dml(sessionOptions, &dmlOptions);

session_ = std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
```

**Model Loading:**
- Models loaded from `<exe_dir>/models/`
- HifiGAN: `models/hifigan.onnx` (54 MB)
- RMVPE: `models/rmvpe.onnx` (345 MB)
- Delay-load hook: `Source/Utils/OnnxRuntimeDelayLoadHook.cpp`
- Manual initialization via `ORT_API_MANUAL_INIT` definition

**ONNX Runtime Headers:**
- `onnxruntime_cxx_api.h` - Core C++ API
- `dml_provider_factory.h` - DirectML GPU provider
- `onnxruntime_float16.h` - Float16 support

## GPU Hardware Integration

### DirectML (Windows)

**NuGet Package:** `Microsoft.AI.DirectML` 1.15.4
**Detection:** `Source/Utils/GpuDetector.cpp`
**Verification:** `Source/Utils/DmlRuntimeVerifier.cpp`

**NuGet Download (CMake):**
```cmake
# From CMakeLists.txt:53-80
function(opentune_fetch_nuget_package package_id package_version out_dir)
    set(download_url "https://api.nuget.org/v3-flatcontainer/${package_id_lower}/${version}/${nupkg}")
    file(DOWNLOAD "${download_url}" "${nupkg_path}" ...)
    file(ARCHIVE_EXTRACT INPUT "${nupkg_path}" DESTINATION "${package_dir}")
endfunction()

opentune_fetch_nuget_package("Microsoft.AI.DirectML" "1.15.4" OPENTUNE_DIRECTML_ROOT)
```

**DML Configuration (DmlConfig.h):**
```cpp
struct DmlConfig {
    int deviceId = 0;
    int performancePreference = 1;  // 0=Default, 1=HighPerformance, 2=MinimumPower
    int deviceFilter = 1;           // 1=Gpu (OrtDmlDeviceFilter::Gpu)
};
```

**GPU Detection Logic:**
```cpp
// Check DLL size (DirectML version >= 12MB)
const int64_t dmlDllMinSize = 12 * 1024 * 1024;

// Enumerate GPUs via DXGI
Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
CreateDXGIFactory1(__uuidof(IDXGIFactory1), &factory);

// Enumerate adapters
for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    // Check vendor: NVIDIA (0x10DE), AMD (0x1002), Intel (0x8086)
}
```

**GPU Selection Priority:**
1. Discrete GPU (NVIDIA, AMD, Intel Arc)
2. Integrated GPU with sufficient memory
3. CPU fallback

**Memory Configuration:**
```cpp
std::unordered_map<std::string, std::string> dmlOptions;
dmlOptions["device_id"] = std::to_string(gpu.getDirectMLDeviceId());
dmlOptions["gpu_mem_limit"] = std::to_string(gpuMemLimit);
sessionOptions.AppendExecutionProvider("DML", dmlOptions);
```

**GPU Requirements:**
- DirectX 12 capable GPU
- Minimum 512MB effective VRAM
- Latest GPU drivers

**DirectX Agility SDK:**
- NuGet Package: `Microsoft.Direct3D.D3D12` 1.619.1
- SDK Version Token: 619 (compiled into app)
- Bootstrap: `Source/Utils/D3D12AgilityBootstrap.cpp`
- Deployed files:
  - `D3D12/D3D12Core.dll`
  - `D3D12/D3D12SDKLayers.dll`

**DirectX Libraries Linked:**
- `d3d12.lib` - Direct3D 12 API
- `dxgi.lib` - DirectX Graphics Infrastructure
- `version.lib` - Windows version API

## Audio System Integration

### JUCE Audio Framework

**Audio Device Management:**
- `juce::AudioDeviceManager` - Device enumeration and configuration
- ASIO support enabled (`JUCE_ASIO=1`)
- Sample rates: 44.1kHz to 192kHz supported
- Buffer sizes: Configurable via audio settings dialog

**Audio Processing:**
- `OpenTuneAudioProcessor` extends `juce::AudioProcessor` (`Source/PluginProcessor.h:67`)
- Supports mono and stereo input
- Double-precision processing support (`supportsDoublePrecisionProcessing()`)
- Real-time safe audio callback (`processBlock()`)

**DSP Primitives from JUCE:**
| Class | Purpose | Usage Location |
|-------|---------|----------------|
| `juce::dsp::FFT` | FFT operations | `Source/DSP/MelSpectrogram.cpp` |
| `juce::dsp::WindowingFunction<float>` | Windowing | `Source/DSP/MelSpectrogram.cpp` |
| `juce::LagrangeInterpolator` | Interpolation | Export resampling |

### r8brain Audio Resampling

**Library:** r8brain-free-src
**Location:** `ThirdParty/r8brain-free-src-master/`
**License:** MIT
**Author:** Aleksey Vaneev (Voxengo)

**Purpose:** Professional-grade sample rate conversion

**Key Classes:**
| Class | Purpose |
|-------|---------|
| `r8b::CDSPResampler` | Base resampler class |
| `CDSPBlockConvolver` | FFT-based convolution |
| `CDSPRealFFT` | Real FFT implementation |

**Integration:** `Source/DSP/ResamplingManager.cpp`
```cpp
// Downsample to 16kHz for RMVPE
r8b::CDSPResampler resampler(sourceRate, 16000, inputLength);
resampler.oneshot(input, inputLength, output, outputLength);
```

**Key Operations:**
- Downsample to 16kHz for RMVPE inference
- Upsample rendered audio to device sample rate
- Convert between F0 frame rate and audio sample rate

## UI Framework Integration

### JUCE GUI Components

**Rendering:** OpenGL via `juce_opengl` module

**Custom LookAndFeel Implementations:**
| File | Theme |
|------|-------|
| `Source/Standalone/UI/OpenTuneLookAndFeel.h` | Base theme interface |
| `Source/Standalone/UI/BlueBreezeLookAndFeel.cpp` | Blue breeze theme |
| `Source/Standalone/UI/DarkBlueGreyLookAndFeel.cpp` | Dark blue-grey theme |
| `Source/Standalone/UI/AuroraLookAndFeel.cpp` | Aurora theme |

**Font Integration:**
- Custom font: `Resources/Fonts/HONORSansCN-Medium.ttf` (7.7 MB)
- Loaded via JUCE binary data system (`juce_add_binary_data`)
- Font embedded in executable

**Key UI Components:**
| Component | File | Purpose |
|-----------|------|---------|
| MainControlPanel | `Source/Standalone/UI/MainControlPanel.cpp` | Main control interface |
| PianoRollComponent | `Source/Standalone/UI/PianoRollComponent.cpp` | Pitch correction editing |
| TimelineComponent | `Source/Standalone/UI/TimelineComponent.cpp` | Timeline display |
| TrackPanelComponent | `Source/Standalone/UI/TrackPanelComponent.cpp` | Multi-track panel |
| TransportBarComponent | `Source/Standalone/UI/TransportBarComponent.cpp` | Play/stop controls |

## DSP Processing

### FFT Integration
- PFFFT (Pretty Fast FFT) from r8brain
- SIMD optimizations: SSE2, AVX, AVX-512, NEON
- Location: `ThirdParty/r8brain-free-src-master/fft/pffft_double.c`

### Mel Spectrogram
- Implementation: `Source/DSP/MelSpectrogram.cpp`
- Used for: Vocoder input feature extraction
- Parameters: 128 mel bins, 512 hop size, 2048 FFT size
- Uses JUCE FFT and windowing functions

**Mel Spectrogram Configuration:**
```cpp
// From MelSpectrogram.h
struct MelSpectrogramConfig {
    int sampleRate = 44100;
    int nFft = 2048;
    int winLength = 2048;
    int hopLength = 512;
    int nMels = 128;
    float fMin = 40.0f;
    float fMax = 16000.0f;
};
```

### Scale Inference
- Implementation: `Source/DSP/ScaleInference.cpp`
- Purpose: Musical key detection from F0 curve

## External Model Sources

**AI Model Dependencies:**
| Model | Source | License | Purpose |
|-------|--------|---------|---------|
| RMVPE | [yxlllc/RMVPE](https://github.com/yxlllc/RMVPE) | MIT | Pitch extraction |
| PC-NSF-HiFiGAN | [openvpi/vocoders](https://github.com/openvpi/vocoders) | MIT | Neural vocoding |

**Model File Structure:**
```
models/
├── hifigan.onnx                    # Neural vocoder (from pc_nsf_hifigan_44.1k_ONNX/)
├── rmvpe.onnx                      # Pitch extraction
├── NOTICE.txt                      # License notices
├── NOTICE.zh-CN.txt                # Chinese license notices
└── STATEMENTS.txt                  # Dependency statements
```

**Model Specifications:**

RMVPE Model:
- Input: Waveform `[1, num_samples]` at 16 kHz, Threshold `[1]`
- Output: F0 `[1, num_frames]`, UV `[1, num_frames]`
- Hop size: 160 samples (10ms frames)
- Sample rate: 16kHz

PC-NSF-HiFiGAN Model:
- Input: Mel spectrogram, F0 contour
- Output: Audio waveform at 44.1 kHz
- Hop size: 512 samples (~11.6ms frames)
- Mel bins: 128

## System Integration

### Windows DLL Search Path
- Custom DLL search path handling: `Source/Utils/WindowsDllSearchPath.cpp`
- Ensures ONNX Runtime DLLs are found correctly
- Delay-load configuration in CMakeLists.txt:304

### CPU Feature Detection
| Component | File | Purpose |
|-----------|------|---------|
| CpuFeatures | `Source/Utils/CpuFeatures.cpp` | Detect AVX, AVX2, AVX-512, SSE2 |
| CpuBudgetManager | `Source/Utils/CpuBudgetManager.cpp` | Allocate threads for ONNX |
| SimdAccelerator | `Source/Utils/SimdAccelerator.cpp` | SIMD-accelerated operations |

### Performance Monitoring
- Implementation: `Source/Utils/AppLogger.cpp`
- Audio callback timing histogram
- Cache hit/miss tracking
- Render queue depth monitoring

**Performance Probes in PluginProcessor:**
```cpp
// From PluginProcessor.h
struct PerfProbeSnapshot {
    double audioCallbackP99Ms{0.0};
    double cacheMissRate{0.0};
    int renderQueueDepth{0};
    uint64_t cacheChecks{0};
    uint64_t cacheMisses{0};
};
```

## File Formats

### Input Formats (Audio Import)
| Format | Extension | Compile Flag | Implementation |
|--------|-----------|--------------|----------------|
| WAV | .wav | `JUCE_USE_WINDOWS_MEDIA_FORMAT=1` | JUCE |
| FLAC | .flac | `JUCE_USE_FLAC=1` | JUCE |
| OGG Vorbis | .ogg | `JUCE_USE_OGGVORBIS=1` | JUCE |
| MP3 | .mp3 | `JUCE_USE_MP3AUDIOFORMAT=1` | JUCE |

### Output Format (Audio Export)
| Format | Extension | Sample Rate | Bit Depth |
|--------|-----------|-------------|-----------|
| WAV | .wav | 44.1kHz | 32-bit float |

### Project Format
- State serialized via `juce::ValueTree`
- XML-based binary encoding
- Stores: pitch curves, correction segments, BPM, zoom level, track state

## Test Infrastructure

**CTest Integration:**
- Test executable: `OpenTuneTests`
- Unit tests for core components
- Uses `juce::juce_core` and `juce::juce_audio_basics` only

**Test File:** `Tests/TestMain.cpp`

**Build Option:** `OPENTUNE_BUILD_TESTS` (default ON)

## No External Network Services

**No External APIs:**
- No HTTP/HTTPS calls
- No cloud services integration
- All processing is local/offline

**No External Authentication:**
- No OAuth or API key requirements
- No user accounts or cloud sync

**No Payment Processing:**
- Not applicable (open-source desktop application)

**No External Database:**
- Uses local filesystem only
- Presets stored in user preferences (JUCE PropertiesFile)

**Communication Channels:**
- No email integration
- No SMS/notification services
- No webhooks
- No telemetry/analytics

## Configuration Files

**Model Configuration:**
- Original hifigan model: `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx`

**Application Configuration:**
- Runtime settings via JUCE PropertiesFile (user-specific)
- Compile-time definitions:
  - `OPENTUNE_VERSION` - Application version string
  - `OPENTUNE_D3D12_AGILITY_SDK_VERSION` - DirectX Agility SDK version token
  - `ORT_API_MANUAL_INIT` - Manual ONNX Runtime initialization

## Runtime DLLs

**Deployed to executable directory:**

| DLL | Source | Purpose |
|-----|--------|---------|
| `onnxruntime.dll` | ONNX Runtime DML 1.24.4 | Core inference (DML builtin) |
| `DirectML.dll` | DirectML NuGet 1.15.4 | GPU ML acceleration |
| `D3D12/D3D12Core.dll` | DirectX Agility NuGet | D3D12 runtime |
| `D3D12/D3D12SDKLayers.dll` | DirectX Agility NuGet | D3D12 debug layers |

**Delay Load Configuration:**
- `/DELAYLOAD:onnxruntime.dll` - Delays loading until first inference
- Hook: `Source/Utils/OnnxRuntimeDelayLoadHook.cpp`
- DLL search path: `Source/Utils/WindowsDllSearchPath.cpp`

## Future Integrations (Planned)

**Plugin Formats:**
- VST3 support planned
- ARA2 support planned for DAW integration

**Cross-Platform:**
- macOS support planned
- Linux support under consideration

---

*Integration audit: 2026-04-01*
