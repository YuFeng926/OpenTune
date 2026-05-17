#include "VST3AraSession.h"
#include "PluginProcessor.h"

#include "Utils/AppLogger.h"

#include <cmath>
#include <limits>
#include <set>

namespace OpenTune {

namespace {

bool nearlyEqualSeconds(double lhs, double rhs)
{
    return std::abs(lhs - rhs) <= 1.0e-9;
}

constexpr int kAraBindingArchiveMagic = 0x4F544142; // OTAB
constexpr int kAraBindingArchiveVersion = 1;
constexpr int kAraBindingArchiveMaxBindings = 4096;

juce::String mapRestoredAudioModificationPersistentId(const juce::String& archivedPersistentId,
                                                      const juce::ARARestoreObjectsFilter* filter)
{
    if (archivedPersistentId.isEmpty())
        return {};

    if (filter == nullptr)
        return archivedPersistentId;

    auto* audioModification = filter->getAudioModificationToRestoreStateWithID(archivedPersistentId.toRawUTF8());
    if (audioModification == nullptr)
        return {};

    const auto& restoredPersistentId = audioModification->getPersistentID();
    return restoredPersistentId.empty() ? juce::String() : juce::String::fromUTF8(restoredPersistentId.c_str());
}

const VST3AraSession::SourceSlot* findSourceSlotInCollection(
    const std::vector<VST3AraSession::SourceSlot>& sourceSlots,
    juce::ARAAudioSource* audioSource)
{
    const auto it = std::find_if(sourceSlots.begin(), sourceSlots.end(),
                                 [audioSource](const VST3AraSession::SourceSlot& slot)
                                 {
                                     return slot.audioSource == audioSource;
                                 });
    return it != sourceSlots.end() ? &(*it) : nullptr;
}

VST3AraSession::PublishedRegionView buildPublishedRegionViewFromState(
    const VST3AraSession::RegionSlot& regionSlot,
    const VST3AraSession::SourceSlot& sourceSlot)
{
    VST3AraSession::PublishedRegionView view;
    view.regionIdentity = regionSlot.identity;
    view.sourceId = sourceSlot.sourceId;
    view.appliedProjection = regionSlot.appliedProjection;
    view.copiedAudio = sourceSlot.hasAudio() ? sourceSlot.copiedAudio : nullptr;
    view.sampleRate = sourceSlot.sampleRate;
    view.numChannels = sourceSlot.numChannels;
    view.numSamples = sourceSlot.numSamples;
    view.materializationRevision = regionSlot.appliedProjection.appliedMaterializationRevision;
    view.projectionRevision = regionSlot.projectionRevision;
    view.playbackStartSeconds = regionSlot.playbackStartSeconds;
    view.playbackEndSeconds = regionSlot.playbackEndSeconds;
    view.sourceWindow = regionSlot.sourceWindow;
    view.materializationDurationSeconds = regionSlot.materializationDurationSeconds;
    // Derive bindingState from region/source state
    if (regionSlot.appliedProjection.isValid()
        && regionSlot.appliedProjection.materializationId != 0
        && regionSlot.appliedProjection.appliedRegionIdentity == regionSlot.identity)
    {
        view.bindingState = VST3AraSession::BindingState::Renderable;
    }
    else if (regionSlot.appliedProjection.isValid() && regionSlot.appliedProjection.materializationId != 0)
    {
        view.bindingState = VST3AraSession::BindingState::BoundNeedsRender;
    }
    else if (sourceSlot.hasAudio())
    {
        view.bindingState = VST3AraSession::BindingState::Unbound;
    }
    else
    {
        view.bindingState = VST3AraSession::BindingState::HydratingSource;
    }
    return view;
}

VST3AraSession::RegionIdentity reconcilePreferredRegionFromState(
    const std::vector<VST3AraSession::SourceSlot>& sourceSlots,
    const std::vector<VST3AraSession::RegionSlot>& regionSlots,
    const VST3AraSession::RegionIdentity& preferredRegion)
{
    if (preferredRegion.isValid())
    {
        const auto preferredIt = std::find_if(regionSlots.begin(), regionSlots.end(),
                                              [&preferredRegion](const VST3AraSession::RegionSlot& slot)
                                              {
                                                  return slot.identity == preferredRegion && slot.isValid();
                                              });
        if (preferredIt != regionSlots.end()
            && findSourceSlotInCollection(sourceSlots, preferredRegion.audioSource) != nullptr)
        {
            return preferredRegion;
        }
    }

    const auto fallbackIt = std::find_if(regionSlots.begin(), regionSlots.end(),
                                         [&sourceSlots](const VST3AraSession::RegionSlot& slot)
                                         {
                                             return slot.isValid()
                                                 && findSourceSlotInCollection(sourceSlots, slot.identity.audioSource) != nullptr;
                                         });
    return fallbackIt != regionSlots.end() ? fallbackIt->identity : VST3AraSession::RegionIdentity{};
}

} // namespace

VST3AraSession::VST3AraSession()
    : publishedSnapshot_(std::make_shared<PublishedSnapshot>())
    , hydrationWorkerThread_([this]() { hydrationWorkerLoop(); })
{
}

VST3AraSession::~VST3AraSession()
{
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        hydrationWorkerRunning_ = false;
        for (auto& [audioSource, sourceSlot] : sources_)
        {
            juce::ignoreUnused(audioSource);
            sourceSlot.cancelRead = true;
        }
    }

    hydrationCv_.notify_all();

    if (hydrationWorkerThread_.joinable())
        hydrationWorkerThread_.join();
}

