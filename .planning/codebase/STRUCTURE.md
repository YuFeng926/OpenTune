# Codebase Structure

**Analysis Date:** 2026-06-03

## Directory Layout

```
OpenTune/
├── CMakeLists.txt              # Root build config (1378 lines): targets, dependencies, platform logic
├── CMakePresets.json           # Pre-defined CMake configure/build presets (VS2022, Xcode)
├── AGENTS.md                   # Agent workflow instructions (Chinese)
├── README.md                   # Project overview, build guide, release notes
├── .gitignore                  # Excludes JUCE-master/, ThirdParty/, build artifacts
├── .gitattributes              # LFS tracking rules
├── .planning/                  # Planning artifacts (codebase maps, phase plans)
│   └── codebase/               # Auto-generated architecture docs
├── Source/                     # ★ All application source code
├── JUCE-master/                # ★ JUCE 8 framework (gitignored, cloned externally)
├── ThirdParty/                 # ★ Third-party dependencies (gitignored)
│   ├── ARA_SDK-releases-2.2.0/ # Celemony ARA SDK
│   ├── r8brain-free-src-master/# Voxengo r8brain resampler
│   ├── onnxruntime-win-x64-1.24.4/  # ONNX Runtime CPU (Windows)
│   ├── onnxruntime-dml-1.24.4/      # ONNX Runtime DML (Windows)
│   ├── onnxruntime-osx-arm64-1.24.4/# ONNX Runtime (macOS)
│   ├── microsoft.ai.directml.1.15.4/     # DirectML SDK
│   ├── microsoft.direct3d.d3d12.1.619.1/ # D3D12 Agility SDK
│   └── soundtouch-2.3.3/          # SoundTouch time-stretch library
├── Resources/                  # Binary resources compiled into JUCE BinaryData
│   ├── Fonts/                  # HONORSansCN-Medium.ttf
│   ├── PianoSamples-mp3/       # Piano key audition samples (88 files)
│   ├── UI/assets/              # Theme images (PNGs)
│   └── AppIcon.png             # Application icon
├── models/                     # AI model files (gitignored, from releases)
│   ├── rmvpe.onnx              # RMVPE F0 extraction model
│   └── GAME/                   # GAME note generator ONNX bundle
├── pc_nsf_hifigan_44.1k_ONNX/  # Vocoder model files (gitignored, from releases)
├── docs/                       # User documentation
│   └── UserGuide.html
├── Tests/                      # Unit & integration test source files
├── cmake/                      # Custom CMake modules
│   └── OpenTuneJuceVST3ClientOverlay.cmake
├── Installer/                  # Installer scripts/packaging
├── dist/                       # Distribution output
├── Python/                     # Python utility scripts
├── build-ara-overlay-vs18-clean/# Build output directory (gitignored)
└── LICENSES/                   # Third-party license files
```

## Directory Purposes

**Source/ (root level):**
- Purpose: Core data model files shared across all build targets
- Contains: `PluginProcessor.{h,cpp}` (main orchestrator), `SourceStore.{h,cpp}`, `MaterializationStore.{h,cpp}`, `StandaloneArrangement.{h,cpp}`
- Key files: `PluginProcessor.h` (885 lines, aggregation header), `MaterializationStore.h` (318 lines)

**Source/ARA/:**
- Purpose: ARA2 protocol integration for VST3 deep DAW embedding
- Contains: `AudioSource.{h,cpp}` (source identity/sample access), `AudioModification.{h,cpp}` (content/materialization binding), `PlaybackRegion.{h,cpp}` (placement), `OpenTuneDocumentController.{h,cpp}` (projection/persistence with partial archive `ARAStoreObjectsFilter`/`ARARestoreObjectsFilter` support), `OpenTuneEditorView.{h,cpp}` (host selection UI role), `OpenTunePlaybackRenderer.{h,cpp}` (assigned-region rendering)
- Only compiled when `OPENTUNE_ENABLE_ARA=ON` (default) in VST3 target

**Source/Audio/:**
- Purpose: Audio format I/O and async loading abstractions
- Contains: `AudioFormatRegistry.{h,cpp}`, `AsyncAudioLoader.h`
- Key files: `AudioFormatRegistry.cpp` (WAV/FLAC/OGG/MP3 registration)

