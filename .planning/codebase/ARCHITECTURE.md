<!-- refreshed: 2026-06-03 -->
# Architecture

**Analysis Date:** 2026-06-03

## System Overview

OpenTune is an AI-powered pitch correction application (开源AI智能修音软件) built on the JUCE framework, supporting dual-format deployment: Standalone executable and VST3 plugin (with optional ARA2 extension). The core audio pipeline uses ONNX Runtime inference (RMVPE for F0 extraction + PC-NSF HiFiGAN neural vocoder) to resynthesize vocals while preserving formants.

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                          Entry Points (2 formats)                             │
├────────────────────────────────────┬─────────────────────────────────────────┤
│   Standalone Editor               │   VST3 Plugin Editor                     │
│   `Source/Standalone/EditorFactor│   `Source/Plugin/EditorFactoryPlugin.cpp` │
│   yStandalone.cpp`                │                                          │
│   `Source/Standalone/PluginEditor.│   `Source/Plugin/PluginEditor.{h,cpp}`   │
│   {h,cpp}`                        │                                          │
└───────────────┬───────────────────┴───────────────────┬──────────────────────┘
                │                                       │
                ▼                                       ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                     OpenTuneAudioProcessor (Shared Core)                      │
│                     `Source/PluginProcessor.{h,cpp}`                          │
│                                                                              │
│  ┌─────────────────┐  ┌────────────────────┐  ┌────────────────────────────┐ │
│  │   SourceStore    │  │ MaterializationStore│  │  StandaloneArrangement     │ │
│  │  (source truth)  │  │  (editable truth)  │  │  (placement/mix truth)     │ │
│  └────────┬────────┘  └─────────┬──────────┘  └─────────────┬──────────────┘ │
│           │                      │                            │               │
│           └──────────────────────┼────────────────────────────┘               │
│                                  │                                            │
│   ┌──────────────────────────────┼───────────────────────────────────┐       │
│   │      Infrastructure Services │                                    │       │
│   │  ┌──────────────┐ ┌──────────┴─────┐ ┌────────────────────────┐  │       │
│   │  │F0Inference   │ │ VocoderDomain  │ │ RenderCache /          │  │       │
│   │  │Service (RMVPE)│ │ (PC-NSF HiFiGAN)│ │ ChunkRenderStrategy   │  │       │
│   │  └──────────────┘ └────────┬───────┘ └────────────────────────┘  │       │
│   │                             │                                     │       │
│   │                    ┌────────┴────────┐                           │       │
│   │                    │ INoteGenerator  │ ← GAME (ONNX) or Legacy   │       │
│   │                    └─────────────────┘                           │       │
│   └──────────────────────────────────────────────────────────────────┘       │
│                                  │                                            │
│   ┌──────────────────────────────┼───────────────────────────────────┐       │
│   │         Background Workers    │                                    │       │
│   │  ┌──────────────────────┐ ┌──┴───────────────────────────────┐   │       │
│   │  │ Chunk Render Worker  │ │ Stage 2 Time-Stretch Worker      │   │       │
│   │  │ (incremental render) │ │ (SoundTouch WSOLA clip-wide)     │   │       │
│   │  └──────────────────────┘ └──────────────────────────────────┘   │       │
│   │  ┌──────────────────────┐ ┌──────────────────────────────────┐   │       │
│   │  │ Note Generator Pool  │ │ Materialization Refresh Service  │   │       │
│   │  │ (ThreadPool, 1)      │ │ (F0ExtractionService)             │   │       │
│   │  └──────────────────────┘ └──────────────────────────────────┘   │       │
│   └──────────────────────────────────────────────────────────────────┘       │
└──────────────────────────────────────────────────────────────────────────────┘
         │                                       │
         ▼                                       ▼
┌────────────────────┐              ┌──────────────────────────────────┐
│   ARA Extension     │              │   Regular VST3 Capture            │
│   `Source/ARA/`     │              │   `Source/Plugin/Capture/`        │
│   DocumentController│              │   CaptureSession / RingBuffer     │
│   + PlaybackRenderer │              │   (record→import pipeline)       │
│   + EditorView       │              └──────────────────────────────────┘
└────────────────────┘
         │
         ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                               External / Output                                │
