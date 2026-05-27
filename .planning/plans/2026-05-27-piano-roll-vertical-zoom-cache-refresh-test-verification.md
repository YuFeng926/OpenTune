# PianoRoll Vertical Zoom Cache Refresh Test Verification

Date: 2026-05-27
Status: ACTIVE

## Scope

Bug: vertical zoom in the PianoRoll does not refresh immediately and can draw visible gaps between piano-key atlas slices until another interaction forces a refresh.

The fix must preserve the timeline rendering pipeline: `paint()` consumes a prepared render model, and state-change paths prepare/invalidate the model outside paint.

## Verification Levels

### L1 Static Contract

Command:

```bat
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& build-ara-overlay-vs18-clean\OpenTuneTests\Release\OpenTuneTests.exe timeline-rendering"
```

Checks:

- `PianoRollRenderModelCache::Key` includes vertical zoom and vertical scroll geometry.
- Vertical zoom, vertical wheel scroll, panning, scrollbar movement, and fit-to-screen vertical changes call the same prepared-model refresh helper.
- `PianoRollRenderer::RenderContext` uses snapshot coordinate lambdas for `midiToY` and `freqToY`; they must not capture `this`.
- `paint()` still does not build render-model data.

### L2 Focused Visual Contract

Command:

```bat
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& build-ara-overlay-vs18-clean\OpenTuneTests\Release\OpenTuneTests.exe piano-roll-f0-visual"
```

Checks:

- Existing prepared F0 visual contracts still pass after vertical geometry enters the render-model key.
- No parallel F0 drawing path is introduced.

### L1 Compile Gate

Command:

```bat
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"
```

Checks:

- The edited PianoRoll component, render model cache, and tests compile under the active VS/MSBuild build tree.

## Non-Goals

- Do not modify the piano-key atlas or hide gaps by inflating key rectangles.
- Do not rebuild render models from `paint()`.
- Do not redesign horizontal scroll dirty-rect behavior in this patch.
- Do not claim `OpenTuneTests.exe ui` or full-suite PASS from this focused fix.
