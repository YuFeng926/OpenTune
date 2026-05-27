#pragma once

#include "../Utils/Note.h"
#include "../Utils/TimeGrid.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

enum class ReferenceFeatureStatus : uint8_t {
    NotRequested = 0,
    Extracting = 1,
    Ready = 2,
    Failed = 3
};

enum class ReferenceFeatureProducer : uint8_t {
    Unknown = 0,
    Basic = 1,
    Game = 2
};

enum class ReferenceTimingAnchorKind : uint8_t {
    Onset = 0,
    PitchTransition = 1
};

struct ReferencePitchFeatures {
    std::vector<Note> notes;

    bool empty() const noexcept
    {
        return notes.empty();
    }

    void clear()
    {
        notes.clear();
    }
};

struct ReferenceTimingAnchor {
    uint64_t anchorId{0};
    double sourceSeconds{0.0};
    float strength{0.0f};
    ReferenceTimingAnchorKind kind{ReferenceTimingAnchorKind::Onset};
    float confidence{0.0f};
};

struct ReferenceTimingFeatures {
    std::vector<ReferenceTimingAnchor> anchors;

    bool empty() const noexcept
    {
        return anchors.empty();
    }

    void clear()
    {
        anchors.clear();
    }
};

struct ReferenceFeatureSet {
    int analysisRevision{0};
    ReferenceFeatureStatus status{ReferenceFeatureStatus::NotRequested};
    ReferenceFeatureProducer producer{ReferenceFeatureProducer::Unknown};
    ReferencePitchFeatures pitch;
    ReferenceTimingFeatures timing;
    int64_t inputFingerprint{0};
    double sourceDurationSeconds{0.0};
    juce::String errorMessage;

    void reset()
    {
        analysisRevision = 0;
        status = ReferenceFeatureStatus::NotRequested;
        producer = ReferenceFeatureProducer::Unknown;
        pitch.clear();
        timing.clear();
        inputFingerprint = 0;
        sourceDurationSeconds = 0.0;
        errorMessage.clear();
    }

    bool isReady() const noexcept
    {
        return status == ReferenceFeatureStatus::Ready;
    }

    bool hasPitchNotes() const noexcept
    {
        return !pitch.empty();
    }

    bool hasTimingAnchors(size_t minimumCount = 1) const noexcept
    {
        return timing.anchors.size() >= minimumCount;
    }
};

class EffectiveTimeMap {
public:
    static EffectiveTimeMap identity(double totalDurationSeconds) noexcept
    {
        EffectiveTimeMap map;
        map.totalDurationSeconds_ = juce::jmax(0.0, totalDurationSeconds);
        return map;
    }

    static EffectiveTimeMap fromTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot,
                                         double fallbackDurationSeconds) noexcept
    {
        EffectiveTimeMap map;
        if (snapshot != nullptr) {
            map.snapshot_ = std::move(snapshot);
            map.totalDurationSeconds_ = juce::jmax(0.0, map.snapshot_->totalDurationSeconds());
            return map;
        }

        map.totalDurationSeconds_ = juce::jmax(0.0, fallbackDurationSeconds);
        return map;
    }

    double tau(double sourceSeconds) const noexcept
    {
        if (snapshot_ != nullptr) {
            return snapshot_->tauForward(sourceSeconds);
        }

        return juce::jlimit(0.0, totalDurationSeconds_, sourceSeconds);
    }

    double tauInverse(double outputSeconds) const noexcept
    {
        if (snapshot_ != nullptr) {
            return snapshot_->tauInverse(outputSeconds);
        }

        return juce::jlimit(0.0, totalDurationSeconds_, outputSeconds);
    }

    double totalDurationSeconds() const noexcept
    {
        return totalDurationSeconds_;
    }

    bool isIdentity() const noexcept
    {
        return snapshot_ == nullptr || snapshot_->isIdentity();
    }

    bool hasExplicitTimeGrid() const noexcept
    {
        return snapshot_ != nullptr;
    }

    const std::shared_ptr<const TimeGridSnapshot>& snapshot() const noexcept
    {
        return snapshot_;
    }

private:
    std::shared_ptr<const TimeGridSnapshot> snapshot_;
    double totalDurationSeconds_{0.0};
};

} // namespace OpenTune
