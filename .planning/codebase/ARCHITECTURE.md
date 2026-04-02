# Architecture

**Generated:** 2026-04-02
**Project:** OpenTune

## Pattern Overview

**Overall:** Layered Audio Processing Application with Plugin Architecture

**Key Characteristics:**
- JUCE-based audio plugin (supports both standalone and DAW plugin modes)
- Real-time audio processing with AI inference pipeline
- Multi-track architecture with thread-safe state management
- Producer-consumer pattern for rendering jobs
- Immutable snapshot pattern for UI state

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        OpenTuneAudioProcessor                           │
│                    (JUCE AudioProcessor, Central Hub)                   │
├─────────────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐ │
│  │ TrackState[] │  │ RenderCache  │  │ PitchCurve   │  │ UndoManager │ │
│  │ (12 tracks)  │  │  (per clip)  │  │  (per clip)  │  │   (global)  │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └─────────────┘ │
├─────────────────────────────────────────────────────────────────────────┤
│                         Threading Layers                                │
│  ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────────────┐│
│  │  Audio Thread   │  │ Render Worker    │  │  Inference Threads      ││
│  │  (processBlock) │  │ (chunkRenderLoop)│  │  (detached, max 4)      ││
│  │  ReadLock only  │  │ WriteLock safe   │  │  Async callbacks        ││
│  └─────────────────┘  └──────────────────┘  └─────────────────────────┘│
├─────────────────────────────────────────────────────────────────────────┤
│                          Inference Layer                                │
│  ┌──────────────────┐  ┌───────────────────┐  ┌─────────────────────┐ │
│  │F0InferenceService│  │ RMVPEExtractor    │  │ PCNSFHifiGANVocoder │ │
│  │   (CPU-only)     │  │ F0 @ 16kHz        │  │  Synthesis @ 44.1kHz│ │
│  └──────────────────┘  └───────────────────┘  └─────────────────────┘ │
│  ┌──────────────────┐  ┌───────────────────────────────────────────┐  │
│  │  VocoderDomain   │  │         ModelFactory (ONNX Sessions)       │  │
│  │  (Lifecycle)     │  └───────────────────────────────────────────┘  │
│  └──────────────────┘                                                  │
├─────────────────────────────────────────────────────────────────────────┤
│                            DSP Layer                                    │
│  ┌──────────────────┐  ┌───────────────────┐  ┌─────────────────────┐ │
│  │ResamplingManager │  │ MelSpectrogram    │  │ ScaleInference      │ │
│  │ r8brain-based    │  │ 128-bin mel bands │  │ Key detection       │ │
│  └──────────────────┘  └───────────────────┘  └─────────────────────┘ │
├─────────────────────────────────────────────────────────────────────────┤
│                             UI Layer                                    │
│  ┌──────────────────┐  ┌───────────────────┐  ┌─────────────────────┐ │
│  │ PianoRollComponent│  │ArrangementView   │  │ TrackPanelComponent │ │
│  │ F0 editing/notes  │  │ Clip arrangement │  │ Track controls      │ │
│  └──────────────────┘  └───────────────────┘  └─────────────────────┘ │
│  ┌──────────────────┐  ┌───────────────────┐  ┌─────────────────────┐ │
│  │ ParameterPanel   │  │ TransportBar      │  │ MenuBar             │ │
│  │ Retune/vibrato   │  │ Play/pause/stop   │  │ File operations     │ │
│  └──────────────────┘  └───────────────────┘  └─────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

## Layers

### Audio Processing Layer
- Purpose: Real-time audio I/O, mixing, and output generation
- Location: `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`, `Source/Host/`
- Contains: OpenTuneAudioProcessor, HostIntegration interface and implementations
- Depends on: DSP layer, Inference layer, Utils
- Used by: JUCE framework (AudioProcessor interface)

### Inference Layer
- Purpose: AI model inference for F0 extraction and vocoder synthesis
- Location: `Source/Inference/`
- Contains: F0InferenceService, VocoderDomain, RenderCache, RMVPEExtractor, PCNSFHifiGANVocoder, ModelFactory, VocoderFactory
- Depends on: ONNX Runtime, DSP layer
- Used by: Audio Processing Layer, Services Layer

### DSP Layer
- Purpose: Audio signal processing utilities
- Location: `Source/DSP/`
- Contains: ResamplingManager, MelSpectrogramProcessor, ScaleInference
- Depends on: JUCE DSP module, third-party FFT (r8brain)
- Used by: Audio Processing Layer, Inference Layer

### UI Layer
- Purpose: User interface components and interaction handling
- Location: `Source/Standalone/`, `Source/Editor/`
- Contains: PianoRollComponent, ArrangementViewComponent, ParameterPanel, TrackPanelComponent, TransportBarComponent, TopBarComponent, MenuBarComponent, Themes
- Depends on: JUCE GUI modules, PluginProcessor
- Used by: PluginEditor

