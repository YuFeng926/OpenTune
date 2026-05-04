# Phase 15 Test Verification

## Scope

Phase 15 is a shared-core contract phase. Verification focuses on sample-domain truth, frozen render boundaries, and strict callback publish behavior. No UI or host E2E journey applies here, so L5 is not applicable.

## Verification Levels

| Level | Goal | Command | Status | Notes |
| --- | --- | --- | --- | --- |
| L1 | Build `OpenTuneTests` against current Phase 15 code | `cmake --build build --target OpenTuneTests --config Release` | Passed | Requires VS developer environment; actual run used `VsDevCmd.bat` before invoking CMake. |
| L2 | Run the full unit/integration regression binary | `.\build\OpenTuneTests.exe` | Passed | Single-config Ninja build produces `build\OpenTuneTests.exe`. |
| L4 | Static contract grep for frozen boundary and mismatch symbols | `rg "FrozenRenderBoundaries|preparePublishedAudioFromSynthesis|synthSampleCountMismatch" Source/PluginProcessor.h Source/PluginProcessor.cpp` | Pending rerun | Command is the authoritative static verification step for this phase. |
| L6 | Run the full regression suite after Phase 15 tests were added | `.\build\OpenTuneTests.exe` | Passed | Confirms new Phase 15 tests execute with the existing suite. |

## Path Notes

- The legacy multi-config command `.\build\Release\OpenTuneTests.exe` does not exist in the current `build/` tree.
- Phase 15 uses the real single-config output path `.\build\OpenTuneTests.exe` for L2/L6 execution.

## L5 Applicability

- L5: Not applicable.
- Reason: Phase 15 only changes shared core sample-boundary contracts and automated callback/cache behavior. No UI workflow or manual user journey is introduced.

## Evidence Captured

- Build succeeded with `cmake --build build --target OpenTuneTests --config Release` after loading `VsDevCmd.bat` in the same shell.
- `OpenTuneTests.exe` passed all existing suites plus the new Phase 15 section:
  - `SAMP_01_ImportedClipSampleRangeUsesStored44k1Length`
  - `TASK_01_FreezeRenderBoundariesProducesPublishAndSynthCounts`
  - `TASK_03_PreparePublishedAudioRejectsSynthLengthMismatch`
