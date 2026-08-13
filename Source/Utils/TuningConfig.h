#pragma once

namespace OpenTune::TuningConfig {

constexpr float kDefaultTuningHz = 440.0f;
constexpr float kMinTuningHz = 400.0f;
constexpr float kMaxTuningHz = 500.0f;

/// Runtime tuning frequency — updated by AppPreferences::setTuning().
/// PitchUtils and ViewMapper read from here at call time.
inline float& currentTuningHz() {
    static float s_tuningHz = kDefaultTuningHz;
    return s_tuningHz;
}

struct TuningSettings {
    float tuningHz = kDefaultTuningHz;
    
    static TuningSettings getDefault() {
        return TuningSettings{};
    }
};

} // namespace OpenTune::TuningConfig
