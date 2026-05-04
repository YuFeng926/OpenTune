# Phase 17 Verification

## Requirement Traceability

| Requirement | Target files | Verification command | Pass criteria |
| --- | --- | --- | --- |
| TASK-05 | `Source/Inference/RenderCache.h`, `Source/Inference/RenderCache.cpp`, `Source/PluginProcessor.cpp`, `Tests/TestMain.cpp` | `.\build\OpenTuneTests.exe` | `TASK_05_BaseChunkPublishesTrimmedTrueSampleSpan` and `TASK_05_ResampledCacheUsesBaseSampleProjection` both pass, proving base cache only publishes trimmed true audio and resampled cache consumes the same true sample span. |
| SAMP-03 | `Source/Inference/RenderCache.h`, `Source/Inference/RenderCache.cpp`, `Tests/TestMain.cpp` | `rtk grep "startSample|endSampleExclusive|PublishedChunk|runPhase17RealLengthPublishTests" "Source/Inference/RenderCache.h" "Source/Inference/RenderCache.cpp" "Tests/TestMain.cpp" && .\build\OpenTuneTests.exe` | `SAMP_03_PublishedChunkDisplayTimeIsProjectedFromSamples` passes, and the static audit confirms `PublishedChunk` exposes sample boundaries plus projected `startSeconds/endSeconds`. |

## Additional Static Evidence

- `RenderCache::addChunk(...)` now accepts `startSample` / `endSampleExclusive` and rejects audio whose length does not equal `endSampleExclusive - startSample`.
- `RenderCache::addResampledChunk(...)` now attaches resampled audio to the same true sample span as the published base chunk.
- `RenderCache::getPublishedChunks()` now exports `startSample`, `endSampleExclusive`, and time values projected from those samples.
- worker publish in `Source/PluginProcessor.cpp` passes `FrozenRenderBoundaries.trueStartSample` and `trueEndSample` directly into cache storage.

## Gate Evaluation

| Gate | Result | Evidence |
| --- | --- | --- |
| Build | PASS | `pwsh -NoProfile -File ".planning/scripts/invoke-msvc-cmake.ps1" -BuildDir build -Target OpenTuneTests` completed successfully after the CMake runtime-link fix. |
| Automated tests | PASS | `.\build\OpenTuneTests.exe` passed, including all Phase 15/16 regressions plus the new Phase 17 section. |
| Static contract audit | PASS | `rtk grep "startSample|endSampleExclusive|addChunk\(|addResampledChunk\(|runPhase17RealLengthPublishTests|PublishedChunk" ...` confirms the publish contract and regression entrypoint are present. |

## Final Gate Template

- PASS: `TASK-05` and `SAMP-03` have direct code, build, and automated-test evidence.
- FAIL: Any Phase 17 regression test fails or `RenderCache` loses sample-authoritative publish symbols.
- BLOCKED: `OpenTuneTests` cannot be configured, built, or executed.

## Current Decision

- Gate status: PASS.
- Phase 17 automated evidence is complete for `TASK-05` and `SAMP-03`.
