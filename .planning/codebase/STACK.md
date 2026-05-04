# Technology Stack

**Analysis Date:** 2026-04-20

## Languages

**Primary:**
- C++17 - Application, plugin, ARA session, inference pipeline, UI, and tests live under `Source/**/*.cpp`, `Source/**/*.h`, and `Tests/TestMain.cpp`; enforced in `CMakeLists.txt:11`, `CMakeLists.txt:21`, `CMakeLists.txt:357`, `CMakeLists.txt:863`.

**Secondary:**
- C - Vendored FFT/resampling code is compiled from `ThirdParty/r8brain-free-src-master/fft/pffft_double.c` and linked into the main target through `CMakeLists.txt:11`, `ThirdParty/r8brain-free-src-master/fft/pffft_double.c`.
- CMake - Entire build, target split, runtime packaging, install step, and test registration live in `CMakeLists.txt:6`, `CMakeLists.txt:303`, `CMakeLists.txt:709`, `CMakeLists.txt:805`, `CMakeLists.txt:961`, `CMakeLists.txt:970`.
- PowerShell - Local MSVC bootstrap helper exists at `.planning/scripts/invoke-msvc-cmake.ps1`.

## Runtime

**Environment:**
- Native desktop audio runtime built on JUCE for Windows and macOS only; unsupported platforms fail at configure time in `CMakeLists.txt:49`, `CMakeLists.txt:227`.
- Product formats are `Standalone` and `VST3` from one JUCE plugin definition in `CMakeLists.txt:303`, `CMakeLists.txt:324`.
- VST3 ARA support is compiled through JUCE ARA integration plus `Source/ARA/*.cpp`; ARA enablement depends on a resolvable SDK path in `CMakeLists.txt:57`, `CMakeLists.txt:326`, `CMakeLists.txt:539`.

**Package Manager / Project Manifests:**
- No app-level package manager manifest is present at the repo root: no root `package.json`, `pyproject.toml`, `requirements.txt`, `Cargo.toml`, or `go.mod` was found.
- `.jucer` files exist only inside vendored JUCE extras, not for the OpenTune app itself, so Projucer is not the active build source.

## Frameworks

**Core:**
- JUCE (vendored) - Base framework for app/plugin lifecycle, GUI, audio I/O, DSP, and plugin client integration via `JUCE-master/` and `CMakeLists.txt:49`, `CMakeLists.txt:573`.
- ARA SDK 2.2.0 - Bundled ARA sources under `ThirdParty/ARA_SDK-releases-2.2.0/`; wired into JUCE and tests via `CMakeLists.txt:37`, `CMakeLists.txt:58`, `CMakeLists.txt:895`.
- ONNX Runtime 1.24.4 - Local inference runtime for RMVPE and vocoder models; configured in `CMakeLists.txt:206`, `CMakeLists.txt:240`, `CMakeLists.txt:296` and consumed by `Source/Inference/ModelFactory.cpp:45`.

**Application Structure:**
- Shared processor runtime shell composes `SourceStore`, `MaterializationStore`, `StandaloneArrangement`, and `VST3AraSession` in `Source/PluginProcessor.h:6`, `Source/PluginProcessor.h:30`, `Source/PluginProcessor.h:31`, `Source/ARA/VST3AraSession.h:21`.
- Standalone-only editor shell lives in `Source/Standalone/PluginEditor.cpp` and `Source/Standalone/PluginEditor.h`; VST3-only editor shell lives in `Source/Plugin/PluginEditor.cpp` and `Source/Plugin/PluginEditor.h`; target split is enforced by `CMakeLists.txt:504`, `CMakeLists.txt:518`.
- Shared app preferences are centralized in `Source/Utils/AppPreferences.h:18` and `Source/Utils/AppPreferences.cpp:205`; shared UI pages live in `Source/Editor/Preferences/SharedPreferencePages.cpp:38`, and standalone-only pages in `Source/Editor/Preferences/StandalonePreferencePages.cpp:55`.
- Editing behavior is driven by the explicit rule layer in `Source/Utils/AudioEditingScheme.h:8` rather than a runtime manager.

**Testing:**
- `OpenTuneTests` is a native CMake test executable defined in `CMakeLists.txt:863`.
- Test suites are grouped into `core`, `processor`, `ui`, and `architecture` in `Tests/TestMain.cpp:30`.
- CTest registration is present through `enable_testing()` and `add_test(NAME OpenTuneCoreTests COMMAND OpenTuneTests)` in `CMakeLists.txt:961`, `CMakeLists.txt:963`.

## Key Dependencies

**Critical:**
- `JUCE-master/` - Supplies audio processor, standalone shell, GUI, and VST3 client code used by `CMakeLists.txt:49`, `CMakeLists.txt:575`.
- `ThirdParty/ARA_SDK-releases-2.2.0/` - Supplies bundled ARA headers and helper sources referenced by `CMakeLists.txt:38`, `CMakeLists.txt:898`.
- `ThirdParty/onnxruntime-win-x64-1.24.4/` - CPU ONNX Runtime headers/libs present in the live tree and referenced by `CMakeLists.txt:207`, `CMakeLists.txt:230`.
- `ThirdParty/onnxruntime-dml-1.24.4/` - Windows DML ONNX Runtime package present in the live tree and referenced by `CMakeLists.txt:240`, `CMakeLists.txt:247`, `CMakeLists.txt:266`.
- `ThirdParty/r8brain-free-src-master/` - Vendored DSP support library present in the live tree and included by `CMakeLists.txt:555`.

