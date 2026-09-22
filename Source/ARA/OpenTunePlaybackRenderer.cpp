#include "OpenTunePlaybackRenderer.h"

#include "OpenTuneDocumentController.h"
#include "../Utils/PlaybackAudioReader.h"

#include <algorithm>

namespace OpenTune {

namespace {
    double mapPlaybackTimeToSourceLocalTime(const OpenTunePlaybackRenderer::PlaybackRegionRenderItem& region,
                                            const PlaybackReadSource& source,
                                            double playbackTimeSeconds) noexcept
    {
        const auto& snapshot = *source.contentSnapshot;
        return region.startInModificationTime
            + (playbackTimeSeconds - region.startInPlaybackTime)
            - snapshot.sourceWindow.sourceStartSeconds;
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
        if (!projection.isPlaybackRenderable())
            continue;

        const auto* contentRenderService = documentController_->getContentRenderService();
        PlaybackReadSource readSource;
        if (contentRenderService == nullptr
            || !contentRenderService->getPlaybackReadSource(projection.contentKey, readSource))
            continue;

        PlaybackRegionRenderItem item;
        item.contentKey = projection.contentKey;
        item.startInPlaybackTime = projection.startInPlaybackTime;
        item.startInModificationTime = projection.startInModificationTime;
        item.durationInPlaybackTime = projection.durationInPlaybackTime;
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

    // 设备率切换 → 准备所有 CRS caches（各 cache 使用自有 resampler）
    if (auto crs = std::atomic_load_explicit(&contentRenderServiceSnapshot_, std::memory_order_acquire))
        crs->preparePlaybackSampleRate(sampleRate);
}

void OpenTunePlaybackRenderer::releaseResources()
{
    playbackScratch_.setSize(0, 0);
}

bool OpenTunePlaybackRenderer::processBlock(juce::AudioBuffer<float>& buffer,
                                             juce::AudioProcessor::Realtime /*realtime*/,
                                             const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    auto crs = std::atomic_load_explicit(&contentRenderServiceSnapshot_, std::memory_order_acquire);
    const auto plan = currentPlan_.load(std::memory_order_acquire);
    const auto positionTime = positionInfo.getTimeInSeconds();

    if (crs == nullptr || plan == nullptr || plan->items.empty() || !positionTime.hasValue())
        return true;

    const double blockStartSeconds = *positionTime;
    for (const auto& region : plan->items)
    {
        const auto overlap = computeRegionBlockRenderSpan(blockStartSeconds,
                                                           numSamples,
                                                           hostSampleRate_,
                                                           region.startInPlaybackTime,
                                                           region.endInPlaybackTime());
        if (!overlap.has_value())
            continue;

        PlaybackReadSource readSource;
        if (!crs->getPlaybackReadSource(region.contentKey, readSource))
            continue;

        const double readStartSeconds = mapPlaybackTimeToSourceLocalTime(
            region, readSource, overlap->overlapStartSeconds);
        const int64_t readStartSample = TimeCoordinate::secondsToSamples(
            readStartSeconds, hostSampleRate_);
        const PlaybackReadRequest request(readSource,
                                          readStartSample,
                                          hostSampleRate_,
                                          overlap->samplesToCopy);

        playbackScratch_.clear();
        const int copied = readPlaybackAudio(request, playbackScratch_, 0);
        const int samplesToCopy = juce::jmin(copied, overlap->samplesToCopy);
        if (samplesToCopy <= 0)
            continue;

        const int channels = juce::jmin(buffer.getNumChannels(), playbackScratch_.getNumChannels());
        for (int channel = 0; channel < channels; ++channel)
            buffer.addFrom(channel,
                           overlap->destinationStartSample,
                           playbackScratch_,
                           channel,
                           0,
                           samplesToCopy);
    }

    return true;
}

} // namespace OpenTune
