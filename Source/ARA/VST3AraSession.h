#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Utils/SourceWindow.h"

namespace OpenTune {

class OpenTuneAudioProcessor;

#if defined(OPENTUNE_TEST_BUILD)
struct VST3AraSessionTestProbe;
#endif

class VST3AraSession {
public:
    struct RegionIdentity
    {
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        juce::ARAAudioSource* audioSource{nullptr};

        bool isValid() const noexcept
        {
            return playbackRegion != nullptr && audioSource != nullptr;
        }

        void clear() noexcept
        {
            playbackRegion = nullptr;
            audioSource = nullptr;
        }

        friend bool operator==(const RegionIdentity& lhs, const RegionIdentity& rhs) noexcept
        {
            return lhs.playbackRegion == rhs.playbackRegion && lhs.audioSource == rhs.audioSource;
        }

        friend bool operator!=(const RegionIdentity& lhs, const RegionIdentity& rhs) noexcept
        {
            return !(lhs == rhs);
        }
    };

    struct AppliedMaterializationProjection
    {
        uint64_t sourceId{0};
        uint64_t materializationId{0};
        uint64_t appliedMaterializationRevision{0};
        uint64_t appliedProjectionRevision{0};
        SourceWindow appliedSourceWindow;
        double   playbackStartSeconds{0.0};
        RegionIdentity appliedRegionIdentity;

        bool isValid() const noexcept { return sourceId != 0 && materializationId != 0; }
        void clear() noexcept { *this = AppliedMaterializationProjection{}; }
    };

    struct SourceSlot
    {
        SourceSlot() = default;

        SourceSlot(const SourceSlot& other)
            : audioSource(other.audioSource)
            , sourceId(other.sourceId)
            , name(other.name)
            , sampleRate(other.sampleRate)
            , numChannels(other.numChannels)
            , numSamples(other.numSamples)
            , copiedAudio(other.copiedAudio)
            , contentRevision(other.contentRevision)
            , hydratedContentRevision(other.hydratedContentRevision)
            , leaseGeneration(other.leaseGeneration)
            , sampleAccessEnabled(other.sampleAccessEnabled)
            , hostReadInFlight(false)
            , pendingLeaseReset(false)
            , pendingRemoval(false)
        {
        }

        SourceSlot& operator=(const SourceSlot& other)
        {
            if (this == &other)
                return *this;

            audioSource = other.audioSource;
            sourceId = other.sourceId;
            name = other.name;
            sampleRate = other.sampleRate;
            numChannels = other.numChannels;
            numSamples = other.numSamples;
            copiedAudio = other.copiedAudio;
            readerLease.reset();
            retiringReaderLease.reset();
            contentRevision = other.contentRevision;
            hydratedContentRevision = other.hydratedContentRevision;
            leaseGeneration = other.leaseGeneration;
            sampleAccessEnabled = other.sampleAccessEnabled;
            hostReadInFlight = false;
            queuedForHydration = false;
            readingFromHost = false;
            cancelRead = false;
            enablePendingHydration = false;
            pendingLeaseReset = false;
            pendingRemoval = false;
            return *this;
        }

        SourceSlot(SourceSlot&&) noexcept = default;
        SourceSlot& operator=(SourceSlot&&) noexcept = default;

        juce::ARAAudioSource* audioSource{nullptr};
        uint64_t sourceId{0};
        juce::String name;
        double sampleRate{0.0};
        int numChannels{0};
        int64_t numSamples{0};
        std::shared_ptr<juce::AudioBuffer<float>> copiedAudio;
        std::unique_ptr<ARA::PlugIn::HostAudioReader> readerLease;
        std::unique_ptr<ARA::PlugIn::HostAudioReader> retiringReaderLease;
        uint64_t contentRevision{0};
        uint64_t hydratedContentRevision{0};
        uint64_t leaseGeneration{0};
        bool sampleAccessEnabled{false};
        bool hostReadInFlight{false};
        bool queuedForHydration{false};
        bool readingFromHost{false};
        bool cancelRead{false};
        bool enablePendingHydration{false};
        bool pendingLeaseReset{false};
        bool pendingRemoval{false};

