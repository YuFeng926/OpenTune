# Coding Conventions

**Analysis Date:** 2026-06-02

## Languages

- **C++17** — Primary language. C++ standard enforced via `CMAKE_CXX_STANDARD 17` with `CMAKE_CXX_EXTENSIONS OFF` in `CMakeLists.txt:30-32`
- **Python 3** — Secondary (analysis scripts only, in `Python/`)
- **CMake 3.22+** — Build system

## Naming Patterns

**Files:**
- `PascalCase` for headers and source files: `PluginProcessor.h`, `MaterializationStore.cpp`
- Interface (pure abstract) files use `I` prefix: `IF0Extractor.h`, `INoteGenerator.h`
- Test files use either `Tests/{Name}.cpp` or `Tests/Test{Name}.cpp` pattern: `TimeGridTests.cpp`, `TestUndoManagerContract.cpp`

**Classes:**
- `PascalCase` throughout: `OpenTuneAudioProcessor`, `MaterializationStore`, `StandaloneArrangement`
- Interface classes use `I` prefix: `IF0Extractor`, `INoteGenerator`, `VocoderInterface`
- Structs are `PascalCase`: `CorrectedSegment`, `PreparedImport`, `HostTransportSnapshot`

**Functions:**
- `camelCase` member functions: `prepareToPlay()`, `getSampleRate()`, `commitPreparedImportAsPlacement()`
- `camelCase` free functions in anonymous namespace or `OpenTune` namespace
- JUCE override functions follow JUCE naming: `createEditor()`, `processBlock()`, `handleAsyncUpdate()`
- Factory functions use `make` prefix: `makePreparedImport()`, `makeTestClipRequest()`, `makeHandle()`

**Variables:**
- `camelCase` for local and member variables: `currentSampleRate_`, `trackHeight_`, `showWaveform_`
- Member variables have trailing underscore: `undoManager_`, `pianoKeyAudition_`, `materializationStore_`
- Static constants use `k` prefix: `kSampleRate`, `kPi`, `kMaxPeriodSamples`
- `constexpr` globals use `k` prefix: `kRenderSampleRate`, `kTrackPanelCardInsetX`, `kCurrentProjectFormatVersion`

**Namespaces:**
- All production code in `OpenTune` namespace declared via `namespace OpenTune { ... }`
- Sub-namespaces for subsystems: `Capture`, `ARA::PlugIn`
- JUCE library types accessed via `juce::` prefix (not using `using namespace juce` in headers)

**Enums:**
- `enum class` for scoped enums: `enum class HandleKind : uint8_t`, `enum class LogLevel`
- Values are `PascalCase`: `HandleKind::ClipStart`, `ErrorCode::ModelNotFound`, `AutoRefAvailability::Status::NoReference`

## Code Style

**Formatting:**
- No `.clang-format` or `.editorconfig` detected — style enforced via manual review and `.clangd` diagnostics
- 4-space indentation observed throughout
- Opening brace on same line for classes/functions: `class Foo {`
- `#pragma once` for all header guards (no `#ifndef` guards)

**Clangd Configuration** (`.clangd`):
- `UnusedIncludes: Strict` — includes must be justified
- `modernize-use-trailing-return-type` and `readability-identifier-length` diagnostics suppressed
- Inlay hints enabled for parameter names and deduced types

**MSVC Compiler Flags:**
- `/utf-8` — source encoding is UTF-8
- `/MP` — multi-processor compilation
- `/wd4100` `/wd4127` — suppressed unreferenced parameter / conditional constant warnings
- `/arch:AVX2` selectively on SIMD hot files: `SimdAccelerator.cpp`, `MelSpectrogram.cpp`

**Language Idioms:**
- Heavy use of `constexpr` for compile-time constants: `constexpr double kSampleRate = 44100.0`
- Widespread `noexcept` on getters and helper functions: `double getSampleRate() const noexcept { ... }`
- `override` keyword on all virtual function overrides
- `default` keyword for default constructors/destructors: `virtual ~UndoAction() = default;`
- Move semantics: `std::move()` used for transferring ownership
- `= delete` used to suppress copy: `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`
- `auto` for iterator types and complex template expressions
- Forward declarations to minimize header coupling: `namespace Capture { class CaptureSession; }`

## Import Organization

**Order** (observed in `PluginProcessor.h` and consistent across codebase):
1. Standard library includes (`<memory>`, `<atomic>`, `<vector>`, etc.)
2. Third-party/JUCE includes (`<juce_audio_processors/juce_audio_processors.h>`)
3. Project includes (`"SourceStore.h"`, `"Utils/PitchCurve.h"`)
4. Forward declarations at the end of the include block

**Path Style:**
- Project includes use relative paths from `Source/`: `"Utils/AppLogger.h"`, `"DSP/ResamplingManager.h"`
- No path aliases — includes reference the directory structure directly

## Error Handling

