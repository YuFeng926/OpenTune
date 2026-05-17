# ARA-Capable Regular VST3 Runtime Mode Test Verification

**Date:** 2026-05-17
**Status:** Automated checks pass; host L5 pending
**Related plan:** `.planning/plans/2026-05-17-ara-capable-regular-vst3-runtime-mode.md`

## Scope

Verify that an ARA-capable VST3 binary handles both official runtime modes:

- ARA-bound instance: host has called `bindToDocumentController*()`.
- Regular VST3 instance: host has loaded the same binary without ARA binding.

The Studio One failure logs prove the failing track-insert path is regular VST3 mode: they contain processor construction and prepare calls, but no `DocumentController created` or `didBindToARA`.

## Requirements

- **ARA-RUNTIME-01:** VST3 instances create regular capture state independently of the ARA build flag.
- **ARA-RUNTIME-02:** ARA-bound instances do not expose or consume regular capture state.
- **ARA-RUNTIME-03:** `recordRequested()` chooses regular VST3 capture when the instance is not ARA-bound and has a capture session.
- **ARA-RUNTIME-04:** `recordRequested()` chooses ARA focused-region refresh when the instance is ARA-bound.
- **ARA-RUNTIME-05:** regular VST3 mode never reports `Unable to access VST3 ARA DocumentController`.
- **ARA-RUNTIME-06:** ARA state serialization remains owned by ARA document/session paths; regular capture persistence only runs in regular VST3 mode.
- **ARA-RUNTIME-07:** existing ARA renderer stopped-state gate keeps passing.

## L1 Static Validation

Command:

```powershell
git diff --check
```

Expected:

- Exit code 0.
- No whitespace or patch-format errors.

## L2 Contract Tests

Build command:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

Run command:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
```

Expected focused guards:

- `PluginProcessor.cpp` no longer compiles capture session construction only under `#if !JucePlugin_Enable_ARA`.
- `PluginEditor.cpp::recordRequested()` no longer wraps the capture state machine in `#if !JucePlugin_Enable_ARA`.
- `recordRequested()` contains a regular VST3 mode branch before ARA `DocumentController` error handling.
- `getCaptureSession()` or the equivalent mode accessor suppresses capture exposure when `JucePlugin_Enable_ARA && isBoundToARA()`.
- Serialization paths use the runtime capture accessor rather than directly serializing `captureSession_` for ARA-bound instances.
- The old `Unable to access VST3 ARA DocumentController` text is not reachable as the first response to a regular VST3 capture request.

## L3 Integration Smoke

Build commands:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```

Expected:

- ARA VST3 build succeeds.
- non-ARA VST3 build succeeds.
- No Standalone-only UI or packaging path is touched.

## L4 Regression Scope

Run commands:

```powershell
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe core
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
```

Expected:

- Existing ARA stopped render gate tests still pass.
- Capture persistence tests still pass.
- ARA multi-region binding guards still pass.
- Processor state version/persistence tests do not regress.

## L5 Host Journeys

These require user confirmation before marking PASS.

### Studio One Normal Track Insert

1. Load the rebuilt ARA-capable VST3 on a normal Studio One track insert.
2. Click `Read Audio`.
3. Confirm it arms/stops regular VST3 capture instead of showing the ARA `DocumentController` dialog.
4. Confirm logs contain `mode=regular-vst3` and do not contain `DocumentController created` for that instance.

### Studio One ARA Workflow

1. Load OpenTune through Studio One's ARA workflow.
2. Confirm logs contain `DocumentController created` and `didBindToARA`.
3. Click `Read Audio`.
4. Confirm focused-region refresh still reaches `RecordTrace: VST3 recordRequested materializationId=...`.

### REAPER ARA Track FX

1. Load OpenTune as a REAPER ARA-enabled track FX.
2. Confirm logs contain `mode=ara-bound`.
3. Confirm regular capture logs do not appear for this bound instance.

### Cubase Extension Workflow

1. Activate OpenTune through Cubase's extension workflow on an event or audio track.
2. Confirm ARA binding logs appear.
3. Confirm `Read Audio` follows ARA focused-region refresh.

### Ableton Live VST3 Insert

1. Load OpenTune as a VST3 insert in Live.
2. Click `Read Audio`.
3. Confirm regular capture mode is used.
4. Confirm no ARA `DocumentController` dialog appears.

## L6 Wider Matrix

Commands:

```powershell
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe core
build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe processor
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"
```

Expected:

- All listed commands pass.
- `ui` suite remains outside the pass claim until the existing runner exit-code issue is explained.
