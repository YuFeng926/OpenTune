#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace OpenTune {

enum class GameLanguage : int64_t {
    Universal = 0,
    English = 1,
    Japanese = 2,
    Cantonese = 3,
    Chinese = 4
};

struct ReferenceNote {
    double onset;       // seconds, materialization-local
    double offset;      // seconds, materialization-local
    float midiPitch;    // floating point MIDI scale (A4 = 69.0)
    bool voiced;        // presence flag (true = voiced note)
};

struct GameConfig {
    float boundaryThreshold = 0.3f;
    int boundaryRadius = 2;           // frames (10ms each)
    int d3pmSteps = 8;
    float presenceThreshold = 0.2f;
    GameLanguage language = GameLanguage::Universal;

    /**
     * Create GameConfig from "Note Detail" knob value (1~10).
     * detail=1:  threshold=0.6, radius=5, steps=2, presence=0.5
     * detail=5:  threshold=0.3, radius=2, steps=8, presence=0.2
     * detail=10: threshold=0.15, radius=1, steps=12, presence=0.1
     */
    static GameConfig fromDetailLevel(int detail) {
        const float t = static_cast<float>(std::clamp(detail, 1, 10) - 1) / 9.0f;
        GameConfig cfg;
        cfg.boundaryThreshold = 0.6f - t * 0.45f;      // 0.6 → 0.15
        cfg.boundaryRadius = static_cast<int>(5.0f - t * 4.0f + 0.5f); // 5 → 1
        cfg.d3pmSteps = static_cast<int>(2.0f + t * 10.0f + 0.5f);     // 2 → 12
        cfg.presenceThreshold = 0.5f - t * 0.4f;       // 0.5 → 0.1
        return cfg;
    }
};

} // namespace OpenTune
