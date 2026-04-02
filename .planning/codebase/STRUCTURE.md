# Codebase Structure

**Generated:** 2026-04-02
**Project:** OpenTune

## Directory Layout

```
E:\TRAE\OpenTune0402\
├── Source/                     # All application source code
│   ├── Audio/                  # Async audio processing utilities
│   ├── DSP/                    # Digital signal processing modules
│   ├── Editor/                 # Editor factory (plugin entry point)
│   ├── Host/                   # Host integration layer (DAW/standalone)
│   ├── Inference/              # AI inference and rendering pipeline
│   ├── Services/               # Business logic services
│   ├── Standalone/             # Standalone app specific code
│   │   └── UI/                 # UI components
│   │       └── PianoRoll/      # Piano roll sub-components
│   └── Utils/                  # Shared utilities and data structures
├── ThirdParty/                 # Third-party libraries
│   └── r8brain-free-src-master/# High-quality resampling library
├── JUCE-master/                # JUCE framework (bundled)
├── onnxruntime-win-x64-1.17.3/ # ONNX Runtime binaries
├── models/                     # AI model files (RMVPE)
├── pc_nsf_hifigan_44.1k_ONNX/  # Vocoder model
├── Resources/                  # Application resources (fonts, icons)
├── Tests/                      # Unit tests
├── .planning/                  # Planning and codebase analysis documents
│   └── codebase/               # Codebase mapping output
├── docs/                       # Documentation
│   ├── adr/                    # Architecture Decision Records
│   └── architecture/           # Architecture diagrams
├── build/                      # Build output (generated)
├── CMakeLists.txt              # Main build configuration
└── AGENTS.md                   # Agent development guidelines
```

## Directory Purposes

### Source/Audio
- **Purpose:** Background audio processing utilities
- **Contains:** Async loaders that run off the audio thread
- **Key files:**
  - `Source/Audio/AsyncAudioLoader.h` - Background file loading with validity token pattern

### Source/DSP
- **Purpose:** Digital signal processing algorithms
- **Contains:** Resampling, spectral analysis, music theory inference
- **Key files:**
  - `Source/DSP/ResamplingManager.h` - Sample rate conversion using r8brain
  - `Source/DSP/MelSpectrogram.h` - Mel spectrogram computation (128 bins)
  - `Source/DSP/ScaleInference.h` - Musical key detection

### Source/Editor
- **Purpose:** Plugin editor entry point factory
- **Contains:** Factory function for creating editors
- **Key files:**
  - `Source/Editor/EditorFactory.h` - `createOpenTuneEditor()` function declaration

### Source/Host
- **Purpose:** Abstract host integration layer
- **Contains:** Interface and implementations for different host modes
- **Key files:**
  - `Source/Host/HostIntegration.h` - Abstract interface for DAW vs standalone mode

### Source/Inference
- **Purpose:** AI inference engine and rendering pipeline
- **Contains:** F0 extraction, vocoder, rendering management, caching
- **Key files:**
  - `Source/Inference/F0InferenceService.h` - CPU-only F0 extraction service
  - `Source/Inference/VocoderDomain.h` - Vocoder lifecycle encapsulation
  - `Source/Inference/RMVPEExtractor.h` - F0 extraction model (16kHz, hop=160)
  - `Source/Inference/PCNSFHifiGANVocoder.h` - Neural vocoder (44.1kHz, hop=512)
  - `Source/Inference/ModelFactory.h` - ONNX session creation
  - `Source/Inference/VocoderFactory.h` - Vocoder creation with backend selection
  - `Source/Inference/RenderCache.h` - Rendered audio storage with state machine
  - `Source/Inference/VocoderRenderScheduler.h` - Synthesis job scheduling
  - `Source/Inference/VocoderInferenceService.h` - Vocoder inference wrapper
  - `Source/Inference/IF0Extractor.h` - F0 extractor interface
  - `Source/Inference/VocoderInterface.h` - Vocoder interface
  - `Source/Inference/DmlConfig.h` - DirectML configuration
  - `Source/Inference/DmlVocoder.h` - DirectML vocoder implementation

