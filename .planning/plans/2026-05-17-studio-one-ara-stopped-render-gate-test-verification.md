# Studio One ARA Stopped Renderer Gate Test Verification

**Date:** 2026-05-17
**Status:** Implemented; Studio One L5 pending
**Related plan:** `.planning/plans/2026-05-17-studio-one-ara-stopped-render-gate.md`

## Scope

Verify the Studio One stopped/pause ARA playback noise fix at the renderer contract boundary.

The observed host trace is:

- `HostTransportSnapshot: playing=false time=80.000000`
- repeated `ARA Mapping: playbackTime=80.000000 mappedLocalTime=80.000000 mappedLocalSampleForLog=3528000`

This means Studio One can keep calling the ARA playback renderer while the transport is stopped. The test source of truth is therefore the renderer gate, not VST3 editor controls or the non-ARA playback branch.

## Requirements

- **ARA-GATE-01:** realtime ARA playback blocks must not render materialization audio when `PositionInfo::getIsPlaying()` is false.
- **ARA-GATE-02:** realtime ARA playback blocks must continue rendering when `getIsPlaying()` is true.
- **ARA-GATE-03:** non-realtime ARA rendering must remain allowed so ARA offline readers, export, and analysis paths are not muted by the Studio One realtime gate.
- **ARA-GATE-04:** the gate must run before playback-region overlap mapping and before `OpenTuneAudioProcessor::readPlaybackAudio()`.
- **ARA-GATE-05:** when the gate handles a stopped realtime block, `OpenTunePlaybackRenderer::processBlock(...)` must return `true` after clearing the buffer, because `false` means non-ARA fallback is required.
- **ARA-GATE-06:** Studio One manual verification must use `AppLogger` evidence and user confirmation before this host journey is marked PASS.

## L1 Static Validation

Command:

```powershell
git diff --check
```

Expected:

- Exit code 0.
- No whitespace or patch-format errors.

## L2 Targeted Unit/Contract Tests

Build command:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

Run command:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

Expected focused coverage:

- `OpenTunePlaybackRenderer` exposes or contains a small gate decision that returns false for realtime stopped blocks.
- The same decision returns true for realtime playing blocks.
- The same decision returns true for non-realtime stopped blocks.
- Source inspection proves the gate appears before `getTimeInSeconds()`, `ARA Mapping`, and `readPlaybackAudio()`.
- Source inspection proves the stopped realtime gate branch clears the buffer and returns `true`, not `false`.

## L3 Integration Smoke

Build command:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```

Expected:

- ARA VST3 build succeeds.
- No new source dependency leaks into Standalone-only code.

## L4 Regression Scope

Run commands:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe core
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

Expected:

- Existing ARA renderer block-span and ARA binding guards still pass.
- Processor and core playback read tests do not regress.

## L5 Studio One Manual Journey

Manual DAW journey, user-confirmed:

1. Install or load the rebuilt ARA VST3 in Studio One.
2. Open the same project shape that reproduced the issue.
3. Put Studio One transport in stop and pause states while the playhead is inside an ARA playback region.
4. Confirm there is no repeated monotone sample noise.
5. Press Play and confirm normal ARA playback still works.
6. Inspect `C:\Users\FY\AppData\Roaming\OpenTune\Logs\OpenTune*.log`.

Expected log evidence:

- If Studio One keeps calling the renderer stopped, logs should show a limited `ARA RenderGate` silence line with `playing=false` and realtime mode.
- Stopped-state logs must no longer show repeated `ARA Mapping` lines for the same frozen playback time after the gate.

Pass rule:

- Do not mark this L5 journey PASS until the user confirms the audible behavior in Studio One.

## L6 Wider Build Matrix

Commands:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```

Expected:

- non-ARA VST3 still builds.
- ARA VST3 still builds.
- The fix remains ARA renderer scoped and does not change non-ARA playback semantics.
