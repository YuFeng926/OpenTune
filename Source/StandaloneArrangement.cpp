#include "StandaloneArrangement.h"

#include <algorithm>
#include <cmath>

#include "Utils/AppLogger.h"

namespace OpenTune {

namespace {

juce::Colour makeDefaultTrackColour(int trackId)
{
    juce::ignoreUnused(trackId);
    auto& random = juce::Random::getSystemRandom();
    // Generate a vibrant pastel color: random hue, moderate saturation, bright
    float hue = random.nextFloat();
    float saturation = 0.5f + random.nextFloat() * 0.3f;  // 0.5-0.8
    float brightness = 0.7f + random.nextFloat() * 0.3f;  // 0.7-1.0
    return juce::Colour::fromHSV(hue, saturation, brightness, 1.0f);
}

} // namespace

StandaloneArrangement::StandaloneArrangement()
{
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        tracks_[static_cast<size_t>(trackId)].name = "Track " + juce::String(trackId + 1);
        tracks_[static_cast<size_t>(trackId)].colour = makeDefaultTrackColour(trackId);
    }

    const juce::ScopedWriteLock lock(stateLock_);
    publishPlaybackSnapshotLocked();
}

StandaloneArrangement::~StandaloneArrangement() = default;

StandaloneArrangement::PlaybackSnapshotHandle StandaloneArrangement::loadPlaybackSnapshot() const
{
    return std::atomic_load(&playbackSnapshot_);
}

int StandaloneArrangement::getActiveTrackId() const
{
    const juce::ScopedReadLock lock(stateLock_);
    return activeTrackId_;
}

bool StandaloneArrangement::setActiveTrack(int trackId)
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    activeTrackId_ = trackId;
    return true;
}

bool StandaloneArrangement::setTrackMuted(int trackId, bool muted)
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    tracks_[static_cast<size_t>(trackId)].isMuted = muted;
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::isTrackMuted(int trackId) const
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedReadLock lock(stateLock_);
    return tracks_[static_cast<size_t>(trackId)].isMuted;
}

bool StandaloneArrangement::setTrackSolo(int trackId, bool solo)
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    tracks_[static_cast<size_t>(trackId)].isSolo = solo;
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::isTrackSolo(int trackId) const
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedReadLock lock(stateLock_);
    return tracks_[static_cast<size_t>(trackId)].isSolo;
}

bool StandaloneArrangement::setTrackVolume(int trackId, float volume)
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    tracks_[static_cast<size_t>(trackId)].volume = std::max(0.0f, volume);
    publishPlaybackSnapshotLocked();
    return true;
}

float StandaloneArrangement::getTrackVolume(int trackId) const
{
    if (!isValidTrackId(trackId)) {
        return 1.0f;
    }

    const juce::ScopedReadLock lock(stateLock_);
    return tracks_[static_cast<size_t>(trackId)].volume;
}

float StandaloneArrangement::getTrackRmsDb(int trackId) const
{
    if (!isValidTrackId(trackId)) {
        return -100.0f;
    }

    return tracks_[static_cast<size_t>(trackId)].currentRmsDb.load(std::memory_order_relaxed);
}

bool StandaloneArrangement::setTrackRmsDb(int trackId, float rmsDb)
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    tracks_[static_cast<size_t>(trackId)].currentRmsDb.store(rmsDb, std::memory_order_relaxed);
    return true;
}

int StandaloneArrangement::getNumPlacements(int trackId) const
{
    if (!isValidTrackId(trackId)) {
        return 0;
    }

    const juce::ScopedReadLock lock(stateLock_);
    const auto& placements = tracks_[static_cast<size_t>(trackId)].placements;
    int count = 0;
    for (const auto& p : placements) {
        if (!p.isRetired) ++count;
    }
    return count;
}

uint64_t StandaloneArrangement::getPlacementId(int trackId, int placementIndex) const
{
    Placement placement;
    return getPlacementByIndex(trackId, placementIndex, placement) ? placement.placementId : 0;
}

int StandaloneArrangement::findPlacementIndexById(int trackId, uint64_t placementId) const
{
    if (!isValidTrackId(trackId) || placementId == 0) {
        return -1;
    }

    const juce::ScopedReadLock lock(stateLock_);
    const auto& placements = tracks_[static_cast<size_t>(trackId)].placements;
    int activeIndex = 0;
    for (const auto& p : placements) {
        if (!p.isRetired) {
            if (p.placementId == placementId) {
                return activeIndex;
            }
            ++activeIndex;
        }
    }
    return -1;
}

