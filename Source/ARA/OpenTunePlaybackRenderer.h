#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <optional>

namespace OpenTune {

struct RenderBlockSpan
{
    int destinationStartSample{0};
    int samplesToCopy{0};
    double overlapStartSeconds{0.0};
};

std::optional<RenderBlockSpan> computeRegionBlockRenderSpan(double blockStartSeconds,
                                                            int blockSamples,
                                                            double hostSampleRate,
                                                            double playbackStartSeconds,
                                                            double playbackEndSeconds) noexcept;

class OpenTuneDocumentController;

class OpenTunePlaybackRenderer : public juce::ARAPlaybackRenderer
{
public:
    using juce::ARAPlaybackRenderer::ARAPlaybackRenderer;
    
    ~OpenTunePlaybackRenderer() override;
    
    void prepareToPlay(double sampleRate,
                       int maximumSamplesPerBlock,
                       int numChannels,
                       juce::AudioProcessor::ProcessingPrecision precision,
                       AlwaysNonRealtime alwaysNonRealtime) override;
    
    void releaseResources() override;
    
    bool processBlock(juce::AudioBuffer<float>& buffer,
                      juce::AudioProcessor::Realtime realtime,
                      const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;
    
private:
    double hostSampleRate_ = 44100.0;
    int numChannels_ = 2;
    int maximumSamplesPerBlock_ = 512;
    juce::AudioBuffer<float> playbackScratch_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTunePlaybackRenderer)
};

}
