# Testing Patterns

**Analysis Date:** 2026-04-01

## Overview

OpenTune uses CMake's CTest framework with a single unified test executable (`OpenTuneTests`). The tests use a custom lightweight framework rather than JUCE's UnitTest infrastructure, providing simple pass/fail reporting with section organization.

## Test Framework

**Runner:**
- CTest (CMake's built-in test runner)
- Custom test framework (NOT JUCE UnitTest)

**Configuration:**
- Single test executable: `OpenTuneTests`
- `enable_testing()` enables CTest integration
- Test registered as `OpenTuneCoreTests`

**Run Commands:**
```bash
# Build tests
cmake --build build --config Debug

# Run tests via CTest
cd build && ctest -C Debug --output-on-failure

# Run test executable directly
./build/OpenTuneTests              # Linux/macOS
build\Debug\OpenTuneTests.exe      # Windows

# Run via CMake
cmake --build build --target OpenTuneTests
```

## Test Executable

### OpenTuneTests

**Location:** `Tests/TestMain.cpp` (1021 lines)

**CMake Configuration:**
```cmake
add_executable(OpenTuneTests
    Tests/TestMain.cpp
)

target_include_directories(OpenTuneTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests
        ${ONNXRUNTIME_INCLUDE_DIR}
)

target_link_libraries(OpenTuneTests
    PRIVATE
        OpenTune
        juce::juce_core
        juce::juce_audio_basics
)

enable_testing()
add_test(NAME OpenTuneCoreTests COMMAND OpenTuneTests)
```

## Test File Organization

**Primary Location:**
- `Tests/TestMain.cpp` - All unit tests in single file

**Test Dependencies (via OpenTune target):**
- `Source/Utils/LockFreeQueue.h`
- `Source/Utils/PitchCurve.h`, `Source/Utils/PitchCurve.cpp`
- `Source/Utils/Note.h`
- `Source/Utils/PitchUtils.h`
- `Source/Inference/RenderCache.h`, `Source/Inference/RenderCache.cpp`
- `Source/Inference/VocoderRenderScheduler.h`
- `Source/Inference/F0InferenceService.h`
- `Source/Inference/VocoderInferenceService.h`
- `Source/Standalone/UI/PianoRoll/PianoRollUndoSupport.h`
- `Source/PluginProcessor.h`, `Source/PluginProcessor.cpp`

## Test Executable Definitions

### Example: SilentGapDetectorTest

```cmake
add_executable(SilentGapDetectorTest
    Tests/SilentGapDetectorTest.cpp
    Source/Utils/SilentGapDetector.cpp
    Source/Utils/SilentGapDetector.h
    Source/Utils/SimdAccelerator.cpp
    Source/Utils/SimdAccelerator.h
    Source/Utils/CpuFeatures.cpp
    Source/Utils/CpuFeatures.h
    Source/Utils/AppLogger.cpp
    Source/Utils/AppLogger.h
)

target_include_directories(SilentGapDetectorTest
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(SilentGapDetectorTest
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
```

### Example: OpenTunePitchCurveTests

```cmake
add_executable(OpenTunePitchCurveTests
    Source/Tests/PitchCurveTests.cpp
    Source/Inference/RenderCache.cpp
    Source/Inference/RenderCache.h
    Source/Inference/RenderingManager.cpp
    Source/Inference/RenderingManager.h
    Source/Utils/PitchCurve.cpp
    Source/Utils/PitchCurve.h
    # ... additional source files
)

target_compile_definitions(OpenTunePitchCurveTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_UNIT_TESTS=1
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
)
```

## Test Patterns

### Custom Test Framework (from `Tests/TestMain.cpp`)

The project uses a simple custom test framework, NOT JUCE's UnitTest:

```cpp
// Test helpers
void logPass(const char* testName) {
    std::cout << "[PASS] " << testName << std::endl;
}

void logFail(const char* testName, const char* detail) {
    std::cout << "[FAIL] " << testName << ": " << detail << std::endl;
}

void logSection(const char* section) {
    std::cout << "\n=== " << section << " ===" << std::endl;
}

bool approxEqual(float a, float b, float tol = 1e-5f) {
    return std::abs(a - b) <= tol;
}

bool approxEqual(double a, double b, double tol = 1e-9) {
    return std::abs(a - b) <= tol;
}
```

### Test Function Pattern

Tests are organized into functions by module:

```cpp
void runLockFreeQueueTests();
void runPitchUtilsTests();
void runNoteTests();
void runRenderCacheTests();
void runPitchCurveTests();
void runPianoRollUndoMatrixTests();
void runVocoderSchedulerTests();
void runF0VocoderIsolationTests();
void runVocoderServiceSerializationTests();

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "OpenTune Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    runLockFreeQueueTests();
    runPitchUtilsTests();
    runNoteTests();
    runRenderCacheTests();
    runPitchCurveTests();
    runPianoRollUndoMatrixTests();
    runVocoderSchedulerTests();
    runF0VocoderIsolationTests();
    runVocoderServiceSerializationTests();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Tests Complete" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
```

### Individual Test Pattern

Each test uses scoped blocks with early return on failure:

```cpp
void runLockFreeQueueTests() {
    logSection("LockFreeQueue Tests");

    {
        const char* test = "Basic enqueue/dequeue";
        LockFreeQueue<int> queue(16);

        if (!queue.empty()) { logFail(test, "new queue not empty"); return; }
        queue.try_enqueue(42);
        if (queue.size() != 1) { logFail(test, "size not 1 after enqueue"); return; }

        int value = 0;
        if (!queue.try_dequeue(value)) { logFail(test, "dequeue failed"); return; }
        if (value != 42) { logFail(test, "wrong value"); return; }

        logPass(test);
    }
}
```

### Concurrent Test Pattern

Thread-based tests for concurrent data structures:

```cpp
{
    const char* test = "Concurrent MPMC";
    LockFreeQueue<int> queue(1024);
    std::atomic<int> enqueueCount{0};
    std::atomic<int> dequeueCount{0};
    std::atomic<bool> done{false};

    const int numProducers = 4;
    const int itemsPerProducer = 500;

    std::vector<std::thread> producers, consumers;

    for (int p = 0; p < numProducers; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < itemsPerProducer; ++i) {
                while (!queue.try_enqueue(p * itemsPerProducer + i)) {
                    std::this_thread::yield();
                }
                enqueueCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int c = 0; c < numProducers; ++c) {
        consumers.emplace_back([&]() {
            int value;
            while (!done.load() || !queue.empty()) {
                if (queue.try_dequeue(value)) {
                    dequeueCount.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    while (dequeueCount.load() < numProducers * itemsPerProducer) std::this_thread::yield();
    done.store(true);
    for (auto& t : consumers) t.join();

    if (enqueueCount.load() != numProducers * itemsPerProducer ||
        dequeueCount.load() != numProducers * itemsPerProducer) {
        logFail(test, "count mismatch");
        return;
    }

    logPass(test);
}
```

## Mocking Strategy

**Pattern:** Custom mock classes that inherit from service interfaces

### MockVocoderService Example

Location: `Tests/TestMain.cpp:28-71`

```cpp
class MockVocoderService final : public OpenTune::VocoderInferenceService {
public:
    std::atomic<int> concurrentCalls_{0};
    int maxConcurrentSeen_{0};
    int numSuccessfulCalls_{0};
    int numFailedCalls_{0};
    std::chrono::milliseconds callDuration_{50};
    std::atomic<bool> failOnCall_{false};
    std::string failMessage_{"mock failure"};

    MockVocoderService() = default;

protected:
    OpenTune::Result<std::vector<float>> doSynthesizeAudioWithEnergy(
        const std::vector<float>& f0,
        const std::vector<float>& energy,
        const float* mel,
        size_t melSize) override
    {
        (void)energy;
        (void)mel;
        (void)melSize;

        if (failOnCall_.load()) {
            ++numFailedCalls_;
            return OpenTune::Result<std::vector<float>>::failure(
                OpenTune::ErrorCode::ModelInferenceFailed, failMessage_);
        }

        int before = concurrentCalls_.fetch_add(1);
        if (before >= maxConcurrentSeen_) {
            maxConcurrentSeen_ = before + 1;
        }

        std::this_thread::sleep_for(callDuration_);

        concurrentCalls_.fetch_sub(1);
        ++numSuccessfulCalls_;

        std::vector<float> audio;
        audio.resize(static_cast<size_t>(f0.size()) * 512, 0.5f);
        return OpenTune::Result<std::vector<float>>::success(std::move(audio));
    }
};
```

**Mock Features:**
- Tracks concurrent calls for serialization verification
- Configurable call duration for timing tests
- Configurable failure mode for error handling tests
- No ONNX Runtime dependency

## Test Fixtures

**Pattern:** Inline test data creation within test blocks

```cpp
void runPianoRollUndoMatrixTests() {
    logSection("PianoRoll Undo Matrix Tests");

    OpenTuneAudioProcessor processor;
    UndoManager undoManager;

    ClipSnapshot snap;
    auto clipAudio = std::make_shared<juce::AudioBuffer<float>>(1, 44100);
    clipAudio->clear();
    snap.audioBuffer = clipAudio;
    snap.name = "UndoMatrixClip";
    snap.colour = juce::Colours::lightgrey;
    snap.pitchCurve = std::make_shared<PitchCurve>();
    snap.pitchCurve->setOriginalF0(std::vector<float>(512, 440.0f));
    snap.originalF0State = OriginalF0State::Ready;
    snap.renderCache = std::make_shared<RenderCache>();

    // ... test setup continues
}
```

**Lambda Helper Pattern:**
```cpp
auto resetBaseline = [&]() {
    undoManager.clear();
    auto notes = makeBaseNotes();
    processor.setClipNotes(trackId, clipIndex, notes);
    curve->clearAllCorrections();
    curve->applyCorrectionToRange(notes, 0, frameFromSec(1.0), 0.8f, 0.0f, 6.0f, 44100.0);
};

auto captureState = [&]() -> PianoState {
    PianoState s;
    s.notes = processor.getClipNotes(trackId, clipIndex);
    s.segments = CorrectedSegmentsChangeAction::captureSegments(curve);
    return s;
};
```

## Coverage

**Requirements:** No coverage target defined.

**Coverage Tools Available:**
- MSVC: `/coverage` flag with coverage tools
- GCC/Clang: `--coverage` flag with `gcov`/`lcov`

**Recommendations:**
```cmake
# Add coverage support
option(ENABLE_COVERAGE "Enable code coverage" OFF)
if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(OpenTune PRIVATE --coverage -O0 -g)
    target_link_options(OpenTune PRIVATE --coverage)
endif()
```

## CI/CD Integration

**Status:** No CI/CD configuration detected.

**Missing:**
- `.github/workflows/` directory (GitHub Actions)
- `azure-pipelines.yml` (Azure Pipelines)
- `.gitlab-ci.yml` (GitLab CI)
- Jenkinsfile

**Recommended GitHub Actions Workflow:**

```yaml
# .github/workflows/tests.yml
name: Tests

on: [push, pull_request]

jobs:
  test-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      
      - name: Setup MSVC
        uses: ilammy/msvc-dev-cmd@v1
      
      - name: Configure CMake
        run: cmake -B build -DCMAKE_BUILD_TYPE=Debug
      
      - name: Build
        run: cmake --build build --config Debug
      
      - name: Run Tests
        run: cd build && ctest -C Debug --output-on-failure

  test-macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      
      - name: Configure CMake
        run: cmake -B build -DCMAKE_BUILD_TYPE=Debug
      
      - name: Build
        run: cmake --build build
      
      - name: Run Tests
        run: cd build && ctest --output-on-failure
```

## Test Categories

### Unit Tests (from `Tests/TestMain.cpp`)

**LockFreeQueue Tests:**
- Basic enqueue/dequeue
- Full queue behavior
- Concurrent MPMC (multi-producer, multi-consumer with 4 producers, 4 consumers, 500 items each)

**PitchUtils Tests:**
- Frequency to MIDI conversion (A4=440Hz → MIDI 69)
- MIDI to Frequency conversion
- MIDI round trip accuracy
- Mix retune interpolation (0%, 50%, 100%)

**Note Tests:**
- Note basics (duration calculation, MIDI conversion)
- Note adjusted pitch with pitch offset
- NoteSequence insert sorted
- NoteSequence non-overlapping normalization

**RenderCache Tests:**
- Empty cache initialization
- Add chunk operations
- Revision mismatch handling (wrong revision rejected)

**PitchCurve Tests:**
- Empty curve state
- Set original F0
- Manual correction application
- Snapshot immutability (Copy-on-Write verification)

**PianoRoll Undo Matrix Tests:**
- Draw note undo/redo
- Move notes undo/redo
- Resize note undo/redo
- Delete notes undo/redo
- Hand-draw F0 curve undo/redo
- Line anchor undo/redo
- Retune speed undo/redo
- Vibrato depth undo/redo
- Auto-tune undo/redo

**VocoderRenderScheduler Tests:**
- Scheduler starts not running, requires initialization
- Queue accepts jobs before initialization
- Rejects null service
- Serializes run on single session (verifies maxConcurrentSeen=1)
- Extreme concurrency test (32 jobs)

**F0/Vocoder Isolation Tests:**
- F0 and Vocoder are independent instances
- Vocoder shutdown does not affect F0
- Multiple shutdown is safe
- Independent shutdown order
- Scheduler shutdown does not affect services
- No shared mutex between services

**VocoderInferenceService Serialization Tests:**
- Serializes run on single session (16 concurrent callers)
- Verifies maxConcurrentSeen=1

### Integration Tests

Tests that verify component interactions:
- PianoRollUndoSupport with OpenTuneAudioProcessor
- RenderCache with revision tracking
- VocoderRenderScheduler with MockVocoderService

### Concurrency Tests

Tests specifically for thread safety:
- LockFreeQueue MPMC test
- VocoderScheduler serial execution test
- VocoderService concurrent access test

## Test Dependencies

### JUCE Modules Used in Tests

```cmake
# Core modules
juce::juce_core
juce::juce_audio_basics

# Via OpenTune target
juce::juce_audio_utils
juce::juce_audio_processors
juce::juce_dsp
juce::juce_opengl
juce::juce_graphics
juce::juce_gui_basics
juce::juce_gui_extra
```

### ONNX Runtime

Tests link against `OpenTune` which includes ONNX Runtime. Test executable copies ONNX DLLs post-build:
```cmake
add_custom_command(TARGET OpenTuneTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_LIB_DIR}/onnxruntime.dll"
        "$<TARGET_FILE_DIR:OpenTuneTests>/onnxruntime.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_LIB_DIR}/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:OpenTuneTests>/onnxruntime_providers_shared.dll"
    COMMENT "Copying ONNX Runtime DLLs to unit tests..."
)
```

### Model Files

Tests do NOT require model files - they use mock implementations.

## CI/CD Integration

**Status:** No CI/CD configuration detected.

**Missing:**
- `.github/workflows/` directory (GitHub Actions)
- `azure-pipelines.yml` (Azure Pipelines)
- `.gitlab-ci.yml` (GitLab CI)

**Recommended GitHub Actions Workflow:**

```yaml
# .github/workflows/tests.yml
name: Tests

on: [push, pull_request]

jobs:
  test-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      
      - name: Setup MSVC
        uses: ilammy/msvc-dev-cmd@v1
      
      - name: Configure CMake
        run: cmake -B build -G "Visual Studio 17 2022" -A x64
      
      - name: Build
        run: cmake --build build --config Release
      
      - name: Run Tests
        run: cd build && ctest -C Release --output-on-failure

  test-macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      
      - name: Configure CMake
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release
      
      - name: Build
        run: cmake --build build
      
      - name: Run Tests
        run: cd build && ctest --output-on-failure
```

## Coverage

**Requirements:** No coverage target defined.

**View Coverage:**
```bash
# With GCC/Clang
cmake -B build -DENABLE_COVERAGE=ON
cmake --build build
cd build && ctest
gcovr -r .. --html-details coverage.html
```

## Best Practices Used

1. **Unified Test Executable:** All tests in single `OpenTuneTests` target
2. **Mock Objects:** `MockVocoderService` for testing without ONNX
3. **Section Organization:** Tests grouped by functional area
4. **Early Return Pattern:** Tests fail fast on first error
5. **Concurrent Testing:** Thread safety verified with multi-threaded tests

## Anti-Patterns to Avoid

1. **Don't** require model files for unit tests (use mocks)
2. **Don't** create interdependent tests (each should be isolated)
3. **Don't** use `std::cout` directly - use `logPass`/`logFail` helpers
4. **Don't** skip timeout handling in concurrent tests

---

*Testing analysis: 2026-04-01*
