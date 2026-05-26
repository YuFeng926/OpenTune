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

bool sourceWindowsMatch(const SourceWindow& lhs, const SourceWindow& rhs) noexcept
{
    return lhs.sourceId == rhs.sourceId
        && nearlyEqualSeconds(lhs.sourceStartSeconds, rhs.sourceStartSeconds)
        && nearlyEqualSeconds(lhs.sourceEndSeconds, rhs.sourceEndSeconds);
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
    view.sampleRate = sourceSlot.sampleRate;
    view.numChannels = sourceSlot.numChannels;
    view.numSamples = sourceSlot.numSamples;
    view.materializationRevision = regionSlot.appliedProjection.appliedMaterializationRevision;
    view.projectionRevision = regionSlot.projectionRevision;
    view.playbackStartSeconds = regionSlot.playbackStartSeconds;
    view.playbackEndSeconds = regionSlot.playbackEndSeconds;
    view.sourceWindow = regionSlot.sourceWindow;
    view.materializationDurationSeconds = regionSlot.materializationDurationSeconds;

    const bool hasAppliedBinding = regionSlot.appliedProjection.isValid()
        && regionSlot.appliedProjection.materializationId != 0;
    const bool appliedRegionMatches = regionSlot.appliedProjection.appliedRegionIdentity == regionSlot.identity;
    const bool appliedWindowMatches = sourceWindowsMatch(regionSlot.appliedProjection.appliedSourceWindow,
                                                         regionSlot.sourceWindow);

    if (hasAppliedBinding && appliedRegionMatches && appliedWindowMatches)
    {
        view.bindingState = VST3AraSession::BindingState::Renderable;
    }
    else if (hasAppliedBinding)
    {
        view.bindingState = VST3AraSession::BindingState::BoundNeedsRender;
    }
    else if (sourceSlot.readerLease == nullptr || !sourceSlot.sampleAccessEnabled)
    {
        view.bindingState = VST3AraSession::BindingState::HydratingSource;
    }
    else
    {
        view.bindingState = VST3AraSession::BindingState::Unbound;
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
    , birthWorkerThread_([this]() { birthWorkerLoop(); })
{
}

VST3AraSession::~VST3AraSession()
{
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        birthWorkerRunning_ = false;
        readyBirthWorkQueue_.clear();
        pendingBirths_.clear();
        for (auto& [audioSource, sourceSlot] : sources_)
        {
            juce::ignoreUnused(audioSource);
            sourceSlot.cancelRead = true;
        }
    }

    birthCv_.notify_all();

    if (birthWorkerThread_.joinable())
        birthWorkerThread_.join();
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

    if (regionNeedsMaterializationBirthLocked(regionSlot))
        upsertPendingBirthLocked(regionSlot.audioModificationPersistentId,
                                  regionSlot.sourceWindow, identity.audioSource);

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

    if (regionNeedsMaterializationBirthLocked(regionSlot))
        upsertPendingBirthLocked(regionSlot.audioModificationPersistentId,
                                  regionSlot.sourceWindow, audioSource);

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

        for (const auto& [pr, slot] : regions_) {
            juce::ignoreUnused(pr);
            if (slot.identity.audioSource == audioSource
                && regionNeedsMaterializationBirthLocked(slot)) {
                upsertPendingBirthLocked(slot.audioModificationPersistentId,
                                          slot.sourceWindow, audioSource);
            }
        }
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
    for (const auto& [pr, slot] : regions_) {
        juce::ignoreUnused(pr);
        if (slot.identity.audioSource == audioSource
            && regionNeedsMaterializationBirthLocked(slot)) {
            upsertPendingBirthLocked(slot.audioModificationPersistentId,
                                      slot.sourceWindow, audioSource);
        }
    }
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
        sourceSlot.readerLease = std::make_shared<ARA::PlugIn::HostAudioReader>(audioSource);
        ++sourceSlot.leaseGeneration;
    }

    for (const auto& [pr, slot] : regions_) {
        juce::ignoreUnused(pr);
        if (slot.identity.audioSource == audioSource
            && regionNeedsMaterializationBirthLocked(slot)) {
            upsertPendingBirthLocked(slot.audioModificationPersistentId,
                                      slot.sourceWindow, audioSource);
        }
    }
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

void VST3AraSession::clearSourcePayloadLocked(SourceSlot& sourceSlot) noexcept
{
    sourceSlot.readerLease.reset();
    sourceSlot.retiringReaderLease.reset();
}

bool VST3AraSession::regionNeedsMaterializationBirthLocked(const RegionSlot& regionSlot) const
{
    if (!regionSlot.isValid() || regionSlot.audioModificationPersistentId.isEmpty())
        return false;

    const auto bindingIt = materializationBindings_.find(regionSlot.audioModificationPersistentId);
    if (bindingIt == materializationBindings_.end())
        return true;

    const auto& binding = bindingIt->second;
    return binding.sourceId != regionSlot.sourceWindow.sourceId
        || !sourceWindowsMatch(binding.sourceWindow, regionSlot.sourceWindow);
}

void VST3AraSession::upsertPendingBirthLocked(const juce::String& persistentId,
                                               const SourceWindow& desiredWindow,
                                               juce::ARAAudioSource* audioSource)
{
    if (persistentId.isEmpty() || audioSource == nullptr)
        return;

    // Always record/update the PendingBirth regardless of source readiness.
    // The PendingBirth stores the latest desired revision even when sample
    // access is not yet available. Source readiness only determines whether
    // we push to the worker queue.
    auto& pending = pendingBirths_[persistentId];
    pending.audioModificationPersistentId = persistentId;
    pending.audioSource = audioSource;
    pending.desiredWindow = desiredWindow;
    pending.revision = nextBirthRevision_++;

    // Only push to worker queue if source sample access is actually ready.
    // Do NOT require readerLease here: the worker itself checks readerLease
    // before starting actual audio reads (tests may seed ready source without
    // a real HostAudioReader to avoid crashing on fake ARA pointers).
    auto* sourceSlot = findSourceSlot(audioSource);
    if (sourceSlot != nullptr && sourceSlot->sampleAccessEnabled
        && sourceSlot->numSamples > 0 && sourceSlot->numChannels > 0)
    {
        if (!pending.queuedForReadyWork)
        {
            readyBirthWorkQueue_.push_back(persistentId);
            pending.queuedForReadyWork = true;
            birthCv_.notify_one();
        }
    }
}

void VST3AraSession::invalidateSourceReaderLeaseLocked(SourceSlot& sourceSlot) noexcept
{
    ++sourceSlot.leaseGeneration;
    sourceSlot.sampleAccessEnabled = false;
    sourceSlot.cancelRead = true;

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

void VST3AraSession::birthWorkerLoop()
{
    while (true)
    {
        juce::ARAAudioSource* audioSource = nullptr;
        juce::String persistentId;

        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            birthCv_.wait(lock,
                          [this]()
                              {
                                  return !birthWorkerRunning_
                                  || !readyBirthWorkQueue_.empty();
                          });

            if (!birthWorkerRunning_ && readyBirthWorkQueue_.empty())
                return;

            persistentId = std::move(readyBirthWorkQueue_.front());
            readyBirthWorkQueue_.pop_front();

            // 查找 PendingBirth
            auto pendingIt = pendingBirths_.find(persistentId);
            if (pendingIt == pendingBirths_.end())
                continue; // stale entry

            pendingIt->second.queuedForReadyWork = false;

            PendingBirth pending = pendingIt->second; // 快照
            const uint64_t revision = pending.revision;
            audioSource = pending.audioSource;

            auto* sourceSlot = findSourceSlot(audioSource);
            if (sourceSlot == nullptr || sourceSlot->readerLease == nullptr
                || !sourceSlot->sampleAccessEnabled)
            {
                // Source not ready — keep pending, don't busy retry.
                // When source becomes ready, upsertPendingBirthLocked or
                // setProcessor picks up the pending and pushes to queue.
                continue;
            }

            // 查找有此 persistentId 且需要 birth 的 region
            RegionSlot* targetRegionSlot = nullptr;
            for (auto& [pr, slot] : regions_)
            {
                juce::ignoreUnused(pr);
                if (slot.identity.audioSource == audioSource
                    && slot.audioModificationPersistentId == persistentId
                    && regionNeedsMaterializationBirthLocked(slot))
                {
                    targetRegionSlot = &slot;
                    break;
                }
            }

            if (targetRegionSlot == nullptr)
            {
                // 不再需要 birth 了——清理 pending
                pendingBirths_.erase(persistentId);
                continue;
            }

            // 构建 birth request
            OpenTuneAudioProcessor::AraOriginalF0BirthRequest request;
            request.audioSource = audioSource;
            request.sourceId = sourceSlot->sourceId;
            request.readerLease = sourceSlot->readerLease;
            request.numChannels = sourceSlot->numChannels;
            request.numSamples = sourceSlot->numSamples;
            request.sourceSampleRate = sourceSlot->sampleRate;
            request.sourceWindow = targetRegionSlot->sourceWindow;
            request.playbackStartSeconds = targetRegionSlot->playbackStartSeconds;

            auto* processor = processor_.load(std::memory_order_acquire);

            lock.unlock();

            if (processor == nullptr)
            {
                lock.lock();
                continue;
            }

            auto birthResult = processor->birthAraMaterializationWithOriginalF0(std::move(request));

            lock.lock();

            // ====== 提交前检查 revision ======
            auto currentPendingIt = pendingBirths_.find(persistentId);

            // revision 检查：如果 pending 已不存在或 revision 不匹配（被新目标覆盖）→ 丢弃结果
            if (currentPendingIt == pendingBirths_.end()
                || currentPendingIt->second.revision != revision)
            {
                // stale result — 丢弃
                continue;
            }

            // 重新检查 region 是否仍然需要 birth
            targetRegionSlot = nullptr;
            for (auto& [pr, slot] : regions_)
            {
                juce::ignoreUnused(pr);
                if (slot.identity.audioSource == audioSource
                    && slot.audioModificationPersistentId == persistentId)
                {
                    targetRegionSlot = &slot;
                    break;
                }
            }

            if (targetRegionSlot == nullptr
                || !sourceWindowsMatch(targetRegionSlot->sourceWindow,
                                       currentPendingIt->second.desiredWindow)
                || !regionNeedsMaterializationBirthLocked(*targetRegionSlot))
            {
                pendingBirths_.erase(currentPendingIt);
                continue;
            }

            if (!birthResult.has_value() || birthResult->materializationId == 0)
            {
                pendingBirths_.erase(currentPendingIt);
                continue;
            }

            // 提交 binding
            AraMaterializationBinding binding;
            binding.audioModificationPersistentId = persistentId;
            binding.sourceId = birthResult->sourceId;
            binding.materializationId = birthResult->materializationId;
            binding.sourceWindow = targetRegionSlot->sourceWindow;
            binding.materializationRevision = birthResult->materializationRevision;
            binding.materializationDurationSeconds = birthResult->materializationDurationSeconds;

            upsertMaterializationBindingLocked(binding);
            applyBindingsToRegionSlotsLocked();
            markSnapshotDirtyLocked();

            // 清理 pending（birth 成功）
            pendingBirths_.erase(currentPendingIt);

            if (pendingSnapshotPublication_ && editingDepth_ == 0)
                publishSnapshotLocked();
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

    if (processor != nullptr)
    {
        // Wake pending births whose sources are now ready (processor may have
        // been null during early birth upserts).
        const std::lock_guard<std::mutex> lock(stateMutex_);
        for (auto& [persistentId, pending] : pendingBirths_)
        {
            juce::ignoreUnused(persistentId);
            auto* sourceSlot = findSourceSlot(pending.audioSource);
            if (sourceSlot != nullptr && sourceSlot->sampleAccessEnabled
                && sourceSlot->numSamples > 0 && sourceSlot->numChannels > 0)
            {
                if (!pending.queuedForReadyWork)
                {
                    readyBirthWorkQueue_.push_back(pending.audioModificationPersistentId);
                    pending.queuedForReadyWork = true;
                }
            }
        }
        if (!readyBirthWorkQueue_.empty())
            birthCv_.notify_one();
    }
}

} // namespace OpenTune
