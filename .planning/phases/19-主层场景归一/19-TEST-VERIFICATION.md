# Phase 19 Test Verification

## Scope

Phase 19 closes the PianoRoll single-scene-host contract. `PianoRollComponent` now owns the playhead in the main layer, scroll / zoom / viewport updates no longer synchronize a second PianoRoll overlay coordinate system, and fresh regression evidence is rebuilt from a clean build directory.

## Verification Levels

| Level | Goal | Command | Status | Pass Criteria |
| --- | --- | --- | --- | --- |
| L1 | Build `OpenTuneTests` against the Phase 19 code | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase19-docs -Target OpenTuneTests` | Passed | Fresh configure/build completes, resolves dependencies from `ThirdParty/`, and produces `build-phase19-docs/OpenTuneTests.exe`. |
| L2 | Run the regression binary with the Phase 19 main-layer tests | `& ".\build-phase19-docs\OpenTuneTests.exe"` | Passed | The suite prints `Phase 19: Main Layer Scene Tests`; all three `LAYER_*` regressions pass; `TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead` and `TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead` remain green. |
| L4 | Static contract audit for single-scene-host wiring | `rtk rg -n "playheadOverlay_|PlayheadOverlayComponent|drawPlayhead|runPhase19MainLayerSceneTests|LAYER_01_PianoRollHasNoPlayheadOverlayChild|LAYER_01_PianoRollContinuousScrollStillUsesProjectedPlayheadAfterOverlayRemoval|LAYER_02_PianoRollPageScrollStillUsesProjectedPlayheadAfterOverlayRemoval|TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead|TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead" "Source/Standalone/UI/PianoRollComponent.h" "Source/Standalone/UI/PianoRollComponent.cpp" "Source/Standalone/UI/PianoRoll/PianoRollRenderer.h" "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp" "Tests/TestMain.cpp"` | Passed | Static audit proves the old overlay symbols are gone from `PianoRollComponent`, renderer exposes `drawPlayhead(...)`, and `runPhase19MainLayerSceneTests()` remains wired into the regression binary alongside the retained Phase 18 guards. |
| L6 | Re-run the full regression suite after all fixes | `& ".\build-phase19-docs\OpenTuneTests.exe"` | Passed | The full suite passes on a second run with no new PianoRoll or timeline regressions. |

## Required Journeys

- `LAYER_01_PianoRollHasNoPlayheadOverlayChild`
- `LAYER_01_PianoRollContinuousScrollStillUsesProjectedPlayheadAfterOverlayRemoval`
- `LAYER_02_PianoRollPageScrollStillUsesProjectedPlayheadAfterOverlayRemoval`
- `TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead`
- `TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead`

## L5 Applicability

- L5: Not applicable.
- Reason: Phase 19 changes native JUCE component layering and main-layer paint semantics, but the repository still has no executable UI E2E harness for PianoRoll. Verification stays in `OpenTuneTests` plus static contract audit.

## Gate Policy

- No test may be marked skipped by default.
- If any blocking command cannot run, execution must stop and report the exact gate, reason, and impact.
- `19-VERIFICATION.md` must map `LAYER-01` and `LAYER-02` to direct tests or static audit evidence.
- Phase 18 projected-playhead regressions remain mandatory guards for every Phase 19 verification run.

## Evidence To Capture

- `PianoRollComponent` no longer contains `PlayheadOverlayComponent` or `playheadOverlay_`.
- `PianoRollRenderer` exposes `drawPlayhead(...)` as the main-layer playhead paint entry.
- `PianoRollComponent::paint()` draws the playhead inside the clipped content region shared with notes, waveform, and F0.
- Continuous scroll keeps projected playhead semantics after overlay removal.
- Page scroll keeps projected playhead semantics after overlay removal.

## Final Gate

- Passed: L1, L2, L4, and L6 all satisfy the Phase 19 closure criteria in `build-phase19-docs`.
- Passed: `LAYER-01` and `LAYER-02` now have fresh automated proof plus static wiring evidence.

## Execution Results

### L1 - Passed

- Command: `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build-phase19-docs -Target OpenTuneTests`
- Result: fresh configure/build completed and produced `build-phase19-docs\OpenTuneTests.exe`.
- Evidence:
  - Configure resolved ARA and ONNX Runtime from `ThirdParty/` and generated `build-phase19-docs` successfully.
  - Build compiled `Source/Standalone/UI/PianoRollComponent.cpp`, `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`, and `Source/Standalone/UI/PlayheadOverlayComponent.cpp` into the fresh target graph.
  - Ninja linked `build-phase19-docs\OpenTuneTests.exe`.
- Notes: the build emitted existing warnings (`LockFreeQueue` alignment padding, deprecated Standalone multi-track APIs, and existing local shadowing), but no blocking errors.

### L2 - Passed

- Command: `& ".\build-phase19-docs\OpenTuneTests.exe"`
- Result: the fresh regression binary ran the Phase 19 section and the full suite passed.
- Evidence:
  - Output contained `=== Phase 18: Timeline Projection Tests ===`.
  - Output contained `[PASS] TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead`.
  - Output contained `[PASS] TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead`.
  - Output contained `=== Phase 19: Main Layer Scene Tests ===`.
  - Output contained `[PASS] LAYER_01_PianoRollHasNoPlayheadOverlayChild`.
  - Output contained `[PASS] LAYER_01_PianoRollContinuousScrollStillUsesProjectedPlayheadAfterOverlayRemoval`.
  - Output contained `[PASS] LAYER_02_PianoRollPageScrollStillUsesProjectedPlayheadAfterOverlayRemoval`.
  - Output ended with `Tests Complete`.

### L4 - Passed

- Command: `rtk rg -n "playheadOverlay_|PlayheadOverlayComponent|drawPlayhead|runPhase19MainLayerSceneTests|LAYER_01_PianoRollHasNoPlayheadOverlayChild|LAYER_01_PianoRollContinuousScrollStillUsesProjectedPlayheadAfterOverlayRemoval|LAYER_02_PianoRollPageScrollStillUsesProjectedPlayheadAfterOverlayRemoval|TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead|TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead" "Source/Standalone/UI/PianoRollComponent.h" "Source/Standalone/UI/PianoRollComponent.cpp" "Source/Standalone/UI/PianoRoll/PianoRollRenderer.h" "Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp" "Tests/TestMain.cpp"`
- Result: the static audit found the new main-layer wiring and found no overlay ownership symbols in `PianoRollComponent.h/cpp`.
- Evidence:
  - `Source/Standalone/UI/PianoRollComponent.h:390` defines `mainLayerPlayheadSeconds_`.
  - `Source/Standalone/UI/PianoRollComponent.h:423` defines `playheadColour_`.
  - `Source/Standalone/UI/PianoRollComponent.cpp:698` calls `renderer_->drawPlayhead(g, ctx);`.
  - `Source/Standalone/UI/PianoRollComponent.cpp:1288` updates `mainLayerPlayheadSeconds_ = playheadTimeSeconds;`.
  - `Source/Standalone/UI/PianoRollComponent.cpp:1761` wires `ctx.playheadSeconds = mainLayerPlayheadSeconds_;`.
  - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h:59`, `:60`, `:61`, and `:84` expose `showPlayhead`, `playheadSeconds`, `playheadColour`, and `drawPlayhead(...)`.
  - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp:518`, `:523`, and `:528` draw the playhead from the main-layer render context.
  - `Tests/TestMain.cpp:3447`, `:3451`, `:3480`, `:3533`, and `:3933` keep the Phase 19 regression entry wired into the main test run.
  - The audit output reported `PlayheadOverlayComponent` only in `Tests/TestMain.cpp` for the negative assertion; it reported no `playheadOverlay_` or `PlayheadOverlayComponent` matches inside `PianoRollComponent.h/cpp`.
- Scope note: `PlayheadOverlayComponent` still exists elsewhere in the repository because `ArrangementViewComponent` and tests still include it. Phase 19 only removes the dependency from `PianoRollComponent`.

### L6 - Passed

- Command: `& ".\build-phase19-docs\OpenTuneTests.exe"`
- Result: the second full-suite run matched L2 and passed again on the fresh binary.
- Evidence:
  - Output again contained `[PASS] TIME_01_PianoRollContinuousScrollUsesProjectedClipPlayhead`.
  - Output again contained `[PASS] TIME_01_PianoRollPageScrollUsesProjectedClipPlayhead`.
  - Output again contained `=== Phase 19: Main Layer Scene Tests ===`.
  - Output again contained all three passing `LAYER_*` regressions.
  - Output again ended with `Tests Complete`.
