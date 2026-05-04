---
phase: 10-state-visualization-consistency-guard
plan: 02
subsystem: core
tags: [diagnostics, processor, transport, STAB-03, SAFE-01]

requires:
  - phase: 10-01
    provides: Shared render-status snapshot contract and editor polling points

provides:
  - Read-only `DiagnosticInfo` snapshot API on `OpenTuneAudioProcessor`
  - Last transport control tracking via `recordControlCall`
  - Editor-side debug heartbeat that reports edit/revision/control-call facts

affects: [10-03, verification, debugging]

tech-stack:
  added: []
  patterns:
    - "Processor exposes minimal consistency facts as read-only query API"
    - "Editors record transport intent explicitly rather than inferring from UI state"

key-files:
  created: []
  modified:
    - Source/PluginProcessor.h
    - Source/PluginProcessor.cpp
    - Source/Plugin/PluginEditor.cpp
    - Source/Standalone/PluginEditor.cpp

key-decisions:
  - "D10-02-01: DiagnosticInfo resolves by trackId + clipId so VST3 and Standalone can query the exact active clip"
  - "D10-02-02: recordControlCall is side-effect free with respect to transport state to satisfy SAFE-01"

patterns-established:
  - "Pattern: getDiagnosticInfo(trackId, clipId) returns edit/revision/control-call facts without mutating RenderCache"
  - "Pattern: transport UI entrypoints call recordControlCall alongside host/control requests"

requirements-completed: [STAB-03]

duration: 19min
completed: 2026-04-08
---

# Phase 10 Plan 02: Diagnostic Consistency API Summary

**Processor-level diagnostic snapshots now expose edit version, clip revision state, and last transport control call so consistency issues can be inspected without ad-hoc logging.**

## Performance

- **Duration:** 19 min
- **Started:** 2026-04-08T03:50:00Z
- **Completed:** 2026-04-08T04:09:50Z
- **Tasks:** 4
- **Files modified:** 4

## Accomplishments

- `OpenTuneAudioProcessor` now exposes `DiagnosticInfo` and `recordControlCall()` as a shared read-only diagnostic surface
- VST3 and Standalone editor transport entrypoints record play/pause/stop/seek intent explicitly
- Editors log render status + revision/control-call heartbeats in debug builds, making STAB-03 facts directly observable
- `Tests/TestMain.cpp` now verifies edit-version tracking, clip revision reporting, read-only diagnostic queries, and transport immutability

## Task Commits

Implementation for this plan was already present in the workspace before this execute-phase pass.

- **Code commits:** None created during this run
- **Plan metadata:** will be committed together with Phase 10 execution docs

## Files Created/Modified

- `Source/PluginProcessor.h` - `DiagnosticControlCall`, `DiagnosticInfo`, and public diagnostic API declarations
- `Source/PluginProcessor.cpp` - diagnostic snapshot assembly and control-call recording
- `Source/Plugin/PluginEditor.cpp` - VST3 transport requests record control intent and emit heartbeat logs
- `Source/Standalone/PluginEditor.cpp` - Standalone transport requests record control intent and emit heartbeat logs

## Decisions Made

- The diagnostic API was widened from `trackId` only to `trackId + clipId` so the caller can interrogate the exact clip it is rendering, which is necessary for VST3 single-clip workflows and multi-track Standalone selection
- `recordControlCall()` intentionally does not mutate transport position or play state; this preserves SAFE-01 while still capturing the latest requested control

## Deviations from Plan

- The plan sketched a narrower `getDiagnosticInfo(int trackId = 0)` API. The shipped implementation adds optional `clipId` targeting because the editor already tracks clip context and this avoids ambiguous selected-clip lookups.

## Issues Encountered

- None in automated verification. Diagnostic API and tests were already present and passed unchanged.

## User Setup Required

- None - no external service configuration required.

## Next Phase Readiness

- STAB-03 diagnostic facts are implemented and automated tests pass
- SAFE-01 manual verification still remains before final phase gate closure

## Self-Check: PASSED (automation)

- ✓ Diagnostic API symbols verified in source
- ✓ Diagnostic unit/integration tests passed in `OpenTuneTests`
- ✓ `recordControlCall()` was validated as read-only with respect to transport state

---
*Phase: 10-state-visualization-consistency-guard*
*Completed: 2026-04-08*