        bool hasAudio() const noexcept
        {
            return copiedAudio != nullptr && numSamples > 0 && hydratedContentRevision == contentRevision;
        }
    };

    struct RegionSlot
    {
        RegionIdentity identity;
        AppliedMaterializationProjection appliedProjection;
        double playbackStartSeconds{0.0};
        double playbackEndSeconds{0.0};
        SourceWindow sourceWindow;
        double materializationDurationSeconds{0.0};
        uint64_t projectionRevision{0};

        bool isValid() const noexcept
        {
            return identity.isValid() && playbackEndSeconds > playbackStartSeconds && sourceWindow.isValid();
        }
    };

    enum class BindingState : uint8_t {
        Unbound = 0,
        HydratingSource,
        BoundNeedsRender,
        Renderable
    };

    struct PublishedRegionView
    {
        RegionIdentity regionIdentity;
        uint64_t sourceId{0};
        AppliedMaterializationProjection appliedProjection;
        std::shared_ptr<const juce::AudioBuffer<float>> copiedAudio;
        double sampleRate{0.0};
        int numChannels{0};
        int64_t numSamples{0};
        uint64_t materializationRevision{0};
        uint64_t projectionRevision{0};
        double playbackStartSeconds{0.0};
        double playbackEndSeconds{0.0};
        SourceWindow sourceWindow;
        double materializationDurationSeconds{0.0};
        BindingState bindingState{BindingState::Unbound};

        bool isValid() const noexcept
        {
            return regionIdentity.isValid() && playbackEndSeconds > playbackStartSeconds && sourceWindow.isValid();
        }
    };

    struct PublishedSnapshot
    {
        uint64_t epoch{0};
        RegionIdentity preferredRegion;
        std::vector<PublishedRegionView> publishedRegions;

        const PublishedRegionView* findRegion(juce::ARAPlaybackRegion* playbackRegion) const noexcept
        {
            const auto it = std::find_if(publishedRegions.begin(), publishedRegions.end(),
                                         [playbackRegion](const PublishedRegionView& view)
                                         {
                                             return view.regionIdentity.playbackRegion == playbackRegion;
                                         });
            return it != publishedRegions.end() ? &(*it) : nullptr;
        }

        const PublishedRegionView* findPreferredRegion() const noexcept
        {
            const auto it = std::find_if(publishedRegions.begin(), publishedRegions.end(),
                                         [this](const PublishedRegionView& view)
                                         {
                                             return view.regionIdentity == preferredRegion;
                                         });
            return it != publishedRegions.end() ? &(*it) : nullptr;
        }
    };

    using SnapshotHandle = std::shared_ptr<const PublishedSnapshot>;

    VST3AraSession();
    ~VST3AraSession();

    static SnapshotHandle buildSnapshotForPublication(const std::vector<SourceSlot>& sourceSlots,
                                                      const std::vector<RegionSlot>& regionSlots,
                                                      const RegionIdentity& preferredRegion,
                                                      uint64_t epoch);
    static SnapshotHandle publishPendingSnapshot(const SnapshotHandle& currentSnapshot,
                                                 const std::vector<SourceSlot>& sourceSlots,
                                                 const std::vector<RegionSlot>& regionSlots,
                                                 const RegionIdentity& preferredRegion,
                                                 uint64_t epoch,
                                                 bool pendingSnapshotPublication);
    static RegionIdentity makeRegionIdentity(const juce::ARAPlaybackRegion* playbackRegion);

    SnapshotHandle loadSnapshot() const;

