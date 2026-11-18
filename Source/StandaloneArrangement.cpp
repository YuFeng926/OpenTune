#include "StandaloneArrangement.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "Render/ContentRenderService.h"
#include "Utils/AppLogger.h"
#include "Utils/TimeCoordinate.h"

namespace OpenTune {

namespace {

juce::Colour makeDefaultTrackColour()
{
    auto& random = juce::Random::getSystemRandom();
    // Generate a vibrant pastel color: random hue, moderate saturation, bright
    float hue = random.nextFloat();
    float saturation = 0.5f + random.nextFloat() * 0.3f;  // 0.5-0.8
    float brightness = 0.7f + random.nextFloat() * 0.3f;  // 0.7-1.0
    return juce::Colour::fromHSV(hue, saturation, brightness, 1.0f);
}

// 连续切片共享同一个固定 D 的来源身份条件。identity 与 nonidentity 永不混接；
// nonidentity 只有同一内容、同一 TimeGrid 快照和同一来源窗口才属于同一输出域。
bool sameContinuityProvenance(const PlaybackReadSource& lhs,
                              const PlaybackReadSource& rhs) noexcept
{
    if (lhs.contentSnapshot == nullptr || rhs.contentSnapshot == nullptr
        || lhs.contentSnapshot->timeGrid == nullptr || rhs.contentSnapshot->timeGrid == nullptr)
        return false;

    if (!(lhs.nativeSource == rhs.nativeSource))
        return false;

    const bool lhsIdentity = lhs.contentSnapshot->timeGrid->isIdentity();
    const bool rhsIdentity = rhs.contentSnapshot->timeGrid->isIdentity();
    if (lhsIdentity != rhsIdentity)
        return false;

    if (lhsIdentity) {
        return lhs.contentSnapshot->sourceWindow.isValid()
            && rhs.contentSnapshot->sourceWindow.isValid();
    }

    const auto& lhsWindow = lhs.contentSnapshot->sourceWindow;
    const auto& rhsWindow = rhs.contentSnapshot->sourceWindow;
    return lhsWindow.isValid()
        && rhsWindow.isValid()
        && lhs.contentKey == rhs.contentKey
        && lhs.contentSnapshot->timeGrid == rhs.contentSnapshot->timeGrid
        && lhsWindow.sourceStartSeconds == rhsWindow.sourceStartSeconds
        && lhsWindow.sourceEndSeconds == rhsWindow.sourceEndSeconds;
}

// 实际读取域在时间轴边界上严格相接，不用任何容差。
bool isExactContinuation(const StandaloneArrangement::PlaybackPlacement& previous,
                         const StandaloneArrangement::PlaybackPlacement& current)
{
    if (previous.timelineStartSeconds + previous.durationSeconds
        != current.timelineStartSeconds)
        return false;

    if (previous.source.contentSnapshot->timeGrid->isIdentity()) {
        const auto& previousWindow = previous.source.contentSnapshot->sourceWindow;
        const auto& currentWindow = current.source.contentSnapshot->sourceWindow;
        const double previousSourceEnd = previous.durationSeconds
                == previousWindow.durationSeconds() - previous.clipInSeconds
            ? previousWindow.sourceEndSeconds
            : previousWindow.sourceStartSeconds + previous.clipInSeconds + previous.durationSeconds;
        return previousSourceEnd == currentWindow.sourceStartSeconds + current.clipInSeconds;
    }

    return previous.outputAnchorSeconds + previous.durationSeconds
        == current.outputAnchorSeconds;
}

} // namespace

StandaloneArrangement::StandaloneArrangement()
{
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        auto& track = tracks_[static_cast<size_t>(trackId)];
        track.identity = nextTrackIdentity_++;
        track.name = defaultTrackName(trackId);
        track.colour = makeDefaultTrackColour();
    }

    const juce::ScopedWriteLock lock(stateLock_);
    publishPlaybackSnapshotLocked();
}

StandaloneArrangement::~StandaloneArrangement() = default;

juce::String StandaloneArrangement::defaultTrackName(int trackId) const
{
    return Loc::get(language_, Loc::Keys::kTrackPrefix) + juce::String(trackId + 1);
}

