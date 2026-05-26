# DAW Timeline Rendering Pipeline Test Verification

## Purpose

This document is the verification source of truth for the DAW-style timeline rendering pipeline refactor.
The goal is to make PianoRoll and Arrangement playback scrolling smooth by separating playhead overlay,
viewport scrolling, render-model preparation, cached drawing assets, and frame scheduling.

This refactor must improve the structure without creating parallel old/new render paths, compatibility shims,
or cache truth that competes with `Source + Materialization + Placement`.

## Verification Rule

No implementation task may be marked complete until the matching checks in this document pass in the current
implementation context. Runtime smoothness claims require instrumentation evidence; static source guards alone
can only prove structural contracts.

## L0 - Spec Completeness

Purpose: prove the implementation plan is traceable to the DAW-style requirements.

Required checks:

- The plan references this verification document before implementation tasks.
- The plan maps the six required characteristics:
  - lightweight playhead overlay
  - scroll without full content repaint
  - waveform LOD/tile cache
  - visible-range virtualization for PianoRoll
  - no data preparation inside `paint()`
  - centralized frame scheduling
- The plan identifies old paths to delete, especially direct full repaint paths and duplicate F0 rendering paths.
- The plan preserves the existing persisted truth model: `SourceStore`, `MaterializationStore`,
  `StandaloneArrangement`, and `VST3AraSession`.

Expected result: PASS by review before coding starts.

## L1 - Static Contract Gate

Purpose: ensure the patch removes known structural causes of playback-scroll jank.

Commands:

```powershell
git diff --check
rg -n "buildRenderContext\\(\\)|renderCorrectedOnlyRange\\(0,.*size|drawSelectedOriginalF0Curve|requestInvalidate\\(\\*this, FrameScheduler::Priority::Interactive\\)|repaint\\(\\);" Source/Standalone/UI Tests
rg -n "TimelineViewportState|TimelineFrame|RenderModelCache|WaveformTileCache|timeline-rendering" Source/Standalone/UI Tests
```

Required result:

- `PlayheadOverlayComponent` must not repaint the entire overlay for simple playhead position changes.
- `PianoRollComponent::paint()` must not call `buildRenderContext()` or build large F0/note/waveform payloads.
- `ArrangementViewComponent::paint()` must not enumerate all placements and build full-width waveform paths.
- `ArrangementViewComponent::setScrollOffset()` and `PianoRollComponent::setScrollOffset()` must not trigger
  whole-component invalidation during continuous playback scrolling.
- The selected OriginalF0 overlay must not remain as a separate old F0 render path.
- Direct `pianoRoll_.repaint()` / full view repaint calls from editor heartbeat paths must be replaced by
  semantic invalidation requests.

## L2 - Focused Unit And Source Guards

Purpose: lock down the new rendering contracts without needing a DAW.

Build command:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

Main focused suite:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
```

Required tests:

- `TimelinePlayhead_OverlayDirtyRectOnly`
- `TimelinePlayhead_PositionDoesNotEnterRenderModelKey`
- `TimelineInvalidation_ViewportShiftExposesOnlyNewStrip`
- `TimelineInvalidation_BigJumpPromotesToSingleFullRedraw`
- `PianoRoll_PlayheadOnlyTicksDoNotRebuildRenderModel`
- `PianoRoll_PaintConsumesPreparedRenderModelOnly`
- `PianoRoll_F0VisualsUseSingleRenderPath`
- `PianoRoll_VisibleRangeCullsNotesF0AndWaveformTiles`
- `Arrangement_ScrollOffsetDoesNotInvalidateWholeComponent`
- `Arrangement_PaintConsumesVisibleRenderModelOnly`
- `Arrangement_VisibleRangeCullsOffscreenPlacements`
- `WaveformTileCache_HasBoundedMemoryAndEviction`
- `TimelineFrameDriver_CoalescesRequestsByPriority`
- `TimelineFrameDriver_DropsNonCriticalAnimationRateDuringPlayback`
- `TimelinePaint_HasNoProcessorOrStoreSideEffects`

Expected result: all listed tests PASS.

## L3 - Integration Gate

Purpose: prove PianoRoll, Arrangement, frame scheduling, and render caches interact without crossing product boundaries.

Commands:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering architecture processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe piano-roll-f0-visual
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe piano-roll-intent
```