**Source/DSP/:**
- Purpose: Classical digital signal processing algorithms
- Contains: `ResamplingManager`, `MelSpectrogram`, `ChromaKeyDetector`, `ReferenceAutoAlign`, `AutoTunePitchShifter`, `TimeGridPatchBuilder`, `ReferenceFeatures.h`
- Key files: `ResamplingManager.{h,cpp}` (r8brain wrapper), `AutoTunePitchShifter.{h,cpp}` (monitoring pitch shift)

**Source/Inference/:**
- Purpose: AI inference layer — ONNX model loading, F0 extraction, vocoder synthesis, note generation, render caching
- Contains: 32 files including `F0InferenceService`, `VocoderDomain`, `VocoderRenderScheduler`, `VocoderInferenceService`, `RenderCache`, `RMVPEExtractor`, `PCNSFHifiGANVocoder`, `GameNoteGenerator`, `ModelFactory`, `INoteGenerator.h`, `IF0Extractor.h`, `SoundTouchStretcher`, `TimeStretchCache`, `ChunkRenderStrategy.h`
- Key files: `F0InferenceService.{h,cpp}` (RMVPE extraction), `VocoderDomain.{h,cpp}` (vocoder orchestration)

**Source/Editor/:**
- Purpose: Shared editor infrastructure across formats
- Contains: `EditorFactory.h` (shared interface), `EditorFactoryPlugin.cpp` (VST3 entry), `Preferences/` (SharedPreferencePages, StandalonePreferencePages, TabbedPreferencesDialog), AutoRenderOverlayComponent, PitchShiftDialogContent, ConfirmDialogContent, RenderBadgeComponent
- Key files: `EditorFactory.h` (single factory function `createOpenTuneEditor()`)

**Source/Plugin/:**
- Purpose: VST3 plugin-specific editor and capture module
- Contains: `PluginEditor.{h,cpp}` (VST3 editor shell), `Capture/` (10 files: CaptureSession, CaptureRingBuffer, CaptureSegment, CaptureCompactor, CapturePersistence)
- Compile-guard: `JucePlugin_Build_VST3`

**Source/Standalone/:**
- Purpose: Standalone application editor and UI components
- Contains: `PluginEditor.{h,cpp}` (319 lines, full multi-track editor), `EditorFactoryStandalone.cpp`, `StandaloneArrangementHelpers.h`, `UI/` (43 files)
- Compile-guard: `JucePlugin_Build_Standalone`

**Source/Standalone/UI/:**
- Purpose: Reusable UI components shared across both editor formats
- Contains: `PianoRollComponent` (shared), `ParameterPanel`, `MenuBarComponent`, `TransportBarComponent`, `TopBarComponent`, `TrackPanelComponent` (Standalone only), `ArrangementViewComponent` (Standalone only), `PlayheadOverlayComponent`, themes (AuroraLookAndFeel, BlueBreeze, DarkBlueGrey, Overdose), `FrameScheduler`, `SmallButton`, `ToolbarIcons`, `WaveformMipmap`
- Key files: `PianoRollComponent.{h,cpp}` (shared between Standalone and VST3 editors), `ArrangementViewComponent.{h,cpp}` (Standalone-only multi-track view)

**Source/Standalone/UI/PianoRoll/:**
- Purpose: Piano roll rendering substructure — renderers, tool handlers, interaction state, correction workers
- Contains: `PianoRollRenderer`, `PianoRollRenderModelCache`, `PianoRollVisualInvalidation`, `PianoRollToolHandler`, `PianoRollCorrectionWorker`, `InteractionState`
- Key files: `PianoRollToolHandler.{h,cpp}` (pen/note/anchor/time-grid tool logic with undo actions)

**Source/Services/:**
- Purpose: Async background service operations
- Contains: `F0ExtractionService.{h,cpp}` (materialization refresh scheduling with thread pool), `ReferenceAnalysisService.{h,cpp}` (async reference alignment feature extraction), `ImportedClipF0Extraction.h`
- Key files: `F0ExtractionService.{h,cpp}` (queue-based materialization refresh, 1 worker thread, 64 capacity)