bool StandaloneArrangement::isDefaultTrackName(const juce::String& name, int trackId)
{
    for (int i = 0; i < static_cast<int>(Language::Count); ++i) {
        const auto language = static_cast<Language>(i);
        if (name == Loc::get(language, Loc::Keys::kTrackPrefix) + juce::String(trackId + 1))
            return true;
    }
    return false;
}

void StandaloneArrangement::setLanguage(Language language)
{
    const juce::ScopedWriteLock lock(stateLock_);
    if (language_ == language)
        return;

    language_ = language;
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        auto& name = tracks_[static_cast<size_t>(trackId)].name;
        if (isDefaultTrackName(name, trackId))
            name = defaultTrackName(trackId);
    }
}

StandaloneArrangement::PlaybackSnapshotRead StandaloneArrangement::loadPlaybackSnapshot() const
{
    return playbackSnapshot_.read();
}

void StandaloneArrangement::collectPlaybackSnapshots()
{
    const juce::ScopedWriteLock lock(stateLock_);
    playbackSnapshot_.collect();
}

void StandaloneArrangement::configurePlaybackSources(ContentRenderService* renderService,
                                                     double playbackSampleRate)
{
    const juce::ScopedWriteLock lock(stateLock_);
    if (playbackSourceService_ == renderService
        && playbackSampleRate_ == playbackSampleRate) {
        return;
    }

    playbackSourceService_ = renderService;
    playbackSampleRate_ = playbackSampleRate;
    publishPlaybackSnapshotLocked();
}

void StandaloneArrangement::refreshPlaybackSnapshot()
{
    const juce::ScopedWriteLock lock(stateLock_);
    publishPlaybackSnapshotLocked();
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

bool StandaloneArrangement::setTrackVolume(int trackId, double volume)
{
    if (!isValidTrackId(trackId)) {
        return false;
    }

    const juce::ScopedWriteLock lock(stateLock_);
    tracks_[static_cast<size_t>(trackId)].volume = static_cast<float>(
        std::clamp(volume, 0.0, static_cast<double>(kMaxTrackVolumeLinear)));
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

std::set<StandaloneArrangement::PlacementSelectionKey> StandaloneArrangement::getSelectedPlacements() const
{
    const juce::ScopedReadLock lock(stateLock_);
    return selectedPlacements_;
}

bool StandaloneArrangement::setSelectedPlacements(std::set<PlacementSelectionKey> placements)
{
    const juce::ScopedWriteLock lock(stateLock_);
    for (const auto& key : placements) {
        if (!isValidTrackId(key.trackId) || key.placementId == 0)
            return false;
        const int index = findPlacementIndexUnlocked(key.trackId, key.placementId);
        if (index < 0 || tracks_[static_cast<size_t>(key.trackId)].placements[static_cast<size_t>(index)].isRetired)
            return false;
    }
    selectedPlacements_ = std::move(placements);
    return true;
}

StandaloneArrangement::SelectionState StandaloneArrangement::getSelectionState() const
{
    const juce::ScopedReadLock lock(stateLock_);
    SelectionState state;
    state.activeTrackId = activeTrackId_;
    state.activeTrackIdentity = tracks_[static_cast<size_t>(activeTrackId_)].identity;
    for (const auto& key : selectedPlacements_) {
        auto stableKey = key;
        stableKey.trackIdentity = tracks_[static_cast<size_t>(key.trackId)].identity;
        state.placements.insert(stableKey);
    }
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        state.selectedPlacementIds[static_cast<size_t>(trackId)] =
            tracks_[static_cast<size_t>(trackId)].selectedPlacementId;
        state.selectedTrackIdentities[static_cast<size_t>(trackId)] =
            tracks_[static_cast<size_t>(trackId)].identity;
    }
    return state;
}

bool StandaloneArrangement::setSelectionState(const SelectionState& state)
{
    const juce::ScopedWriteLock lock(stateLock_);
    int activeTrack = state.activeTrackIdentity != 0 ? -1 : state.activeTrackId;
    if (state.activeTrackIdentity != 0) {
        for (int trackId = 0; trackId < kTrackCount; ++trackId)
            if (tracks_[static_cast<size_t>(trackId)].identity == state.activeTrackIdentity) {
                activeTrack = trackId;
                break;
            }
        if (isValidTrackId(state.activeTrackId)
            && state.selectedTrackIdentities[static_cast<size_t>(state.activeTrackId)]
                == tracks_[static_cast<size_t>(state.activeTrackId)].identity)
            activeTrack = state.activeTrackId;
    }
    if (!isValidTrackId(activeTrack))
        activeTrack = activeTrackId_;

    std::array<uint64_t, kTrackCount> selectedIds{};
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        const auto placementId = state.selectedPlacementIds[static_cast<size_t>(trackId)];
        if (placementId == 0)
            continue;
        const auto identity = state.selectedTrackIdentities[static_cast<size_t>(trackId)];
        int resolvedTrack = identity == 0 ? trackId : -1;
        if (identity != 0) {
            for (int current = 0; current < kTrackCount; ++current)
                if (tracks_[static_cast<size_t>(current)].identity == identity) {
                    resolvedTrack = current;
                    break;
                }
        }
        if (!isValidTrackId(resolvedTrack))
            continue;
        const int index = findPlacementIndexUnlocked(resolvedTrack, placementId);
        if (index >= 0 && !tracks_[static_cast<size_t>(resolvedTrack)]
                               .placements[static_cast<size_t>(index)].isRetired)
            selectedIds[static_cast<size_t>(resolvedTrack)] = placementId;
    }

    activeTrackId_ = activeTrack;
    selectedPlacements_.clear();
    for (const auto& key : state.placements) {
        int resolvedTrack = key.trackIdentity == 0 ? key.trackId : -1;
        if (key.trackIdentity != 0) {
            for (int trackId = 0; trackId < kTrackCount; ++trackId)
                if (tracks_[static_cast<size_t>(trackId)].identity == key.trackIdentity) {
                    resolvedTrack = trackId;
                    break;
                }
        }
        if (!isValidTrackId(resolvedTrack) || key.placementId == 0)
            continue;
        const int index = findPlacementIndexUnlocked(resolvedTrack, key.placementId);
        if (index >= 0 && !tracks_[static_cast<size_t>(resolvedTrack)]
                               .placements[static_cast<size_t>(index)].isRetired)
            selectedPlacements_.insert({resolvedTrack, key.placementId});
    }
    for (int trackId = 0; trackId < kTrackCount; ++trackId)
        tracks_[static_cast<size_t>(trackId)].selectedPlacementId = selectedIds[static_cast<size_t>(trackId)];
    return true;
}

