# Architecture

**Analysis Date:** 2026-04-30

## Pattern Overview

**Overall:** One shared JUCE processor runtime, four concrete runtime carriers, and two compile-time-isolated editor shells. `OpenTuneAudioProcessor` 组合 `SourceStore + MaterializationStore + StandaloneArrangement + VST3AraSession`；persisted truth 是 `Source + Materialization + Placement`，`Projection` 只是 derived contract。

**Key Characteristics:**
- `Source/PluginProcessor.h` and `Source/PluginProcessor.cpp` define `OpenTuneAudioProcessor` as the shared runtime shell for both products; the constructor instantiates `SourceStore`, `MaterializationStore`, `StandaloneArrangement`, `VST3AraSession`, `HostIntegration`, and `ResamplingManager`.
- `Source/Standalone/PluginEditor.h` and `Source/Standalone/PluginEditor.cpp` remain the Standalone editor shell; `Source/Plugin/PluginEditor.h` and `Source/Plugin/PluginEditor.cpp` remain the VST3 editor shell; `Source/Standalone/EditorFactoryStandalone.cpp` and `Source/Editor/EditorFactoryPlugin.cpp` select the shell at compile time.
- Shared playback reading is centralized in `OpenTuneAudioProcessor::readPlaybackAudio()` in `Source/PluginProcessor.cpp`; Standalone `processBlock()` and `Source/ARA/OpenTunePlaybackRenderer.cpp` both build read requests into that same function.
- The live tree already exposes separate `sourceId` / `materializationId` / `placementId` in `OpenTuneAudioProcessor::CommittedPlacement` in `Source/PluginProcessor.h`; `StandaloneArrangement::Placement` in `Source/StandaloneArrangement.h` keeps placement-local timeline data plus a referenced `materializationId`.
- VST3 ARA state is no longer described by `OpenTuneDocumentController` itself; `Source/ARA/OpenTuneDocumentController.cpp` mainly forwards host callbacks into `VST3AraSession` and creates `OpenTunePlaybackRenderer`.

## Clarified Product Truth Model (2026-04-21)

**Persisted owners:**

- `Source`: 原始来源真相；回答“这些实例是否同源”。拥有原始音频 provenance / hydration 资产，不拥有 notes 或 corrected F0。
- `Materialization`: 独立可编辑实例真相；拥有 local audio slice、notes、pitch curve、corrected segments、detected key、render state。`Note.startTime/endTime` 与 `CorrectedSegment.startFrame/endFrame` 都必须 materialization-local。
- `Placement`: 时间轴摆放真相；拥有 `placementId`、track、timeline start、gain/fade、UI metadata，不拥有 editable payload。

**Derived contract:**

- `Projection`: 由一个 placement 与一个 materialization 生成的显式映射值对象；供 Piano Roll、playback、renderer 消费，不是 persisted truth owner。

**Implications:**

- 同一份 source 再次出现于新的 placement / playback region 时，默认必须 birth 新 materialization，而不是共享 editable owner。
- split 默认必须 birth left/right materialization，而不是只改 placement window。
- ARA sibling regions 只能共享 source hydration 资产，不能共享 editable materialization。

## Current Object Mapping And Role Mismatch