VST3AraSession::SnapshotHandle VST3AraSession::buildSnapshotForPublication(
    const std::vector<SourceSlot>& sourceSlots,
    const std::vector<RegionSlot>& regionSlots,
    const RegionIdentity& preferredRegion,
    uint64_t epoch)
{
    auto snapshot = std::make_shared<PublishedSnapshot>();
    snapshot->epoch = epoch;
    snapshot->preferredRegion = reconcilePreferredRegionFromState(sourceSlots, regionSlots, preferredRegion);
    snapshot->publishedRegions.reserve(regionSlots.size());

    for (const auto& regionSlot : regionSlots)
    {
        const auto* sourceSlot = findSourceSlotInCollection(sourceSlots, regionSlot.identity.audioSource);
        if (sourceSlot == nullptr)
            continue;

        const auto view = buildPublishedRegionViewFromState(regionSlot, *sourceSlot);
        if (view.isValid())
            snapshot->publishedRegions.push_back(view);
    }

    return std::static_pointer_cast<const PublishedSnapshot>(snapshot);
}

VST3AraSession::SnapshotHandle VST3AraSession::publishPendingSnapshot(
    const SnapshotHandle& currentSnapshot,
    const std::vector<SourceSlot>& sourceSlots,
    const std::vector<RegionSlot>& regionSlots,
    const RegionIdentity& preferredRegion,
    uint64_t epoch,
    bool pendingSnapshotPublication)
{
    if (!pendingSnapshotPublication)
        return currentSnapshot;

    return buildSnapshotForPublication(sourceSlots, regionSlots, preferredRegion, epoch);
}

VST3AraSession::RegionIdentity VST3AraSession::makeRegionIdentity(
    const juce::ARAPlaybackRegion* playbackRegion)
{
    if (playbackRegion == nullptr)
        return {};

    auto* audioModification = playbackRegion->getAudioModification();
    if (audioModification == nullptr)
        return {};

    auto* audioSource = audioModification->getAudioSource();
    if (audioSource == nullptr)
        return {};

    RegionIdentity identity;
    identity.playbackRegion = const_cast<juce::ARAPlaybackRegion*>(playbackRegion);
    identity.audioSource = audioSource;
    return identity;
}

VST3AraSession::SnapshotHandle VST3AraSession::loadSnapshot() const
{
    return std::atomic_load(&publishedSnapshot_);
}

void VST3AraSession::willBeginEditing()
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();
    ++editingDepth_;
}

void VST3AraSession::didEndEditing()
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();
    if (editingDepth_ > 0)
        --editingDepth_;

    if (editingDepth_ != 0 || !pendingSnapshotPublication_)
        return;

    AppLogger::log("ARA: didEndEditing publishing snapshot");
    publishSnapshotLocked();
}

void VST3AraSession::didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion* playbackRegion)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    AppLogger::log("ARA: didUpdatePlaybackRegionProperties called");

    const auto identity = makeRegionIdentity(playbackRegion);
    if (!identity.isValid())
    {
        AppLogger::log("ARA: PlaybackRegion mapping update ignored because identity is incomplete");
        return;
    }

    auto& sourceSlot = ensureSourceSlot(identity.audioSource);
    auto& regionSlot = ensureRegionSlot(playbackRegion, identity.audioSource);
    const bool projectionChanged = updateRegionProjectionFromPlaybackRegionLocked(regionSlot, playbackRegion);
    regionSlot.sourceWindow.sourceId = sourceSlot.sourceId;

    if (projectionChanged)
        bumpRegionProjectionRevisionLocked(playbackRegion);

    const bool bindingChanged = applyBindingToRegionSlotLocked(regionSlot);
    const bool preferredChanged = preferredRegion_ != identity;
    updatePreferredRegionLocked(identity);

    if (projectionChanged || bindingChanged || preferredChanged)
        markSnapshotDirtyLocked();
}

void VST3AraSession::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    AppLogger::log("ARA: didAddPlaybackRegionToAudioModification called");

    if (audioModification == nullptr || playbackRegion == nullptr)
    {
        AppLogger::log("ARA: Null parameters in didAddPlaybackRegionToAudioModification");
        return;
    }

    auto* audioSource = audioModification->getAudioSource();
    if (audioSource == nullptr)
    {
        AppLogger::log("ARA: PlaybackRegion add ignored because AudioSource is missing");
        return;
    }

    auto& sourceSlot = ensureSourceSlot(audioSource);
    auto& regionSlot = ensureRegionSlot(playbackRegion, audioSource);
    regionSlot.audioModificationPersistentId = copyAudioModificationPersistentId(audioModification);
    const bool projectionChanged = updateRegionProjectionFromPlaybackRegionLocked(regionSlot, playbackRegion);
    regionSlot.sourceWindow.sourceId = sourceSlot.sourceId;

    if (projectionChanged)
        bumpRegionProjectionRevisionLocked(playbackRegion);

    const bool bindingChanged = applyBindingToRegionSlotLocked(regionSlot);
    const bool preferredChanged = preferredRegion_ != regionSlot.identity;
    updatePreferredRegionLocked(regionSlot.identity);

    if (regionSlot.audioModificationPersistentId.isEmpty())
    {
        AppLogger::error("[ARA] AudioModification has no persistent ID; materialization binding cannot be restored");
    }

    if (projectionChanged || bindingChanged || preferredChanged)
        markSnapshotDirtyLocked();
}

