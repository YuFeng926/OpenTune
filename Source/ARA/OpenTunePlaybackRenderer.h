#pragma once

#include "../Utils/SourceWindow.h"
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

#if JucePlugin_Enable_ARA
bool shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime realtime,
                                  const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept;

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
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        SourceWindow contentWindow;
        ContentKey contentKey;
        double startInPlaybackTime{0.0};
        double startInModificationTime{0.0};
        double durationInPlaybackTime{0.0};
        double durationInModificationTime{0.0};
        double contentDurationSeconds{0.0};

        double endInPlaybackTime() const noexcept { return startInPlaybackTime + durationInPlaybackTime; }
    };

    struct RenderPlan
    {
        std::vector<juce::ARAPlaybackRegion*> playbackRegions;
        std::vector<PlaybackRegionRenderItem> items;
    };
    
    ~OpenTunePlaybackRenderer() override;

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
    double hostSampleRate_ = 44100.0;
    int numChannels_ = 2;
    int maximumSamplesPerBlock_ = 512;
    juce::AudioBuffer<float> playbackScratch_;
    OpenTuneDocumentController* documentController_ = nullptr;

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