### Source/Services
- **Purpose:** Business logic services
- **Contains:** High-level operations coordinating multiple components
- **Key files:**
  - `Source/Services/F0ExtractionService.h` - F0 extraction orchestration with thread pool

### Source/Standalone
- **Purpose:** Standalone application specific code
- **Contains:** Main editor, mode-specific factories
- **Key files:**
  - `Source/Standalone/PluginEditor.h` - Main window implementation

### Source/Standalone/UI
- **Purpose:** All UI components
- **Contains:** Visual components, look-and-feel, themes
- **Key files:**
  - `Source/Standalone/UI/PianoRollComponent.h` - Main pitch editing view
  - `Source/Standalone/UI/ArrangementViewComponent.h` - Timeline/clip view
  - `Source/Standalone/UI/TrackPanelComponent.h` - Track mixer strip
  - `Source/Standalone/UI/ParameterPanel.h` - Retune/vibrato parameter controls
  - `Source/Standalone/UI/MenuBarComponent.h` - Application menu
  - `Source/Standalone/UI/TransportBarComponent.h` - Play/stop controls
  - `Source/Standalone/UI/TopBarComponent.h` - Top toolbar with scale selector
  - `Source/Standalone/UI/TimelineComponent.h` - Time ruler
  - `Source/Standalone/UI/OpenTuneLookAndFeel.h` - Custom styling base
  - `Source/Standalone/UI/AuroraLookAndFeel.h` - Aurora theme
  - `Source/Standalone/UI/BlueBreezeLookAndFeel.h` - BlueBreeze theme
  - `Source/Standalone/UI/DarkBlueGreyLookAndFeel.h` - DarkBlueGrey theme
  - `Source/Standalone/UI/UIColors.h` - Color definitions
  - `Source/Standalone/UI/UiText.h` - Text/localization constants
  - `Source/Standalone/UI/ThemeTokens.h` - Theme property definitions
  - `Source/Standalone/UI/ToolIds.h` - Tool identifier enum
  - `Source/Standalone/UI/SmallButton.h` - Reusable button component
  - `Source/Standalone/UI/TimeConverter.h` - Time unit conversion
  - `Source/Standalone/UI/PlayheadOverlayComponent.h` - Playhead rendering
  - `Source/Standalone/UI/RippleOverlayComponent.h` - Ripple effect overlay
  - `Source/Standalone/UI/AutoRenderOverlayComponent.h` - Auto-render progress overlay
  - `Source/Standalone/UI/FrameScheduler.h` - UI frame scheduling
  - `Source/Standalone/UI/WaveformMipmap.h` - Waveform visualization

