# Codebase Concerns

**Analysis Date:** 2026-05-02

## Tech Debt

**[Confirmed] Release metadata still drifts from official shipped milestone naming:**
- Issue: build metadata still declares `1.0.0` even though official planning says the latest shipped milestone is `v2.2`.
- Files: `CMakeLists.txt:11`, `CMakeLists.txt:316`, `.planning/PROJECT.md:19`, `.planning/ROADMAP.md:33`
- Impact: host-visible plugin version strings, generated artifacts, and release bookkeeping can describe different versions of the same build.

**[Confirmed] Test helper code is duplicated, but one copy is not compiled:**
- Issue: `Tests/TestSupport.cpp` redefines `gHasTestFailure`, `logPass`, `logFail`, `logSection`, `approxEqual`, `serializeProcessorStateToValueTree`, `makeTestClipRequest`, and `seedPublishedIdleChunk`, while `OpenTuneTests` only compiles `Tests/TestMain.cpp` plus `Tests/TestEditorFactoryStub.cpp`.
- Files: `Tests/TestSupport.cpp`, `Tests/TestMain.cpp:2624`, `CMakeLists.txt:863`
- Impact: edits can land in `Tests/TestSupport.cpp` and appear meaningful in review while never affecting the actual test binary.

**[Confirmed] Core runtime and editor ownership are still concentrated in a few very large files:**
- Issue: several critical files remain multi-thousand-line change hotspots: `Source/PluginProcessor.cpp` (3187 lines), `Source/Standalone/PluginEditor.cpp` (2614 lines), `Source/Standalone/UI/PianoRollComponent.cpp` (2869 lines), `Source/Standalone/UI/ArrangementViewComponent.cpp` (1778 lines), and `Tests/TestMain.cpp` (2881 lines).
- Files: `Source/PluginProcessor.cpp`, `Source/Standalone/PluginEditor.cpp`, `Source/Standalone/UI/PianoRollComponent.cpp`, `Source/Standalone/UI/ArrangementViewComponent.cpp`, `Tests/TestMain.cpp`
- Impact: unrelated work keeps colliding in the same files, and refactors remain expensive because UI, async flow, playback, test harness, and owner logic are still interleaved.

## Known Bugs

**[Confirmed] The only registered CTest entry is misnamed:**
- Symptoms: CTest registers `OpenTuneCoreTests`, but the command is just `OpenTuneTests` with no suite argument, which runs every suite.
- Files: `CMakeLists.txt:963`, `Tests/TestMain.cpp:2871`
- Trigger: any automation that assumes `ctest -R OpenTuneCoreTests` is only exercising the `core` suite.
- Impact: CI or local verification can report a narrower scope than it actually executed.

## Performance Bottlenecks

**[Confirmed] ARA hydration still duplicates entire source audio into one in-memory buffer:**
- Problem: the hydration worker allocates `juce::AudioBuffer<float>(numChannels, numSamples)` for each hydrated source, then fills it chunk by chunk before publishing it.
- Files: `Source/ARA/VST3AraSession.cpp:745`, `Source/ARA/VST3AraSession.cpp:749`, `Source/ARA/VST3AraSession.h:120`
- Impact: long sources scale memory linearly with full source length instead of active placement window size.

**[Confirmed] Standalone editor teardown still blocks on outstanding async work:**
- Problem: editor destruction waits for all stored `std::future<void>` jobs and joins `exportWorker_` synchronously.
- Files: `Source/Standalone/PluginEditor.cpp:616`, `Source/Standalone/PluginEditor.cpp:623`, `Source/Standalone/PluginEditor.cpp:628`, `Source/Standalone/PluginEditor.cpp:663`, `Source/Standalone/PluginEditor.cpp:671`, `Source/Standalone/PluginEditor.cpp:1793`
- Impact: closing the Standalone editor can stall behind import/export work instead of canceling or handing off background tasks cleanly.

## Fragile Areas

**[Confirmed] ARA session correctness is still highly lifecycle- and threading-sensitive:**
- Why fragile: snapshot publication depends on `editingDepth_`, `sampleAccessEnabled`, `leaseGeneration`, `cancelRead`, `pendingLeaseReset`, `pendingRemoval`, and the hydration worker staying aligned across callbacks and worker-thread reads.
- Files: `Source/ARA/VST3AraSession.h:123`, `Source/ARA/VST3AraSession.h:126`, `Source/ARA/VST3AraSession.h:130`, `Source/ARA/VST3AraSession.cpp:234`, `Source/ARA/VST3AraSession.cpp:389`, `Source/ARA/VST3AraSession.cpp:647`, `Source/ARA/VST3AraSession.cpp:693`
- Impact: seemingly local callback changes can easily reintroduce stale snapshot publication, leaked leases, or broken sample-access transitions.