void VST3AraSession::didUpdateAudioSourceProperties(juce::ARAAudioSource* audioSource)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    AppLogger::log("ARA: didUpdateAudioSourceProperties called");

    if (audioSource == nullptr)
        return;

    auto& sourceSlot = ensureSourceSlot(audioSource);
    const auto nextName = audioSource->getName() != nullptr
                            ? juce::String::fromUTF8(audioSource->getName())
                            : juce::String();
    const auto nextSampleRate = audioSource->getSampleRate();
    const auto nextNumChannels = static_cast<int>(audioSource->getChannelCount());
    const auto nextNumSamples = audioSource->getSampleCount();
    const bool audioShapeChanged = sourceSlot.sampleRate != nextSampleRate
        || sourceSlot.numChannels != nextNumChannels
        || sourceSlot.numSamples != nextNumSamples;
    const bool metadataChanged = sourceSlot.name != nextName
        || sourceSlot.sampleRate != nextSampleRate
        || sourceSlot.numChannels != nextNumChannels
        || sourceSlot.numSamples != nextNumSamples;

    sourceSlot.name = nextName;
    sourceSlot.sampleRate = nextSampleRate;
    sourceSlot.numChannels = nextNumChannels;
    sourceSlot.numSamples = nextNumSamples;

    AppLogger::log("ARA: AudioSource '" + sourceSlot.name
                   + "' sampleRate=" + juce::String(sourceSlot.sampleRate)
                   + " channels=" + juce::String(sourceSlot.numChannels)
                   + " samples=" + juce::String(static_cast<juce::int64>(sourceSlot.numSamples)));

    if (audioShapeChanged)
    {
        bumpSourceContentRevisionLocked(audioSource);
        clearSourcePayloadLocked(sourceSlot);
        if (sourceSlot.readingFromHost)
            sourceSlot.cancelRead = true;

        enqueueSourceHydrationLocked(audioSource);
    }

    if (metadataChanged)
        markSnapshotDirtyLocked();
}

void VST3AraSession::doUpdateAudioSourceContent(juce::ARAAudioSource* audioSource,
                                                juce::ARAContentUpdateScopes scopeFlags)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    if (audioSource == nullptr)
        return;

    auto& sourceSlot = ensureSourceSlot(audioSource);
    juce::ignoreUnused(sourceSlot);

    if (!scopeFlags.affectSamples())
        return;

    bumpSourceContentRevisionLocked(audioSource);
    clearSourcePayloadLocked(sourceSlot);
    if (sourceSlot.readingFromHost)
        sourceSlot.cancelRead = true;
    enqueueSourceHydrationLocked(audioSource);
    markSnapshotDirtyLocked();
}

void VST3AraSession::willEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                        bool enable)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    if (audioSource == nullptr)
        return;

    auto& sourceSlot = ensureSourceSlot(audioSource);

    if (!enable)
    {
        clearSourcePayloadLocked(sourceSlot);
        invalidateSourceReaderLeaseLocked(sourceSlot);
        markSnapshotDirtyLocked();
        publishSnapshotLocked();
    }
}

void VST3AraSession::didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                                       bool enable)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    if (audioSource == nullptr)
        return;

    auto& sourceSlot = ensureSourceSlot(audioSource);

    if (!enable)
    {
        sourceSlot.sampleAccessEnabled = false;
        drainDeferredSourceCleanupLocked();
        return;
    }

    sourceSlot.sampleAccessEnabled = true;
    sourceSlot.cancelRead = false;

    if (sourceSlot.readerLease == nullptr)
    {
        sourceSlot.readerLease = std::make_unique<ARA::PlugIn::HostAudioReader>(audioSource);
        ++sourceSlot.leaseGeneration;
    }

    if (sourceSlot.readingFromHost)
    {
        sourceSlot.enablePendingHydration = true;
        return;
    }

    enqueueSourceHydrationLocked(audioSource);
}

void VST3AraSession::willRemovePlaybackRegionFromAudioModification(
    juce::ARAPlaybackRegion* playbackRegion)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    if (removePlaybackRegionFromStateLocked(playbackRegion))
    {
        if (editingDepth_ == 0)
            publishSnapshotLocked();
        else
            markSnapshotDirtyLocked();
    }
}

void VST3AraSession::willDestroyAudioSource(juce::ARAAudioSource* audioSource)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    if (removeAudioSourceFromStateLocked(audioSource))
    {
        if (editingDepth_ == 0)
            publishSnapshotLocked();
        else
            markSnapshotDirtyLocked();
    }
}

void VST3AraSession::bindPlaybackRegionToMaterialization(juce::ARAPlaybackRegion* playbackRegion,
                                                         uint64_t materializationId,
                                                         uint64_t materializationRevision,
                                                         uint64_t projectionRevision,
                                                         SourceWindow sourceWindow,
                                                         double materializationDurationSeconds,
                                                 double playbackStartSeconds)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    const auto identity = makeRegionIdentity(playbackRegion);
    if (!identity.isValid() || materializationId == 0)
    {
        AppLogger::error("[ARA] bindPlaybackRegionToMaterialization: invalid playbackRegion or materializationId");
        return;
    }

    auto& sourceSlot = ensureSourceSlot(identity.audioSource);
    auto& regionSlot = ensureRegionSlot(playbackRegion, identity.audioSource);
    if (regionSlot.audioModificationPersistentId.isEmpty())
    {
        if (auto* audioModification = playbackRegion->getAudioModification())
            regionSlot.audioModificationPersistentId = copyAudioModificationPersistentId(audioModification);
    }

    if (regionSlot.audioModificationPersistentId.isEmpty())
    {
        AppLogger::error("[ARA] bindPlaybackRegionToMaterialization: AudioModification persistent ID is missing");
        return;
    }

    AraMaterializationBinding binding;
    binding.audioModificationPersistentId = regionSlot.audioModificationPersistentId;
    binding.sourceId = sourceSlot.sourceId;
    binding.materializationId = materializationId;
    binding.sourceWindow = sourceWindow;
    binding.materializationRevision = materializationRevision;
    binding.materializationDurationSeconds = materializationDurationSeconds;
    upsertMaterializationBindingLocked(binding);

    applyBindingToRegionSlotLocked(regionSlot);
    regionSlot.appliedProjection.appliedProjectionRevision = projectionRevision;
    regionSlot.appliedProjection.playbackStartSeconds = playbackStartSeconds;
    regionSlot.sourceWindow = sourceWindow;
    regionSlot.materializationDurationSeconds = materializationDurationSeconds;

    publishSnapshotLocked();
}

