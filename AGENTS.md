# AGENTS.md - OpenTune

## Project Overview

OpenTune is an AI-powered pitch correction standalone application built with the JUCE
framework (C++17). It uses NSF-HiFiGAN neural vocoder and RMVPE pitch extraction via
ONNX Runtime for formant-preserving vocal pitch shifting. Company: DAYA. License: AGPL-3.0.

## Build Commands

### Requirements

- CMake 3.22+
- C++17 compiler (MSVC 2022 / Visual Studio 17 on Windows)
- All dependencies are vendored (JUCE, ONNX Runtime 1.17.3, r8brain-free-src)
- `.onnx` model files are tracked via Git LFS -- run `git lfs pull` after cloning

### Configure and Build (Windows)

```bash
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Output: `build/OpenTune_artefacts/Release/Standalone/OpenTune.exe`

Post-build automatically copies ONNX Runtime DLLs and model files to the output directory.

### macOS / Linux

Not officially supported yet. The CMakeLists.txt targets Windows/MSVC. A macOS or Linux
build would require providing a platform-appropriate ONNX Runtime library and adjusting
the linker configuration (currently hardcoded to `.lib` / `.dll`).

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
  PluginProcessor.cpp/h      # Core audio processor (multi-track, playback, mixing)
  Inference/                  # AI model layer (ONNX Runtime wrappers)
    InferenceManager.h        # Singleton, Pimpl pattern
    RMVPEExtractor.h          # F0 pitch extraction
    HifiGANVocoder.h          # Neural vocoder
    ModelFactory.h             # Static factory for model creation
    RenderingManager.h        # Chunk-based render pipeline
    RenderCache.h             # Cached render results
  DSP/                        # Signal processing
    MelSpectrogram.h          # Mel spectrogram computation
    ResamplingManager.h       # Audio resampling (r8brain)
    ScaleInference.h          # Musical scale/key detection
  Services/                   # Background services
    F0ExtractionService.h     # Async F0 extraction with worker threads
  Standalone/                 # Standalone app UI
    PluginEditor.h            # Main editor window (central mediator)
    UI/                       # All UI components
      PianoRollComponent.h    # Piano roll editor
      MainControlPanel.h      # Control panel
      ThemeTokens.h           # Theme system
  Utils/                      # Utilities
    PitchCurve.h              # Immutable-snapshot pitch curve (COW)
    Note.h                    # Note data structures
    AppLogger.h               # Static logging
    UndoAction.h              # Command pattern undo/redo
    LockFreeQueue.h           # Lock-free concurrent containers
  Host/                       # Host integration abstractions
  Audio/                      # Audio subsystem
  Editor/                     # Editor factory
ThirdParty/
  r8brain-free-src-master/    # Vendored resampling library
JUCE-master/                  # Vendored JUCE framework
onnxruntime-win-x64-1.17.3/  # Vendored ONNX Runtime (Windows x64)
models/                       # ONNX model files (Git LFS)
Resources/                    # App icon, fonts
```

## Code Style Guidelines

### Naming Conventions

| Element              | Convention                          | Example                              |
|----------------------|-------------------------------------|--------------------------------------|
| Classes / Structs    | PascalCase                          | `InferenceManager`, `TrackState`     |
| Interfaces (ABC)     | `I` prefix + PascalCase             | `IF0Extractor`, `IVocoder`           |
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
- **Lock-free structures**: Custom `LockFreeQueue`, `SPSCRingBuffer` with cache-line
  alignment (`alignas(64)`)
- **Two-phase import**: Heavy work in worker thread (`prepareImportClip`), lightweight
  commit under write lock on main thread (`commitPreparedImportClip`)

### Comments

- **Doc comments**: `/** @brief ... @param ... @return ... */` (Doxygen style) on public APIs
- **Section banners**: `// ====...` separator lines for major sections
- **Bilingual**: Comments in both Chinese and English are acceptable and common in this
  codebase. Chinese is used for domain-specific explanations
- **Inline**: `//` for short clarifications

### Common Design Patterns