│  ONNX Runtime (DirectML/CoreML) · SoundTouch (WSOLA) · r8brain (resampler)   │
│  ARA SDK 2.2.0 (Celemony) · JUCE 8 framework                                 │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Component Responsibilities

| Component | Responsibility | File |
|-----------|----------------|------|
| **OpenTuneAudioProcessor** | Central orchestrator: audio I/O, transport, AI inference, project state | `Source/PluginProcessor.{h,cpp}` |
| **SourceStore** | Immutable source audio identity and lifecycle (retire/revive) | `Source/SourceStore.{h,cpp}` |
| **MaterializationStore** | Editable audio payload truth: notes, pitch curves, keys, RenderCache, TimeGrid | `Source/MaterializationStore.{h,cpp}` |
| **StandaloneArrangement** | Multi-track timeline: Placements, Track state, playback snapshots | `Source/StandaloneArrangement.{h,cpp}` |
| **AudioSource / AudioModification / PlaybackRegion** | ARA document model using official object ownership: source identity/sample access, editable content/materialization, and placement | `Source/ARA/AudioSource.*`, `AudioModification.*`, `PlaybackRegion.*` |
| **OpenTuneDocumentController** | ARA projection and persistence owner: maps host objects into playback/materialization projections, stores materialization bindings, and respects `ARAStoreObjectsFilter`/`ARARestoreObjectsFilter` for partial persistency (full-document + sub-graph archive) | `Source/ARA/OpenTuneDocumentController.{h,cpp}` |
| **OpenTuneEditorView** | ARA editor role: consumes host `ViewSelection` / `notifySelection` and exposes focused selection to the VST3 UI | `Source/ARA/OpenTuneEditorView.{h,cpp}` |
| **OpenTunePlaybackRenderer** | ARA playback role: renders the playback-region set assigned by the host to this renderer and returns handled silence for empty/non-overlap blocks | `Source/ARA/OpenTunePlaybackRenderer.{h,cpp}` |
| **F0InferenceService** | RMVPE-based fundamental frequency extraction (CPU only, ONNX) | `Source/Inference/F0InferenceService.{h,cpp}` |
| **VocoderDomain** | Vocoder orchestration: submits inference jobs to scheduler | `Source/Inference/VocoderDomain.{h,cpp}` |
| **RenderCache** | Per-materialization render chunk cache, chunk stats, partial invalidation | `Source/Inference/RenderCache.{h,cpp}` |
| **INoteGenerator** | Polymorphic note generator: GAME (ONNX) or Legacy (DSP on F0) | `Source/Inference/INoteGenerator.h` |
| **VocoderRenderScheduler** | Chunk-level vocoder job scheduling and dispatch | `Source/Inference/VocoderRenderScheduler.{h,cpp}` |
| **ResamplingManager** | Audio sample rate conversion via r8brain | `Source/DSP/ResamplingManager.{h,cpp}` |
| **ChromaKeyDetector** | Auto-key detection from pitch curve data | `Source/DSP/ChromaKeyDetector.{h,cpp}` |
| **SoundTouchStretcher** | WSOLA time-stretch for vocal time manipulation (Stage 2) | `Source/Inference/SoundTouchStretcher.{h,cpp}` |
| **AutoTunePitchShifter** | Lightweight cycle-resampling pitch shift for monitoring | `Source/DSP/AutoTunePitchShifter.{h,cpp}` |
| **CaptureSession** | VST3 live recording: ring buffer, segment state machine, persistence | `Source/Plugin/Capture/CaptureSession.{h,cpp}` |
| **UndoManager** | Undo/redo stack with CompositeUndoAction support | `Source/Utils/UndoManager.{h,cpp}` |
| **ProjectSession** | Standalone project lifecycle: open/save/save-as, dirty tracking, media copying | `Source/Utils/ProjectSession.{h,cpp}` |

## Pattern Overview

**Overall:** Layered Architecture with shared processor core, dual-format editors

