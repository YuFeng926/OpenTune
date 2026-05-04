# Codebase Structure

**Analysis Date:** 2026-04-30

## Directory Layout

```text
[project-root]/
|-- .planning/                  # project context, state, and codebase memory docs
|-- docs/                       # plan docs and bundled user guide source
|-- JUCE-master/                # vendored JUCE source
|-- models/                     # RMVPE model assets
|-- pc_nsf_hifigan_44.1k_ONNX/  # HiFi-GAN ONNX model asset source
|-- Resources/                  # embedded resources such as fonts and app icon
|-- Source/                     # production C++ source tree
|-- Tests/                      # native smoke and architecture tests
|-- ThirdParty/                 # vendored ARA, ONNX, DirectML, and other dependencies
|-- build-debug/                # generated build tree
`-- CMakeLists.txt              # shared build definition for app, plugin, and tests
```

## Root-Level Roles

**`CMakeLists.txt`:**
- Declares one shared `OpenTune` JUCE target, then extends `OpenTune_Standalone`, `OpenTune_VST3`, and `OpenTuneTests`.
- Lists source files explicitly; this repository does not rely on source globbing.
- Copies runtime model files and platform runtime dependencies after build.
- Adds Standalone-only docs packaging and macOS bundle plist/resource handling.

**`.planning/`:**
- Stores active project context and memory docs.
- Core files: `.planning/PROJECT.md`, `.planning/REQUIREMENTS.md`, `.planning/ROADMAP.md`, `.planning/STATE.md`.
- Codebase memory docs live in `.planning/codebase/`.

**`docs/`:**
- Contains `docs/plans/` and `docs/UserGuide.html`.
- `docs/UserGuide.html` is copied into Standalone outputs by `CMakeLists.txt`.

**`build-debug/`:**
- Generated build output tree.
- Contains Visual Studio projects, test logs, and built artifacts for Standalone, VST3, and tests.

## Production Source Tree

**`Source/`:**
- Main production tree.
- Top-level subdirectories are architecture boundaries: `ARA/`, `Audio/`, `DSP/`, `Editor/`, `Host/`, `Inference/`, `Plugin/`, `Services/`, `Standalone/`, `Utils/`.
- Top-level shared owners also live directly under `Source/`: `SourceStore.*`, `MaterializationStore.*`, `StandaloneArrangement.*`, and `PluginProcessor.*`.

**`Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`:**
- Shared runtime shell and JUCE processor entrypoint.
- Main coordination point for playback, import, render scheduling, undo, and processor state serialization.

**`Source/SourceStore.h`, `Source/SourceStore.cpp`:**
- Source identity and provenance owner.
- Holds source identity, provenance windows, and lineage metadata.

**`Source/MaterializationStore.h`, `Source/MaterializationStore.cpp`:**
- Materialization payload owner.
- Holds audio buffers, dry-signal playback copies, notes, pitch curve, render cache, silent gaps, pending render jobs, source provenance window, and lineage metadata.

**`Source/StandaloneArrangement.h`, `Source/StandaloneArrangement.cpp`:**
- Standalone placement and track-state owner.
- Holds placements, selection, mute/solo/volume, RMS, and immutable playback snapshots.

## Source Subdirectories

**`Source/ARA/`:**
- VST3 ARA adapter and session state.
- Files: `OpenTuneDocumentController.*`, `OpenTunePlaybackRenderer.*`, `VST3AraSession.*`.
- Boundary: host callbacks, published region snapshots, source hydration, and ARA playback rendering.

**`Source/Audio/`:**
- Import-time audio helpers.
- Files: `AsyncAudioLoader.h`, `AudioFormatRegistry.cpp`, `AudioFormatRegistry.h`.
- Boundary: supported input formats and async file-loading support.

**`Source/DSP/`:**
- DSP-only helpers that do not own application workflow state.
- Files: `ResamplingManager.*`, `MelSpectrogram.*`, `ScaleInference.*`.

**`Source/Editor/`:**
- Small shared editor seams.
- Files: `EditorFactory.h`, `EditorFactoryPlugin.cpp`, `Preferences/`.
- Boundary: cross-product editor creation seam and preference-page composition.

**`Source/Editor/Preferences/`:**
- Shared preferences dialog construction.
- Files: `SharedPreferencePages.*`, `StandalonePreferencePages.*`, `TabbedPreferencesDialog.h`.
- Boundary: shared pages vs standalone-only pages are split here, not inside the processor.

**`Source/Host/`:**
- Host abstraction boundary.
- Files: `HostIntegration.h`, `HostIntegrationPlugin.cpp`, `HostIntegrationStandalone.cpp`.
- Boundary: initial processor configuration and format-specific processing hooks.

**`Source/Inference/`:**
- Model inference, render cache, and vocoder pipeline.
- Files include `F0InferenceService.*`, `RenderCache.*`, `RMVPEExtractor.*`, `PCNSFHifiGANVocoder.*`, `VocoderDomain.*`, `VocoderRenderScheduler.*`, `ModelFactory.*`.

**`Source/Plugin/`:**
- VST3-only editor shell.
- Files: `PluginEditor.h`, `PluginEditor.cpp`.
- Boundary: single-workspace VST3 UI, ARA read-audio workflow, host transport UI coordination.

**`Source/Services/`:**
- Mid-level background services used by the processor.
- Files: `F0ExtractionService.*`, `ImportedClipF0Extraction.h`.

**`Source/Standalone/`:**
- Standalone-only editor shell and standalone editor factory.
- Files: `PluginEditor.h`, `PluginEditor.cpp`, `EditorFactoryStandalone.cpp`, `UI/`.
- Boundary: multi-track arrangement workflow, import queueing, preset flow, and standalone-only editing shell.

**`Source/Standalone/UI/`:**
- Reusable JUCE UI components used heavily by Standalone and partly by VST3.
- Key files: `MenuBarComponent.*`, `TransportBarComponent.*`, `TopBarComponent.*`, `ParameterPanel.*`, `PianoRollComponent.*`, `ArrangementViewComponent.*`, `TrackPanelComponent.*`, `PlayheadOverlayComponent.*`, `AutoRenderOverlayComponent.h`, `RenderBadgeComponent.h`.
- Theme/look-and-feel files present in live tree: `OpenTuneLookAndFeel.h`, `BlueBreezeLookAndFeel.*`, `DarkBlueGreyLookAndFeel.*`, `AuroraLookAndFeel.*`, plus matching theme headers.

**`Source/Standalone/UI/PianoRoll/`:**
- Piano-roll internal submodules.
- Files: `InteractionState.*`, `PianoRollCorrectionWorker.*`, `PianoRollRenderer.*`, `PianoRollToolHandler.*`, `PianoRollUndoSupport.*`, `PianoRollVisualInvalidation.*`.

**`Source/Utils/`:**
- Shared low-level models, preference carriers, logging, and policy helpers.
- Key files: `AppPreferences.*`, `AudioEditingScheme.h`, `ParameterPanelSync.h`, `PitchCurve.*`, `PresetManager.*`, `SilentGapDetector.*`, `TimeCoordinate.h`, `UndoAction.*`, `UndoManager.*`, `PianoRollEditAction.*`, `F0Timeline.h`, `LocalizationManager.h`.

## Tests And Verification Assets

**`Tests/`:**
- Native test target sources.
- Files: `TestMain.cpp`, `TestSupport.cpp`, `TestSupport.h`, `TestEditorFactoryStub.cpp`.
- `TestMain.cpp` declares four suites: `core`, `processor`, `ui`, and `architecture`.
- Tests verify both source behavior and selected repository structure/contracts by reading workspace files directly.

## Build-Vs-Source Ownership

**Shared production sources:**
- Declared under `target_sources(OpenTune ...)` in `CMakeLists.txt`.
- Include processor, content store, standalone arrangement, shared UI, inference, utilities, host integration, and ARA source files.

**Standalone-only sources:**
- Declared under `target_sources(OpenTune_Standalone ...)`.
- Current files: `Source/Standalone/EditorFactoryStandalone.cpp`, `Source/Standalone/PluginEditor.cpp`, `Source/Standalone/PluginEditor.h`, and `Source/Utils/D3D12AgilityBootstrap.cpp`.

**VST3-only sources:**
- Declared under `target_sources(OpenTune_VST3 ...)`.
- Current files: `Source/Editor/EditorFactoryPlugin.cpp`, `Source/Plugin/PluginEditor.cpp`, `Source/Plugin/PluginEditor.h`, and `Source/Utils/D3D12AgilityBootstrap.cpp`.

**Shared ARA sources still compiled from the shared target:**
- `Source/ARA/OpenTuneDocumentController.*`, `Source/ARA/OpenTunePlaybackRenderer.*`, `Source/ARA/VST3AraSession.*` are attached via `target_sources(OpenTune PRIVATE ...)`.
- Usage is still gated by build flags and source-level `#if` branches.

