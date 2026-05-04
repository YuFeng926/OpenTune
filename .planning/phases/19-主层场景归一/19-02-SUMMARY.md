# Phase 19-02 Summary

## Completed Work

- Removed the PianoRoll-specific `PlayheadOverlayComponent` dependency from `Source/Standalone/UI/PianoRollComponent.h` and `Source/Standalone/UI/PianoRollComponent.cpp`.
- Added `mainLayerPlayheadSeconds_` and `playheadColour_` as PianoRoll-owned playhead state.
- Extended `PianoRollRenderer::RenderContext` with `showPlayhead`, `playheadSeconds`, and `playheadColour`.
- Added `PianoRollRenderer::drawPlayhead(...)` and wired `PianoRollComponent::paint()` to draw the playhead inside the clipped content region.
- Redirected non-playing playhead updates and VBlank playhead updates to the main component repaint path instead of an overlay child.

## Verification

- Static audit: `PianoRollComponent.h` / `PianoRollComponent.cpp` no longer contain `PlayheadOverlayComponent` or `playheadOverlay_`.
- Static audit: `mainLayerPlayheadSeconds_`, `drawPlayhead`, `playheadSeconds`, and `playheadColour` are present in the expected PianoRoll files.

## Notes

- This summary documents `19-02-PLAN.md` work. It was previously stored as `19-01-SUMMARY.md`, which conflicted with `19-01-PLAN.md` because `19-01` is the test-contract and regression-definition wave.
- Phase 19 intentionally leaves `requestInteractiveRepaint()`, `FrameScheduler`, heartbeat, and timer structure in place; those remain for Phase 20-21.
