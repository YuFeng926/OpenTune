# Arrangement Min-Zoom Waveform And Cross-Track Drag Preview Plan

**Goal:** Fix two Standalone Arrangement visual contracts:

1. At the minimum horizontal zoom, a clip must still show a waveform whenever the source has drawable audio.
2. While dragging a clip across tracks, the target track must show a live clip preview before mouse release; the actual placement move is committed only on mouse release.

**Verification Source:** `.planning/plans/2026-05-27-arrangement-min-zoom-waveform-and-cross-track-drag-preview-test-verification.md`

**Architecture:** Keep this inside the existing DAW timeline rendering pipeline. `Source + Materialization + Placement` remains the persisted truth model. The min-zoom waveform fix belongs to Arrangement render-model/tile bounds. The cross-track drag fix belongs to a UI-only move preview model consumed by Arrangement painting. The preview must not become processor state, project state, audio-thread state, or a second placement owner.

---

## Current Root Causes

### 1. Minimum zoom can collapse the waveform drawable area

Current chain:

- `ArrangementViewComponent::setZoomLevel()` clamps zoom to `0.02`, so the minimum arrangement scale is about `2 px/s`.
- `ArrangementViewComponent::buildProjectedPlacementBounds()` clamps a very narrow visible clip to at least `8 px` wide.
- `ArrangementRenderModelCache::update()` then computes `waveformBounds = placementBounds.reduced(6, 6)`.
- For an `8 px` clip, a horizontal inset of `12 px` leaves a non-positive waveform width.
- `WaveformTileCache::buildTile()` returns an empty tile when the drawable width/path has no pixels, and `drawPlacementClips()` silently skips empty waveform paths.

This is primarily a drawable-rect contract bug, not an audio extraction or mipmap data failure.

### 2. Cross-track drag has commit logic but no pre-release projection

Current chain:

- `mouseDown()` records the placement id, starting track, and starting time.
- `mouseDrag()` mutates the selected placement's real timeline start on its original track.
- Track changes are not projected during drag.
- `mouseUp()` derives the release track and calls the real `moveStandalonePlacement(...)` commit path only then.

The final commit path is already in the right layer, but the UI has no transient target-track preview. The current drag path also uses real placement mutation as horizontal preview, which makes "release commits the move" less clean than it should be.

---

## Non-Negotiable Boundaries

- Do not modify `StandaloneArrangement`, `OpenTuneAudioProcessor::movePlacementToTrack`, or `MovePlacementAction` for preview behavior.
- Do not add preview state to project serialization, processor state, source/materialization/placement stores, or audio playback snapshots.
- Do not make a parallel drag commit path. `mouseUp()` remains the only place where a move drag changes placement truth.
- Do not use minimum zoom clamping as the waveform fix. Users must still be able to zoom out fully.
- Do not fake waveform visibility by only changing alpha/color. The drawable waveform bounds/tile contract must remain positive.
- Do not reintroduce paint-time processor/store enumeration outside the existing prepared Arrangement render model pipeline.
- Do not use a broad `ui` suite as the primary proof gate while its runner exit-code issue remains unresolved.

---

## Allowed Files

Primary implementation scope:

- `Source/Standalone/UI/ArrangementViewComponent.h`
- `Source/Standalone/UI/ArrangementViewComponent.cpp`
- `Source/Standalone/UI/ArrangementRenderModelCache.h`
- `Source/Standalone/UI/ArrangementRenderModelCache.cpp`
- `Source/Standalone/UI/WaveformTileCache.h`

Test scope:

- `Tests/TestTimelineRenderingPipeline.cpp`
- `Tests/TestMain.cpp`
- `Tests/TestSupport.h` only if an existing helper is needed.

Planning scope:

- This plan.
- Its matching test-verification document.
- Top-level `.planning` state docs only for status synchronization.

Stop and ask before touching:

- `Source/StandaloneArrangement.*`
- `Source/PluginProcessor.*`
- `Source/Utils/PlacementActions.*`
- VST3/ARA editor or session files.

---

## Task 1: Lock The Failing Contracts With Focused Tests

Add tests before implementation changes.

Required tests:

- `ArrangementWaveform_MinZoomNarrowClipKeepsPositiveDrawableBounds`
  - Constructs the same geometry contract as the Arrangement render model: an `8 px` placement at minimum zoom must still produce a positive waveform drawable width.
- `WaveformTileCache_NarrowDrawableBoundsBuildsNonEmptyPath`
  - With a built mipmap and non-silent synthetic audio, a narrow positive waveform rectangle must produce a non-empty path.
- `ArrangementDragPreview_MouseDragDoesNotCommitMove`
  - A move drag updates preview state but does not call real placement time/track mutation before `mouseUp()`.
