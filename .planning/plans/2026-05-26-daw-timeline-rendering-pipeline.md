# DAW Timeline Rendering Pipeline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Rebuild the PianoRoll and Arrangement timeline rendering path into a DAW-style display pipeline that keeps playback scrolling smooth under real editing load.

**Architecture:** The timeline UI becomes a four-layer pipeline: frame driver, viewport state, prepared render models/tile caches, and lightweight overlays. `paint()` only consumes prepared model data; playback-head movement and continuous scrolling no longer rebuild musical content. Business truth remains `Source + Materialization + Placement`; projection, render models, and drawing caches are transient UI state.

**Tech Stack:** C++17, JUCE `Component`/`Graphics`, existing `FrameScheduler`, `VBlankAttachment`, `PianoRollComponent`, `PianoRollRenderer`, `ArrangementViewComponent`, `WaveformMipmap`, native `OpenTuneTests`.

**Verification Source:** `.planning/plans/2026-05-26-daw-timeline-rendering-pipeline-test-verification.md`

---

## Design Principles

1. Playback head is an overlay, not content.
2. Scroll changes viewport state; it does not rebuild the world.
3. Waveform drawing consumes LOD/tile cache, not paint-time path generation.
4. PianoRoll renders only visible time and pitch ranges.
5. `paint()` has no data-preparation side effects.
6. One frame driver coordinates playhead, scroll, hover, meters, and low-priority animation.

## Current Root Problems

- `PlayheadOverlayComponent` setters repaint the full overlay instead of old/new narrow dirty rects.
- `PianoRollComponent::setScrollOffset()` and `ArrangementViewComponent::setScrollOffset()` can invalidate large content areas during continuous playback.
- `PianoRollComponent::paint()` starts by building a render context and materialization items.
- `buildMaterializationRenderItem()` can allocate and fill full `correctedF0` data during paint.
- `PianoRollRenderer::drawF0Curve()` rebuilds visual segments and paths on each draw.
- `ArrangementViewComponent::paint()` iterates tracks/placements and builds waveform paths in the hot paint path.
- `drawSelectedOriginalF0Curve()` is a legacy separate F0 paint path that must be merged into the unified renderer/model.
- Editor heartbeat paths still contain direct repaint calls that bypass semantic invalidation.

## Target Structure

```
TimelineFrameCoordinator
    -> reads playhead source / VBlank
    -> coalesces scroll, hover, meter, animation requests
    -> dispatches overlay dirty rects and content exposed strips

TimelineViewportState
    -> zoom, scroll offset, visible time range, content bounds
    -> shared math for PianoRoll and Arrangement

Prepared Render Models
    -> PianoRollRenderModelCache
    -> ArrangementRenderModelCache
    -> WaveformTileCache

Paint
    -> consumes immutable prepared model snapshots
    -> draws only the provided visible model/tile data
```

## Non-Negotiable Boundaries

- Do not put UI render caches in `OpenTuneAudioProcessor`.
- Do not write app visual preferences into processor/project state.
- Do not let the audio thread wait on UI caches, UI locks, or render-model generation.
- Do not make Arrangement cache visible to VST3; Arrangement is Standalone-only.
- Do not make projection/render models/tile caches a fourth persisted truth owner.
- Do not keep old/new F0, waveform, playhead, or scroll rendering paths alive in parallel.

## Task 1: Establish Timeline Rendering Tests

**Files:**
- Modify: `Tests/TestMain.cpp`
- Modify: `Tests/TestSupport.h`
- Test source: `.planning/plans/2026-05-26-daw-timeline-rendering-pipeline-test-verification.md`

**Steps:**
1. Add a focused `timeline-rendering` suite to the test registry.
2. Add source-guard tests from L2:
   - `TimelinePlayhead_OverlayDirtyRectOnly`
   - `PianoRoll_PaintConsumesPreparedRenderModelOnly`
   - `Arrangement_ScrollOffsetDoesNotInvalidateWholeComponent`
   - `Timeline_NoParallelF0RenderPaths`
3. Add lightweight test probes for `ArrangementViewComponent` only where needed.
4. Run the new suite and confirm the current tree fails on the known old paths.
5. Commit only after the tests fail for the right reasons.

**Run:**

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
```

**Expected before implementation:** FAIL on full overlay repaint, paint-time context building, and full scroll invalidation.

## Task 2: Make Playhead Overlay Truly Cheap

**Files:**
- Modify: `Source/Standalone/UI/PlayheadOverlayComponent.h`
- Modify: `Source/Standalone/UI/PlayheadOverlayComponent.cpp`
- Modify: `Source/Standalone/UI/PianoRollComponent.cpp`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Add a helper that computes the playhead dirty rectangle for a given pixel X, line width, top triangle, and viewport bounds.
2. In each setter, skip work when the effective value has not changed.
3. For position changes, repaint the union of old and new narrow dirty rects.
4. For zoom/scroll/timeline-origin changes, repaint only old/new rects unless viewport geometry changed.
5. Keep overlay transparent and visually identical.
6. Ensure playhead movement does not call parent view invalidation.

**Delete/replace:**
- Remove unconditional overlay `repaint()` from simple playhead position changes.
- Remove any parent content repaint triggered only by playhead position.

**Verify:**

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
```