| Live-tree object | Current behavior | Clarified target role | Reading |
|---|---|---|---|
| `SourceStore` | provenance + hydrated source audio | `SourceStore` | Landed |
| `MaterializationStore` | `audioBuffer + notes + pitchCurve + key + render cache` | `MaterializationStore` | Landed |
| `StandaloneArrangement::Placement` | placement + `materializationId` | `Placement` | Landed |
| `MaterializationTimelineProjection` | 显式 timeline/materialization window 映射 | `Projection` value object | Landed |
| `VST3AraSession::SourceSlot` | source identity + copied host audio hydration | Source adapter carrier | Mostly right |
| `VST3AraSession::RegionSlot` | region timing + applied content binding | region-local placement/materialization binding | Currently mixed |
| `AppliedMaterializationProjection` | region-local applied owner + projection revision | `AppliedMaterializationProjection` | Landed |
| `PublishedRegionView` | immutable ARA read model publishing `sourceId + appliedProjection + projection` | `PublishedRegionView` | Landed |
| `PluginEditor::recordRequested()` / `syncImportedAraClipIfNeeded()` | current region birth + current region refresh | region-local materialization creation/refresh | Landed |
| `OpenTuneAudioProcessor::CommittedPlacement` | `{sourceId, materializationId, placementId}` | `{sourceId, materializationId, placementId}` | Landed |
| `reclaimUnreferencedMaterialization()` / `reclaimUnreferencedSource()` | three-layer reclaim (`placement -> materialization -> source`) | three-layer reclaim | Landed |

## Layers

**Build And Target Split:**
- Purpose: Declare the dual-format JUCE target, list shared sources, and attach format-only files and post-build resources.
- Location: `CMakeLists.txt`
- Verified responsibilities:
- `juce_add_plugin(OpenTune ...)` builds `Standalone` and `VST3` formats from one shared target.
- `target_sources(OpenTune_Standalone ...)` adds `Source/Standalone/EditorFactoryStandalone.cpp` and `Source/Standalone/PluginEditor.cpp` only to the Standalone target.
- `target_sources(OpenTune_VST3 ...)` adds `Source/Editor/EditorFactoryPlugin.cpp` and `Source/Plugin/PluginEditor.cpp` only to the VST3 target.
- `target_sources(OpenTune PRIVATE ...)` always includes shared processor, UI, inference, utility, and ARA source files.
- `OpenTuneTests` is declared as a separate native test executable in `CMakeLists.txt`.

**Shared Runtime Shell:**
- Purpose: Coordinate transport, playback, import, derived refresh, render scheduling, undo/redo, and processor state persistence.
- Location: `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`
- Verified responsibilities:
- `prepareToPlay()`, `processBlock()`, `createEditor()`, `getStateInformation()`, and `setStateInformation()` remain the JUCE entrypoints.
- `prepareImport()`, `commitPreparedImportAsPlacement()`, and `commitPreparedImportAsMaterialization()` separate background preprocessing from main-thread `Source -> Materialization -> Placement` birth.
- `requestMaterializationRefresh()` starts asynchronous derived refresh through `F0ExtractionService`.
- `readPlaybackAudio()` is the shared dry-signal-plus-render-cache read kernel.
- `performUndo()` and `performRedo()` return structured `UndoExecutionResult` values instead of void side effects.
- `getUndoManager()` exposes the processor-owned custom `UndoManager` (cursor-based, 500-deep) for both editor shells.

**Source + Materialization Layers:**
- Purpose: `SourceStore` owns source provenance / hydrated source audio identity; `MaterializationStore` owns editable local payload and playback/read truth independent of Standalone placement or VST3 region mapping.
- Location: `Source/SourceStore.h`, `Source/SourceStore.cpp`, `Source/MaterializationStore.h`, `Source/MaterializationStore.cpp`
- Verified responsibilities:
- `SourceStore` owns `sourceId`, display/provenance metadata, source audio buffer, and source sample-rate metadata.
- `MaterializationStore` owns `materializationId`, `sourceId`, source provenance window, lineage parent metadata, local audio buffer, `PitchCurve`, `DetectedKey`, `RenderCache`, notes, silent gaps, and render/note revisions.
- `getPlaybackReadSource()` publishes the render cache and device-rate dry signal needed by playback readers.
- `replaceAudio()`, `setPitchCurve()`, `setNotes()`, `setSilentGaps()`, and `setDetectedKey()` update materialization-local state.
- `enqueuePartialRender()` and `pullNextPendingRenderJob()` make `MaterializationStore` the owner of pending render work segmentation.