bool StandaloneArrangement::getPlacementByIndex(int trackId, int placementIndex, Placement& out) const
{
    out = Placement{};
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedReadLock lock(stateLock_);
    const auto& placements = tracks_[static_cast<size_t>(trackId)].placements;
    int activeIndex = 0;
    for (const auto& p : placements) {
        if (!p.isRetired) {
            if (activeIndex == placementIndex) {
                out = p;
                return true;
            }
            ++activeIndex;
        }
    }
    return false;
}

bool StandaloneArrangement::getPlacementById(int trackId, uint64_t placementId, Placement& out) const
{
    out = Placement{};
    if (!isValidTrackId(trackId) || placementId == 0) {
        return false;
    }

    const juce::ScopedReadLock lock(stateLock_);
    const int index = findPlacementIndexUnlocked(trackId, placementId);
    if (index < 0) {
        return false;
    }

    const auto& placement = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)];
    if (placement.isRetired) {
        return false;
    }

    out = placement;
    return true;
}

uint64_t StandaloneArrangement::getSelectedPlacementId(int trackId) const
{
    if (!isValidTrackId(trackId)) {
        return 0;
    }

    const juce::ScopedReadLock lock(stateLock_);
    return tracks_[static_cast<size_t>(trackId)].selectedPlacementId;
}

int StandaloneArrangement::getSelectedPlacementIndex(int trackId) const
{
    if (!isValidTrackId(trackId)) {
        return -1;
    }

    const juce::ScopedReadLock lock(stateLock_);
    const auto& track = tracks_[static_cast<size_t>(trackId)];
    if (track.selectedPlacementId == 0) {
        return -1;
    }

    // Return active-only index
    const auto& placements = track.placements;
    int activeIndex = 0;
    for (const auto& p : placements) {
        if (!p.isRetired) {
            if (p.placementId == track.selectedPlacementId) {
                return activeIndex;
            }
            ++activeIndex;
        }
    }
    return -1;
}

bool StandaloneArrangement::selectPlacement(int trackId, uint64_t placementId)
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    auto& track = tracks_[static_cast<size_t>(trackId)];
    if (placementId == 0) {
        track.selectedPlacementId = 0;
        activeTrackId_ = trackId;
        return true;
    }

    if (findPlacementIndexUnlocked(trackId, placementId) < 0) {
        return false;
    }

    // Primary selection is exclusive: clear all other tracks' selectedPlacementId
    for (int t = 0; t < kTrackCount; ++t) {
        if (t != trackId)
            tracks_[static_cast<size_t>(t)].selectedPlacementId = 0;
    }
    track.selectedPlacementId = placementId;
    activeTrackId_ = trackId;
    return true;
}

bool StandaloneArrangement::setSelectedPlacementIndex(int trackId, int placementIndex)
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    auto& track = tracks_[static_cast<size_t>(trackId)];
    // placementIndex is an active-only index
    int activeIndex = 0;
    for (const auto& p : track.placements) {
        if (!p.isRetired) {
            if (activeIndex == placementIndex) {
                // Primary selection is exclusive: clear all other tracks
                for (int t = 0; t < kTrackCount; ++t) {
                    if (t != trackId)
                        tracks_[static_cast<size_t>(t)].selectedPlacementId = 0;
                }
                track.selectedPlacementId = p.placementId;
                activeTrackId_ = trackId;
                return true;
            }
            ++activeIndex;
        }
    }
    track.selectedPlacementId = 0;
    return true;
}

void StandaloneArrangement::clear()
{
    const juce::ScopedWriteLock lock(stateLock_);
    activeTrackId_ = 0;
    nextPlacementId_ = 1;

    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        auto& track = tracks_[static_cast<size_t>(trackId)];
        track.placements.clear();
        track.selectedPlacementId = 0;
        track.isMuted = false;
        track.isSolo = false;
        track.volume = 1.0f;
        track.name = "Track " + juce::String(trackId + 1);
        track.colour = makeDefaultTrackColour(trackId);
        track.currentRmsDb.store(-100.0f, std::memory_order_relaxed);
    }

    publishPlaybackSnapshotLocked();
}

