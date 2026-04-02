# Coding Conventions

**Analysis Date:** 2026-04-01

## Overview

OpenTune is a C++17 JUCE-based AI pitch correction application. The codebase follows consistent conventions derived from JUCE best practices with some project-specific patterns.

## Language & Standard

**Primary Language:** C++17

**Configuration:**
- C++ Standard: C++17 (`CMAKE_CXX_STANDARD 17`)
- Extensions: Disabled (`CMAKE_CXX_EXTENSIONS OFF`)
- Required: ON (`CMAKE_CXX_STANDARD_REQUIRED ON`)

## Header Guards

**Pattern:** Always use `#pragma once`

```cpp
#pragma once

/**
 * Documentation comment
 */

#include <vector>
// ...
```

**Location:** First line of every header file. Do NOT use traditional `#ifndef/#define/#endif` guards.

## Naming Conventions

### Classes and Structs

**Pattern:** PascalCase

```cpp
class OpenTuneAudioProcessor { ... };
class PianoRollComponent { ... };
struct SilentGap { ... };
struct MelSpectrogramConfig { ... };
```

### Functions and Methods

**Pattern:** camelCase

```cpp
void prepareToPlay(double sampleRate, int samplesPerBlock);
bool isBusesLayoutSupported(const BusesLayout& layouts) const;
std::vector<float> extractF0(const float* audio, size_t length, int sampleRate);
```

### Member Variables

**Pattern:** camelCase with trailing underscore for private/protected members

```cpp
private:
    std::shared_ptr<PitchCurve> currentCurve_;
    double zoomLevel_ = 1.0;
    int scrollOffset_ = 0;
    std::atomic<bool> isPlaying_{false};
```

### Local Variables

**Pattern:** camelCase, no trailing underscore

```cpp
double durationSec = static_cast<double>(audioLength) / sampleRate;
size_t requiredMB = estimateMemoryRequiredMB(audioSamples16k);
```

### Constants

**Pattern:** `k` prefix + PascalCase for static constexpr, or SCREAMING_SNAKE_CASE

```cpp
static constexpr int MAX_TRACKS = 12;
static constexpr double kDefaultSampleRate = 44100.0;
static constexpr size_t kDefaultGlobalCacheLimitBytes = 1536ULL * 1024 * 1024;
static constexpr float kDefaultThreshold_dB = -40.0f;
```

### Enums

**Pattern:** PascalCase for enum class names, PascalCase for values

```cpp
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

enum class OriginalF0State : uint8_t {
    NotRequested = 0,
    Extracting,
    Ready,
    Failed
};

enum class TimeUnit {
    Seconds,
    Bars
};
```

### Namespaces

**Pattern:** PascalCase, all code in `OpenTune` namespace

```cpp
namespace OpenTune {

class SomeClass { ... };

} // namespace OpenTune
```

## File Organization

### Header File Structure

```cpp
#pragma once

/**
 * Brief description (Chinese or English)
 * 
 * Detailed explanation of purpose and usage.
 */

#include <juce_core/juce_core.h>
#include <vector>
#include <memory>

namespace OpenTune {

// Forward declarations (if needed)
class ForwardDeclaredClass;

// Main declarations
class MyClass {
    // ...
};

} // namespace OpenTune
```

### Source File Structure

```cpp
#include "MyClass.h"
#include "OtherDependency.h"
#include "../Utils/AppLogger.h"

#include <cmath>
#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace OpenTune {

// Implementation

} // namespace OpenTune
```

### Include Order

1. Header matching the source file (if .cpp)
2. JUCE headers (grouped by module)
3. Project headers (relative paths)
4. STL headers
5. Platform-specific headers (with `#ifdef` guards)

## Code Style

### Braces

**Pattern:** Opening brace on same line (K&R style)

```cpp
class MyClass {
public:
    void method() {
        if (condition) {
            // ...
        } else {
            // ...
        }
    }
};
```

### Indentation

- 4 spaces per level (no tabs specified in config)
- JUCE standard practice

### Line Length

- No explicit limit enforced
- Aim for readability, typically under 120 characters

### Whitespace

- Space after keywords (`if (`, `for (`, `while (`)
- Space around operators (`a + b`, `x = y`)
- No trailing whitespace

## Documentation

### Comment Style

**Pattern:** Chinese documentation comments are common; use block comments (`/** */`) for API documentation

```cpp
/**
 * OpenTune 核心音频处理器
 * 
 * OpenTuneAudioProcessor 是整个应用的核心，负责：
 * - 多轨道音频数据管理（最多 MAX_TRACKS 个轨道）
 * - 实时音频播放和混音（processBlock）
 * - AI 推理调度（通过 RenderingManager）
 */

/**
 * @brief Extract F0 from audio buffer
 * @param audio Input audio buffer (raw PCM float samples)
 * @param length Number of samples in audio buffer
 * @param sampleRate Input sample rate (e.g., 44100, 48000, 96000)
 * @return F0 curve in Hz (one value per hop frame at target sample rate)
 */
```

