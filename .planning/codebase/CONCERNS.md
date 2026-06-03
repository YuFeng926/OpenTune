# Codebase Concerns

**Analysis Date:** 2026-06-03

## Tech Debt

### God Class: OpenTuneAudioProcessor

- Issue: `OpenTuneAudioProcessor` in `Source/PluginProcessor.cpp` (6084 lines) and `Source/PluginProcessor.h` (885 lines) has accumulated far too many responsibilities. The class directly manages: ARA session lifecycle, transport control, note generator orchestration, vocoder model lifecycle, rendering pipeline (chunk render worker thread), import/export, placement CRUD operations, clipboard, preference bridging, undo history, and playback reading. The header has ~300+ method/field declarations.
- Files: `Source/PluginProcessor.cpp`, `Source/PluginProcessor.h`
- Impact: Any modification to the processor risks side effects across unrelated features. The class is untestable in isolation — most tests require constructing the entire processor and its dependency tree. New developers face a steep learning curve understanding how all the subsystems interact.
- Fix approach: Gradually extract cohesive subsystems into dedicated classes. Candidates: a dedicated `PlaybackEngine` (lines 1100-1600 for processBlock + readPlaybackAudio), a standalone `ImportManager`, a `PlacementOperations` facade, and moving the chunk render worker into its own `RenderWorker` class. Each extraction should have its own unit test suite before the refactor lands.

### Tight Coupling Between Stores and Processor

- Issue: `SourceStore`, `MaterializationStore`, and `StandaloneArrangement` are conceptually independent but their lifecycle and many operations are exclusively managed through `OpenTuneAudioProcessor`. The processor serves as a "god orchestrator" connecting everything through manual delegation rather than dependency injection or message-passing.
- Files: `Source/PluginProcessor.cpp` (lines 506-508 for store ownership), `Source/MaterializationStore.cpp`, `Source/SourceStore.cpp`, `Source/StandaloneArrangement.cpp`
- Impact: Adding a new store operation requires threading it through the processor's already-overloaded public interface. Testing store interactions is only possible by going through the processor.
- Fix approach: Introduce a `Session` or `Project` class that holds the stores and arrangement as an aggregate. Let the processor delegate to this session object. Consider an event/command pattern for operations that span multiple stores.

### GUI-Processor Direct Coupling in Standalone Editor

- Issue: `Source/Standalone/PluginEditor.cpp` (2857 lines) and `Source/Standalone/UI/PianoRollComponent.cpp` (3151 lines) directly call into the processor for data manipulation. There is no intermediate model or view-model layer — UI components own interaction state, rendering state, and business logic together.
- Files: `Source/Standalone/PluginEditor.cpp`, `Source/Standalone/UI/PianoRollComponent.cpp`, `Source/Standalone/UI/ArrangementViewComponent.cpp` (2583 lines)
- Impact: UI refactors require understanding the full processor API. Editor code cannot be tested without a full processor instance. Visual layout code is interwoven with data manipulation code.
- Fix approach: Introduce a thin `EditorModel` or `EditorController` that abstracts the processor's editing API. Move interaction state (note drafting, tool handling, selection state) out of UI component classes into this model.

### Hardcoded Constants

- Issue: Magic numbers appear throughout the codebase without clear justification or centralized definition. Constants like `44100.0` (sample rate), `512` (hop size), `5` (render lookahead seconds), `30` (render timeout seconds) are scattered across files.
- Files: `Source/PluginProcessor.h` (line 96-100 for AudioConstants), scattered throughout `Source/PluginProcessor.cpp`, `Source/Inference/*.cpp`, `Source/DSP/*.cpp`
- Impact: Changing a fundamental constant (e.g., internal sample rate) requires finding and updating dozens of locations. Risk of subtle bugs where different parts of the system use slightly different constants.
- Fix approach: Centralize all audio pipeline constants (sample rates, hop sizes, frame sizes, buffer dimensions) into `AudioConstants` or a dedicated `PipelineConfig` header. Use `constexpr` throughout and enforce via compilation.

### Singleton Anti-Pattern Usage

