# VST3 ARA Multi-Region Standard Fix Plan

**Date:** 2026-05-15
**Status:** Implemented in current workspace; L5 Reaper host journey pending
**Related issue:** https://github.com/YuFeng926/OpenTune/issues/6
**Verification source:** `.planning/plans/2026-05-15-vst3-ara-multi-region-test-verification.md`

## Goal

Fix the Reaper multi-item ARA workflow from the ARA model itself, not by adding another fallback around the current preferred-region behavior.

The target behavior is:

- Multiple DAW items/playback regions assigned to the plug-in can render concurrently.
- Each item that is edited independently owns an independent editable OpenTune materialization.
- The VST3 editor can focus one region for PianoRoll editing without changing playback truth for sibling regions.
- Closing/reopening the editor or project restores existing ARA edit state without forcing the user to re-run Read Audio.
- `preferredRegion` remains a UI focus hint only; it must not be the owner of import, render, persistence, or recovery state.

## Official ARA Model

Primary references:

- Celemony ARA API repository: https://github.com/Celemony/ARA_API
- Celemony ARA SDK repository: https://github.com/Celemony/ARA_SDK
- Local SDK version: `ThirdParty/ARA_SDK-releases-2.2.0`

The official model has four important layers:

| ARA object | Official role | OpenTune equivalent |
|---|---|---|
| `Document` | Root of the model graph and persistency owner | ARA document controller + shared stores |
| `AudioSource` | Original audio source | `SourceStore` / `sourceId` |
| `AudioModification` | Persistent user edit state for one source | `MaterializationStore` / `materializationId` |
| `PlaybackRegion` | Non-persistent placement/view mapping from modification time to playback time | `VST3AraSession::RegionSlot` / projection |

Key official constraints from the SDK:

- Model graph edits happen inside `beginEditing()` / `endEditing()` cycles, so plug-ins can publish a complete synchronized graph state after edits. See `ThirdParty/ARA_SDK-releases-2.2.0/ARA_API/ARAInterface.h:656`.
- `AudioModification` contains user edits, is owned by one `AudioSource`, owns any number of playback regions, and is persistent in document storage. See `ARAInterface.h:906`.
- `PlaybackRegion` maps an arbitrary section of an audio modification to playback time. Playback regions that share the same audio modification share the same musical content, and playback regions are not persistent; the host recreates them. See `ARAInterface.h:951`.
- `cloneAudioModification()` is the official mechanism for independent edit variations, explicitly contrasted with aliases made by merely adding playback regions to the same modification. See `ARAInterface.h:2794`.
- ARA 2.0 recommends playback-region-level content reading for most content types, because region transitions and borders can make region content differ from a simple transformed audio modification. See `ARAInterface.h:2919`.
- `restoreObjectsFromArchive()` / `storeObjectsToArchive()` are the official persistence hooks, with filters and partial archive support. See `ARAInterface.h:3089`.
- A playback renderer can have multiple playback regions assigned; if they overlap, they all sound concurrently. See `ARAInterface.h:3630`.

JUCE's ARA wrapper mirrors this model:

- `juce::ARADocumentControllerSpecialisation` requires `doRestoreObjectsFromStream()` and `doStoreObjectsToStream()` for persistent ARA archive data. See `JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARADocumentController.h:159`.
- JUCE exposes audio-modification and playback-region content separately; playback-region content access may delegate to modification state only when that is semantically valid. See `juce_ARADocumentController.h:259`.
- `ARAPlaybackRenderer::getPlaybackRegions()` returns the current assigned set of playback regions, matching the official concurrent-region contract. See `JUCE-master/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAPlugInInstanceRoles.h:142`.

## Current OpenTune Reality

The current live tree now follows the official ARA owner split for the multi-item root cause:

- `VST3AraSession::RegionSlot` stores per-playback-region projection, applied materialization, and the parent `audioModificationPersistentId`. See `Source/ARA/VST3AraSession.h:149`.
- `VST3AraSession::AraMaterializationBinding` records `audioModificationPersistentId -> materializationId/sourceWindow/revision/duration`, owned by the ARA session instead of the editor. See `Source/ARA/VST3AraSession.h:168`.
- `materializationBindings_` is keyed by AudioModification persistent ID. Same AudioModification + multiple PlaybackRegions intentionally share one materialization; different persistent IDs with the same source window do not reuse by source/window equality.
- `PublishedSnapshot::publishedRegions` can publish multiple region views. See `Source/ARA/VST3AraSession.h:195`.
- `bindPlaybackRegionToMaterialization()` is already region-local. See `Source/ARA/VST3AraSession.h:256`.
- `OpenTunePlaybackRenderer::processBlock()` already iterates `getPlaybackRegions()` and reads each renderable region independently. See `Source/ARA/OpenTunePlaybackRenderer.cpp:180`.
- `OpenTuneDocumentController::doRestoreObjectsFromStream()` and `doStoreObjectsToStream()` forward to versioned session binding archive store/restore. See `Source/ARA/OpenTuneDocumentController.cpp:125`.
- `ensureAraRegionMaterialization()` no longer reuses editable materialization by `sourceId + sourceWindow`; new AudioModification identity births a new materialization.
- VST3 editor display no longer depends on an `araClipImportArmed_` display gate. A renderable existing binding can attach to PianoRoll during snapshot sync without re-running Read Audio.

Remaining validation gap:

- The automated contract/build layers are covered locally; the real Reaper multi-item/project-reload L5 journey is still pending and must not be treated as completed.

## Standard To Enforce

OpenTune should adopt the official ARA distinction directly:

1. **Source is provenance, not edit owner.**
   `AudioSource` maps to `sourceId` and copied/hydrated source audio only.

2. **Materialization is the editable modification.**
   Each independently editable ARA item maps to one `materializationId`. Notes, corrected F0, detected key, rendered audio, lineage, and source window live there.

3. **PlaybackRegion is placement/projection.**
   A playback region maps materialization-local time to DAW playback time. It is allowed to disappear and be recreated by the host; it is not the persisted edit owner.

4. **Alias must be explicit.**
   Sharing one materialization across multiple playback regions is only valid when OpenTune intentionally models ARA alias semantics. It must not happen as an optimization through `sourceId + sourceWindow` reuse.

5. **Focus is not truth.**
   `preferredRegion` may drive which region the editor displays, but it must not decide which regions exist, which regions render, which regions are persisted, or which restored materializations are valid.

## Implemented Repair

### 1. Introduce Stable ARA Binding Identity

Added a small persisted ARA binding record owned by the ARA adapter layer:

```cpp
struct AraMaterializationBinding
{
    juce::String audioModificationPersistentId;
    uint64_t sourceId;
    uint64_t materializationId;
    SourceWindow sourceWindow;
    uint64_t materializationRevision;
};
```

The stable key is `ARAAudioModificationProperties::persistentID`, not the transient `ARAPlaybackRegion*`. If a host does not provide a usable persistent ID, the session leaves the region unbound and the L5 Reaper journey must capture the actual host behavior before choosing a product policy.

This binding belongs with `OpenTuneDocumentController` / `VST3AraSession`, because it is ARA graph state. It should not leak into Standalone UI or become a shared-core fallback.

### 2. Bind Materialization To AudioModification, Project Through PlaybackRegion

On `didAddPlaybackRegionToAudioModification()` / `didUpdatePlaybackRegionProperties()` the session now:

- Copy the playback-region projection into `RegionSlot`.
- Resolve the parent `AudioModification` persistent ID.
- Find an existing `AraMaterializationBinding` for that modification ID.
- If found, bind this playback region to the bound `materializationId`.
- If not found and source audio is hydrated, queue/birth a new materialization for this audio modification and store the binding.
- Publish a new immutable snapshot.

This matches official semantics: the persistent edit owner is audio modification/materialization, while each playback region remains a non-persistent projection onto it.

### 3. Make Independent Item Edits The Default

Remove `sourceId + sourceWindow` reuse from ARA auto-birth as the default path.

The default must be:

- new ARA audio modification -> new materialization,
- cloned ARA audio modification -> cloned or newly restored materialization,
- multiple playback regions under the same audio modification -> shared materialization, because that is official alias behavior,
- different audio modifications with the same source window -> independent materializations.

If later the product needs explicit aliasing, add it as an intentional command and represent it directly in the binding table. Do not infer it from source/window equality.

### 4. Narrow Preferred-Only Read Audio