### Inline Comments

**Pattern:** Use `//` for inline comments, `/* */` for block comments

```cpp
// Multi-track support
int activeTrackId_{0};

/* 
 * Duration gate check - reject audio longer than limit 
 */
if (durationSec > kMaxAudioDurationSec) {
    // ...
}
```

## Type System

### Primitive Types

- Use `int`, `float`, `double` for numeric values
- Use `size_t` for sizes and counts
- Use `int64_t`, `uint64_t` for cross-platform fixed-width integers

### Strings

**Pattern:** Use `juce::String` for all strings

```cpp
juce::String getName() const override;
juce::String lastExportError_;
```

### Containers

**Pattern:** Prefer STL containers; use JUCE containers when JUCE integration is needed

```cpp
std::vector<Note> notes_;
std::array<TrackState, MAX_TRACKS> tracks_;
std::map<double, Chunk> chunks_;
std::unordered_map<int, std::vector<float>> resampledAudio;
juce::AudioBuffer<float> audioBuffer;
```

### Smart Pointers

**Pattern:** Use `std::unique_ptr` for ownership, `std::shared_ptr` for shared ownership

```cpp
std::unique_ptr<RenderingManager> renderingManager_;
std::shared_ptr<PitchCurve> currentCurve_;
std::unique_ptr<juce::dsp::FFT> fft_;
```

### JUCE Types

Use JUCE's type system consistently:

```cpp
juce::AudioBuffer<float>          // Audio buffers
juce::String                       // Strings
juce::File                         // File paths
juce::Colour                       // Colors
juce::Rectangle<int>               // Geometry
juce::CriticalSection              // Thread synchronization
juce::ReadWriteLock                // Read-write locks
juce::ListenerList<Listener>       // Observer pattern
```

## Error Handling

### Logging

**Pattern:** Use `AppLogger` for all logging; never use `std::cout` or `printf`

```cpp
// From Source/Utils/AppLogger.h
AppLogger::debug("Detailed diagnostic message");
AppLogger::info("Normal operation message");
AppLogger::warn("Warning condition");
AppLogger::error("Error condition");
```

**Log Levels:**
- `Debug`: Detailed diagnostics (disabled in release)
- `Info`: Normal operation messages
- `Warning`: Warning conditions
- `Error`: Error conditions

**Log Prefix Convention:**
```cpp
AppLogger::error("[InferenceManager] ONNX error: " + juce::String(e.what()));
AppLogger::info("[InferenceManager] Initialized with F0 model: " + modelName);
```

### Result<T> Pattern (Primary Error Handling)

**Pattern:** Use `Result<T>` for fallible operations instead of exceptions

```cpp
// From Source/Utils/Error.h
template<typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error err) : data_(std::move(err)) {}
    
    static Result success(T value);
    static Result failure(ErrorCode code, const std::string& context = "");
    
    bool ok() const;
    explicit operator bool() const;
    
    const T& value() const&;
    T&& value() &&;
    const T& operator*() const&;
    const T* operator->() const;
    const Error& error() const&;
    
    T valueOr(T defaultValue) const&;
    
    template<typename F>
    auto map(F&& f) const& -> Result<decltype(f(std::declval<T>()))>;
    
    template<typename F>
    auto andThen(F&& f) const& -> decltype(f(std::declval<T>()));
};
```

**Usage Examples:**
```cpp
// Function returning Result
Result<void> configure(const MelSpectrogramConfig& cfg);
F0Result extractF0(const float* audio, size_t length, int sampleRate);

// Checking and using result
auto result = extractF0(audio, length, sampleRate);
if (!result) {
    AppLogger::error("[InferenceManager] " + result.error().fullMessage());
    return Result<T>::failure(result.error());
}
return Result<T>::success(std::move(result.value()));

// Chaining operations
return result.andThen([](const auto& f0) {
    return synthesizeAudio(f0);
});
```

**Error Class:**
```cpp
struct Error {
    ErrorCode code{ErrorCode::Success};
    std::string message;
    std::string context;
    
    static Error fromCode(ErrorCode c, const std::string& ctx = "");
    bool ok() const;
    std::string fullMessage() const;
};

enum class ErrorCode : int {
    Success = 0,
    ModelNotFound = 100,
    ModelLoadFailed = 101,
    NotInitialized = 200,
    InvalidAudioInput = 300,
    F0ExtractionFailed = 401,
    // ...
};
```

### Error Return Pattern (Alternative)

**Pattern:** Return bool for success/failure, use output parameters for results

```cpp
bool prepareImportClip(int trackId,
                       juce::AudioBuffer<float>&& inBuffer,
                       double inSampleRate,
                       const juce::String& clipName,
                       PreparedImportClip& out);

std::optional<SilentGap> findNearestGap(
    const std::vector<SilentGap>& gaps,
    double positionSeconds,
    double maxSearchDistanceSec,
    bool searchForward);
```

