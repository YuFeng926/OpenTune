#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <cstdint>

namespace OpenTune {

/**
 * UI/test diagnostics for the DAW timeline rendering pipeline.
 *
 * This is intentionally Standalone UI infrastructure. It is not persisted, not
 * owned by OpenTuneAudioProcessor, and should only be fed by UI paint/cache/frame
 * coordination hooks.
 */
class TimelineRenderingDiagnostics final
{
public:
    struct Snapshot
    {
        uint64_t overlayPaints = 0;
        uint64_t contentPaints = 0;
        uint64_t overlayRepaintRequests = 0;
        uint64_t contentRepaintRequests = 0;
        uint64_t renderModelRebuilds = 0;
        uint64_t exposedStripRepaintRequests = 0;
        uint64_t fullRepaintPromotions = 0;
        uint64_t tileCacheHits = 0;
        uint64_t tileCacheMisses = 0;
        uint64_t f0PathCacheHits = 0;
        uint64_t f0PathCacheMisses = 0;
        uint64_t lowPriorityAnimationRequests = 0;
        uint64_t lowPriorityAnimationDrops = 0;
        uint64_t coalescedRequests = 0;
        uint64_t dispatchedFrames = 0;
        uint64_t dirtyRectPixels = 0;
        uint64_t frameDurationSamples = 0;
        uint64_t frameDurationTotalMicros = 0;
        uint64_t frameDurationMaxMicros = 0;

        double averageFrameDurationMs() const noexcept
        {
            if (frameDurationSamples == 0)
                return 0.0;

            return static_cast<double>(frameDurationTotalMicros)
                 / static_cast<double>(frameDurationSamples)
                 / 1000.0;
        }

        double maxFrameDurationMs() const noexcept
        {
            return static_cast<double>(frameDurationMaxMicros) / 1000.0;
        }
    };

    static TimelineRenderingDiagnostics& instance()
    {
        static TimelineRenderingDiagnostics diagnostics;
        return diagnostics;
    }

    void setEnabled(bool shouldBeEnabled) noexcept
    {
        enabled_.store(shouldBeEnabled, std::memory_order_release);
    }

    bool isEnabled() const noexcept
    {
        return enabled_.load(std::memory_order_acquire);
    }

    void reset() noexcept
    {
        overlayPaints_.store(0, std::memory_order_relaxed);
        contentPaints_.store(0, std::memory_order_relaxed);
        overlayRepaintRequests_.store(0, std::memory_order_relaxed);
        contentRepaintRequests_.store(0, std::memory_order_relaxed);
        renderModelRebuilds_.store(0, std::memory_order_relaxed);
        exposedStripRepaintRequests_.store(0, std::memory_order_relaxed);
        fullRepaintPromotions_.store(0, std::memory_order_relaxed);
        tileCacheHits_.store(0, std::memory_order_relaxed);
        tileCacheMisses_.store(0, std::memory_order_relaxed);
        f0PathCacheHits_.store(0, std::memory_order_relaxed);
        f0PathCacheMisses_.store(0, std::memory_order_relaxed);
        lowPriorityAnimationRequests_.store(0, std::memory_order_relaxed);
        lowPriorityAnimationDrops_.store(0, std::memory_order_relaxed);
        coalescedRequests_.store(0, std::memory_order_relaxed);
        dispatchedFrames_.store(0, std::memory_order_relaxed);
        dirtyRectPixels_.store(0, std::memory_order_relaxed);
        frameDurationSamples_.store(0, std::memory_order_relaxed);
        frameDurationTotalMicros_.store(0, std::memory_order_relaxed);
        frameDurationMaxMicros_.store(0, std::memory_order_relaxed);
    }

    Snapshot snapshot() const noexcept
    {
        Snapshot result;
        result.overlayPaints = overlayPaints_.load(std::memory_order_relaxed);
        result.contentPaints = contentPaints_.load(std::memory_order_relaxed);
        result.overlayRepaintRequests = overlayRepaintRequests_.load(std::memory_order_relaxed);
        result.contentRepaintRequests = contentRepaintRequests_.load(std::memory_order_relaxed);
        result.renderModelRebuilds = renderModelRebuilds_.load(std::memory_order_relaxed);
        result.exposedStripRepaintRequests = exposedStripRepaintRequests_.load(std::memory_order_relaxed);
        result.fullRepaintPromotions = fullRepaintPromotions_.load(std::memory_order_relaxed);
        result.tileCacheHits = tileCacheHits_.load(std::memory_order_relaxed);
        result.tileCacheMisses = tileCacheMisses_.load(std::memory_order_relaxed);
        result.f0PathCacheHits = f0PathCacheHits_.load(std::memory_order_relaxed);
        result.f0PathCacheMisses = f0PathCacheMisses_.load(std::memory_order_relaxed);
        result.lowPriorityAnimationRequests = lowPriorityAnimationRequests_.load(std::memory_order_relaxed);
        result.lowPriorityAnimationDrops = lowPriorityAnimationDrops_.load(std::memory_order_relaxed);
        result.coalescedRequests = coalescedRequests_.load(std::memory_order_relaxed);
        result.dispatchedFrames = dispatchedFrames_.load(std::memory_order_relaxed);
        result.dirtyRectPixels = dirtyRectPixels_.load(std::memory_order_relaxed);
        result.frameDurationSamples = frameDurationSamples_.load(std::memory_order_relaxed);
        result.frameDurationTotalMicros = frameDurationTotalMicros_.load(std::memory_order_relaxed);
        result.frameDurationMaxMicros = frameDurationMaxMicros_.load(std::memory_order_relaxed);
        return result;
    }

