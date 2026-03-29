# OpenTune

AI Pitch Correction Application - Standalone Client

## Build Requirements

- CMake 3.22+
- C++17 compiler (MSVC 2022 recommended on Windows)
- Visual Studio 2022 (Windows)

## Build Instructions

### Windows (MSVC)

```bash
# Create build directory
mkdir build
cd build

# Generate project files
cmake .. -G "Visual Studio 17 2022" -A x64

# Build
cmake --build . --config Release

# Output: build/OpenTune_artefacts/Release/Standalone/OpenTune.exe
```

### Build Output

After successful build, the Standalone executable will be located at:
```
build/OpenTune_artefacts/Release/Standalone/OpenTune.exe
```

Required DLLs and models will be automatically copied to the same directory.

## Dependencies (Included)

- **JUCE** - Cross-platform audio framework
- **ONNX Runtime 1.17.3** - ML inference engine
- **r8brain-free-src** - High-quality audio resampling library

## AI Models

The application requires two ONNX models (included):
- `models/rmvpe.onnx` - Pitch extraction model
- `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` - Vocoder model

## License

Please refer to the license files in each dependency folder.