void StandaloneArrangement::clearAllSelections()
{
    const juce::ScopedWriteLock lock(stateLock_);
    activeTrackId_ = 0;
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        tracks_[static_cast<size_t>(trackId)].selectedPlacementId = 0;
    }
}

bool StandaloneArrangement::insertPlacement(int trackId, Placement& placement)
{
    if (!isValidTrackId(trackId)) return false;
    const int rawEnd = static_cast<int>(tracks_[static_cast<size_t>(trackId)].placements.size());
    return insertPlacement(trackId, rawEnd, placement);
}

bool StandaloneArrangement::insertPlacement(int trackId, int insertIndex, Placement& placement)
{
    if (!isValidTrackId(trackId) || placement.durationSeconds <= 0.0) {
        return false;
    }

    // Content owner lifecycle is now managed by StandaloneContentRepository

    const juce::ScopedWriteLock lock(stateLock_);
    auto& track = tracks_[static_cast<size_t>(trackId)];

    if (placement.placementId == 0) {
        placement.placementId = nextPlacementId_++;
    } else {
        if (placementIdExistsUnlocked(placement.placementId)) {
            return false;
        }

        nextPlacementId_ = juce::jmax(nextPlacementId_, placement.placementId + 1);
    }

    insertIndex = juce::jlimit(0, static_cast<int>(track.placements.size()), insertIndex);
    track.placements.insert(track.placements.begin() + insertIndex, placement);
    if (track.selectedPlacementId == 0) {
        track.selectedPlacementId = placement.placementId;
    }
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::deletePlacementById(int trackId,
                                                uint64_t placementId,
                                                Placement* deletedOut,
                                                int* deletedIndexOut)
{
    if (!isValidTrackId(trackId) || placementId == 0) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    auto& track = tracks_[static_cast<size_t>(trackId)];
    const int deletedIndex = findPlacementIndexUnlocked(trackId, placementId);
    if (deletedIndex < 0) {
        return false;
    }

    if (deletedOut != nullptr) {
        *deletedOut = track.placements[static_cast<size_t>(deletedIndex)];
    }
    if (deletedIndexOut != nullptr) {
        *deletedIndexOut = deletedIndex;
    }

    const int currentSelectedIndex = track.selectedPlacementId == 0
        ? -1
        : findPlacementIndexUnlocked(trackId, track.selectedPlacementId);
    track.placements.erase(track.placements.begin() + deletedIndex);

    clearInboundReferencesToPlacementUnlocked(placementId);

    refreshSelectedPlacementUnlocked(trackId,
                                     selectIndexAfterErase(currentSelectedIndex,
                                                           deletedIndex,
                                                           static_cast<int>(track.placements.size())));
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::movePlacementToTrack(int sourceTrackId,
                                                 int targetTrackId,
                                                 uint64_t placementId,
                                                 double newTimelineStartSeconds)
{
    if (!isValidTrackId(sourceTrackId)
        || !isValidTrackId(targetTrackId)
        || sourceTrackId == targetTrackId
        || placementId == 0) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    auto& sourceTrack = tracks_[static_cast<size_t>(sourceTrackId)];
    auto& targetTrack = tracks_[static_cast<size_t>(targetTrackId)];
    const int sourceIndex = findPlacementIndexUnlocked(sourceTrackId, placementId);
    if (sourceIndex < 0) {
        return false;
    }

    const int currentSelectedIndex = sourceTrack.selectedPlacementId == 0
        ? -1
        : findPlacementIndexUnlocked(sourceTrackId, sourceTrack.selectedPlacementId);

    Placement movedPlacement = sourceTrack.placements[static_cast<size_t>(sourceIndex)];
    movedPlacement.timelineStartSeconds = std::max(0.0, newTimelineStartSeconds);
    ++movedPlacement.mappingRevision;

    sourceTrack.placements.erase(sourceTrack.placements.begin() + sourceIndex);
    refreshSelectedPlacementUnlocked(sourceTrackId,
                                     selectIndexAfterErase(currentSelectedIndex,
                                                           sourceIndex,
                                                           static_cast<int>(sourceTrack.placements.size())));

    targetTrack.placements.push_back(std::move(movedPlacement));
    checkOverlapAndClearReferenceUnlocked(targetTrackId, placementId);
    clearInvalidInboundReferencesToPlacementUnlocked(placementId);
    targetTrack.selectedPlacementId = placementId;
    activeTrackId_ = targetTrackId;
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::setPlacementTimelineStartSeconds(int trackId,
                                                             uint64_t placementId,
                                                             double timelineStartSeconds)
{
    if (!isValidTrackId(trackId) || placementId == 0) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    const int index = findPlacementIndexUnlocked(trackId, placementId);
    if (index < 0) {
        return false;
    }

    auto& placement = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)];
    const double clampedTimelineStartSeconds = std::max(0.0, timelineStartSeconds);
    constexpr double epsilonSeconds = 1.0 / 44100.0;
    if (std::abs(placement.timelineStartSeconds - clampedTimelineStartSeconds) <= epsilonSeconds) {
        return true;
    }

    placement.timelineStartSeconds = clampedTimelineStartSeconds;
    ++placement.mappingRevision;
    checkOverlapAndClearReferenceUnlocked(trackId, placementId);
    clearInvalidInboundReferencesToPlacementUnlocked(placementId);
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::setPlacementGain(int trackId, uint64_t placementId, float gain)
{
    if (!isValidTrackId(trackId) || placementId == 0) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    const int index = findPlacementIndexUnlocked(trackId, placementId);
    if (index < 0) {
        return false;
    }

    tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)].gain = std::max(0.0f, gain);
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::setPlacementTrim(int trackId, uint64_t placementId, double clipInSeconds, double durationSeconds)
{
    const juce::ScopedWriteLock lock(stateLock_);
    if (!isValidTrackId(trackId)) return false;
    const auto index = findPlacementIndexUnlocked(trackId, placementId);
    if (index < 0) return false;

    auto& placement = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)];
    if (clipInSeconds < 0.0) clipInSeconds = 0.0;
    if (durationSeconds < 0.01) durationSeconds = 0.01;

    placement.clipInSeconds = clipInSeconds;
    placement.durationSeconds = durationSeconds;

    // Re-clamp fade durations after trim — existing fades may exceed 90% of new duration
    {
        const double maxFade = placement.durationSeconds * 0.9;
        if (placement.fadeInDuration > maxFade) placement.fadeInDuration = maxFade;
        if (placement.fadeOutDuration > maxFade) placement.fadeOutDuration = maxFade;
    }

    ++placement.mappingRevision;
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::setPlacementFade(int trackId, uint64_t placementId, double fadeInDuration, double fadeOutDuration)
{
    const juce::ScopedWriteLock lock(stateLock_);
    if (!isValidTrackId(trackId)) return false;
    const auto index = findPlacementIndexUnlocked(trackId, placementId);
    if (index < 0) return false;

    auto& placement = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)];
    if (fadeInDuration < 0.0) fadeInDuration = 0.0;
    if (fadeOutDuration < 0.0) fadeOutDuration = 0.0;
    const double maxFade = placement.durationSeconds * 0.9;
    if (fadeInDuration > maxFade) fadeInDuration = maxFade;
    if (fadeOutDuration > maxFade) fadeOutDuration = maxFade;

    if (placement.fadeInDuration == fadeInDuration && placement.fadeOutDuration == fadeOutDuration)
        return true; // Idempotent: values unchanged, but operation succeeded

    placement.fadeInDuration = fadeInDuration;
    placement.fadeOutDuration = fadeOutDuration;
    ++placement.mappingRevision;
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::setPlacementTrimAndTimelineStart(int trackId,
                                                             uint64_t placementId,
                                                             double clipInSeconds,
                                                             double durationSeconds,
                                                             double timelineStartSeconds)
{
    if (!isValidTrackId(trackId) || placementId == 0) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    const int index = findPlacementIndexUnlocked(trackId, placementId);
    if (index < 0) {
        return false;
    }

    auto& placement = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)];

    // Clamp trim values
    if (clipInSeconds < 0.0) clipInSeconds = 0.0;
    if (durationSeconds < 0.01) durationSeconds = 0.01;

    const double clampedTimelineStartSeconds = std::max(0.0, timelineStartSeconds);

    // No-op gate: skip publish if all three values are unchanged (common with snap grid drag)
    if (placement.clipInSeconds == clipInSeconds
        && placement.durationSeconds == durationSeconds
        && placement.timelineStartSeconds == clampedTimelineStartSeconds) {
        return true;
    }

    placement.clipInSeconds = clipInSeconds;
    placement.durationSeconds = durationSeconds;

    // Re-clamp fades if they exceed 90% of new duration
    {
        const double maxFade = placement.durationSeconds * 0.9;
        if (placement.fadeInDuration > maxFade) placement.fadeInDuration = maxFade;
        if (placement.fadeOutDuration > maxFade) placement.fadeOutDuration = maxFade;
    }

    placement.timelineStartSeconds = clampedTimelineStartSeconds;

    ++placement.mappingRevision;
    checkOverlapAndClearReferenceUnlocked(trackId, placementId);
    clearInvalidInboundReferencesToPlacementUnlocked(placementId);
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::isValidTrackId(int trackId) noexcept
{
    return trackId >= 0 && trackId < kTrackCount;
}