**Key Characteristics:**
- Single `OpenTuneAudioProcessor` shared between Standalone and VST3 builds
- Editor is format-specific (Standalone has multi-track ArrangementView, VST3 has PianoRoll only)
- ARA responsibility separation follows the official SDK model: `AudioSource` owns source identity/sample access, `AudioModification` owns editable content/materialization, `PlaybackRegion` owns placement only, `EditorView` owns UI selection projection, and `PlaybackRenderer` renders only the host-assigned playback-region set.
- Source/ARA 层内部设计底线：无锁、无 mutex、无 atomic、无 JUCE lock、无 AppLogger。这不是 ARA2 SDK 规范要求，而是项目层面对 ARA 回调线程模型的内部约束。VST3/ARA editor 通过 processor 侧的 relaxed atomic 镜像读取 host position/play/BPM/time signature/loop。
- Retire/revive lifecycle for undo-safe deferred garbage collection
- Background worker threads: chunk render worker, stage-2 time-stretch worker, note generator pool, materialization refresh service

## Layers

**Core Data Layer:**
- Purpose: Source-of-truth storage for audio sources, editable materializations, and timeline arrangement
- Location: `Source/SourceStore.{h,cpp}`, `Source/MaterializationStore.{h,cpp}`, `Source/StandaloneArrangement.{h,cpp}`
- Contains: CRUD operations with ReadWriteLock, snapshot APIs, retirement lifecycle
- Depends on: JUCE audio basics, Utils (PitchCurve, Note, TimeGrid, etc.)
- Used by: OpenTuneAudioProcessor, Editors, ARA controller, Background workers

**Inference Layer:**
- Purpose: AI model inference for F0 extraction, vocoder synthesis, and note generation
- Location: `Source/Inference/`
- Contains: F0InferenceService (RMVPE), VocoderDomain (PC-NSF HiFiGAN), GameNoteGenerator (ONNX), ModelFactory, VocoderInferenceService, VocoderRenderScheduler, RenderCache, TimeStretchCache, SoundTouchStretcher
- Depends on: ONNX Runtime (Ort::Env), SoundTouch library
- Used by: OpenTuneAudioProcessor (via VocoderDomain/F0InferenceService), Background workers

**DSP Layer:**
- Purpose: Classical digital signal processing — resampling, mel spectrograms, key detection, auto-align, pitch shifting
- Location: `Source/DSP/`
- Contains: ResamplingManager, MelSpectrogram, ChromaKeyDetector, ReferenceAutoAlign, AutoTunePitchShifter, TimeGridPatchBuilder
- Depends on: r8brain (resampler), JUCE DSP
- Used by: OpenTuneAudioProcessor, MaterializationStore, F0ExtractionService

**Service Layer:**
- Purpose: Asynchronous background operations bridging data and inference layers
- Location: `Source/Services/`
- Contains: F0ExtractionService (materialization refresh scheduling), ReferenceAnalysisService (reference alignment features)
- Depends on: Inference, DSP, Data stores
- Used by: OpenTuneAudioProcessor

**Editor Layer:**
- Purpose: JUCE GUI for each build format
- Location: `Source/Standalone/PluginEditor.{h,cpp}` (Standalone), `Source/Plugin/PluginEditor.{h,cpp}` (VST3), `Source/Editor/` (shared)
- Contains: Editor factories, shared preference pages, dialog content
- Depends on: Core processor, Shared UI components
- Used by: JUCE createEditor() entry point

**UI Layer:**
- Purpose: Reusable visual components shared across editor formats
- Location: `Source/Standalone/UI/`
- Contains: PianoRollComponent (shared between Standalone and VST3), ParameterPanel, ArrangementViewComponent (Standalone only), themes, tools, transport bar, menu bar, top bar, track panel, playhead overlay
- Depends on: JUCE graphics/gui_basics/gui_extra
- Used by: Both editor formats

**ARA Layer:**
- Purpose: ARA2 protocol integration for deep DAW integration
- Location: `Source/ARA/`
- Contains: AudioSource, AudioModification, PlaybackRegion, OpenTuneDocumentController, OpenTuneEditorView, OpenTunePlaybackRenderer
- Depends on: ARA SDK 2.2.0, JUCE ARA extension, OpenTuneAudioProcessor playback/materialization APIs
- Used by: VST3 ARA build only (`OPENTUNE_ENABLE_ARA=ON`)