- **Listener / Observer**: Nested `Listener` class with virtual callbacks; `juce::ListenerList`
- **Singleton**: `InferenceManager::getInstance()` with private constructor
- **Pimpl**: `InferenceManager` hides implementation behind `std::unique_ptr<Impl>`
- **Factory**: `ModelFactory::createF0Extractor(...)`, `ModelFactory::createVocoder(...)`
- **Command (Undo/Redo)**: `UndoAction` base class with concrete actions like `NotesChangeAction`,
  `ClipMoveAction`; `CompoundUndoAction` for atomic multi-step ops
- **RAII**: Lock guards, smart pointers, `OriginalF0ExtractionGuard`, `PerfTimer`
- **Move semantics**: Explicitly support where needed (`PreparedImportClip&&`)

### Compile Definitions to Know

- `JUCE_STRICT_REFCOUNTEDPOINTER=1` -- strict JUCE pointer safety
- `ORT_API_MANUAL_INIT` -- manual ONNX Runtime initialization
- `NOMINMAX` / `WIN32_LEAN_AND_MEAN` -- standard Windows defines
- `JucePlugin_Build_Standalone` -- conditional compilation for standalone vs plugin builds

## Audio Pipeline

End-to-end flow from user edit to audible output:

```
User Edit (UI) --> PitchCurve snapshot update --> Chunk Render Queue
--> Mel spectrogram + corrected F0 preparation --> NSF-HiFiGAN vocoder (ONNX)
--> RenderCache --> processBlock reads cache --> Audio Output
```

### Import Phase

1. User imports audio file. `PluginProcessor` resamples to 44100 Hz via `ResamplingManager`
   (r8brain), stores in `clip.audioBuffer`.
2. `F0ExtractionService` (2 worker threads) submits async extraction. Worker resamples
   audio to 16 kHz, runs `RMVPEExtractor` (ONNX), returns F0 + energy arrays at 100 fps
   (hop=160 @ 16 kHz). Result committed on message thread into `PitchCurve`.

### Edit Phase (Pure Algorithm -- No Models)

When user edits pitch (note drag, hand-draw, line-anchor, auto-tune):
1. `PianoRollCorrectionWorker` (background thread) picks up the request.
2. Computes corrected F0 array via `PitchCurve::applyCorrectionToRange` (see
   Pitch Correction Algorithm section below).
3. Atomically publishes a new immutable `PitchCurveSnapshot` (COW pattern).
4. Callback on message thread fires `PluginEditor::pitchCurveEdited` -->
   `PluginProcessor::enqueuePartialRender`.

### Chunk Rendering Phase

`enqueuePartialRender` (`PluginProcessor.cpp`):
1. Splits clip by `silentGaps` into natural chunks (boundaries at gap midpoints).
2. For each chunk overlapping the edited range: bumps `desiredRevision` in `RenderCache`,
   pushes `ChunkRenderTask` to front of deque (newest edits render first).

`chunkRenderWorkerThread_` (dedicated `std::thread`):
1. Extracts mono audio from `clip.audioBuffer` (44100 Hz) for the chunk time range.
2. Calls `snap->renderF0Range` to get corrected F0 (merges original + corrections).
3. Interpolates F0 from 100 fps to mel frame rate (~86 fps at 44100/512 hop).
4. Fills F0 gaps via `fillF0GapsForVocoder` (internal interpolation + boundary lookahead).
5. Computes log-mel spectrogram: 128 mels, 2048 FFT, 512 hop, 44100 Hz, 40--16000 Hz.
6. Packages `ChunkInputs{mel, f0, energy, targetSamples}` and submits to `RenderingManager`.

`RenderingManager` worker pool (hardware_concurrency / 2 threads):
1. Applies psychoacoustic calibration (`SimdPerceptualPitchEstimator::getPerceptualOffset`).
2. Calls `InferenceManager::synthesizeAudioWithEnergy` --> ONNX vocoder inference.
3. Crops output to exact `targetSamples`, stores in `RenderCache` at 44100 Hz +
   resampled copy at device sample rate.

