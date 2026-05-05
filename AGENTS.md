# AGENTS.md - OpenTune

## Project Overview

OpenTune is an AI-powered pitch correction application built with the JUCE framework
(C++17). It ships as both a **Standalone** desktop app and a **VST3/ARA** plugin. It uses
PC-NSF-HiFiGAN neural vocoder and RMVPE pitch extraction via ONNX Runtime (DirectML GPU
acceleration on Windows) for formant-preserving vocal pitch shifting. Company: DAYA.
License: AGPL-3.0.

## Build Commands

### Requirements

- CMake 3.22+
- C++17 compiler (MSVC 2022 / Visual Studio 17 on Windows)
- Dependencies: JUCE (vendored, `JUCE-master/`), ARA SDK 2.2.0 (vendored,
  `ThirdParty/ARA_SDK-releases-2.2.0/`), r8brain-free-src (vendored, `ThirdParty/`)
- ONNX Runtime: DirectML 1.15.4 + D3D12 Agility SDK 1.619.1 (auto-downloaded via NuGet)
- `.onnx` model files are tracked via Git LFS -- run `git lfs pull` after cloning

### Configure and Build (Windows)

```bash
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Output:
- Standalone: `build/OpenTune_artefacts/Release/Standalone/OpenTune.exe`
- VST3: `build/OpenTune_artefacts/Release/VST3/OpenTune.vst3`

Post-build automatically copies ONNX Runtime DLLs and model files to the output directory.

### macOS / Linux

Not officially supported yet. The CMakeLists.txt targets Windows/MSVC. A macOS or Linux
build would require providing a platform-appropriate ONNX Runtime library and adjusting
the linker configuration.

### LSP Support

`CMAKE_EXPORT_COMPILE_COMMANDS=ON` is set. After cmake configure, `compile_commands.json`
is generated in the build directory for clangd / LSP integration.

## Tests

There is **no test infrastructure**. Tests and CTest registration are commented out in
CMakeLists.txt. No test source files exist in the repository.

## Linting / Formatting

No `.clang-format`, `.clang-tidy`, or `.editorconfig` files are configured. No CI/CD
pipelines exist. Follow the conventions documented below to maintain consistency.

## Project Structure

```
Source/
  PluginProcessor.cpp/h        # Core audio processor (Standalone + VST3 shared)
  SourceStore.cpp/h             # Original audio source storage (identity, buffer, metadata)
  MaterializationStore.cpp/h    # Editable materialization storage (notes, F0, render cache)
  StandaloneArrangement.cpp/h   # Standalone multi-track timeline (12 tracks, placements, playback snapshot)
  Inference/                    # AI model layer (ONNX Runtime wrappers)
    IF0Extractor.h              # F0 extractor abstract interface
    RMVPEExtractor.h            # RMVPE F0 pitch extraction (implements IF0Extractor)
    F0InferenceService.h        # CPU-only F0 extraction service (concurrent, auto-release idle model)
    ModelFactory.h              # Static factory for IF0Extractor creation
    VocoderInterface.h          # Vocoder abstract interface
    OnnxVocoderBase.h           # ONNX vocoder base class (mel/F0 tensor management)
    PCNSFHifiGANVocoder.h       # Pitch-Controllable NSF-HiFiGAN vocoder
    DmlVocoder.h                # DirectML GPU-accelerated vocoder (IO binding)
    DmlConfig.h                 # DirectML adapter configuration
    VocoderFactory.h            # Vocoder factory (CPU/DML/CoreML backends)
    VocoderInferenceService.h   # Vocoder ONNX session lifecycle management
    VocoderRenderScheduler.h    # Serial vocoder synthesis task queue (DML requires serial Run())
    VocoderDomain.h             # Vocoder scheduling facade (InferenceService + RenderScheduler)
    RenderCache.h               # Cached render results (per-chunk, multi-rate, LRU eviction)
  DSP/                          # Signal processing
    MelSpectrogram.h            # Log-mel spectrogram (2048 FFT, 128 mel, 512 hop)
    ResamplingManager.h         # Audio resampling (r8brain)
    ChromaKeyDetector.h         # Musical key/scale detection (Chroma/HPCP + template matching)
    CrossoverMixer.h            # LR4 crossover mixer (14kHz, rendered LPF + dry HPF)
  Services/                     # Background services
    F0ExtractionService.h       # Async F0 extraction task manager (worker pool, dedup, cancel)
    ImportedClipF0Extraction.h  # Convenience wrapper for F0 extraction from materialization
  ARA/                          # VST3 ARA integration
    OpenTuneDocumentController.h # ARA document controller (lifecycle callbacks)
    OpenTunePlaybackRenderer.h   # ARA playback renderer (fills ARA regions with rendered audio)
    VST3AraSession.h             # ARA session manager (region/source ↔ SourceStore/MaterializationStore)
  Standalone/                   # Standalone app
    PluginEditor.h              # Standalone main editor (central mediator, all UI coordination)
    EditorFactoryStandalone.cpp # Standalone editor factory
    UI/                         # UI components
      PianoRollComponent.h      # Piano roll editor (F0 curve + note editing, tools, zoom)
      ParameterPanel.h          # Side parameter panel (Retune Speed, Vibrato knobs, tool select)
      TopBarComponent.h         # Top navigation container (MenuBar + TransportBar)
      MenuBarComponent.h        # Menu bar (File/Edit/View, JUCE MenuBarModel)
      TransportBarComponent.h   # Transport controls (play/stop/loop, BPM, key, time display)
      TrackPanelComponent.h     # Track side panel (mute/solo/volume/meter per track)
      ArrangementViewComponent.h # Multi-track arrangement view (clip drag, waveform, timeline)
      ThemeTokens.h             # Theme system (BlueBreeze / DarkBlueGrey / Aurora)
      AuroraLookAndFeel.h       # Aurora theme renderer (neon knobs, glass buttons)
      OpenTuneLookAndFeel.h     # Base LookAndFeel (knob/slider/button rendering)
      FrameScheduler.h          # Frame-level task scheduler (priority-based repaint coalescing)
      TimeConverter.h           # Time ↔ pixel coordinate conversion
      PlayheadOverlayComponent.h # Playhead vertical line overlay
      WaveformMipmap.h          # Waveform multi-level mipmap + LRU cache
      UIColors.h                # Global UI color constants and metrics
      ToolIds.h                 # Edit tool IDs (AutoTune/Select/DrawNote/LineAnchor/HandDraw)
      ToolbarIcons.h            # SVG toolbar icon paths
      SmallButton.h             # Small rounded button component
      RippleOverlayComponent.h  # Mouse click ripple effect overlay
      OpenTuneTooltipWindow.h   # Dark-style tooltip with shortcut badge
      UiText.h                  # Localized tooltip text factory
      PianoRoll/                # Piano roll subsystem (extracted from PianoRollComponent)
        InteractionState.h      # Interaction state container (selection, drag, anchor, draw)
        PianoRollCorrectionWorker.h # Background pitch correction worker (4 correction types)
        PianoRollRenderer.h     # Piano roll renderer (F0 lines, notes, grid, beats)
        PianoRollToolHandler.h  # Mouse/keyboard → tool state machine router
        PianoRollVisualInvalidation.h # Visual invalidation flags and refresh priorities
  Plugin/                       # VST3 plugin
    PluginEditor.h              # VST3/ARA plugin editor shell (Timer-polled, ARA region projection)
  Audio/                        # Audio subsystem
    AudioFormatRegistry.h       # Audio format registration (WAV/AIFF/FLAC/MP3) and file probing
    AsyncAudioLoader.h          # Background async audio loader (thread pool, cancel token, mipmap)
  Editor/                       # Editor factory and overlays
    EditorFactory.h             # Factory function: createOpenTuneEditor() (Standalone vs VST3)
    AutoRenderOverlayComponent.h # Render status overlay (progress animation, chunk stats)
    RenderBadgeComponent.h      # Floating render status badge
    Preferences/                # Preferences dialog
      TabbedPreferencesDialog.h # Tabbed preferences dialog container
      SharedPreferencePages.h   # Shared preference pages (Standalone + VST3)
      StandalonePreferencePages.h # Standalone-only preference pages
  Host/                         # Host integration
    HostIntegration.h           # Host integration abstraction
  Utils/                        # Utilities
    PitchCurve.h                # Immutable-snapshot pitch curve (COW, atomic lock-free reads)
    Note.h                      # Note / LineAnchor / NoteSequence data structures
    NoteGenerator.h             # Auto-generate notes from F0 (segmentation, scale snap, vibrato)
    PitchUtils.h                # Pitch math (MIDI↔freq, retune blend, note names)
    PitchControlConfig.h        # Default pitch correction parameters
    SimdPerceptualPitchEstimator.h # Perceptual pitch estimation (PIP, VNC, Fletcher-Munson)
    AppLogger.h                 # Static logging system (file output)
    UndoManager.h               # Linear undo stack (500 levels, cursor-based)
    PianoRollEditAction.h       # Piano roll undo action (notes + segments snapshot)
    PlacementActions.h          # Placement undo actions (split/merge/delete/move/gain)
    LockFreeQueue.h             # Lock-free MPMC queue (atomic, pre-allocated)
    SilentGapDetector.h         # Silent gap detection (-40dBFS, 50ms min, spectral gate)
    F0Timeline.h                # F0 frame ↔ time bidirectional mapping (100fps @ 16kHz)
    TimeCoordinate.h            # Time ↔ sample conversion (kRenderSampleRate = 44100)
    Error.h                     # Result<T> type (Rust-style ErrorCode + message)
    AccelerationDetector.h      # GPU / DirectML / CoreML hardware detection
    CpuFeatures.h               # CPU SIMD detection (SSE2/AVX/AVX2/AVX-512)
    CpuBudgetManager.h          # ONNX thread budget allocation (GPU vs CPU mode)
    ModelPathResolver.h         # ONNX model/DLL path resolution (multi-fallback search)
    AppPreferences.h            # Global app preferences (theme, language, rendering priority)
    PresetManager.h             # Preset save/load/delete (XML format)
    LocalizationManager.h       # Multi-language i18n (EN/ZH/JA/RU/ES, singleton, listener)
    KeyShortcutConfig.h         # Keyboard shortcut configuration and mapping
    AudioEditingScheme.h        # Editing scheme selector (CorrectedF0Primary vs NotesPrimary)
    ParameterPanelSync.h        # Parameter panel ↔ selection sync context
    PianoRollVisualPreferences.h # Piano roll visual preferences (note names, chunk borders)
    ZoomSensitivityConfig.h     # Zoom sensitivity settings
    MouseTrailConfig.h          # Mouse trail effect theme config
    PianoKeyAudition.h          # 88-key piano audition (MP3 samples, lock-free FIFO)
    MaterializationState.h      # F0 extraction state enum (NotRequested/Extracting/Ready/Failed)
    MaterializationTimelineProjection.h # Timeline ↔ local time projection for materializations
    SourceWindow.h              # Source absolute window (sourceId + time range)