**Capture Layer:**
- Purpose: VST3 non-ARA live audio recording pipeline
- Location: `Source/Plugin/Capture/`
- Contains: CaptureSession (state machine), CaptureRingBuffer, CaptureSegment, CaptureCompactor, CapturePersistence
- Depends on: JUCE audio, OpenTuneAudioProcessor
- Used by: VST3 non-ARA workflow (instantiated conditionally at runtime)

**Audio Layer:**
- Purpose: Audio format I/O and async loading
- Location: `Source/Audio/`
- Contains: AudioFormatRegistry, AsyncAudioLoader
- Depends on: JUCE audio formats
- Used by: Standalone editor (import pipeline)

## Data Flow

### Primary Request Path (Real-time Audio Playback)

1. `processBlock()` — audio callback receives buffer from host/standalone (`Source/PluginProcessor.cpp`)
2. For each track in `PlaybackSnapshot`, compute clip overlap window (`StandaloneArrangement::loadPlaybackSnapshot()`)
3. `readPlaybackAudio()` — unified read: first read dry PCM from source buffer, then overlay rendered chunks from `RenderCache` (`Source/PluginProcessor.cpp:readPlaybackAudio`)
4. Apply `AutoTunePitchShifter` monitoring pass, track gain, fade-in/out, and mix into output buffer
5. Update `playStartPosition` atomically for UI position display

### Import Pipeline (Standalone/VST3 Import)

1. **Prepare** (background thread): `prepareImport()` — resample to 44.1kHz via `ResamplingManager`, detect silent gaps (`Source/PluginProcessor.cpp`)
2. **Commit** (message thread): `commitPreparedImportAsPlacement()` — create `SourceStore` entry, create `MaterializationStore` entry, add `Placement` to `StandaloneArrangement`
3. **Render**: `requestMaterializationRefresh()` — triggers F0 extraction → note generation (GAME/Legacy) → auto-tune correction → chunk-level vocoder render → publish to `RenderCache`

### ARA Audio Modification Birth & Persistence Pipeline

1. Host creates/updates `AudioSource`, `AudioModification`, and `PlaybackRegion` objects through JUCE ARA callbacks on `OpenTuneDocumentController`.
2. `OpenTuneDocumentController` projects the official ARA graph into `PlaybackRegionProjection`: modification content/materialization plus playback placement.
3. Read Audio triggers `OpenTuneDocumentController::refreshAllAudioModifications()`, which deduplicates currently placed regions by unique `AudioModification`.
4. `birthMaterializationForModification()` reads ARA source samples via the host reader lease, writes SourceStore metadata, creates one MaterializationStore entry per AudioModification, notifies content changed, and schedules F0 extraction.
5. `PlaybackRegion` remains placement only: start, offset, duration, and the AudioModification identity it places. Multiple PlaybackRegions can place the same materialized content, including overlaps.
6. Audio source PCM remains host-owned and is streamed from ARA sample access; the plugin stores binding identity/revision metadata and does not duplicate the full source buffer.
7. ARA persistency: `doStoreObjectsToStream()` respects `ARAStoreObjectsFilter` — when non-null, only writes bindings for filter-specified AudioModifications; when null, writes all renderable modifications. `doRestoreObjectsFromStream()` maps archived persistent IDs through `ARARestoreObjectsFilter` and applies pending bindings when modification objects arrive.

### VST3 Capture Pipeline

1. `Capturing` → ring buffer accumulates audio in `CaptureRingBuffer`
2. Segment transitions to `Processing` via `CaptureCompactor`
3. `SubmitForRenderFn` callback feeds compacted buffer into `prepareImport()` → `commitPreparedImportAsMaterialization()` → `requestMaterializationRefresh()`
4. `CapturePersistence` auto-saves session state across plugin reopens

### Render Scheduling (Two-Stage)

