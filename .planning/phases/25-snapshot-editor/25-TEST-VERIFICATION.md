# Phase 25: Snapshot Consumers And Preferred-Region Sync

## Purpose

Phase 25 closes the remaining read-side gap between the Phase 23 snapshot model and the Phase 24 callback-driven lifecycle. The playback renderer and VST3 editor must now consume exactly one immutable snapshot contract: renderer reads one snapshot per audio block, editor sync decisions read snapshot epoch plus preferred-region truth, manual import reads only the preferred region view, and same-source preferred-region switches are surfaced through `binding.appliedRegion` instead of revision coincidence.

This document is the verification source of truth for Phase 25. The phase is not complete when the code merely compiles: it must prove that `processBlock()` loads one snapshot per block, `timerCallback()` and `syncImportedAraClipIfNeeded()` consume snapshot epoch plus preferred-region truth, `recordRequested()` imports only from the preferred region view, and `appliedRegion` exposes region-only sync truth even when content and mapping revisions stay unchanged.

## Verification Levels

### L1 Build Gate

- Command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase25-docs -Target OpenTuneTests`
- Goal: prove the Phase 25 snapshot-consumer contract integrates cleanly before RED/GREEN evidence is accepted.

### L2 Snapshot Consumer Guard Run

- Command: `& ".\build-phase25-docs\OpenTuneTests.exe"`
- Goal: execute the focused Phase 25 snapshot-consumer guards in `runPhase25SnapshotConsumerTests()`.

### L4 Static Audit Checklist

- Audit together: `processBlock`, `loadSnapshot`, `findRenderableRegionView`, `timerCallback`, `syncImportedAraClipIfNeeded`, `recordRequested`, `registerPlaybackRegionClipBinding`, `updatePlaybackRegionBindingRevisions`, `clearPlaybackRegionClipBinding`, `appliedRegion`, `snapshot->epoch`, `getCurrentPlaybackAudioSource`
- Goal: prove renderer/editor only consume immutable snapshot truth and that source-level fallback does not return through implementation drift.

### L6 Full Regression Run

- Command: `& ".\build-phase25-docs\OpenTuneTests.exe"`
- Goal: keep the snapshot-consumer contract on the same executable verification path used by the focused guards.

## Required Journeys

- `CONS_01_PlaybackRendererLoadsSnapshotOncePerBlock`
- `CONS_02_EditorTimerUsesSnapshotEpochAndPreferredRegionTruth`
- `CONS_03_RecordRequestedImportsPreferredRegionViewOnly`
- `CONS_04_PreferredRegionSwitchTriggersMappingSyncWithoutRevisionBump`

## L4 Static Audit Checklist

- `processBlock()` loads the document snapshot exactly once per audio block.
- Renderer published-view lookup stays on `findRenderableRegionView(*snapshot, region)`.
- `timerCallback()` keeps ARA clip sync ownership inside `syncImportedAraClipIfNeeded()`.
- `syncImportedAraClipIfNeeded()` consumes `snapshot->epoch`, `binding.appliedRegion`, `lastConsumedAraSnapshotEpoch_`, and `lastConsumedPreferredAraRegion_`.
- `recordRequested()` imports only from `resolvePreferredAraRegionView(*snapshot)` and the preferred region view's copied audio plus mapping.
- `registerPlaybackRegionClipBinding()`, `updatePlaybackRegionBindingRevisions()`, and `clearPlaybackRegionClipBinding()` remain the only binding mutation entry points.
- `appliedRegion` is the explicit truth for same-source preferred-region switch detection.
- `getCurrentPlaybackAudioSource` must stay grep-zero across the renderer and VST3 editor consumer path.

## Evidence To Capture

- Renderer reads exactly one snapshot per block and resolves playback truth directly from the host playback region.
- Editor timer sync decides only from snapshot epoch plus preferred-region truth.
- Manual import reads copied audio and mapping only from the preferred region view.
- When preferred region switches within the same source and `mappingRevision` stays unchanged, `appliedRegion` still exposes pending sync truth.

## RED/GREEN Evidence Log

### RED Baseline

- Status: Observed during Phase 25 TDD RED run
- L1 command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase25-docs -Target OpenTuneTests`
- L1 result:
  - `Build files have been written to: E:/TRAE/OPenTune0427/build-phase25-docs`
  - `OpenTuneTests.exe` rebuilt successfully for the new Phase 25 guards.