void StandaloneArrangement::clear()
{
    const juce::ScopedWriteLock lock(stateLock_);
    activeTrackId_ = 0;
    visibleTrackCount_ = kDefaultVisibleTracks;
    nextPlacementId_ = 1;
    selectedPlacements_.clear();

    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        auto& track = tracks_[static_cast<size_t>(trackId)];
        track.identity = nextTrackIdentity_++;
        track.placements.clear();
        track.selectedPlacementId = 0;
        track.isMuted = false;
        track.isSolo = false;
        track.volume = 1.0f;
        track.name = defaultTrackName(trackId);
        track.colour = makeDefaultTrackColour();
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

    int activePlacementCount = 0;
    for (const auto& existing : track.placements)
        if (!existing.isRetired)
            ++activePlacementCount;
    insertIndex = juce::jlimit(0, activePlacementCount, insertIndex);

    int rawInsertIndex = 0;
    int activeIndex = 0;
    for (; rawInsertIndex < static_cast<int>(track.placements.size()); ++rawInsertIndex) {
        if (track.placements[static_cast<size_t>(rawInsertIndex)].isRetired)
            continue;
        if (activeIndex == insertIndex)
            break;
        ++activeIndex;
    }
    track.placements.insert(track.placements.begin() + rawInsertIndex, placement);
    if (track.selectedPlacementId == 0) {
        track.selectedPlacementId = placement.placementId;
    }
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::insertPlacements(int trackId, std::vector<Placement>& placements)
{
    if (!isValidTrackId(trackId) || placements.empty())
        return false;

    const juce::ScopedWriteLock lock(stateLock_);
    std::set<uint64_t> ids;
    for (const auto& placement : placements) {
        if (!placement.contentKey.isValid() || !std::isfinite(placement.timelineStartSeconds)
            || !std::isfinite(placement.durationSeconds) || placement.timelineStartSeconds < 0.0
            || placement.durationSeconds <= 0.0)
            return false;
        if (placement.placementId != 0
            && (placementIdExistsUnlocked(placement.placementId) || !ids.insert(placement.placementId).second))
            return false;
    }

    auto& track = tracks_[static_cast<size_t>(trackId)];
    track.placements.reserve(track.placements.size() + placements.size());
    for (auto& placement : placements) {
        if (placement.placementId == 0) {
            while (nextPlacementId_ == 0 || placementIdExistsUnlocked(nextPlacementId_))
                ++nextPlacementId_;
            placement.placementId = nextPlacementId_++;
        } else {
            nextPlacementId_ = juce::jmax(nextPlacementId_, placement.placementId + 1);
        }
        placement.isRetired = false;
        track.placements.push_back(placement);
    }
    if (track.selectedPlacementId == 0)
        track.selectedPlacementId = track.placements[track.placements.size() - placements.size()].placementId;
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
    selectedPlacements_.erase(PlacementSelectionKey{trackId, placementId});
    track.placements.erase(track.placements.begin() + deletedIndex);


    refreshSelectedPlacementUnlocked(trackId,
                                     selectIndexAfterErase(currentSelectedIndex,
                                                           deletedIndex,
                                                           static_cast<int>(track.placements.size())));
    publishPlaybackSnapshotLocked();
    return true;
}

StandaloneArrangement::PlacementMoveResult StandaloneArrangement::applyPlacementMoves(
    const std::vector<StandaloneArrangement::PlacementMove>& moves)
{
    if (moves.empty())
        return PlacementMoveResult::Unchanged;

    const juce::ScopedWriteLock lock(stateLock_);
    for (size_t i = 0; i < moves.size(); ++i) {
        const auto& move = moves[i];
        if (!isValidTrackId(move.sourceTrackId)
            || !isValidTrackId(move.targetTrackId)
            || move.placementId == 0
            || findPlacementIndexUnlocked(move.sourceTrackId, move.placementId) < 0) {
            return PlacementMoveResult::Rejected;
        }
        const int sourceIndex = findPlacementIndexUnlocked(move.sourceTrackId, move.placementId);
        if (tracks_[static_cast<size_t>(move.sourceTrackId)]
                .placements[static_cast<size_t>(sourceIndex)].isRetired) {
            return PlacementMoveResult::Rejected;
        }
        for (size_t j = 0; j < i; ++j) {
            if (moves[j].placementId == move.placementId)
                return PlacementMoveResult::Rejected;
        }
    }

    auto movedSelection = selectedPlacements_;
    for (const auto& move : moves) {
        const PlacementSelectionKey sourceKey{move.sourceTrackId, move.placementId};
        if (movedSelection.erase(sourceKey) != 0)
            movedSelection.insert({move.targetTrackId, move.placementId});
    }

    bool changed = false;
    for (const auto& move : moves) {
        const auto& placement = tracks_[static_cast<size_t>(move.sourceTrackId)]
            .placements[static_cast<size_t>(findPlacementIndexUnlocked(move.sourceTrackId, move.placementId))];
        if (move.sourceTrackId != move.targetTrackId
            || placement.timelineStartSeconds != std::max(0.0, move.newTimelineStartSeconds)) {
            changed = true;
            break;
        }
    }
    if (!changed)
        return PlacementMoveResult::Unchanged;

    std::vector<Placement> movedPlacements;
    movedPlacements.reserve(moves.size());
    for (const auto& move : moves) {
        const int sourceIndex = findPlacementIndexUnlocked(move.sourceTrackId, move.placementId);
        movedPlacements.push_back(tracks_[static_cast<size_t>(move.sourceTrackId)]
                                      .placements[static_cast<size_t>(sourceIndex)]);
    }

    for (const auto& move : moves) {
        if (move.sourceTrackId == move.targetTrackId)
            continue;
        auto& sourceTrack = tracks_[static_cast<size_t>(move.sourceTrackId)];
        const int sourceIndex = findPlacementIndexUnlocked(move.sourceTrackId, move.placementId);
        const int currentSelectedIndex = sourceTrack.selectedPlacementId == 0
            ? -1
            : findPlacementIndexUnlocked(move.sourceTrackId, sourceTrack.selectedPlacementId);
        sourceTrack.placements.erase(sourceTrack.placements.begin() + sourceIndex);
        refreshSelectedPlacementUnlocked(move.sourceTrackId,
                                         selectIndexAfterErase(currentSelectedIndex,
                                                               sourceIndex,
                                                               static_cast<int>(sourceTrack.placements.size())));
    }

    for (size_t i = 0; i < moves.size(); ++i) {
        const auto& move = moves[i];
        auto& placement = movedPlacements[i];
        placement.timelineStartSeconds = std::max(0.0, move.newTimelineStartSeconds);
        if (move.sourceTrackId == move.targetTrackId) {
            const int index = findPlacementIndexUnlocked(move.sourceTrackId, move.placementId);
            tracks_[static_cast<size_t>(move.sourceTrackId)].placements[static_cast<size_t>(index)] = std::move(placement);
        } else {
            auto& targetTrack = tracks_[static_cast<size_t>(move.targetTrackId)];
            targetTrack.placements.push_back(std::move(placement));
            targetTrack.selectedPlacementId = move.placementId;
            activeTrackId_ = move.targetTrackId;
        }
    }
    selectedPlacements_ = std::move(movedSelection);
    publishPlaybackSnapshotLocked();
    return PlacementMoveResult::Applied;
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
    constexpr double epsilonSeconds = 1.0 / TimeCoordinate::kRenderSampleRate;
    if (std::abs(placement.timelineStartSeconds - clampedTimelineStartSeconds) <= epsilonSeconds) {
        return true;
    }

    placement.timelineStartSeconds = clampedTimelineStartSeconds;
    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::setPlacementGain(int trackId, uint64_t placementId, float gain)
{
    if (!isValidTrackId(trackId) || placementId == 0 || !std::isfinite(gain)) {
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

    publishPlaybackSnapshotLocked();
    return true;
}

bool StandaloneArrangement::setPlacementTrimState(int trackId, uint64_t placementId,
                                                  double clipInSeconds, double durationSeconds,
                                                  double timelineStartSeconds,
                                                  double fadeInDuration, double fadeOutDuration)
{
    if (!isValidTrackId(trackId) || placementId == 0
        || !std::isfinite(clipInSeconds) || !std::isfinite(durationSeconds)
        || !std::isfinite(timelineStartSeconds) || !std::isfinite(fadeInDuration)
        || !std::isfinite(fadeOutDuration))
        return false;

    const juce::ScopedWriteLock lock(stateLock_);
    const int index = findPlacementIndexUnlocked(trackId, placementId);
    if (index < 0)
        return false;

    auto& placement = tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)];
    const double newClipIn = std::max(0.0, clipInSeconds);
    const double newDuration = std::max(0.01, durationSeconds);
    const double newTimelineStart = std::max(0.0, timelineStartSeconds);
    const double maxFade = newDuration * 0.9;
    const double newFadeIn = std::clamp(fadeInDuration, 0.0, maxFade);
    const double newFadeOut = std::clamp(fadeOutDuration, 0.0, maxFade);
    if (placement.clipInSeconds == newClipIn && placement.durationSeconds == newDuration
        && placement.timelineStartSeconds == newTimelineStart
        && placement.fadeInDuration == newFadeIn && placement.fadeOutDuration == newFadeOut)
        return true;

    placement.clipInSeconds = newClipIn;
    placement.durationSeconds = newDuration;
    placement.timelineStartSeconds = newTimelineStart;
    placement.fadeInDuration = newFadeIn;
    placement.fadeOutDuration = newFadeOut;
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

void StandaloneArrangement::buildPlaybackGeometry(std::vector<PlaybackPlacement>& placements,
                                                  double sampleRate)
{
    for (auto& placement : placements) {
        placement.sampleGeometry = PlaybackSampleGeometry::Placement{};
        placement.outputAnchorSeconds = 0.0;
        if (sampleRate <= 0.0 || !placement.hasPlayableSource())
            continue;

        const auto& content = *placement.source.contentSnapshot;
        const auto& grid = *content.timeGrid;
        const bool identity = grid.isIdentity();
        const double outputAnchorSeconds = grid.tauForward(placement.clipInSeconds);
        // identity 读取使用 native 来源网格；非 identity 使用 Stage2 输出域，
        // M 是内容本地输出锚点。
        const double sourceStartSeconds = identity
            ? content.sourceWindow.sourceStartSeconds + placement.clipInSeconds
            : outputAnchorSeconds;
        placement.outputAnchorSeconds = outputAnchorSeconds;
        placement.sampleGeometry = PlaybackSampleGeometry::makePlacement(
            sampleRate,
            placement.timelineStartSeconds,
            placement.timelineStartSeconds + placement.durationSeconds,
            sourceStartSeconds);
    }

    if (placements.size() < 2)
        return;

    std::vector<size_t> order(placements.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&placements](size_t lhs, size_t rhs) {
        const auto& a = placements[lhs];
        const auto& b = placements[rhs];
        if (a.timelineStartSeconds != b.timelineStartSeconds) {
            return a.timelineStartSeconds < b.timelineStartSeconds;
        }
        return a.durationSeconds < b.durationSeconds;
    });

    // 其他放置可能重叠在连续切片之间，所以反向查找真正的前驱：来源身份
    // 兼容且时间轴、实际读取域端点都严格相接。找不到前驱时保留独立投影。
    for (size_t i = 1; i < order.size(); ++i) {
        auto& current = placements[order[i]];
        if (!current.hasPlayableView())
            continue;

        for (size_t candidate = i; candidate-- > 0;) {
            const auto& predecessor = placements[order[candidate]];
            if (!predecessor.hasPlayableView()
                || predecessor.sampleGeometry.playbackRate != current.sampleGeometry.playbackRate
                || !sameContinuityProvenance(predecessor.source, current.source)
                || !isExactContinuation(predecessor, current)) {
                continue;
            }

            current.sampleGeometry = PlaybackSampleGeometry::makeContiguousSlice(
                predecessor.sampleGeometry,
                current.timelineStartSeconds,
                current.timelineStartSeconds + current.durationSeconds);
            break;
        }
    }
}

void StandaloneArrangement::publishPlaybackSnapshotLocked()
{
    playbackSnapshot_.collect();
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
            if (p.isRetired) {
                continue;
            }

            PlaybackPlacement projected;
            projected.contentKey = p.contentKey;
            projected.timelineStartSeconds = p.timelineStartSeconds;
            projected.durationSeconds = p.durationSeconds;
            projected.clipInSeconds = p.clipInSeconds;
            projected.gain = p.gain;
            projected.fadeInDuration = p.fadeInDuration;
            projected.fadeOutDuration = p.fadeOutDuration;

            // Freeze the immutable source view. Geometry is assigned below at
            // the current playback rate by the shared segment builder.
            if (playbackSourceService_ != nullptr) {
                PlaybackReadSource view;
                if (playbackSourceService_->getPlaybackReadSource(p.contentKey, view)
                    && view.hasAudio()
                    && view.contentSnapshot != nullptr
                    && view.contentSnapshot->timeGrid != nullptr) {
                    projected.source = std::move(view);
                }
            }

            publishedTrack.placements.push_back(std::move(projected));
        }
        buildPlaybackGeometry(publishedTrack.placements, playbackSampleRate_);
        snapshot->anySoloed = snapshot->anySoloed || sourceTrack.isSolo;
    }

    RealtimeSnapshot<PlaybackSnapshot>::Owner owner = std::move(snapshot);
    playbackSnapshot_.publish(std::move(owner));
}