**Primary Pattern — Result<T>** (`Source/Utils/Error.h`):
- Custom `Result<T>` class wrapping `std::variant<T, Error>` (a Rust-like Result type)
- Error codes defined as `enum class ErrorCode` with ranges: 100s (model), 200s (init), 300s (audio), 400s (F0), 500s (mel), 600s (params)
- `Result::success(value)` / `Result::failure(code, context)` factory methods
- `valueOr(defaultValue)` for safe extraction with fallback

**Secondary Pattern — `std::optional`:**
- Used for operations that may not produce a result: `std::optional<SplitOutcome>`, `std::optional<MergeOutcome>`
- Call sites check `.isValid()` or use `if (result.has_value())`

**Tertiary Pattern — Inline Status Enums:**
- Embedded status enums within struct types: `ReferenceAlignmentResult::Status`, `AutoRefAvailability::Status`
- Caller checks `succeeded()` or `canRunAutoRef()` methods

**Assertions:**
- `jassertfalse` for unreachable paths
- `jassert` in JUCE-compatible debug builds

**Exception Safety:**
- Minimal use of exceptions — the `Error`/`Result` pattern is preferred
- `Result<void>::value()` throws on error access, but callers expected to check `ok()` first

## Logging

**Framework:** Custom `AppLogger` (`Source/Utils/AppLogger.h` / `.cpp`)

**Patterns:**
- Singleton with static methods: `AppLogger::info("message")`, `AppLogger::error("message")`
- Four log levels: `Debug`, `Info`, `Warning`, `Error`
- Log output to file in user app data directory
- Structured log format: `[LEVEL] message`
- Thread-safe via internal mutex (implied by singleton pattern)

## Comments

**Documentation Style:**
- Top-level file comments in Chinese describing module purpose and responsibilities
- Doxygen-style `@brief`, `@param`, `@return` on interfaces (`IF0Extractor.h`, `INoteGenerator.h`)
- Block comments `/** ... */` for class/file-level documentation
- Line comments `//` for inline explanations

**Thread Safety Documentation:**
- Explicitly documented in class headers: "线程安全说明" blocks describing which thread owns which data
- Atomic variables annotated with load/store memory orders: `.load(std::memory_order_relaxed)`

**Spec References:**
- Test files reference the spec they implement: `"Covers spec: openspec/changes/vocal-time-stretch/specs/time-grid/spec.md"`

**When to Comment:**
- Class-level headers always have a descriptive comment block
- Complex algorithms and state machines are documented
- Design decisions and rationale ("为什么这样做") are inline-commented
- Public API functions are documented with parameter descriptions

## Function Design

**Size:** Functions are generally concise — helper functions extracted for reuse. Complex classes (e.g., `OpenTuneAudioProcessor` at 885 lines in header) use private helpers.

**Parameters:**
- `const&` for non-primitive input parameters: `const juce::String& displayName`
- Pointer for output/optional parameters: `PreparedImport& out`
- `std::move` to transfer ownership: `std::unique_ptr<UndoAction>` parameters

**Return Values:**
- `bool` for success/failure: `bool prepareToPlay(...)`, `bool commitPreparedImportAsPlacement(...)`
- `std::optional<T>` for nullable results
- `Result<T>` in `Source/Utils/Error.h` (not widely adopted yet — mostly in Inference layer)
- `const&` as return for member access: `const std::vector<TimeHandle>& handles() const noexcept { return handles_; }`

## Module Design

**Exports:** Each `.h` file declares its public interface. No barrel/index files.

**Directory Organization:**
- `Source/ARA/` — ARA (Audio Random Access) SDK integration for VST3
- `Source/Audio/` — Audio format support
- `Source/DSP/` — Signal processing (F0, pitch shift, mel spectrogram, chroma)
- `Source/Editor/` — Shared editor components (preferences, dialogs)
- `Source/Inference/` — AI/ONNX inference engine (vocoder, F0 extraction, note generation)
- `Source/Plugin/` — Plugin-specific editor and capture pipeline
- `Source/Services/` — Async services (F0 extraction, reference analysis)
- `Source/Standalone/` — Standalone app UI (editor, piano roll, arrangement view)
- `Source/Utils/` — Utilities (logging, preferences, undo, serialization, data structures)

**Class Design:**
- Single Responsibility: `SourceStore` owns sources, `MaterializationStore` owns materializations, `StandaloneArrangement` owns placements
- Dependency injection via constructor or setter: `setAppPreferences(AppPreferences* prefs)`
- COW (Copy-on-Write) snapshot pattern for thread-safe data: `TimeGridSnapshot` shared via `std::shared_ptr<const T>` and `atomic_store/load`
- Listener/Observer pattern: `ReferenceAnalysisService::Listener` interface

**Thread Safety:**
- `std::atomic` for shared state between threads: `currentSampleRate_`, `isPlaying_`, `f0Ready_`
- `std::mutex`/`std::SpinLock` for critical sections: `schedulerMutex_`, `stage2Mutex_`
- Immutable snapshots (`shared_ptr<const T>`) for lock-free reads from audio thread

---

*Convention analysis: 2026-06-02*