## Key File Locations

**Runtime Entry Points:**
- `Source/PluginProcessor.cpp`: `createPluginFilter()`, processor construction, shared playback loop.
- `Source/PluginProcessor.h`: shared public runtime API and read/import request types.

**Editor Entry Points:**
- `Source/Editor/EditorFactory.h`: editor factory declaration.
- `Source/Standalone/EditorFactoryStandalone.cpp`: Standalone editor creation.
- `Source/Editor/EditorFactoryPlugin.cpp`: VST3 editor creation.

**State Owners:**
- `Source/SourceStore.cpp`: source identity and provenance truth.
- `Source/MaterializationStore.cpp`: materialization payload truth.
- `Source/StandaloneArrangement.cpp`: placement and track truth.
- `Source/ARA/VST3AraSession.cpp`: ARA source/region/binding truth.

**ARA Bridge:**
- `Source/ARA/OpenTuneDocumentController.cpp`: host callback bridge.
- `Source/ARA/OpenTunePlaybackRenderer.cpp`: host playback rendering bridge.

**Policy And Preferences:**
- `Source/Utils/AppPreferences.h`: shared and standalone-only app preference schemas.
- `Source/Utils/AudioEditingScheme.h`: explicit editing-scheme rules.
- `Source/Utils/UndoAction.h`: undo result-chain types.