StandaloneArrangement::TrackIdentity StandaloneArrangement::getTrackIdentity(int trackId) const
{
    if (!isValidTrackId(trackId))
        return 0;
    const juce::ScopedReadLock lock(stateLock_);
    return tracks_[static_cast<size_t>(trackId)].identity;
}

int StandaloneArrangement::findTrackIndexByIdentity(TrackIdentity identity) const
{
    if (identity == 0)
        return -1;
    const juce::ScopedReadLock lock(stateLock_);
    for (int trackId = 0; trackId < kTrackCount; ++trackId)
        if (tracks_[static_cast<size_t>(trackId)].identity == identity)
            return trackId;
    return -1;
}

bool StandaloneArrangement::hasPlacement(TrackIdentity identity, uint64_t placementId) const
{
    if (identity == 0 || placementId == 0)
        return false;
    const juce::ScopedReadLock lock(stateLock_);
    for (int trackId = 0; trackId < kTrackCount; ++trackId)
        if (tracks_[static_cast<size_t>(trackId)].identity == identity)
            return findPlacementIndexUnlocked(trackId, placementId) >= 0;
    return false;
}

bool StandaloneArrangement::hasPlacementGlobal(uint64_t placementId) const
{
    const juce::ScopedReadLock lock(stateLock_);
    int trackId = -1;
    size_t index = 0;
    return findPlacementByIdGlobalUnlocked(placementId, trackId, index);
}