int StandaloneArrangement::selectIndexAfterErase(int currentSelectedIndex, int erasedIndex, int remainingCount)
{
    if (remainingCount <= 0) {
        return -1;
    }
    if (currentSelectedIndex < 0) {
        return -1;
    }
    if (currentSelectedIndex == erasedIndex) {
        return std::min(erasedIndex, remainingCount - 1);
    }
    if (currentSelectedIndex > erasedIndex) {
        return currentSelectedIndex - 1;
    }
    return currentSelectedIndex;
}

bool StandaloneArrangement::placementIdExistsUnlocked(uint64_t placementId) const
{
    if (placementId == 0) {
        return false;
    }

    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        if (findPlacementIndexUnlocked(trackId, placementId) >= 0) {
            return true;
        }
    }

    return false;
}

int StandaloneArrangement::findPlacementIndexUnlocked(int trackId, uint64_t placementId) const
{
    const auto& placements = tracks_[static_cast<size_t>(trackId)].placements;
    for (int index = 0; index < static_cast<int>(placements.size()); ++index) {
        if (placements[static_cast<size_t>(index)].placementId == placementId) {
            return index;
        }
    }

    return -1;
}

void StandaloneArrangement::refreshSelectedPlacementUnlocked(int trackId, int preferredIndex)
{
    auto& track = tracks_[static_cast<size_t>(trackId)];
    if (preferredIndex < 0 || preferredIndex >= static_cast<int>(track.placements.size())) {
        track.selectedPlacementId = 0;
        return;
    }

    track.selectedPlacementId = track.placements[static_cast<size_t>(preferredIndex)].placementId;
}