ThirdParty/
  r8brain-free-src-master/      # Vendored resampling library
  ARA_SDK-releases-2.2.0/       # Vendored ARA SDK for VST3 ARA extension
pc_nsf_hifigan_44.1k_ONNX/     # PC-NSF-HiFiGAN vocoder model (~50 MB, hop=512, 128 mel)
models/                         # ONNX model files (Git LFS) -- contains rmvpe.onnx
Resources/                      # App icon, fonts, piano samples
  Fonts/                        # Bundled fonts
  PianoSamples-mp3/             # Piano key audition MP3 samples
```

## Code Style Guidelines

### Naming Conventions

| Element              | Convention                          | Example                              |
|----------------------|-------------------------------------|--------------------------------------|
| Classes / Structs    | PascalCase                          | `VocoderDomain`, `TrackState`        |
| Interfaces (ABC)     | `I` prefix + PascalCase             | `IF0Extractor`, `VocoderInterface`   |
| Methods              | camelCase                           | `extractF0()`, `prepareToPlay()`     |
| Member variables     | camelCase + trailing underscore     | `sampleRate_`, `inferenceReady_`     |
| Local variables      | camelCase                           | `numSamples`, `clipGain`             |
| Parameters           | camelCase                           | `sampleRate`, `modelDir`             |
| Class constants      | `k` prefix + PascalCase             | `kMaxAudioDurationSec`              |
| Namespace constants  | PascalCase                          | `DefaultSampleRate`                  |
| Legacy constants     | UPPER_SNAKE_CASE                    | `MAX_TRACKS`, `MENU_BAR_HEIGHT`      |
| Enums                | `enum class`, PascalCase type+values| `enum class LogLevel { Info, Error }`|
| Namespaces           | PascalCase                          | `OpenTune`, `AudioConstants`         |
| Template params      | Single letter or PascalCase         | `T`, `ClipType`                      |

### Getters / Setters / Predicates

- Getters: `get` prefix -- `getSnapshot()`, `getPosition()`
- Setters: `set` prefix -- `setPlaying()`, `setScale()`
- Boolean queries: `is`/`has`/`can` prefix -- `isPlaying()`, `hasEditor()`, `canUndo()`

### Includes

Use `#pragma once` (no include guards). Order:

1. Corresponding header (in `.cpp` files)
2. JUCE module headers: `<juce_core/juce_core.h>`
3. Third-party headers: `<onnxruntime_cxx_api.h>`
4. Standard library headers: `<memory>`, `<vector>`, `<atomic>`
5. Project headers with relative paths: `"../Utils/PitchCurve.h"`

Use forward declarations to avoid unnecessary includes where possible.

### Formatting

- **Indentation**: 4 spaces (no tabs)
- **Braces**: Same-line (K&R style) for classes, functions, control flow
- **Namespace closing**: Annotate `} // namespace OpenTune`
- **Line length**: Aim for ~120 characters, no strict hard limit
- **Blank lines**: Single blank line between methods and between logical sections
- **Constructor init lists**: Colon on same line, each initializer on its own line with leading comma
- **Space after keywords**: `if (`, `for (`, `while (`; no space before function call parens

### Types and Const-Correctness

- **`const` everywhere**: On member functions, local variables, references, range-for loops
- **`static_cast`** for all conversions -- never use C-style casts
- **`auto`**: Use sparingly -- for iterators, complex return types, lambdas, range-for.
  Spell out primitive types (`int`, `float`, `double`, `bool`) explicitly
- **Smart pointers**: `std::unique_ptr` for exclusive ownership, `std::shared_ptr` for
  shared ownership, `std::weak_ptr` for non-owning references. Use `std::make_unique` /
  `std::make_shared`