bool StandaloneArrangement::hasActivePlacement(TrackIdentity identity, uint64_t placementId) const
{
    if (identity == 0 || placementId == 0)
        return false;
    const juce::ScopedReadLock lock(stateLock_);
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        if (tracks_[static_cast<size_t>(trackId)].identity != identity)
            continue;
        const int index = findPlacementIndexUnlocked(trackId, placementId);
        return index >= 0 && !tracks_[static_cast<size_t>(trackId)].placements[static_cast<size_t>(index)].isRetired;
    }
    return false;
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
    selectedPlacements_.erase(PlacementSelectionKey{trackId, placementId});
    auto& track = tracks_[static_cast<size_t>(trackId)];
    if (track.selectedPlacementId == placementId)
        track.selectedPlacementId = 0;
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

bool StandaloneArrangement::referencesContentActive(ContentKey contentKey) const
{
    if (!contentKey.isValid()) return false;
    const juce::ScopedReadLock lock(stateLock_);
    for (int trackId = 0; trackId < kTrackCount; ++trackId)
        for (const auto& placement : tracks_[static_cast<size_t>(trackId)].placements)
            if (!placement.isRetired && placement.contentKey == contentKey)
                return true;
    return false;
}

std::vector<StandaloneArrangement::RetiredPlacementEntry> StandaloneArrangement::getRetiredPlacements() const
{
    std::vector<RetiredPlacementEntry> result;
    const juce::ScopedReadLock lock(stateLock_);
    for (int trackId = 0; trackId < kTrackCount; ++trackId) {
        for (const auto& p : tracks_[static_cast<size_t>(trackId)].placements) {
            if (p.isRetired) {
                result.push_back({trackId, tracks_[static_cast<size_t>(trackId)].identity,
                                  p.placementId, p.contentKey});
            }
        }
    }
    return result;
}

