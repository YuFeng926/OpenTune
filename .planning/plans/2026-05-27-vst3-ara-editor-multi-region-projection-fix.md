# 2026-05-27 VST3 ARA Editor Multi-Region Projection Fix

## Scope

Fix the REAPER ARA multi-item display symptom where the VST3 editor showed only the last/preferred item even though
ARA materialization birth and F0 extraction had already run for multiple playback regions.

This is intentionally not a birth, restore, or host-special-case fix.

## Root Cause

`PluginEditor::resolveCurrentMaterializationSync()` still used `resolvePreferredAraRegionView()` as both:

- the active editable materialization selector; and
- the display placement collection source.

That collapsed the PianoRoll timeline to one `preferredRegion`. In REAPER multi-item projects, preferred is last-wins, so
the editor displayed only the last item.

## Fixed Contract

- `VST3AraSession` remains the ARA lifecycle owner.
- `AudioModification persistentId` remains the editable materialization owner.
- ARA-bound `PluginEditor` builds PianoRoll display placements from `snapshot->publishedRegions`.
- `preferredRegion` only selects `activeMaterializationId`.
- Active/focus determines the current editable materialization only; it must not filter the displayed ARA playback regions.

## Files Changed

- `Source/Plugin/PluginEditor.cpp`
- `Tests/TestMain.cpp`

## Verification

- Added RED/GREEN architecture guard: `AraEditor_BuildsPlacementsFromAllPublishedRegions`.
- `cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"` PASS.
- `build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture` PASS.
- `build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe core` PASS.
- `build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor` PASS.
- `cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"` PASS.
- `git diff --check` PASS except existing CRLF normalization warnings.

`OpenTuneTests.exe ui` was run only to verify the touched AppPreferences test. It still has unrelated existing source-guard
failures and must not be reported as a full UI PASS.

REAPER manual L5 was not executed by Codex in this session and remains a host validation item.
