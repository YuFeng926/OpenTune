#include "OpenTunePlaybackRenderer.h"

#include "OpenTuneDocumentController.h"
#include "../Audio/NotePitchAudition.h"
#include "../Render/ContentRenderService.h"
#include "../Utils/PlaybackAudioReader.h"
#include "../Utils/PlaybackSampleGeometry.h"

#include <algorithm>

namespace OpenTune {

namespace {

bool sameContinuityProvenance(const PlaybackReadSource& lhs,
                              const PlaybackReadSource& rhs) noexcept
{
    // Full native identity: buffer, sampling rate, source grid origin, revision.
    if (!(lhs.nativeSource == rhs.nativeSource))
        return false;

    if (lhs.contentSnapshot == nullptr || rhs.contentSnapshot == nullptr
        || lhs.contentSnapshot->timeGrid == nullptr || rhs.contentSnapshot->timeGrid == nullptr)
        return false;

    const bool lhsIdentity = lhs.contentSnapshot->timeGrid->isIdentity();
    const bool rhsIdentity = rhs.contentSnapshot->timeGrid->isIdentity();
    if (lhsIdentity != rhsIdentity)
        return false;

    // Identity content maps source time directly, so the same native PCM is
    // enough for slices from different modifications to share their fixed D.
    if (lhsIdentity)
        return true;

    // Non-identity Stage2 output grids are content-local. Only one output
    // domain may share a mapping; different content origins stay independent.
    const auto& lhsWindow = lhs.contentSnapshot->sourceWindow;
    const auto& rhsWindow = rhs.contentSnapshot->sourceWindow;
    return lhs.contentKey == rhs.contentKey
        && lhs.contentSnapshot->timeGrid == rhs.contentSnapshot->timeGrid
        && lhsWindow.sourceStartSeconds == rhsWindow.sourceStartSeconds
        && lhsWindow.sourceEndSeconds == rhsWindow.sourceEndSeconds;
}

bool sourcesPreparedAtRate(const std::vector<OpenTunePlaybackRenderer::PlaybackRegionRenderItem>& items,
                           double sampleRate) noexcept
{
    for (const auto& item : items)
        if (item.source.preparedDry.sampleRate != sampleRate)
            return false;
    return true;
}

juce::ARAPlaybackRegion* toJucePlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    return static_cast<juce::ARAPlaybackRegion*>(playbackRegion);
}

struct RegionAnchors
{
    double placementAnchorSeconds{0.0};
    double semanticAnchorSeconds{0.0};
};

RegionAnchors makeRegionAnchors(const PlaybackReadSource& source,
                                double startInModificationTime) noexcept
{
    const auto& snapshot = *source.contentSnapshot;
    const double localSourceSeconds =
        startInModificationTime - snapshot.sourceWindow.sourceStartSeconds;
    if (snapshot.timeGrid->isIdentity())
        return { startInModificationTime, localSourceSeconds };

    // Non-identity content: both the fixed mapping and the automation anchor
    // live in the Stage2 output domain, relative to the content origin.
    const double outputSeconds = snapshot.timeGrid->tauForward(localSourceSeconds);
    return { outputSeconds, outputSeconds };
}

} // namespace

