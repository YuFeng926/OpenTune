#pragma once

/**
 * Per-note EQ settings data structure
 * 
 * 5-band EQ for piano roll notes:
 * - LowCut: 48dB/oct Butterworth highpass (adjustable freq 20-2000Hz)
 * - LowShelf: Low shelf filter (fixed 500Hz, Q=2, adjustable gain ±12dB)
 * - Peak: Parametric peak (adjustable freq 500Hz-12kHz, Q=2, adjustable gain ±12dB)
 * - HighShelf: High shelf filter (fixed 8kHz, Q=2, adjustable gain ±12dB)
 * - HighCut: 48dB/oct Butterworth lowpass (adjustable freq 4000-20000Hz)
 */

#include <optional>
#include <cmath>
#include <algorithm>

namespace OpenTune {

struct EqBandSettings {
    float gainDb = 0.0f;      // Gain in dB (±12)
    float frequency = 1000.0f; // Frequency in Hz (adjustable for Peak/LowCut/HighCut)
    
    static constexpr float kMinGainDb = -12.0f;
    static constexpr float kMaxGainDb = 12.0f;
    static constexpr float kMinFreq = 20.0f;
    static constexpr float kMaxFreq = 20000.0f;
    
    void setGainDb(float g) {
        gainDb = std::clamp(g, kMinGainDb, kMaxGainDb);
    }
    
    void setFrequency(float f) {
        frequency = std::clamp(f, kMinFreq, kMaxFreq);
    }
};

struct EqSettings {
    static constexpr int kNumBands = 5;
    
    EqBandSettings bands[kNumBands];
    bool active = false;  // Global bypass
    
    // Band indices (match Qt EQGraphWidget)
    static constexpr int kLowCut = 0;
    static constexpr int kLowShelf = 1;
    static constexpr int kPeak = 2;
    static constexpr int kHighShelf = 3;
    static constexpr int kHighCut = 4;
    
    // Fixed frequencies for shelves (from Qt source)
    static constexpr float kLowShelfFreq = 500.0f;
    static constexpr float kHighShelfFreq = 8000.0f;
    static constexpr float kShelfQ = 2.0f;
    
    // Default peak frequency
    static constexpr float kDefaultPeakFreq = 1000.0f;
    
    // Frequency ranges for adjustable bands (from Qt source)
    static constexpr float kLowCutMinFreq = 20.0f;
    static constexpr float kLowCutMaxFreq = 2000.0f;
    static constexpr float kHighCutMinFreq = 4000.0f;
    static constexpr float kHighCutMaxFreq = 20000.0f;
    static constexpr float kPeakMinFreq = 500.0f;
    static constexpr float kPeakMaxFreq = 12000.0f;
    
    EqSettings() {
        // Initialize with flat response
        bands[kLowCut].frequency = 100.0f;    // Default low cut
        bands[kLowShelf].frequency = kLowShelfFreq;
        bands[kLowShelf].gainDb = 0.0f;
        bands[kPeak].frequency = kDefaultPeakFreq;
        bands[kPeak].gainDb = 0.0f;
        bands[kHighShelf].frequency = kHighShelfFreq;
        bands[kHighShelf].gainDb = 0.0f;
        bands[kHighCut].frequency = 8000.0f;  // Default high cut
    }
    
    bool isFlat() const {
        for (const auto& band : bands) {
            if (std::abs(band.gainDb) > 0.01f) return false;
        }
        return true;
    }
};

// Check if two EqSettings are approximately equal
inline bool eqSettingsEqual(const EqSettings& a, const EqSettings& b, float toleranceDb = 0.01f) {
    if (a.active != b.active) return false;
    for (int i = 0; i < EqSettings::kNumBands; ++i) {
        if (std::abs(a.bands[i].gainDb - b.bands[i].gainDb) > toleranceDb) return false;
        if (std::abs(a.bands[i].frequency - b.bands[i].frequency) > 0.1f) return false;
    }
    return true;
}

} // namespace OpenTune