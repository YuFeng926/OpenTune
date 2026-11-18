#include "../Source/StandaloneArrangement.h"
#include "../Source/PluginProcessor.h"
#include "../Source/Utils/PlacementActions.h"
#include "../Source/Utils/UndoManager.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#if defined(OPENTUNE_ENABLE_CONTROL_API)
#include "../Source/Control/ControlUiBridge.h"
#include "../Source/Control/Edit/ControlOriginAction.h"
#include "../Source/Control/Ops/ArrangementOps.h"
#include "../Source/Control/Ops/OpsRegistry.h"
#include "../Source/Control/Ops/ProjectReadOps.h"
#include "../Source/Control/Ops/TrackOps.h"
#include "../Source/Control/Ops/UndoOps.h"

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

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

struct ProcessorFixture
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    OpenTune::OpenTuneAudioProcessor processor;

    ProcessorFixture()
    {
        processor.prepareToPlay(44100.0, 512);
        processor.getStandaloneArrangement()->setVisibleTrackCount(4);
    }
};

bool insertTestPlacement(OpenTune::OpenTuneAudioProcessor& processor, int trackId, double start,
                         StandaloneArrangement::Placement& placement)
{
    auto* arrangement = processor.getStandaloneArrangement();
    auto* repository = processor.getStandaloneContentRepository();
    if (arrangement == nullptr || repository == nullptr)
        return false;

    const auto contentKey = repository->createClip();
    if (!contentKey.isValid())
        return false;
    placement = makePlacement(contentKey.objectId, start);
    placement.contentKey = contentKey;
    return arrangement->insertPlacement(trackId, placement);
}

bool commitGain(OpenTune::OpenTuneAudioProcessor& processor, int trackId, uint64_t placementId,
                double gain, const char* failureMessage)
{
    auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr)
        return expect(false, failureMessage);

    StandaloneArrangement::Placement placement;
    const auto plan = arrangement->getPlacementById(trackId, placementId, placement)
        ? OpenTune::planPlacementGain(placement, trackId, gain) : std::nullopt;
    if (!plan)
        return expect(false, failureMessage);

    std::vector<std::unique_ptr<OpenTune::UndoAction>> actions;
    if (!OpenTune::commitPlacementEdit(processor, *plan, actions) || actions.size() != 1)
        return expect(false, failureMessage);
    processor.getUndoManager().addAction(std::move(actions.front()));
    return true;
}

bool commitMoveBatch(OpenTune::OpenTuneAudioProcessor& processor,
                     const std::vector<OpenTune::MultiMovePlacementAction::Entry>& entries,
                     OpenTune::PlacementKey primaryAfter, const char* failureMessage)
{
    std::unique_ptr<OpenTune::UndoAction> action;
    if (!OpenTune::applyPlacementMoveBatch(processor, entries, primaryAfter, action) || action == nullptr)
        return expect(false, failureMessage);
    processor.getUndoManager().addAction(std::move(action));
    return true;
}