void StandaloneArrangement::publishPlaybackSnapshotLocked()
{
    auto snapshot = std::make_shared<PlaybackSnapshot>();
    snapshot->epoch = nextPlaybackEpoch_++;

    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        const auto& sourceTrack = tracks_[static_cast<size_t>(trackId)];
        auto& publishedTrack = snapshot->tracks[static_cast<size_t>(trackId)];
        publishedTrack.isMuted = sourceTrack.isMuted;
        publishedTrack.isSolo = sourceTrack.isSolo;
        publishedTrack.volume = sourceTrack.volume;
        publishedTrack.placements.reserve(sourceTrack.placements.size());
        for (const auto& p : sourceTrack.placements) {
            if (!p.isRetired) {
                publishedTrack.placements.push_back(PlaybackPlacement{
                    p.contentKey, p.timelineStartSeconds, p.durationSeconds,
                    p.clipInSeconds, p.gain, p.fadeInDuration, p.fadeOutDuration
                });
            }
        }
        snapshot->anySoloed = snapshot->anySoloed || sourceTrack.isSolo;
    }

    // Publish lock-free via atomic exchange.
    // Audio thread reads via atomic_load, never blocks.
    // Retire old snapshot into the writer-side list; sweep entries
    // whose use_count()==1 (only the list holds a reference) —
    // guaranteeing free/malloc never hits the RT path.
    auto old = std::atomic_exchange(&playbackSnapshot_,
                                    std::shared_ptr<const PlaybackSnapshot>(snapshot));
    if (old) {
        retiredSnapshots_.push_back(std::move(old));
    }
    retiredSnapshots_.erase(
        std::remove_if(retiredSnapshots_.begin(), retiredSnapshots_.end(),
            [](const auto& p) { return p.use_count() <= 1; }),
        retiredSnapshots_.end());
}