OpenTunePlaybackRenderer::OpenTunePlaybackRenderer(ARA::PlugIn::DocumentController* araDc,
                                                   OpenTuneDocumentController* docController)
    : juce::ARAPlaybackRenderer(araDc)
    , documentController_(docController)
{
    std::atomic_store_explicit(&contentRenderServiceSnapshot_,
                                docController ? docController->getContentRenderServiceShared() : nullptr,
                                std::memory_order_release);
    if (docController != nullptr)
        notePitchAudition_.store(&docController->getNotePitchAudition(),
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
        // 1. Clear audition pointer FIRST so audio thread stops querying it
        notePitchAudition_.store(nullptr, std::memory_order_release);
        // 2. Publish empty render plan to stop audio thread from entering render loop
        currentPlan_.store(std::make_shared<RenderPlan>(), std::memory_order_release);
        // 3. Clear CRS snapshot atomically so audio thread sees nullptr
        std::atomic_store_explicit(&contentRenderServiceSnapshot_,
                                   std::shared_ptr<ContentRenderService>(),
                                   std::memory_order_release);
        // 4. Finally clear documentController_ pointer
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

std::shared_ptr<OpenTunePlaybackRenderer::RenderPlan> OpenTunePlaybackRenderer::buildRenderPlan(
    std::vector<juce::ARAPlaybackRegion*> playbackRegions,
    double targetSampleRate) const
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    auto nextPlan = std::make_shared<RenderPlan>();
    nextPlan->playbackRegions = std::move(playbackRegions);
    nextPlan->targetSampleRate = targetSampleRate;
    if (documentController_ == nullptr)
        return nextPlan;

    auto crs = std::atomic_load_explicit(&contentRenderServiceSnapshot_,
                                         std::memory_order_acquire);
    const auto projections = documentController_->getPlaybackRegionProjectionsFor(nextPlan->playbackRegions);
    nextPlan->items.reserve(projections.size());

    for (const auto& projection : projections)
    {
        if (!projection.isPlaybackRenderable())
            continue;

        PlaybackReadSource readSource;
        if (crs == nullptr || !crs->getPlaybackReadSource(projection.contentKey, readSource))
            continue;
        if (readSource.contentSnapshot == nullptr || readSource.contentSnapshot->timeGrid == nullptr)
            continue;

        PlaybackRegionRenderItem item;
        item.playbackRegion = projection.playbackRegion;
        item.contentKey = projection.contentKey;
        item.startInPlaybackTime = projection.startInPlaybackTime;
        item.endInPlaybackTime = projection.endInPlaybackTime;
        item.startInModificationTime = projection.startInModificationTime;
        item.endInModificationTime = projection.endInModificationTime;
        const auto anchors = makeRegionAnchors(readSource, projection.startInModificationTime);
        item.placementAnchorSeconds = anchors.placementAnchorSeconds;
        item.semanticAnchorSeconds = anchors.semanticAnchorSeconds;
        item.source = std::move(readSource);
        nextPlan->items.push_back(std::move(item));
    }

    assignContinuityAndPlacements(nextPlan->items, targetSampleRate);
    return nextPlan;
}

void OpenTunePlaybackRenderer::assignContinuityAndPlacements(
    std::vector<PlaybackRegionRenderItem>& items,
    double targetSampleRate)
{
    std::sort(items.begin(), items.end(),
              [](const PlaybackRegionRenderItem& lhs, const PlaybackRegionRenderItem& rhs)
              {
                  if (lhs.startInPlaybackTime != rhs.startInPlaybackTime)
                      return lhs.startInPlaybackTime < rhs.startInPlaybackTime;
                  if (lhs.endInPlaybackTime != rhs.endInPlaybackTime)
                      return lhs.endInPlaybackTime < rhs.endInPlaybackTime;
                  return lhs.startInModificationTime < rhs.startInModificationTime;
              });

    uint32_t nextContinuityGroup = 1;
    for (size_t index = 0; index < items.size(); ++index)
    {
        auto& item = items[index];
        bool inheritedFromPredecessor = false;

        // Other regions may overlap between a slice and its predecessor, so the
        // predecessor is the nearest earlier item with matching provenance and
        // exact shared playback/modification boundaries.
        for (size_t earlier = index; earlier > 0; --earlier)
        {
            const auto& predecessor = items[earlier - 1];
            if (!sameContinuityProvenance(predecessor.source, item.source)
                || predecessor.endInPlaybackTime != item.startInPlaybackTime
                || predecessor.endInModificationTime != item.startInModificationTime)
                continue;

            // A warped source boundary can move within the shared Stage2
            // output domain. Its output anchor must also meet the actual
            // playback boundary under the predecessor's translation.
            if (!item.source.contentSnapshot->timeGrid->isIdentity()
                && predecessor.startInPlaybackTime
                    + (item.placementAnchorSeconds - predecessor.placementAnchorSeconds)
                    != item.startInPlaybackTime)
                continue;

            item.continuityGroup = predecessor.continuityGroup;
            item.placement = PlaybackSampleGeometry::makeContiguousSlice(
                predecessor.placement,
                item.startInPlaybackTime,
                item.endInPlaybackTime);
            inheritedFromPredecessor = true;
            break;
        }

        if (inheritedFromPredecessor)
            continue;

        item.continuityGroup = nextContinuityGroup++;
        item.placement = PlaybackSampleGeometry::makePlacement(
            targetSampleRate,
            item.startInPlaybackTime,
            item.endInPlaybackTime,
            item.placementAnchorSeconds);
    }
}

void OpenTunePlaybackRenderer::publishRenderPlanFor(std::vector<juce::ARAPlaybackRegion*> playbackRegions)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    for (;;)
    {
        // Capture the publication point before reading the document. A rate
        // reprojection that wins while the build runs invalidates this build.
        auto expected = currentPlan_.load(std::memory_order_acquire);
        const double targetSampleRate = hostSampleRate_.load(std::memory_order_acquire);
        auto next = buildRenderPlan(playbackRegions, targetSampleRate);

        // The host rate changed during the build; rebuild from the new rate.
        if (hostSampleRate_.load(std::memory_order_acquire) != targetSampleRate)
            continue;

        // CRS preparation for a rate change may still be in flight. Keep the
        // region topology but never expose a plan whose source views belong to
        // another grid; the pending reprojection publishes the matching plan.
        next->sourceViewsPrepared = targetSampleRate <= 0.0
            || sourcesPreparedAtRate(next->items, targetSampleRate);

        if (currentPlan_.compareExchange(next, expected))
            return;

        // A reprojection won with a newer rate/source view; rebuild against it.
        playbackRegions = next->playbackRegions;
    }
}

