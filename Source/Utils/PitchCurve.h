#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <memory>
#include <atomic>
#include "F0Timeline.h"
#include "Note.h"
#include "PitchUtils.h"

namespace OpenTune {

struct PitchCorrectionSegment {
    int startFrame;
    int endFrame;
    std::vector<float> f0Data;

    enum class Source : uint8_t {
        None = 0,
        NoteBased = 1,
        HandDraw = 2,
        LineAnchor = 3
    };
    Source source = Source::None;

    float retuneSpeed = -1.0f;
    float vibratoDepth = -1.0f;
    float vibratoRate = -1.0f;
    float pitchDriftScale = 1.0f;

    PitchCorrectionSegment() = default;
    PitchCorrectionSegment(int start, int end, const std::vector<float>& data, Source src = Source::None)
        : startFrame(start), endFrame(end), f0Data(data), source(src) {}
};

class PitchCurveSnapshot {
public:
    PitchCurveSnapshot(
        std::vector<float> originalF0,
        std::vector<float> originalEnergy,
        std::vector<PitchCorrectionSegment> correctionSegments,
        int hopSize,
        double sampleRate,
        uint64_t renderGeneration = 0)
        : originalF0_(std::move(originalF0))
        , originalEnergy_(std::move(originalEnergy))
        , correctionSegments_(std::move(correctionSegments))
        , hopSize_(hopSize)
        , sampleRate_(sampleRate)
        , renderGeneration_(renderGeneration)
    {}

    const std::vector<float>& getOriginalF0() const { return originalF0_; }
    const std::vector<float>& getOriginalEnergy() const { return originalEnergy_; }
    const std::vector<PitchCorrectionSegment>& getCorrectionSegments() const { return correctionSegments_; }
    int getHopSize() const { return hopSize_; }
    double getSampleRate() const { return sampleRate_; }

    bool isEmpty() const { return originalF0_.empty(); }
    size_t size() const { return originalF0_.size(); }

    uint64_t getRenderGeneration() const { return renderGeneration_; }

    bool hasCorrectionLayer() const { return !correctionSegments_.empty(); }

    bool hasCorrectionInRange(int startFrame, int endFrame) const;

    /// Calls sink(startFrame, f0Data, length) for each correction span;
    /// calls sink(startFrame, nullptr, length) where there is no correction
    /// coverage over the span. The caller must choose a baseline (e.g.
    /// OriginalF0) for those regions.
    template <typename Sink>
    void forEachCorrectionF0Span(int startFrame, int endFrame, Sink&& sink) const {
        if (startFrame >= endFrame || startFrame < 0) return;
        const int maxFrame = static_cast<int>(originalF0_.size());
        if (endFrame > maxFrame) endFrame = maxFrame;
        if (startFrame >= maxFrame) return;

        auto it = std::lower_bound(correctionSegments_.begin(), correctionSegments_.end(), startFrame,
            [](const PitchCorrectionSegment& seg, int frame) {
                return seg.endFrame <= frame;
            });

        int currentPos = startFrame;
        while (currentPos < endFrame) {
            if (it != correctionSegments_.end() && it->startFrame < endFrame) {
                if (currentPos < it->startFrame) {
                    const int gapEnd = std::min(it->startFrame, endFrame);
                    sink(currentPos, nullptr, gapEnd - currentPos);
                    currentPos = gapEnd;
                }

                if (currentPos < it->endFrame && currentPos < maxFrame) {
                    const int segStart = std::max(currentPos, it->startFrame);
                    const int segEnd = std::min(endFrame, std::min(it->endFrame, maxFrame));
                    const int offset = segStart - it->startFrame;
                    const int length = segEnd - segStart;
                    if (length <= 0) { ++it; continue; }
                    if (static_cast<size_t>(offset + length) > it->f0Data.size()) { ++it; continue; }
                    sink(segStart, it->f0Data.data() + offset, length);
                    currentPos = segEnd;
                }
                ++it;
            } else {
                sink(currentPos, nullptr, endFrame - currentPos);
                currentPos = endFrame;
            }
        }
    }

    bool hasOriginalF0Data() const { return !originalF0_.empty(); }

private:
    const std::vector<float> originalF0_;
    const std::vector<float> originalEnergy_;
    const std::vector<PitchCorrectionSegment> correctionSegments_;
    const int hopSize_;
    const double sampleRate_;
    const uint64_t renderGeneration_;
};

class PitchCurve {
public:
    PitchCurve() : snapshot_(std::make_shared<const PitchCurveSnapshot>(
        std::vector<float>(), std::vector<float>(), std::vector<PitchCorrectionSegment>(), 512, 16000.0)) {}
    ~PitchCurve() = default;

