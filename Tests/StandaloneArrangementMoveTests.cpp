#include "../Source/StandaloneArrangement.h"
#include "../Source/Utils/UndoManager.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>

#if defined(OPENTUNE_ENABLE_CONTROL_API)
#include "../Source/Control/ControlUiBridge.h"
#include "../Source/Control/Ops/OpsRegistry.h"
#include "../Source/Control/Ops/TrackOps.h"
#include "../Source/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#endif

namespace {

using OpenTune::DomainKind;
using OpenTune::StandaloneArrangement;
using OpenTune::kMaxTrackVolumeLinear;

StandaloneArrangement::Placement makePlacement(uint64_t contentId, double start)
{
    StandaloneArrangement::Placement placement;
    placement.contentKey = {DomainKind::StandaloneClip, contentId, 0};
    placement.timelineStartSeconds = start;
    placement.durationSeconds = 2.0;
    placement.name = "test";
    return placement;
}

bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1.0e-9;
}

bool testAppliedMovePublishesOnce()
{
    StandaloneArrangement arrangement;
    auto first = makePlacement(1, 1.0);
    auto second = makePlacement(2, 3.0);
    if (!arrangement.insertPlacement(0, first) || !arrangement.insertPlacement(0, second))
        return false;

    const auto before = arrangement.loadPlaybackSnapshot()->epoch;
    const auto result = arrangement.applyPlacementMoves({
        {0, 1, first.placementId, 10.0},
        {0, 1, second.placementId, 12.0}
    });

    StandaloneArrangement::Placement movedFirst;
    StandaloneArrangement::Placement movedSecond;
    const auto after = arrangement.loadPlaybackSnapshot()->epoch;
    return result == StandaloneArrangement::PlacementMoveResult::Applied
        && after == before + 1
        && !arrangement.getPlacementById(0, first.placementId, movedFirst)
        && !arrangement.getPlacementById(0, second.placementId, movedSecond)
        && arrangement.getPlacementById(1, first.placementId, movedFirst)
        && arrangement.getPlacementById(1, second.placementId, movedSecond)
        && near(movedFirst.timelineStartSeconds, 10.0)
        && near(movedSecond.timelineStartSeconds, 12.0);
}

bool testRejectedMoveIsAtomic()
{
    StandaloneArrangement arrangement;
    auto placement = makePlacement(3, 2.0);
    if (!arrangement.insertPlacement(0, placement))
        return false;

    const auto before = arrangement.loadPlaybackSnapshot()->epoch;
    const auto result = arrangement.applyPlacementMoves({
        {0, 1, placement.placementId, 8.0},
        {0, 1, 999999, 9.0}
    });

    StandaloneArrangement::Placement current;
    const bool stillAtSource = arrangement.getPlacementById(0, placement.placementId, current);
    const double sourceStart = current.timelineStartSeconds;
    const bool absentAtTarget = !arrangement.getPlacementById(1, placement.placementId, current);
    return result == StandaloneArrangement::PlacementMoveResult::Rejected
        && arrangement.loadPlaybackSnapshot()->epoch == before
        && stillAtSource
        && absentAtTarget
        && near(sourceStart, 2.0);
}

bool testUnchangedMoveDoesNotPublish()
{
    StandaloneArrangement arrangement;
    auto placement = makePlacement(4, 2.0);
    if (!arrangement.insertPlacement(0, placement))
        return false;

    const auto before = arrangement.loadPlaybackSnapshot()->epoch;
    const auto result = arrangement.applyPlacementMoves({{0, 0, placement.placementId, 2.0}});
    return result == StandaloneArrangement::PlacementMoveResult::Unchanged
        && arrangement.loadPlaybackSnapshot()->epoch == before;
}

bool testTrackVolumeUsesSharedBounds()
{
    StandaloneArrangement arrangement;
    return arrangement.setTrackVolume(0, -1.0f)
        && near(arrangement.getTrackVolume(0), 0.0)
        && arrangement.setTrackVolume(0, 0.0f)
        && near(arrangement.getTrackVolume(0), 0.0)
        && arrangement.setTrackVolume(0, kMaxTrackVolumeLinear)
        && near(arrangement.getTrackVolume(0), kMaxTrackVolumeLinear)
        && arrangement.setTrackVolume(0, kMaxTrackVolumeLinear + 1.0f)
        && near(arrangement.getTrackVolume(0), kMaxTrackVolumeLinear)
        && arrangement.setTrackVolume(0, std::numeric_limits<double>::max())
        && near(arrangement.getTrackVolume(0), kMaxTrackVolumeLinear)
        && arrangement.setTrackVolume(0, std::nextafter(4.0, 0.0))
        && near(arrangement.getTrackVolume(0), kMaxTrackVolumeLinear);
}

