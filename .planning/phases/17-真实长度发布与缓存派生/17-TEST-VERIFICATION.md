# Phase 17 Test Verification

## Scope

Phase 17 focuses on sample-authoritative cache publishing: base cache must persist only trimmed real audio, resampled cache must derive from that same true span, and all display/debug time values must be projected from sample truth rather than reintroduced as persistent seconds facts.

## Verification Levels

| Level | Goal | Command | Status | Notes |
| --- | --- | --- | --- | --- |
| L1 | Configure and build `OpenTuneTests` against the current Phase 17 code | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build -Target OpenTuneTests` | Passed | Root cause fix: `OpenTuneTests` now gets explicit MSVC/UCRT/UM link directories, so the prior `_purecall` / `operator new/delete` / `atexit` linker gate is gone. Final build completed after the dynamically discovered MSVC developer shell configured the Ninja build. |
| L2 | Run the unit/integration regression binary | `.\build\OpenTuneTests.exe` | Passed | Full suite passed, including the new `Phase 17: Real-Length Cache Publish Tests` section. |
| L4 | Static contract audit for sample-authoritative publish semantics | `rtk grep "startSample|endSampleExclusive|addChunk\(|addResampledChunk\(|runPhase17RealLengthPublishTests|PublishedChunk" "Source/Inference/RenderCache.h" "Source/Inference/RenderCache.cpp" "Source/PluginProcessor.cpp" "Tests/TestMain.cpp"` | Passed | Confirms RenderCache publish APIs, published chunk contract, worker publish path, and Phase 17 regression entrypoint all exist in code. |
| L6 | Re-run the full regression suite after all fixes | `.\build\OpenTuneTests.exe` | Passed | Reused the fresh L2 binary after the deterministic regression fixes; no failing suites remained. |

## Required Journeys

- `TASK_05_BaseChunkPublishesTrimmedTrueSampleSpan`
- `TASK_05_ResampledCacheUsesBaseSampleProjection`
- `SAMP_03_PublishedChunkDisplayTimeIsProjectedFromSamples`

## L5 Applicability

- L5: Not applicable.
- Reason: Phase 17 changes shared cache publish semantics and diagnostic/export projections only. No manual UI or host journey is introduced in this plan.

## Evidence Captured

- The linker gate was real and fixed structurally:
  - Before the fix, `OpenTuneTests.exe` failed to link with unresolved `_purecall`, `operator new/delete`, `atexit`, `__std_terminate`, and `free`.
  - Investigation showed `CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES` was empty and the `VsDevCmd.bat` call path did not leave usable `LIB` state for the later Ninja link step.
  - `CMakeLists.txt` now derives and injects MSVC + Windows SDK runtime link directories from `CMAKE_CXX_COMPILER` and `CMAKE_MT`.
- Full `OpenTuneTests.exe` run passed after the fix, including:
  - `TASK_05_BaseChunkPublishesTrimmedTrueSampleSpan`
  - `TASK_05_ResampledCacheUsesBaseSampleProjection`
  - `SAMP_03_PublishedChunkDisplayTimeIsProjectedFromSamples`
- Legacy regressions were updated to the new sample-truth semantics:
  - partial resampled fallback now expects rendered fallback inside the true sample span before dry fallback
  - hop-aligned chunk tests now call the pure chunk-boundary helper directly so they no longer race the live render worker thread

## Final Gate

- PASS: L1, L2, L4, and L6 all passed with fresh evidence.
- PASS: Phase 17 requirement evidence is now complete for `TASK-05` and `SAMP-03`.