// ============================================================================
// Placement lookup helpers
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

bool StandaloneArrangement::removeTrackAndShift(int trackId, int visibleCount)
{
    if (!isValidTrackId(trackId) || visibleCount <= 1 || trackId >= visibleCount)
        return false;

    const juce::ScopedWriteLock lock(stateLock_);

    std::set<PlacementSelectionKey> shiftedSelection;
    for (auto key : selectedPlacements_) {
        if (key.trackId == trackId)
            continue;
        if (key.trackId > trackId && key.trackId < visibleCount)
            --key.trackId;
        shiftedSelection.insert(key);
    }

    // Shift tracks [trackId+1, visibleCount) down by one
    for (int i = trackId; i < visibleCount - 1; ++i) {
        auto& dst = tracks_[static_cast<size_t>(i)];
        auto& src = tracks_[static_cast<size_t>(i + 1)];

        dst.identity = src.identity;
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
    cleared.identity = nextTrackIdentity_++;
    cleared.placements.clear();
    cleared.selectedPlacementId = 0;
    cleared.isMuted = false;
    cleared.isSolo = false;
    cleared.volume = 1.0f;
    cleared.name = defaultTrackName(lastSlot);
    cleared.colour = makeDefaultTrackColour();
    cleared.currentRmsDb.store(-100.0f, std::memory_order_relaxed);

    // Adjust active track if needed
    if (activeTrackId_ == trackId) {
        activeTrackId_ = juce::jlimit(0, visibleCount - 2, trackId);
    } else if (activeTrackId_ > trackId && activeTrackId_ < visibleCount) {
        --activeTrackId_;
    }
    selectedPlacements_ = std::move(shiftedSelection);

    publishPlaybackSnapshotLocked();
    return true;
}

void StandaloneArrangement::setVisibleTrackCount(int count)
{
    const int clamped = juce::jlimit(1, kTrackCount, count);
    const juce::ScopedWriteLock lock(stateLock_);
    visibleTrackCount_ = clamped;
}

int StandaloneArrangement::getVisibleTrackCount() const
{
    const juce::ScopedReadLock lock(stateLock_);
    return visibleTrackCount_;
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