    void willBeginEditing();
    void didEndEditing();
    void didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion* playbackRegion);
    void didAddPlaybackRegionToAudioModification(juce::ARAAudioModification* audioModification,
                                                 juce::ARAPlaybackRegion* playbackRegion);
    void didUpdateAudioSourceProperties(juce::ARAAudioSource* audioSource);
    void doUpdateAudioSourceContent(juce::ARAAudioSource* audioSource,
                                    juce::ARAContentUpdateScopes scopeFlags);
    void willEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                            bool enable);
    void didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                           bool enable);
    void willRemovePlaybackRegionFromAudioModification(juce::ARAPlaybackRegion* playbackRegion);
    void willDestroyAudioSource(juce::ARAAudioSource* audioSource);

    void bindPlaybackRegionToMaterialization(juce::ARAPlaybackRegion* playbackRegion,
                                     uint64_t materializationId,
                                     uint64_t materializationRevision,
                                     uint64_t projectionRevision,
                                     SourceWindow sourceWindow,
                                     double materializationDurationSeconds,
                                     double playbackStartSeconds);
    void updatePlaybackRegionMaterializationRevisions(juce::ARAPlaybackRegion* playbackRegion,
                                              uint64_t materializationRevision,
                                              uint64_t projectionRevision);
    void clearPlaybackRegionMaterialization(juce::ARAPlaybackRegion* playbackRegion);

    void setProcessor(OpenTuneAudioProcessor* processor) noexcept;

private:
#if defined(OPENTUNE_TEST_BUILD)
    friend struct VST3AraSessionTestProbe;
#endif

    using SourceMap = std::map<juce::ARAAudioSource*, SourceSlot>;
    using RegionMap = std::map<juce::ARAPlaybackRegion*, RegionSlot>;

    SourceSlot* findSourceSlot(juce::ARAAudioSource* audioSource);
    const SourceSlot* findSourceSlot(juce::ARAAudioSource* audioSource) const;
    RegionSlot* findRegionSlot(juce::ARAPlaybackRegion* playbackRegion);
    const RegionSlot* findRegionSlot(juce::ARAPlaybackRegion* playbackRegion) const;
    SourceSlot& ensureSourceSlot(juce::ARAAudioSource* audioSource);
    RegionSlot& ensureRegionSlot(juce::ARAPlaybackRegion* playbackRegion,
                                 juce::ARAAudioSource* audioSource);

    SnapshotHandle buildPublishedSnapshotLocked() const;
    void publishSnapshotLocked();
    bool sourceNeedsHydrationLocked(const SourceSlot& sourceSlot) const noexcept;
    void clearSourcePayloadLocked(SourceSlot& sourceSlot) noexcept;
    void enqueueSourceHydrationLocked(juce::ARAAudioSource* audioSource);
    void invalidateSourceReaderLeaseLocked(SourceSlot& sourceSlot) noexcept;
    void drainDeferredSourceCleanupLocked();
    void hydrationWorkerLoop();
    void bumpSourceContentRevisionLocked(juce::ARAAudioSource* audioSource);
    void bumpRegionProjectionRevisionLocked(juce::ARAPlaybackRegion* playbackRegion);
    bool updateRegionProjectionFromPlaybackRegionLocked(RegionSlot& regionSlot,
                                                        const juce::ARAPlaybackRegion* playbackRegion);
    bool removePlaybackRegionFromStateLocked(juce::ARAPlaybackRegion* playbackRegion);
    bool removeAudioSourceFromStateLocked(juce::ARAAudioSource* audioSource);
    void updatePreferredRegionLocked(const RegionIdentity& regionIdentity);
    void reconcilePreferredRegionLocked();
    void markSnapshotDirtyLocked() noexcept;

    mutable std::mutex stateMutex_;
    SourceMap sources_;
    RegionMap regions_;
    RegionIdentity preferredRegion_;
    SnapshotHandle publishedSnapshot_;
    int editingDepth_{0};
    uint64_t nextSourceContentRevision_{1};
    uint64_t nextSourceId_{1};
    uint64_t nextRegionProjectionRevision_{1};
    uint64_t nextPublishedEpoch_{1};
    bool pendingSnapshotPublication_{false};
    std::deque<juce::ARAAudioSource*> hydrationQueue_;
    std::condition_variable hydrationCv_;
    std::thread hydrationWorkerThread_;
    bool hydrationWorkerRunning_{true};

    std::atomic<OpenTuneAudioProcessor*> processor_{nullptr};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VST3AraSession)
};

} // namespace OpenTune