void OpenTunePlaybackRenderer::reprojectRenderPlanForSampleRate(
    double sampleRate,
    const std::shared_ptr<ContentRenderService>& crs)
{
    if (sampleRate <= 0.0)
        return;

    for (;;)
    {
        auto previous = currentPlan_.load(std::memory_order_acquire);
        auto next = std::make_shared<RenderPlan>();
        next->targetSampleRate = sampleRate;
        if (previous != nullptr)
            next->playbackRegions = previous->playbackRegions;

        if (previous != nullptr && crs != nullptr)
        {
            next->items.reserve(previous->items.size());
            for (const auto& frozen : previous->items)
            {
                PlaybackReadSource preparedSource;
                if (!crs->getPlaybackReadSource(frozen.contentKey, preparedSource))
                    continue;

                PlaybackRegionRenderItem item = frozen;
                const auto anchors = makeRegionAnchors(preparedSource, item.startInModificationTime);
                item.placementAnchorSeconds = anchors.placementAnchorSeconds;
                item.semanticAnchorSeconds = anchors.semanticAnchorSeconds;
                item.source = std::move(preparedSource);
                next->items.push_back(std::move(item));
            }

            // Continuity groups are resolved independently of physical
            // adjacency, so overlapping regions cannot break shared D.
            assignContinuityAndPlacements(next->items, sampleRate);

            // A source view still on another grid means preparation did not
            // complete; never publish a rate-mismatched plan.
            if (!sourcesPreparedAtRate(next->items, sampleRate))
                return;
        }

        auto expected = previous;
        if (currentPlan_.compareExchange(next, expected))
            break;
    }
}