    void recordOverlayPaint() noexcept { increment(overlayPaints_); }
    void recordContentPaint() noexcept { increment(contentPaints_); }
    void recordRenderModelRebuild() noexcept { increment(renderModelRebuilds_); }
    void recordTileCacheHit() noexcept { increment(tileCacheHits_); }
    void recordTileCacheMiss() noexcept { increment(tileCacheMisses_); }
    void recordF0PathCacheHit() noexcept { increment(f0PathCacheHits_); }
    void recordF0PathCacheMiss() noexcept { increment(f0PathCacheMisses_); }

    void recordOverlayRepaintRequest(const juce::Rectangle<int>& dirtyArea) noexcept
    {
        increment(overlayRepaintRequests_);
        addDirtyArea(dirtyArea);
    }

    void recordContentRepaintRequest(const juce::Rectangle<int>& dirtyArea) noexcept
    {
        increment(contentRepaintRequests_);
        addDirtyArea(dirtyArea);
    }

    void recordExposedStripRepaintRequest(const juce::Rectangle<int>& dirtyArea) noexcept
    {
        increment(exposedStripRepaintRequests_);
        addDirtyArea(dirtyArea);
    }

    void recordFullRepaintPromotion() noexcept { increment(fullRepaintPromotions_); }
    void recordLowPriorityAnimationRequest() noexcept { increment(lowPriorityAnimationRequests_); }
    void recordLowPriorityAnimationDrop() noexcept { increment(lowPriorityAnimationDrops_); }
    void recordCoalescedRequest() noexcept { increment(coalescedRequests_); }
    void recordDispatchedFrame() noexcept { increment(dispatchedFrames_); }

    void recordFrameDurationMs(double durationMs) noexcept
    {
        if (!isEnabled() || durationMs < 0.0)
            return;

        const auto micros = static_cast<uint64_t>(durationMs * 1000.0 + 0.5);
        frameDurationSamples_.fetch_add(1, std::memory_order_relaxed);
        frameDurationTotalMicros_.fetch_add(micros, std::memory_order_relaxed);
        updateMax(frameDurationMaxMicros_, micros);
    }

private:
    TimelineRenderingDiagnostics() = default;

    static uint64_t dirtyAreaPixels(const juce::Rectangle<int>& dirtyArea) noexcept
    {
        if (dirtyArea.isEmpty())
            return 0;

        return static_cast<uint64_t>(dirtyArea.getWidth())
             * static_cast<uint64_t>(dirtyArea.getHeight());
    }

    void increment(std::atomic<uint64_t>& counter, uint64_t amount = 1) noexcept
    {
        if (isEnabled())
            counter.fetch_add(amount, std::memory_order_relaxed);
    }

    void addDirtyArea(const juce::Rectangle<int>& dirtyArea) noexcept
    {
        increment(dirtyRectPixels_, dirtyAreaPixels(dirtyArea));
    }

    static void updateMax(std::atomic<uint64_t>& target, uint64_t value) noexcept
    {
        auto current = target.load(std::memory_order_relaxed);
        while (current < value
               && !target.compare_exchange_weak(current,
                                                value,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed))
        {
        }
    }

    std::atomic<bool> enabled_ { false };
    std::atomic<uint64_t> overlayPaints_ { 0 };
    std::atomic<uint64_t> contentPaints_ { 0 };
    std::atomic<uint64_t> overlayRepaintRequests_ { 0 };
    std::atomic<uint64_t> contentRepaintRequests_ { 0 };
    std::atomic<uint64_t> renderModelRebuilds_ { 0 };
    std::atomic<uint64_t> exposedStripRepaintRequests_ { 0 };
    std::atomic<uint64_t> fullRepaintPromotions_ { 0 };
    std::atomic<uint64_t> tileCacheHits_ { 0 };
    std::atomic<uint64_t> tileCacheMisses_ { 0 };
    std::atomic<uint64_t> f0PathCacheHits_ { 0 };
    std::atomic<uint64_t> f0PathCacheMisses_ { 0 };
    std::atomic<uint64_t> lowPriorityAnimationRequests_ { 0 };
    std::atomic<uint64_t> lowPriorityAnimationDrops_ { 0 };
    std::atomic<uint64_t> coalescedRequests_ { 0 };
    std::atomic<uint64_t> dispatchedFrames_ { 0 };
    std::atomic<uint64_t> dirtyRectPixels_ { 0 };
    std::atomic<uint64_t> frameDurationSamples_ { 0 };
    std::atomic<uint64_t> frameDurationTotalMicros_ { 0 };
    std::atomic<uint64_t> frameDurationMaxMicros_ { 0 };
};

} // namespace OpenTune
