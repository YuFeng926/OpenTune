# Phase 24: Callback-Driven Sample Access Lifecycle

## Purpose

Phase 24 closes the gap between the Phase 23 snapshot model and the official ARA callback lifecycle. Sample access, content dirty state, snapshot publication, and stale-truth removal must all follow host-driven callback ownership instead of eager reads, delayed retry, or editor-side cleanup.

This document is the verification source of truth for Phase 24. The phase is not complete when the controller merely compiles: it must prove that `didUpdateAudioSourceProperties()` no longer reads host audio, sample-access enable callbacks are the only place allowed to copy samples, content changes publish only after `didEndEditing()`, and removal callbacks purge stale snapshot truth immediately.

## Verification Levels

### L1 Build Gate

- Command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase24-docs -Target OpenTuneTests`
- Goal: prove the Phase 24 lifecycle contract integrates cleanly before RED/GREEN evidence is accepted.

### L2 Lifecycle Guard Run

- Command: `& ".\build-phase24-docs\OpenTuneTests.exe"`
- Goal: execute the focused Phase 24 lifecycle guards in `runPhase24LifecycleCallbackTests()`.

### L4 Static Audit Checklist

- Audit together: `didUpdateAudioSourceProperties`, `doUpdateAudioSourceContent`, `willEnableAudioSourceSamplesAccess`, `didEnableAudioSourceSamplesAccess`, `willRemovePlaybackRegionFromAudioModification`, `willDestroyPlaybackRegion`, `willDestroyAudioSource`, `copyAudioSourceSamples`, `juce::Timer::callAfterDelay`, `sampleAccessRetryCount`
- Goal: prove the controller follows callback-driven sample access and that eager-read / retry residue is gone from the lifecycle path.

### L6 Full Regression Run

- Command: `& ".\build-phase24-docs\OpenTuneTests.exe"`
- Goal: keep the lifecycle contract on the same executable verification path used by the focused guards.

## Required Journeys

- `LIFE_01_DidUpdateAudioSourcePropertiesStopsEagerReadAndRetry`
- `LIFE_02_SampleAccessLifecycleOnlyUsesEnableCallbacks`
- `LIFE_03_ContentDirtyPublishesAtDidEndEditing`
- `LIFE_04_RemovalPurgesStaleSnapshotTruth`

## L4 Static Audit Checklist

- `didUpdateAudioSourceProperties()` no longer directly reads host audio and no longer owns retry behavior.
- `doUpdateAudioSourceContent()` only marks content-dirty lifecycle state; it does not copy samples and does not publish snapshots directly.
- `willEnableAudioSourceSamplesAccess()` / `didEnableAudioSourceSamplesAccess()` are the only lifecycle callbacks allowed to disable or enable copied-audio ownership.
- `willRemovePlaybackRegionFromAudioModification()`, `willDestroyPlaybackRegion()`, and `willDestroyAudioSource()` are wired to stale-truth purge rather than deferred editor cleanup.
- `copyAudioSourceSamples()` is the only helper that performs host sample copy for the controller lifecycle.
- `juce::Timer::callAfterDelay` must be grep-zero in the controller lifecycle path.
- `sampleAccessRetryCount` must be grep-zero in the controller lifecycle path.

## Evidence To Capture

- `didUpdateAudioSourceProperties()` no longer directly reads audio.
- Sample access enable/disable is the only time copied audio can be created or cleared.
- Content dirty state is held until `didEndEditing()` publishes a new snapshot.
- Region/source removal immediately removes stale truth from the published snapshot.

## RED/GREEN Evidence Log

### RED Baseline

- Status: Observed during Wave 2 and Wave 3
- RED failure 1: `[FAIL] LIFE_02_SampleAccessLifecycleOnlyUsesEnableCallbacks: didEnableAudioSourceSamplesAccess does not establish source lifecycle state before copying`
- RED failure 2: `[FAIL] LIFE_04_RemovalPurgesStaleSnapshotTruth: shared removal helpers are not wired in controller cpp`
- Note: Both RED runs came from `build-phase24-docs` + `OpenTuneTests.exe`, proving the new lifecycle guards caught real gaps before the controller was updated.

### GREEN Verification

- Status: PASS (2026-04-16)
- L1 command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase24-docs -Target OpenTuneTests`
- L1 result:
  - `Build files have been written to: E:/TRAE/OPenTune0427/build-phase24-docs`
  - `ninja: no work to do.`
- L2 command: `& ".\build-phase24-docs\OpenTuneTests.exe"`
- L2 key results:
  - `[PASS] LIFE_01_DidUpdateAudioSourcePropertiesStopsEagerReadAndRetry`
  - `[PASS] LIFE_02_SampleAccessLifecycleOnlyUsesEnableCallbacks`
  - `[PASS] LIFE_03_ContentDirtyPublishesAtDidEndEditing`
  - `[PASS] LIFE_04_RemovalPurgesStaleSnapshotTruth`
  - `[PASS] LIFE_04_AudioSourceRemovalPurgesDependentRegions`
- L6 command: `& ".\build-phase24-docs\OpenTuneTests.exe"`
- L6 key results:
  - `[PASS] LIFE_01_DidUpdateAudioSourcePropertiesStopsEagerReadAndRetry`
  - `[PASS] LIFE_02_SampleAccessLifecycleOnlyUsesEnableCallbacks`
  - `[PASS] LIFE_03_ContentDirtyPublishesAtDidEndEditing`
  - `[PASS] LIFE_04_RemovalPurgesStaleSnapshotTruth`
  - `[PASS] LIFE_04_AudioSourceRemovalPurgesDependentRegions`
- L4 result:
  - `Source/ARA/OpenTuneDocumentController.h` and `Source/ARA/OpenTuneDocumentController.cpp` are grep-zero for `Timer::callAfterDelay` and `sampleAccessRetryCount`
  - `didUpdateAudioSourceProperties()` stays metadata-only, `doUpdateAudioSourceContent()` does not call `publishSnapshot()`, and `didEnableAudioSourceSamplesAccess()` is the only copy entrypoint
  - `willRemovePlaybackRegionFromAudioModification()`, `willDestroyPlaybackRegion()`, and `willDestroyAudioSource()` all use shared removal helpers plus immediate `publishSnapshot()`

## Exit Condition For Phase 24

- `didUpdateAudioSourceProperties()` is metadata-only.
- Sample access lifecycle is owned by `willEnableAudioSourceSamplesAccess()` / `didEnableAudioSourceSamplesAccess()`.
- Content changes publish only at `didEndEditing()`.
- Removal callbacks purge stale snapshot truth immediately.