- Issue: `LocalizationManager::getInstance()` in `Source/Utils/LocalizationManager.h` and `getLoggerLock()` (static `juce::CriticalSection`) in `Source/Utils/AppLogger.cpp` (line 5-7) use singleton patterns that make testing harder and create hidden dependencies.
- Files: `Source/Utils/LocalizationManager.h`, `Source/Utils/AppLogger.cpp`
- Impact: Tests cannot isolate localization state. The static logger lock prevents parallel test execution. Singletons make it impossible to run multiple independent processor instances with different configurations.
- Fix approach: Replace `LocalizationManager` singleton with dependency injection (pass a `LanguageState` reference). Replace the static logger lock with an instance-based logger that accepts a sink.

### Obsolete Feature Flags / Platform Checks

- Issue: Comment blocks reference legacy backend modes ("basic", "aggressive") that have been migrated to "game". The `OPENTUNE_NOTE_BACKEND` environment variable and legacy paths remain.
- Files: `Source/PluginProcessor.cpp` (lines 49-86 for `AutoRefGameBackendProbe`), `Source/Utils/LegacyNoteGenerator.h`
- Impact: Dead code paths increase cognitive load, accidental fallback to untested paths if environment variable is set, and confusion about what the "current" backend is.
- Fix approach: Once GAME backend is fully stable, remove `LegacyNoteGenerator` entirely and remove the `OPENTUNE_NOTE_BACKEND` environment variable check. Clean up all `forceLegacy` and `gameBundlePresent` probing code.

## Known Bugs

### Garbled Chinese Characters in Source Comments

- Symptoms: Some Chinese text displays as garbled/mojibake characters (U+FFFD replacement characters or mixed encodings) in comment blocks.
- Files: `Source/PluginProcessor.cpp` (lines 74, 78, 82, 85 for garbled UTF-8 strings), possibly other files with embedded Chinese
- Trigger: Viewing or processing the file with tools that don't correctly interpret the encoding
- Workaround: The literal strings are in UTF-8 but may have been corrupted during editing. A rebuild with a UTF-8-aware editor should fix display issues. The functional code uses `juce::String::fromUTF8(u8"...")` which handles UTF-8 correctly at runtime.

### Raw `delete` on JUCE-owned reader

- Symptoms: `AudioFormatRegistry.cpp` line 120 calls `delete reader;` on a reader created by `format->createReaderFor()`. While this is currently safe (JUCE format readers are heap-allocated), it breaks RAII patterns.
- Files: `Source/Audio/AudioFormatRegistry.cpp` (line 120)
- Trigger: This is a diagnostic probe function that creates a reader solely to check if the format can decode the file, then immediately deletes it. The `delete` is not guarded by a unique_ptr.
- Workaround: Replace with `std::unique_ptr<juce::AudioFormatReader>(reader)` or use a scoped wrapper.

### ARA Host Selection Semantics Vary by DAW

- Symptoms: Studio One, Logic Pro, Cubase/Nuendo, and other ARA2 hosts may send different combinations of explicit playback regions, region sequences, and time ranges through `ARAViewSelection`.
- Files: `Source/ARA/OpenTuneEditorView.cpp`, `Source/ARA/OpenTuneDocumentController.cpp`, `Source/Plugin/PluginEditor.cpp`
- Trigger: OpenTune follows the official EditorView path: the host selection arrives through `doNotifySelection()`, effective playback regions are copied immediately, and the first effective region is treated as the focused editor target. No local `preferredPlaybackRegion_` fallback or global selection state exists inside `Source/ARA/`. This matches the SDK contract but still needs host-specific verification for UI focus behavior.
- Workaround: Keep the implementation on the official `ViewSelection::getEffectivePlaybackRegions()` path and verify in supported DAWs instead of adding any local selection state.

## Security Considerations

### Untrusted ONNX Model Loading

- Risk: ONNX model files (`rmvpe.onnx`, `hifigan.onnx`, GAME models) are loaded from user-configurable filesystem paths without integrity verification. A maliciously crafted ONNX model could exploit vulnerabilities in ONNX Runtime's model parser or the DirectML execution provider.
- Files: `Source/Inference/ModelFactory.cpp` (loads ONNX sessions), `Source/Inference/GameNoteGenerator.cpp` (lines 34-37 for GAME model paths), `Source/Utils/ModelPathResolver.h`
- Current mitigation: Models are loaded from a bundled `models/` directory alongside the executable. The ONNX Runtime version (1.24.4) receives security updates.
- Recommendations: Add SHA-256 integrity verification for shipped model files. Consider signing model files and verifying at load time. Monitor ONNX Runtime CVEs and update to the latest patch version.

