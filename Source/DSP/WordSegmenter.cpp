#include "WordSegmenter.h"

#include <algorithm>

namespace OpenTune {

namespace {

constexpr int kTier1Priority = 3;
constexpr int kTier2Priority = 1;

HandleKind kindForDestinationClass(PhonemeClass c) noexcept
{
    switch (c) {
        case PhonemeClass::Voiced:   return HandleKind::OnsetVoiced;
        case PhonemeClass::Sibilant: return HandleKind::OnsetSibilant;
        case PhonemeClass::Silence:  return HandleKind::OnsetSilence;
    }
    return HandleKind::UserAdded;
}

} // namespace

WordSegmenterResult WordSegmenter::segment(const std::vector<PhonemeClass>& phonemeClasses,
                                            const std::vector<int>& onsetFrames100fps) const
{
    WordSegmenterResult result;

    // ─── Tier 1: PhonemeClass transitions ────────────────────────────────
    std::vector<WordSegmenterCandidate> tier1;
    if (phonemeClasses.size() >= 2) {
        for (size_t i = 1; i < phonemeClasses.size(); ++i) {
            if (phonemeClasses[i] != phonemeClasses[i - 1]) {
                WordSegmenterCandidate cand;
                cand.frame100fps = static_cast<int>(i);
                cand.priority    = kTier1Priority;
                cand.kind        = kindForDestinationClass(phonemeClasses[i]);
                tier1.push_back(cand);
            }
        }
    }

    // ─── Tier 2: OnsetDetector events not within ±NMS of any Tier 1 ──────
    std::vector<WordSegmenterCandidate> tier2;
    const int nmsRadius = std::max(1, config_.nmsRadiusFrames);
    for (int frame : onsetFrames100fps) {
        if (frame < 0) continue;
        if (!phonemeClasses.empty() && frame >= static_cast<int>(phonemeClasses.size())) continue;

        bool nearTier1 = false;
        for (const auto& t1 : tier1) {
            if (std::abs(t1.frame100fps - frame) <= nmsRadius) {
                nearTier1 = true;
                break;
            }
        }
        if (nearTier1) continue;

        WordSegmenterCandidate cand;
        cand.frame100fps = frame;
        cand.priority    = kTier2Priority;
        cand.kind        = HandleKind::InternalOnset;
        tier2.push_back(cand);
    }

    // ─── Combine + sort by frame ─────────────────────────────────────────
    std::vector<WordSegmenterCandidate> all;
    all.reserve(tier1.size() + tier2.size());
    all.insert(all.end(), tier1.begin(), tier1.end());
    all.insert(all.end(), tier2.begin(), tier2.end());
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
        return a.frame100fps < b.frame100fps;
    });
    result.allCandidates = all;

    // ─── NMS within ±NMS frames ─────────────────────────────────────────
    // Within a window, keep highest priority; ties broken by earliest frame.
    std::vector<WordSegmenterCandidate> kept;
    for (const auto& cand : all) {
        if (kept.empty()) {
            kept.push_back(cand);
            continue;
        }
        auto& last = kept.back();
        if (cand.frame100fps - last.frame100fps <= nmsRadius) {
            // Conflict: replace if higher priority
            if (cand.priority > last.priority) {
                last = cand;
            }
            // else: drop current (lower or equal priority and later — keep the earlier)
        } else {
            kept.push_back(cand);
        }
    }

    // ─── Optional: collapse handles within minHandleGapMs ────────────────
    const int minGapFrames = std::max(1, static_cast<int>(
        std::round(static_cast<double>(config_.minHandleGapMs) / 1000.0 * config_.frameRateHz)));
    if (minGapFrames > 1 && kept.size() > 1) {
        std::vector<WordSegmenterCandidate> collapsed;
        collapsed.push_back(kept.front());
        for (size_t i = 1; i < kept.size(); ++i) {
            if (kept[i].frame100fps - collapsed.back().frame100fps < minGapFrames) {
                // Merge by keeping higher priority
                if (kept[i].priority > collapsed.back().priority) {
                    collapsed.back() = kept[i];
                }
            } else {
                collapsed.push_back(kept[i]);
            }
        }
        kept = std::move(collapsed);
    }

    result.kept = kept;
    result.seedHandleFrames.reserve(kept.size());
    result.seedHandleKinds.reserve(kept.size());
    for (const auto& cand : kept) {
        result.seedHandleFrames.push_back(cand.frame100fps);
        result.seedHandleKinds.push_back(cand.kind);
    }

    return result;
}

} // namespace OpenTune
