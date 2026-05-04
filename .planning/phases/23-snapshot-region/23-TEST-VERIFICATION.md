# Phase 23: Snapshot Model And Region Truth Guards

## Purpose

Phase 23 first freezes the controller snapshot contract before reducer, publish gate, renderer, and editor migration continue. Wave 1 is not allowed to break the tree and defer repair to later plans: even when this wave only locks `OpenTuneDocumentController` into a snapshot-backed contract, it must preserve a compilable tree through a snapshot-backed transition shell and the L1 build gate.

This document is the Phase 23 verification source of truth for RED/GREEN execution. The phase is not complete when code merely compiles: the contract must prove single-snapshot publication, region-level truth preservation, immutable read-side migration, and strict Phase 23 scope control.

## Verification Levels

### L1 Build Gate

- Command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase23-docs -Target OpenTuneTests`
- Goal: prove the Phase 23 controller/test/read-side contract integrates cleanly and keeps the tree compiling before RED/GREEN evidence is accepted.

### L2 Snapshot Guard Run

- Command: `& ".\build-phase23-docs\OpenTuneTests.exe"`
- Goal: execute the focused Phase 23 snapshot/reducer guards in `runPhase23SnapshotRegionTests()` and capture RED/GREEN journey evidence.

### L4 Static Audit Gate

- Required symbols that must be audited together: `loadSnapshot`, `PublishedSnapshot`, `PublishedRegionView`, `preferredRegion`, `didEndEditing`, `getAudioSourceState`, `getPlaybackRangeForAudioSource`, `getSourceRangeForAudioSource`, `getCurrentPlaybackAudioSource`
- Goal: prove the public/read-side contract is snapshot-only, the old mutable/source-level getters are not the truth surface anymore, and renderer/editor acceptance is based on snapshot-only reads rather than compile-follow.

### L6 Full Regression Run

- Command: `& ".\build-phase23-docs\OpenTuneTests.exe"`
- Goal: keep the Phase 23 contract and renderer/editor snapshot-only migration on the same executable verification path.

## Required Journeys

- `SNAP_01_ControllerPublishesSingleSnapshotAtEditBatchEnd`
  - After one ARA edit batch completes, controller readers observe one coherent published snapshot rather than a mixed read across mutable tables.
- `SNAP_02_ControllerPreservesMultipleRegionsPerAudioSource`
  - Multiple `ARAPlaybackRegion` instances under the same `ARAAudioSource` remain distinct region truths; no source-level single-slot overwrite is allowed.
- `SNAP_03_ControllerReadSideNoLongerExposesMutableAudioSourceCarrier`
  - Public read-side contract no longer relies on `AudioSourceState*`, source-level range getters, or `currentPlaybackAudioSource_` as truth carriers.
- `SNAP_03_PreferredRegionIdentityChangesWithoutContentRewrite`
  - Preferred-region identity changes must be observable even when copied audio content and revisions do not require a content rewrite.

## L4 Static Audit Checklist

- `Source/ARA/OpenTuneDocumentController.h/.cpp` must expose and publish `loadSnapshot`, `PublishedSnapshot`, `PublishedRegionView`, `preferredRegion`, and `didEndEditing` as the Phase 23 read contract.
- `Source/ARA/OpenTunePlaybackRenderer.cpp` must read snapshot truth through `loadSnapshot`; passing compile without this migration does not count.
- `Source/Plugin/PluginEditor.cpp` timer/import read paths must read snapshot truth through `loadSnapshot`; passing compile without this migration does not count.
- `Source/ARA/OpenTunePlaybackRenderer.cpp` and `Source/Plugin/PluginEditor.cpp` must not read truth through `getAudioSourceState`, `getPlaybackRangeForAudioSource`, `getSourceRangeForAudioSource`, or `getCurrentPlaybackAudioSource`.
- `Source/ARA/OpenTunePlaybackRenderer.cpp` and `Source/Plugin/PluginEditor.cpp` must not introduce `appliedRegion`, epoch consumer UX, retry cleanup, sample-access lifecycle, or other out-of-phase logic.

## Evidence To Capture

- Same-source multi-region truth: when two playback regions share one audio source, the published snapshot still contains two distinct region truths.
- Preferred-region selection truth: changing the preferred region without rewriting content still changes snapshot region identity / selection truth.
- Immutable public read-side: controller-facing verification proves `AudioSourceState*` and similar mutable carriers are not exposed as read-side truth.
- Renderer/editor acceptance: `OpenTunePlaybackRenderer.cpp` and `Source/Plugin/PluginEditor.cpp` are accepted only when they read snapshot-only truth, not when they merely compile.
- Phase boundary control: `OpenTunePlaybackRenderer.cpp` and `Source/Plugin/PluginEditor.cpp` must not include `appliedRegion`, epoch consumer, retry cleanup, sample-access lifecycle, or removal-cleanup logic.

## Wave 1 Contract Rules

- Wave 1 may freeze only the controller contract, but the published read side must already be snapshot-backed.
- Any temporary compatibility symbol kept for compile-follow purposes must be a pure projection of the immutable snapshot, not an alternate mutable contract.
- `build-phase23-docs` is the required build directory for this phase’s documentation-backed gates.
- The shared processor boundary stays unchanged; no Phase 23 work may expand `Source/PluginProcessor.*` to work around adapter-state problems.

## RED/GREEN Evidence Log

### RED Baseline

- Status: Not observed on 2026-04-16
- Note: after `runPhase23SnapshotRegionTests()` landed, the current live tree already satisfied the new Phase 23 guards on first execution. No failing `OpenTuneTests` run occurred to capture, so Phase 23 evidence proceeds directly to GREEN documentation.

### GREEN Verification

- Status: PASS (2026-04-16)
- L1 command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase23-docs -Target OpenTuneTests`
- L1 result:
  - `Building CXX object CMakeFiles\OpenTuneTests.dir\Tests\TestMain.cpp.obj`
  - `Linking CXX executable OpenTuneTests.exe`
- L2 command: `& ".\build-phase23-docs\OpenTuneTests.exe"`
- L2 key results:
  - `[PASS] SNAP_01_ControllerPublishesSingleSnapshotAtEditBatchEnd`
  - `[PASS] SNAP_02_ControllerPreservesMultipleRegionsPerAudioSource`
  - `[PASS] SNAP_03_ControllerReadSideNoLongerExposesMutableAudioSourceCarrier`
  - `[PASS] SNAP_03_PreferredRegionIdentityChangesWithoutContentRewrite`
- L6 command: `& ".\build-phase23-docs\OpenTuneTests.exe"`
- L6 key results:
  - `[PASS] SNAP_01_ControllerPublishesSingleSnapshotAtEditBatchEnd`
  - `[PASS] SNAP_02_ControllerPreservesMultipleRegionsPerAudioSource`
  - `[PASS] SNAP_03_ControllerReadSideNoLongerExposesMutableAudioSourceCarrier`
  - `[PASS] SNAP_03_PreferredRegionIdentityChangesWithoutContentRewrite`

## Exit Condition For 23-01

- `OpenTuneDocumentController` exposes a snapshot-first read contract.
- Region identity is explicit in the published truth surface.
- L1 build gate passes before the phase proceeds to reducer and publish-gate work.