### File Path Deserialization from Untrusted Sources

- Risk: `ProjectPersistence.cpp` and `ProjectSession.cpp` deserialize file paths from project state data. When a user opens a `.opentune` project file, audio file paths and model paths are read and used directly for file I/O. A malicious project file could reference sensitive files or trigger path traversal.
- Files: `Source/Utils/ProjectPersistence.cpp`, `Source/Utils/ProjectSession.cpp`
- Current mitigation: None explicit. The serialized state uses a binary format (magic number `0x4F545354`) which reduces casual tampering but does not prevent targeted attacks.
- Recommendations: Validate all deserialized file paths against an allowlist (e.g., only within the project directory). Implement path sanitization against `..` traversal. Consider moving to a structured serialization format (JSON/protobuf) with schema validation.

### Unvalidated Audio File Parsing

- Risk: Audio files (WAV, FLAC, MP3, OGG) imported by users are parsed through JUCE's `AudioFormatReader` which delegates to format-specific decoders. A malformed audio file could trigger buffer overflows or out-of-bounds reads in the underlying decoder libraries.
- Files: `Source/Audio/AudioFormatRegistry.cpp`, `Source/Utils/ProjectSession.cpp` (line 229 for `loadAudioFile`)
- Current mitigation: JUCE's bundled audio format readers are well-tested. The application does not accept audio from network sources directly.
- Recommendations: Add audio file size limits before decoding. Consider sandboxing audio file parsing in a separate process for the standalone application.

### Unsigned Application Binaries

- Risk: macOS builds use ad-hoc signing (no Apple Notarization). Windows builds use no Authenticode signing. Users must bypass Gatekeeper/SmartScreen protections to install the software.
- Files: `README.md` (lines 99-104 document the ad-hoc signing workaround), `Installer/`
- Current mitigation: Install scripts strip quarantine flags on macOS. The application is open-source so users can audit the code.
- Recommendations: Pursue Apple Developer ID signing and notarization for macOS releases. Consider Windows Authenticode signing for official releases.

## Performance Bottlenecks

### Single-Threaded ONNX Inference Pool

- Problem: The note generator thread pool (`noteGeneratorPool_` in `Source/PluginProcessor.h` line 542) is configured with only 1 thread. GAME ONNX model inference (encoder, segmenter, estimator, bd2dur) is serialized for the entire application.
- Files: `Source/PluginProcessor.h` (line 542), `Source/PluginProcessor.cpp` (lines 1700-1900 for note generation), `Source/Inference/GameNoteGenerator.cpp`
- Cause: Comment says "single-threaded ORT-safe" — likely a workaround for ONNX Runtime thread-safety issues with certain execution providers.
- Improvement path: Investigate if ONNX Runtime 1.24.4 supports concurrent session execution with separate `Ort::MemoryInfo` instances. If feasible, increase pool size to `std::thread::hardware_concurrency()` capped at a reasonable limit. Profile GAME model inference to identify the slowest model in the pipeline.

### Lock Contention in AppPreferences

- Problem: `Source/Utils/AppPreferences.cpp` has 23 separate `get`/`set` methods, each acquiring a `std::lock_guard<std::mutex>` on `mutex_`. With the audio thread potentially calling preferences (e.g., theme/language state), this creates unnecessary contention.
- Files: `Source/Utils/AppPreferences.cpp` (lines 410-620 for all locked accessors)
- Cause: Every individual property access locks the entire preferences mutex. While individual locks are short, frequent access on the audio thread adds jitter.
- Improvement path: Use `juce::ReadWriteLock` or `std::shared_mutex` to allow concurrent reads. Consider separating read-heavy preferences (visual, language) from write-heavy preferences (shortcuts, settings) into different lock domains.

### Unbounded Render Queue Growth

- Problem: The chunk render queue in `MaterializationStore` (`renderQueueMutex_`) and the Stage 2 rebuild queue in `PluginProcessor` (`stage2RebuildQueue_`) have no hard size limits. Rapid user edits (e.g., scrubbing time grid handles) could enqueue hundreds of render jobs faster than the worker thread can process them.
- Files: `Source/MaterializationStore.cpp` (lines 786-808 for render queue), `Source/PluginProcessor.cpp` (lines 1450-1500 for Stage 2 queue)
- Cause: No backpressure mechanism. The queue accepts any number of jobs.
- Improvement path: Add a maximum queue depth (e.g., 64 pending jobs). When the limit is hit, coalesce adjacent chunk requests or drop oldest pending jobs in favor of the most recent. Use `stage2QueueDepth_` atomics to monitor congestion in real-time.

