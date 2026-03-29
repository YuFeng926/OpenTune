#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <vector>
#include <memory>
#include <atomic>
#include "Note.h"
#include "PitchUtils.h"

namespace OpenTune {

struct CorrectedSegment {
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

    CorrectedSegment() = default;
    CorrectedSegment(int start, int end, const std::vector<float>& data, Source src = Source::None)
        : startFrame(start), endFrame(end), f0Data(data), source(src) {}
};

class PitchCurveSnapshot {
public:
    PitchCurveSnapshot(
        std::vector<float> originalF0,
        std::vector<float> originalEnergy,
        std::vector<CorrectedSegment> correctedSegments,
        int hopSize,
        double sampleRate,
        uint64_t renderGeneration = 0)
        : originalF0_(std::move(originalF0))
        , originalEnergy_(std::move(originalEnergy))
        , correctedSegments_(std::move(correctedSegments))
        , hopSize_(hopSize)
        , sampleRate_(sampleRate)
        , renderGeneration_(renderGeneration)
    {}

    const std::vector<float>& getOriginalF0() const { return originalF0_; }
    const std::vector<float>& getOriginalEnergy() const { return originalEnergy_; }
    const std::vector<CorrectedSegment>& getCorrectedSegments() const { return correctedSegments_; }
    int getHopSize() const { return hopSize_; }
    double getSampleRate() const { return sampleRate_; }

    bool isEmpty() const { return originalF0_.empty(); }
    size_t size() const { return originalF0_.size(); }

    uint64_t getRenderGeneration() const { return renderGeneration_; }

    bool hasAnyCorrection() const { return !correctedSegments_.empty(); }

    bool hasCorrectionInRange(int startFrame, int endFrame) const;

    void renderF0Range(int startFrame, int endFrame,
                       std::function<void(int, const float*, int)> callback) const;

    void renderCorrectedOnlyRange(int startFrame, int endFrame,
                                  std::function<void(int, const float*, int)> callback) const;

    bool hasAnyCorrectedVisible() const { return !correctedSegments_.empty(); }

    bool shouldDrawCorrectedF0(bool& outUseMask) const {
        outUseMask = false;
        return !correctedSegments_.empty();
    }

    bool hasRenderableCorrectedF0() const { return !correctedSegments_.empty(); }

    bool hasCorrectedVisibleInRange(double startSeconds, double endSeconds) const;

    bool getCorrectedVisibleTimeBounds(double& outStartSeconds, double& outEndSeconds) const;

    bool getCorrectedVisibleOverlapInRange(double startSeconds, double endSeconds,
                                            double& outOverlapStart, double& outOverlapEnd) const;

    size_t getMemoryUsage() const {
        size_t total = 0;
        total += originalF0_.capacity() * sizeof(float);
        total += originalEnergy_.capacity() * sizeof(float);
        for (const auto& seg : correctedSegments_) {
            total += sizeof(CorrectedSegment);
            total += seg.f0Data.capacity() * sizeof(float);
        }
        return total;
    }

private:
    const std::vector<float> originalF0_;
    const std::vector<float> originalEnergy_;
    const std::vector<CorrectedSegment> correctedSegments_;
    const int hopSize_;
    const double sampleRate_;
    const uint64_t renderGeneration_;
};

class PitchCurve {
public:
    PitchCurve() : snapshot_(std::make_shared<const PitchCurveSnapshot>(
        std::vector<float>(), std::vector<float>(), std::vector<CorrectedSegment>(), 512, 16000.0)) {}
    ~PitchCurve() = default;

    std::shared_ptr<const PitchCurveSnapshot> getSnapshot() const {
        return std::atomic_load(&snapshot_);
    }