void VST3AraSession::updatePlaybackRegionMaterializationRevisions(juce::ARAPlaybackRegion* playbackRegion,
                                                                  uint64_t materializationRevision,
                                                                  uint64_t projectionRevision)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    auto* regionSlot = findRegionSlot(playbackRegion);
    if (regionSlot == nullptr)
    {
        AppLogger::error("[ARA] updatePlaybackRegionMaterializationRevisions: no region slot for binding update");
        return;
    }

    if (regionSlot->audioModificationPersistentId.isNotEmpty())
    {
        auto bindingIt = materializationBindings_.find(regionSlot->audioModificationPersistentId);
        if (bindingIt != materializationBindings_.end())
            bindingIt->second.materializationRevision = materializationRevision;
    }

    for (auto& [playbackRegionKey, siblingRegionSlot] : regions_)
    {
        juce::ignoreUnused(playbackRegionKey);
        if (siblingRegionSlot.audioModificationPersistentId == regionSlot->audioModificationPersistentId)
        {
            siblingRegionSlot.appliedProjection.appliedMaterializationRevision = materializationRevision;
            siblingRegionSlot.appliedProjection.appliedProjectionRevision = projectionRevision;
        }
    }
    publishSnapshotLocked();
}

void VST3AraSession::clearPlaybackRegionMaterialization(juce::ARAPlaybackRegion* playbackRegion)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    drainDeferredSourceCleanupLocked();

    auto* regionSlot = findRegionSlot(playbackRegion);
    if (regionSlot == nullptr)
        return;

    if (regionSlot->audioModificationPersistentId.isNotEmpty())
        materializationBindings_.erase(regionSlot->audioModificationPersistentId);

    for (auto& [playbackRegionKey, siblingRegionSlot] : regions_)
    {
        juce::ignoreUnused(playbackRegionKey);
        if (siblingRegionSlot.audioModificationPersistentId == regionSlot->audioModificationPersistentId)
        {
            siblingRegionSlot.appliedProjection.clear();
            siblingRegionSlot.materializationDurationSeconds = 0.0;
        }
    }
    publishSnapshotLocked();
}

std::vector<VST3AraSession::AraMaterializationBinding> VST3AraSession::exportMaterializationBindings() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    std::vector<AraMaterializationBinding> result;
    result.reserve(materializationBindings_.size());
    for (const auto& [persistentId, binding] : materializationBindings_)
    {
        juce::ignoreUnused(persistentId);
        result.push_back(binding);
    }
    return result;
}

void VST3AraSession::replaceMaterializationBindings(std::vector<AraMaterializationBinding> bindings)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    materializationBindings_.clear();

    for (const auto& binding : bindings)
        upsertMaterializationBindingLocked(binding);

    applyBindingsToRegionSlotsLocked();
    publishSnapshotLocked();
}

bool VST3AraSession::storeMaterializationBindings(juce::OutputStream& output,
                                                  const juce::ARAStoreObjectsFilter* filter) const
{
    juce::ignoreUnused(filter);

    const auto bindings = exportMaterializationBindings();

    bool ok = output.writeInt(kAraBindingArchiveMagic);
    ok = output.writeInt(kAraBindingArchiveVersion) && ok;
    ok = output.writeInt(static_cast<int>(bindings.size())) && ok;

    for (const auto& binding : bindings)
    {
        ok = output.writeString(binding.audioModificationPersistentId) && ok;
        ok = output.writeInt64(static_cast<juce::int64>(binding.sourceId)) && ok;
        ok = output.writeInt64(static_cast<juce::int64>(binding.materializationId)) && ok;
        ok = output.writeInt64(static_cast<juce::int64>(binding.sourceWindow.sourceId)) && ok;
        ok = output.writeDouble(binding.sourceWindow.sourceStartSeconds) && ok;
        ok = output.writeDouble(binding.sourceWindow.sourceEndSeconds) && ok;
        ok = output.writeInt64(static_cast<juce::int64>(binding.materializationRevision)) && ok;
        ok = output.writeDouble(binding.materializationDurationSeconds) && ok;
    }

    return ok;
}

bool VST3AraSession::restoreMaterializationBindings(juce::InputStream& input,
                                                    const juce::ARARestoreObjectsFilter* filter)
{
    const int magic = input.readInt();
    if (magic != kAraBindingArchiveMagic)
    {
        AppLogger::error("[ARA] Binding archive restore failed: invalid magic");
        return false;
    }

    const int version = input.readInt();
    if (version != kAraBindingArchiveVersion)
    {
        AppLogger::error("[ARA] Binding archive restore failed: unsupported version "
                         + juce::String(version));
        return false;
    }

    const int bindingCount = input.readInt();
    if (bindingCount < 0 || bindingCount > kAraBindingArchiveMaxBindings)
    {
        AppLogger::error("[ARA] Binding archive restore failed: invalid binding count "
                         + juce::String(bindingCount));
        return false;
    }

    std::vector<AraMaterializationBinding> restored;
    restored.reserve(static_cast<size_t>(bindingCount));
    for (int i = 0; i < bindingCount; ++i)
    {
        AraMaterializationBinding binding;
        binding.audioModificationPersistentId = input.readString();
        binding.audioModificationPersistentId = mapRestoredAudioModificationPersistentId(binding.audioModificationPersistentId,
                                                                                         filter);
        if (binding.audioModificationPersistentId.isEmpty())
        {
            const auto ignoredSourceId = input.readInt64();
            const auto ignoredMaterializationId = input.readInt64();
            const auto ignoredSourceWindowId = input.readInt64();
            const auto ignoredStart = input.readDouble();
            const auto ignoredEnd = input.readDouble();
            const auto ignoredRevision = input.readInt64();
            const auto ignoredDuration = input.readDouble();
            juce::ignoreUnused(ignoredSourceId,
                               ignoredMaterializationId,
                               ignoredSourceWindowId,
                               ignoredStart,
                               ignoredEnd,
                               ignoredRevision,
                               ignoredDuration);
            continue;
        }

        binding.sourceId = static_cast<uint64_t>(input.readInt64());
        binding.materializationId = static_cast<uint64_t>(input.readInt64());
        binding.sourceWindow.sourceId = static_cast<uint64_t>(input.readInt64());
        binding.sourceWindow.sourceStartSeconds = input.readDouble();
        binding.sourceWindow.sourceEndSeconds = input.readDouble();
        binding.materializationRevision = static_cast<uint64_t>(input.readInt64());
        binding.materializationDurationSeconds = input.readDouble();

        if (!binding.isValid() || binding.materializationDurationSeconds <= 0.0)
        {
            AppLogger::error("[ARA] Binding archive restore failed: invalid binding record");
            return false;
        }

        restored.push_back(std::move(binding));
    }

    replaceMaterializationBindings(std::move(restored));
    return true;
}

