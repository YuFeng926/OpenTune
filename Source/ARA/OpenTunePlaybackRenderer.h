#pragma once

#include "../Utils/TimeCoordinate.h"
#include "../Content/ContentKey.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace OpenTune {

struct RenderBlockSpan
{
    int destinationStartSample{0};
    int samplesToCopy{0};
    double overlapStartSeconds{0.0};
};

// JUCE ARA playback semantics: realtime calls render only while the host is
// playing; offline/non-realtime calls may still render while stopped.
inline bool shouldRenderPlaybackBlock(juce::AudioProcessor::Realtime realtime,
                                      bool isPlaying) noexcept
{
    return realtime != juce::AudioProcessor::Realtime::yes || isPlaying;
}

inline std::optional<RenderBlockSpan> computeRegionBlockRenderSpan(double blockStartSeconds,
                                                                    int blockSamples,
                                                                    double hostSampleRate,
                                                                    double playbackStartSeconds,
                                                                    double playbackEndSeconds) noexcept
{
    if (blockSamples <= 0 || hostSampleRate <= 0.0)
        return std::nullopt;

    const double blockEndSeconds = blockStartSeconds
        + (static_cast<double>(blockSamples) / hostSampleRate);
    const double overlapStartSeconds = juce::jmax(blockStartSeconds, playbackStartSeconds);
    const double overlapEndSeconds = juce::jmin(blockEndSeconds, playbackEndSeconds);
    if (!(overlapEndSeconds > overlapStartSeconds))
        return std::nullopt;

    const int destinationStartSample = juce::jlimit(0,
                                                     blockSamples,
                                                     static_cast<int>(TimeCoordinate::secondsToSamplesFloor(overlapStartSeconds - blockStartSeconds,
                                                                                                            hostSampleRate)));
    const int destinationEndSample = juce::jlimit(destinationStartSample,
                                                   blockSamples,
                                                   static_cast<int>(TimeCoordinate::secondsToSamplesCeil(overlapEndSeconds - blockStartSeconds,
                                                                                                         hostSampleRate)));

    RenderBlockSpan span;
    span.destinationStartSample = destinationStartSample;
    span.samplesToCopy = destinationEndSample - destinationStartSample;
    span.overlapStartSeconds = overlapStartSeconds;
    return span.samplesToCopy > 0 ? std::optional<RenderBlockSpan>(span) : std::nullopt;
}

inline std::optional<RenderBlockSpan> computeRegionBlockRenderSpanFromSamplePosition(
    int64_t blockStartSample,
    int blockSamples,
    double hostSampleRate,
    double playbackStartSeconds,
    double playbackEndSeconds) noexcept
{
    if (blockSamples <= 0 || hostSampleRate <= 0.0)
        return std::nullopt;

    // Convert region boundaries once into the host sample domain. The block
    // anchor is already in that domain, so no sample -> seconds -> sample
    // round-trip is needed for the overlap or destination bounds.
    const int64_t playbackStartSample = TimeCoordinate::secondsToSamplesFloor(
        playbackStartSeconds, hostSampleRate);
    const int64_t playbackEndSample = TimeCoordinate::secondsToSamplesCeil(
        playbackEndSeconds, hostSampleRate);
    const int64_t blockEndSample = blockStartSample + static_cast<int64_t>(blockSamples);
    const int64_t overlapStartSample = juce::jmax(blockStartSample, playbackStartSample);
    const int64_t overlapEndSample = juce::jmin(blockEndSample, playbackEndSample);
    if (!(overlapEndSample > overlapStartSample))
        return std::nullopt;

    RenderBlockSpan span;
    span.destinationStartSample = static_cast<int>(overlapStartSample - blockStartSample);
    span.samplesToCopy = static_cast<int>(overlapEndSample - overlapStartSample);
    span.overlapStartSeconds = TimeCoordinate::samplesToSeconds(overlapStartSample,
                                                                hostSampleRate);
    return span.samplesToCopy > 0 ? std::optional<RenderBlockSpan>(span) : std::nullopt;
}

#if JucePlugin_Enable_ARA
class OpenTuneDocumentController;
class ContentRenderService;
class OpenTunePlaybackRenderer : public juce::ARAPlaybackRenderer
{
public:
    OpenTunePlaybackRenderer(ARA::PlugIn::DocumentController* araDc,
                             OpenTuneDocumentController* docController);
    using juce::ARAPlaybackRenderer::ARAPlaybackRenderer;

    struct PlaybackRegionRenderItem
    {
        ContentKey contentKey;
        double startInPlaybackTime{0.0};
        double startInModificationTime{0.0};
        double durationInPlaybackTime{0.0};

        double endInPlaybackTime() const noexcept { return startInPlaybackTime + durationInPlaybackTime; }
    };

    struct RenderPlan
    {
        std::vector<juce::ARAPlaybackRegion*> playbackRegions;
        std::vector<PlaybackRegionRenderItem> items;
    };
    
~OpenTunePlaybackRenderer() override;

    // Owner-driven detach: clears documentController_ for matching owner and publishes empty render plan.
    // Called by OpenTuneDocumentController before clearing playbackRenderers_ in destructor.
    void detachDocumentController(OpenTuneDocumentController& owner);

    void refreshRenderPlanFromDocument();
    
    void prepareToPlay(double sampleRate,
                       int maximumSamplesPerBlock,
                       int numChannels,
                       juce::AudioProcessor::ProcessingPrecision precision,
                       AlwaysNonRealtime alwaysNonRealtime) override;
    
    void releaseResources() override;
    
    bool processBlock(juce::AudioBuffer<float>& buffer,
                      juce::AudioProcessor::Realtime realtime,
                      const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;

protected:
    void didAddPlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;
    void willRemovePlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;
    
private:
    double hostSampleRate_ = 0.0;
    int numChannels_ = 2;
    int maximumSamplesPerBlock_ = 512;
    juce::AudioBuffer<float> playbackScratch_;

    OpenTuneDocumentController* documentController_ = nullptr;
    std::shared_ptr<ContentRenderService> contentRenderServiceSnapshot_;

    struct AtomicRenderPlan
    {
        std::shared_ptr<const RenderPlan> value{std::make_shared<RenderPlan>()};

        std::shared_ptr<const RenderPlan> load(std::memory_order order = std::memory_order_acquire) const noexcept
        {
            return std::atomic_load_explicit(&value, order);
        }

        void store(std::shared_ptr<const RenderPlan> next,
                   std::memory_order order = std::memory_order_release) noexcept
        {
            std::atomic_store_explicit(&value, std::move(next), order);
        }
    };

    AtomicRenderPlan currentPlan_;

    std::shared_ptr<const RenderPlan> buildRenderPlan(
        std::vector<juce::ARAPlaybackRegion*> playbackRegions) const;
    void publishRenderPlanFor(std::vector<juce::ARAPlaybackRegion*> playbackRegions);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTunePlaybackRenderer)
};
#endif // JucePlugin_Enable_ARA

}