- `ArrangementDragPreview_TargetTrackVisibleBeforeMouseUp`
  - Dragging over another track creates a preview placement projected to that track.
- `ArrangementDragPreview_MouseUpCommitsOnceAndClearsPreview`
  - Release commits the final time/track exactly once and removes preview state.

Prefer adding these to the focused `timeline-rendering` suite unless a narrower `arrangement-interaction` grouping already exists.

---

## Task 2: Fix The Low-Zoom Waveform Drawable Bounds

Implement a single helper that derives waveform drawable bounds from the visible placement bounds.

Required behavior:

- Horizontal inset must be adaptive:
  - use the existing visual padding for normal-width clips;
  - shrink the horizontal inset when the clip is narrow;
  - guarantee a positive drawable width when the placement width is positive.
- Vertical inset may remain visually similar, but must not collapse the waveform height for compact tracks.
- The helper must live in Arrangement UI/render-model code, not in `WaveformMipmap`.
- `ArrangementRenderModelCache::update()` must call this helper instead of hard-coding `placementBounds.reduced(6, 6)`.
- `WaveformTileCache::buildTile()` may keep returning an empty tile for invalid rectangles, but it must receive a valid positive rectangle for valid visible clips.

Delete/replace:

- Replace the fixed `placementBounds.reduced(6, 6)` waveform rectangle.
- Do not add a second fallback waveform draw path in `drawPlacementClips()`.

---

## Task 3: Add A UI-Only Move Drag Preview Model

Introduce an Arrangement-only transient preview state for `DragOperation::Move`.

Required state:

- active/inactive flag
- source placement ids and original tracks
- preview target track
- preview start seconds for each dragged placement
- enough revision/epoch data for render-model invalidation

Required behavior:

- `mouseDrag()` for move drag computes snapped preview start time and target track from the live pointer.
- `mouseDrag()` must not call:
  - `setStandalonePlacementStartSeconds(...)`
  - `moveStandalonePlacement(...)`
  - `OpenTuneAudioProcessor::movePlacementToTrack(...)`
- The committed placement remains unchanged in `StandaloneArrangement` until `mouseUp()`.
- The Arrangement render model receives the preview state and emits preview placements projected into the target track.
- While preview is active, the committed source placement should not be drawn as a competing full-strength duplicate. Either suppress it in the render model or draw it only as an intentionally dim origin marker.
- Preview drawing should reuse the existing clip visual language:
  - same rounded clip body geometry;
  - visible title/gain when space allows;
  - optional waveform if a valid tile is available;
  - preview alpha/outline distinct enough to read as pending.

Preferred structure:

- Add a small `MoveDragPreviewState` in `ArrangementViewComponent`.
- Pass a value object into `ArrangementRenderModelCache::update(...)`.
- Add `RenderModel::PreviewPlacement` or mark `VisiblePlacement::isPreview`.
- Keep `drawPlacementClips(...)` as the only clip drawing entry point by having it consume committed and preview placements from the render model.

---

## Task 4: Commit Or Discard Preview On Gesture End

`mouseUp()` becomes the only real move commit point.

Required behavior:

- If the drag did not pass the threshold, clear preview and leave placement truth unchanged.
- If the drag is valid and remains on the same track, commit the final snapped start time once.
- If the drag is valid and targets another track, call the existing `moveStandalonePlacement(...)` path once per moved placement.
- Multi-selection preview and commit must match the current product semantics. Do not invent a new relative-track policy unless the user explicitly asks for it.
- Undo actions must represent the final committed move, not intermediate preview updates.
- Selection must resolve to the moved placement(s) after a successful cross-track commit.
- Clear preview state on release and on any path that cancels/abandons the drag.

Delete/replace:

- Delete move-drag real-time calls that mutate placement start seconds during `mouseDrag()`.
- Delete any separate "ghost component" approach if it bypasses the render model.

---

## Task 5: Focused Verification

Run the gates from the verification document in order:

1. Static contract gate.
2. Focused `OpenTuneTests` build.
3. Focused `timeline-rendering` suite.
4. Manual Standalone visual smoke for the exact two reported behaviors.

Do not claim L5/manual visual PASS without user confirmation.

---

## Exit Condition

The fix is complete only when:

- At minimum zoom, clips with non-silent source audio still show a waveform or a visibly meaningful waveform envelope.
- Cross-track drag shows a target-track clip preview before mouse release.
- Real placement time/track truth is not changed until `mouseUp()`.
- Existing move undo/selection behavior remains coherent after release.
- The focused automated gates pass in the current checkout.
- Manual visual smoke is either confirmed by the user or explicitly left as pending.
