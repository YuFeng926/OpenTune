#include "OpenTunePlaybackRenderer.h"

#include "OpenTuneDocumentController.h"
#include "../Utils/PlaybackAudioReader.h"

#include <algorithm>

namespace OpenTune {

bool shouldRenderAraPlaybackBlock(juce::AudioProcessor::Realtime realtime,
                                  const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    return realtime != juce::AudioProcessor::Realtime::yes || positionInfo.getIsPlaying();
}

namespace {
    double mapPlaybackTimeToContentTime(const OpenTunePlaybackRenderer::PlaybackRegionRenderItem& region,
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
                            juce::jmax(0.0, region.contentDurationSeconds),
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

OpenTunePlaybackRenderer::OpenTunePlaybackRenderer(ARA::PlugIn::DocumentController* araDc,
                                                   OpenTuneDocumentController* docController)
    : juce::ARAPlaybackRenderer(araDc)
    , documentController_(docController)
{
    std::atomic_store_explicit(&contentRenderServiceSnapshot_,
                               docController ? docController->getContentRenderServiceShared() : nullptr,
                               std::memory_order_release);
}

OpenTunePlaybackRenderer::~OpenTunePlaybackRenderer()
{
    // After owner-driven detach, destructor becomes no-op.
    // Only unregister when documentController_ is still live (relationship not revoked).
    if (documentController_ != nullptr)
        documentController_->unregisterPlaybackRenderer(*this);
}

void OpenTunePlaybackRenderer::detachDocumentController(OpenTuneDocumentController& owner)
{
    // Atomically clear documentController_ only for the matching owner.
    // Per architecture: teardown is owner-driven. Renderer cannot call into destroyed DC.
    if (documentController_ == &owner)
    {
        // 1. Publish empty render plan FIRST to stop audio thread from entering render loop
        currentPlan_.store(std::make_shared<RenderPlan>(), std::memory_order_release);
        // 2. Clear CRS snapshot atomically so audio thread sees nullptr
        std::atomic_store_explicit(&contentRenderServiceSnapshot_,
                                   std::shared_ptr<ContentRenderService>(),
                                   std::memory_order_release);
        // 3. Finally clear documentController_ pointer
        documentController_ = nullptr;
    }
}

void OpenTunePlaybackRenderer::didAddPlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    auto* jucePlaybackRegion = toJucePlaybackRegion(playbackRegion);
    auto plan = currentPlan_.load(std::memory_order_acquire);
    auto playbackRegions = plan != nullptr ? plan->playbackRegions
                                           : std::vector<juce::ARAPlaybackRegion*>{};
    if (jucePlaybackRegion != nullptr
        && std::find(playbackRegions.begin(), playbackRegions.end(), jucePlaybackRegion)
            == playbackRegions.end())
    {
        playbackRegions.push_back(jucePlaybackRegion);
    }

    publishRenderPlanFor(std::move(playbackRegions));
}

void OpenTunePlaybackRenderer::willRemovePlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    auto* jucePlaybackRegion = toJucePlaybackRegion(playbackRegion);
    auto plan = currentPlan_.load(std::memory_order_acquire);
    auto playbackRegions = plan != nullptr ? plan->playbackRegions
                                           : std::vector<juce::ARAPlaybackRegion*>{};
    playbackRegions.erase(std::remove(playbackRegions.begin(),
                                      playbackRegions.end(),
                                      jucePlaybackRegion),
                          playbackRegions.end());
    publishRenderPlanFor(std::move(playbackRegions));
}

void OpenTunePlaybackRenderer::refreshRenderPlanFromDocument()
{
    auto plan = currentPlan_.load(std::memory_order_acquire);
    publishRenderPlanFor(plan != nullptr ? plan->playbackRegions
                                         : std::vector<juce::ARAPlaybackRegion*>{});
}

void OpenTunePlaybackRenderer::setContentRenderService(std::shared_ptr<ContentRenderService> crs)
{
    std::atomic_store_explicit(&contentRenderServiceSnapshot_, crs, std::memory_order_release);
}

std::shared_ptr<const OpenTunePlaybackRenderer::RenderPlan> OpenTunePlaybackRenderer::buildRenderPlan(
    std::vector<juce::ARAPlaybackRegion*> playbackRegions) const
{
    auto nextPlan = std::make_shared<RenderPlan>();
    nextPlan->playbackRegions = std::move(playbackRegions);
    if (documentController_ == nullptr)
        return nextPlan;

    const auto projections = documentController_->getPlaybackRegionProjectionsFor(nextPlan->playbackRegions);
    nextPlan->items.reserve(projections.size());

    for (const auto& projection : projections)
    {
        if (!projection.isRenderable())
            continue;

        PlaybackRegionRenderItem item;
        item.playbackRegion = projection.playbackRegion;
        item.contentWindow = projection.contentWindow;
        item.contentKey = projection.contentKey;
        item.startInPlaybackTime = projection.startInPlaybackTime;
        item.startInModificationTime = projection.startInModificationTime;
        item.durationInPlaybackTime = projection.durationInPlaybackTime;
        item.durationInModificationTime = projection.durationInModificationTime;
        item.contentDurationSeconds = projection.contentDurationSeconds;
        nextPlan->items.push_back(item);
    }

    return nextPlan;
}

void OpenTunePlaybackRenderer::publishRenderPlanFor(std::vector<juce::ARAPlaybackRegion*> playbackRegions)
{
    currentPlan_.store(buildRenderPlan(std::move(playbackRegions)), std::memory_order_release);
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

    // Use CRS snapshot instead of chasing documentController_ pointer
    // to avoid TOCTOU race with DocumentController destruction
    auto crs = std::atomic_load_explicit(&contentRenderServiceSnapshot_, std::memory_order_acquire);
    if (crs == nullptr)
        return true;

    const auto plan = currentPlan_.load(std::memory_order_acquire);
    if (plan == nullptr || plan->items.empty())
        return true;

    const double blockStartSeconds = positionInfo.getTimeInSeconds().orFallback(0.0);

    for (const auto& region : plan->items)
    {
        const auto overlap = computeRegionBlockRenderSpan(blockStartSeconds,
                                                           buffer.getNumSamples(),
                                                           hostSampleRate_,
                                                           region.startInPlaybackTime,
                                                           region.endInPlaybackTime());
        if (!overlap.has_value())
            continue;

        PlaybackReadSource readSource;
        if (!crs->getPlaybackReadSource(region.contentKey, readSource))
            continue;

        const double readStartSeconds = mapPlaybackTimeToContentTime(region,
                                                                              overlap->overlapStartSeconds);
        const PlaybackReadRequest request(readSource,
                                           readStartSeconds,
                                           hostSampleRate_,
                                           overlap->samplesToCopy);

        playbackScratch_.clear();
        const int copied = readPlaybackAudio(request, playbackScratch_, 0);
        if (copied <= 0)
            continue;

        const int samplesToMix = juce::jmin(copied, overlap->samplesToCopy);
        mixScratchInto(buffer, playbackScratch_, overlap->destinationStartSample, samplesToMix);
    }

    return true;
}

} // namespace OpenTune