**Standalone Placement Layer:**
- Purpose: Own Standalone-only placement, track state, selection, and audio-thread playback snapshots.
- Location: `Source/StandaloneArrangement.h`, `Source/StandaloneArrangement.cpp`
- Verified responsibilities:
- `Placement` carries `placementId`, `materializationId`, `mappingRevision`, `timelineStartSeconds`, `durationSeconds`, `gain`, fades, name, and colour.
- `Track` stores placement lists, selected placement, mute/solo, gain, display metadata, and RMS.
- `PlaybackSnapshot` publishes immutable playback state for 12 tracks.
- `insertPlacement()`, `replacePlacement()`, `deletePlacementById()`, `movePlacementToTrack()`, and `setPlacementTimelineStartSeconds()` define placement-level mutations.
- `loadPlaybackSnapshot()` is the audio-thread read seam for Standalone playback.
- 2026-04-21 clarification: `contentStartSeconds` and `contentId` now read as leaked historical naming; target semantics should be `materializationId` and, by default, no placement-owned editable window.

**VST3 ARA Session Layer:**
- Purpose: Own VST3-only ARA source identity, region mapping, preferred-region selection, copied source audio hydration, and region-local source/materialization binding publication.
- Location: `Source/ARA/VST3AraSession.h`, `Source/ARA/VST3AraSession.cpp`
- Verified responsibilities:
- `SourceSlot` stores `ARAAudioSource` identity, copied host audio, sample-access reader leases, and source content revisions.
- `RegionSlot` stores `RegionIdentity`, source/playback time ranges, `projectionRevision`, and `AppliedMaterializationProjection`.
- `AppliedMaterializationProjection` stores `sourceId`, `materializationId`, applied materialization/projection revisions, source range, playback start, and the explicit `appliedRegionIdentity`.
- `PublishedRegionView` and `PublishedSnapshot` are the immutable read models consumed by the VST3 editor and ARA renderer, and now publish `sourceId` explicitly.
- `bindPlaybackRegionToMaterialization()`, `updatePlaybackRegionMaterializationRevisions()`, and `clearPlaybackRegionMaterialization()` define the visible source/materialization-plus-projection bridge toward the VST3 path.
- A dedicated hydration worker thread is implemented inside `VST3AraSession.cpp`.
- 2026-04-21 clarification: the source carrier itself is still useful, but the current region-to-content bridge is no longer considered the correct editable-owner model because sibling regions must not share one editable materialization by default.

**ARA Adapter Layer:**
- Purpose: Bridge ARA host callbacks and host playback requests into `VST3AraSession` and the shared playback read path.
- Location: `Source/ARA/OpenTuneDocumentController.h`, `Source/ARA/OpenTuneDocumentController.cpp`, `Source/ARA/OpenTunePlaybackRenderer.h`, `Source/ARA/OpenTunePlaybackRenderer.cpp`
- Verified responsibilities:
- `OpenTuneDocumentController::setProcessor()` stores the processor pointer and resolves the shared `VST3AraSession`.
- Most document-controller lifecycle callbacks forward directly into the session.
- `doCreatePlaybackRenderer()` returns `OpenTunePlaybackRenderer`.
- `OpenTunePlaybackRenderer` maps overlap between host playback blocks and published ARA region spans, then calls `OpenTuneAudioProcessor::readPlaybackAudio()`.

**Format-Specific Editor Shells:**
- Purpose: Keep product workflow and UI orchestration isolated while reusing shared widgets.
- Location: `Source/Standalone/PluginEditor.h`, `Source/Standalone/PluginEditor.cpp`, `Source/Plugin/PluginEditor.h`, `Source/Plugin/PluginEditor.cpp`
- Verified Standalone responsibilities:
- Owns arrangement view, track panel, drag-and-drop import, queued background import, preset handling, and placement selection to piano-roll sync.
- Calls `commitPreparedImportAsPlacement()` with explicit `ImportPlacement` from editor-side workflow.
- Verified VST3 responsibilities:
- Owns single-workspace piano-roll flow, host transport requests, ARA read-audio import, and ARA snapshot consumption.
- `recordRequested()` reads the preferred ARA region, ensures the referenced `SourceStore` owner exists, births a fresh region-local materialization, registers the region-materialization binding, and triggers derived refresh.

