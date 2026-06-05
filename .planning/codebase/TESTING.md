# Testing Patterns

**Analysis Date:** 2026-06-03

## Test Framework

**Runner:**
- Custom test runner (no external framework). Tests are compiled as a standalone executable `OpenTuneTests` defined in `CMakeLists.txt:1214`
- Test discovery is manual — each suite function is registered in `Tests/TestMain.cpp` in the `kSuites` array

**Build System:**
- CMake option: `OPENTUNE_BUILD_TESTS=ON` (default ON, `CMakeLists.txt:1211`)
- Test executable links against `OpenTune` shared code, `juce::juce_core`, `juce::juce_audio_basics`
- Built with `OPENTUNE_TEST_BUILD=1` preprocessor define — gates test-only API in production code (e.g., `setReferenceAnalysisNotificationDispatcherForTests` in `PluginProcessor.h:764`)
- `JucePlugin_Build_Standalone=1` / `JucePlugin_Build_VST3=0` forced for test build to match `Standalone` ODR layout

**Assertion Library:**
- **Custom minimal assertions** via `logPass(testName)` / `logFail(testName, detail)` declared in `Tests/TestSupport.h`
- `approxEqual(float, float, tol)` / `approxEqual(double, double, tol)` for floating-point comparisons
- No external assertion library

**Run Commands:**
```bash
# Build architecture/static-contract tests with the Windows Path de-dup wrapper
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTuneTests"

# Run only the architecture contract suite
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& build-ara-overlay-vs18-clean\Release\OpenTuneTests.exe architecture"

# Build the ARA VST3 target
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-ara-overlay-vs18-clean --config Release --target OpenTune_VST3"

# Configure and build the non-ARA VST3 target
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --preset windows-nonara-vs2022"
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_VST3"

# Build the non-ARA Standalone target
cmd /v:on /c "set CLEAN_PATH=%Path%& set PATH=& set Path=!CLEAN_PATH!& cmake --build build-nonara-overlay-vs18-clean --config Release --target OpenTune_Standalone"
```

**Preset:**
```json
"testPresets": [
  {
    "name": "windows-ara-release",
    "configurePreset": "windows-ara-vs2022",
    "configuration": "Release",
    "output": { "outputOnFailure": true }
  }
]
```

## Test File Organization

**Location:**
- All tests in `Tests/` directory at project root — separate from source code (not co-located)

**Naming:**
- Two patterns coexist:
  1. `Tests/{Name}Tests.cpp` — e.g., `TimeGridTests.cpp`, `SoundTouchStretcherTests.cpp`, `IntegrationPipelineTests.cpp`
  2. `Tests/Test{Name}.cpp` — e.g., `TestUndoManagerContract.cpp`, `TestCompositeUndoAction.cpp`, `TestAutoRefArchitecture.cpp`
- No distinction in meaning — both are used for logical unit or contract tests

**Structure:**
```
Tests/
├── TestMain.cpp                  # Entry point, suite registry, shared helpers
├── TestSupport.h                 # Shared test utilities, harnesses, probes
├── TestEditorFactoryStub.cpp     # Stub to avoid linking real UI
├── TimeGridTests.cpp             # Data model tests
├── SoundTouchStretcherTests.cpp  # Wrapper tests
├── TimeStretchCacheTests.cpp     # Cache tests
├── MaterializationStoreTimeGridTests.cpp  # Integration tests
├── Stage2WorkerTests.cpp         # Worker thread tests
├── TimeToolHandlerTests.cpp      # UI tool handler tests
├── IntegrationPipelineTests.cpp  # L3 integration tests
├── InvariantContractTests.cpp    # L4 contract/invariant tests
├── GameNoteGeneratorTests.cpp    # ONNX inference tests
├── VocoderConfigTests.cpp        # Vocoder config tests
├── TestTimeGridPatchBuilder.cpp  # AUTO Ref compiler tests
├── TestReferenceAutoAlign.cpp    # Reference alignment tests
├── TestAutoRefFailure.cpp        # Failure mode tests
├── TestAutoRefArchitecture.cpp   # Architecture guards (source-scan tests)
├── TestAutoRefIntegration.cpp    # Integration tests
├── TestReferenceFeaturesCacheSmoke.cpp  # Cache smoke tests
├── TestReferenceFeaturesCacheLifecycle.cpp  # Cache lifecycle tests
├── TestReferenceBinding.cpp      # Reference binding tests
├── TestPlacementReferenceCascade.cpp  # Cascade tests
├── TestMaterializationContract.cpp    # Contract tests
├── TestReferenceAnalysisService.cpp   # Service lifecycle tests
├── TestArrangementContract.cpp        # Arrangement tests
├── TestProjectSessionReference.cpp    # Session roundtrip tests
├── TestTimelineRenderingPipeline.cpp  # Rendering tests
├── TestCompositeUndoAction.cpp        # Undo composite tests
├── TestUndoManagerContract.cpp        # Undo manager tests
├── AutoTunePitchShifterTests.cpp      # Pitch shifter tests
```

