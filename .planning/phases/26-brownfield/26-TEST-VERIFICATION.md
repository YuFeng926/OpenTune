# Phase 26: Legacy Cleanup And Brownfield Guard

## Purpose

Phase 26 removes the last legacy ARA source-level helper surface and orphan artifact left behind after the Phase 23-25 snapshot convergence, then proves with brownfield guards that the cleanup did not expand the shared processor core contract and did not leak VST3/ARA cleanup semantics into the Standalone shell.

This document is the verification source of truth for Phase 26. The phase is not complete when old symbols merely disappear: it must also prove that `OpenTuneDocumentController` no longer exposes source-level helper/projection APIs, renderer/editor consumers still stay on snapshot and preferred-region truth, `PluginProcessor.h` keeps the current import/replace/playback API boundary, and `Source/Standalone/PluginEditor.cpp` stays free of ARA cleanup symbols.

## Verification Levels

### L1 Build Gate

- Command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase26-docs -Target OpenTuneTests`
- Goal: prove the Phase 26 cleanup guards integrate cleanly on a fresh test binary before GREEN evidence is accepted.

### L2 Cleanup Guard Run

- Command: `& ".\build-phase26-docs\OpenTuneTests.exe"`
- Goal: execute the focused Phase 26 cleanup and brownfield guards in `runPhase26CleanupBrownfieldTests()`.

### L4 Static Audit Checklist

- Audit together: `AudioSourceState.h`, `getAudioSourceContentRevision`, `getAudioSourceMappingRevision`, `getAudioSourceClipBinding`, `getPlaybackRegionClipBinding`, `findAudioSourceByClipId`, `findProjectedRegionForAudioSource`, `findFirstRegionForAudioSource`, `registerPlaybackRegionClipBinding`, `snapshot->epoch`, `readPlaybackAudio`, `getPluginPlaybackReadSource`, `Source/Standalone/PluginEditor.cpp`
- Goal: prove Phase 26 deleted the orphan and source-level helper residue while preserving snapshot-only consumers, the shared processor boundary, and Standalone isolation.

### L6 Full Regression Run

- Command: `& ".\build-phase26-docs\OpenTuneTests.exe"`
- Goal: keep Phase 26 on the same executable regression path as the focused cleanup guards.

## Required Journeys

- `CLEAN_01_RemovesLegacyAudioSourceStateArtifact`
- `CLEAN_01_RemovesLegacySourceLevelControllerHelpers`
- `CLEAN_02_ConsumerAndGrepZeroGuardsStayCleanAfterCleanup`
- `CLEAN_03_SharedProcessorBoundaryAndStandaloneIsolationHold`

## L4 Static Audit Checklist

- `Source/ARA/AudioSourceState.h` is deleted from the repository and removed from `CMakeLists.txt` target sources.
- `Source/ARA/OpenTuneDocumentController.h` and `Source/ARA/OpenTuneDocumentController.cpp` are grep-zero for `getAudioSourceContentRevision`, `getAudioSourceMappingRevision`, `getAudioSourceClipBinding`, `getPlaybackRegionClipBinding`, `findAudioSourceByClipId`, `findProjectedRegionForAudioSource`, and `findFirstRegionForAudioSource`.
- `OpenTunePlaybackRenderer.cpp` still resolves playback from one `loadSnapshot()` in `processBlock()` and stays grep-zero for legacy source-level playback fallbacks.
- `PluginEditor.cpp` still uses `resolvePreferredAraRegionView(*snapshot)`, `binding.appliedRegion`, and `registerPlaybackRegionClipBinding`, and stays grep-zero for deleted source-level helper usage.
- `PluginProcessor.h` still exposes `readPlaybackAudio`, `getPluginPlaybackReadSource`, and the existing import/replace/playback mutation APIs without adapter-side types such as `PublishedSnapshot`, `AudioSourceClipBinding`, `RegionSlot`, `SourceSlot`, or `ARAPlaybackRegion`.
- `Source/Standalone/PluginEditor.cpp` stays grep-zero for `loadSnapshot`, `registerPlaybackRegionClipBinding`, `updatePlaybackRegionBindingRevisions`, `clearPlaybackRegionClipBinding`, `RegionIdentity`, and `PublishedSnapshot`.

## Evidence To Capture

- Orphan file deletion: `Source/ARA/AudioSourceState.h` is gone and `CMakeLists.txt` no longer references it.
- Controller helper grep-zero: the legacy source-level helper/projection surface is absent from `OpenTuneDocumentController.h/.cpp`.
- Renderer/editor consumer contract: `OpenTunePlaybackRenderer.cpp` remains snapshot-only and `PluginEditor.cpp` remains preferred-region-only.
- Shared processor boundary: `PluginProcessor.h` still preserves the current import, replace, and playback APIs and does not absorb adapter-side cleanup types.
- Standalone shell isolation: `Source/Standalone/PluginEditor.cpp` does not contain snapshot cleanup symbols.

## RED/GREEN Evidence Log

### RED Baseline

- Status: Observed during Phase 26 Plan 01 RED run
- L1 command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase26-docs -Target OpenTuneTests`
- L1 result:
  - `Build files have been written to: E:/TRAE/OPenTune0427/build-phase26-docs`
  - `OpenTuneTests.exe` rebuilt successfully with the new Phase 26 guards.
