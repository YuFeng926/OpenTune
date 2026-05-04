# External Integrations

**Analysis Date:** 2026-05-05

## APIs & External Services

**Host Audio Platforms:**
- VST3 host DAW integration is provided by JUCE VST3 client code and the plugin editor shell in `CMakeLists.txt:324`, `CMakeLists.txt:576`, `Source/Plugin/PluginEditor.cpp`.
- ARA host integration is handled by `OpenTuneDocumentController`, `OpenTunePlaybackRenderer`, and `VST3AraSession` in `Source/ARA/OpenTuneDocumentController.cpp:188`, `Source/ARA/OpenTuneDocumentController.cpp:193`, `Source/ARA/OpenTunePlaybackRenderer.cpp`, `Source/ARA/VST3AraSession.h:21`.
- Host playback control callbacks sent back to the DAW are `requestSetPlaybackPosition`, `requestStartPlayback`, and `requestStopPlayback` in `Source/ARA/OpenTuneDocumentController.cpp:193`, `Source/ARA/OpenTuneDocumentController.cpp:214`, `Source/ARA/OpenTuneDocumentController.cpp:235`.

**Local AI Inference:**
- ONNX Runtime sessions are created locally for F0 extraction in `Source/Inference/ModelFactory.cpp:45`, `Source/Inference/ModelFactory.cpp:181`.
- Vocoder inference is local and processor-owned through `Source/PluginProcessor.h:37`, `Source/PluginProcessor.h:375`, `Source/PluginProcessor.cpp:786`, `Source/PluginProcessor.cpp:3077`.
- Windows GPU probing uses ORT `GetExecutionProviderApi("DML")` via `Source/Utils/AccelerationDetector.cpp`; D3D12 Agility SDK bootstrap in `Source/Utils/D3D12AgilityBootstrap.cpp:9`.
- macOS F0 extraction attempts CoreML execution provider attachment in `Source/Inference/ModelFactory.cpp:139`, `Source/Inference/ModelFactory.cpp:145`.

**Remote Services:**
- No HTTP client/server, webhook, socket, named-pipe, or remote telemetry integration was found under `Source/`.
- No auth provider, token flow, or cloud backend integration was found under `Source/`.

## Data Storage

**Databases:**
- None detected. No SQL/NoSQL client, ORM, or external database configuration is present in `Source/` or `CMakeLists.txt`.

**Local Filesystem Storage:**
- App preferences persist as XML `juce::PropertiesFile` data under user app-data via `Source/Utils/AppPreferences.h:39`, `Source/Utils/AppPreferences.cpp:52`, `Source/Utils/AppPreferences.cpp:365`.
- Presets persist as `.otpreset` XML files under the user documents directory in `Source/Utils/PresetManager.cpp:5`, `Source/Utils/PresetManager.cpp:66`, `Source/Utils/PresetManager.cpp:108`.
- Processor/project state persists as a JUCE `ValueTree` XML blob rooted at `OpenTuneState` in `Source/PluginProcessor.cpp:1518`, `Source/PluginProcessor.cpp:1608`.
- Log files persist under `OpenTune/Logs` in user app-data, with temp-directory fallback, in `Source/Utils/AppLogger.cpp:58`, `Source/Utils/AppLogger.cpp:62`, `Source/Utils/AppLogger.cpp:68`.
- Models are discovered from application install paths, ProgramData, module-adjacent `models/`, bundle `Resources/models`, registry, or current working directory in `Source/Utils/ModelPathResolver.h:40`, `Source/Utils/ModelPathResolver.h:71`, `Source/Utils/ModelPathResolver.h:92`, `Source/Utils/ModelPathResolver.h:99`, `Source/Utils/ModelPathResolver.h:110`.

**In-Memory Caches:**
- Content/render caching is in-process only through `Source/Inference/RenderCache.cpp`, `Source/MaterializationStore.cpp`, and processor playback read sources in `Source/PluginProcessor.h:251`.

## Monitoring & Observability

**Logging:**
- Central local file logging is implemented by `AppLogger` in `Source/Utils/AppLogger.cpp:50`, `Source/Utils/AppLogger.cpp:93`.
- Inference, DirectML probing, VST3 editor transport requests, and ARA mapping emit diagnostic logs in `Source/Inference/ModelFactory.cpp:32`, `Source/Utils/AccelerationDetector.cpp`, `Source/Plugin/PluginEditor.cpp:198`, `Source/ARA/OpenTuneDocumentController.cpp:210`, `Source/ARA/VST3AraSession.cpp:251`. Note: `DmlRuntimeVerifier` was deleted entirely (2026-05-02); DirectML probing now uses `AccelerationDetector` alone with `GetExecutionProviderApi("DML")`.

**Error Tracking / Telemetry:**
- No Sentry, Crashpad, Bugsnag, or remote telemetry integration was found in the live tree.

## Deployment & Runtime Boundaries

