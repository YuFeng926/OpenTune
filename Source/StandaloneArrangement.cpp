#include "StandaloneArrangement.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

juce::Colour makeDefaultTrackColour(int trackId)
{
    return juce::Colour::fromHSV(static_cast<float>(trackId) * 0.3f, 0.6f, 0.8f, 1.0f);
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
    const juce::SpinLock::ScopedLockType lock(snapshotLock_);
    return playbackSnapshot_;
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

bool StandaloneArrangement::insertPlacement(int trackId, Placement& placement)
{
    if (!isValidTrackId(trackId)) return false;
    const int rawEnd = static_cast<int>(tracks_[static_cast<size_t>(trackId)].placements.size());
    return insertPlacement(trackId, rawEnd, placement);
}

bool StandaloneArrangement::insertPlacement(int trackId, int insertIndex, Placement& placement)
{
    if (!isValidTrackId(trackId) || placement.materializationId == 0 || placement.durationSeconds <= 0.0) {
        return false;
    }

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
    movedPlacement.colour = targetTrack.colour;
    ++movedPlacement.mappingRevision;

    sourceTrack.placements.erase(sourceTrack.placements.begin() + sourceIndex);
    refreshSelectedPlacementUnlocked(sourceTrackId,
                                     selectIndexAfterErase(currentSelectedIndex,
                                                           sourceIndex,
                                                           static_cast<int>(sourceTrack.placements.size())));

    targetTrack.placements.push_back(std::move(movedPlacement));
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
        for (const auto& p : sourceTrack.placements) {
            if (!p.isRetired) {
                publishedTrack.placements.push_back(p);
            }
        }
        snapshot->anySoloed = snapshot->anySoloed || sourceTrack.isSolo;
    }

    const juce::SpinLock::ScopedLockType lock(snapshotLock_);
    playbackSnapshot_ = std::move(snapshot);
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

bool StandaloneArrangement::referencesMaterializationAnyState(uint64_t materializationId) const
{
    if (materializationId == 0) return false;
    const juce::ScopedReadLock lock(stateLock_);
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        for (const auto& p : tracks_[static_cast<size_t>(trackId)].placements) {
            if (p.materializationId == materializationId) {
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
                result.push_back({trackId, p.placementId, p.materializationId});
            }
        }
    }
    return result;
}

} // namespace OpenTune