**[Confirmed] Standalone import/export orchestration still mixes UI lifetime, async coordination, and processor mutation inside one editor class:**
- Why fragile: chooser callbacks, background task dispatch, export threading, undo wiring, and UI refresh logic still meet inside `OpenTuneAudioProcessorEditor`.
- Files: `Source/Standalone/PluginEditor.cpp:644`, `Source/Standalone/PluginEditor.cpp:1214`, `Source/Standalone/PluginEditor.cpp:1709`, `Source/Standalone/PluginEditor.cpp:1792`
- Impact: extending import/export behavior still risks lifecycle regressions because there is no dedicated coordinator boundary.

## Scaling Limits

**[Confirmed] VST3 ARA remains intentionally single-workspace in practice:**
- Current shape: the repo still centers ARA state on one active published snapshot with one preferred region and source/region maps inside a single `VST3AraSession`.
- Files: `Source/ARA/VST3AraSession.h:178`, `Source/ARA/VST3AraSession.h:181`, `Source/ARA/VST3AraSession.h:258`, `Source/ARA/VST3AraSession.h:259`
- Limit: the current session model is not a drop-in path to multi-workspace or Melodyne-style concurrent source editing.

## Dependencies At Risk

**[Confirmed] Windows builds still assume fixed SDK roots and tightly pinned vendored binary versions:**
- Risk: configure-time discovery still defaults to absolute Windows Kits roots and pinned DirectML / D3D12 Agility / ONNX Runtime package versions.
- Files: `CMakeLists.txt:72`, `CMakeLists.txt:73`, `CMakeLists.txt:82`, `CMakeLists.txt:84`, `CMakeLists.txt:207`, `CMakeLists.txt:240`
- Impact: a machine with different SDK layout or missing vendored package folders still fails hard instead of negotiating a clearer setup path.

**[Resolved] DmlRuntimeVerifier caused hang on RTX 5070 Ti and used unreliable DLL-size heuristic:**
- Issue: The separate DmlRuntimeVerifier pre-validation layer (512 lines) created D3D12+DML devices to probe GPU capability before DmlVocoder did the same through ORT, causing duplicated work and a hang on RTX 5070 Ti. AccelerationDetector also used a 12MB DLL size heuristic to guess DML availability.
- Resolution: DmlRuntimeVerifier deleted entirely. AccelerationDetector simplified to use `Ort::GetApi().GetExecutionProviderApi("DML")` for DML availability. DmlVocoder changed from DML2 to DML1 API with explicit adapter index binding.
- Files: `Source/Utils/DmlRuntimeVerifier.h` (deleted), `Source/Utils/DmlRuntimeVerifier.cpp` (deleted), `Source/Utils/AccelerationDetector.cpp` (rewritten), `Source/Inference/DmlVocoder.cpp` (DML1), `Source/Inference/VocoderFactory.cpp` (overrideBackend)
- Date: 2026-05-02

## Test Coverage Gaps

**[Confirmed] The advertised `core` suite still contains no assertions:**
- Evidence: `runCoreBehaviorSuite()` only logs the section header and returns.
- Files: `Tests/TestMain.cpp:2766`
- Impact: `core` appears in suite listings, but it does not execute any real behavior checks.

**[Confirmed] A large share of test coverage is still source-text inspection instead of runtime execution:**
- Evidence: many tests read workspace files and assert `contains(...)` / `!contains(...)` on source text rather than exercising live UI or host behavior.
- Files: `Tests/TestMain.cpp:153`, `Tests/TestMain.cpp:186`, `Tests/TestMain.cpp:1085`, `Tests/TestMain.cpp:1207`, `Tests/TestMain.cpp:1250`, `Tests/TestMain.cpp:1285`
- Impact: naming/structure drift is guarded, but JUCE lifecycle, async ordering, and host/runtime behavior can still regress behind green tests.

**[Confirmed] No host-level plugin validation is wired into CMake test registration:**
- Evidence: CMake exposes one `add_test(...)` entry for `OpenTuneTests`; there is no separate DAW/host automation target in the test registration path.
- Files: `CMakeLists.txt:861`, `CMakeLists.txt:963`
- Impact: ARA host integration and final plugin-loading behavior still depend on manual DAW verification outside the automated test pipeline.

---

*Concerns audit: 2026-04-21 after rechecking the live tree and removing 2026-04-19 concerns that are no longer supported by current source state*