### Source/Standalone/UI/PianoRoll
- **Purpose:** Piano roll sub-components
- **Contains:** Rendering, interaction, undo support, correction
- **Key files:**
  - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h` - Drawing logic for F0 curves, notes, waveforms
  - `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.h` - Tool interactions (Select, Draw, HandDraw, LineAnchor)
  - `Source/Standalone/UI/PianoRoll/PianoRollUndoSupport.h` - Undo/redo for piano roll operations
  - `Source/Standalone/UI/PianoRoll/PianoRollCorrectionWorker.h` - Async auto-correction logic
  - `Source/Standalone/UI/PianoRoll/InteractionState.h` - Mouse/keyboard interaction state

### Source/Utils
- **Purpose:** Shared utilities and core data structures
- **Contains:** Note/curve data, undo, logging, hardware detection
- **Key files:**
  - `Source/Utils/Note.h` - Note and NoteSequence data structures
  - `Source/Utils/PitchCurve.h` - Pitch curve with immutable snapshots
  - `Source/Utils/PitchUtils.h` - Pitch conversion utilities (Hz ↔ MIDI)
  - `Source/Utils/TimeCoordinate.h` - Sample/time conversion (44.1kHz centric)
  - `Source/Utils/UndoAction.h` - Undo action definitions
  - `Source/Utils/ClipSnapshot.h` - Clip serialization structure
  - `Source/Utils/AppLogger.h` - Application logging
  - `Source/Utils/CpuFeatures.h` - CPU capability detection
  - `Source/Utils/CpuBudgetManager.h` - CPU resource management
  - `Source/Utils/GpuDetector.h` - GPU capability detection
  - `Source/Utils/DmlRuntimeVerifier.h` - DirectML runtime verification
  - `Source/Utils/SimdAccelerator.h` - SIMD optimization helpers
  - `Source/Utils/SilentGapDetector.h` - Silence detection for chunk boundaries
  - `Source/Utils/PresetManager.h` - Preset save/load
  - `Source/Utils/ModelPathResolver.h` - Model file location
  - `Source/Utils/LockFreeQueue.h` - Lock-free data structure
  - `Source/Utils/Error.h` - Result<T> error handling type
  - `Source/Utils/NoteGenerator.h` - Note generation from F0
  - `Source/Utils/PitchControlConfig.h` - Retune/vibrato configuration
  - `Source/Utils/KeyShortcutConfig.h` - Keyboard shortcut definitions
  - `Source/Utils/MouseTrailConfig.h` - Mouse trail effect configuration
  - `Source/Utils/ZoomSensitivityConfig.h` - Zoom sensitivity settings
  - `Source/Utils/LocalizationManager.h` - Multi-language support

## Key File Locations

### Entry Points
| File | Purpose |
|------|---------|
| `Source/PluginProcessor.h` | `OpenTuneAudioProcessor` class definition |
| `Source/PluginProcessor.cpp` | Audio processor implementation, creates processor instance |
| `Source/Standalone/PluginEditor.h` | `OpenTuneAudioProcessorEditor` class definition |
| `Source/Editor/EditorFactory.h` | Editor factory for plugin mode |

### Configuration
| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Build configuration, source lists, dependencies |
| `AGENTS.md` | Development guidelines and rules |

### Core Logic
| File | Purpose |
|------|---------|
| `Source/PluginProcessor.h:67-530` | Central processor class |
| `Source/Inference/F0InferenceService.h:24-98` | F0 service interface |
| `Source/Inference/VocoderDomain.h:27-88` | Vocoder lifecycle management |
| `Source/Inference/RenderCache.h:17-131` | Cache state machine |
| `Source/Utils/PitchCurve.h:111-381` | Pitch data management |
| `Source/Utils/TimeCoordinate.h` | Sample rate constants (44.1kHz) |
| `Source/Utils/UndoAction.h` | UndoManager class |

### AI Model Interfaces
| File | Purpose |
|------|---------|
| `Source/Inference/IF0Extractor.h:31-101` | F0 extractor interface |
| `Source/Inference/VocoderInterface.h:17-36` | Vocoder interface |
| `Source/Inference/ModelFactory.h` | F0 model creation factory |
| `Source/Inference/VocoderFactory.h` | Vocoder creation factory |

## Module Boundaries

### Layer Hierarchy
```
┌─────────────────────────────────────────┐
│            UI Layer                      │
│  (Standalone/UI/, PluginEditor)          │
│  - User interaction, visualization        │
│  - May depend on all layers below        │
├─────────────────────────────────────────┤
│            Services Layer                │
│  (Services/, Host/)                      │
│  - Business logic, orchestration          │
│  - May depend on Core, Inference         │
├─────────────────────────────────────────┤
│            Core Layer                    │
│  (PluginProcessor, Utils/)               │
│  - Data structures, state management      │
│  - May depend on Inference, Platform     │
├─────────────────────────────────────────┤
│            Inference Layer               │
│  (Inference/, DSP/)                      │
│  - AI model inference, DSP               │
│  - May depend on Platform only           │
├─────────────────────────────────────────┤
│            Platform Layer                │
│  (JUCE, ONNX Runtime, ThirdParty)        │
│  - Framework, external dependencies       │
│  - No internal dependencies              │
└─────────────────────────────────────────┘
```

### Dependency Rules
- **UI Layer:** May depend on all layers below
- **Services Layer:** May depend on Core, Inference, Platform
- **Core Layer:** May depend on Inference, Platform (NOT UI)
- **Inference Layer:** May depend on Platform only
- **Platform Layer:** No internal dependencies

### Namespace Organization
All code under `namespace OpenTune {}`

## Where to Add New Code

### New Feature
- **Core logic:** `Source/` appropriate subdirectory by layer
- **Audio processing:** Add to `Source/PluginProcessor.cpp`, update `Source/PluginProcessor.h`
- **Tests:** `Tests/` directory

### New UI Component
- **Component:** `Source/Standalone/UI/NewComponent.h/cpp`
- **Sub-component:** `Source/Standalone/UI/PianoRoll/` if piano roll related
- **Register in:** `CMakeLists.txt` target_sources(OpenTune ...)
- **Add to editor:** `Source/Standalone/PluginEditor.h` as member

### New AI Model
- **Interface:** Extend `Source/Inference/IF0Extractor.h` or create new interface
- **Implementation:** `Source/Inference/[ModelName]Extractor.h/cpp`
- **Factory update:** `Source/Inference/ModelFactory.cpp` or `VocoderFactory.cpp`
- **Model file:** Place in `models/` directory

### New DSP Algorithm
- **Location:** `Source/DSP/[AlgorithmName].h/cpp`
- **Include in CMakeLists.txt:** Add to `target_sources(OpenTune ...)`

### New Utility/Data Structure
- **Shared utility:** `Source/Utils/[UtilityName].h/cpp`
- **Include in CMakeLists.txt:** Add to `target_sources(OpenTune ...)`

### New Test
- **Location:** `Tests/[TestName].cpp`
- **Include in CMakeLists.txt:** Add to `add_executable(OpenTuneTests ...)`

### New Undo Action
- **Location:** `Source/Utils/UndoAction.h` - add new action class
- **Implement:** `undo()` and `redo()` methods
- **Register:** Call `processor.getUndoManager().addAction()` when performing action

### New Theme
- **Definition:** `Source/Standalone/UI/[ThemeName]Theme.h`
- **LookAndFeel:** `Source/Standalone/UI/[ThemeName]LookAndFeel.h/cpp`
- **Register:** Update theme switching in `Source/Standalone/PluginEditor.cpp`

## Naming Conventions

### Files
- PascalCase for class files: `PluginProcessor.cpp`, `PitchCurve.h`
- Interface prefix `I`: `IVocoder.h`, `IF0Extractor.h`
- Component suffix `Component`: `PianoRollComponent.h`, `TransportBarComponent.h`

### Directories
- PascalCase for modules: `Inference/`, `Services/`, `Standalone/`
- Lowercase for config/build: `build/`, `models/`

### Code
- **Classes:** PascalCase (e.g., `OpenTuneAudioProcessor`, `RenderCache`)
- **Functions/Methods:** camelCase (e.g., `prepareImportClip`, `extractF0`)
- **Member variables:** trailing underscore (e.g., `tracks_`, `renderingManager_`)
- **Constants:** `k` prefix (e.g., `kRenderSampleRate`, `kDefaultGlobalCacheLimitBytes`)
- **Namespaces:** PascalCase (e.g., `OpenTune`)

## Special Directories

### .planning/
- **Purpose:** Development planning and codebase analysis
- **Contains:** Phase plans, codebase mapping documents, debug artifacts
- **Generated:** Yes (by GSD tools)
- **Committed:** Yes (for team visibility)

### JUCE-master/
- **Purpose:** Bundled JUCE framework source
- **Generated:** No (third-party)
- **Committed:** Yes
- **Note:** Do not modify; update via JUCE upstream

### ThirdParty/
- **Purpose:** Third-party library sources
- **Contains:** r8brain resampling library
- **Generated:** No
- **Committed:** Yes

### onnxruntime-win-x64-1.17.3/
- **Purpose:** ONNX Runtime pre-built binaries
- **Generated:** No (downloaded from Microsoft)
- **Committed:** Yes (for build simplicity)
- **Note:** Contains DLLs linked at runtime

### build/
- **Purpose:** CMake build output
- **Generated:** Yes
- **Committed:** No (in .gitignore)
- **Output:** `OpenTune_artefacts/Release/Standalone/OpenTune.exe`

---

*Structure analysis: 2026-04-02*