bool StandaloneArrangement::retirePlacement(int trackId, uint64_t placementId)
{
    if (!isValidTrackId(trackId) || placementId == 0) return false;
    const juce::ScopedWriteLock lock(stateLock_);
    const int index = findPlacementIndexUnlocked(trackId, placementId);
    if (index < 0) return false;
    auto& placement = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)];
    if (placement.isRetired) return false;
    placement.isRetired = true;
    clearInboundReferencesToPlacementUnlocked(placementId);
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::revivePlacement(int trackId, uint64_t placementId)
{
    if (!isValidTrackId(trackId) || placementId == 0) return false;
    const juce::ScopedWriteLock lock(stateLock_);
    const int index = findPlacementIndexUnlocked(trackId, placementId);
    if (index < 0) return false;
    auto& placement = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)];
    if (!placement.isRetired) return false;
    placement.isRetired = false;
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::referencesContentAnyState(ContentKey contentKey) const
{
    if (!contentKey.isValid()) return false;
    const juce::ScopedReadLock lock(stateLock_);
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        for (const auto& p : tracks_[static_cast<size_t>(trackId)].placements) {
            if (p.contentKey == contentKey) {
                return true;
            }
        }
    }
    return false;
}

std::vector<StandaloneArrangement::RetiredPlacementEntry> StandaloneArrangement::getRetiredPlacements() const
{
    std::vector<RetiredPlacementEntry> result;
    const juce::ScopedReadLock lock(stateLock_);
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        for (const auto& p : tracks_[static_cast<size_t>(trackId)].placements) {
            if (p.isRetired) {
                result.push_back({trackId, p.placementId, p.contentKey});
            }
        }
    }
    return result;
}

// ============================================================================
// Reference binding – private helpers
// ============================================================================

bool StandaloneArrangement::findPlacementByIdGlobalUnlocked(uint64_t placementId,
                                                            int& outTrackId,
                                                            size_t& outIndex) const
{
    if (placementId == 0) {
        return false;
    }

    for (int t = 0; t < kTrackCount; ++t) {
        const auto& placements = tracks_[static_cast<size_t>(t)].placements;
        for (size_t i = 0; i < placements.size(); ++i) {
            if (placements[i].placementId == placementId) {
                outTrackId = t;
                outIndex = i;
                return true;
            }
        }
    }

    return false;
}

bool StandaloneArrangement::isCyclicReferenceUnlocked(int trackId,
                                                       uint64_t targetPlacementId,
                                                       uint64_t candidateReferenceId) const
{
    if (targetPlacementId == 0 || candidateReferenceId == 0) {
        return false;
    }

    // 计算 clip 总数作为最大链长上限
    size_t totalPlacements = 0;
    for (int t = 0; t < kTrackCount; ++t) {
        totalPlacements += tracks_[static_cast<size_t>(t)].placements.size();
    }

    uint64_t currentId = candidateReferenceId;
    for (size_t step = 0; step < totalPlacements; ++step) {
        if (currentId == targetPlacementId) {
            return true; // 发现循环
        }

        int refTrackId = -1;
        size_t refIndex = 0;
        if (!findPlacementByIdGlobalUnlocked(currentId, refTrackId, refIndex)) {
            return false; // 链断裂
        }

        const auto& refPlacement = tracks_[static_cast<size_t>(refTrackId)].placements[refIndex];
        if (refPlacement.referencePlacementId == 0) {
            return false; // 链终结
        }

        currentId = refPlacement.referencePlacementId;
    }

    // 超链长保守视为非循环（实际不应到达）
    return false;
}

bool StandaloneArrangement::placementsOverlap(const Placement& target, const Placement& reference) noexcept
{
    const double overlap = std::min(target.timelineEndSeconds(), reference.timelineEndSeconds())
                         - std::max(target.timelineStartSeconds, reference.timelineStartSeconds);
    return overlap > 0.0;
}

bool StandaloneArrangement::clearReferenceBindingUnlocked(Placement& target) noexcept
{
    if (target.referencePlacementId == 0) {
        return false;
    }

    target.referencePlacementId = 0;
    ++target.referenceBindingRevision;
    return true;
}