VST3AraSession::SourceSlot* VST3AraSession::findSourceSlot(juce::ARAAudioSource* audioSource)
{
    const auto it = sources_.find(audioSource);
    return it != sources_.end() ? &it->second : nullptr;
}

const VST3AraSession::SourceSlot* VST3AraSession::findSourceSlot(juce::ARAAudioSource* audioSource) const
{
    const auto it = sources_.find(audioSource);
    return it != sources_.end() ? &it->second : nullptr;
}

VST3AraSession::RegionSlot* VST3AraSession::findRegionSlot(juce::ARAPlaybackRegion* playbackRegion)
{
    const auto it = regions_.find(playbackRegion);
    return it != regions_.end() ? &it->second : nullptr;
}

const VST3AraSession::RegionSlot* VST3AraSession::findRegionSlot(juce::ARAPlaybackRegion* playbackRegion) const
{
    const auto it = regions_.find(playbackRegion);
    return it != regions_.end() ? &it->second : nullptr;
}

VST3AraSession::SourceSlot& VST3AraSession::ensureSourceSlot(juce::ARAAudioSource* audioSource)
{
    auto& slot = sources_[audioSource];
    slot.audioSource = audioSource;
    if (slot.sourceId == 0)
        slot.sourceId = nextSourceId_++;
    return slot;
}

VST3AraSession::RegionSlot& VST3AraSession::ensureRegionSlot(juce::ARAPlaybackRegion* playbackRegion,
                                                             juce::ARAAudioSource* audioSource)
{
    auto& slot = regions_[playbackRegion];
    slot.identity.playbackRegion = playbackRegion;
    slot.identity.audioSource = audioSource;
    if (slot.audioModificationPersistentId.isEmpty())
    {
        if (auto* audioModification = playbackRegion != nullptr ? playbackRegion->getAudioModification() : nullptr)
            slot.audioModificationPersistentId = copyAudioModificationPersistentId(audioModification);
    }
    return slot;
}

VST3AraSession::SnapshotHandle VST3AraSession::buildPublishedSnapshotLocked() const
{
    std::vector<SourceSlot> sourceSlots;
    sourceSlots.reserve(sources_.size());
    for (const auto& [audioSource, sourceSlot] : sources_)
    {
        juce::ignoreUnused(audioSource);
        sourceSlots.push_back(sourceSlot);
    }

    std::vector<RegionSlot> regionSlots;
    regionSlots.reserve(regions_.size());
    for (const auto& [playbackRegion, regionSlot] : regions_)
    {
        juce::ignoreUnused(playbackRegion);
        regionSlots.push_back(regionSlot);
    }

    return buildSnapshotForPublication(sourceSlots, regionSlots, preferredRegion_, nextPublishedEpoch_);
}

void VST3AraSession::publishSnapshotLocked()
{
    reconcilePreferredRegionLocked();

    const auto snapshot = buildPublishedSnapshotLocked();
    preferredRegion_ = snapshot != nullptr ? snapshot->preferredRegion : RegionIdentity{};
    std::atomic_store(&publishedSnapshot_, snapshot);
    ++nextPublishedEpoch_;
    pendingSnapshotPublication_ = false;
}

bool VST3AraSession::sourceNeedsHydrationLocked(const SourceSlot& sourceSlot) const noexcept
{
    return sourceSlot.sampleAccessEnabled
        && sourceSlot.readerLease != nullptr
        && sourceSlot.numSamples > 0
        && sourceSlot.numChannels > 0
        && (sourceSlot.copiedAudio == nullptr || sourceSlot.hydratedContentRevision != sourceSlot.contentRevision);
}

void VST3AraSession::clearSourcePayloadLocked(SourceSlot& sourceSlot) noexcept
{
    sourceSlot.copiedAudio.reset();
    sourceSlot.hydratedContentRevision = 0;
}

void VST3AraSession::enqueueSourceHydrationLocked(juce::ARAAudioSource* audioSource)
{
    auto* sourceSlot = findSourceSlot(audioSource);
    if (audioSource == nullptr || sourceSlot == nullptr)
        return;

    if (!sourceNeedsHydrationLocked(*sourceSlot) || sourceSlot->queuedForHydration)
        return;

    sourceSlot->queuedForHydration = true;
    hydrationQueue_.push_back(audioSource);
    hydrationCv_.notify_one();
}

void VST3AraSession::invalidateSourceReaderLeaseLocked(SourceSlot& sourceSlot) noexcept
{
    ++sourceSlot.leaseGeneration;
    sourceSlot.sampleAccessEnabled = false;
    sourceSlot.cancelRead = true;
    sourceSlot.queuedForHydration = false;

    if (sourceSlot.readingFromHost)
    {
        if (sourceSlot.retiringReaderLease == nullptr)
            sourceSlot.retiringReaderLease = std::move(sourceSlot.readerLease);
        else
            sourceSlot.readerLease.reset();
    }
    else
    {
        sourceSlot.readerLease.reset();
        sourceSlot.retiringReaderLease.reset();
    }

    sourceSlot.pendingLeaseReset = sourceSlot.retiringReaderLease != nullptr;
}

