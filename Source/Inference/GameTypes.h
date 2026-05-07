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
     * detail=1:  threshold=0.35, radius=3, steps=6,  presence=0.3
     * detail=5:  threshold=0.18, radius=1, steps=10, presence=0.12
     * detail=10: threshold=0.08, radius=1, steps=14, presence=0.05
     */
    static GameConfig fromDetailLevel(int detail) {
        const float t = static_cast<float>(std::clamp(detail, 1, 10) - 1) / 9.0f;
        GameConfig cfg;
        cfg.boundaryThreshold = 0.35f - t * 0.27f;     // 0.35 → 0.08
        cfg.boundaryRadius = std::max(1, static_cast<int>(3.0f - t * 2.0f + 0.5f)); // 3 → 1
        cfg.d3pmSteps = static_cast<int>(6.0f + t * 8.0f + 0.5f);      // 6 → 14
        cfg.presenceThreshold = 0.3f - t * 0.25f;      // 0.3 → 0.05
        return cfg;
    }
};

} // namespace OpenTune