void StandaloneArrangement::checkOverlapAndClearReferenceUnlocked(int trackId, uint64_t targetPlacementId)
{
    const int targetIndex = findPlacementIndexUnlocked(trackId, targetPlacementId);
    if (targetIndex < 0) {
        return;
    }

    auto& target = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(targetIndex)];
    if (target.referencePlacementId == 0) {
        return;
    }

    // 查找 reference placement
    int refTrackId = -1;
    size_t refIndex = 0;
    if (!findPlacementByIdGlobalUnlocked(target.referencePlacementId, refTrackId, refIndex)) {
        // reference 不存在，清空 binding
        AppLogger::info("checkOverlapAndClearReference: reference placement gone, clearing binding (targetId="
                        + juce::String(targetPlacementId)
                        + ", referenceId=" + juce::String(target.referencePlacementId) + ")");
        clearReferenceBindingUnlocked(target);
        return;
    }

    const auto& ref = tracks_[static_cast<size_t>(refTrackId)].placements[refIndex];
    if (ref.isRetired || !placementsOverlap(target, ref)) {
        AppLogger::info("checkOverlapAndClearReference: time overlap lost, clearing reference binding (targetId="
                        + juce::String(targetPlacementId)
                        + ", referenceId=" + juce::String(target.referencePlacementId) + ")");
        clearReferenceBindingUnlocked(target);
    }
}

void StandaloneArrangement::clearInboundReferencesToPlacementUnlocked(uint64_t referencePlacementId)
{
    if (referencePlacementId == 0) {
        return;
    }

    for (int t = 0; t < kTrackCount; ++t) {
        for (auto& placement : tracks_[static_cast<size_t>(t)].placements) {
            if (placement.referencePlacementId == referencePlacementId) {
                clearReferenceBindingUnlocked(placement);
            }
        }
    }
}

void StandaloneArrangement::clearInvalidInboundReferencesToPlacementUnlocked(uint64_t referencePlacementId)
{
    if (referencePlacementId == 0) {
        return;
    }

    int referenceTrackId = -1;
    size_t referenceIndex = 0;
    if (!findPlacementByIdGlobalUnlocked(referencePlacementId, referenceTrackId, referenceIndex)) {
        clearInboundReferencesToPlacementUnlocked(referencePlacementId);
        return;
    }

    const auto& reference = tracks_[static_cast<size_t>(referenceTrackId)].placements[referenceIndex];
    if (reference.isRetired) {
        clearInboundReferencesToPlacementUnlocked(referencePlacementId);
        return;
    }

    for (int t = 0; t < kTrackCount; ++t) {
        for (auto& target : tracks_[static_cast<size_t>(t)].placements) {
            if (target.referencePlacementId == referencePlacementId && !placementsOverlap(target, reference)) {
                clearReferenceBindingUnlocked(target);
            }
        }
    }
}

// ============================================================================
// Reference binding – public API
// ============================================================================

bool StandaloneArrangement::setPlacementReferencePlacement(int trackId,
                                                           uint64_t targetPlacementId,
                                                           uint64_t referencePlacementId)
{
    if (!isValidTrackId(trackId) || targetPlacementId == 0) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);

    const int targetIndex = findPlacementIndexUnlocked(trackId, targetPlacementId);
    if (targetIndex < 0) {
        AppLogger::warn("setPlacementReferencePlacement: target placement not found (trackId="
                        + juce::String(trackId) + ", targetId=" + juce::String(targetPlacementId) + ")");
        return false;
    }

    auto& target = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(targetIndex)];
    if (target.isRetired) {
        AppLogger::warn("setPlacementReferencePlacement: target placement is retired (targetId="
                        + juce::String(targetPlacementId) + ")");
        return false;
    }

    // 清空引用
    if (referencePlacementId == 0) {
        if (clearReferenceBindingUnlocked(target)) {
            publishPlaybackSnapshotLocked();
        }
        return true;
    }

    // 拒绝 self-reference
    if (targetPlacementId == referencePlacementId) {
        AppLogger::warn("setPlacementReferencePlacement: self-reference rejected (placementId="
                        + juce::String(targetPlacementId) + ")");
        return false;
    }

    // 验证 reference placement 存在（跨所有轨道）
    int refTrackId = -1;
    size_t refIndex = 0;
    if (!findPlacementByIdGlobalUnlocked(referencePlacementId, refTrackId, refIndex)) {
        AppLogger::warn("setPlacementReferencePlacement: reference placement not found (referenceId="
                        + juce::String(referencePlacementId) + ")");
        return false;
    }

    // 拒绝循环引用
    if (isCyclicReferenceUnlocked(trackId, targetPlacementId, referencePlacementId)) {
        AppLogger::warn("setPlacementReferencePlacement: cyclic reference rejected (targetId="
                        + juce::String(targetPlacementId)
                        + ", referenceId=" + juce::String(referencePlacementId) + ")");
        return false;
    }

    // 检查时间重叠
    const auto& ref = tracks_[static_cast<size_t>(refTrackId)].placements[refIndex];
    if (ref.isRetired) {
        AppLogger::warn("setPlacementReferencePlacement: reference placement is retired (referenceId="
                        + juce::String(referencePlacementId) + ")");
        return false;
    }

    if (!placementsOverlap(target, ref)) {
        AppLogger::warn("setPlacementReferencePlacement: no time overlap, binding rejected (targetId="
                        + juce::String(targetPlacementId)
                        + ", referenceId=" + juce::String(referencePlacementId) + ")");
        return false;
    }

    target.referencePlacementId = referencePlacementId;
    ++target.referenceBindingRevision;
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::clearPlacementReferencePlacement(int trackId, uint64_t targetPlacementId)
{
    if (!isValidTrackId(trackId) || targetPlacementId == 0) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);

    const int index = findPlacementIndexUnlocked(trackId, targetPlacementId);
    if (index < 0) {
        return false;
    }

    auto& placement = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)];
    if (placement.referencePlacementId == 0) {
        return true; // 已经无引用
    }

    clearReferenceBindingUnlocked(placement);
    publishPlaybackSnapshotLocked();
    return true;
}