**Stage 1 (Chunk Render Worker):** Background thread polls `MaterializationStore` for pending render jobs. For each materialization, runs note generation + auto-tune correction + vocoder synthesis per chunk. Publishes results to `RenderCache` with revision tracking.

**Stage 2 (Time-Stretch Worker):** Separate thread owned by processor. When `TimeGrid` is non-identity, runs SoundTouch WSOLA full-pass rebuild on source PCM. Independent from Stage 1 to keep incremental chunks responsive. Output stored in `TimeStretchCache`.

## Key Abstractions

**Three-Store Truth Model:**
- **SourceStore** — immutable source audio identity. A `Source` represents raw imported audio with buffer, sample rate, and display name. Never holds edit state. Supports soft-delete (retire/revive) for undo safety.
- **MaterializationStore** — editable audio payload. A `Materialization` derives from a `Source` with a `SourceWindow` provenance. Holds: pitch curve, notes, corrected segments, detected key, `RenderCache`, `TimeGrid`, silent gaps, reference features. Each edit bumps a `renderRevision`.
- **StandaloneArrangement** — timeline placement. A `Placement` references a `materializationId` and has timeline position/duration/gain/fades. The arrangement produces immutable `PlaybackSnapshot` for audio thread consumption.

**Placement-Materialization Separation:**
Editors never mutate placements for content edits. All edits (pitch curves, notes, time grids) go through `MaterializationStore`. The `StandaloneArrangement` only tracks which materialization is placed where and when. This aligns with the ARA model: `AudioModification` owns content, `PlaybackRegion` owns placement.

**RenderCache + Playback Projection Pattern:**
- `RenderCache` holds per-materialization rendered audio chunks, each tagged with `renderRevision`
- Standalone playback continues to consume arrangement snapshots produced by `StandaloneArrangement`
- ARA playback does not use the standalone arrangement snapshot path; `OpenTunePlaybackRenderer` pulls fresh projections for the playback regions assigned to that renderer by the host
- Chunk-level granularity enables partial re-render without full clip synthesis

**INoteGenerator Polymorphism:**
- `GameNoteGenerator` (`Source/Inference/GameNoteGenerator.{h,cpp}`) — ONNX GAME-small model, consumes raw audio + sample rate
- `LegacyNoteGenerator` (`Source/Utils/LegacyNoteGenerator.{h,cpp}`) — DSP threshold-based on F0 + energy, consumes frame-domain fields
- Selected at runtime via `ensureNoteGeneratorReady()`: prefers GAME if model bundle exists, falls back to legacy

**Vocoder Pipeline (Domain → Service → Scheduler):**
- `VocoderDomain` — public API: submit(Job), manages hop size and mel bins
- `VocoderInferenceService` — owns ONNX inference session, thread-safe concurrent submits
- `VocoderRenderScheduler` — job queue, chunk key deduplication, completion callbacks to `RenderCache`

## Entry Points

**Standalone Application:**
- Location: `Source/Standalone/EditorFactoryStandalone.cpp` (JM_ENTRY via CMake)
- Triggers: User launches `OpenTune.exe`
- Responsibilities: Create `OpenTuneAudioProcessor`, `OpenTuneAudioProcessorEditor`, setup `AppPreferences` and `ProjectSession`, register drop targets

**VST3 Plugin:**
- Location: `Source/Plugin/EditorFactoryPlugin.cpp` (JM_ENTRY via CMake)
- Triggers: Host loads `OpenTune.vst3`
- Responsibilities: Create processor, detect wrapper type, conditionally instantiate `CaptureSession` or bind to ARA

**ARA Document Controller:**
- Location: `Source/ARA/OpenTuneDocumentController.{h,cpp}`
- Triggers: Host creates ARA document (e.g., dragging audio to track in Studio One)
- Responsibilities: Own ARA object projections, persist materialization bindings, create `OpenTuneEditorView` and `OpenTunePlaybackRenderer`

**ARA Editor View:**
- Location: `Source/ARA/OpenTuneEditorView.{h,cpp}`
- Triggers: Host sends `ARAEditorViewInterface::notifySelection`
- Responsibilities: Call the JUCE base `ARAEditorView` hook, copy effective playback-region selection, expose focused region selection to the VST3 plugin UI