bool testPlaybackBorrowRetainsOldViewAndCollectsLatest()
{
    StandaloneArrangement arrangement;
    auto held = arrangement.loadPlaybackSnapshot();
    const auto initialEpoch = held->epoch;
    const auto initialVolume = held->tracks[0].volume;
    for (int i = 1; i <= 12; ++i)
        arrangement.setTrackVolume(0, static_cast<double>(i) / 4.0);

    if (held->epoch != initialEpoch || held->tracks[0].volume != initialVolume)
        return false;
    held = {};
    arrangement.collectPlaybackSnapshots();
    const auto latest = arrangement.loadPlaybackSnapshot();
    return latest->epoch == initialEpoch + 12 && near(latest->tracks[0].volume, 3.0);
}

bool testSelectionSnapshotFollowsTrackIdentityAfterDelete()
{
    StandaloneArrangement arrangement;
    auto placement = makePlacement(5, 2.0);
    if (!arrangement.insertPlacement(2, placement))
        return false;

    const auto trackIdentity = arrangement.getTrackIdentity(2);
    auto selection = arrangement.getSelectionState();
    selection.activeTrackId = 2;
    selection.activeTrackIdentity = trackIdentity;
    selection.selectedPlacementIds[2] = placement.placementId;
    selection.selectedTrackIdentities[2] = trackIdentity;
    selection.placements.insert({2, placement.placementId, trackIdentity});
    if (!arrangement.setSelectionState(selection))
        return false;

    const auto beforeDelete = arrangement.getSelectionState();
    if (!arrangement.removeTrackAndShift(0, 3) || !arrangement.setSelectionState(beforeDelete))
        return false;

    const auto restored = arrangement.getSelectionState();
    return arrangement.getTrackIdentity(1) == trackIdentity
        && restored.activeTrackId == 1
        && restored.activeTrackIdentity == trackIdentity
        && restored.selectedPlacementIds[1] == placement.placementId
        && restored.placements.count({1, placement.placementId, trackIdentity}) == 1;
}

class HistoryProbeAction final : public OpenTune::UndoAction
{
public:
    HistoryProbeAction(uint64_t trackIdentity, uint64_t placementId = 0, OpenTune::ContentKey content = {},
                       bool exists = true)
        : trackIdentity_(trackIdentity), placementId_(placementId), content_(content), exists_(exists)
    {
    }

    void undo() override { ++undoCount; }
    void redo() override { ++redoCount; }
    juce::String getDescription() const override { return "History probe"; }
    bool retainsRetiredPlacement(uint64_t identity, uint64_t placementId) const override
    {
        return identity == trackIdentity_ && placementId == placementId_ && placementId_ != 0;
    }
    bool dependsOnTrackIdentity(uint64_t identity) const override { return identity == trackIdentity_; }
    bool dependsOnPlacement(uint64_t identity, uint64_t placementId) const override
    {
        return identity == trackIdentity_ && placementId == placementId_ && placementId_ != 0;
    }
    bool dependsOnContent(OpenTune::ContentKey key) const override { return content_ == key && key.isValid(); }
    bool dependenciesExist() const override { return exists_; }

    int undoCount = 0;
    int redoCount = 0;

private:
    uint64_t trackIdentity_;
    uint64_t placementId_;
    OpenTune::ContentKey content_;
    bool exists_;
};

