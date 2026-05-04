# Phase 18 Test Verification

## Scope

Phase 18 focuses on UI timeline projection correctness. The piano roll playhead, auto-scroll, and clip-focus baseline must consume sample-authoritative clip projections, while the Standalone arrangement timeline must derive clip spans from the same shared sample->time projection contract used by the shared core.

## Verification Levels

| Level | Goal | Command | Status | Pass Criteria |
| --- | --- | --- | --- | --- |
| L1 | Build `OpenTuneTests` against the Phase 18 code | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build -Target OpenTuneTests` | Passed | Build completed successfully after the new Processor / PianoRoll / ArrangementView code landed. Only pre-existing deprecation and alignment warnings remained. |
| L2 | Run the regression binary with the new Phase 18 timeline tests | `& ".\build\OpenTuneTests.exe"` | Passed | The suite printed `Phase 18: Timeline Projection Tests`, and all four Phase 18 cases passed in the final run. |
| L4 | Static contract audit for shared projection wiring | `rtk grep "ClipTimelineProjection|ClipPlayheadProjection|getClipTimelineProjectionById|projectClipPlayheadById|buildProjectedClipBounds|readProjectedPlayheadTime" "Source/PluginProcessor.h" "Source/PluginProcessor.cpp" "Source/Standalone/UI/PianoRollComponent.cpp" "Source/Standalone/UI/ArrangementViewComponent.cpp" "Tests/TestMain.cpp"` | Passed | Shared projection structs/helpers exist, PianoRoll consumes projected playhead time, ArrangementView consumes projected clip bounds, and Phase 18 tests reference the helpers directly. |
| L6 | Re-run the full regression suite after all fixes | `& ".\build\OpenTuneTests.exe"` | Passed | Reused the fresh final full-suite run after the desktop-host test fix; no failing suites remained. |

## Required Journeys

- `TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead`
- `TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead`
- `TIME_02_ClipTimelineProjectionUsesStoredSampleSpan`
- `TIME_03_ClipPlayheadProjectionClampsToStoredSampleSpan`

## L5 Applicability

- L5: Not applicable.
- Reason: Phase 18 changes native JUCE timeline projection semantics and regression coverage is provided through `OpenTuneTests` plus static contract audit. The repository still has no executable UI E2E harness for these views.

## Gate Policy

- No test may be marked skipped by default.
- If any blocking command cannot run, execution must stop and report the exact gate, reason, and impact.
- `18-VERIFICATION.md` must map `TIME-01`, `TIME-02`, and `TIME-03` to concrete test names or static audit evidence.

## Evidence Captured

- Shared-core projection contract now exists in `Source/PluginProcessor.h` / `Source/PluginProcessor.cpp`:
  - `ClipTimelineProjection`
  - `ClipPlayheadProjection`
  - `getClipTimelineProjectionById(...)`
  - `projectClipPlayheadById(...)`
- `PianoRollComponent` now reads projected playhead time through `readProjectedPlayheadTime()` and reuses clip projection for fit-to-screen duration.
- `ArrangementViewComponent` now builds clip rectangles through `buildProjectedClipBounds(...)` and removes duplicate auto-scroll pixel math based on raw `100.0 * zoomLevel_` formulas.
- The first Phase 18 test run exposed a real verification issue: `onScrollVBlankCallback()` requires `isShowing()`, so a plain unattached test component never entered the VBlank auto-scroll path.
- The regression fix was structural and test-only: attach the PianoRoll test host to a desktop peer before executing the VBlank callback. No production code was changed for that issue.

## Final Gate

- PASS: L1, L2, L4, and L6 all passed with fresh evidence.
- PASS: `TIME-01`, `TIME-02`, and `TIME-03` now have direct automated or static-contract proof.