## Test Structure

**Suite Organization:**
Each test file defines a `runXxxSuite()` aggregator function and one or more `run...Test()` test functions. Suite aggregators are linked into `kSuites` in `TestMain.cpp`.

**Example from `Tests/TestCompositeUndoAction.cpp`:**
```cpp
// Anonymous namespace for test-local helpers
namespace {

struct MockAction : public UndoAction {
    mutable int undoCount = 0;
    mutable int redoCount = 0;
    juce::String desc;

    explicit MockAction(juce::String d = "Mock") : desc(std::move(d)) {}

    void undo() override { ++undoCount; }
    void redo() override { ++redoCount; }
    juce::String getDescription() const override { return desc; }
};

} // namespace

// Each test is a standalone free function
void runCompositeUndoActionUndoReversesOrderTest()
{
    constexpr const char* testName = "CompositeUndoAction_UndoReversesOrder";

    // Arrange
    CompositeUndoAction composite("TestComposite");
    auto a1 = std::make_unique<MockAction>("A");
    auto a2 = std::make_unique<MockAction>("B");
    MockAction* p1 = a1.get();
    MockAction* p2 = a2.get();
    composite.addAction(std::move(a1));
    composite.addAction(std::move(a2));

    // Act
    composite.redo();
    // Assert
    if (p1->redoCount != 1) { logFail(testName, "first sub-action redo not called"); return; }
    if (p2->redoCount != 1) { logFail(testName, "second sub-action redo not called"); return; }

    composite.undo();
    if (p1->undoCount != 1) { logFail(testName, "first sub-action undo not called"); return; }
    if (p2->undoCount != 1) { logFail(testName, "second sub-action undo not called"); return; }

    logPass(testName);
}

// Suite aggregator — registered in TestMain.cpp
void runCompositeUndoActionSuite()
{
    logSection("CompositeUndoAction");
    runCompositeUndoActionUndoReversesOrderTest();
    runCompositeUndoActionEmptyNoCrashTest();
    runCompositeUndoActionCountsSingleStepTest();
}
```

**Patterns:**
- **Setup:** Inline in each test function or via helper factories (`makePreparedImport()`, `makeTestClipRequest()`)
- **Teardown:** RAII — no explicit teardown needed (JUCE components, unique_ptr auto-destroy)
- **Assertions:** `logFail(testName, message); return;` pattern — early return on first failure
- **Skip pattern:** Self-skip when optional dependencies missing (e.g., `GameNoteGeneratorTests` skips if GAME ONNX models absent)

## Mocking

**Framework:** No external mocking framework. Manual mocks/subclasses used in tests.

**Patterns:**
```cpp
// Manual mock via subclass — override virtual methods
struct MockAction : public UndoAction {
    mutable int undoCount = 0;
    mutable int redoCount = 0;

    void undo() override { ++undoCount; }
    void redo() override { ++redoCount; }
    juce::String getDescription() const override { return desc; }
};
```

```cpp
// Lambda-based mock — for tool handler / interaction testing
PianoRollToolHandler::Context buildContext() {
    PianoRollToolHandler::Context ctx;
    ctx.getState = [this]() -> InteractionState& { return state; };
    ctx.commitNoteDraft = [this]() {
        ++commitNoteDraftCalls;
        if (!commitNoteDraftResult) return false;
        committedNotes = state.noteDraft.workingNotes;
        state.noteDraft.clear();
        return true;
    };
    // ... more lambdas
    return ctx;
}
```