**Shared UI Layer:**
- Purpose: Provide reusable JUCE widgets used by both editor shells.
- Location: `Source/Standalone/UI/`, `Source/Standalone/UI/PianoRoll/`
- Verified responsibilities:
- `ParameterPanel`, `MenuBarComponent`, `TransportBarComponent`, `TopBarComponent`, and `PianoRollComponent` are included by both editor headers.
- `TrackPanelComponent` and `ArrangementViewComponent` are Standalone-only workflow widgets.
- `AutoRenderOverlayComponent` is reused by both shells.
- `RenderBadgeComponent` is a lightweight floating status badge (semi-transparent rounded rect + white text) held by both editor shells for render state display.
- `PlayheadOverlayComponent` is an independent transparent overlay child handling playhead line drawing, extracted from PianoRoll paint.
- `PianoRollRenderer`, `PianoRollToolHandler`, `PianoRollUndoSupport`, `PianoRollCorrectionWorker`, and `PianoRollVisualInvalidation` separate piano-roll sub-responsibilities under `Source/Standalone/UI/PianoRoll/`.

**Inference, DSP, And Utility Policy Layer:**
- Purpose: Supply audio preprocessing, model inference, render publication, app preferences, editing rules, and logging.
- Location: `Source/Inference/`, `Source/DSP/`, `Source/Services/`, `Source/Utils/`
- Verified responsibilities:
- `F0InferenceService`, `VocoderDomain`, `VocoderRenderScheduler`, `RenderCache`, `RMVPEExtractor`, and `PCNSFHifiGANVocoder` live under `Source/Inference/`.
- `ResamplingManager`, `MelSpectrogram`, and `ScaleInference` live under `Source/DSP/`.
- `F0ExtractionService` lives under `Source/Services/` and is called by `requestMaterializationRefresh()`.
- `AppPreferences`, `AudioEditingScheme`, `ParameterPanelSync`, `PitchCurve`, `PresetManager`, `TimeCoordinate`, and `AppLogger` live under `Source/Utils/`.

## Data Flow

**Standalone Import -> Source -> Materialization -> Placement -> Derived Refresh:**
- `Source/Standalone/PluginEditor.cpp` resolves an explicit `OpenTuneAudioProcessor::ImportPlacement` before background work starts.
- `OpenTuneAudioProcessor::prepareImport()` preprocesses the incoming buffer into stored local audio and leaves post-import analysis for later refresh.
- `OpenTuneAudioProcessor::commitPreparedImportAsPlacement()` creates or resolves one `sourceId`, births a new `materializationId` with source provenance metadata, then inserts a `StandaloneArrangement::Placement` that references that materialization.
- The Standalone editor updates the piano roll using the returned `materializationId` and asks `requestMaterializationRefresh()` to populate derived material.

**Standalone Playback -> Snapshot -> Materialization Read -> Mix:**
- `OpenTuneAudioProcessor::processBlock()` loads `StandaloneArrangement::PlaybackSnapshot`.
- Each active placement resolves a `MaterializationStore::PlaybackReadSource` by `placement.materializationId`.
- The processor computes the overlap between the current block and each placement's timeline span.
- `readPlaybackAudio()` copies device-rate dry audio and overlays published render-cache audio into the clip scratch buffer.
- Per-placement fades and per-track gain/mute/solo rules are applied before the track mix is accumulated into the output block.