**Platform-Specific:**
- `ThirdParty/microsoft.direct3d.d3d12.1.619.1/` is vendored and used for D3D12 Agility packaging on Windows in `CMakeLists.txt:137`, `CMakeLists.txt:170`, `CMakeLists.txt:741`, `CMakeLists.txt:831`.
- No vendored `ThirdParty/microsoft.ai.directml.*` package is present in the live tree; Windows DirectML headers/runtime therefore rely on the CMake fallback to Windows SDK headers plus `C:/Windows/System32/DirectML.dll`, unless overrides are supplied, per `CMakeLists.txt:127`, `CMakeLists.txt:151`, `CMakeLists.txt:158`.
- No vendored `ThirdParty/onnxruntime-osx-arm64-1.24.4/` directory is present in the live tree, even though macOS configuration expects that default path unless `ONNXRUNTIME_ROOT` is overridden in `CMakeLists.txt:215`, `CMakeLists.txt:217`.

## Configuration

**Build Configuration:**
- Root build entry is `CMakeLists.txt`.
- Important cache variables are `ARA_SDK_PATH`, `ONNXRUNTIME_ROOT`, `ONNXRUNTIME_DML_ROOT`, `OPENTUNE_DIRECTML_ROOT`, `OPENTUNE_D3D12_AGILITY_ROOT`, and `OPENTUNE_DIRECTML_DLL` in `CMakeLists.txt:39`, `CMakeLists.txt:208`, `CMakeLists.txt:241`, `CMakeLists.txt:129`, `CMakeLists.txt:138`, `CMakeLists.txt:162`.
- Windows builds require Windows Kits include/lib roots through `OPENTUNE_WINDOWS_KITS_INCLUDE_ROOT` and `OPENTUNE_WINDOWS_KITS_LIB_ROOT` in `CMakeLists.txt:82`, `CMakeLists.txt:84`.

**Runtime / Diagnostics Knobs:**
- `OPENTUNE_ORT_PROFILE` enables ONNX profiling in debug builds via `Source/Inference/ModelFactory.cpp:15`, `Source/Inference/ModelFactory.cpp:189`.
- `OPENTUNE_SELFTEST` is read by the Standalone editor to trigger self-tests in `Source/Standalone/PluginEditor.cpp:450`.

**Persistent App Configuration:**
- App preferences are stored as XML `juce::PropertiesFile` data in `app-preferences.settings` under user app-data, with an inter-process lock, via `Source/Utils/AppPreferences.h:39`, `Source/Utils/AppPreferences.cpp:52`, `Source/Utils/AppPreferences.cpp:365`.
- Shared persisted preference fields include language, theme, editing scheme, piano-roll visual options, and zoom sensitivity in `Source/Utils/AppPreferences.cpp:9`, `Source/Utils/AppPreferences.cpp:205`, `Source/Utils/AppPreferences.cpp:234`.
- Standalone-only persisted preference fields include shortcut bindings and mouse-trail theme in `Source/Utils/AppPreferences.cpp:18`, `Source/Utils/AppPreferences.cpp:228`, `Source/Editor/Preferences/StandalonePreferencePages.cpp:93`, `Source/Editor/Preferences/StandalonePreferencePages.cpp:148`.

## Resources

**Bundled Assets:**
- Binary data currently embeds `Resources/Fonts/HONORSansCN-Medium.ttf` through `CMakeLists.txt:349`, `CMakeLists.txt:351`.
- App icon file is `Resources/AppIcon.png`, referenced by `CMakeLists.txt:339`, `CMakeLists.txt:340`.
- Help document source is `docs/UserGuide.html`; it is packaged for Standalone on macOS and copied to `docs/` beside the Standalone executable on Windows via `CMakeLists.txt:693`, `CMakeLists.txt:697`, `CMakeLists.txt:788`.
- AI model source files in the repo are `models/rmvpe.onnx` and `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx`, then copied into output `models/` as `rmvpe.onnx` and `hifigan.onnx` in `CMakeLists.txt:704`, `CMakeLists.txt:705`, `CMakeLists.txt:753`, `CMakeLists.txt:844`.

## Platform Requirements

**Development:**
- Windows development expects MSVC static runtime, Windows Kits headers/libs, ONNX Runtime packages, and the vendored D3D12 Agility SDK in `CMakeLists.txt:28`, `CMakeLists.txt:82`, `CMakeLists.txt:184`, `CMakeLists.txt:196`, `CMakeLists.txt:203`.
- macOS configuration expects arm64 ONNX Runtime plus Accelerate linkage, and injects Standalone-only bundle metadata in `CMakeLists.txt:216`, `CMakeLists.txt:596`, `CMakeLists.txt:656`.

**Runtime Packaging:**
- Windows Standalone output copies `onnxruntime.dll`, optional shared providers, `DirectML.dll`, `D3D12` runtime files, models, and `docs/UserGuide.html` through `CMakeLists.txt:709`, `CMakeLists.txt:734`, `CMakeLists.txt:741`, `CMakeLists.txt:753`, `CMakeLists.txt:788`.
- Windows VST3 output copies `onnxruntime.dll`, optional shared providers, `DirectML.dll`, `D3D12` runtime files, and models, but not the Standalone help file, through `CMakeLists.txt:805`, `CMakeLists.txt:823`, `CMakeLists.txt:831`, `CMakeLists.txt:844`.
- macOS Standalone bundles `libonnxruntime.1.24.4.dylib`, models, and `docs/UserGuide.html` inside the app bundle in `CMakeLists.txt:686`, `CMakeLists.txt:690`, `CMakeLists.txt:693`, `CMakeLists.txt:766`, `CMakeLists.txt:775`.

---

*Stack analysis: 2026-04-20*