void OpenTunePlaybackRenderer::prepareToPlay(double sampleRate,
                                             int maximumSamplesPerBlock,
                                             int numChannels,
                                             juce::AudioProcessor::ProcessingPrecision precision,
                                             AlwaysNonRealtime alwaysNonRealtime)
{
    juce::ignoreUnused(precision, alwaysNonRealtime);

    hostSampleRate_.store(sampleRate, std::memory_order_release);
    numChannels_ = numChannels;
    maximumSamplesPerBlock_ = maximumSamplesPerBlock;
    playbackScratch_.setSize(juce::jmax(1, numChannels_),
                              juce::jmax(1, maximumSamplesPerBlock_),
                              false,
                              true,
                              true);

    // 设备率切换 → 准备所有 CRS caches（各 cache 使用自有 resampler）
    auto crs = std::atomic_load_explicit(&contentRenderServiceSnapshot_, std::memory_order_acquire);
    if (crs != nullptr)
    {
        crs->preparePlaybackSampleRate(sampleRate);
        // This call may arrive off the message thread. Only frozen plan
        // semantics and CRS source views are touched here.
        reprojectRenderPlanForSampleRate(sampleRate, crs);
    }
}

void OpenTunePlaybackRenderer::releaseResources()
{
    playbackScratch_.setSize(0, 0);
}

bool OpenTunePlaybackRenderer::processBlock(juce::AudioBuffer<float>& buffer,
                                             juce::AudioProcessor::Realtime realtime,
                                             const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    if (!shouldRenderPlaybackBlock(realtime, positionInfo.getIsPlaying()))
        return true;

    const auto timeInSamples = positionInfo.getTimeInSamples();
    if (numSamples <= 0 || !timeInSamples.hasValue())
        return true;

    const auto plan = currentPlan_.load(std::memory_order_acquire);
    if (plan == nullptr || plan->items.empty())
        return true;

    const double planSampleRate = plan->targetSampleRate;
    if (!plan->sourceViewsPrepared
        || planSampleRate <= 0.0
        || planSampleRate != hostSampleRate_.load(std::memory_order_acquire))
        return true;

    const int64_t blockStartSample = *timeInSamples;
    const PlaybackSampleGeometry::SampleSpan blockSpan{
        blockStartSample,
        blockStartSample + static_cast<int64_t>(numSamples)};

    const auto* audition = notePitchAudition_.load(std::memory_order_acquire);
    const void* suppressedRegion =
        (realtime == juce::AudioProcessor::Realtime::yes && audition != nullptr)
            ? audition->activeTargetRegion() : nullptr;

    for (const auto& item : plan->items)
    {
        if (static_cast<const void*>(item.playbackRegion) == suppressedRegion)
            continue;

        const auto overlap = PlaybackSampleGeometry::intersect(
            PlaybackSampleGeometry::hostPlaybackSpan(item.placement), blockSpan);
        if (overlap.empty())
            continue;

        const int destinationStartSample =
            static_cast<int>(overlap.begin - blockStartSample);
        const int samplesToCopy = static_cast<int>(overlap.end - overlap.begin);
        if (samplesToCopy <= 0 || destinationStartSample < 0)
            continue;

        const auto address = PlaybackSampleGeometry::mapHostSampleToPrepared(
            item.placement,
            overlap.begin,
            item.source.preparedDry.firstPreparedSample);

        const double outputStartSeconds = item.semanticAnchorSeconds
            + static_cast<double>(overlap.begin - item.placement.playbackBeginSample)
                / planSampleRate;

        const PlaybackReadRequest request(item.source,
                                          address.preparedGlobalIndex,
                                          planSampleRate,
                                          samplesToCopy,
                                          outputStartSeconds);

        playbackScratch_.clear();
        const int copied = readPlaybackAudio(request, playbackScratch_, 0);
        const int mixSamples = juce::jmin(juce::jmin(copied, samplesToCopy),
                                          buffer.getNumSamples() - destinationStartSample);
        if (mixSamples <= 0)
            continue;

        const int channels = juce::jmin(buffer.getNumChannels(),
                                        playbackScratch_.getNumChannels());
        for (int channel = 0; channel < channels; ++channel)
            buffer.addFrom(channel,
                           destinationStartSample,
                           playbackScratch_,
                           channel,
                           0,
                           mixSamples);
    }

    return true;
}

} // namespace OpenTune