bool commitDeleteBatch(OpenTune::OpenTuneAudioProcessor& processor,
                       const std::vector<OpenTune::PlacementKey>& placements,
                       const char* failureMessage)
{
    auto deletion = OpenTune::commitPlacementDelete(processor, placements);
    if (deletion.undoAction == nullptr || deletion.outcomes.size() != placements.size())
        return expect(false, failureMessage);
    processor.getUndoManager().addAction(std::move(deletion.undoAction));
    return true;
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

bool testTrackDeletionReconcilesPlacementHistory()
{
    ProcessorFixture fixture;
    auto& processor = fixture.processor;
    auto* arrangement = processor.getStandaloneArrangement();
    auto* repository = processor.getStandaloneContentRepository();
    if (!expect(arrangement != nullptr && repository != nullptr, "track-delete fixture stores unavailable"))
        return false;

    StandaloneArrangement::Placement survivor, placementA, placementB;
    if (!expect(insertTestPlacement(processor, 2, 1.0, survivor), "could not create the surviving placement")
        || !expect(insertTestPlacement(processor, 0, 2.0, placementA), "could not create placement A")
        || !expect(insertTestPlacement(processor, 1, 3.0, placementB), "could not create placement B"))
        return false;

    auto& history = processor.getUndoManager();
    const auto survivorIdentity = arrangement->getTrackIdentity(2);
    if (!commitGain(processor, 2, survivor.placementId, 0.75,
                    "could not commit the surviving gain edit")
        || !commitGain(processor, 0, placementA.placementId, 0.25,
                       "could not commit placement A gain edit")
        || !commitDeleteBatch(processor, {{0, placementA.placementId}, {1, placementB.placementId}},
                              "could not commit the cross-track delete"))
        return false;

    const int visibleTrackCount = arrangement->getVisibleTrackCount();
    if (!expect(processor.deleteTrack(1, visibleTrackCount), "could not delete track B"))
        return false;
    processor.runReclaimSweepOnMessageThread();

    if (!expect(history.size() == 1 && history.cursor() == 1,
                "track deletion and sweep must leave only the valid surviving edit"))
        return false;
    const int survivorTrack = arrangement->findTrackIndexByIdentity(survivorIdentity);
    StandaloneArrangement::Placement currentSurvivor;
    if (!expect(survivorTrack == 1
                    && arrangement->getPlacementById(survivorTrack, survivor.placementId, currentSurvivor)
                    && near(currentSurvivor.gain, 0.75),
                "track deletion must preserve the shifted surviving placement and gain"))
        return false;
    const auto retiredPlacements = arrangement->getRetiredPlacements();
    const bool placementAStillRetired = std::any_of(retiredPlacements.begin(), retiredPlacements.end(),
        [&placementA](const auto& retired) { return retired.placementId == placementA.placementId; });
    if (!expect(!placementAStillRetired && repository->findClip(placementA.contentKey) == nullptr,
                "sweep must reclaim A after its cross-track delete history is clipped"))
        return false;

    auto* validatedTop = history.peekUndo();
    if (!expect(validatedTop != nullptr && history.peekUndo() == validatedTop
                    && history.size() == 1 && history.cursor() == 1,
                "post-sweep undo top must validate as the sole surviving edit"))
        return false;
    if (!expect(history.undo() == validatedTop,
                "undo must apply the exact action previously returned by peekUndo"))
        return false;

    StandaloneArrangement::Placement undoneSurvivor;
    return expect(history.size() == 1 && history.cursor() == 0
                      && arrangement->getPlacementById(survivorTrack, survivor.placementId, undoneSurvivor)
                      && near(undoneSurvivor.gain, 1.0),
                  "validated undo must restore the surviving placement gain");
}

bool testDeletedMoveSourceClipsGainHistory()
{
    ProcessorFixture fixture;
    auto& processor = fixture.processor;
    auto* arrangement = processor.getStandaloneArrangement();
    if (!expect(arrangement != nullptr, "move-source fixture arrangement unavailable"))
        return false;

    StandaloneArrangement::Placement placementP, placementQ;
    if (!expect(insertTestPlacement(processor, 0, 1.0, placementP), "could not create placement P")
        || !expect(insertTestPlacement(processor, 2, 2.0, placementQ), "could not create placement Q"))
        return false;

    auto& history = processor.getUndoManager();
    const auto trackBIdentity = arrangement->getTrackIdentity(1);
    const auto trackDIdentity = arrangement->getTrackIdentity(3);
    if (!commitGain(processor, 0, placementP.placementId, 0.5, "could not commit P gain edit")
        || !commitMoveBatch(processor,
                            {{0, 1, placementP.placementId, placementP.timelineStartSeconds, 5.0},
                             {2, 3, placementQ.placementId, placementQ.timelineStartSeconds, 7.0}},
                            {1, placementP.placementId}, "could not commit the two-placement move"))
        return false;

    if (!expect(processor.deleteTrack(2, arrangement->getVisibleTrackCount()),
                "could not delete the now-empty C track"))
        return false;
    processor.runReclaimSweepOnMessageThread();

    StandaloneArrangement::Placement currentP, currentQ;
    const int trackD = arrangement->findTrackIndexByIdentity(trackDIdentity);
    if (!expect(history.size() == 0 && history.cursor() == 0 && history.peekUndo() == nullptr,
                "invalid batch move and its now-mislocated prior gain must both leave history"))
        return false;
    return expect(arrangement->findTrackIndexByIdentity(trackBIdentity) == 1
                      && arrangement->getPlacementById(1, placementP.placementId, currentP)
                      && near(currentP.gain, 0.5)
                      && trackD == 2
                      && arrangement->getPlacementById(trackD, placementQ.placementId, currentQ),
                  "P must remain on B at gain 0.5 and Q must remain on shifted D");
}

bool testMoveHistorySurvivesSweepAcrossUndoRedoCursor()
{
    ProcessorFixture fixture;
    auto& processor = fixture.processor;
    auto* arrangement = processor.getStandaloneArrangement();
    if (!expect(arrangement != nullptr, "move replay fixture arrangement unavailable"))
        return false;

    StandaloneArrangement::Placement placement;
    if (!expect(insertTestPlacement(processor, 0, 1.0, placement), "could not create replay placement"))
        return false;

    auto& history = processor.getUndoManager();
    if (!commitGain(processor, 0, placement.placementId, 0.5, "could not commit replay gain edit")
        || !commitMoveBatch(processor, {{0, 1, placement.placementId, placement.timelineStartSeconds, 4.0}},
                            {1, placement.placementId}, "could not commit replay move"))
        return false;

    auto* moveAction = history.peekUndo();
    processor.runReclaimSweepOnMessageThread();
    if (!expect(history.size() == 2 && history.cursor() == 2 && history.peekUndo() == moveAction,
                "sweep must retain the prior A gain while the move places the clip on B"))
        return false;
    if (!expect(moveAction != nullptr && history.undo() == moveAction,
                "initial undo must return the move action"))
        return false;
    if (!expect(history.size() == 2 && history.cursor() == 1,
                "move undo must place the history cursor between gain and move"))
        return false;

    processor.runReclaimSweepOnMessageThread();
    auto* gainAction = history.peekUndo();
    if (!expect(history.size() == 2 && history.cursor() == 1 && gainAction != nullptr
                    && history.peekRedo() == moveAction,
                "sweep must preserve valid history on both sides of the cursor"))
        return false;
    StandaloneArrangement::Placement current;
    if (!expect(arrangement->getPlacementById(0, placement.placementId, current) && near(current.gain, 0.5),
                "sweep must preserve the placement at its intermediate A state"))
        return false;

    if (!expect(history.undo() == gainAction
                    && arrangement->getPlacementById(0, placement.placementId, current)
                    && near(current.gain, 1.0),
                "undo after sweep must apply the gain edit at A"))
        return false;
    if (!expect(history.peekRedo() == gainAction && history.redo() == gainAction
                    && arrangement->getPlacementById(0, placement.placementId, current)
                    && near(current.gain, 0.5),
                "redo after sweep must restore gain before replaying the move"))
        return false;
    return expect(history.peekRedo() == moveAction && history.redo() == moveAction
                      && history.size() == 2 && history.cursor() == 2
                      && arrangement->getPlacementById(1, placement.placementId, current)
                      && near(current.timelineStartSeconds, 4.0)
                      && near(current.gain, 0.5),
                  "move redo after sweep must place the gained clip on B");
}

bool testDeleteRevivalRetainsPriorGainHistory()
{
    ProcessorFixture fixture;
    auto& processor = fixture.processor;
    auto* arrangement = processor.getStandaloneArrangement();
    auto* repository = processor.getStandaloneContentRepository();
    if (!expect(arrangement != nullptr && repository != nullptr,
                "delete-revival fixture stores unavailable"))
        return false;

    StandaloneArrangement::Placement placement;
    if (!expect(insertTestPlacement(processor, 0, 1.0, placement), "could not create revival placement"))
        return false;

    auto& history = processor.getUndoManager();
    if (!commitGain(processor, 0, placement.placementId, 0.5, "could not commit pre-delete gain edit")
        || !commitDeleteBatch(processor, {{0, placement.placementId}}, "could not commit placement delete"))
        return false;

    processor.runReclaimSweepOnMessageThread();
    if (!expect(history.size() == 2 && history.cursor() == 2
                    && repository->isRetired(placement.contentKey),
                "sweep must retain delete and gain history while delete can revive the placement"))
        return false;

    auto* deleteAction = history.peekUndo();
    if (!expect(deleteAction != nullptr && history.undo() == deleteAction
                    && !repository->isRetired(placement.contentKey),
                "undo after sweep must revive the deleted content"))
        return false;
    StandaloneArrangement::Placement current;
    if (!expect(arrangement->getPlacementById(0, placement.placementId, current) && near(current.gain, 0.5),
                "delete undo must restore the placement before its gain undo"))
        return false;

    auto* gainAction = history.peekUndo();
    if (!expect(gainAction != nullptr && history.undo() == gainAction
                    && arrangement->getPlacementById(0, placement.placementId, current)
                    && near(current.gain, 1.0),
                "retained pre-delete gain must undo after placement revival"))
        return false;
    return expect(history.peekRedo() == gainAction && history.redo() == gainAction
                      && arrangement->getPlacementById(0, placement.placementId, current)
                      && near(current.gain, 0.5),
                  "retained pre-delete gain must redo after placement revival");
}

bool testSplitMergeHistorySurvivesBothReplayDirections()
{
    ProcessorFixture fixture;
    auto& processor = fixture.processor;
    auto* arrangement = processor.getStandaloneArrangement();
    auto* repository = processor.getStandaloneContentRepository();
    StandaloneArrangement::Placement original;
    if (!expect(insertTestPlacement(processor, 0, 1.0, original), "could not create split/merge placement"))
        return false;

    auto* content = repository->findClip(original.contentKey);
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 88200);
    audio->clear();
    content->content().sourceWindow = {111, {}, 0.0, 2.0};
    content->content().nativeSource = {audio, 44100.0, 0};
    content->applyAudioBuffer(audio, 44100.0);
    auto curve = std::make_shared<OpenTune::PitchCurve>();
    curve->setSampleRate(44100.0);
    curve->setHopSize(441);
    curve->setOriginalF0(std::vector<float>(200, 220.0f));
    content->applyOriginalF0(std::move(curve));

    auto& history = processor.getUndoManager();
    if (!commitGain(processor, 0, original.placementId, 0.5, "could not commit pre-split gain"))
        return false;
    const auto snapshot = processor.getContentSnapshot(original.contentKey);
    auto split = OpenTune::commitPlacementSplit(processor, 0, original.placementId,
        OpenTune::planPlacementSplit(original, *snapshot, 2.0));
    if (!expect(split.has_value(), "could not split the original placement"))
        return false;
    const auto splitOutcome = split->outcome;
    history.addAction(std::move(split->undoAction));

    StandaloneArrangement::Placement leading, trailing;
    if (!expect(arrangement->getPlacementById(0, splitOutcome.leadingPlacementId, leading)
                    && arrangement->getPlacementById(0, splitOutcome.trailingPlacementId, trailing),
                "split must create both child placements"))
        return false;
    auto merge = OpenTune::commitPlacementMerge(processor, OpenTune::planPlacementMerge(
        0, leading, *processor.getContentSnapshot(leading.contentKey),
        0, trailing, *processor.getContentSnapshot(trailing.contentKey)));
    if (!expect(merge.has_value(), "could not merge the split placements"))
        return false;
    const auto mergedId = merge->outcome.mergedPlacementId;
    history.addAction(std::move(merge->undoAction));
    if (!commitGain(processor, 0, mergedId, 0.75, "could not commit post-merge gain"))
        return false;

    processor.runReclaimSweepOnMessageThread();
    if (!expect(history.size() == 4 && history.cursor() == 4,
                "undo replay must retain gain/split/merge/gain history"))
        return false;
    for (int index = 0; index < 4; ++index)
        if (!expect(history.undo() != nullptr, "split/merge history must undo all four actions"))
            return false;
    StandaloneArrangement::Placement restored;
    if (!expect(arrangement->getNumPlacements(0) == 1
                    && arrangement->getPlacementById(0, original.placementId, restored)
                    && near(restored.gain, 1.0),
                "undo must revive the original placement and gain"))
        return false;

    processor.runReclaimSweepOnMessageThread();
    if (!expect(history.size() == 4 && history.cursor() == 0,
                "redo replay must retain both lineage changes and the final merged gain"))
        return false;
    for (int index = 0; index < 4; ++index)
        if (!expect(history.redo() != nullptr, "split/merge history must redo all four actions"))
            return false;
    return expect(arrangement->getNumPlacements(0) == 1
                      && arrangement->getPlacementById(0, mergedId, restored)
                      && near(restored.gain, 0.75),
                  "redo must restore the merged placement and its final gain");
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

class ArrangementUndoBridge final : public OpenTune::ControlUiBridge
{
public:
    explicit ArrangementUndoBridge(OpenTune::OpenTuneAudioProcessor& processor) : processor_(processor) {}

    std::optional<BusyState> queryBusy() const override { return BusyState{}; }
    std::optional<int> visibleTrackCount() const override
    {
        const auto* arrangement = processor_.getStandaloneArrangement();
        return arrangement != nullptr ? std::optional<int>(arrangement->getVisibleTrackCount()) : std::nullopt;
    }
    std::optional<ViewState> viewState() const override { return ViewState{"arrangement", 0}; }
    std::optional<std::vector<std::uint64_t>> selectedPlacementIds() const override
    {
        std::vector<std::uint64_t> ids;
        const auto* arrangement = processor_.getStandaloneArrangement();
        if (arrangement != nullptr) {
            const int trackId = arrangement->getActiveTrackId();
            if (trackId >= 0 && arrangement->getSelectedPlacementId(trackId) != 0)
                ids.push_back(arrangement->getSelectedPlacementId(trackId));
        }
        return ids;
    }
    void onStructureChanged() override { ++structureNotifications; }
    OpenTune::UndoAction* performUndoRedo(
        bool undo, const std::function<bool(OpenTune::UndoAction*)>& validateTop) override
    {
        auto& manager = processor_.getUndoManager();
        auto* top = undo ? manager.peekUndo() : manager.peekRedo();
        if (validateTop && !validateTop(top))
            return nullptr;
        if (top == nullptr)
            return nullptr;
        auto* applied = undo ? manager.undo() : manager.redo();
        if (applied != nullptr)
            onStructureChanged();
        return applied;
    }

    int structureNotifications = 0;

private:
    OpenTune::OpenTuneAudioProcessor& processor_;
};

bool runControlMethod(const OpenTune::ControlMethod& method, OpenTune::ControlOpContext& context,
                      const OpenTune::JsonValue& params, OpenTune::JsonValue& response)
{
    std::shared_ptr<const void> parsed;
    std::string why;
    if (!method.parse(params, parsed, why)) {
        std::cerr << "Control request parsing failed: " << why << '\n';
        return false;
    }
    const auto result = method.run(context, parsed.get());
    if (!result.ok()) {
        std::cerr << "Control request failed: " << result.error().fullMessage() << '\n';
        return false;
    }
    response = result.value();
    return true;
}

bool runControlMethod(const OpenTune::ControlMethodTable& methods, const char* name,
                      OpenTune::ControlOpContext& context, const OpenTune::JsonValue& params,
                      OpenTune::JsonValue& response)
{
    const auto* method = methods.find(name);
    return method != nullptr && runControlMethod(*method, context, params, response);
}

const OpenTune::JsonValue* historyTop(const OpenTune::JsonValue& response, const char* direction)
{
    const auto* history = response.find("history");
    return history != nullptr ? history->find(direction) : nullptr;
}

bool testControlDeleteUndoSurvivesSweep()
{
    ProcessorFixture fixture;
    auto& processor = fixture.processor;
    auto* arrangement = processor.getStandaloneArrangement();
    auto* repository = processor.getStandaloneContentRepository();
    if (arrangement == nullptr || repository == nullptr)
        return false;

    const auto contentKey = repository->createClip();
    auto* content = repository->findClip(contentKey);
    if (content == nullptr)
        return false;
    OpenTune::Note note;
    note.startTime = 0.25;
    note.endTime = 0.75;
    note.pitch = 440.0f;
    note.originalPitch = 440.0f;
    content->applyNotes({note});

    auto placement = makePlacement(contentKey.objectId, 1.0);
    if (!arrangement->insertPlacement(0, placement))
        return false;
    const auto placementId = placement.placementId;

    ArrangementUndoBridge bridge(processor);
    OpenTune::ControlInstanceFacts facts;
    facts.instanceId = "0123456789abcdef0123456789abcdef";
    auto history = std::make_shared<OpenTune::ControlHistory>();
    OpenTune::ControlOpContext context(processor, &bridge, facts, nullptr, {}, history);
    OpenTune::ControlMethodTable methods;
    OpenTune::registerProjectReadOps(methods);
    OpenTune::registerArrangementOps(methods);
    OpenTune::registerUndoOps(methods);

    OpenTune::JsonValue project;
    if (!runControlMethod(methods, "project.get", context, OpenTune::JsonValue::object(), project))
        return false;
    const auto* asOf = project.find("asOf");
    const auto* arrangementRevision = asOf != nullptr ? asOf->find("arrangementRevision") : nullptr;
    const auto* placements = project.find("placements");
    if (arrangementRevision == nullptr || !arrangementRevision->isString() || placements == nullptr
        || !placements->isArray() || placements->size() != 1)
        return expect(false, "project.get must expose one placement and an arrangement revision");
    const auto* placementHandle = placements->at(0).find("placementId");
    if (placementHandle == nullptr || !placementHandle->isString())
        return false;

    auto deleteParams = OpenTune::JsonValue::object();
    deleteParams.appendMember("expectedArrangementRevision", OpenTune::JsonValue::string(arrangementRevision->text()));
    auto deleteOp = OpenTune::JsonValue::object();
    deleteOp.appendMember("op", OpenTune::JsonValue::string("delete"));
    auto placementIds = OpenTune::JsonValue::array();
    placementIds.append(OpenTune::JsonValue::string(placementHandle->text()));
    deleteOp.appendMember("placementIds", std::move(placementIds));
    auto ops = OpenTune::JsonValue::array();
    ops.append(std::move(deleteOp));
    deleteParams.appendMember("ops", std::move(ops));
    deleteParams.appendMember("confirm", OpenTune::JsonValue::boolean(true));
    OpenTune::JsonValue deleted;
    if (!runControlMethod(methods, "arrangement.edit", context, deleteParams, deleted))
        return false;

    processor.runReclaimSweepOnMessageThread();
    const auto retiredPlacements = arrangement->getRetiredPlacements();
    const auto* retiredContent = repository->getRetiredContent(contentKey);
    if (arrangement->getNumPlacements(0) != 0 || !repository->isRetired(contentKey)
        || retiredContent == nullptr || retiredContent->content.notes.size() != 1
        || retiredContent->content.notes[0].pitch != 440.0f
        || bridge.structureNotifications != 1
        || std::none_of(retiredPlacements.begin(), retiredPlacements.end(), [placementId](const auto& retired) {
               return retired.placementId == placementId;
           }))
        return expect(false, "Control delete sweep must retain the placement and note payload");

    OpenTune::JsonValue historyResponse;
    if (!runControlMethod(methods, "edit.getHistory", context, OpenTune::JsonValue::object(), historyResponse))
        return false;
    const auto* undoTop = historyTop(historyResponse, "undoTop");
    const auto* unsafe = undoTop != nullptr ? undoTop->find("unsafe") : nullptr;
    const auto* undoId = undoTop != nullptr ? undoTop->find("undoId") : nullptr;
    if (unsafe == nullptr || !unsafe->isBoolean() || unsafe->asBoolean()
        || undoId == nullptr || !undoId->isString())
        return expect(false, "Control delete history must expose a safe undoId");
    const std::string actualUndoId = undoId->text();

    auto stepParams = OpenTune::JsonValue::object();
    stepParams.appendMember("expectedUndoId", OpenTune::JsonValue::string(actualUndoId));
    OpenTune::JsonValue undone;
    if (!runControlMethod(methods, "edit.undo", context, stepParams, undone))
        return false;
    OpenTune::StandaloneArrangement::Placement restored;
    const auto restoredContent = processor.getContentSnapshot(contentKey);
    const auto selection = bridge.selectedPlacementIds();
    if (!arrangement->getPlacementById(0, placementId, restored) || restored.isRetired
        || restored.contentKey != contentKey || restoredContent == nullptr || restoredContent->notes.size() != 1
        || restoredContent->notes[0].pitch != 440.0f || restoredContent->notes[0].startTime != 0.25
        || restoredContent->notes[0].endTime != 0.75 || repository->isRetired(contentKey)
        || arrangement->getSelectedPlacementId(0) != placementId || !selection || selection->size() != 1
        || selection->front() != placementId || bridge.structureNotifications != 2)
        return expect(false, "Control undo must restore placement, notes and selection");

    if (!runControlMethod(methods, "edit.getHistory", context, OpenTune::JsonValue::object(), historyResponse))
        return false;
    const auto* redoTop = historyTop(historyResponse, "redoTop");
    const auto* redoId = redoTop != nullptr ? redoTop->find("undoId") : nullptr;
    if (redoId == nullptr || !redoId->isString() || redoId->text() != actualUndoId)
        return expect(false, "Control redo must retain the original undoId");
    OpenTune::JsonValue redone;
    if (!runControlMethod(methods, "edit.redo", context, stepParams, redone))
        return false;
    if (bridge.structureNotifications != 3)
        return false;

    processor.runReclaimSweepOnMessageThread();
    const auto finalRetiredPlacements = arrangement->getRetiredPlacements();
    const auto* finalRetiredContent = repository->getRetiredContent(contentKey);
    return arrangement->getNumPlacements(0) == 0 && repository->isRetired(contentKey)
        && finalRetiredContent != nullptr && finalRetiredContent->content.notes.size() == 1
        && finalRetiredContent->content.notes[0].pitch == 440.0f
        && std::any_of(finalRetiredPlacements.begin(), finalRetiredPlacements.end(), [placementId](const auto& retired) {
               return retired.placementId == placementId;
           });
}

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
        && testTrackDeletionReconcilesPlacementHistory()
        && testDeletedMoveSourceClipsGainHistory()
        && testMoveHistorySurvivesSweepAcrossUndoRedoCursor()
        && testDeleteRevivalRetainsPriorGainHistory()
        && testSplitMergeHistorySurvivesBothReplayDirections();
#if defined(OPENTUNE_ENABLE_CONTROL_API)
    passed = passed && expect(testTrackSetVolumeWarnings(), "Control track volume regression failed");
    passed = passed && expect(testControlDeleteUndoSurvivesSweep(), "Control delete/undo regression failed");
#endif
    if (!passed)
        std::cerr << "StandaloneArrangementMoveTests failed\n";
    return passed ? 0 : 1;
}
