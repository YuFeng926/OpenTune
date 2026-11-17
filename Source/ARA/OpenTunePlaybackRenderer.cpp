#include "OpenTunePlaybackRenderer.h"

#include "OpenTuneDocumentController.h"
#include "../Audio/NotePitchAudition.h"
#include "../Render/ContentRenderService.h"
#include "../Utils/PlaybackAudioReader.h"
#include "../Utils/PlaybackSampleGeometry.h"

#include <algorithm>
#include <exception>
#include <thread>

namespace OpenTune {

namespace {

void requireMessageThread()
{
    auto* messageManager = juce::MessageManager::getInstance();
    if (messageManager == nullptr || !messageManager->isThisTheMessageThread())
        std::terminate();
}

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
    , contentRenderService_(docController != nullptr
                                ? docController->getContentRenderServiceShared()
                                : nullptr)
{
    if (docController != nullptr)
        notePitchAudition_.store(&docController->getNotePitchAudition(),
                                 std::memory_order_release);
}

OpenTunePlaybackRenderer::~OpenTunePlaybackRenderer()
{
    requireMessageThread();
    clearPlaybackSnapshotsAndWait();
    if (documentController_ != nullptr)
    {
        documentController_->unregisterPlaybackRenderer(*this);
        documentController_ = nullptr;
    }
}

void OpenTunePlaybackRenderer::detachDocumentController(OpenTuneDocumentController& owner)
{
    if (documentController_ == &owner)
    {
        requireMessageThread();
        clearPlaybackSnapshotsAndWait();
        documentController_ = nullptr;
    }
}

OpenTunePlaybackRenderer::LifecycleReadGuard::LifecycleReadGuard(
    OpenTunePlaybackRenderer& renderer) noexcept
    : renderer_(renderer.enterLifecycle() ? &renderer : nullptr)
{
}

OpenTunePlaybackRenderer::LifecycleReadGuard::~LifecycleReadGuard()
{
    if (renderer_ != nullptr)
        renderer_->leaveLifecycle();
}

bool OpenTunePlaybackRenderer::enterLifecycle() noexcept
{
    const uint32_t previous = lifecycleGate_.fetch_add(1, std::memory_order_acquire);
    if ((previous & lifecycleClosingBit_) != 0)
    {
        lifecycleGate_.fetch_sub(1, std::memory_order_release);
        return false;
    }
    return true;
}

void OpenTunePlaybackRenderer::leaveLifecycle() noexcept
{
    lifecycleGate_.fetch_sub(1, std::memory_order_release);
}

void OpenTunePlaybackRenderer::closeLifecycleAndWait()
{
    lifecycleGate_.fetch_or(lifecycleClosingBit_, std::memory_order_acq_rel);
    while ((lifecycleGate_.load(std::memory_order_acquire) & lifecycleReaderMask_) != 0)
        std::this_thread::yield();
}

void OpenTunePlaybackRenderer::finishPendingRenderPlanPublication()
{
    while (currentPlan_.hasPending())
        if (!currentPlan_.collect())
            std::this_thread::yield();
}

void OpenTunePlaybackRenderer::didAddPlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    requireMessageThread();
    auto* jucePlaybackRegion = toJucePlaybackRegion(playbackRegion);
    const auto& plan = currentPlan_.latestOwner();
    auto playbackRegions = plan != nullptr ? plan->playbackRegions
                                           : std::vector<juce::ARAPlaybackRegion*>{};
    if (jucePlaybackRegion != nullptr
        && std::find(playbackRegions.begin(), playbackRegions.end(), jucePlaybackRegion)
            == playbackRegions.end())
    {
        playbackRegions.push_back(jucePlaybackRegion);
    }

    publishRenderPlanFor(std::move(playbackRegions),
                         hostSampleRate_.load(std::memory_order_acquire));
}

void OpenTunePlaybackRenderer::willRemovePlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    requireMessageThread();
    auto* jucePlaybackRegion = toJucePlaybackRegion(playbackRegion);
    const auto& plan = currentPlan_.latestOwner();
    auto playbackRegions = plan != nullptr ? plan->playbackRegions
                                           : std::vector<juce::ARAPlaybackRegion*>{};
    playbackRegions.erase(std::remove(playbackRegions.begin(),
                                      playbackRegions.end(),
                                      jucePlaybackRegion),
                          playbackRegions.end());
    publishRenderPlanFor(std::move(playbackRegions),
                         hostSampleRate_.load(std::memory_order_acquire));
}