**Source/Utils/:**
- Purpose: Shared utilities, data types, and cross-cutting helpers
- Contains: 67 files including `ProjectSession`, `ProjectModel`, `ProjectPersistence`, `UndoManager`, `PitchCurve`, `TimeGrid`, `PitchUtils`, `Note.h`, `AppPreferences`, `AppLogger`, `CpuBudgetManager`, `AccelerationDetector`, `SimdAccelerator`, `PianoKeyAudition`, `PlacementActions`, `PianoRollEditAction`, `PitchShiftEditAction`, `TimeGridEditAction`, `CompositeUndoAction`, `SilentGapDetector`, `PlacementClipboard`, `LocalizationManager`, `Error.h`, `LockFreeQueue.h`, `ModelPathResolver`, `AudioEditingScheme`, `ParameterPanelSync`, `MouseTrailConfig`, `ZoomSensitivityConfig`, `WindowDllSearchPath`, `OnnxRuntimeDelayLoadHook`, `D3D12AgilityBootstrap`, `VocoderModelWeight`, `TrackConstants`, `SourceWindow`, `TimeCoordinate`, `SnapUtils`
- Key files: `ProjectSession.{h,cpp}` (164 lines, project Open/Save/SaveAs lifecycle), `UndoManager.{h,cpp}` (undo/redo stack), `PitchCurve.{h,cpp}` (pitch curve data model), `TimeGrid.{h,cpp}` (vocal time-stretch grid)

**Source/Plugin/Capture/:**
- Purpose: VST3 live audio recording and pipeline integration
- Contains: `CaptureSession.{h,cpp}` (259 lines, state machine: Idle→Capturing→Processing), `CaptureRingBuffer.{h,cpp}`, `CaptureSegment.{h,cpp}`, `CaptureCompactor.{h,cpp}`, `CapturePersistence.{h,cpp}`
- Key files: `CaptureSession.h` (session lifecycle, segment state machine, `SubmitForRenderFn` callback)

**Tests/:**
- Purpose: C++ unit and integration tests using JUCE UnitTest framework
- Contains: 30 test files organized by module
- Key files: `TestMain.cpp`, `TestSupport.h`, module-specific tests (GameNoteGenerator, AutoTunePitchShifter, MaterializationStoreTimeGrid, SoundTouchStretcher, Stage2Worker, UndoManager, ReferenceAutoAlign, etc.)

## Key File Locations

**Entry Points:**
- `Source/Standalone/EditorFactoryStandalone.cpp`: Standalone application entry (creates editor via `createOpenTuneEditor()`)
- `Source/Plugin/EditorFactoryPlugin.cpp`: VST3 plugin entry (creates editor via `createOpenTuneEditor()`)
- `Source/Editor/EditorFactory.h`: Shared editor factory interface
- `Source/PluginProcessor.cpp`: Main audio processor implementation (~2000+ lines)

**Configuration:**
- `CMakeLists.txt`: All build configuration, dependencies, target definitions
- `CMakePresets.json`: CMake preset definitions for VS2022/Xcode
- `Source/Utils/AppPreferences.{h,cpp}`: Runtime user preferences persistence
- `cmake/OpenTuneJuceVST3ClientOverlay.cmake`: JUCE VST3 client overlay patching

**Core Logic:**
- `Source/PluginProcessor.h`: Central orchestrator header (all subsystems aggregated)
- `Source/SourceStore.{h,cpp}`: Source audio data store (immutable truth)
- `Source/MaterializationStore.{h,cpp}`: Editable audio payload store
- `Source/StandaloneArrangement.{h,cpp}`: Multi-track timeline placement model
- `Source/Inference/F0InferenceService.{h,cpp}`: RMVPE F0 extraction
- `Source/Inference/VocoderDomain.{h,cpp}`: Vocoder orchestration
- `Source/Inference/GameNoteGenerator.{h,cpp}`: GAME ONNX note generator
- `Source/Inference/RenderCache.{h,cpp}`: Per-materialization render chunk cache
- `Source/Inference/ChunkRenderStrategy.h`: Render scheduling strategy
- `Source/DSP/ResamplingManager.{h,cpp}`: Sample rate conversion