## Structure Of The Current UI Tree

**Shared in both editor headers:**
- `MenuBarComponent`
- `TransportBarComponent`
- `TopBarComponent`
- `ParameterPanel`
- `PianoRollComponent`
- `AutoRenderOverlayComponent`

**Only in Standalone editor header:**
- `TrackPanelComponent`
- `ArrangementViewComponent`
- `RippleOverlayComponent`
- `AsyncAudioLoader`
- `PresetManager`

**Only in VST3 editor header:**
- direct dependency on `ARA/VST3AraSession.h`
- no arrangement or track-panel widgets

## Verified Persistence And Data-Shape Files

- `Source/PluginProcessor.cpp` serializes processor state under `OpenTuneState`.
- `Source/PluginProcessor.cpp` writes separate `Contents` and `StandaloneArrangement` child trees.
- `Source/Utils/AppPreferences.cpp` persists app-level settings outside processor state.

## Naming And Placement Conventions

**Files:**
- Most implementation pairs use `PascalCase`: `SourceStore.cpp`, `MaterializationStore.h`, `StandaloneArrangement.h`, `OpenTunePlaybackRenderer.cpp`.
- Format boundaries are encoded by directory, so both products still use the generic file name `PluginEditor.cpp` in different directories.
- UI widget files consistently end in `Component`, `LookAndFeel`, `Theme`, `Renderer`, `Worker`, or `Support`.

**Directories:**
- Product boundaries are top-level folders under `Source/`: `Standalone/`, `Plugin/`, `ARA/`.
- Shared technical domains are separate peer folders: `Audio/`, `DSP/`, `Inference/`, `Services/`, `Utils/`, `Editor/`, `Host/`.
- Piano-roll internals are the only deeper feature subtree under shared UI: `Source/Standalone/UI/PianoRoll/`.

## Where Code Belongs

**Shared runtime orchestration:**
- Put only true cross-owner coordination in `Source/PluginProcessor.*`.

**Source identity / provenance ownership:**
- Put it in `Source/SourceStore.*`.

**Materialization payload or render-queue ownership:**
- Put it in `Source/MaterializationStore.*`.

**Standalone timeline, selection, or track mix state:**
- Put it in `Source/StandaloneArrangement.*`.

**VST3 ARA source, region, or binding state:**
- Put it in `Source/ARA/VST3AraSession.*`.

**Reusable UI widget:**
- Put it in `Source/Standalone/UI/` or `Source/Standalone/UI/PianoRoll/`.

**Standalone-only shell workflow:**
- Put it in `Source/Standalone/PluginEditor.*`.

**VST3-only shell workflow:**
- Put it in `Source/Plugin/PluginEditor.*`.

**Preference page composition:**
- Put shared pages in `Source/Editor/Preferences/SharedPreferencePages.*`.
- Put standalone-only pages in `Source/Editor/Preferences/StandalonePreferencePages.*`.

**Tests:**
- Put them under `Tests/` and add them to `OpenTuneTests` in `CMakeLists.txt`.

## Practical Repo Rules From Current Structure

- Do not add new production source files without updating `CMakeLists.txt`.
- Keep VST3 shell code under `Source/Plugin/` and Standalone shell code under `Source/Standalone/`; the current repository already relies on this compile-time split.
- Keep app preferences in `Source/Utils/AppPreferences.*`, not in processor state files.
- Reuse shared UI from `Source/Standalone/UI/` rather than copying widgets into `Source/Plugin/`.
- Treat `SourceStore`, `MaterializationStore`, `StandaloneArrangement`, and `VST3AraSession` as separate owner files when adding or documenting state.

---

*Structure analysis: 2026-04-20*
