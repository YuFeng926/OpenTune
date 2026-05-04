---
phase: 21-单 VBlank 视觉循环
plan: 03
subsystem: editor-shell
tags: [standalone, vst3, editor, visual-clock]
requires:
  - phase: 21-单 VBlank 视觉循环
    provides: single-VBlank PianoRoll visual loop
provides:
  - Standalone shell without direct PianoRoll invalidate
  - Static proof that VST3 shell does not call PianoRoll visual APIs
affects: [21-04]
tech-stack:
  added: []
  patterns: [editor-shell-boundary, no-direct-invalidate, no-visual-clock-delegation]
key-files:
  created: [.planning/phases/21-单 VBlank 视觉循环/21-03-SUMMARY.md]
  modified: [Source/Standalone/PluginEditor.cpp, Source/Plugin/PluginEditor.cpp]
key-decisions:
  - "Standalone import completion now refreshes arrangement shell state only; PianoRoll repaint stays self-driven by VBlank."
  - "VST3 editor timer remains a transport/state shell; the obsolete PianoRoll heartbeat call is removed and the user-approved file boundary also captures same-file plugin clip API convergence already present in the dirty tree."
patterns-established:
  - "Editor timers may poll transport and business state, but never proxy PianoRoll visual cadence."
  - "Static audit is the binding proof for editor-shell boundary claims in Phase 21."
requirements-completed: [CLOCK-02, FLOW-02]
duration: 0h 0m
completed: 2026-04-15
---

# Phase 21 Plan 03: Editor Shell Boundary Summary

**Standalone and VST3 editor shells are now cleanly outside the PianoRoll visual loop: the Standalone import path no longer direct-invalidates `pianoRoll_`, and both editors statically prove they do not call PianoRoll visual-clock APIs**

## Performance

- **Duration:** 0h 0m
- **Started:** 2026-04-15T00:00:00Z
- **Completed:** 2026-04-15T00:00:00Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Removed the remaining Standalone editor path that called `FrameScheduler::instance().requestInvalidate(safeThis->pianoRoll_, ...)` after import completion.
- Kept `timerCallback()` in both editors scoped to transport/state, overlay, and business-shell duties only, and removed the stale `pianoRoll_.onHeartbeatTick()` caller from the VST3 shell.
- Verified by static audit that neither editor calls `pianoRoll_.onVisualVBlankCallback`, `pianoRoll_.flushPendingVisualInvalidation`, or `pianoRoll_.invalidateVisual`.

## Files Created/Modified
- `Source/Standalone/PluginEditor.cpp` - drops the import-complete direct invalidate for `pianoRoll_` and narrows the surrounding comment to shell-only refresh semantics.
- `Source/Plugin/PluginEditor.cpp` - removes the obsolete PianoRoll heartbeat call while preserving the existing VST3 transport/state shell flow; the same file-boundary commit also includes pre-existing plugin clip API convergence already present in the dirty tree.
- `.planning/phases/21-单 VBlank 视觉循环/21-03-SUMMARY.md` - captures the Wave 3 shell-boundary cleanup and audit outcome.

## Verification
- Standalone audit passed with zero matches for `FrameScheduler::instance().requestInvalidate(safeThis->pianoRoll_`, `pianoRoll_.onVisualVBlankCallback`, and `pianoRoll_.flushPendingVisualInvalidation` in `Source/Standalone/PluginEditor.cpp`.
- VST3 audit passed with zero matches for `pianoRoll_.onVisualVBlankCallback`, `pianoRoll_.flushPendingVisualInvalidation`, and `pianoRoll_.invalidateVisual` in `Source/Plugin/PluginEditor.cpp`.

## Decisions Made
- Removed only the direct invalidate residue in Standalone because that was the last concrete shell-to-PianoRoll visual write path.
- Removed the stale VST3 `pianoRoll_.onHeartbeatTick()` caller because Wave 2 deleted that component-side heartbeat API; under the user's file-boundary approval, the same commit also carries same-file VST3 clip API cleanup that was already present in the working tree.

## Deviations from Plan

`Source/Plugin/PluginEditor.cpp` carried pre-existing same-file changes outside the Phase 21 heartbeat cleanup. Because the user had already approved Phase 21 file-boundary commits, Wave 3 kept that boundary instead of attempting hunk isolation.

## Issues Encountered

None beyond confirming that the remaining `timerCallback()` methods are still legitimate transport/state polling paths rather than visual cadence proxies.

## Next Phase Readiness

- Wave 4 can now record fresh L1/L2/L4/L6 evidence and close Phase 21 against actual gate results.

## Self-Check: PASSED

- `Source/Standalone/PluginEditor.cpp` no longer contains `FrameScheduler::instance().requestInvalidate(safeThis->pianoRoll_`.
- `Source/Plugin/PluginEditor.cpp` is committed in the same live-tree state that fresh Phase 21 builds and audits used.