- L2 command: `& ".\build-phase26-docs\OpenTuneTests.exe"`
- L2 key result:
  - `[FAIL] CLEAN_01_RemovesLegacyAudioSourceStateArtifact: Source/ARA/AudioSourceState.h still exists in the repository`
- Note: the RED baseline proves the new cleanup guards execute on the live tree and immediately catch the orphan artifact before any production cleanup is applied.

### GREEN Verification

- Status: PASS (2026-04-16)
- L1 command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase26-docs -Target OpenTuneTests`
- L1 result:
  - `Build files have been written to: E:/TRAE/OPenTune0427/build-phase26-docs`
  - `ninja: no work to do.`
- L2 command: `& ".\build-phase26-docs\OpenTuneTests.exe"`
- L2 key results:
  - `[PASS] CLEAN_01_RemovesLegacyAudioSourceStateArtifact`
  - `[PASS] CLEAN_01_RemovesLegacySourceLevelControllerHelpers`
  - `[PASS] CLEAN_02_ConsumerAndGrepZeroGuardsStayCleanAfterCleanup`
  - `[PASS] CLEAN_03_SharedProcessorBoundaryAndStandaloneIsolationHold`
- L6 command: `& ".\build-phase26-docs\OpenTuneTests.exe"`
- L6 key results:
  - `[PASS] CLEAN_01_RemovesLegacyAudioSourceStateArtifact`
  - `[PASS] CLEAN_01_RemovesLegacySourceLevelControllerHelpers`
  - `[PASS] CLEAN_02_ConsumerAndGrepZeroGuardsStayCleanAfterCleanup`
  - `[PASS] CLEAN_03_SharedProcessorBoundaryAndStandaloneIsolationHold`
- L4 result:
  - `Source/ARA/AudioSourceState.h` is absent and `CMakeLists.txt` no longer references it.
  - `Source/ARA/OpenTuneDocumentController.h` and `Source/ARA/OpenTuneDocumentController.cpp` are grep-zero for `getAudioSourceContentRevision`, `getAudioSourceMappingRevision`, `getAudioSourceClipBinding`, `getPlaybackRegionClipBinding`, `findAudioSourceByClipId`, `findProjectedRegionForAudioSource`, and `findFirstRegionForAudioSource`.
  - `OpenTunePlaybackRenderer.cpp` still contains one `loadSnapshot()` in `processBlock()` and keeps `findRenderableRegionView(*snapshot, region)` as the region-truth lookup.
  - `PluginEditor.cpp` still contains `resolvePreferredAraRegionView(*snapshot)`, `binding.appliedRegion`, and `registerPlaybackRegionClipBinding`, while staying grep-zero for deleted source-level helper usage.
  - `PluginProcessor.h` still exposes `prepareImportClip`, `commitPreparedImportClip`, `readPlaybackAudio`, `getPluginPlaybackReadSource`, `setClipStartSecondsById`, `setPluginClipStartSeconds`, `replaceClipAudioById`, `enqueuePartialRenderById`, and `enqueuePluginClipPartialRender`, and remains grep-zero for adapter-side cleanup types.
  - `Source/Standalone/PluginEditor.cpp` stays grep-zero for `loadSnapshot`, `registerPlaybackRegionClipBinding`, `updatePlaybackRegionBindingRevisions`, `clearPlaybackRegionClipBinding`, `RegionIdentity`, and `PublishedSnapshot`.

## Exit Condition For Phase 26

- orphan ARA source artifact is deleted from the repository and build graph.
- source-level helper and projection helper APIs are deleted from `OpenTuneDocumentController`.
- renderer and VST3 editor consumers do not regress to source-level fallback.
- shared processor API and Standalone UI isolation boundaries remain unchanged.

This verification source now serves as the single closure artifact for execute-phase, roadmap/state updates, and v2.2 brownfield milestone review.