void VST3AraSession::drainDeferredSourceCleanupLocked()
{
    for (auto it = sources_.begin(); it != sources_.end();)
    {
        auto& sourceSlot = it->second;

        if (sourceSlot.pendingLeaseReset && !sourceSlot.readingFromHost)
        {
            sourceSlot.retiringReaderLease.reset();
            sourceSlot.cancelRead = false;
            sourceSlot.pendingLeaseReset = false;
        }

        if (sourceSlot.pendingRemoval && !sourceSlot.pendingLeaseReset && !sourceSlot.readingFromHost)
        {
            it = sources_.erase(it);
            continue;
        }

        ++it;
    }
}

void VST3AraSession::hydrationWorkerLoop()
{
    constexpr int64_t kHydrationChunkSamples = 32768;

    while (true)
    {
        juce::ARAAudioSource* audioSource = nullptr;
        ARA::PlugIn::HostAudioReader* reader = nullptr;
        int numChannels = 0;
        int64_t numSamples = 0;
        uint64_t targetContentRevision = 0;
        uint64_t leaseGeneration = 0;

        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            hydrationCv_.wait(lock,
                              [this]()
                              {
                                  return !hydrationWorkerRunning_ || !hydrationQueue_.empty();
                              });

            if (!hydrationWorkerRunning_ && hydrationQueue_.empty())
                return;

            audioSource = hydrationQueue_.front();
            hydrationQueue_.pop_front();

            auto* sourceSlot = findSourceSlot(audioSource);
            if (sourceSlot == nullptr)
                continue;

            sourceSlot->queuedForHydration = false;
            if (!sourceNeedsHydrationLocked(*sourceSlot) || sourceSlot->readingFromHost)
                continue;

            if (sourceSlot->numSamples > std::numeric_limits<int>::max())
            {
                AppLogger::error("[ARA] source hydration skipped because sample count exceeds AudioBuffer capacity");
                continue;
            }

            sourceSlot->readingFromHost = true;
            sourceSlot->cancelRead = false;
            reader = sourceSlot->readerLease.get();
            numChannels = sourceSlot->numChannels;
            numSamples = sourceSlot->numSamples;
            targetContentRevision = sourceSlot->contentRevision;
            leaseGeneration = sourceSlot->leaseGeneration;
        }

        bool readSuccess = true;
        bool canceled = false;
        auto copiedAudio = std::make_shared<juce::AudioBuffer<float>>(numChannels, static_cast<int>(numSamples));
        copiedAudio->clear();

        std::vector<void*> channelPointers(static_cast<size_t>(numChannels));
        for (int64_t sampleOffset = 0; sampleOffset < numSamples; sampleOffset += kHydrationChunkSamples)
        {
            const int64_t chunkSamples = std::min<int64_t>(kHydrationChunkSamples, numSamples - sampleOffset);

            {
                const std::lock_guard<std::mutex> lock(stateMutex_);
                auto* sourceSlot = findSourceSlot(audioSource);
                if (sourceSlot == nullptr
                    || !sourceSlot->sampleAccessEnabled
                    || sourceSlot->readerLease == nullptr
                    || sourceSlot->leaseGeneration != leaseGeneration
                    || sourceSlot->cancelRead)
                {
                    canceled = true;
                    readSuccess = false;
                }
            }

            if (!readSuccess)
                break;

            {
                const std::lock_guard<std::mutex> lock(stateMutex_);
                auto* sourceSlot = findSourceSlot(audioSource);
                if (sourceSlot == nullptr)
                {
                    canceled = true;
                    readSuccess = false;
                    break;
                }

                sourceSlot->hostReadInFlight = true;
            }

            for (int channel = 0; channel < numChannels; ++channel)
            {
                channelPointers[static_cast<size_t>(channel)] = copiedAudio->getWritePointer(channel,
                                                                                            static_cast<int>(sampleOffset));
            }

            const bool chunkReadSuccess = reader->readAudioSamples(sampleOffset, chunkSamples, channelPointers.data());

            {
                const std::lock_guard<std::mutex> lock(stateMutex_);
                auto* sourceSlot = findSourceSlot(audioSource);
                if (sourceSlot != nullptr)
                    sourceSlot->hostReadInFlight = false;
            }

            if (!chunkReadSuccess)
            {
                AppLogger::error("[ARA] source hydration readAudioSamples failed");
                readSuccess = false;
                break;
            }
        }

        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            auto* sourceSlot = findSourceSlot(audioSource);
            if (sourceSlot == nullptr)
                continue;

            sourceSlot->readingFromHost = false;
            sourceSlot->cancelRead = false;

            if (sourceSlot->enablePendingHydration)
            {
                sourceSlot->enablePendingHydration = false;
                enqueueSourceHydrationLocked(audioSource);
            }

            const bool canCommit = readSuccess
                && !canceled
                && sourceSlot->sampleAccessEnabled
                && sourceSlot->readerLease != nullptr
                && sourceSlot->leaseGeneration == leaseGeneration
                && sourceSlot->contentRevision == targetContentRevision;

            if (canCommit)
            {
                sourceSlot->copiedAudio = std::move(copiedAudio);
                sourceSlot->hydratedContentRevision = targetContentRevision;
                markSnapshotDirtyLocked();
                if (editingDepth_ == 0)
                    publishSnapshotLocked();

                auto* processor = processor_.load(std::memory_order_acquire);
                if (processor != nullptr)
                {
                    struct BirthWorkItem {
                        RegionIdentity regionIdentity;
                        uint64_t sourceId{0};
                        std::shared_ptr<const juce::AudioBuffer<float>> audio;
                        double sampleRate{0.0};
                        SourceWindow window;
                        double playbackStart{0.0};
                    };

                    std::vector<BirthWorkItem> worklist;
                    std::set<juce::String> queuedAudioModifications;
                    for (const auto& [playbackRegion, regionSlot] : regions_)
                    {
                        if (regionSlot.identity.audioSource != audioSource)
                            continue;
                        if (regionSlot.appliedProjection.isValid() && regionSlot.appliedProjection.materializationId != 0)
                            continue;
                        if (regionSlot.audioModificationPersistentId.isEmpty())
                        {
                            AppLogger::error("[ARA] auto-birth skipped: AudioModification persistent ID is missing");
                            continue;
                        }
                        if (materializationBindings_.find(regionSlot.audioModificationPersistentId) != materializationBindings_.end())
                            continue;
                        if (!queuedAudioModifications.insert(regionSlot.audioModificationPersistentId).second)
                            continue;

                        BirthWorkItem item;
                        item.regionIdentity = regionSlot.identity;
                        item.sourceId = sourceSlot->sourceId;
                        item.audio = sourceSlot->copiedAudio;
                        item.sampleRate = sourceSlot->sampleRate;
                        item.window = regionSlot.sourceWindow;
                        item.playbackStart = regionSlot.playbackStartSeconds;
                        worklist.push_back(std::move(item));
                    }

                    for (auto& item : worklist)
                    {
                        lock.unlock();

                        auto birthResult = processor->ensureAraRegionMaterialization(
                            item.regionIdentity.audioSource,
                            item.sourceId,
                            item.audio,
                            item.sampleRate,
                            item.window,
                            item.playbackStart);

                        lock.lock();

                        if (birthResult.has_value() && birthResult->materializationId != 0)
                        {
                            auto* updatedRegionSlot = findRegionSlot(item.regionIdentity.playbackRegion);
                            if (updatedRegionSlot != nullptr
                                && updatedRegionSlot->audioModificationPersistentId.isNotEmpty()
                                && materializationBindings_.find(updatedRegionSlot->audioModificationPersistentId) == materializationBindings_.end())
                            {
                                AraMaterializationBinding binding;
                                binding.audioModificationPersistentId = updatedRegionSlot->audioModificationPersistentId;
                                binding.sourceId = birthResult->sourceId;
                                binding.materializationId = birthResult->materializationId;
                                binding.sourceWindow = updatedRegionSlot->sourceWindow;
                                binding.materializationRevision = birthResult->materializationRevision;
                                binding.materializationDurationSeconds = birthResult->materializationDurationSeconds;
                                upsertMaterializationBindingLocked(binding);
                                applyBindingsToRegionSlotsLocked();
                                markSnapshotDirtyLocked();
                            }
                        }
                    }

                    if (pendingSnapshotPublication_ && editingDepth_ == 0)
                        publishSnapshotLocked();
                }
            }

            if (sourceSlot->pendingRemoval && !sourceSlot->pendingLeaseReset)
                sources_.erase(audioSource);

        }
    }
}