```cpp
// Probe pattern — static friend-like helper to access private members
struct PianoRollComponentTestProbe {
    static bool hasPendingVisualInvalidation(const PianoRollComponent& pianoRoll) {
        return pianoRoll.pendingVisualInvalidation_.hasWork();
    }
    static juce::Rectangle<int> getTimelineViewportBounds(const PianoRollComponent& pianoRoll) {
        return pianoRoll.getTimelineViewportBounds();
    }
};
```

**What to Mock:**
- External services and inference (F0 extraction, vocoder, note generator)
- UI callbacks and interaction feedback (tool handlers, visual invalidation)
- File I/O (project serialization/deserialization)

**What NOT to Mock:**
- Core data structures (`TimeGrid`, `PitchCurve`, `MaterializationStore`, `StandaloneArrangement`) — tested directly
- Pure computation functions (DSP math, `ReferenceAutoAlign` request/patch contract)
- JUCE framework primitives

## Fixtures and Factories

**Test Data:**
```cpp
// Audio fixtures
constexpr double kSampleRate = 44100.0;
constexpr double kPi = 3.14159265358979323846264338327950288;

std::vector<float> makeSineTone(double freqHz, double durationSec, double amp = 0.4) {
    const int n = static_cast<int>(std::round(durationSec * kSampleRate));
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * freqHz * i / kSampleRate));
    }
    return out;
}

// Import fixture
OpenTuneAudioProcessor::PreparedImport makePreparedImport(const juce::String& displayName, int numSamples = 128) {
    OpenTuneAudioProcessor::PreparedImport preparedImport;
    preparedImport.displayName = displayName;
    preparedImport.storedAudioBuffer.setSize(1, numSamples);
    preparedImport.storedAudioBuffer.clear();
    if (numSamples > 0)
        preparedImport.storedAudioBuffer.setSample(0, 0, 0.25f);
    return preparedImport;
}
```

**Location:**
- Shared fixtures in `Tests/TestMain.cpp` (bottom, outside `main()`)
- Test-specific fixtures in anonymous namespace of each test file

## Coverage

**Requirements:** No coverage enforcement detected. No coverage tooling configured.

**View Coverage:** Not configured.

## ARA2 Contract Verification

`Tests/TestMain.cpp` contains the architecture contract suite for the current ARA2 implementation. It is a source-scan contract rather than a DAW-host integration test, and it blocks regressions in the parts that can be proven locally:

- Official object model: `AudioSource`, `AudioModification`, and `PlaybackRegion` use ARA names and ownership boundaries.
- Lock-free ARA layer: `Source/ARA/` is scanned for mutex/thread primitives, JUCE locks, atomics, and `AppLogger`.
- DocumentController projection: `OpenTuneDocumentController` owns `PlaybackRegionProjection`, materialization binding persistence with `ARAStoreObjectsFilter`/`ARARestoreObjectsFilter` support for full-document and partial sub-graph archives, and editor selection projection; Read Audio/materialization birth is driven by batch `AudioModification` refresh, with `PlaybackRegion` used only as placement.
- EditorView UI hook: `OpenTuneEditorView` implements JUCE `ARAEditorView::doNotifySelection`, consumes `ViewSelection::getEffectivePlaybackRegions`, and the VST3 editor derives from `AudioProcessorEditorARAExtension`.
- PlaybackRenderer role: `OpenTunePlaybackRenderer` maintains the host assigned playback-region set, mixes overlaps, clears empty blocks, returns handled ARA silence, and does not use a regular VST3 fallback path while ARA-bound.
- Runtime isolation: the contract rejects old ARA session/state-machine tokens, regular VST3 capture/Standalone arrangement dependencies, and any local preferred-region/fallback-selection state inside `Source/ARA/`.

The build matrix for this change is:

- ARA tests: `OpenTuneTests.exe architecture`
- ARA VST3: `OpenTune_VST3` in `build-ara-overlay-vs18-clean`
- non-ARA VST3: configure `windows-nonara-vs2022`, then build `OpenTune_VST3` in `build-nonara-overlay-vs18-clean`
- non-ARA Standalone: build `OpenTune_Standalone` in `build-nonara-overlay-vs18-clean`

## Test Types

**Unit Tests (L0-L2):**
- Data model invariants: `TimeGridTests.cpp` — handle creation, identity, tau mapping, sorting
- Algorithm correctness: `SoundTouchStretcherTests.cpp` — phase transitions, push/pull streaming, endpoint conservation
- Cache behavior: `TimeStretchCacheTests.cpp`, `TestReferenceFeaturesCacheSmoke.cpp`
- Pure functions: `VocoderConfigTests.cpp` — hash invariants
- Tool behavior: `TimeToolHandlerTests.cpp`, `AutoTunePitchShifterTests.cpp`

**Integration Tests (L3):**
- Pipeline integration: `IntegrationPipelineTests.cpp` — Pitch+Time order independence, Stage 2 interaction, undo across layers
- Cache lifecycle: `TestReferenceFeaturesCacheLifecycle.cpp` — set/get/invalidate through MaterializationStore
- Undo integration: `TestCompositeUndoAction.cpp`, `TestUndoManagerContract.cpp` — composite undo semantics

**Contract / Invariant Tests (L4):**
- `InvariantContractTests.cpp` — bypass bit-exactness, ARA region length, RB reset
- `TestMaterializationContract.cpp` — reference feature cache isolation
- `TestArrangementContract.cpp` — idempotent bindings, import UX flow

**Architecture Guard Tests (static analysis via source scanning):**
- `TestAutoRefArchitecture.cpp` — scans source files for patterns (no #include, no deprecated paths)
- `TestTimelineRenderingPipeline.cpp` — timeline rendering pipeline contracts, source-scan checks
- These tests use `WorkspaceFileCache` and `extractWorkspaceFileSection` helpers in `TestMain.cpp` to verify source code patterns at test runtime

**E2E Tests:** Not used. No E2E or UI automation framework.

**ONNX Inference Tests:**
- `GameNoteGeneratorTests.cpp` — tests the GAME-small ONNX model (gracefully skips if models absent)
- Test environment uses `Ort::InitApi()` call (required by `ORT_API_MANUAL_INIT`) and creates `Ort::Env` instance

## Common Patterns

**Async Testing:**
Tests use the real `OpenTuneAudioProcessor` which manages worker threads internally. Some tests synchronously invoke operations:
```cpp
processor.runReclaimSweepOnMessageThread();  // public for test synchronous invocation
```

**Error Testing:**
```cpp
// Verify failure modes
void runAutoRefFailure_NoOverlap() {
    // Arrange: set up placements that don't overlap
    // Act + Assert: alignment should fail with NoOverlap status
    auto result = processor.executeReferenceAlignmentForPlacement(targetId);
    if (result.status != ReferenceAlignmentResult::Status::NoOverlap) {
        logFail(testName, "expected NoOverlap, got ...");
    }
    logPass(testName);
}
```

**Floating Point Comparison:**
```cpp
if (!approxEqual(result, expected, 1e-6f)) {
    logFail(testName, "values not approximately equal");
    return;
}

// exact comparison for bit-exact tests
constexpr double kEpsBitExact = 0.0;
constexpr double kEpsLinear = 1e-9;
```

**Suite Registration Pattern:**
```cpp
// In TestMain.cpp:
constexpr std::array<SuiteEntry, 36> kSuites{{
    { "time-grid", "vocal-time-stretch TimeGrid data model + tau", &runTimeGridSuite },
    { "soundtouch", "vocal-time-stretch SoundTouchStretcher wrapper (WSOLA)", &runSoundTouchStretcherSuite },
    // ... 36 total suites
}};
```

**Stub for Unwanted Dependencies:**
```cpp
// Tests/TestEditorFactoryStub.cpp — prevents linking real UI shells
juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor&) {
    jassertfalse;
    return nullptr;
}
```

---

*Testing analysis: 2026-06-03*