**UI - Shared Components:**
- `Source/Standalone/UI/PianoRollComponent.{h,cpp}`: Piano roll editor (used by both formats)
- `Source/Standalone/UI/ParameterPanel.{h,cpp}`: Retune speed, vibrato controls
- `Source/Standalone/UI/MenuBarComponent.{h,cpp}`: Top menu bar
- `Source/Standalone/UI/TransportBarComponent.{h,cpp}`: Play/stop/record transport
- `Source/Standalone/UI/AuroraLookAndFeel.{h,cpp}`: Primary UI theme
- `Source/Standalone/UI/UIColors.h`: Color palette definitions
- `Source/Standalone/UI/ThemeTokens.h`: Design token aliases
- `Source/Standalone/UI/ToolIds.h`: Tool type identifier constants
- `Source/Standalone/UI/ToolbarIcons.h`: Toolbar icon definitions

**UI - Standalone Only:**
- `Source/Standalone/UI/ArrangementViewComponent.{h,cpp}`: Multi-track timeline view
- `Source/Standalone/UI/TrackPanelComponent.{h,cpp}`: Track list sidebar

**ARA Integration:**
- `Source/ARA/OpenTuneDocumentController.{h,cpp}`: ARA document lifecycle controller
- `Source/ARA/OpenTuneEditorView.{h,cpp}`: ARA editor-view role and host `ViewSelection` bridge
- `Source/ARA/OpenTunePlaybackRenderer.{h,cpp}`: ARA real-time renderer
- `Source/ARA/AudioSource.{h,cpp}` / `AudioModification.{h,cpp}` / `PlaybackRegion.{h,cpp}`: official ARA object-model projections

**Testing:**
- `Tests/TestMain.cpp`: Test runner entry point
- `Tests/TestSupport.h`: Shared test utilities and fixtures
- `Tests/GameNoteGeneratorTests.cpp`: GAME model inference tests
- `Tests/MaterializationStoreTimeGridTests.cpp`: TimeGrid storage/identity tests
- `Tests/IntegrationPipelineTests.cpp`: End-to-end import-render pipeline
- `Tests/InvariantContractTests.cpp`: Core data model invariant verification

## Naming Conventions

**Files:**
- PascalCase for source files: `PluginProcessor.cpp`, `SourceStore.h`, `PianoRollComponent.h`
- `.h` / `.cpp` pairing for classes with implementation
- Some headers are `.h`-only (templates, inline utilities): `Note.h`, `Error.h`, `SnapUtils.h`, `TrackConstants.h`
- Underscore prefix for "internal" headers in Capture module: (none observed — all public)

**Directories:**
- PascalCase for module directories: `Source/ARA/`, `Source/Inference/`, `Source/DSP/`
- Flat organization within modules (minimal sub-nesting)
- UI has sub-directory `PianoRoll/` for renderer decomposition

**Classes:**
- PascalCase, prefixed with `OpenTune` for major framework types: `OpenTuneAudioProcessor`, `OpenTuneAudioProcessorEditor`, `OpenTuneDocumentController`
- Unprefixed PascalCase for domain types in `OpenTune` namespace: `SourceStore`, `MaterializationStore`, `RenderCache`, `VocoderDomain`
- Interface prefix `I`: `INoteGenerator`, `IF0Extractor`, `VocoderInterface`
- Nested types in owning class header: `OpenTuneAudioProcessor::PlaybackReadSource`, `StandaloneArrangement::Placement`

**Functions:**
- camelCase for methods: `prepareToPlay()`, `processBlock()`, `readPlaybackAudio()`
- camelCase for free functions: `fillF0GapsForVocoder()`, `computeRegionBlockRenderSpan()`
- `get`/`set` prefix for accessors: `getSampleRate()`, `setPlaying()`

**Variables:**
- camelCase with trailing underscore for member variables: `currentSampleRate_`, `sourceStore_`, `materializationStore_`
- camelCase without underscore for locals and parameters: `sampleRate`, `inBuffer`, `out`
- `k` prefix for constants within classes: `kTrackCount`, `kMediaDirectoryName` (constexpr char*)
- UPPER_CASE for compile-time constants in namespaces: `AudioConstants::DefaultSampleRate`, `MAX_TRACKS`

**Namespaces:**
- `OpenTune` — primary application namespace (most code)
- `OpenTune::Capture` — VST3 capture module sub-namespace
- `OpenTune::PluginUI` — VST3 plugin editor sub-namespace