    std::shared_ptr<const PitchCurveSnapshot> getSnapshot() const {
        return std::atomic_load(&snapshot_);
    }

    bool isEmpty() const { return getSnapshot()->isEmpty(); }
    size_t size() const { return getSnapshot()->size(); }
    int getHopSize() const { return getSnapshot()->getHopSize(); }
    double getSampleRate() const { return getSnapshot()->getSampleRate(); }
    bool hasCorrectionInRange(int startFrame, int endFrame) const {
        return getSnapshot()->hasCorrectionInRange(startFrame, endFrame);
    }
    bool hasCorrectionLayer() const { return getSnapshot()->hasCorrectionLayer(); }
    bool hasOriginalF0Data() const { return getSnapshot()->hasOriginalF0Data(); }

    void setOriginalF0(const std::vector<float>& f0) {
        auto oldSnapshot = getSnapshot();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            f0,
            oldSnapshot->getOriginalEnergy().size() != f0.size() 
                ? std::vector<float>(f0.size(), 0.0f) 
                : oldSnapshot->getOriginalEnergy(),
            oldSnapshot->getCorrectionSegments(),
            oldSnapshot->getHopSize(),
            oldSnapshot->getSampleRate(),
            oldSnapshot->getRenderGeneration()
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    void setOriginalEnergy(const std::vector<float>& energy) {
        auto oldSnapshot = getSnapshot();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            oldSnapshot->getOriginalF0(),
            energy.size() < oldSnapshot->getOriginalF0().size()
                ? [&]() {
                    auto e = energy;
                    e.resize(oldSnapshot->getOriginalF0().size(), 0.0f);
                    return e;
                }()
                : energy.size() > oldSnapshot->getOriginalF0().size()
                    ? std::vector<float>(energy.begin(), energy.begin() + oldSnapshot->getOriginalF0().size())
                    : energy,
            oldSnapshot->getCorrectionSegments(),
            oldSnapshot->getHopSize(),
            oldSnapshot->getSampleRate(),
            oldSnapshot->getRenderGeneration()
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    void setOriginalF0Range(size_t startFrame, const std::vector<float>& f0Fragment) {
        if (f0Fragment.empty()) return;
        
        auto oldSnapshot = getSnapshot();
        auto originalF0 = oldSnapshot->getOriginalF0();
        const size_t endFrame = startFrame + f0Fragment.size();
        
        if (originalF0.size() < endFrame) {
            originalF0.resize(endFrame, 0.0f);
        }
        std::copy(f0Fragment.begin(), f0Fragment.end(), originalF0.begin() + static_cast<std::ptrdiff_t>(startFrame));
        
        auto originalEnergy = oldSnapshot->getOriginalEnergy();
        if (originalEnergy.size() < originalF0.size()) {
            originalEnergy.resize(originalF0.size(), 0.0f);
        }
        
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            std::move(originalF0),
            std::move(originalEnergy),
            oldSnapshot->getCorrectionSegments(),
            oldSnapshot->getHopSize(),
            oldSnapshot->getSampleRate(),
            oldSnapshot->getRenderGeneration()
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    void setOriginalEnergyRange(size_t startFrame, const std::vector<float>& energyFragment) {
        if (energyFragment.empty()) return;
        
        auto oldSnapshot = getSnapshot();
        auto originalEnergy = oldSnapshot->getOriginalEnergy();
        const size_t endFrame = startFrame + energyFragment.size();
        
        if (originalEnergy.size() < endFrame) {
            originalEnergy.resize(endFrame, 0.0f);
        }
        std::copy(energyFragment.begin(), energyFragment.end(), originalEnergy.begin() + static_cast<std::ptrdiff_t>(startFrame));
        
        auto originalF0 = oldSnapshot->getOriginalF0();
        if (originalF0.size() < originalEnergy.size()) {
            originalF0.resize(originalEnergy.size(), 0.0f);
        }
        
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            std::move(originalF0),
            std::move(originalEnergy),
            oldSnapshot->getCorrectionSegments(),
            oldSnapshot->getHopSize(),
            oldSnapshot->getSampleRate(),
            oldSnapshot->getRenderGeneration()
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    void applyCorrectionToRange(
        const std::vector<Note>& notes,
        int startFrame,
        int endFrame,
        float sourcePitchRatio,
        float retuneSpeed,
        float vibratoDepth = 0.0f,
        float vibratoRate = 7.5f,
        float pitchDriftScale = 1.0f);

    static constexpr int getCorrectedF0BoundaryContextFrames() noexcept { return 8; }
    static F0FrameRange expandNoteBasedCorrectionRange(int startFrame, int endFrameExclusive, int frameCount) noexcept;

    void setManualCorrectionRange(int startFrame, int endFrame, const std::vector<float>& f0Data,
                                   PitchCorrectionSegment::Source source);

    void clearCorrectionRange(int startFrame, int endFrame);

    void clearAllCorrections() {
        auto oldSnapshot = getSnapshot();
        uint64_t newGen = incrementGeneration();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            oldSnapshot->getOriginalF0(),
            oldSnapshot->getOriginalEnergy(),
            std::vector<PitchCorrectionSegment>(),
            oldSnapshot->getHopSize(),
            oldSnapshot->getSampleRate(),
            newGen
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    void replaceCorrectionSegments(const std::vector<PitchCorrectionSegment>& segments) {
        auto oldSnapshot = getSnapshot();
        auto normalized = segments;
        std::sort(normalized.begin(), normalized.end(),
            [](const PitchCorrectionSegment& a, const PitchCorrectionSegment& b) {
                return a.startFrame < b.startFrame;
            });

        uint64_t newGen = incrementGeneration();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            oldSnapshot->getOriginalF0(),
            oldSnapshot->getOriginalEnergy(),
            std::move(normalized),
            oldSnapshot->getHopSize(),
            oldSnapshot->getSampleRate(),
            newGen
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    std::vector<PitchCorrectionSegment> copyCorrectionSegments() const {
        const auto snapshot = getSnapshot();
        const auto& segments = snapshot->getCorrectionSegments();
        return std::vector<PitchCorrectionSegment>(segments.begin(), segments.end());
    }

    void restoreCorrectionSegment(const PitchCorrectionSegment& segment) {
        auto oldSnapshot = getSnapshot();
        auto segments = oldSnapshot->getCorrectionSegments();
        
        segments.erase(
            std::remove_if(segments.begin(), segments.end(),
                [&](const PitchCorrectionSegment& s) {
                    return s.endFrame > segment.startFrame && s.startFrame < segment.endFrame;
                }),
            segments.end()
        );
        
        segments.push_back(segment);
        
        std::sort(segments.begin(), segments.end(),
            [](const PitchCorrectionSegment& a, const PitchCorrectionSegment& b) {
                return a.startFrame < b.startFrame;
            });
        
        uint64_t newGen = incrementGeneration();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            oldSnapshot->getOriginalF0(),
            oldSnapshot->getOriginalEnergy(),
            segments,
            oldSnapshot->getHopSize(),
            oldSnapshot->getSampleRate(),
            newGen
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    void clear() {
        uint64_t newGen = incrementGeneration();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            std::vector<float>(),
            std::vector<float>(),
            std::vector<PitchCorrectionSegment>(),
            512,
            16000.0,
            newGen
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    void setHopSize(int hopSize) {
        auto oldSnapshot = getSnapshot();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            oldSnapshot->getOriginalF0(),
            oldSnapshot->getOriginalEnergy(),
            oldSnapshot->getCorrectionSegments(),
            hopSize,
            oldSnapshot->getSampleRate(),
            oldSnapshot->getRenderGeneration()
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    void setSampleRate(double sampleRate) {
        auto oldSnapshot = getSnapshot();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            oldSnapshot->getOriginalF0(),
            oldSnapshot->getOriginalEnergy(),
            oldSnapshot->getCorrectionSegments(),
            oldSnapshot->getHopSize(),
            sampleRate,
            oldSnapshot->getRenderGeneration()
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    std::shared_ptr<PitchCurve> clone() const {
        auto snapshot = getSnapshot();
        auto copiedCurve = std::make_shared<PitchCurve>();
        copiedCurve->setHopSize(snapshot->getHopSize());
        copiedCurve->setSampleRate(snapshot->getSampleRate());
        copiedCurve->setOriginalF0(snapshot->getOriginalF0());
        copiedCurve->setOriginalEnergy(snapshot->getOriginalEnergy());

        std::vector<PitchCorrectionSegment> segments;
        segments.reserve(snapshot->getCorrectionSegments().size());
        for (const auto& segment : snapshot->getCorrectionSegments()) {
            segments.push_back(segment);
        }
        copiedCurve->replaceCorrectionSegments(segments);
        return copiedCurve;
    }

private:
    std::shared_ptr<const PitchCurveSnapshot> snapshot_;
    std::atomic<uint64_t> nextRenderGeneration_{1};

    uint64_t incrementGeneration() {
        return nextRenderGeneration_.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace OpenTune