bool testTrackDeletionPrunesHistoryByIdentityAndContent()
{
    StandaloneArrangement arrangement;
    const uint64_t deletedTrack = arrangement.getTrackIdentity(0);
    const uint64_t survivorTrack = arrangement.getTrackIdentity(2);
    if (!arrangement.removeTrackAndShift(0, 3) || arrangement.findTrackIndexByIdentity(survivorTrack) != 1)
        return false;

    OpenTune::UndoManager history;
    constexpr uint64_t placementId = 31;
    const OpenTune::ContentKey content{DomainKind::StandaloneClip, 41, 0};

    history.addAction(std::make_unique<HistoryProbeAction>(deletedTrack, placementId));
    history.addAction(std::make_unique<HistoryProbeAction>(survivorTrack, 0, content));
    history.addAction(std::make_unique<HistoryProbeAction>(survivorTrack));
    if (!history.retainsRetiredPlacement(deletedTrack, placementId))
        return false;
    history.undo();
    history.undo();

    const auto result = history.pruneForTrackDeletion(
        deletedTrack, {{deletedTrack, placementId}}, {content}, [](OpenTune::ContentKey) { return false; });
    return result.undo == 1 && result.redo == 1
        && history.size() == 1 && history.cursor() == 0
        && history.canRedo() && !history.canUndo()
        && !history.retainsRetiredPlacement(deletedTrack, placementId);
}

bool testContentHistorySurvivesWhilePlacementStillReferencesIt()
{
    OpenTune::UndoManager history;
    constexpr uint64_t deletedTrackIdentity = 10;
    constexpr uint64_t actionTrackIdentity = 20;
    const OpenTune::ContentKey content{DomainKind::StandaloneClip, 42, 0};
    history.addAction(std::make_unique<HistoryProbeAction>(actionTrackIdentity, 0, content));

    const auto result = history.pruneForTrackDeletion(
        deletedTrackIdentity, {}, {content}, [](OpenTune::ContentKey) { return true; });
    return result.undo == 0 && result.redo == 0 && history.size() == 1 && history.canUndo();
}

bool testContentReleasePrunesDependentHistoryAndAdjustsCursor()
{
    OpenTune::UndoManager history;
    const OpenTune::ContentKey content{DomainKind::StandaloneClip, 43, 0};
    history.addAction(std::make_unique<HistoryProbeAction>(20, 0, content));
    history.addAction(std::make_unique<HistoryProbeAction>(30));
    history.undo();

    const int removed = history.pruneForContentRelease(content);
    return removed == 1 && history.size() == 1 && history.cursor() == 0
        && !history.canUndo() && history.canRedo();
}

#if defined(OPENTUNE_ENABLE_CONTROL_API)
class TrackMixBridge final : public OpenTune::ControlUiBridge
{
public:
    explicit TrackMixBridge(StandaloneArrangement& arrangement) : arrangement_(arrangement) {}

    std::optional<BusyState> queryBusy() const override { return BusyState{}; }
    std::optional<int> visibleTrackCount() const override { return 1; }

    bool setTrackMix(int trackId, std::optional<bool>, std::optional<bool>, std::optional<double> volumeLinear,
                     std::optional<std::uint32_t>) override
    {
        lastVolume_ = volumeLinear;
        return !volumeLinear || arrangement_.setTrackVolume(trackId, *volumeLinear);
    }

    const std::optional<double>& lastVolume() const { return lastVolume_; }

private:
    StandaloneArrangement& arrangement_;
    std::optional<double> lastVolume_;
};

bool runTrackSet(const OpenTune::ControlMethod& method, OpenTune::ControlOpContext& context,
                 const char* field, const char* value, OpenTune::JsonValue& response)
{
    const std::string requestText = std::string("{\"tracks\":[{\"trackId\":0,\"") + field + "\":" + value + "}]}";
    const auto request = OpenTune::parseJson(requestText);
    if (!request.ok)
        return false;

    std::shared_ptr<const void> parsed;
    std::string why;
    if (!method.parse(request.value, parsed, why))
        return false;
    const auto result = method.run(context, parsed.get());
    if (!result.ok())
        return false;
    response = result.value();
    return true;
}

const OpenTune::JsonValue* clampedWarningData(const OpenTune::JsonValue& response)
{
    const auto* warnings = response.find("warnings");
    if (warnings == nullptr || !warnings->isArray() || warnings->size() != 1)
        return nullptr;
    const auto& warning = warnings->at(0);
    const auto* code = warning.find("code");
    return code != nullptr && code->isString() && code->text() == "CLAMPED" ? warning.find("data") : nullptr;
}