Required checks:

- Simulating 120 playhead ticks does not rebuild PianoRoll or Arrangement content render models.
- Simulating continuous scroll changes only viewport state and exposed strips unless the jump exceeds the full-redraw threshold.
- Dense F0/notes/waveform data remain visually available through prepared render models.
- F0 visual style and layering contracts from `piano-roll-f0-visual` remain intact.
- Mouse intent behavior from `piano-roll-intent` remains intact.
- Processor, materialization, placement, and ARA ownership contracts remain unchanged.

Expected result: all focused suites PASS. Do not use the broad `ui` suite as this refactor's primary proof gate.

## L4 - Runtime Instrumentation Gate

Purpose: prove the new structure actually improves frame behavior.

Required implementation:

- Add test-build-only or diagnostics-gated counters for:
  - content paint count
  - overlay paint count
  - render model rebuild count
  - dirty rect area
  - full repaint promotions
  - waveform tile cache hit/miss
  - F0 path/model cache hit/miss
  - frame p50/p95/max duration where measurable inside the JUCE message loop
- Counters must live in UI diagnostics/test surfaces, not in `OpenTuneAudioProcessor` truth.

Runtime command:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering-perf
```

Required scenarios:

- Synthetic PianoRoll long clip: dense OriginalF0, corrected segments, notes, and TimeGrid identity.
- Synthetic PianoRoll stretched clip: non-identity TimeGrid to exercise waveform projection.
- Synthetic Arrangement project: multiple tracks, many placements, long waveform mipmaps, most placements offscreen.
- Continuous playhead movement for at least 30 seconds of simulated UI time.
- Continuous follow-scroll for at least 30 seconds of simulated UI time.

Required result:

- Playhead-only ticks rebuild zero content render models.
- Continuous scroll does not promote every frame to full repaint.
- Cache hit rate is stable after warm-up.
- Memory usage of UI caches remains bounded by the configured budget.
- No test evidence suggests UI work touches the audio thread.

## L5 - Visual And Host Smoke

Purpose: confirm real user-visible smoothness and absence of visual artifacts.

Standalone journey:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

Steps:

1. Launch `build-ara-overlay-vs18-clean\OpenTune_artefacts\Release\Standalone\OpenTune.exe` from the project root.
2. Load or create a long vocal clip with visible waveform, OriginalF0, CorrectedF0, notes, and at least one edit.
3. Play with Continuous Scroll enabled in PianoRoll.
4. Play with Continuous Scroll enabled in Arrangement.
5. Switch between Arrangement and PianoRoll while playing.
6. Zoom horizontally during playback.
7. Confirm no flicker, no playhead trail, no obvious frame collapse, and no audio interruption.

Host smoke:

- ARA VST3 in REAPER or Studio One when available.
- Regular VST3 insert in Studio One or another non-ARA host when available.

User confirmation required for each L5 journey:

```text
用户旅程 "[Journey Name]" 已完成验证。
证据: [logs/screenshots/perf counters]
请确认是否符合您的预期？(符合/不符合)
```

Only mark the L5 journey PASS after explicit user confirmation.

## L6 - Regression Gate

Purpose: ensure the timeline rendering refactor does not regress existing product contracts.

Commands:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe piano-roll-f0-visual
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe piano-roll-intent
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe core
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe undo
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe memory
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```

Notes:

- The broad `OpenTuneTests.exe ui` runner has a known exit-code gap in `.planning/STATE.md`; do not claim full-suite PASS from it until that separate runner issue is explained.
- `ctest --test-dir build-ara-overlay-vs18-clean -C Release --output-on-failure` may be run as a final diagnostic, but it is not the first proof gate for this refactor.

## Exit Condition

The refactor is complete only when:

- L1-L4 focused gates pass.
- Existing PianoRoll visual and intent focused gates pass.
- Standalone and VST3 ARA/non-ARA build gates pass.
- L5 visual/host journeys have user confirmation where applicable.
- No old full-repaint, paint-time data-preparation, or parallel F0/waveform/playhead render paths remain.