void VST3AraSession::bumpSourceContentRevisionLocked(juce::ARAAudioSource* audioSource)
{
    auto* sourceSlot = findSourceSlot(audioSource);
    if (sourceSlot == nullptr)
        return;

    sourceSlot->contentRevision = nextSourceContentRevision_++;
}

void VST3AraSession::bumpRegionProjectionRevisionLocked(juce::ARAPlaybackRegion* playbackRegion)
{
    auto* regionSlot = findRegionSlot(playbackRegion);
    if (regionSlot == nullptr)
        return;

    regionSlot->projectionRevision = nextRegionProjectionRevision_++;
}

bool VST3AraSession::updateRegionProjectionFromPlaybackRegionLocked(
    RegionSlot& regionSlot,
    const juce::ARAPlaybackRegion* playbackRegion)
{
    const auto previousIdentity = regionSlot.identity;
    const double previousPlaybackStart = regionSlot.playbackStartSeconds;
    const double previousPlaybackEnd = regionSlot.playbackEndSeconds;
    const SourceWindow previousSourceWindow = regionSlot.sourceWindow;

    regionSlot.identity = makeRegionIdentity(playbackRegion);
    regionSlot.playbackStartSeconds = playbackRegion->getStartInPlaybackTime();
    regionSlot.playbackEndSeconds = playbackRegion->getEndInPlaybackTime();
    regionSlot.sourceWindow.sourceStartSeconds = playbackRegion->getStartInAudioModificationTime();
    regionSlot.sourceWindow.sourceEndSeconds = playbackRegion->getEndInAudioModificationTime();

    return regionSlot.identity != previousIdentity
        || !nearlyEqualSeconds(regionSlot.playbackStartSeconds, previousPlaybackStart)
        || !nearlyEqualSeconds(regionSlot.playbackEndSeconds, previousPlaybackEnd)
        || !nearlyEqualSeconds(regionSlot.sourceWindow.sourceStartSeconds, previousSourceWindow.sourceStartSeconds)
        || !nearlyEqualSeconds(regionSlot.sourceWindow.sourceEndSeconds, previousSourceWindow.sourceEndSeconds);
}

juce::String VST3AraSession::copyAudioModificationPersistentId(juce::ARAAudioModification* audioModification) const
{
    if (audioModification == nullptr)
        return {};

    const auto& persistentId = audioModification->getPersistentID();
    return persistentId.empty() ? juce::String() : juce::String::fromUTF8(persistentId.c_str());
}

