/**
 * WordSegmenter — Two-tier handle seeding for TimeGrid initialization.
 *
 * Tier 1 (always emitted): PhonemeClass transitions
 *   Voiced↔Sibilant↔Silence boundaries
 *   labeled by destination class (OnsetVoiced / OnsetSibilant / OnsetSilence)
 *
 * Tier 2 (supplementary): OnsetDetector events
 *   Internal-onset events that don't have a Tier-1 event within ±50 ms.
 *   Captures within-class transients (e.g., note attack inside a sustained vowel).
 *
 * NMS: 50 ms radius, Tier 1 always wins over Tier 2.
 *
 * Output: list of seed handles to feed into TimeGridSnapshot::makeFromHandles
 * (caller adds ClipStart/ClipEnd).
 *
 * Design v7 §3 — BeatGrid intentionally NOT used (clip may not be beat-aligned).
 *
 * Spec: openspec/changes/vocal-time-stretch/specs/word-segmenter/spec.md
 */
#pragma once

#include "../Utils/TimeGrid.h"     // for HandleKind
#include "PhonemeClassifier.h"      // for PhonemeClass
#include <cstdint>
#include <vector>

namespace OpenTune {

struct WordSegmenterConfig {
    int frameRateHz = 100;
    int nmsRadiusFrames = 5;     // ±50 ms (5 frames @ 100 fps)
    int minHandleGapMs = 30;     // post-process: collapse handles within this gap
};

/**
 * Internal candidate event (combined from Tier 1 and Tier 2 sources).
 */
struct WordSegmenterCandidate {
    int frame100fps = 0;
    int priority = 0;            // 3 = Tier 1 (class transition), 1 = Tier 2 (onset)
    HandleKind kind = HandleKind::UserAdded;
};

/**
 * Output of segmentation.
 *
 * `seedHandleFrames[i]` is each seed handle's 100 fps frame index.
 * `seedHandleKinds[i]` matches with the corresponding kind (parallel arrays).
 */
struct WordSegmenterResult {
    std::vector<int>        seedHandleFrames;
    std::vector<HandleKind> seedHandleKinds;
    std::vector<WordSegmenterCandidate> allCandidates;  // pre-NMS, for diagnostic
    std::vector<WordSegmenterCandidate> kept;           // post-NMS, parallel to seedHandleFrames
};

class WordSegmenter {
public:
    void configure(const WordSegmenterConfig& cfg) noexcept { config_ = cfg; }
    const WordSegmenterConfig& getConfig() const noexcept { return config_; }

    /**
     * Compute segmentation.
     *
     * @param phonemeClasses        Sequential PhonemeClass per 100 fps frame.
     * @param onsetFrames100fps     OnsetDetector output (Tier 2 candidates).
     * @return                      Seed handles ready for TimeGrid construction.
     */
    WordSegmenterResult segment(const std::vector<PhonemeClass>& phonemeClasses,
                                const std::vector<int>& onsetFrames100fps) const;

private:
    WordSegmenterConfig config_;
};

} // namespace OpenTune