**CMake Targets:**
- `OpenTune` — shared library target (all common sources)
- `OpenTune_Standalone` — standalone executable (inherits from `OpenTune`)
- `OpenTune_VST3` — VST3 plugin target (inherits from `OpenTune`)
- `OpenTuneResources` — JUCE BinaryData target (embedded resources)

## Where to Add New Code

**New AI Model:**
- Implementation: `Source/Inference/NewModel.{h,cpp}` (implementing `IF0Extractor` or `VocoderInterface`)
- Registration: `Source/Inference/ModelFactory.{h,cpp}` (factory method)
- Tests: `Tests/NewModelTests.cpp`

**New DSP Algorithm:**
- Implementation: `Source/DSP/NewAlgo.{h,cpp}`
- Tests: `Tests/NewAlgoTests.cpp`

**New UI Component (shared between formats):**
- Implementation: `Source/Standalone/UI/NewComponent.{h,cpp}`
- Registration: Add to both `Source/Standalone/PluginEditor.cpp` and `Source/Plugin/PluginEditor.cpp`
- CMake: Add to `target_sources(OpenTune ...)` in root `CMakeLists.txt` (lines 548-600 area)

**New Standalone-only UI Component:**
- Implementation: `Source/Standalone/UI/NewComponent.{h,cpp}`
- Consumer: Only referenced in `Source/Standalone/PluginEditor.{h,cpp}`
- CMake: Add to shared `target_sources(OpenTune ...)` (shared UI is always compiled)

**New Undoable Action:**
- Implementation: `Source/Utils/NewEditAction.{h,cpp}`
- Pattern: Follow `PianoRollEditAction`, `TimeGridEditAction`, `PitchShiftEditAction` — implement `perform()`/`undo()` returning `bool`
- Registration: Call `undoManager_.record(std::make_unique<NewEditAction>(...))` from the action's caller

**New Background Service:**
- Implementation: `Source/Services/NewService.{h,cpp}`
- Aggregation: Add as member to `OpenTuneAudioProcessor` in `Source/PluginProcessor.h`
- Thread model: Use `juce::ThreadPool` or dedicated `std::thread` with `std::condition_variable`

**New Test File:**
- Implementation: `Tests/NewTests.cpp`
- Pattern: Inherit from `juce::UnitTest`, use `beginTest("name")` / `expect(...)` / `expectEquals(...)`
- Registration: Instantiate static instance in file
- CMake: Auto-discovered via `file(GLOB)` in test target

**New Build Target:**
- Add conditional `target_sources()` block in root `CMakeLists.txt` guarded by format defines
- Add format-specific `target_compile_definitions` for `JucePlugin_Build_*` macros

## Special Directories

**JUCE-master/:**
- Purpose: JUCE 8 framework source (vendored, not a git submodule)
- Generated: No (cloned externally by developer)
- Committed: No (`.gitignore` excludes it due to large binary size)

**ThirdParty/:**
- Purpose: All external C/C++ dependencies — ONNX Runtime, ARA SDK, r8brain, DirectML, D3D12, SoundTouch
- Generated: No (downloaded/extracted externally by developer)
- Committed: No (`.gitignore` excludes all of it — ~hundreds of MB of binaries)

**models/ & pc_nsf_hifigan_44.1k_ONNX/:**
- Purpose: AI model files (ONNX format) for inference
- Generated: No (downloaded from GitHub Releases)
- Committed: No (`.gitignore` excludes large binary model files)

**.planning/:**
- Purpose: Codebase analysis documents generated by `/gsd-map-codebase` and consumed by `/gsd-plan-phase` / `/gsd-execute-phase`
- Generated: Yes (by planning tools)
- Committed: Yes (lightweight markdown, enables phase continuity)

**build-ara-overlay-vs18-clean/ (and similar):**
- Purpose: CMake build output directories
- Generated: Yes (by `cmake --preset` / `cmake --build`)
- Committed: No (`.gitignore`)

**dist/:**
- Purpose: Distribution/packaging output
- Generated: Yes (by installer scripts)
- Committed: No (`.gitignore`)

---

*Structure analysis: 2026-06-03*