**Materialization Refresh -> Render Queue -> Vocoder Publication:**
- `requestMaterializationRefresh()` validates materialization existence and clears only the changed note/correction range when requested.
- `F0ExtractionService` refreshes materialization-local derived state for the target `materializationId`.
- `enqueueMaterializationPartialRenderById()` leads to `MaterializationStore::enqueuePartialRender()`, which stores hop-aligned pending jobs.
- `chunkRenderWorkerLoop()` consumes pending jobs, submits vocoder work through `VocoderDomain`, and writes completion back into `RenderCache`.

**VST3 Read Audio -> Region-Local Materialization Binding -> Piano Roll Sync:**
- `PluginUI::OpenTuneAudioProcessorEditor::recordRequested()` loads the current `VST3AraSession::PublishedSnapshot`.
- It resolves the preferred `PublishedRegionView`, seeds the missing `SourceStore` owner if needed, preprocesses copied ARA audio, and births a fresh materialization using the existing `sourceId` of that ARA source.
- The editor calls `VST3AraSession::bindPlaybackRegionToMaterialization()` with `materializationId`, projection revision, source range, and playback start.
- The editor then syncs the piano roll and requests derived refresh for the bound materialization.

**VST3 ARA Playback -> Published Region -> Shared Read Path:**
- `OpenTuneDocumentController` forwards host edits and sample-access lifecycle into `VST3AraSession`.
- `VST3AraSession` updates mutable source/region state, reconciles preferred region, and publishes immutable snapshots.
- `OpenTunePlaybackRenderer` finds the renderable published region for the host block and maps playback time into source time.
- The renderer converts that mapping into a shared `PlaybackReadRequest` and reuses `OpenTuneAudioProcessor::readPlaybackAudio()`.

**Processor State Persistence:**
- `getStateInformation()` writes a root `OpenTuneState` `ValueTree`.
- It serializes `Contents` and `StandaloneArrangement` as separate child trees, rather than one mixed placement-content container.
- Only content IDs referenced by current Standalone placements or current VST3 published bindings are added to `Contents`.
- `setStateInformation()` restores content-local metadata into already-existing content entries, then restores Standalone arrangement state if placement records are valid.

## Ownership Boundaries

**`SourceStore`:**
- Owns: `sourceId`, source provenance metadata, hydrated source audio, source sample-rate metadata.
- Does not own: editable notes / pitch / render cache / placement timeline.

**`MaterializationStore`:**
- Owns: materialization-local audio, dry-signal playback copy, notes, pitch curve, silent gaps, detected key, render cache, render queue, source provenance window, lineage metadata.
- Does not own: Standalone track placement, VST3 ARA region identity, editor selection state.

**`StandaloneArrangement`:**
- Owns: Standalone track ordering, placement timeline spans, per-track mute/solo/volume, selected placement, playback snapshots.
- Does not own: audio buffers, pitch data, ARA host objects.

**`VST3AraSession`:**
- Owns: ARA source slots, region slots, preferred region, copied host audio hydration, region-to-materialization binding snapshots.
- Does not own: materialization-local edit payload or Standalone track arrangement.

**`OpenTuneAudioProcessor`:**
- Owns: orchestration, shared playback read API, render workers, undo manager, and processor state serialization.
- Does not replace the three domain owners above.

## Key Abstractions

**Materialization Snapshot:**
- Files: `Source/MaterializationStore.h`, `Source/MaterializationStore.cpp`
- Role: Immutable-style copy of materialization-local state keyed by `materializationId`, including source provenance window and lineage metadata.

**Placement Snapshot:**
- Files: `Source/StandaloneArrangement.h`, `Source/StandaloneArrangement.cpp`
- Role: Timeline-facing placement data plus immutable `PlaybackSnapshot` publication for the audio thread.

**ARA Published Snapshot:**
- Files: `Source/ARA/VST3AraSession.h`, `Source/ARA/VST3AraSession.cpp`
- Role: Immutable VST3 read model containing preferred region and published region views.