uint64_t StandaloneArrangement::getPlacementReferencePlacement(int trackId, uint64_t targetPlacementId) const
{
    if (!isValidTrackId(trackId) || targetPlacementId == 0) {
        return 0;
    }

    const juce::ScopedReadLock lock(stateLock_);

    const int index = findPlacementIndexUnlocked(trackId, targetPlacementId);
    if (index < 0) {
        return 0;
    }

    return tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)].referencePlacementId;
}

bool StandaloneArrangement::isCyclicReference(int trackId,
                                               uint64_t targetPlacementId,
                                               uint64_t candidateReferenceId) const
{
    const juce::ScopedReadLock lock(stateLock_);
    return isCyclicReferenceUnlocked(trackId, targetPlacementId, candidateReferenceId);
}

bool StandaloneArrangement::removeTrackAndShift(int trackId, int visibleCount)
{
    if (!isValidTrackId(trackId) || visibleCount <= 1 || trackId >= visibleCount)
        return false;

    const juce::ScopedWriteLock lock(stateLock_);

    // Shift tracks [trackId+1, visibleCount) down by one
    for (int i = trackId; i < visibleCount - 1; ++i) {
        auto& dst = tracks_[static_cast<size_t>(i)];
        auto& src = tracks_[static_cast<size_t>(i + 1)];

        dst.placements = std::move(src.placements);
        dst.selectedPlacementId = src.selectedPlacementId;
        dst.isMuted = src.isMuted;
        dst.isSolo = src.isSolo;
        dst.volume = src.volume;
        dst.name = std::move(src.name);
        dst.colour = src.colour;
        dst.currentRmsDb.store(src.currentRmsDb.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    }

    // Clear the last visible slot (now vacated)
    const int lastSlot = visibleCount - 1;
    auto& cleared = tracks_[static_cast<size_t>(lastSlot)];
    cleared.placements.clear();
    cleared.selectedPlacementId = 0;
    cleared.isMuted = false;
    cleared.isSolo = false;
    cleared.volume = 1.0f;
    cleared.name = "Track " + juce::String(lastSlot + 1);
    cleared.colour = makeDefaultTrackColour(lastSlot);
    cleared.currentRmsDb.store(-100.0f, std::memory_order_relaxed);

    // Adjust active track if needed
    if (activeTrackId_ == trackId) {
        activeTrackId_ = juce::jlimit(0, visibleCount - 2, trackId);
    } else if (activeTrackId_ > trackId && activeTrackId_ < visibleCount) {
        --activeTrackId_;
    }

    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::setTrackColour(int trackId, juce::Colour colour)
{
    if (!isValidTrackId(trackId)) return false;
    const juce::ScopedWriteLock lock(stateLock_);
    tracks_[static_cast<size_t>(trackId)].colour = colour;
    return true;
}

juce::Colour StandaloneArrangement::getTrackColour(int trackId) const
{
    const juce::ScopedReadLock lock(stateLock_);
    if (!isValidTrackId(trackId)) return juce::Colours::grey;
    return tracks_[static_cast<size_t>(trackId)].colour;
}

} // namespace OpenTune