### Redundant Audio Buffer Copying

- Problem: `sliceAudioBuffer` in `Source/PluginProcessor.cpp` (lines 176-200) always creates a new `juce::AudioBuffer` copy even when the requested slice equals the full buffer. Every split/clone/export operation copies entire audio buffers unnecessarily.
- Files: `Source/PluginProcessor.cpp` (lines 176-200, called at lines 3226-3242 for split, 3670+ for export)
- Cause: The slice function always allocates and copies. No short-circuit for identity slices.
- Improvement path: Add a fast-path check: if `startSample == 0 && endSampleExclusive == audioBuffer->getNumSamples()`, return the original `shared_ptr` without copying. Consider using `juce::AudioBuffer<float>` with an offset/view pattern where possible.

## Fragile Areas

### ARA Role Sequencing in Real Hosts

- Files: `Source/ARA/OpenTuneDocumentController.cpp`, `Source/ARA/OpenTuneEditorView.cpp`, `Source/ARA/OpenTunePlaybackRenderer.cpp`
- Why fragile: The local contract tests can prove the implementation uses official roles and remains lock-free, but they cannot emulate every host timing for document graph edits, editor opening/closing, selection notification, playback-region assignment, real-time process callbacks, and ARA archive persistency.
- Safe modification: Keep ARA responsibilities separated by role: DocumentController owns projections and persistence (full-document + sub-graph via `ARAStoreObjectsFilter`/`ARARestoreObjectsFilter`), EditorView owns UI selection projection, and PlaybackRenderer renders only the host-assigned playback-region set. No local preferred-region state, no regular VST3 capture/session state inside `Source/ARA/`.
- Test coverage: `OpenTuneTests.exe architecture` guards the static contract; ARA VST3 compilation validates JUCE/SDK hook signatures. Real DAW smoke tests are still required for host behavior.

### Cross-Thread Materialization State Transitions

- Files: `Source/PluginProcessor.cpp` (lines 5370-6000 for chunk render worker), `Source/MaterializationStore.cpp` (getSnapshot, commit operations)
- Why fragile: The chunk render worker thread reads materialization state (audio buffer, pitch curve, notes) while the main thread may simultaneously commit edits (notes, time grid, pitch curve). The system uses snapshot-based concurrency (the worker takes a `MaterializationSnapshot` at job start), but TOCTOU checks in `commitAutoTuneGeneratedNotesByMaterializationId` (line 5320) suggest this is known to be race-prone.
- Safe modification: Always snapshot materialization state at the worker level, never read store state incrementally. Add the "revision check before commit" pattern used in lines 5320-5323 to all state-mutating operations outside the main thread.
- Test coverage: `InvariantContractTests.cpp` covers bypass bit-exactness and ARA region length invariants. `Stage2WorkerTests.cpp` covers Stage 2 worker paths. Gaps exist for the chunk render worker's edge cases (zero-length chunks, boundary alignment).

### Time Grid Editing Pipeline

- Files: `Source/Utils/TimeGrid.h`, `Source/DSP/TimeGridPatchBuilder.cpp`, `Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp` (2203 lines)
- Why fragile: Vocal time-stretch (Phase D) involves a multi-layer pipeline: UI tool handler → time grid snapshot → patch builder → Stage 2 worker → TimeStretchCache → PlaybackReadSource. Each layer has its own thread-safety and identity/bypass semantics. Any mismatch in time grid revision tracking or identity flag propagation causes audible artifacts.
- Safe modification: When modifying any link in this pipeline, verify: (1) `timeGridIsIdentity` is correctly set to `true` after import and to `false` after any time handle edit, (2) `timeGridRevision` is incremented atomically on every commit, (3) the Stage 2 rebuild is triggered when notes or pitch curve change with a non-identity grid (line 5331-5336).
- Test coverage: `TimeGridTests.cpp`, `TimeStretchCacheTests.cpp`, `TimeToolHandlerTests.cpp`, `Stage2WorkerTests.cpp`, `IntegrationPipelineTests.cpp`, `InvariantContractTests.cpp`. Good coverage overall but edge cases at extreme time stretches (0.5x-2x) are likely untested.

