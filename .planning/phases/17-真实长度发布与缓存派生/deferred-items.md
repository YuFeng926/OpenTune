# Phase 17 Deferred Items

## Out-of-scope discoveries

- None.

## Resolved During 17-02

1. **`OpenTuneTests.exe` link gate lacked MSVC/UCRT/UM runtime search paths**
   - **Observed during:** Phase 17 Plan 01 verification and 17-02 Task 3
   - **Root cause:** CMake generated the Windows/MSVC compiler metadata with empty `CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES`; the active `VsDevCmd.bat` call path did not leave a usable `LIB` environment for the later Ninja link step.
   - **Resolution:** `CMakeLists.txt` now derives the MSVC and Windows SDK runtime library directories from `CMAKE_CXX_COMPILER` and `CMAKE_MT`, then injects them into the final link targets (`OpenTune_Standalone`, `OpenTune_VST3`, `OpenTune_vst3_helper`, `OpenTuneTests`).
   - **Outcome:** `OpenTuneTests.exe` now links and runs successfully.