**ARAPlaybackRenderer:**
- Location: `Source/ARA/OpenTunePlaybackRenderer.{h,cpp}`
- Triggers: Host audio callback per ARA region
- Responsibilities: Maintain the host-assigned playback-region set, compute block-aligned overlap for every assigned region, mix overlaps, call `readPlaybackAudio()`, clear buffers and return handled silence when no region renders

## Architectural Constraints

- **Threading:** Standalone audio reads immutable arrangement snapshots; ARA code is intentionally lock-free and contains no mutex, atomic, JUCE lock, AppLogger, or fallback state machine. Edit mutations use the existing store locks outside `Source/ARA`. Background workers: chunk render, stage-2 time-stretch, note generator pool (1 thread), materialization refresh service (1 thread).
- **Global state:** `Ort::Env` shared across all inference services. `ResamplingManager`, `SourceStore`, `MaterializationStore` are `shared_ptr`-owned within the processor (or shared document controller for ARA).
- **Circular imports:** Front-facing `PluginProcessor.h` is the aggregation point — includes all sub-modules. Sub-modules avoid including `PluginProcessor.h` (use forward declarations).
- **Double-precision:** Processor supports `supportsDoublePrecisionProcessing()` — uses `doublePrecisionScratch_` buffer to convert double→float internally since inference works in float.
- **Stored sample rate:** All audio data stored at fixed 44.1kHz (`TimeCoordinate::kRenderSampleRate`), independent of host/device sample rate. `ResamplingManager` handles conversion at boundaries.

## Anti-Patterns

### Editor Format Compile-Guard `#if`

**What happens:** Editor implementation files are guarded by `#if JucePlugin_Build_Standalone` / `#if JucePlugin_Build_VST3` to compile different sources per format.
**Why it's wrong:** Creates source files that exist only partially for some targets. Makes IDE navigation confusing.
**Do this instead:** The editor factory pattern (`EditorFactory.h` + format-specific `.cpp` files) already isolates the format selection. Keep guards only where CMake `target_sources` can't cleanly separate (e.g., shared headers that include format-specific includes). New editors: use `EditorFactory.cpp` approach with CMake per-target source lists.

### Large Header Aggregation in PluginProcessor.h

**What happens:** `Source/PluginProcessor.h` includes 30+ internal headers, making it a de facto "god header" (885 lines). Every change to any utility triggers full rebuilds of all consumers.
**Why it's wrong:** Slow incremental build times; tight coupling between core processor and utilities.
**Do this instead:** Add new includes in `.cpp` when possible. Use forward declarations in headers. Consider extracting stable interfaces (e.g., `INoteGenerator.h`, `IF0Extractor.h`) as standalone headers.

## Error Handling

**Strategy:** `Result<T>` monad pattern via `Source/Utils/Error.h` for fallible operations. Callback-based error reporting for async services (e.g., `onComplete(bool, String&, vector<float>&)`).

**Patterns:**
- Inference services return `Result<std::vector<float>>` for extraction results
- `ReferenceAnalysisService::Listener` interface for async analysis completion/failure
- `ReferenceAlignmentResult::Status` enum for structured alignment outcomes
- Export errors stored in `lastExportError_` String member

## Cross-Cutting Concerns

**Logging:** `AppLogger` (`Source/Utils/AppLogger.{h,cpp}`) — structured logging with file output. `ChannelLayoutLogger` for audio channel diagnostics.
**Validation:** `SilentGapDetector` analyzes audio for silent regions during import. `SourceWindow` defines valid audio ranges.
**Authentication:** Not applicable (offline application, no network auth).
**Localization:** `LocalizationManager` (`Source/Utils/LocalizationManager.h`) — supports zh-CN, en, ja, ru, es. Language change listener pattern propagated to all UI components.
**Preferences:** `AppPreferences` (`Source/Utils/AppPreferences.{h,cpp}`) — persisted via JUCE PropertiesFile, covers GPU/CPU toggle, theme, language, snap settings, vocoder model weight.

---

*Architecture analysis: 2026-06-03*