### ONNX Runtime DLL Delay-Loading (Windows)

- Files: `Source/Utils/OnnxRuntimeDelayLoadHook.cpp`, `Source/Utils/WindowsDllSearchPath.cpp`
- Why fragile: On Windows, `onnxruntime.dll` is delay-loaded through a custom hook that searches multiple candidate directories (module dir, `Program Files/OpenTune`, `ProgramData/OpenTune`). If none of these contain the DLL, the application fails to initialize silently when the first ONNX symbol is called.
- Safe modification: Any change to the deployment directory structure must update the candidate path list in `OnnxRuntimeDelayLoadHook.cpp` (lines 98-107). The hook is installed at static initialization time, so late DLL discovery is not supported.
- Test coverage: No automated test for DLL resolution fallback paths. Relies on the build system placing the DLL in the output directory.

## Scaling Limits

### Audio Duration Limitations

- Current capacity: The system stores all audio in memory as `juce::AudioBuffer<float>` objects. A 10-minute mono 44.1kHz clip requires ~26.5 MB. The `SourceStore` keeps source buffers for all imported clips, and `MaterializationStore` keeps rendered/corrected versions. With multi-track projects, memory consumption grows linearly.
- Limit: The README notes "ultra-long audio (>10 minutes) processing time is long." Beyond ~30 minutes of multi-track audio, the application may exceed practical memory limits on consumer hardware or trigger audio dropouts due to buffer copies.
- Scaling path: Implement memory-mapped or chunked audio storage. Use `juce::AudioFormatReader` for streaming playback of uncorrected source audio instead of keeping full buffers in memory. Consider a disk-backed cache for rendered chunks.

### Render Thread Throughput

- Current capacity: Single chunk render worker thread (line 595) processing one chunk at a time. Chunk size is determined by the user's edit range.
- Limit: Heavily edited projects (many small edit ranges) produce many small render jobs, overwhelming a single worker. Real-time interactive scrubbing of time-grid handles can produce render requests faster than the worker processes them.
- Scaling path: Parallelize the chunk render worker by processing non-overlapping chunks concurrently (requires isolated ONNX sessions or proof that ONNX Runtime 1.24.4 supports concurrent inference). Implement a render priority system where the currently visible/audible time range is rendered first.

### Maximum Track and Placement Count

- Current capacity: `MAX_TRACKS = 32` in `Source/Utils/TrackConstants.h`. No explicit limit on placements per track, but each placement adds a `StandaloneArrangement::Placement` struct and triggers a `MaterializationStore` entry.
- Limit: With 32 tracks × ~100 placements each, combined with large audio buffers, the application likely faces memory pressure before hitting algorithmic limits.
- Scaling path: The current limits are reasonable for most use cases. If increased track counts are needed, switch from linear scans in `findPlacementByIdGlobal` and `getPlacementByIndex` to hash maps.

## Dependencies at Risk

### ONNX Runtime 1.24.4

- Risk: Version-pinned to 1.24.4. ONNX Runtime releases frequently (monthly). The release is from early 2025 and may have known vulnerabilities or performance regressions compared to newer versions.
- Impact: Security vulnerabilities in ONNX Runtime's model parsing or execution providers could affect the application. New model formats (e.g., ONNX opset updates) may not be supported.
- Migration plan: Monitor ONNX Runtime releases for security fixes. Test upgrades on a staging branch with CI to verify model compatibility (rmvpe.onnx, hifigan.onnx, GAME models all pass inference validation). The DirectML NuGet package and CPU package must be upgraded in lockstep.

### JUCE Framework (vendored git clone)

- Risk: JUCE is cloned from `juce-framework/JUCE.git` into `JUCE-master/`. No version pinning — whatever the latest commit on `master` is at clone time.
- Impact: JUCE API changes between commits could break the build. Reproducible builds are impossible without a known-good commit hash.
- Migration plan: Pin JUCE to a specific commit hash or release tag in the README. Consider using JUCE as a git submodule at a pinned revision.

### Microsoft.AI.DirectML 1.15.4

- Risk: DirectML 1.15.4 is a NuGet package. Future Windows updates may ship with a newer DirectML runtime that is not ABI-compatible with the headers.
- Impact: The vendored `DirectML.dll` in the application directory would take precedence over the system version. If Microsoft introduces breaking changes between minor DirectML versions, the application's vendored DLL would need updating.
- Migration plan: Test with each new DirectML release. Use `OPENTUNE_DIRECTML_DLL` CMake variable to test with system-provided DirectML.