- **Raw pointers**: Only for non-owning, nullable references (JUCE parameter pointers,
  temporary component references). Avoid `new` except where JUCE's API requires it
- **JUCE types** for GUI/audio: `juce::String`, `juce::AudioBuffer<float>`,
  `juce::Component`, `juce::Colour`
- **STL types** for data/logic: `std::vector`, `std::map`, `std::atomic`, `std::mutex`,
  `std::function`
- **In-class member initialization** is the norm: `float rate_ = 44100.0f;`
- Mark single-argument constructors `explicit`

### Error Handling

- **No exceptions thrown** by project code. Catch exceptions only at external API boundaries
  (ONNX Runtime calls) using `try { ... } catch (const std::exception& e) { ... } catch (...) { ... }`
- **Result<T>** type for structured error returns (Rust-style `ErrorCode` + message)
- **Bool returns** for success/failure: `bool initialize(...)`, `bool configure(...)`
- **Enum results** for richer status: `SubmitResult::Accepted`, `SubmitResult::QueueFull`
- **Structured diagnostics**: `PreflightResult` with `success`, `errorMessage`, `errorCategory`
- **Early returns** with safe defaults on invalid input
- **Logging** via `AppLogger::log(...)`, `AppLogger::debug(...)`, `AppLogger::error(...)`
- **JUCE helpers**: `juce::ignoreUnused()` for unused params, `juce::jlimit()` for clamping

### Class Organization

1. `public:` section first (constructor, destructor, JUCE overrides, public API)
2. `private:` section (internal methods and member state)
3. End every significant class with `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClassName)`
4. Group methods by functional area, separated by banner comments:
   ```cpp
   // ============================================================================
   // Import API
   // ============================================================================
   ```
5. Define `Listener` inner classes for observer callbacks with `virtual ~Listener() = default`
6. Provide default (empty) implementations for optional listener callbacks

### Threading Patterns

- **Audio thread safety**: `juce::ReadWriteLock` for track data (read lock on audio thread,
  write lock on UI thread). `juce::ScopedNoDenormals` in `processBlock()`
- **Immutable snapshots (COW)**: `PitchCurve` publishes `shared_ptr<const PitchCurveSnapshot>`
  via `std::atomic_store` / `std::atomic_load` for lock-free audio thread reads
- **Atomics**: Use `std::atomic<T>` with explicit memory ordering (`relaxed`, `acquire`,
  `release`) where appropriate
- **Lock-free structures**: Custom `LockFreeQueue` with cache-line alignment (`alignas(64)`)
- **Playback snapshot**: `StandaloneArrangement` publishes immutable `PlaybackSnapshot` for
  zero-lock audio thread reads

### Comments

- **Doc comments**: `/** @brief ... @param ... @return ... */` (Doxygen style) on public APIs
- **Section banners**: `// ====...` separator lines for major sections
- **Bilingual**: Comments in both Chinese and English are acceptable and common in this
  codebase. Chinese is used for domain-specific explanations
- **Inline**: `//` for short clarifications

### Common Design Patterns

