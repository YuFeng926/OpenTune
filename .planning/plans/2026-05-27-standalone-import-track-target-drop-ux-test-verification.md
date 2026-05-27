# Standalone Import Track-Target Drop UX Test Verification

## Purpose

This document is the verification source of truth for the Standalone import UX upgrade covering:

1. drag-drop import to the track under the pointer;
2. blank-area drop creating a new track;
3. chooser import preserving a fast default path;
4. editor-owned explicit `ImportPlacement` remaining the only placement truth entry.

## Verification Rule

No implementation task may be marked complete until the matching checks pass in the current checkout. Manual visual checks are not PASS unless the user explicitly confirms them.

---

## L0 - Spec Completeness

Required checks:

- The implementation plan references this verification document.
- The plan identifies `filesDropped(...)` discarding `x,y` as the current drag-drop root cause.
- The plan identifies the modal track picker as a legacy drag-drop UX problem, not a processor-placement requirement.
- The plan preserves explicit `ImportPlacement` ownership in Standalone editor code.
- The plan states the blank-area new-track rule and the `MAX_TRACKS` behavior.

Expected result: PASS by review before coding starts.

---

## L1 - Static Contract Gate

Commands:

```powershell
git diff --check
rg -n "filesDropped|fileDragEnter|fileDragMove|fileDragExit|promptTrackSelectionForDroppedFile|ImportPlacement|trackIdForViewportY|setVisibleTrackCount|showMoreTracks" Source/Standalone Tests
```

Required result:

- Drag-drop no longer uses `promptTrackSelectionForDroppedFile(...)` as the normal path.
- File-drag enter/move/exit handling exists for Standalone import target preview.
- Import target resolution stays in Standalone editor/UI files.
- `commitPreparedImportAsPlacement()` still receives explicit placement from editor code.
- No new processor-side track inference or Arrangement-state hover owner exists.

---

## L2 - Focused Unit And Source Guards

Build command:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

Focused suites:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe arrangement-contract timeline-rendering
```

Required coverage:

- `StandaloneImportDrop_UsesDropTrackInsteadOfPrompt`
- `StandaloneImportDrop_BlankArrangementAreaTargetsNewTrack`
- `StandaloneImportDrop_OutOfArrangementFallsBackToActiveTrack`
- `StandaloneImportChooser_SingleFileRemainsActiveTrackFastPath`
- `StandaloneImportDrop_PreviewIsTransientOnly`

Expected result: all focused tests PASS.

---

## L3 - Integration Gate

Commands:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe arrangement-contract timeline-rendering
```

Required checks:

- Import placement still enters shared core only through explicit Standalone editor decisions.
- No preview state leaks into `StandaloneArrangement`, processor serialization, undo history, or playback snapshots.
- Track-count growth for blank-area drop remains bounded by `MAX_TRACKS`.
- Existing Arrangement rendering and selection contracts remain PASS.

Expected result: all focused suites PASS.

---

## L4 - Standalone Manual Smoke

Build command:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

Journey:

1. Launch `build-ara-overlay-vs18-clean\OpenTune_artefacts\Release\Standalone\OpenTune.exe` from the project root.
2. Ensure Arrangement shows at least two visible tracks.
3. Drag a supported audio file onto track 1.
4. Confirm the clip lands on track 1 with no track-choice popup.
5. Drag a supported audio file onto track 2.
6. Confirm the clip lands on track 2 with no track-choice popup.
7. Drag a supported audio file into blank Arrangement space below the last visible track.
8. Confirm one new visible track is created and the clip lands there.
9. Repeat when already at `MAX_TRACKS`.
10. Confirm the app surfaces a direct limit message and does not open the legacy track picker.
11. Use chooser import for one file.
12. Confirm it still imports into the active track without a track-choice popup.

User confirmation required:

```text
Journey "Standalone import track-target drop UX" has been checked.
Evidence: [screenshots/logs/notes]
Does this match the expected behavior? (match / does not match)
```

Expected result: PASS only after explicit user confirmation.

---

## L5 - Regression Notes

Do not use the broad `OpenTuneTests.exe ui` runner as the primary proof gate until its exit-code/no-`[FAIL]` issue is separately resolved.

Useful additional checks if the patch touches Arrangement hover rendering:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe timeline-rendering
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
```

## Exit Condition

Verification is complete only when:

- L1 static contracts pass;
- L2 focused tests pass;
- L3 integration gates pass;
- L4 manual smoke is either user-confirmed PASS or explicitly recorded as pending.
