# Studio One ARA Stopped Renderer Gate Plan

**Date:** 2026-05-17
**Status:** Implemented; Studio One L5 pending
**Verification source:** `.planning/plans/2026-05-17-studio-one-ara-stopped-render-gate-test-verification.md`

## Goal

Fix the Studio One-only ARA playback-chain bug where a repeated monotone sample continues sounding while the host transport is paused or stopped, then disappears as soon as playback starts.

The fix must be structural: the ARA playback renderer must honor realtime host transport state before reading materialization audio. It must not be implemented as a VST3 editor workaround, a non-ARA playback fallback, or a host-specific compatibility path.

## Evidence

The Studio One log shows the host reporting stopped transport while still calling the ARA playback renderer:

```text
HostTransportSnapshot: playing=false time=80.000000
ARA PlaybackRenderer: First processBlock call, regions count = 1 hostSampleRate=96000
ARA Mapping: playbackTime=80.000000 mappedLocalTime=80.000000 mappedLocalSampleForLog=3528000 hostSampleRate=96000.0 sourceSampleRate=44100.0
ARA Mapping: playbackTime=80.000000 mappedLocalTime=80.000000 mappedLocalSampleForLog=3528000 hostSampleRate=96000.0 sourceSampleRate=44100.0
```

The repeated mapping is not a 96k/44.1k time-domain error: `80 * 44100 = 3528000`, so the mapped source sample is consistent. The problem is that the same frozen realtime host time is rendered repeatedly while `playing=false`.

## Root Cause

`OpenTuneAudioProcessor::processBlock()` routes bound ARA instances through JUCE `processBlockForARA(...)` before the normal non-ARA stopped-state silence branch can run.

JUCE's ARA extension calls `ARAPlaybackRenderer::processBlock(...)` for bound ARA instances. `OpenTunePlaybackRenderer::processBlock(...)` clears the output buffer, but then renders any published playback region that overlaps `positionInfo.getTimeInSeconds()`. It does not check `positionInfo.getIsPlaying()`.

In REAPER this may not reproduce because the host does not route stopped-state ARA renderer output the same way. Studio One exposes the missing renderer contract by continuing to pull realtime blocks at a frozen playhead position.

## Required Behavior

- Realtime ARA playback renderer blocks must be silent when host transport is not playing.
- Realtime ARA playback renderer blocks must render normally when host transport is playing.
- Non-realtime ARA rendering must remain allowed, because JUCE ARA readers and export/analysis paths can use non-realtime processing and may set their own position info.
- The gate belongs in `Source/ARA/OpenTunePlaybackRenderer.*`; editor transport buttons and processor fallback branches are consumers, not the ARA playback truth owner.

## Implementation Plan

1. Add a small testable predicate near `OpenTunePlaybackRenderer`, for example:

   ```cpp
   bool shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime realtime,
                                     const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept;
   ```

2. Predicate semantics:

   - `Realtime::yes` and `getIsPlaying() == false` -> false.
   - `Realtime::yes` and `getIsPlaying() == true` -> true.
   - `Realtime::no` -> true.

3. Call the predicate at the start of `OpenTunePlaybackRenderer::processBlock(...)`, before region mapping, `getTimeInSeconds()`, and `readPlaybackAudio(...)`.

4. If the predicate rejects a block:

   - clear the buffer,
   - optionally emit a bounded `ARA RenderGate` diagnostic for Studio One verification,
   - return `true` from `processBlock(...)` to report ARA-handled silence, not a request for non-ARA fallback,
   - return without reading materialization audio.

5. Add focused `OpenTuneTests` coverage matching `.planning/plans/2026-05-17-studio-one-ara-stopped-render-gate-test-verification.md`.

## Non-Goals

- Do not change `PluginEditor` transport behavior.
- Do not add Studio One-specific host detection.
- Do not change non-ARA playback stop/fade behavior.
- Do not change region/materialization binding or ARA archive behavior.
- Do not disable offline/non-realtime ARA rendering.

## Manual Verification

After implementation and build verification, the remaining decisive check is Studio One L5:

- stopped and paused inside a renderable ARA region must be silent,
- Play must still produce normal ARA playback,
- new logs should show render-gate silence instead of repeated frozen `ARA Mapping` lines.

This L5 host journey remains pending until the user confirms the rebuilt plugin in Studio One.
