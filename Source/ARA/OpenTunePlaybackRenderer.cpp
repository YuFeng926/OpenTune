#include "OpenTunePlaybackRenderer.h"

#include "OpenTuneDocumentController.h"
#include "PluginProcessor.h"

#include <algorithm>

namespace OpenTune {

bool shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime realtime,
                                  const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    return realtime != juce::AudioProcessor::Realtime::yes || positionInfo.getIsPlaying();
}

namespace {
    double mapPlaybackTimeToMaterializationTime(const OpenTunePlaybackRenderer::PlaybackRegionRenderItem& region,
                                                double playbackTimeSeconds) noexcept
    {
        if (region.durationInPlaybackTime <= 0.0 || region.durationInModificationTime <= 0.0)
            return 0.0;

        const double playbackOffset = playbackTimeSeconds - region.startInPlaybackTime;
        const double modificationOffset = playbackOffset
            * (region.durationInModificationTime / region.durationInPlaybackTime);
        const double modificationTime = region.startInModificationTime + modificationOffset;
        const double contentOffset = modificationTime - region.contentWindow.sourceStartSeconds;

        return juce::jlimit(0.0,
                            juce::jmax(0.0, region.materializationDurationSeconds),
                            contentOffset);
    }

    void mixScratchInto(juce::AudioBuffer<float>& destination,
                        const juce::AudioBuffer<float>& source,
                        int destinationStartSample,
                        int samplesToMix) noexcept
    {
        const int channels = juce::jmin(destination.getNumChannels(), source.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dest = destination.getWritePointer(ch, destinationStartSample);
            const auto* src = source.getReadPointer(ch);
            for (int sample = 0; sample < samplesToMix; ++sample)
                dest[sample] += src[sample];
        }
    }

    juce::ARAPlaybackRegion* toJucePlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
    {
        return static_cast<juce::ARAPlaybackRegion*>(playbackRegion);
    }
}

OpenTunePlaybackRenderer::~OpenTunePlaybackRenderer()
{
    if (auto* dc = getDocumentController())
        if (auto* docController =
                juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<OpenTuneDocumentController>(dc))
            docController->unregisterPlaybackRenderer(*this);
}

void OpenTunePlaybackRenderer::didAddPlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    auto* jucePlaybackRegion = toJucePlaybackRegion(playbackRegion);
    if (jucePlaybackRegion != nullptr
        && std::find(assignedPlaybackRegions_.begin(), assignedPlaybackRegions_.end(), jucePlaybackRegion)
            == assignedPlaybackRegions_.end())
    {
        assignedPlaybackRegions_.push_back(jucePlaybackRegion);
    }

    refreshRenderPlanFromDocument();
}

void OpenTunePlaybackRenderer::willRemovePlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    auto* jucePlaybackRegion = toJucePlaybackRegion(playbackRegion);
    assignedPlaybackRegions_.erase(std::remove(assignedPlaybackRegions_.begin(),
                                               assignedPlaybackRegions_.end(),
                                               jucePlaybackRegion),
                                   assignedPlaybackRegions_.end());
    renderItems_.erase(std::remove_if(renderItems_.begin(), renderItems_.end(),
                                      [jucePlaybackRegion](const PlaybackRegionRenderItem& item)
                                      {
                                          return item.playbackRegion == jucePlaybackRegion;
                                      }),
                       renderItems_.end());
}

void OpenTunePlaybackRenderer::refreshRenderPlanFromDocument()
{
    auto* dc = getDocumentController();
    if (dc == nullptr)
    {
        processor_ = nullptr;
        renderItems_.clear();
        return;
    }

    auto* docController =
        juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<OpenTuneDocumentController>(dc);
    if (docController == nullptr)
    {
        processor_ = nullptr;
        renderItems_.clear();
        return;
    }

    processor_ = docController->getProcessor();
    const auto projections = docController->getPlaybackRegionProjectionsFor(assignedPlaybackRegions_);
    std::vector<PlaybackRegionRenderItem> nextItems;
    nextItems.reserve(projections.size());

    for (const auto& projection : projections)
    {
        if (!projection.isRenderable())
            continue;

        PlaybackRegionRenderItem item;
        item.playbackRegion = projection.playbackRegion;
        item.contentWindow = projection.contentWindow;
        item.materializationId = projection.materializationId;
        item.startInPlaybackTime = projection.startInPlaybackTime;
        item.startInModificationTime = projection.startInModificationTime;
        item.durationInPlaybackTime = projection.durationInPlaybackTime;
        item.durationInModificationTime = projection.durationInModificationTime;
        item.materializationDurationSeconds = projection.materializationDurationSeconds;
        nextItems.push_back(item);
    }

    renderItems_ = std::move(nextItems);
}

void OpenTunePlaybackRenderer::prepareToPlay(double sampleRate,
                                             int maximumSamplesPerBlock,
                                             int numChannels,
                                             juce::AudioProcessor::ProcessingPrecision precision,
                                             AlwaysNonRealtime alwaysNonRealtime)
{
    juce::ignoreUnused(precision, alwaysNonRealtime);

    hostSampleRate_ = sampleRate;
    numChannels_ = numChannels;
    maximumSamplesPerBlock_ = maximumSamplesPerBlock;
    playbackScratch_.setSize(juce::jmax(1, numChannels_),
                             juce::jmax(1, maximumSamplesPerBlock_),
                             false,
                             true,
                             true);

}

void OpenTunePlaybackRenderer::releaseResources()
{
    playbackScratch_.setSize(0, 0);
}

bool OpenTunePlaybackRenderer::processBlock(juce::AudioBuffer<float>& buffer,
                                            juce::AudioProcessor::Realtime realtime,
                                            const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    if (!shouldRenderAraPlaybackBlock(realtime, positionInfo))
    {
        buffer.clear();
        return true;
    }

    buffer.clear();

    auto* processor = processor_;
    if (processor == nullptr || renderItems_.empty())
        return true;

    const double blockStartSeconds = positionInfo.getTimeInSeconds().orFallback(0.0);

    for (const auto& region : renderItems_)
    {
        const auto overlap = computeRegionBlockRenderSpan(blockStartSeconds,
                                                          buffer.getNumSamples(),
                                                          hostSampleRate_,
                                                          region.startInPlaybackTime,
                                                          region.endInPlaybackTime());
        if (!overlap.has_value())
            continue;

        OpenTuneAudioProcessor::PlaybackReadSource readSource;
        if (!processor->getPlaybackReadSourceByMaterializationId(region.materializationId, readSource))
            continue;

        const double readStartSeconds = mapPlaybackTimeToMaterializationTime(region,
                                                                             overlap->overlapStartSeconds);
        const OpenTuneAudioProcessor::PlaybackReadRequest request(readSource,
                                                                  readStartSeconds,
                                                                  hostSampleRate_,
                                                                  overlap->samplesToCopy);

        playbackScratch_.clear();
        const int copied = processor->readPlaybackAudio(request, playbackScratch_, 0);
        if (copied <= 0)
            continue;

        const int samplesToMix = juce::jmin(copied, overlap->samplesToCopy);
        mixScratchInto(buffer, playbackScratch_, overlap->destinationStartSample, samplesToMix);
    }

    return true;
}

} // namespace OpenTune