- **Listener / Observer**: Nested `Listener` class with virtual callbacks; `juce::ListenerList`
- **Factory**: `ModelFactory::createF0Extractor(...)`, `VocoderFactory::create(...)`
- **Domain Facade**: `VocoderDomain` composes `VocoderInferenceService` + `VocoderRenderScheduler`
- **Command (Undo/Redo)**: `UndoAction` base class with concrete actions `PianoRollEditAction`,
  `SplitPlacementAction`, `MovePlacementAction`, etc.; `UndoManager` with 500-level linear stack
- **RAII**: Lock guards, smart pointers, `PerfTimer`
- **Result type**: `Result<T>` for error propagation without exceptions
- **Move semantics**: Explicitly support where needed

### Compile Definitions to Know

- `JUCE_STRICT_REFCOUNTEDPOINTER=1` -- strict JUCE pointer safety
- `ORT_API_MANUAL_INIT` -- manual ONNX Runtime initialization
- `NOMINMAX` / `WIN32_LEAN_AND_MEAN` -- standard Windows defines
- `JucePlugin_Build_Standalone` -- conditional compilation for standalone vs plugin builds
- `JucePlugin_Build_VST3` -- conditional compilation for VST3 builds
- `JucePlugin_Enable_ARA` -- ARA extension support

## Audio Pipeline

End-to-end flow from user edit to audible output:

```
User Edit (UI) --> PitchCurve snapshot update --> Chunk Render Queue
--> Mel spectrogram + corrected F0 preparation --> PC-NSF-HiFiGAN vocoder (ONNX)
--> RenderCache --> processBlock reads cache --> CrossoverMixer --> Audio Output
```

### Import Phase

1. User imports audio file. `AsyncAudioLoader` loads and resamples to 44100 Hz via
   `ResamplingManager` (r8brain), builds `WaveformMipmap`, stores in `SourceStore`.
2. `F0ExtractionService` submits async extraction. Worker calls `F0InferenceService`
   which manages `RMVPEExtractor` lifecycle (auto-loads on demand, auto-releases after
   30s idle). Extractor resamples audio to 16 kHz, runs RMVPE (ONNX), returns F0 + energy
   arrays at 100 fps (hop=160 @ 16 kHz). Result committed on message thread into
   `MaterializationStore`'s `PitchCurve`.

### Edit Phase (Pure Algorithm -- No Models)

When user edits pitch (note drag, hand-draw, line-anchor, auto-tune):
1. `PianoRollCorrectionWorker` (background thread) picks up the request.
2. Computes corrected F0 array via `PitchCurve::applyCorrectionToRange` (see
   Pitch Correction Algorithm section below).
3. Atomically publishes a new immutable `PitchCurveSnapshot` (COW pattern).
4. Callback on message thread triggers render enqueue.

### Chunk Rendering Phase

Render enqueue:
1. Splits materialization by `silentGaps` (from `SilentGapDetector`) into natural chunks.
2. For each chunk overlapping the edited range: bumps `desiredRevision` in `RenderCache`,
   pushes render task (newest edits render first).

`chunkRenderWorkerThread_` (dedicated `std::thread`):
1. Extracts mono audio from source buffer (44100 Hz) for the chunk time range.
2. Calls `snap->renderF0Range` to get corrected F0 (merges original + corrections).
3. Interpolates F0 from 100 fps to mel frame rate (~86 fps at 44100/512 hop).
4. Fills F0 gaps via `fillF0GapsForVocoder` (internal interpolation + boundary lookahead).
5. Computes log-mel spectrogram: 128 mels, 2048 FFT, 512 hop, 44100 Hz, 40--16000 Hz.
6. Submits to `VocoderDomain`.

`VocoderRenderScheduler` (serial execution -- DML requires serial `Run()`):
1. Applies psychoacoustic calibration (`SimdPerceptualPitchEstimator::getPerceptualOffset`).
2. Calls `VocoderInferenceService` --> ONNX vocoder inference (CPU or DirectML).
3. Crops output to exact `targetSamples`, stores in `RenderCache` at 44100 Hz +
   resampled copy at device sample rate.

### Playback Phase

`processBlock` (`PluginProcessor.cpp`) runs on the **audio thread**:
1. `juce::ScopedNoDenormals` for CPU safety.
2. Reads playback position (lock-free atomic).
3. For Standalone: reads immutable `PlaybackSnapshot` from `StandaloneArrangement`.
   For VST3/ARA: reads ARA playback regions.
4. For each placement/clip: reads dry signal. If corrected F0 exists, attempts
   `RenderCache::readAtTimeForRate` with `ScopedTryLock` (non-blocking). On lock
   contention, falls back to dry signal.
5. `CrossoverMixer` blends rendered audio (LPF < 14kHz) with dry signal (HPF > 14kHz)
   for artifact-free mixing.
