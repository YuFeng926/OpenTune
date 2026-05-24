# Piano Roll F0 Visual Style Test Verification

## Purpose

This document is the verification source of truth for the Piano Roll OriginalF0 / CorrectedF0 visual-style pass.
The goal is to make both F0 curves match the provided reference image direction without changing pitch truth,
render-cache truth, or edit semantics. All alpha, smoothing, endpoint fade, and glow changes are display-only.

## Verification Levels

### L0 Spec Completeness

- Confirm the implementation plan references this verification document before any implementation task.
- Confirm the change is scoped to Piano Roll display code and theme tokens only.
- Confirm the plan explicitly preserves `PitchCurveSnapshot`, `F0Timeline`, `MaterializationTimelineProjection`, and audio/render data as truth.

### L1 Static Gate

- Command: `git diff --check`
- Goal: prove the patch introduces no whitespace, encoding, or merge-hunk breakage.

- Command: `rg -n "F0Visual|buildF0|sample.*F0|energy.*alpha|drawF0Curve" Source/Standalone/UI Tests`
- Goal: audit that the new visual helper lives in the Piano Roll UI renderer/test surface and is not pushed into processor/audio truth.

### L2 Unit / Focused Guard

- Build command:
  `cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"`

- Test command:
  `build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe ui`

- Required focused guards:
  - `PianoRollF0Visual_UsesEnergyAlphaWithinBounds`
  - `PianoRollF0Visual_OriginalF0DrawsBelowCorrectedAndDimmer`
  - `PianoRollF0Visual_ZoomedOutBucketsAreBoundedByPixelDensity`
  - `PianoRollF0Visual_ZoomedInRestoresDetail`
  - `PianoRollF0Visual_VoicelessGapsDoNotConnectAcrossSegments`
  - `PianoRollF0Visual_EndpointFadeAppliesToBothCurves`

Expected: all focused guards PASS. If the existing `ui` suite still exits 1 without `[FAIL]`, report that as the known runner gap from `.planning/STATE.md`; do not claim full-suite PASS from this run alone.

### L3 Integration Gate

- Test command:
  `build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture`

- Goal: ensure source/materialization/projection contracts remain intact:
  - `PitchCurveSnapshot::getOriginalEnergy()` remains frame-aligned with `getOriginalF0()`.
  - CorrectedF0 display uses the same frame-domain energy as OriginalF0.
  - The renderer does not mutate `PitchCurve`, `correctedF0`, notes, or materialization state.

### L4 Contract Audit

- Audit files:
  - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h`
  - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
  - `Source/Standalone/UI/PianoRollComponent.cpp`
  - `Source/Standalone/UI/ThemeTokens.h`
  - `Source/Standalone/UI/UIColors.h`
  - `Tests/TestMain.cpp`

- Required contract points:
  - F0 visual sampling is renderer-local and screen-space driven.
  - Energy-to-alpha mapping clamps to the requested visible range.
  - OriginalF0 is painted before CorrectedF0.
  - OriginalF0 base alpha is lower than CorrectedF0 at matching energy.
  - Soft glow intensity is lower than the current Aurora/BlueBreeze/Overdose F0 glow.
  - Endpoint fades are based on visual segments, not persisted corrected segment boundaries.

### L5 Visual Smoke

- Build command:
  `cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_Standalone"`

- Journey:
  1. Launch `build-nonara-overlay-vs18-clean\OpenTune_artefacts\Release\Standalone\OpenTune.exe`.
  2. Load or create a clip with visible OriginalF0 and CorrectedF0.
  3. Compare the Piano Roll view against the uploaded reference image.
  4. Check zoomed-out and zoomed-in views.
  5. Capture screenshots for review.

- Visual acceptance:
  - OriginalF0 reads as orange/gold and sits behind CorrectedF0.
  - CorrectedF0 reads as bright cyan/blue and remains the main visual focus.
  - Both curves are semi-transparent and respond to local energy.
  - Low-energy spans are lighter, with visual alpha near 70% of the curve's own maximum.
  - High-energy spans can reach 100% of the curve's own maximum.
  - OriginalF0 remains slightly more transparent on average than CorrectedF0.
  - Zoomed-out views do not become dense combs or noisy scribbles.
  - Zoomed-in views automatically reveal more curve detail.
  - Segment ends taper smoothly rather than ending as hard dots or square cuts.
  - Glow is subtle and does not dominate the reference-like line work.

### L6 Residual Review

- Re-run the focused test suite and static audit after visual tuning.
- Record any residual mismatch against the reference image:
  - color hue or saturation
  - alpha response curve
  - smoothing amount at low zoom
  - detail recovery at high zoom
  - endpoint fade length
  - glow strength

## Exit Condition

The pass is complete only when focused guards pass, architecture contracts are preserved, and visual smoke screenshots show both F0 curves matching the reference direction without changing the editable/audio truth.