**Shared Playback Request:**
- Files: `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`
- Role: Product-agnostic playback read contract built from a render cache, dry signal, read start, target rate, and sample count.

**Undo Result Chain:**
- Files: `Source/Utils/UndoAction.h`, `Source/Utils/UndoAction.cpp`, `Source/PluginProcessor.cpp`
- Role: Structured undo/redo deltas carrying domain, kind, `materializationId`, `placementId`, `trackId`, and optional affected frame range.

**App Preference Carrier:**
- Files: `Source/Utils/AppPreferences.h`, `Source/Utils/AppPreferences.cpp`
- Role: Persist shared app state and standalone-only app state outside processor/project serialization.

**Editing Scheme Rules:**
- Files: `Source/Utils/AudioEditingScheme.h`
- Role: Pure functions that derive voiced-only editing, parameter targets, and auto-tune targets from explicit scheme input.

## Entry Points

**Build Entry:**
- File: `CMakeLists.txt`
- Role: Declares shared sources, format-specific editor sources, vendored dependencies, and `OpenTuneTests`.

**Runtime Entry:**
- File: `Source/PluginProcessor.cpp`
- Role: `createPluginFilter()` constructs `OpenTuneAudioProcessor`.

**Editor Creation Entry:**
- Files: `Source/Editor/EditorFactoryPlugin.cpp`, `Source/Standalone/EditorFactoryStandalone.cpp`
- Role: Return the format-specific editor shell selected by compile definitions.

**ARA Host Entry:**
- File: `Source/ARA/OpenTuneDocumentController.cpp`
- Role: Receives ARA lifecycle callbacks and creates `OpenTunePlaybackRenderer`.

**Test Entry:**
- File: `Tests/TestMain.cpp`
- Role: Declares `core`, `processor`, `ui`, and `architecture` suites in one native test executable.

## Historical Direction Of `content + placement`

- `OpenTuneAudioProcessor::CommittedPlacement` in `Source/PluginProcessor.h` returns both `contentId` and `placementId`.
- `StandaloneArrangement::Placement` in `Source/StandaloneArrangement.h` stores its own `placementId` plus referenced `contentId`, placement timeline start, content start, duration, and `mappingRevision`.
- `getStateInformation()` in `Source/PluginProcessor.cpp` serializes `Contents` and `StandaloneArrangement` into separate trees.
- `VST3AraSession::AppliedContentProjection` in `Source/ARA/VST3AraSession.h` stores `contentId`, applied content/projection revisions, source bounds, playback start, and the applied region identity.
- `OpenTunePlaybackRenderer` reads ARA playback by mapping published region playback time back into source time, then into `contentId`-backed playback reads.

These bullets still describe the live tree, but after the 2026-04-21 clarification they are no longer sufficient to describe the correct product truth model.

## Cross-Cutting Concerns

**Logging:**
- `Source/Utils/AppLogger.h` and `Source/Utils/AppLogger.cpp` are used by processor, ARA, and editor code.

**Threading:**
- `SourceStore` and `MaterializationStore` use `juce::ReadWriteLock`.
- `StandaloneArrangement` uses `juce::ReadWriteLock` plus a snapshot spin lock.
- `VST3AraSession` uses `std::mutex`, a condition variable, and a worker thread for source hydration.
- Audio-thread reads consume immutable snapshot handles and read-source structs rather than mutable editor state.

**Preferences Boundary:**
- `AppPreferences` persists app-level settings separately from processor state.
- Shared UI preferences and editing scheme live under `SharedPreferencesState`; shortcut and mouse-trail settings live under `StandalonePreferencesState`.

**UI Isolation:**
- VST3 and Standalone editors remain in different directories and are chosen by separate factory `.cpp` files.
- Shared widgets stay under `Source/Standalone/UI/` instead of duplicating code across both shells.

---

*Architecture analysis: 2026-04-20*