6. Mix: scaled by `clipGain * trackVolume * fadeGain`. Sum all tracks into output buffer.

**Key invariant**: the audio thread **never blocks**. `ScopedTryLock` on `RenderCache`
ensures zero priority inversion.

## Pitch Correction Algorithm

All pitch correction is **pure math / DSP**. No ML models are involved. Models are only
used at the endpoints: RMVPE for F0 extraction (input) and PC-NSF-HiFiGAN for synthesis
(output).

### NoteBased Correction (`PitchCurve::applyCorrectionToRange`)

Processes each F0 frame in the requested range through five stages:

**Stage 1 -- Slope Rotation Compensation**

Compensates for natural pitch drift (e.g. rising intonation at phrase end) so that the
corrected output sounds level.

1. Collect voiced F0 frames in the note range, convert to MIDI values.
2. Split into early and late segments, take **median** MIDI of each.
3. Compute slope: `(lateMidi - earlyMidi) / (lateTime - earlyTime)` (semitones/sec).
4. Convert to angle: `atan(slope / 7.0)` (7.0 semitones/sec normalizes to 45 deg).
5. Only apply if absolute angle is in [10 deg, 30 deg] (ignore trivial / extreme drift).
6. Apply as **2D coordinate rotation** around note time-center:
   ```
   x = time - timeCenterSeconds
   y = freqToMidi(f0) - anchorMidi
   yRotated = x * sin(rotationRad) + y * cos(rotationRad)
   baseF0 = midiToFreq(anchorMidi + yRotated)
   ```

**Stage 2 -- Pitch Shifting**

Translates the (rotation-compensated) original F0 to the target pitch region while
**preserving all original detail** (vibrato, slides, micro-variations):
```
offsetSemitones = freqToMidi(targetPitch) - freqToMidi(anchorPitch)
shiftRatio = 2^(offsetSemitones / 12)
shiftedF0 = baseF0 * shiftRatio
```
`shiftedF0` is the result at retuneSpeed = 0% (full detail, just shifted).

**Stage 3 -- Vibrato LFO Injection**

Standard sine LFO applied to the flat target pitch:
```
depthSemitones = (vibratoDepth / 100) * 1.0
lfoValue = depthSemitones * sin(2 * pi * vibratoRate * timeInNote)
targetF0 *= 2^(lfoValue / 12)
```
- `vibratoRate`: oscillation frequency in Hz (typical 5--7 Hz)
- `vibratoDepth`: amplitude as percentage (100 = 1 semitone peak)

**Stage 4 -- Retune Speed Blending** (`PitchUtils::mixRetune`)

Log-space linear interpolation between the detail-preserving shifted F0 and the flat
target F0:
```
logResult = log2(shiftedF0) + (log2(targetF0) - log2(shiftedF0)) * retuneSpeed
result = 2^logResult
```
- `retuneSpeed = 0.0`: output = shiftedF0 (full original detail, just transposed)
- `retuneSpeed = 0.5`: half detail, half flat
- `retuneSpeed = 1.0`: output = targetF0 (perfectly flat "auto-tune" sound)

Log2 space is used because human pitch perception is logarithmic (octave = 2x frequency).

**Stage 5 -- Transition Smoothing**

Each corrected segment gets 10-frame (~100 ms) transition ramps on both sides:
```
w = (i + 1) / (len + 1)   // weight 0-->1
logResult = log2(originalF0) + (log2(boundaryF0) - log2(originalF0)) * w
```
Prevents audible pitch discontinuities at segment boundaries.

### HandDraw Correction (Note-Bound)

HandDraw is a **precision drawing tool within notes**. User paints F0 values directly,
but the drawn data is **clipped to note boundaries** via `clipDrawDataToNotes()` in
`PianoRollToolHandler`. Data outside any note region is discarded. Each overlapping
note gets an independent `CorrectedSegment` with `Source::HandDraw`. No slope rotation
or shifting is applied. Transition ramps are still inserted at boundaries. After drawing,
the affected notes are auto-selected.

### LineAnchor Correction (Note-Bound)

LineAnchor is a **precision drawing tool within notes**. User places anchor points;
system interpolates F0 between them in log2 space. The interpolated F0 curve is then
**clipped to note boundaries** using the same `clipDrawDataToNotes()` function as
HandDraw. Data outside any note region is discarded. At render time (`renderF0Range`),
if `retuneSpeed >= 0`, the interpolated target is blended with original F0 via
`mixRetune`. Otherwise the interpolated values are used directly.

