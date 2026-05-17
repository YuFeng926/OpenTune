# VST3 ARA Multi-Region Test Verification

**Date:** 2026-05-15
**Status:** Automated layers executed for current implementation; L5 Reaper journey pending
**Design:** `.planning/plans/2026-05-15-vst3-ara-multi-region-standard-fix-plan.md`

## Rule

No completed level may be described as PASS unless it has been run in the current implementation context. Host-level L5 remains manual and pending.

## L0 - Spec Completeness

**Purpose:** Confirm the design traces to official ARA object ownership.

Checks:

- The design states `AudioSource -> SourceStore`, `AudioModification -> MaterializationStore`, `PlaybackRegion -> projection`.
- The design rejects source-window reuse as default editable-state sharing.
- The design treats `preferredRegion` as UI focus only.
- The design includes ARA archive store/restore.

Current result: PASS by review. The implemented model maps `AudioSource -> SourceStore`, `AudioModification persistentID -> AraMaterializationBinding/materializationId`, and `PlaybackRegion -> projection`.

## L1 - Static Validation

**Purpose:** Ensure no banned old ownership patterns remain.

Commands:

```powershell
rg -n "findMaterializationBySourceWindow\\(|source\\+window|preferredRegion_.*materialization|araClipImportArmed_.*setEditedMaterialization" Source Tests .planning
rg -n "doRestoreObjectsFromStream|doStoreObjectsToStream|storeMaterializationBindings|restoreMaterializationBindings" Source/ARA Tests
rg -n "ARAPlaybackRegion\\*.*persistent|reinterpret_cast<.*ARA|void\\*.*playbackRegion" Source Tests
git diff --check
```

Required result:

- No ARA default birth path reuses editable materialization by only `sourceId + sourceWindow`.
- No persistent state writes raw `ARAPlaybackRegion*`.
- `doRestoreObjectsFromStream()` and `doStoreObjectsToStream()` are no longer empty stubs.
- `araClipImportArmed_` is not a display gate for already-bound materializations.

Current result:

- Static scan still intentionally finds test fake host pointers such as `reinterpret_cast<juce::ARAPlaybackRegion*>` in probe-based tests; those are non-persistent test identities, not archive state.
- `doRestoreObjectsFromStream()` / `doStoreObjectsToStream()` now forward to binding archive store/restore.

## L2 - Unit Tests

**Purpose:** Verify the binding model without a DAW.

Required tests:

- `AraBinding_NewPersistentIdSameSourceWindowCreatesIndependentMaterialization`
  - Arrange two audio modification identities with the same source and same source window.
  - Expect two distinct materialization IDs.

- `AraBinding_MultiplePlaybackRegionsSameAudioModificationShareMaterialization`
  - Arrange one audio modification identity with two playback regions.
  - Expect both region views bind to the same materialization ID.

- `AraBinding_RestoredPersistentIdRebindsNewPlaybackRegion`
  - Restore a binding table by audio modification persistent ID.
  - Add a new playback region pointer for that modification.
  - Expect the new region view to bind to the restored materialization.

- `AraEditor_AttachesRenderableBindingWithoutReadAudioArm`
  - Construct editor state with a renderable ARA binding.
  - Expect PianoRoll materialization attachment without requiring `araClipImportArmed_`.

Execution command:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

Current result:

- `OpenTuneTests` Release build in `build-ara-overlay-vs18-clean` passed after this doc/test cleanup.
- `architecture` suite passed after this doc/test cleanup, including `AraBinding_RestoredPersistentIdRebindsNewPlaybackRegion`.
- `core`, `processor`, `memory`, and `undo` suites passed before this doc/test cleanup and were not touched by the cleanup.

## L3 - Integration Tests

**Purpose:** Verify session, processor stores, renderer, and editor projection interact correctly.

Required tests:

- Session publishes all playback regions with stable binding states after hydration.
- Renderer processes two overlapping assigned playback regions and reads both materializations.
- Editing one materialization changes only regions bound to its audio modification.
- Removing one playback region does not retire a materialization still referenced by another region from the same audio modification.
- Destroying an audio modification retires only its bound materialization when no undo/lineage references remain.

Execution command:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture processor
```

Current result:

- Renderer and session contract checks are covered by `architecture`; processor owner checks are covered by `processor`.
- There is no separate `ara` suite in the current runner.

## L4 - Contract Tests

**Purpose:** Guard official ARA semantics.

Required checks:

- `AudioModification` persistent ID is copied during ARA callback handling.
- Binding table keys do not use transient `ARAPlaybackRegion*`.
- Archive format is versioned and rejects unknown future versions clearly.
- `PlaybackRegion` projection changes update `RegionSlot` without creating new editable materialization.
- ARA archive restore can run before playback regions are recreated, and later region add callbacks rebind correctly.

Execution command:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

Current result:

- The architecture suite is the required gate for ARA binding/static contract guards and passed after this doc/test cleanup.

## L5 - Manual Reaper Journey

**Purpose:** Confirm the real host behavior that unit tests cannot simulate.

Environment:

- Windows 11
- Reaper 7.72 or newer
- ARA VST3 build
- Project from issue #6 if available

Journeys:

1. **Multiple Items Same Source**
   - Put at least two Reaper items from the same audio source on one track.
   - Open OpenTune ARA.
   - Confirm logs show two playback regions and stable audio modification IDs.
   - Read/focus item A and item B separately.
   - Expected: each independently editable item has its own materialization unless host reports they are the same audio modification alias.

2. **Concurrent Playback**
   - Overlap two ARA items in playback time.
   - Play through the overlap.
   - Expected: both assigned playback regions render; no last-item-only behavior.

3. **Editor Reopen**
   - Read/refresh F0 for two items.
   - Close the plug-in window and reopen it.
   - Expected: focusing each item reattaches its existing F0/PianoRoll state without recalculation.

4. **Project Save Reload**
   - Save the Reaper project.
   - Close and reopen Reaper/project.
   - Expected: restored regions rebind to restored materializations by persistent audio modification ID.

Required evidence:

- AppLogger trace for region count, audio modification persistent IDs, source windows, materialization IDs, binding states, and focused region.
- Screenshot or short screen recording of item A/item B showing distinct restored F0 where expected.

User confirmation required:

```text
用户旅程 "Reaper ARA multi-item restore" 已完成验证。
证据: [logs/screenshots]
请确认是否符合您的预期？(符合/不符合)
```

## L6 - Regression Suite

**Purpose:** Ensure Standalone and non-ARA VST3 behavior remains isolated.

Required checks:

- Targeted `OpenTuneTests` suites PASS for the changed area.
- Standalone target builds PASS.
- ARA VST3 target builds PASS.
- non-ARA VST3 target builds PASS.
- Standalone import/split/delete materialization ownership remains unchanged.
- non-ARA capture does not call ARA DocumentController paths.

Build command shape:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```

Current result:

- ARA `OpenTuneTests` build: PASS after doc/test cleanup.
- ARA VST3 build: PASS before doc/test cleanup.
- non-ARA VST3 build: PASS before doc/test cleanup.
- `OpenTuneTests.exe ui` currently exits 1 after printing `[PASS] PianoRoll_DrawNoteDraft_SurvivesMultiEventDrag` and no `[FAIL]` text; do not claim full-suite PASS until this runner exit behavior is fixed or explained.
- Standalone target was not re-run as part of the ARA multi-region implementation verification.
- Avoid parallel MSBuild invocations in the same build directory; a prior parallel run hit a `.tlog` permission lock and passed after serial rerun.