- L2 command: `& ".\build-phase25-docs\OpenTuneTests.exe"`
- L2 key results:
  - `[PASS] CONS_01_PlaybackRendererLoadsSnapshotOncePerBlock`
  - `[FAIL] CONS_02_EditorTimerUsesSnapshotEpochAndPreferredRegionTruth: editor sync path does not consume snapshot epoch plus preferred-region truth`
- L6 command: `& ".\build-phase25-docs\OpenTuneTests.exe"`
- L6 key results:
  - `[PASS] CONS_01_PlaybackRendererLoadsSnapshotOncePerBlock`
  - `[FAIL] CONS_02_EditorTimerUsesSnapshotEpochAndPreferredRegionTruth: editor sync path does not consume snapshot epoch plus preferred-region truth`
- Note: the RED baseline confirms the new Phase 25 guards are executable and that `syncImportedAraClipIfNeeded()` still lacks the epoch plus applied-region consumer contract required by later waves.

### GREEN Verification

- Status: PASS (2026-04-16)
- L1 command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase25-docs -Target OpenTuneTests`
- L1 result:
  - `Build files have been written to: E:/TRAE/OPenTune0427/build-phase25-docs`
  - `ninja: no work to do.`
- L2 command: `& ".\build-phase25-docs\OpenTuneTests.exe"`
- L2 key results:
  - `[PASS] CONS_01_PlaybackRendererLoadsSnapshotOncePerBlock`
  - `[PASS] CONS_02_EditorTimerUsesSnapshotEpochAndPreferredRegionTruth`
  - `[PASS] CONS_03_RecordRequestedImportsPreferredRegionViewOnly`
  - `[PASS] CONS_04_PreferredRegionSwitchTriggersMappingSyncWithoutRevisionBump`
- L6 command: `& ".\build-phase25-docs\OpenTuneTests.exe"`
- L6 key results:
  - `[PASS] CONS_01_PlaybackRendererLoadsSnapshotOncePerBlock`
  - `[PASS] CONS_02_EditorTimerUsesSnapshotEpochAndPreferredRegionTruth`
  - `[PASS] CONS_03_RecordRequestedImportsPreferredRegionViewOnly`
  - `[PASS] CONS_04_PreferredRegionSwitchTriggersMappingSyncWithoutRevisionBump`
- L4 result:
  - `OpenTunePlaybackRenderer.cpp` keeps exactly one `loadSnapshot()` inside `processBlock()` and resolves views with `findRenderableRegionView(*snapshot, region)`.
  - `PluginEditor.cpp` contains `snapshot->epoch`, `binding.appliedRegion`, `lastConsumedAraSnapshotEpoch_`, and `lastConsumedPreferredAraRegion_` in `syncImportedAraClipIfNeeded()`.
  - `OpenTunePlaybackRenderer.cpp` and `PluginEditor.cpp` are grep-zero for `getAudioSourceState`, `getPlaybackRangeForAudioSource`, `getSourceRangeForAudioSource`, and `getCurrentPlaybackAudioSource`.

## Exit Condition For Phase 25

- `OpenTunePlaybackRenderer` loads one snapshot per block and resolves truth by playback region.
- VST3 editor sync reads snapshot epoch plus preferred-region truth instead of source-level fallback.
- Manual import and timer sync both consume only the preferred region view.
- Same-source preferred-region switches trigger sync through `appliedRegion` even when revisions do not bump.