### Unified Selection Model

All selection state flows through `Note.selected`. There are no parallel selection
mechanisms. `InteractionState::SelectionState` only contains transient rubber-band drag
coordinates (`dragStart/EndTime`, `dragStart/EndMidi`, `isSelectingArea`) and derived
F0 frame range (`selectedF0StartFrame/EndFrame`). Deselection triggers: Escape key
(only when notes are selected), click on empty area, tool switch.

### AutoTune

`PianoRollCorrectionWorker` with `Kind::AutoTuneGenerate`:
1. `NoteGenerator::generate` segments the original F0 into notes:
   - Voiced frames (F0 > 0) accumulate into a note segment.
   - Segment splits when pitch deviation from running average exceeds
     `transitionThresholdCents` (default 80 cents).
   - Silent gaps <= `gapBridgeMs` (10 ms) are bridged (don't split the note).
   - Minimum note duration: `minDurationMs` (100 ms).
   - Representative pitch per note: `SimdPerceptualPitchEstimator::estimatePIP`
     (energy-weighted VNC average), fallback to median.
   - Pitch quantized to nearest semitone (or scale-snapped if `ScaleSnapConfig` set).
2. Generated notes are fed to `applyCorrectionToRange` (same 5-stage pipeline above).

## AI Models

### RMVPE -- F0 Extraction

- **Purpose**: Extract fundamental frequency (F0) from vocal audio.
- **Location**: `Source/Inference/RMVPEExtractor.h`
- **Model file**: `models/rmvpe.onnx` (Git LFS)
- **Format**: ONNX, run via ONNX Runtime (CPU only).
- **Input**: mono audio resampled to 16 kHz `[1, num_samples]` + threshold `[1]`.
- **Output**: F0 `[1, num_frames]` + UV (unvoiced) `[1, num_frames]`.
- **Frame rate**: 100 fps (hop = 160 samples at 16 kHz = 10 ms per frame).
- **Post-processing**: octave error correction, gap filling (up to 8 frames).
- **Preflight**: estimates 6x memory overhead, enforces 10-minute max duration, checks
  available system memory.
- **Lifecycle**: `F0InferenceService` loads model on first use, auto-releases after 30s idle.

### PC-NSF-HiFiGAN -- Neural Vocoder

- **Purpose**: Synthesize audio from mel spectrogram + corrected F0, preserving original
  timbre while changing pitch.
- **Location**: `Source/Inference/PCNSFHifiGANVocoder.h` (CPU), `Source/Inference/DmlVocoder.h` (GPU)
- **Model file**: `pc_nsf_hifigan_44.1k_ONNX/pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx` (~50 MB)
- **Input**: mel `[1,128,frames]` + f0 `[1,frames]`
- **Output**: audio at 44100 Hz, length = frames * 512 samples.
- **Backends**: CPU (OnnxVocoderBase) or DirectML GPU (DmlVocoder with IO binding).
  Backend selected by `VocoderFactory` based on `AccelerationDetector` results.
- **Managed by**: `VocoderDomain` facade → `VocoderInferenceService` (session) +
  `VocoderRenderScheduler` (serial queue).
- **DML constraint**: DirectML requires serial `Run()` calls -- `VocoderRenderScheduler`
  enforces single-threaded synthesis.

### Psychoacoustic Calibration

`SimdPerceptualPitchEstimator::getPerceptualOffset` (`Source/Utils/SimdPerceptualPitchEstimator.h`):
- Compensates for Fletcher-Munson equal-loudness contour effects.
- < 1000 Hz and loud (> -12 dB): +2 cents.
- \> 2500 Hz and loud (> -12 dB): -3 cents.
- Applied to F0 before vocoder inference in `VocoderRenderScheduler`.

### Perceptual Intentional Pitch (PIP)

`SimdPerceptualPitchEstimator::estimatePIP` (`Source/Utils/SimdPerceptualPitchEstimator.h`):
- Used by `NoteGenerator` to compute representative pitch per note.
- Vibrato-Neutral Center (VNC): 150 ms centered moving average of F0.
- Stable-State Analysis (SSA): Tukey window (15% cosine ramp edges).
- Final: `Sum(VNC * SSA * Energy) / Sum(SSA * Energy)`.

## Core Data Model

### Three-Store Architecture

The data model separates concerns into three stores:

- **`SourceStore`**: Original audio sources. Immutable after import. Holds audio buffers,
  sample rate, channel count. Identity-based (sourceId). No editing state.
- **`MaterializationStore`**: Editable processing state per source region. Holds notes,
  corrected F0 segments, detected key/scale, silent gaps, render cache. One materialization
  per source window. All pitch editing happens here.
- **`StandaloneArrangement`** (Standalone only): Multi-track timeline with 12 tracks.
  Placements reference materializations at timeline positions. Publishes immutable
  `PlaybackSnapshot` for lock-free audio thread reads.

For VST3/ARA mode, `VST3AraSession` maps ARA regions/sources to the same SourceStore
and MaterializationStore, replacing StandaloneArrangement's role.

### PitchCurve / PitchCurveSnapshot (COW)

`Source/Utils/PitchCurve.h` -- central hub connecting editing to rendering.

- `PitchCurve` holds `shared_ptr<const PitchCurveSnapshot>`, published via
  `std::atomic_store`. Every mutation creates a new snapshot (Copy-on-Write).
- Audio thread reads via `std::atomic_load` -- **lock-free**.
- `PitchCurveSnapshot` contains:
  - `originalF0_` -- RMVPE-extracted F0 (Hz per frame, 100 fps).
  - `originalEnergy_` -- per-frame RMS energy aligned with F0.
  - `correctedSegments_` -- sorted vector of `CorrectedSegment`.
  - `renderGeneration_` -- monotonic counter, incremented on each correction change,
    drives cache invalidation.
- `renderF0Range(start, end, callback)` -- iterates frame range, emits corrected F0
  where segments exist, falls back to original F0 in gaps.

### CorrectedSegment

`Source/Utils/PitchCurve.h`

- `startFrame`, `endFrame` -- frame range.
- `f0Data` -- corrected F0 values (Hz).
- `source` -- `NoteBased`, `HandDraw`, or `LineAnchor`.
- `retuneSpeed`, `vibratoDepth`, `vibratoRate` -- per-segment parameters.

### Note / NoteSequence

`Source/Utils/Note.h`

- `Note`: `startTime`/`endTime` (seconds), `pitch` (Hz), `originalPitch` (Hz, unquantized
  from RMVPE), `pitchOffset` (semitones), `retuneSpeed`, `vibratoDepth`/`vibratoRate`.
- `getAdjustedPitch()`: `pitch * 2^(pitchOffset / 12)`.
- `NoteSequence`: sorted, non-overlapping note container with insert/erase/range-replace.

### RenderCache

`Source/Inference/RenderCache.h`

- `std::map<double, Chunk>` keyed by `startSeconds`.
- Each `Chunk`: synthesized audio at 44100 Hz + `unordered_map<int, vector<float>>`
  of resampled versions keyed by target sample rate.
- Revision protocol: `desiredRevision` (bumped by edits) vs `publishedRevision` (set by
  render completion). Read succeeds only when `desired == published`.
- Memory limit: 256 MB cap, LRU eviction.
- Thread safety: `juce::SpinLock` -- audio thread uses `ScopedTryLock` (non-blocking).

## Threading Model

| Thread | Count | Role | Key Data Access |
|--------|-------|------|-----------------|
| Audio thread (`processBlock`) | 1 | Real-time mixing | Reads `PlaybackSnapshot` (immutable), `RenderCache` (non-blocking try-lock). |
| UI / Message thread | 1 | User interaction, state mutation | Writes `MaterializationStore`, `SourceStore`, `StandaloneArrangement`. |
| `chunkRenderWorkerThread_` | 1 | Mel + F0 preparation for vocoder | Reads `PitchCurveSnapshot` (immutable COW). Submits to `VocoderDomain`. |
| `VocoderRenderScheduler` | 1 | Serial ONNX vocoder inference (DML requires serial Run()) | Reads chunk inputs. Writes to `RenderCache`. |
| `F0InferenceService` | concurrent | RMVPE inference (shared_mutex, CPU-only) | Reads source audio (copy). Results committed on message thread. |
| `PianoRollCorrectionWorker` | 1 | Pitch correction computation | Reads/writes `PitchCurve` snapshots. Results applied on message thread. |

### Synchronization Primitives

- `PitchCurve` snapshots: `std::atomic_store`/`load` for lock-free COW.
- `PlaybackSnapshot`: immutable snapshot published by `StandaloneArrangement` for audio thread.
- `RenderCache::lock_` (`juce::SpinLock`): audio thread uses `ScopedTryLock`.
- `schedulerMutex_` + `schedulerCv_`: chunk render task queue coordination.
- `pendingRequestMutex_` on `PianoRollCorrectionWorker`: guards request handoff.
- `std::shared_mutex` on F0 extractor in `F0InferenceService`.