### Playback Phase

`processBlock` (`PluginProcessor.cpp`) runs on the **audio thread**:
1. `juce::ScopedNoDenormals` for CPU safety.
2. Reads playback position from `positionAtomic_` (lock-free atomic).
3. Acquires `ScopedReadLock` on `tracksLock_`.
4. For each track/clip: reads dry signal from `clip.drySignalBuffer_` (pre-resampled
   to device rate). If corrected F0 exists, attempts `RenderCache::readAtTimeForRate`
   with `ScopedTryLock` (non-blocking). On lock contention, falls back to dry signal.
5. Mix: rendered audio (if available) or dry signal, scaled by `clipGain * trackVolume *
   fadeGain`. Sum all tracks into output buffer.

**Key invariant**: the audio thread **never blocks**. `ScopedTryLock` on `RenderCache`
ensures zero priority inversion.

## Pitch Correction Algorithm

All pitch correction is **pure math / DSP**. No ML models are involved. Models are only
used at the endpoints: RMVPE for F0 extraction (input) and NSF-HiFiGAN for synthesis
(output).

### NoteBased Correction (`PitchCurve::applyCorrectionToRange`)

Core entry point at `Source/Utils/PitchCurve.cpp:439`. Processes each F0 frame in the
requested range through four stages:

**Stage 1 -- Slope Rotation Compensation** (`PitchCurve.cpp:510-546`)

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

**Stage 2 -- Pitch Shifting** (`PitchCurve.cpp:608-611`)

Translates the (rotation-compensated) original F0 to the target pitch region while
**preserving all original detail** (vibrato, slides, micro-variations):
```
offsetSemitones = freqToMidi(targetPitch) - freqToMidi(anchorPitch)
shiftRatio = 2^(offsetSemitones / 12)
shiftedF0 = baseF0 * shiftRatio
```
`shiftedF0` is the result at retuneSpeed = 0% (full detail, just shifted).

**Stage 3 -- Vibrato LFO Injection** (`PitchCurve.cpp:590-595`)

Standard sine LFO applied to the flat target pitch:
```
depthSemitones = (vibratoDepth / 100) * 1.0
lfoValue = depthSemitones * sin(2 * pi * vibratoRate * timeInNote)
targetF0 *= 2^(lfoValue / 12)
```
- `vibratoRate`: oscillation frequency in Hz (typical 5--7 Hz)
- `vibratoDepth`: amplitude as percentage (100 = 1 semitone peak)

**Stage 4 -- Retune Speed Blending** (`PitchUtils::mixRetune`, `PitchUtils.h:16`)

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

**Stage 5 -- Transition Smoothing** (`PitchCurve.cpp:76-196`)

Each corrected segment gets 10-frame (~100 ms) transition ramps on both sides:
```
w = (i + 1) / (len + 1)   // weight 0-->1
logResult = log2(originalF0) + (log2(boundaryF0) - log2(originalF0)) * w
```
Prevents audible pitch discontinuities at segment boundaries.

### HandDraw Correction (`PitchCurve::setManualCorrectionRange`)

User paints F0 values directly. The painted f0Data array is stored verbatim as a
`CorrectedSegment` with `Source::HandDraw`. No slope rotation or shifting is applied.
Transition ramps are still inserted at boundaries.

### LineAnchor Correction

User places anchor points; system interpolates F0 between them. At render time
(`renderF0Range`), if `retuneSpeed >= 0`, the interpolated target is blended with
original F0 via `mixRetune`. Otherwise the interpolated values are used directly.

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
- **Format**: ONNX, run via ONNX Runtime 1.17.3.
- **Input**: mono audio resampled to 16 kHz `[1, num_samples]` + threshold `[1]`.
- **Output**: F0 `[1, num_frames]` + UV (unvoiced) `[1, num_frames]`.
- **Frame rate**: 100 fps (hop = 160 samples at 16 kHz = 10 ms per frame).
- **Post-processing**: octave error correction, gap filling (up to 8 frames).
- **Preflight**: estimates 6x memory overhead, enforces 10-minute max duration, checks
  available system memory.