bool VST3AraSession::applyBindingToRegionSlotLocked(RegionSlot& regionSlot)
{
    const auto previousProjection = regionSlot.appliedProjection;
    const double previousDuration = regionSlot.materializationDurationSeconds;

    if (regionSlot.audioModificationPersistentId.isEmpty())
    {
        const bool hadProjection = regionSlot.appliedProjection.isValid()
            || regionSlot.materializationDurationSeconds != 0.0;
        regionSlot.appliedProjection.clear();
        regionSlot.materializationDurationSeconds = 0.0;
        return hadProjection;
    }

    const auto bindingIt = materializationBindings_.find(regionSlot.audioModificationPersistentId);
    if (bindingIt == materializationBindings_.end())
    {
        const bool hadProjection = regionSlot.appliedProjection.isValid()
            || regionSlot.materializationDurationSeconds != 0.0;
        regionSlot.appliedProjection.clear();
        regionSlot.materializationDurationSeconds = 0.0;
        return hadProjection;
    }

    const auto& binding = bindingIt->second;
    if (binding.sourceId != regionSlot.sourceWindow.sourceId)
    {
        const bool hadProjection = regionSlot.appliedProjection.isValid()
            || regionSlot.materializationDurationSeconds != 0.0;
        regionSlot.appliedProjection.clear();
        regionSlot.materializationDurationSeconds = 0.0;
        return hadProjection;
    }

    regionSlot.appliedProjection.sourceId = binding.sourceId;
    regionSlot.appliedProjection.materializationId = binding.materializationId;
    regionSlot.appliedProjection.appliedMaterializationRevision = binding.materializationRevision;
    regionSlot.appliedProjection.appliedProjectionRevision = regionSlot.projectionRevision;
    regionSlot.appliedProjection.appliedSourceWindow = binding.sourceWindow;
    regionSlot.appliedProjection.playbackStartSeconds = regionSlot.playbackStartSeconds;
    regionSlot.appliedProjection.appliedRegionIdentity = regionSlot.identity;
    regionSlot.materializationDurationSeconds = binding.materializationDurationSeconds;

    return previousProjection.sourceId != regionSlot.appliedProjection.sourceId
        || previousProjection.materializationId != regionSlot.appliedProjection.materializationId
        || previousProjection.appliedMaterializationRevision != regionSlot.appliedProjection.appliedMaterializationRevision
        || previousProjection.appliedProjectionRevision != regionSlot.appliedProjection.appliedProjectionRevision
        || previousProjection.appliedRegionIdentity != regionSlot.appliedProjection.appliedRegionIdentity
        || !nearlyEqualSeconds(previousProjection.playbackStartSeconds, regionSlot.appliedProjection.playbackStartSeconds)
        || !nearlyEqualSeconds(previousDuration, regionSlot.materializationDurationSeconds)
        || !nearlyEqualSeconds(previousProjection.appliedSourceWindow.sourceStartSeconds, regionSlot.appliedProjection.appliedSourceWindow.sourceStartSeconds)
        || !nearlyEqualSeconds(previousProjection.appliedSourceWindow.sourceEndSeconds, regionSlot.appliedProjection.appliedSourceWindow.sourceEndSeconds)
        || previousProjection.appliedSourceWindow.sourceId != regionSlot.appliedProjection.appliedSourceWindow.sourceId;
}

void VST3AraSession::applyBindingsToRegionSlotsLocked()
{
    for (auto& [playbackRegion, regionSlot] : regions_)
    {
        juce::ignoreUnused(playbackRegion);
        applyBindingToRegionSlotLocked(regionSlot);
    }
}

void VST3AraSession::upsertMaterializationBindingLocked(const AraMaterializationBinding& binding)
{
    if (!binding.isValid())
    {
        AppLogger::error("[ARA] Ignored invalid materialization binding");
        return;
    }

    materializationBindings_[binding.audioModificationPersistentId] = binding;
}

bool VST3AraSession::removePlaybackRegionFromStateLocked(juce::ARAPlaybackRegion* playbackRegion)
{
    if (playbackRegion == nullptr)
        return false;

    const auto regionIt = regions_.find(playbackRegion);
    if (regionIt == regions_.end())
        return false;

    const auto removedIdentity = regionIt->second.identity;
    regions_.erase(regionIt);

    if (preferredRegion_ == removedIdentity || preferredRegion_.playbackRegion == playbackRegion)
        preferredRegion_.clear();

    reconcilePreferredRegionLocked();
    return true;
}

bool VST3AraSession::removeAudioSourceFromStateLocked(juce::ARAAudioSource* audioSource)
{
    if (audioSource == nullptr)
        return false;

    const auto sourceIt = sources_.find(audioSource);
    const bool removedSource = sourceIt != sources_.end();
    bool removedRegion = false;

    if (removedSource)
    {
        clearSourcePayloadLocked(sourceIt->second);
        invalidateSourceReaderLeaseLocked(sourceIt->second);
        sourceIt->second.pendingRemoval = true;
    }

    for (auto it = regions_.begin(); it != regions_.end();)
    {
        if (it->second.identity.audioSource == audioSource)
        {
            it = regions_.erase(it);
            removedRegion = true;
            continue;
        }

        ++it;
    }

    if (!removedSource && !removedRegion)
        return false;

    if (preferredRegion_.audioSource == audioSource)
        preferredRegion_.clear();

    reconcilePreferredRegionLocked();
    drainDeferredSourceCleanupLocked();
    return true;
}

void VST3AraSession::updatePreferredRegionLocked(const RegionIdentity& regionIdentity)
{
    if (regionIdentity.isValid())
        preferredRegion_ = regionIdentity;
}

void VST3AraSession::reconcilePreferredRegionLocked()
{
    if (preferredRegion_.isValid())
    {
        const auto* preferredSlot = findRegionSlot(preferredRegion_.playbackRegion);
        if (preferredSlot != nullptr
            && preferredSlot->identity == preferredRegion_
            && preferredSlot->isValid()
            && findSourceSlot(preferredRegion_.audioSource) != nullptr)
        {
            return;
        }
    }

    preferredRegion_.clear();

    for (const auto& [playbackRegion, regionSlot] : regions_)
    {
        juce::ignoreUnused(playbackRegion);

        if (!regionSlot.isValid())
            continue;

        if (findSourceSlot(regionSlot.identity.audioSource) == nullptr)
            continue;

        preferredRegion_ = regionSlot.identity;
        return;
    }
}

void VST3AraSession::markSnapshotDirtyLocked() noexcept
{
    pendingSnapshotPublication_ = true;
}

void VST3AraSession::setProcessor(OpenTuneAudioProcessor* processor) noexcept
{
    processor_.store(processor, std::memory_order_release);
}

} // namespace OpenTune