void OpenTunePlaybackRenderer::refreshRenderPlanFromDocument()
{
    requireMessageThread();
    const auto& plan = currentPlan_.latestOwner();
    publishRenderPlanFor(plan != nullptr ? plan->playbackRegions
                                         : std::vector<juce::ARAPlaybackRegion*>{},
                         hostSampleRate_.load(std::memory_order_acquire));
}

void OpenTunePlaybackRenderer::collectPlaybackSnapshots()
{
    requireMessageThread();
    currentPlan_.collect();
    finishPendingRenderPlanPublication();

    const double requestedSampleRate =
        pendingRenderPlanSampleRate_.exchange(0.0, std::memory_order_acq_rel);
    if (requestedSampleRate <= 0.0 || documentController_ == nullptr)
        return;

    // Preparing the shared source views and freezing this renderer's plan are
    // one message-thread transaction. Another renderer cannot replace the
    // prepared rate between these two operations.
    if (contentRenderService_ != nullptr)
        contentRenderService_->preparePlaybackSampleRate(requestedSampleRate);

    const auto& plan = currentPlan_.latestOwner();
    publishRenderPlanFor(plan != nullptr ? plan->playbackRegions
                                         : std::vector<juce::ARAPlaybackRegion*>{},
                         requestedSampleRate);
}

void OpenTunePlaybackRenderer::handleAsyncUpdate()
{
    collectPlaybackSnapshots();
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

    auto* crs = contentRenderService_.get();
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

void OpenTunePlaybackRenderer::publishRenderPlanFor(
    std::vector<juce::ARAPlaybackRegion*> playbackRegions,
    double targetSampleRate)
{
    requireMessageThread();
    if (documentController_ == nullptr)
        return;

    auto next = buildRenderPlan(std::move(playbackRegions), targetSampleRate);

    // Keep topology available while CRS source views catch up to the rate.
    next->sourceViewsPrepared = targetSampleRate <= 0.0
        || sourcesPreparedAtRate(next->items, targetSampleRate);

    currentPlan_.publish(std::move(next));
    finishPendingRenderPlanPublication();
}

void OpenTunePlaybackRenderer::prepareToPlay(double sampleRate,
                                             int maximumSamplesPerBlock,
                                             int numChannels,
                                             juce::AudioProcessor::ProcessingPrecision precision,
                                             AlwaysNonRealtime alwaysNonRealtime)
{
    LifecycleReadGuard lifecycle(*this);
    if (!lifecycle)
        return;

    juce::ignoreUnused(precision, alwaysNonRealtime);

    hostSampleRate_.store(sampleRate, std::memory_order_release);
    numChannels_ = numChannels;
    maximumSamplesPerBlock_ = maximumSamplesPerBlock;
    playbackScratch_.setSize(juce::jmax(1, numChannels_),
                              juce::jmax(1, maximumSamplesPerBlock_),
                              false,
                              true,
                              true);

    pendingRenderPlanSampleRate_.store(sampleRate, std::memory_order_release);
    if (juce::MessageManager::existsAndIsCurrentThread())
        collectPlaybackSnapshots();
    else
        triggerAsyncUpdate();
}

void OpenTunePlaybackRenderer::clearPlaybackSnapshotsAndWait()
{
    requireMessageThread();
    closeLifecycleAndWait();
    cancelPendingUpdate();

    currentPlan_.clear();
    currentPlan_.collect();
    pendingRenderPlanSampleRate_.store(0.0, std::memory_order_release);
    notePitchAudition_.store(nullptr, std::memory_order_release);
    contentRenderService_.reset();
}

void OpenTunePlaybackRenderer::releaseResources()
{
    LifecycleReadGuard lifecycle(*this);
    if (!lifecycle)
        return;
    playbackScratch_.setSize(0, 0);
}

bool OpenTunePlaybackRenderer::processBlock(juce::AudioBuffer<float>& buffer,
                                             juce::AudioProcessor::Realtime realtime,
                                             const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    LifecycleReadGuard lifecycle(*this);
    if (!lifecycle)
    {
        buffer.clear();
        return true;
    }

    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    if (!shouldRenderPlaybackBlock(realtime, positionInfo.getIsPlaying()))
        return true;

    const auto timeInSamples = positionInfo.getTimeInSamples();
    if (numSamples <= 0 || !timeInSamples.hasValue())
        return true;

    const auto plan = currentPlan_.read();
    if (!plan || plan->items.empty())
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

        const PlaybackReadRequest request{item.source,
                                          address.preparedGlobalIndex,
                                          planSampleRate,
                                          samplesToCopy,
                                          outputStartSeconds};

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