### D3D12 Agility SDK 1.619.1

- Risk: The DirectX Agility SDK provides a D3D12 runtime that the application bundles alongside its executable. Microsoft regularly updates the Agility SDK to support new GPU features and fix driver compatibility issues.
- Impact: Old Agility SDK versions may not support new GPUs or may have known driver interaction bugs on specific hardware.
- Migration plan: Periodically update to the latest Agility SDK NuGet package. Test on a range of GPU hardware (NVIDIA, AMD, Intel) to verify no regressions.

## Missing Critical Features

### No Headless/CLI Mode

- Problem: The application requires a GUI (JUCE MessageManager) to function. There is no command-line interface for batch processing audio files.
- Blocks: Automated testing of the full audio pipeline. Batch processing of multiple files. CI pipeline integration for render validation.
- Files: The entire codebase assumes a GUI event loop via `juce::MessageManager::getInstance()`.

### No Project Export to Standard Formats

- Problem: The only export path is WAV through `exportSelectedTrack` / `exportFullMix`. No support for FLAC, MP3, OGG exports despite the import pipeline supporting all these formats. No export of individual corrected clips with their pitch correction applied.
- Blocks: Users who want lossless compressed output. Users who want to export per-clip stems.
- Files: `Source/PluginProcessor.cpp` (export functions around line 3700-3900)

### No Audio Effect Bypass for Monitoring

- Problem: There is no "dry/wet" mix control or bypass toggle that lets the user A/B compare the original vs. corrected audio during playback.
- Blocks: Users who want to hear the difference their corrections make. Critical for mixing decisions.
- Files: `Source/PluginProcessor.h` (no bypass parameter), `Source/Standalone/UI/ParameterPanel.cpp`

## Test Coverage Gaps

### No DAW-Driven ARA Protocol Integration Tests

- What is not tested: Real host bind/unbind timing, `EditorView` selection focus, assigned playback-region set updates, overlapping region playback in host context, transport/playhead behavior, and ARA archive round-trip (save/reopen/restore bindings) through DAW projects.
- Files: `Source/ARA/OpenTuneDocumentController.cpp`, `Source/ARA/OpenTuneEditorView.cpp`, `Source/ARA/OpenTunePlaybackRenderer.cpp`, `Source/Plugin/PluginEditor.cpp`
- Risk: ARA protocol behavior that depends on a specific DAW host may pass local static contracts but still need host smoke validation.
- Priority: Medium — mitigated by ARA/non-ARA/Standalone build isolation and architecture contracts, `ARAStoreObjectsFilter`/`ARARestoreObjectsFilter` support, and the removal of any local fallback state. Real DAW regression coverage is still missing.

### No GPU Inference Path Tests

- What's not tested: DirectML execution provider initialization, fallback to CPU when DML is unavailable, model inference correctness comparison between CPU and DML paths, memory management for GPU tensors.
- Files: `Source/Inference/DmlVocoder.cpp`, `Source/Utils/AccelerationDetector.cpp`, `Source/Inference/ModelFactory.cpp`
- Risk: GPU-specific inference errors produce NaN or silent values in rendered audio. D3D12 device loss recovery is untested.
- Priority: Medium — affected users would hear corrupted audio without clear error messages.

### No macOS-Specific Build/Test Coverage in CI

- What's not tested: macOS builds (Xcode generator, CoreML execution provider), macOS code signing workflow, macOS-specific file paths and bundle layouts.
- Files: `CMakeLists.txt` (lines 230-243 for macOS), `Source/Inference/ModelFactory.cpp` (CoreML EP path), `Source/Inference/DmlVocoder.cpp` (Apple-specific)
- Risk: macOS-specific regressions introduced in cross-platform refactors may go unnoticed until release.
- Priority: Medium if macOS users are a significant portion of the user base. Low if macOS is secondary.

### No Performance Regression Tests

- What's not tested: Inference latency benchmarks, render cache hit rates, memory usage under load, audio thread execution time (must stay under realtime deadline).
- Files: Entire render pipeline
- Risk: Performance optimizations that inadvertently degrade the audio thread's realtime safety. Memory leaks that accumulate over long sessions.
- Priority: High — audio dropouts are the most user-visible failure mode.

---

*Concerns audit: 2026-06-03*
