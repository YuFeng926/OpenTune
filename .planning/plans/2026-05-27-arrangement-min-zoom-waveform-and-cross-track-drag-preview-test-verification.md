# Arrangement Min-Zoom Waveform And Cross-Track Drag Preview Test Verification

## Purpose

This document is the verification source of truth for the Arrangement visual fix covering:

1. waveform visibility at minimum horizontal zoom;
2. pre-release cross-track clip preview during move drag.

The implementation must preserve the existing `Source + Materialization + Placement` truth model and keep drag preview as Standalone UI-only transient state.

## Verification Rule

No implementation task may be marked complete until its matching checks pass in the current checkout. Visual smoke checks are not PASS unless the user explicitly confirms them.

---

## L0 - Spec Completeness

Required checks:

- The implementation plan references this verification document.
- The plan identifies the min-zoom waveform root cause as collapsed drawable bounds, not failed F0/audio extraction.
- The plan identifies cross-track drag as a missing UI projection, not a `StandaloneArrangement` commit defect.
- The plan names the allowed file scope and forbidden files.
- The plan states that move drag does not mutate real placement truth before `mouseUp()`.

Expected result: PASS by review before coding starts.

---

## L1 - Static Contract Gate

Commands:

```powershell
git diff --check
rg -n "MoveDragPreview|PreviewPlacement|computeWaveform|waveformBounds|reduced\\(6, 6\\)|setStandalonePlacementStartSeconds|moveStandalonePlacement" Source/Standalone/UI Tests
```

Required result:

- `ArrangementRenderModelCache::update()` no longer hard-codes `placementBounds.reduced(6, 6)` for waveform bounds.
- The replacement waveform-bounds helper guarantees positive drawable width for positive visible placement width.
- Move preview state exists only in Standalone Arrangement UI/render-model files.
- `mouseDrag()` for `DragOperation::Move` does not call real placement mutation helpers.
- `mouseUp()` remains the real move commit point.
- No new processor/project/arrangement-store preview owner exists.

---

## L2 - Focused Unit And Source Guards

Build command:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

Focused suite:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
```

Required tests:

- `ArrangementWaveform_MinZoomNarrowClipKeepsPositiveDrawableBounds`
- `WaveformTileCache_NarrowDrawableBoundsBuildsNonEmptyPath`
- `ArrangementDragPreview_MouseDragDoesNotCommitMove`
- `ArrangementDragPreview_TargetTrackVisibleBeforeMouseUp`
- `ArrangementDragPreview_MouseUpCommitsOnceAndClearsPreview`

Expected result: all listed tests PASS.

---

## L3 - Integration Gate

Commands:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering architecture processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe piano-roll-intent
```

Required checks:

- Arrangement render-model updates still consume prepared UI data rather than paint-time processor/store enumeration.
- Waveform tile cache remains bounded and keyed by source/materialization/zoom/window/style.
- Drag preview does not leak into processor, arrangement persistence, VST3/ARA session, or audio-thread playback snapshots.
- Existing PianoRoll mouse-intent tests remain PASS, proving the Arrangement change did not disturb nearby interaction contracts.

Expected result: all focused suites PASS.

---

## L4 - Standalone Visual Smoke

Build command:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

Journey:

1. Launch `build-ara-overlay-vs18-clean\OpenTune_artefacts\Release\Standalone\OpenTune.exe` from the project root.
2. Import or open a project with a visible non-silent clip in Arrangement.
3. Zoom Arrangement horizontally to the minimum.
4. Confirm the clip still shows a waveform/envelope at minimum zoom.
5. Drag the clip from one track toward another track and hold without releasing.
6. Confirm the target track shows a clip preview before release.
7. Confirm the original committed placement is not duplicated in a confusing full-strength way during preview.
8. Release the mouse and confirm the move applies once to the target track.
9. Undo and confirm the clip returns to its original track/time.

User confirmation required:

```text
Journey "Arrangement min-zoom waveform + cross-track drag preview" has been checked.
Evidence: [screenshots/logs/notes]
Does this match the expected behavior? (match / does not match)
```

Expected result: PASS only after explicit user confirmation.

---

## L5 - Regression Notes

Do not use the broad `OpenTuneTests.exe ui` runner as the primary proof gate until the known exit-code/no-`[FAIL]` issue is separately resolved.

Useful additional gates if the patch touches shared render-model helpers:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
```

## Exit Condition

The verification is complete only when:

- L1 static contracts pass.
- L2 focused tests pass.
- L3 integration gates pass.
- L4 manual smoke is either user-confirmed PASS or explicitly recorded as pending.