## Task 3: Introduce TimelineViewportState

**Files:**
- Create: `Source/Standalone/UI/TimelineViewportState.h`
- Create: `Source/Standalone/UI/TimelineViewportState.cpp`
- Modify: `Source/Standalone/UI/PianoRollComponent.h`
- Modify: `Source/Standalone/UI/PianoRollComponent.cpp`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.h`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Define a small value object with zoom, scroll offset, viewport bounds, content width, and visible time range.
2. Add helpers:
   - `timeToContentX`
   - `timeToViewportX`
   - `viewportXToTime`
   - `exposedStripForScrollDelta`
   - `requiresFullRedrawForDelta`
3. Move duplicate PianoRoll/Arrangement horizontal time math to this object.
4. Keep PianoRoll-specific pitch/key geometry outside this object.
5. Keep Arrangement-specific tracks/lane geometry outside this object.
6. Update scroll methods to update viewport state first, then request only exposed-strip repaint.

**Delete/replace:**
- Delete duplicate scroll-delta dirty-rect math from individual components once the shared object owns it.
- Replace direct full invalidation on ordinary scroll deltas.

**Verify:**

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
```

## Task 4: Upgrade FrameScheduler Into Timeline Frame Coordinator

**Files:**
- Modify: `Source/Standalone/UI/FrameScheduler.h`
- Modify: `Source/Standalone/UI/PianoRollComponent.h`
- Modify: `Source/Standalone/UI/PianoRollComponent.cpp`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.h`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.cpp`
- Modify: `Source/Standalone/PluginEditor.cpp`
- Modify: `Source/Plugin/PluginEditor.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Keep the existing `FrameScheduler` entry point but add timeline-specific request types:
   - playhead overlay
   - viewport shift
   - content model invalidation
   - low-priority animation
2. Coalesce requests by component and priority before repaint.
3. Let VBlank update playhead and scroll intent, but route repaint requests through the coordinator.
4. During playback, allow meters/hover/ripple/analysis animations to run at lower priority than playhead and scroll.
5. Replace direct editor `pianoRoll_.repaint()` calls with semantic invalidation calls.
6. Keep `onHeartbeatTick()` for background work flushing, not for playhead drawing.

**Delete/replace:**
- Delete direct full repaint calls from playhead/heartbeat paths.
- Delete duplicated VBlank ownership where a component and scheduler both try to drive the same content repaint.

**Verify:**

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe piano-roll-intent
```

## Task 5: Move PianoRoll Data Preparation Out Of Paint

**Files:**
- Create: `Source/Standalone/UI/PianoRoll/PianoRollRenderModelCache.h`
- Create: `Source/Standalone/UI/PianoRoll/PianoRollRenderModelCache.cpp`
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h`
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
- Modify: `Source/Standalone/UI/PianoRollComponent.h`
- Modify: `Source/Standalone/UI/PianoRollComponent.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Define `PianoRollRenderModel` as a prepared, immutable UI snapshot for one visible viewport.
2. Move materialization item building from `paint()` to cache update methods.
3. Cache keys must include:
   - materialization id
   - pitch curve revision
   - notes revision
   - visual preference revision
   - viewport visible range
   - zoom bucket
   - TimeGrid revision
4. Cache keys must not include playhead position.
5. Generate corrected F0 only for required visible frame ranges, or reuse a revision-backed prepared vector when full-curve generation is truly necessary.
6. Move F0 visual segments/path preparation into the render model cache.
7. Paint should receive a ready model and call renderer draw methods without store/processor reads.

**Delete/replace:**
- Delete `PianoRollComponent::paint()` -> `buildRenderContext()` as the paint-time entry.
- Delete paint-time `renderCorrectedOnlyRange(0, size, ...)`.
- Delete paint-time notes copy from processor/store.

**Verify:**

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe piano-roll-f0-visual
```

## Task 6: Merge F0 Rendering Into One Path

**Files:**
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.h`
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
- Modify: `Source/Standalone/UI/PianoRollComponent.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Extend the prepared F0 visual model to carry selection/highlight state.
2. Route selected OriginalF0 overlay styling through the same F0 visual model/path system.
3. Preserve the visual contract: OriginalF0 below CorrectedF0, OriginalF0 softer on average, selected spans visible.
4. Remove `drawSelectedOriginalF0Curve()` after its behavior is represented in the unified model.

**Delete/replace:**
- Delete the old selected OriginalF0 standalone path builder.
- Do not leave a compatibility helper with the old path logic.

**Verify:**

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe piano-roll-f0-visual
```

## Task 7: Add Waveform Tile Cache