- **When called**: once per clip import, via `F0ExtractionService` (2 worker threads).

### NSF-HiFiGAN -- Neural Vocoder

- **Purpose**: Synthesize audio from mel spectrogram + corrected F0, preserving original
  timbre while changing pitch.
- **Variants** (tried in order):
  1. `PCNSFHifiGANVocoder` (~50 MB) -- Pitch-Controllable NSF-HiFiGAN. Preferred.
     Input: mel `[1,128,frames]` + f0 `[1,frames]` + UV tensor.
  2. `HifiGANVocoder` (~14 MB) -- standard HiFiGAN. Fallback.
     Input: mel `[1,128,frames]` + f0 `[1,frames]`.
- **Output**: audio at 44100 Hz, length = frames * 512 samples.
- **Managed by**: `InferenceManager` (singleton, Pimpl), created via `ModelFactory`.
- **When called**: during chunk rendering in `RenderingManager` worker threads.

### Psychoacoustic Calibration

`SimdPerceptualPitchEstimator::getPerceptualOffset` (`Source/Utils/SimdPerceptualPitchEstimator.h:92`):
- Compensates for Fletcher-Munson equal-loudness contour effects.
- < 1000 Hz and loud (> -12 dB): +2 cents.
- \> 2500 Hz and loud (> -12 dB): -3 cents.
- Applied to F0 before vocoder inference in `RenderingManager`.

### Perceptual Intentional Pitch (PIP)

`SimdPerceptualPitchEstimator::estimatePIP` (`Source/Utils/SimdPerceptualPitchEstimator.h:22`):
- Used by `NoteGenerator` to compute representative pitch per note.
- Vibrato-Neutral Center (VNC): 150 ms centered moving average of F0.
- Stable-State Analysis (SSA): Tukey window (15% cosine ramp edges).
- Final: `Sum(VNC * SSA * Energy) / Sum(SSA * Energy)`.

## Key Data Structures

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

`Source/Utils/PitchCurve.h:15`

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
- Memory limit: 1.5 GB global cap, LRU eviction.
- Thread safety: `juce::SpinLock` -- audio thread uses `ScopedTryLock` (non-blocking).

## Threading Model

| Thread | Count | Role | Key Data Access |
|--------|-------|------|-----------------|
| Audio thread (`processBlock`) | 1 | Real-time mixing | Reads `drySignalBuffer_`, `RenderCache` (non-blocking try-lock), `positionAtomic_`. Holds `ScopedReadLock` on `tracksLock_`. |
| UI / Message thread | 1 | User interaction, state mutation | Writes `PitchCurve`, `NoteSequence`. Calls `enqueuePartialRender`. Holds `ScopedWriteLock` on `tracksLock_` for clip mutations. |
| `chunkRenderWorkerThread_` | 1 | Mel + F0 preparation for vocoder | Reads `tracksLock_` (read lock), `PitchCurveSnapshot` (immutable COW). Writes to chunk task queue. |
| `RenderingManager` workers | N/2 | ONNX vocoder inference | Reads `ChunkInputs`. Writes to `RenderCache`. |
| `F0ExtractionService` workers | 2 | RMVPE inference | Reads clip audio (copy). Results committed on message thread. |
| `PianoRollCorrectionWorker` | 1 | Pitch correction computation | Reads/writes `PitchCurve` snapshots. Results applied on message thread. |

### Synchronization Primitives

- `tracksLock_` (`juce::ReadWriteLock`): protects track/clip data. Audio thread holds
  read lock; UI thread holds write lock for mutations.
- `PitchCurve` snapshots: `std::atomic_store`/`load` for lock-free COW.
- `RenderCache::lock_` (`juce::SpinLock`): audio thread uses `ScopedTryLock`.
- `chunkQueueMutex_` + `chunkQueueCv_`: chunk render task queue coordination.
- `pendingRequestMutex_` on `PianoRollCorrectionWorker`: guards request handoff.
- `std::shared_mutex` on F0 extractor in `InferenceManager`.
