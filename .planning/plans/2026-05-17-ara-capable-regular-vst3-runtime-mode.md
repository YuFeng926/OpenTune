# ARA-Capable Regular VST3 Runtime Mode Plan

**Date:** 2026-05-17
**Status:** Implemented; host L5 pending
**Verification source:** `.planning/plans/2026-05-17-ara-capable-regular-vst3-runtime-mode-test-verification.md`

## Goal

Fix the Studio One track-insert `Read Audio` failure:

```text
Read Audio
Unable to access VST3 ARA DocumentController.
```

The fix must model the ARA SDK distinction between an ARA-capable binary and an ARA-bound instance. A VST3 build that supports ARA can still be loaded by a host as a regular VST3 insert. In that mode the plugin must use the regular VST3 capture path, not the ARA `DocumentController` path.

## Host Behavior Survey

### Studio One

Studio One normal track inserts can instantiate the ARA-capable VST3 as a regular VST3 instance. The short failure logs from `C:\Users\FY\AppData\Roaming\OpenTune\Logs` show only `OpenTuneAudioProcessor: ctor` / `prepareToPlay`, with no `ARA: DocumentController created` and no `ARA: didBindToARA`. That makes `getDocumentController() == nullptr` expected for this instance.

Studio One ARA workflows still bind correctly: the successful log contains `DocumentController created`, `didBindToARA`, a published snapshot, and `RecordTrace: VST3 recordRequested materializationId=...`.

### REAPER

REAPER can bind ARA-capable VST3 plug-ins from the track FX workflow when its ARA preference is enabled. This is why the same OpenTune binary can behave as ARA-bound on a REAPER track insert while staying regular VST3 on a Studio One track insert.

### Cubase

Cubase/Nuendo use an `Extensions` workflow for ARA. Steinberg's current Cubase 15 help describes activating an extension for an audio track through the audio track Inspector; the result is that the extension is active for all audio events on that track. Steinberg also documents event-level activation through the Editor.

This means Cubase is not the same as REAPER's plain FX-slot ARA binding model. It is also not identical to Studio One, because Cubase can activate an extension across a track, but it is still an ARA extension workflow, not a normal channel-strip insert.

Primary reference:

- `https://www.steinberg.help/r/cubase-pro/15.0/en/cubase_nuendo/topics/ara_integration/extensions_for_audio_tracks_activating_t.html`
- `https://www.steinberg.help/r/cubase-pro/15.0/en/cubase_nuendo/topics/ara_integration/ara_extension_for_further_events_activating_t.html`

### Ableton Live

Ableton Live currently exposes regular plug-in formats such as VST2, VST3, and AU, but not an ARA extension workflow. Celemony's Live-specific Melodyne documentation instructs users to load Melodyne as an insert effect and use Transfer to record audio into the plug-in. Its ARA documentation separately states that DAWs with ARA avoid the transfer procedure.

Therefore Live should be treated as regular VST3 only for OpenTune. It will never provide an ARA `DocumentController` unless Ableton adds an ARA host workflow in a future version.

Primary reference:

- `https://help.ableton.com/hc/en-us/articles/5937501570460-Supported-Plug-in-Formats`
- `https://helpcenter.celemony.com/M5/doc/melodyneStudio5/en/M5tour_DAW_Live?env=live%2F1000`
- `https://helpcenter.celemony.com/M5/doc/melodyneEssential5/en/M5tour_TransferARA?env=live`

## Root Cause

Current code treats `JucePlugin_Enable_ARA` as if every plugin instance is ARA-bound:

- `Source/PluginProcessor.cpp` creates `captureSession_` only inside `#if !JucePlugin_Enable_ARA`.
- `Source/Plugin/PluginEditor.cpp::recordRequested()` only offers the capture path inside `#if !JucePlugin_Enable_ARA`.
- In ARA builds, `recordRequested()` immediately asks `processorRef_.getDocumentController()`. For a regular VST3 track-insert instance this is null by design, so the UI shows the `Unable to access VST3 ARA DocumentController` dialog.

The incorrect assumption is:

```text
ARA build == ARA-bound instance
```

The correct runtime contract is:

```text
ARA-capable VST3 binary + host bindToDocumentController* call == ARA-bound instance
ARA-capable VST3 binary without bindToDocumentController* == regular VST3 instance
```

## Required Behavior