### Exceptions

**Pattern:** Exceptions caught at integration boundaries (ONNX Runtime, file I/O) and converted to `Result<T>` or logged

```cpp
try {
    // ONNX operations
    return InferenceManager::AudioResult::success(currentVocoder_->synthesize(f0, mel));
} catch (const Ort::Exception& e) {
    return InferenceManager::AudioResult::failure(ErrorCode::ModelInferenceFailed, e.what());
} catch (const std::exception& e) {
    AppLogger::error("[InferenceManager] Error: " + juce::String(e.what()));
    return false;
} catch (...) {
    AppLogger::error("[InferenceManager] Unknown error");
    return false;
}
```

## Thread Safety

### Atomics

**Pattern:** Use `std::atomic` for thread-safe primitives

```cpp
std::atomic<double> currentSampleRate_{44100.0};
std::atomic<bool> isPlaying_{false};
std::atomic<uint64_t> nextClipId_{1};
```

### Locks

**Pattern:** Use JUCE's thread synchronization primitives

```cpp
mutable juce::ReadWriteLock tracksLock_;     // Read-write lock for shared data
mutable juce::SpinLock lock_;                 // Spin lock for fast operations
static juce::CriticalSection& getLoggerLock(); // Critical section for logging
```

### Lock Usage Pattern

```cpp
// Write lock (UI thread)
void setActiveTrack(int trackId) {
    const juce::ScopedWriteLock sl(tracksLock_);
    activeTrackId_ = trackId;
}

// Read lock (audio thread)
int getActiveTrackId() const {
    const juce::ScopedReadLock sl(tracksLock_);
    return activeTrackId_;
}
```

## Memory Management

### RAII

**Pattern:** Always use RAII for resource management

```cpp
// RAII performance timer
class PerfTimer {
public:
    explicit PerfTimer(const juce::String& operation)
        : operation_(operation)
        , startTime_(juce::Time::getMillisecondCounterHiRes())
    {}
    
    ~PerfTimer() {
        double elapsed = juce::Time::getMillisecondCounterHiRes() - startTime_;
        AppLogger::logPerf(operation_, elapsed);
    }
private:
    juce::String operation_;
    double startTime_;
};
```

### Memory Ownership

- `std::unique_ptr`: Exclusive ownership
- `std::shared_ptr`: Shared ownership (use sparingly)
- Raw pointers: Non-owning references only

## Platform-Specific Code

**Pattern:** Use `#ifdef` guards for platform-specific code

```cpp
#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

if(WIN32)
    target_compile_definitions(OpenTune PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(OpenTune PRIVATE /utf-8 /MP /wd4100 /wd4127)
endif()
```

## Compiler Warnings

**MSVC Warning Suppressions:**
- `/wd4100`: Unreferenced formal parameter
- `/wd4127`: Conditional expression is constant

**JUCE Recommended Flags:**
```cmake
juce::juce_recommended_config_flags
juce::juce_recommended_lto_flags
juce::juce_recommended_warning_flags
```

## Formatting Configuration

**Status:** No explicit `.clang-format` or `.editorconfig` file found.

**Inferred Rules:**
- Indent: 4 spaces (JUCE default)
- Braces: K&R style (same line)
- Max line length: ~120 characters (inferred from codebase)

**Recommendation:** Add `.clang-format` file to enforce consistent formatting.

## Linting Configuration

**Status:** No explicit linting configuration found.

**Compiler Warnings Used:**
- JUCE recommended warning flags via CMake
- MSVC: `/wd4100 /wd4127` (suppress specific warnings)

**Recommendation:** Consider adding `clang-tidy` configuration for static analysis.

## Import/Include Patterns

### Header Includes

Use angle brackets for system/external headers, quotes for project headers:

```cpp
#include <juce_core/juce_core.h>      // External (JUCE)
#include <vector>                      // STL
#include <cmath>                       // C standard library

#include "PitchCurve.h"                // Project header (same directory)
#include "../Utils/AppLogger.h"        // Project header (relative path)
```

### JUCE Module Includes

JUCE modules are included via single umbrella headers:

```cpp
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_dsp/juce_dsp.h>
```

## Preprocessor Definitions

**Common Definitions (from CMakeLists.txt):**

```cmake
JUCE_WEB_BROWSER=0
JUCE_USE_CURL=0
JUCE_VST3_CAN_REPLACE_VST2=0
JUCE_REPORT_APP_USAGE=0
JUCE_STRICT_REFCOUNTEDPOINTER=1
JUCE_ASIO=1
JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
JUCE_USE_FLAC=1
JUCE_USE_OGGVORBIS=1
JUCE_USE_MP3AUDIOFORMAT=1
```

---

*Convention analysis: 2026-04-01*