**Files:**
- Create: `Source/Standalone/UI/WaveformTileCache.h`
- Create: `Source/Standalone/UI/WaveformTileCache.cpp`
- Modify: `Source/Standalone/UI/WaveformMipmap.h`
- Modify: `Source/Standalone/UI/WaveformMipmap.cpp`
- Modify: `Source/Standalone/UI/PianoRoll/PianoRollRenderer.cpp`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Preserve `WaveformMipmap` as the LOD peak data source.
2. Add a bounded cache for prepared visible waveform draw tiles or paths.
3. Key tiles by source/materialization identity, visible time span, zoom bucket, channel/lane style, TimeGrid revision where needed.
4. Bound memory with LRU or ring eviction.
5. Make PianoRoll and Arrangement consume the same tile cache API.
6. During scroll, reuse shifted tiles and only request exposed strips.

**Delete/replace:**
- Delete per-paint full-width waveform path loops.
- Delete Arrangement waveform loops over invisible placement width.

**Verify:**

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
```

## Task 8: Add Arrangement Render Model Cache

**Files:**
- Create: `Source/Standalone/UI/ArrangementRenderModelCache.h`
- Create: `Source/Standalone/UI/ArrangementRenderModelCache.cpp`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.h`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.cpp`
- Modify: `Source/StandaloneArrangement.h`
- Modify: `Source/StandaloneArrangement.cpp`
- Test: `Tests/TestMain.cpp`

**Steps:**
1. Define a visible arrangement render model with lanes, visible placements, selection state, analysis state, and waveform tile references.
2. Prepare only placements intersecting the visible time range and visible track range.
3. Avoid repeated paint-time calls to `getNumPlacements()` / `getPlacementByIndex()`.
4. Use a revision or snapshot boundary from `StandaloneArrangement` so model rebuilds happen only when arrangement state changes.
5. Paint lanes, grid, clips, labels, and waveforms from the prepared model.

**Delete/replace:**
- Delete paint-time traversal of all tracks and placements.
- Delete paint-time placement lock/scanning loops.
- Delete full invalidation on normal horizontal follow-scroll.

**Verify:**

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

## Task 9: Add Runtime Instrumentation

**Files:**
- Create: `Source/Standalone/UI/TimelineRenderingDiagnostics.h`
- Create: `Source/Standalone/UI/TimelineRenderingDiagnostics.cpp`
- Modify: `Source/Standalone/UI/PianoRollComponent.cpp`
- Modify: `Source/Standalone/UI/ArrangementViewComponent.cpp`
- Modify: `Source/Standalone/UI/FrameScheduler.h`
- Modify: `Tests/TestMain.cpp`

**Steps:**
1. Add diagnostics counters behind test/diagnostic gates.
2. Count:
   - overlay paints
   - content paints
   - full repaint promotions
   - exposed-strip repaint requests
   - render model rebuilds
   - tile cache hits/misses
   - frame durations where measurable
3. Add `timeline-rendering-perf` suite.
4. Build synthetic long PianoRoll and Arrangement scenarios.
5. Assert structural performance outcomes, not fragile exact FPS numbers.

**Verify:**

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering-perf
```

## Task 10: Final Cleanup And Regression

**Files:**
- Modify as needed:
  - `Source/Standalone/UI/PianoRollComponent.*`
  - `Source/Standalone/UI/PianoRoll/PianoRollRenderer.*`
  - `Source/Standalone/UI/ArrangementViewComponent.*`
  - `Source/Standalone/UI/FrameScheduler.h`
  - `Source/Standalone/UI/PlayheadOverlayComponent.*`
  - `Tests/TestMain.cpp`
  - `.planning/PROJECT.md`
  - `.planning/ROADMAP.md`
  - `.planning/STATE.md`

**Steps:**
1. Run the static kill-list audit:
   - no old selected F0 standalone path
   - no playhead full overlay repaint for position ticks
   - no paint-time render context build
   - no scroll-time whole-component invalidation for normal deltas
   - no unbounded UI caches
   - no processor-owned UI cache
2. Run focused suites.
3. Build Standalone, ARA VST3, and non-ARA VST3.
4. Update `.planning` with the landed architecture and any remaining L5 gaps.
5. Keep the known broad `ui` runner exit-code issue separate unless this refactor directly fixes it.

**Run:**

```powershell
git diff --check
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering-perf
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

## Kill List

- No "quick fix" that only throttles VBlank or lowers FPS.
- No duplicate old/new renderer switch.
- No fallback path that silently returns to full repaint.
- No cache in processor or audio-thread state.
- No visible-range cache that ignores TimeGrid/projection.
- No unbounded tile cache.
- No direct `repaint()` from editor heartbeat for content changes that have a semantic invalidation reason.
- No plan completion claim based only on static guards; runtime instrumentation must support smoothness claims.

## Review Notes

This plan intentionally creates a small number of structural pieces rather than many tiny abstractions:

- `TimelineViewportState` owns time/scroll math.
- `FrameScheduler` becomes the single frame coordination point.
- `PianoRollRenderModelCache` owns PianoRoll prepared display data.
- `ArrangementRenderModelCache` owns Standalone arrangement prepared display data.
- `WaveformTileCache` owns reusable waveform drawing assets.
- `PlayheadOverlayComponent` remains the cheap overlay.

The objective is not to make the code more fragmented. The objective is to make the expensive work happen only when its real input changes.