**Build Outputs:**
- The repo builds a JUCE Standalone app and JUCE VST3 plugin from the same source base in `CMakeLists.txt:303`, `CMakeLists.txt:324`.
- Tests build as a separate `OpenTuneTests` executable in `CMakeLists.txt:863`.

**Format Isolation:**
- Shared runtime logic stays in `Source/PluginProcessor.h` and `Source/PluginProcessor.cpp`.
- Standalone-only editor/UI shell stays in `Source/Standalone/PluginEditor.cpp` and `Source/Standalone/PluginEditor.h`.
- VST3-only editor/UI shell stays in `Source/Plugin/PluginEditor.cpp` and `Source/Plugin/PluginEditor.h`.
- ARA session state is isolated to the VST3 path through `Source/ARA/VST3AraSession.h:21` and `Source/ARA/OpenTuneDocumentController.cpp:188`.

**Shared Runtime Ownership:**
- Shared processor runtime composes `SourceStore`, `MaterializationStore`, `StandaloneArrangement`, and `VST3AraSession` in `Source/PluginProcessor.h:6`, `Source/PluginProcessor.h:30`, `Source/PluginProcessor.h:31`, `Source/ARA/VST3AraSession.h:21`.
- Serialized standalone arrangement state stores `Placement` objects with `placementId`, `contentId`, mapping revision, timeline range, and fades in `Source/PluginProcessor.cpp:1525`, `Source/PluginProcessor.cpp:1553`.
- Serialized content state stores content-local pitch curve, notes, detected key, and original-F0 state in `Source/PluginProcessor.cpp:1583`, `Source/PluginProcessor.cpp:1590`.

## Environment Configuration

**Build-Time Knobs:**
- ARA / ONNX / DirectML / D3D12 path overrides come from CMake cache variables `ARA_SDK_PATH`, `ONNXRUNTIME_ROOT`, `ONNXRUNTIME_DML_ROOT`, `OPENTUNE_DIRECTML_ROOT`, `OPENTUNE_D3D12_AGILITY_ROOT`, and `OPENTUNE_DIRECTML_DLL` in `CMakeLists.txt:39`, `CMakeLists.txt:208`, `CMakeLists.txt:241`, `CMakeLists.txt:129`, `CMakeLists.txt:138`, `CMakeLists.txt:162`.
- Windows SDK include/lib roots are configurable through `OPENTUNE_WINDOWS_KITS_INCLUDE_ROOT` and `OPENTUNE_WINDOWS_KITS_LIB_ROOT` in `CMakeLists.txt:82`, `CMakeLists.txt:84`.

**Runtime / Diagnostic Knobs:**
- `OPENTUNE_ORT_PROFILE` enables debug ONNX profiling in `Source/Inference/ModelFactory.cpp:15`, `Source/Inference/ModelFactory.cpp:189`.
- `OPENTUNE_SELFTEST` is read by the Standalone editor in `Source/Standalone/PluginEditor.cpp:450`.

**Secrets:**
- No `.env` files or credential files were found at the repo root.
- No secret-bearing runtime configuration path was detected in `Source/`.

## File Formats & User Data

**Audio / Model Formats:**
- Import formats are registered through JUCE audio formats in `Source/Audio/AudioFormatRegistry.cpp:171`; optional formats include FLAC, Ogg Vorbis, MP3, CoreAudio, and Windows Media where the build enables them in `Source/Audio/AudioFormatRegistry.cpp:134`, `CMakeLists.txt:626`, `CMakeLists.txt:634`.
- Export path remains WAV-based in `Source/PluginProcessor.cpp`.
- Model payloads are `.onnx` files: `models/rmvpe.onnx` and `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx`, packaged as `rmvpe.onnx` and `hifigan.onnx` by `CMakeLists.txt:704`, `CMakeLists.txt:705`, `CMakeLists.txt:757`, `CMakeLists.txt:779`, `CMakeLists.txt:848`.
- Presets use `.otpreset` files in `Source/Utils/PresetManager.cpp:66` and Standalone preset UI in `Source/Standalone/PluginEditor.cpp:1863`, `Source/Standalone/PluginEditor.cpp:1909`.

**App Preference Payloads:**
- Shared persisted preference keys cover language, theme, editing scheme, piano-roll visual settings, and zoom sensitivity in `Source/Utils/AppPreferences.cpp:9`, `Source/Utils/AppPreferences.cpp:205`, `Source/Utils/AppPreferences.cpp:234`.
- Standalone-only persisted preference keys cover shortcut bindings and mouse-trail theme in `Source/Utils/AppPreferences.cpp:18`, `Source/Utils/AppPreferences.cpp:228`.

## Testing / CI Boundaries

**Local Verification:**
- `OpenTuneTests` exposes `core`, `processor`, `ui`, `architecture`, `undo`, and `memory` suites in `Tests/TestMain.cpp:30`.
- CTest registers `OpenTuneCoreTests` in `CMakeLists.txt:961`, `CMakeLists.txt:963`.

**CI/CD:**
- No GitHub Actions workflow or GitLab CI config was found in the live repo.

---

*Integration audit: 2026-05-05*