Read Audio still acts on the focused/preferred editor region, but it is no longer the owner of multi-region playback truth. Session binding and renderer publication now determine which regions exist and render.

Current minimum behavior:

- If the editor has a focused region, Read Audio ensures/imports that region's parent audio modification if needed and then requests derived refresh.
- If a region already has a renderable binding, editor sync attaches it without requiring Read Audio arm state.
- A future explicit "Read All ARA Regions" or region list remains product UI work, but renderer correctness no longer depends on that UI command.

The simpler product path is to make hydration/auto-birth session-driven and let Read Audio mean "refresh F0 for the currently focused materialization". Then item discovery and binding are not editor-owned.

### 5. Reattach Existing ARA Materialization On Editor Construction

The old editor-local `araClipImportArmed_` display gate has been removed.

New rule:

- If the current focused region has a renderable binding and the materialization payload exists, the editor attaches it to PianoRoll immediately.
- User-triggered creation/refresh is driven by `recordRequested()` and the session/processor binding API.
- Editor reconstruction must not clear persisted/restored state during snapshot sync.

### 6. Implement ARA Archive Persistence

Implemented `OpenTuneDocumentController::doStoreObjectsToStream()` and `doRestoreObjectsFromStream()` with a compact versioned binding archive:

- magic/version,
- `AraMaterializationBinding[]` keyed by audio modification persistent ID,
- materialization revision and materialization duration metadata,
- no transient `ARAPlaybackRegion*` values.

On restore:

- restore binding table by audio modification persistent ID,
- remap IDs through the ARA restore filter when provided,
- when host recreates playback regions, re-project them through `RegionSlot` and attach to the restored materialization,
- publish a snapshot only after the restored binding is coherent.

## Implementation Phases

### Phase 1 - Diagnostics And Contract Guards

- Status: implemented for automated contract guards.
- Added/kept guards for two persistent IDs from one source/window producing distinct materializations, two playback regions under one AudioModification sharing one materialization, editor attachment without Read Audio arm, and archive restore rebind.
- Trace/log evidence is still part of L5 host validation.

### Phase 2 - Binding Owner Move

- Status: implemented.
- `VST3AraSession` owns `AraMaterializationBinding` and `materializationBindings_`.
- Region slots derive applied projection from `audioModificationPersistentId + materializationBindings_ + playback-region properties`.
- ARA default materialization reuse by `sourceId + sourceWindow` was removed.

### Phase 3 - Editor Focus Cleanup

- Status: partially implemented.
- `preferredRegion` remains the editor focus hint; renderer/session truth is independent.
- Already-bound renderable materializations can attach during editor snapshot sync.
- `araClipImportArmed_` display gate was removed.
- Explicit region list / "Read All ARA Regions" UI is not implemented.

### Phase 4 - ARA Archive Persistence

- Status: implemented at binding-table level; host project reload still requires L5 verification.
- Versioned ARA archive store/restore now persists bindings by audio modification persistent ID.
- Automated restore test covers archive restore before host recreates a playback region.

### Phase 5 - Reaper L5 Journey

- Status: pending.
- Validate in Reaper 7.72+ with multiple items on the same track and the same source.
- Validate overlapping playback regions sound concurrently.
- Validate editing item A does not mutate item B unless they are the same audio modification alias.
- Validate editor close/reopen and project save/reload restore the correct F0/PianoRoll state.

## Non-Goals

- Do not introduce a fallback from ARA to non-ARA capture.
- Do not keep both preferred-region import and binding-table import as parallel production paths.
- Do not make source-window equality imply shared editable state.
- Do not store transient host object pointers in persistent state.
- Do not move Standalone placement behavior into VST3 ARA editor code.

## Success Criteria

- Renderer remains multi-region and snapshot-only.
- Every published renderable region has `appliedRegionIdentity == regionIdentity`.
- Same source + same source window + different audio modification creates distinct materializations.
- Same audio modification + multiple playback regions shares one materialization intentionally.
- Editor reconstruction displays already-bound ARA F0 without requiring Read Audio.
- ARA archive store/restore is non-empty, versioned, and covered by contract tests.
- Reaper multi-item workflow no longer depends on callback order or "last item wins" behavior. Pending L5 host confirmation.