### Services Layer
- Purpose: Background task orchestration
- Location: `Source/Services/`
- Contains: F0ExtractionService (thread pool for async F0 extraction)
- Depends on: Inference layer, Utils (LockFreeQueue)
- Used by: PluginEditor for async operations

### Utils Layer
- Purpose: Shared data structures, helpers, and infrastructure
- Location: `Source/Utils/`
- Contains: PitchCurve, Note, UndoAction, UndoManager, TimeCoordinate, SilentGapDetector, ClipSnapshot, PresetManager, Error types, GPU detection
- Depends on: JUCE Core, standard library
- Used by: All layers

## Data Flow

### Audio Playback Flow (Audio Thread)

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Audio Thread (processBlock)                 │
├─────────────────────────────────────────────────────────────────────┤
│  1. Read tracksLock_ (ScopedReadLock)                               │
│  2. For each active track:                                          │
│     ├─ Check mute/solo state                                        │
│     ├─ For each clip in time range:                                 │
│     │   ├─ Read from RenderCache (resampled audio)                  │
│     │   └─ OR fallback to drySignalBuffer_                          │
│     └─ Mix to output buffer                                         │
│  3. Update RMS meters                                               │
│  4. Handle fade-out on stop                                         │
└─────────────────────────────────────────────────────────────────────┘
```

### Audio Import Flow (Two-Phase Pattern)

```
┌──────────────────────────────────────────────────────────────────────┐
│  Two-Phase Import (Thread-Safe)                                      │
├──────────────────────────────────────────────────────────────────────┤
│  Phase 1: Background Thread (no lock)                               │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  AsyncAudioLoader.loadAudioFile()                           │    │
│  │         │                                                    │    │
│  │         ▼                                                    │    │
│  │  prepareImportClip()                                         │    │
│  │    ├─ Resample to 44.1kHz (kRenderSampleRate)               │    │
│  │    ├─ Detect silent gaps (SilentGapDetector)                │    │
│  │    └─ Prepare PreparedImportClip struct                     │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                         │                                            │
│                         ▼                                            │
│  Phase 2: Main Thread (write lock)                                  │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  commitPreparedImportClip()                                  │    │
│  │    ├─ Acquire tracksLock_ (ScopedWriteLock)                 │    │
│  │    ├─ Create AudioClip                                      │    │
│  │    ├─ Store shared_ptr<AudioBuffer>                         │    │
│  │    └─ Create RenderCache                                    │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                         │                                            │
│                         ▼                                            │
│  Post-Process (Background)                                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  F0ExtractionService.extractF0()                             │    │
│  │         │                                                    │    │
│  │         ▼                                                    │    │
│  │  PitchCurve.setOriginalF0()                                 │    │
│  │         │                                                    │    │
│  │         ▼                                                    │    │
│  │  ScaleInference.detectKey()                                 │    │
│  └─────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────┘
```

### Inference Request Flow

```
┌──────────────────────────────────────────────────────────────────────┐
│  UI Thread                    │  Background Worker Thread           │
├──────────────────────────────────────────────────────────────────────┤
│  User edits PitchCurve        │                                     │
│         │                     │                                     │
│         ▼                     │                                     │
│  PitchCurve.applyCorrection() │                                     │
│         │                     │                                     │
│         ▼                     │                                     │
│  RenderCache.requestRender()  │                                     │
│         │                     │                                     │
│         │                     │  ◄──── schedulerCv_.notify()        │
│         │                     │                                     │
│         │                     │  getNextPendingJob() → Job          │
│         │                     │         │                           │
│         │                     │         ▼                           │
│         │                     │  VocoderDomain.submit()             │
│         │                     │         │                           │
│         │                     │         ▼                           │
│         │                     │  PCNSFHifiGANVocoder.synthesize()   │
│         │                     │         │                           │
│         │                     │         ▼                           │
│         │                     │  RenderCache.addChunk()             │
│         │                     │         │                           │
│         │  ◄──────────────────┼─────────┘                           │
│         │  onComplete callback│                                     │
│         ▼                     │                                     │
│  UI refresh (repaint)         │                                     │
└──────────────────────────────────────────────────────────────────────┘
```

## Key Abstractions

### PitchCurve (Immutable Snapshot Pattern)
- Purpose: Thread-safe pitch curve data with correction segments
- Location: `Source/Utils/PitchCurve.h:111-381`
- Pattern: Copy-on-write with atomic shared_ptr swap
```cpp
// Immutable snapshot, safe to read from any thread
std::shared_ptr<const PitchCurveSnapshot> getSnapshot() const {
    return std::atomic_load(&snapshot_);
}
```

### RenderCache (Chunk-based Audio Cache)
- Purpose: Cache synthesized audio chunks with revision tracking
- Location: `Source/Inference/RenderCache.h:17-131`
- Pattern: State machine per chunk
```cpp
enum class Status : uint8_t {
    Idle,    // No pending render
    Pending, // Waiting for worker pickup
    Running, // Rendering in progress
    Blank    // No valid F0, skip render
};
```

### VocoderInterface (Strategy Pattern)
- Purpose: Abstract vocoder backend (CPU/DML)
- Location: `Source/Inference/VocoderInterface.h:17-36`
- Pattern: Interface with multiple implementations selected at runtime

### IF0Extractor (Strategy Pattern)
- Purpose: Abstract F0 extraction models
- Location: `Source/Inference/IF0Extractor.h:31-101`
- Pattern: Interface allowing model switching (RMVPE implementation)

### HostIntegration (Strategy Pattern)
- Purpose: Abstract plugin vs standalone mode differences
- Location: `Source/Host/HostIntegration.h:21-38`
- Pattern: Factory creates mode-specific implementation

### TimeCoordinate
- Purpose: Single source of truth for time/space conversions
- Location: `Source/Utils/TimeCoordinate.h`
- Key constant: `kRenderSampleRate = 44100.0`

## Entry Points

### Plugin Entry (JUCE Plugin Format)
- Location: `Source/PluginProcessor.cpp`
- Triggers: DAW loads plugin or standalone app launches
- Responsibilities: Create processor, initialize resources, manage lifecycle

### Editor Entry
- Location: `Source/Standalone/PluginEditor.h:38-241`
- Triggers: Processor creates editor via `createEditor()`
- Responsibilities: Build UI component hierarchy, connect listeners to processor

### Audio Callback Entry
- Location: `Source/PluginProcessor.cpp:processBlock()`
- Triggers: Audio hardware requests buffer (every ~10ms)
- Responsibilities: Read track state, fetch cached audio, mix and output

### Render Worker Entry
- Location: `Source/PluginProcessor.cpp:chunkRenderWorkerLoop()`
- Triggers: Background thread started in constructor
- Responsibilities: Process pending render jobs, invoke vocoder synthesis

## Threading Model

### Thread Types
| Thread | Role | Locking |
|--------|------|---------|
| Audio Thread | Real-time playback, `processBlock()` | `ScopedReadLock(tracksLock_)` |
| Render Worker | Background chunk rendering | `ScopedReadLock` for data reads |
| Inference Threads | ONNX inference (detached) | Atomic counters, async callbacks |
| UI Thread | JUCE MessageManager | Write operations need `ScopedWriteLock` |
| Async Loader | File I/O | None (copies data before commit) |

### Synchronization Primitives
- `juce::ReadWriteLock tracksLock_`: Protects `tracks_` array (`Source/PluginProcessor.h:252`)
- `juce::SpinLock`: Used in `RenderCache` for fast chunk access
- `std::mutex schedulerMutex_`: Protects render scheduler state
- `std::condition_variable schedulerCv_`: Wake render worker
- `std::atomic<T>`: For flags, counters, playhead position, and state shared across threads

### Critical Sections
- **Audio → UI data access:** Always under `tracksLock_` (read for audio, write for UI)
- **Render job dispatch:** Under `schedulerMutex_` + `tracksLock_`
- **PitchCurve updates:** Atomic snapshot swap (lock-free reads)

## Error Handling

**Strategy:** `Result<T>` type for recoverable errors, silent fallback for audio

**Patterns:**
- `Result<T>` in `Source/Utils/Error.h` for fallible inference operations
- Silent fallback to dry signal when render not available
- Progressive degradation (cache miss → dry signal → silence)
- VocoderCreationResult struct with success/failure states (`Source/Inference/VocoderFactory.h:17-31`)

## Cross-Cutting Concerns

### Memory Management
- RAII throughout: `unique_ptr` for owned resources, `shared_ptr` for shared state
- Global cache limit: 1.5GB (`RenderCache::kDefaultGlobalCacheLimitBytes`)
- Immutable snapshots: Avoid copies, use shared ownership
- Max tracks: 12 (`PluginProcessor::MAX_TRACKS`)

### Logging
- Framework: Custom `AppLogger` (`Source/Utils/AppLogger.h`)
- Output: Debug console, file (configurable)

### Undo/Redo
- Framework: Custom `UndoManager` (`Source/Utils/UndoAction.h`)
- Scope: Global per-processor, 500-action history
- Integration: UI components push actions via `UndoAction` objects

---

*Architecture analysis: 2026-04-02*