bool hasNoWarnings(const OpenTune::JsonValue& response)
{
    const auto* warnings = response.find("warnings");
    return warnings != nullptr && warnings->isArray() && warnings->size() == 0;
}

bool hasLinearClamp(const OpenTune::JsonValue& response, double requested, double applied)
{
    const auto* data = clampedWarningData(response);
    if (data == nullptr)
        return false;
    const auto* field = data->find("field");
    const auto* request = data->find("requested");
    const auto* actual = data->find("applied");
    return field != nullptr && field->isString() && field->text() == "volumeLinear"
        && request != nullptr && request->isNumber() && request->asDouble() == requested
        && actual != nullptr && actual->isNumber() && actual->asDouble() == applied;
}

bool testTrackSetVolumeWarnings()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    OpenTune::OpenTuneAudioProcessor processor;
    auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr)
        return false;
    TrackMixBridge bridge(*arrangement);
    OpenTune::ControlInstanceFacts facts;
    facts.instanceId = "0123456789abcdef0123456789abcdef";
    OpenTune::ControlOpContext context(processor, &bridge, facts);
    OpenTune::ControlMethodTable methods;
    OpenTune::registerTrackOps(methods);
    const auto* method = methods.find("track.set");
    if (method == nullptr)
        return false;

    OpenTune::JsonValue response;
    if (!runTrackSet(*method, context, "volumeLinear", "4", response)
        || !hasNoWarnings(response) || !bridge.lastVolume() || *bridge.lastVolume() != 4.0)
        return false;

    if (!runTrackSet(*method, context, "volumeLinear", "4.0000001", response)
        || !hasLinearClamp(response, 4.0, 4.0) || !bridge.lastVolume() || *bridge.lastVolume() != 4.0000001)
        return false;

    const double inRangeRoundedToFour = std::nextafter(4.0, 0.0);
    if (!runTrackSet(*method, context, "volumeLinear", "3.9999999999999996", response)
        || !hasNoWarnings(response) || !bridge.lastVolume() || *bridge.lastVolume() != inRangeRoundedToFour
        || arrangement->getTrackVolume(0) != 4.0f)
        return false;

    if (!runTrackSet(*method, context, "volumeLinear", "5", response)
        || !hasLinearClamp(response, 5.0, 4.0))
        return false;

    if (!runTrackSet(*method, context, "volumeDb", "20", response))
        return false;
    const auto* dbData = clampedWarningData(response);
    if (dbData == nullptr)
        return false;
    const auto* dbField = dbData->find("field");
    const auto* dbRequested = dbData->find("requested");
    const auto* dbApplied = dbData->find("applied");
    if (dbField == nullptr || !dbField->isString() || dbField->text() != "volumeDb"
        || dbRequested == nullptr || !dbRequested->isNumber() || dbRequested->asDouble() != 20.0
        || dbApplied == nullptr || !dbApplied->isNumber()
        || std::abs(dbApplied->asDouble() - 20.0 * std::log10(4.0)) > 1.0e-6)
        return false;

    const double largestFinite = std::numeric_limits<double>::max();
    return runTrackSet(*method, context, "volumeLinear", "1.7976931348623157e308", response)
        && hasLinearClamp(response, 9.0e15, 4.0)
        && bridge.lastVolume() && *bridge.lastVolume() == largestFinite
        && arrangement->getTrackVolume(0) == 4.0f;
}
#endif

} // namespace

int main()
{
    bool passed = testAppliedMovePublishesOnce()
        && testRejectedMoveIsAtomic()
        && testUnchangedMoveDoesNotPublish()
        && testTrackVolumeUsesSharedBounds()
        && testPlaybackBorrowRetainsOldViewAndCollectsLatest()
        && testSelectionSnapshotFollowsTrackIdentityAfterDelete()
        && testTrackDeletionPrunesHistoryByIdentityAndContent()
        && testContentHistorySurvivesWhilePlacementStillReferencesIt()
        && testContentReleasePrunesDependentHistoryAndAdjustsCursor();
#if defined(OPENTUNE_ENABLE_CONTROL_API)
    passed = passed && testTrackSetVolumeWarnings();
#endif
    if (!passed)
        std::cerr << "StandaloneArrangementMoveTests failed\n";
    return passed ? 0 : 1;
}
