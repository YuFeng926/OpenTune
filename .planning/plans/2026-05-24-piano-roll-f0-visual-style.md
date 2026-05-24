# Piano Roll F0 Visual Style Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make OriginalF0 and CorrectedF0 match the uploaded reference image style: translucent, energy-responsive, smoother at low zoom, more detailed at high zoom, softly tapered at segment ends, and less glow-heavy.

**Architecture:** Keep the change in the Piano Roll display layer. `PitchCurveSnapshot`, `F0Timeline`, corrected-F0 generation, render-cache audio, and materialization truth remain unchanged; the renderer derives temporary visual samples and alpha from the existing frame-domain F0 and `originalEnergy`.

**Tech Stack:** C++17, JUCE Graphics, existing `PianoRollRenderer`, `PianoRollComponent`, `ThemeTokens`, `UIColors`, native `OpenTuneTests`.

**Verification Source:** `.planning/plans/2026-05-24-piano-roll-f0-visual-style-test-verification.md`

---

## Current Findings

- The active draw path is `PianoRollComponent::paint()` -> `PianoRollRenderer::drawF0Curve()`.
- OriginalF0 is already painted before CorrectedF0, so the layering direction is correct.
- Current OriginalF0 alpha is fixed at `0.84` for Aurora/Overdose or `0.55` otherwise; CorrectedF0 is fixed at `1.0`.
- Current F0 rendering is still basically per-frame path construction. It crops to the visible frame range but has no screen-space smoothing or density-aware sampling.
- Current endpoint fade is a small 3-frame ellipse overlay at each segment end, not a tapered line style.
- `PitchCurveSnapshot::getOriginalEnergy()` already exists and is frame-aligned with OriginalF0, so the implementation can reuse it for both OriginalF0 and CorrectedF0 visual alpha.
- Current glow is theme-dependent and fairly strong in Aurora / BlueBreeze / Overdose branches.

## Visual Contract

- Reference color draft:
  - CorrectedF0: bright cyan blue, initial token `#2EC7F8`.
  - OriginalF0: orange gold, initial token `#F8A818`.
- Energy alpha mapping:
  - Interpret energy only as a normalized visual weight.
  - Clamp local alpha factor to `[0.70, 1.00]`.
  - Apply an additional per-curve base opacity so OriginalF0 is always slightly softer than CorrectedF0.
  - Suggested effective maxima:
    - CorrectedF0: base max `0.92` to `1.00`.
    - OriginalF0: base max `0.62` to `0.76`.
- Layering:
  - OriginalF0 remains below CorrectedF0.
  - Selected OriginalF0 overlay should use the same new color family and reduced glow, not the old red-heavy style.
- Density:
  - Low zoom: renderer buckets frames in screen space and outputs one smoothed representative path per pixel/half-pixel band.
  - High zoom: automatically reduces bucket width until individual F0-frame detail becomes visible again.
- Endpoint style:
  - Both curves taper at every voiced visual segment boundary.
  - Fade is path-segment based, not dependent on corrected-segment storage.
- Glow:
  - Keep only a subtle halo and inner line highlight.
  - Remove or reduce broad glow strokes so the result reads like the reference image rather than neon.

## Task 1: Add Renderer-Local Visual Samples