    // Convenience forwarding methods (delegate to snapshot)
    // Note: These methods call getSnapshot() once per call, so for consistency
    // across multiple fields, callers should capture a snapshot explicitly:
    //   auto snap = curve->getSnapshot();
    //   auto& f0 = snap->getOriginalF0();
    //   auto hop = snap->getHopSize();
    bool isEmpty() const { return getSnapshot()->isEmpty(); }
    size_t size() const { return getSnapshot()->size(); }
    int getHopSize() const { return getSnapshot()->getHopSize(); }
    double getSampleRate() const { return getSnapshot()->getSampleRate(); }
    bool hasCorrectionInRange(int startFrame, int endFrame) const {
        return getSnapshot()->hasCorrectionInRange(startFrame, endFrame);
    }
    bool hasAnyCorrection() const { return getSnapshot()->hasAnyCorrection(); }
    bool hasRenderableCorrectedF0() const { return getSnapshot()->hasRenderableCorrectedF0(); }
    bool hasCorrectedVisibleInRange(double startSeconds, double endSeconds) const {
        return getSnapshot()->hasCorrectedVisibleInRange(startSeconds, endSeconds);
    }
    bool getCorrectedVisibleTimeBounds(double& outStartSeconds, double& outEndSeconds) const {
        return getSnapshot()->getCorrectedVisibleTimeBounds(outStartSeconds, outEndSeconds);
    }
    bool shouldDrawCorrectedF0(bool& outUseMask) const {
        return getSnapshot()->shouldDrawCorrectedF0(outUseMask);
    }
    bool getCorrectedVisibleOverlapInRange(double startSeconds, double endSeconds,
                                            double& outOverlapStart, double& outOverlapEnd) const {
        return getSnapshot()->getCorrectedVisibleOverlapInRange(startSeconds, endSeconds, outOverlapStart, outOverlapEnd);
    }
    size_t getMemoryUsage() const {
        return getSnapshot()->getMemoryUsage();
    }
    void renderCorrectedOnlyRange(int startFrame, int endFrame,
                                  std::function<void(int, const float*, int)> callback) const {
        getSnapshot()->renderCorrectedOnlyRange(startFrame, endFrame, callback);
    }
    void renderF0Range(int startFrame, int endFrame,
                       std::function<void(int, const float*, int)> callback) const {
        getSnapshot()->renderF0Range(startFrame, endFrame, callback);
    }

    void setOriginalF0(const std::vector<float>& f0) {
        auto oldSnapshot = getSnapshot();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            f0,
            oldSnapshot->getOriginalEnergy().size() != f0.size() 
                ? std::vector<float>(f0.size(), 0.0f) 
                : oldSnapshot->getOriginalEnergy(),
            oldSnapshot->getCorrectedSegments(),
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
            oldSnapshot->getCorrectedSegments(),
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
            oldSnapshot->getCorrectedSegments(),
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
            oldSnapshot->getCorrectedSegments(),
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
        float retuneSpeed,
        float vibratoDepth = 0.0f,
        float vibratoRate = 7.5f,
        double audioSampleRate = 44100.0);

    void setManualCorrectionRange(int startFrame, int endFrame, const std::vector<float>& f0Data,
                                   CorrectedSegment::Source source);

    void setManualCorrectionRange(int startFrame, int endFrame, const std::vector<float>& f0Data,
                                   CorrectedSegment::Source source, float retuneSpeed);

    void clearCorrectionRange(int startFrame, int endFrame);

    void clearAllCorrections() {
        auto oldSnapshot = getSnapshot();
        uint64_t newGen = incrementGeneration();
        auto newSnapshot = std::make_shared<const PitchCurveSnapshot>(
            oldSnapshot->getOriginalF0(),
            oldSnapshot->getOriginalEnergy(),
            std::vector<CorrectedSegment>(),
            oldSnapshot->getHopSize(),
            oldSnapshot->getSampleRate(),
            newGen
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

    void restoreCorrectedSegment(const CorrectedSegment& segment) {
        auto oldSnapshot = getSnapshot();
        auto segments = oldSnapshot->getCorrectedSegments();
        
        segments.erase(
            std::remove_if(segments.begin(), segments.end(),
                [&](const CorrectedSegment& s) {
                    return s.endFrame > segment.startFrame && s.startFrame < segment.endFrame;
                }),
            segments.end()
        );
        
        segments.push_back(segment);
        
        std::sort(segments.begin(), segments.end(),
            [](const CorrectedSegment& a, const CorrectedSegment& b) {
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
            std::vector<CorrectedSegment>(),
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
            oldSnapshot->getCorrectedSegments(),
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
            oldSnapshot->getCorrectedSegments(),
            oldSnapshot->getHopSize(),
            sampleRate,
            oldSnapshot->getRenderGeneration()
        );
        std::atomic_store(&snapshot_, newSnapshot);
    }

private:
    std::shared_ptr<const PitchCurveSnapshot> snapshot_;
    std::atomic<uint64_t> nextRenderGeneration_{1};

    uint64_t incrementGeneration() {
        return nextRenderGeneration_.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace OpenTune