- If the instance is ARA-bound, `Read Audio` means focused ARA region refresh/ensure and must continue through `VST3AraSession`.
- If the instance is not ARA-bound but is a VST3 insert, `Read Audio` means regular VST3 capture using `CaptureSession`.
- ARA-bound mode must not read or display stale regular capture segments.
- Regular VST3 mode must not show ARA `DocumentController` errors.
- Standalone must remain isolated from Plugin editor behavior.
- The fix must not introduce host-name detection, Studio One-specific branches, or a compatibility shim.

## Implementation Plan

### 1. Convert CaptureSession Into VST3 Regular Mode State

Modify `Source/PluginProcessor.cpp` constructor so VST3 instances create `captureSession_` regardless of `JucePlugin_Enable_ARA`.

Keep the existing `wrapperType == juce::AudioProcessor::wrapperType_VST3` runtime boundary. Do not create capture state for Standalone.

Update comments from "non-ARA VST3 only" to "regular VST3 mode". This is not fallback; it is the normal mode for unbound ARA-capable VST3 instances.

### 2. Gate CaptureSession Access By ARA Binding

Move `getCaptureSession()` out of the inline header if needed, and make it return a session only when the current instance is in regular VST3 mode.

Expected contract:

```cpp
#if JucePlugin_Enable_ARA
if (isBoundToARA())
    return nullptr;
#endif
return captureSession_.get();
```

Use this public accessor in editor timer, editor materialization resolution, state serialization, and `processBlock()` regular VST3 path. This prevents an ARA-bound instance from consuming stale regular capture state.

### 3. Runtime-Split recordRequested()

Modify `Source/Plugin/PluginEditor.cpp::recordRequested()` to branch by runtime state rather than build macro:

1. If `processorRef_.getCaptureSession()` returns a session, run the existing capture arm/stop/processing state machine and log `VST3 recordRequested mode=regular-vst3`.
2. Else, in ARA builds, require `getDocumentController()` and continue the existing ARA focused-region path.
3. If neither mode exists, show an accurate mode error such as `This VST3 instance is not ready for audio capture or ARA reading.`

Do not show `Unable to access VST3 ARA DocumentController` for regular VST3 track-insert mode.

### 4. Runtime-Split processBlock()

Keep the current ARA-first audio routing:

```cpp
#if JucePlugin_Enable_ARA
if (isBoundToARA() && processBlockForARA(...))
    return;
#endif
```

Then run the regular VST3 capture path through `getCaptureSession()`. This preserves ARA playback for bound instances and enables capture for unbound instances in Studio One, Live, and regular insert contexts.

### 5. Serialization Boundary

Regular VST3 capture state should serialize only when `getCaptureSession()` returns a session. ARA-bound state remains owned by ARA document/session archive paths.

This avoids parallel state owners inside a bound ARA document.

### 6. Diagnostics

Add precise logs:

```text
VST3 recordRequested mode=regular-vst3 processor=...
VST3 recordRequested mode=ara-bound processor=... dc=...
ARA: didBindToARA processor=... dc=...
OpenTuneAudioProcessor: dtor processor=... araBound=...
```

The goal is to make the next Studio One/Cubase/Live log distinguish host binding strategy from OpenTune errors in one pass.

## Non-Goals

- Do not add host-name detection.
- Do not make a separate Studio One branch.
- Do not make Live appear ARA-capable.
- Do not change ARA playback renderer stopped-state gate.
- Do not move ARA sample access out of `VST3AraSession`.
- Do not let an ARA-bound instance consume regular capture segments.

## Expected Host Matrix

| Host workflow | Expected OpenTune runtime mode |
|---|---|
| REAPER track FX with ARA enabled | ARA-bound |
| Studio One normal track insert | Regular VST3 |
| Studio One ARA/Event workflow | ARA-bound |
| Cubase extension on event/track | ARA-bound |
| Cubase normal channel insert | Regular VST3 |
| Ableton Live VST3 insert | Regular VST3 |

## Manual Verification

- Studio One normal track insert: `Read Audio` arms/stops regular capture; no ARA `DocumentController` dialog.
- Studio One ARA workflow: existing ARA `Read Audio` region refresh still works.
- REAPER ARA track FX: still ARA-bound and does not use regular capture.
- Cubase extension workflow: ARA-bound if OpenTune is exposed as an extension.
- Live VST3 insert: regular capture workflow.

L5 host journeys require user confirmation before they are marked PASS.