**Files:**
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h`
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Add a small renderer-local struct, for example `F0VisualPoint`, containing `frame`, `x`, `y`, `energyAlpha`, and `segmentBoundary`.
2. Add a renderer-local helper that converts `f0 + originalEnergy + F0Timeline + projection` into visual segments.
3. Keep the helper pure and deterministic so `Tests/TestMain.cpp` can unit-test it without painting a real JUCE component.
4. Do not write this helper into `PitchCurve`, `F0Timeline`, `MaterializationStore`, or processor code.

**Tests:**
- Add `PianoRollF0Visual_UsesEnergyAlphaWithinBounds`.
- Add `PianoRollF0Visual_VoicelessGapsDoNotConnectAcrossSegments`.

## Task 2: Implement Energy-Responsive Alpha

**Files:**
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Read `item.pitchSnapshot->getOriginalEnergy()` inside the F0 visual helper.
2. Normalize energy over the visible frame window with a robust clamp, preferably percentile or local min/max with sane defaults when all values are equal.
3. Map normalized energy into `[0.70, 1.00]`.
4. Multiply by curve-specific base opacity:
   - OriginalF0 lower and thinner.
   - CorrectedF0 higher and visually dominant.
5. Use the same energy alpha for CorrectedF0 because it shares the OriginalF0 frame domain.

**Tests:**
- Low energy never drops below the requested 70% factor.
- High energy can reach the curve maximum.
- OriginalF0 effective alpha remains lower than CorrectedF0 for identical energy.

## Task 3: Add Zoom-Aware Smoothing And Density Control

**Files:**
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Compute the display bucket width from `ctx.pixelsPerSecond` and F0 frame spacing.
2. At low zoom, aggregate multiple frames per visible x bucket.
3. Use energy-weighted average or median-like representative y values to avoid noisy vertical combing.
4. Preserve voiced/unvoiced boundaries so the curve never bridges silent gaps.
5. At high zoom, shrink the bucket toward frame-level detail automatically.
6. Build the JUCE path from visual points, not raw F0 frames.

**Tests:**
- `PianoRollF0Visual_ZoomedOutBucketsAreBoundedByPixelDensity`.
- `PianoRollF0Visual_ZoomedInRestoresDetail`.

## Task 4: Replace Endpoint Dot Fade With Tapered Segment Rendering

**Files:**
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Remove the current endpoint ellipse fade as the primary endpoint treatment.
2. Split each visual segment into small sub-path spans near the beginning and end.
3. Apply fade-in/fade-out alpha over a short pixel-aware or frame-aware window.
4. Use the same taper logic for OriginalF0 and CorrectedF0.
5. Keep selection rendering compatible, but do not let selection reintroduce hard endpoints.

**Tests:**
- `PianoRollF0Visual_EndpointFadeAppliesToBothCurves`.
- Extend the existing corrected-F0 hard-boundary guard so the new taper stays data-driven.

## Task 5: Update Reference Colors And Reduce Glow

**Files:**
- Modify: `Source/Standalone/UI/ThemeTokens.h`
- Modify: `Source/Standalone/UI/UIColors.h`
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
- Modify: `Source/Standalone/UI/PianoRollComponent.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Update the shared Piano Roll F0 tokens to the reference draft colors:
   - OriginalF0 `#F8A818`.
   - CorrectedF0 `#2EC7F8`.
2. Ensure theme-specific tokens no longer override the F0 curves into red, green, or pink for the main Piano Roll visual contract.
3. Reduce broad glow stroke alpha and width in `drawF0Curve()`.
4. Update `drawSelectedOriginalF0Curve()`, hand-draw preview, note-drag preview, and line-anchor preview only where they visually inherit the old F0 style.
5. Preserve tool feedback clarity, but avoid making previews brighter than the committed CorrectedF0 line.

**Tests:**
- Add a static guard that the main F0 tokens contain the new reference color values.
- Add a static guard that `drawF0Curve()` still draws OriginalF0 before CorrectedF0 through `PianoRollComponent::paint()`.

## Task 6: Visual Smoke And Tuning

**Files:**
- No required source changes unless screenshots reveal mismatch.

**Steps:**
1. Build the Standalone target with the command from the verification document.
2. Launch the rebuilt Standalone.
3. Load a clip with both OriginalF0 and CorrectedF0 visible.
4. Capture zoomed-out and zoomed-in screenshots.
5. Compare with the uploaded reference image:
   - cyan/orange hue
   - energy alpha response
   - OriginalF0 behind CorrectedF0
   - smoothed low-zoom density
   - detail recovery at high zoom
   - tapered endpoints
   - reduced glow
6. Tune only renderer constants and tokens. Do not change F0 data generation.

## Kill List Review

- No new persisted visual preference for this pass unless the user later asks for customization.
- No parallel F0 storage.
- No recomputation of RMVPE or corrected F0 for display style.
- No processor/audio-thread involvement.
- No fallback path that keeps old and new F0 renderers alive in parallel.
- No theme-specific drift that turns the curves back into unrelated red/green/pink styling.

## Open Review Point

The exact reference color values should be treated as an implementation starting point. If the first visual smoke shows the cyan or orange is off against the uploaded image, tune `#2EC7F8` / `#F8A818` by screenshot comparison before final verification.
