#pragma once

#include "../Utils/SourceWindow.h"
#include "../Utils/TimeCoordinate.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <optional>
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
class MaterializationStore;
class OpenTunePlaybackRenderer : public juce::ARAPlaybackRenderer
{
public:
    using juce::ARAPlaybackRenderer::ARAPlaybackRenderer;

    struct PlaybackRegionRenderItem
    {
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        SourceWindow contentWindow;
        uint64_t materializationId{0};
        double startInPlaybackTime{0.0};
        double startInModificationTime{0.0};
        double durationInPlaybackTime{0.0};
        double durationInModificationTime{0.0};
        double materializationDurationSeconds{0.0};

        double endInPlaybackTime() const noexcept { return startInPlaybackTime + durationInPlaybackTime; }
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
    MaterializationStore* materializationStore_ = nullptr;
    std::vector<juce::ARAPlaybackRegion*> assignedPlaybackRegions_;
    std::vector<PlaybackRegionRenderItem> renderItems_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTunePlaybackRenderer)
};
#endif // JucePlugin_Enable_ARA

}
