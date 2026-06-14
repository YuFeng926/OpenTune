#include "PlacementActions.h"

namespace OpenTune {

// ============================================================================
// SplitPlacementAction
// ============================================================================

SplitPlacementAction::SplitPlacementAction(OpenTuneAudioProcessor& processor, const SplitOutcome& outcome)
    : processor_(processor)
    , trackId_(outcome.trackId)
    , originalPlacementId_(outcome.originalPlacementId)
    , originalContentKey_(outcome.originalContentKey)
    , leadingPlacementId_(outcome.leadingPlacementId)
    , leadingContentKey_(outcome.leadingContentKey)
    , trailingPlacementId_(outcome.trailingPlacementId)
    , trailingContentKey_(outcome.trailingContentKey)
{
}

void SplitPlacementAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* repo = processor_.getStandaloneContentRepository();

    // Retire the two new placements+contents
    arrangement->retirePlacement(trackId_, leadingPlacementId_);
    arrangement->retirePlacement(trackId_, trailingPlacementId_);
    repo->retireClip(leadingContentKey_);
    repo->retireClip(trailingContentKey_);

    // Revive the original
    repo->reviveClip(originalContentKey_);
    arrangement->revivePlacement(trackId_, originalPlacementId_);
}

void SplitPlacementAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* repo = processor_.getStandaloneContentRepository();

    // Retire the original
    arrangement->retirePlacement(trackId_, originalPlacementId_);
    repo->retireClip(originalContentKey_);

    // Revive the two new ones
    repo->reviveClip(leadingContentKey_);
    repo->reviveClip(trailingContentKey_);
    arrangement->revivePlacement(trackId_, leadingPlacementId_);
    arrangement->revivePlacement(trackId_, trailingPlacementId_);
}

// ============================================================================
// MergePlacementAction
// ============================================================================

MergePlacementAction::MergePlacementAction(OpenTuneAudioProcessor& processor, const MergeOutcome& outcome)
    : processor_(processor)
    , trackId_(outcome.trackId)
    , leadingPlacementId_(outcome.leadingPlacementId)
    , leadingContentKey_(outcome.leadingContentKey)
    , trailingPlacementId_(outcome.trailingPlacementId)
    , trailingContentKey_(outcome.trailingContentKey)
    , mergedPlacementId_(outcome.mergedPlacementId)
    , mergedContentKey_(outcome.mergedContentKey)
{
}

void MergePlacementAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* repo = processor_.getStandaloneContentRepository();

    // Retire merged
    arrangement->retirePlacement(trackId_, mergedPlacementId_);
    repo->retireClip(mergedContentKey_);

    // Revive originals
    repo->reviveClip(leadingContentKey_);
    repo->reviveClip(trailingContentKey_);
    arrangement->revivePlacement(trackId_, leadingPlacementId_);
    arrangement->revivePlacement(trackId_, trailingPlacementId_);
}

void MergePlacementAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* repo = processor_.getStandaloneContentRepository();

    // Retire originals
    arrangement->retirePlacement(trackId_, leadingPlacementId_);
    arrangement->retirePlacement(trackId_, trailingPlacementId_);
    repo->retireClip(leadingContentKey_);
    repo->retireClip(trailingContentKey_);

    // Revive merged
    repo->reviveClip(mergedContentKey_);
    arrangement->revivePlacement(trackId_, mergedPlacementId_);
}

// ============================================================================
// DeletePlacementAction
// ============================================================================

DeletePlacementAction::DeletePlacementAction(OpenTuneAudioProcessor& processor, const DeleteOutcome& outcome)
    : processor_(processor)
    , trackId_(outcome.trackId)
    , placementId_(outcome.placementId)
    , contentKey_(outcome.contentKey)
{
}

void DeletePlacementAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* repo = processor_.getStandaloneContentRepository();

    repo->reviveClip(contentKey_);
    arrangement->revivePlacement(trackId_, placementId_);
}

void DeletePlacementAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* repo = processor_.getStandaloneContentRepository();

    arrangement->retirePlacement(trackId_, placementId_);
    repo->retireClip(contentKey_);
}

// ============================================================================
// MovePlacementAction
// ============================================================================

MovePlacementAction::MovePlacementAction(OpenTuneAudioProcessor& processor,
                                         int sourceTrackId, int targetTrackId,
                                         uint64_t placementId,
                                         double oldStartSeconds, double newStartSeconds)
    : processor_(processor)
    , sourceTrackId_(sourceTrackId)
    , targetTrackId_(targetTrackId)
    , placementId_(placementId)
    , oldStartSeconds_(oldStartSeconds)
    , newStartSeconds_(newStartSeconds)
{
}

void MovePlacementAction::undo()
{
    processor_.movePlacementToTrack(targetTrackId_, sourceTrackId_, placementId_, oldStartSeconds_);
}

void MovePlacementAction::redo()
{
    processor_.movePlacementToTrack(sourceTrackId_, targetTrackId_, placementId_, newStartSeconds_);
}

// ============================================================================
// MultiMovePlacementAction
// ============================================================================

MultiMovePlacementAction::MultiMovePlacementAction(OpenTuneAudioProcessor& processor,
                                                   std::vector<Entry> entries)
    : processor_(processor)
    , entries_(std::move(entries))
{
}

void MultiMovePlacementAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    if (arrangement == nullptr) {
        return;
    }

    for (const auto& entry : entries_) {
        arrangement->setPlacementTimelineStartSeconds(entry.trackId,
                                                      entry.placementId,
                                                      entry.oldStartSeconds);
    }
}

void MultiMovePlacementAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    if (arrangement == nullptr) {
        return;
    }

    for (const auto& entry : entries_) {
        arrangement->setPlacementTimelineStartSeconds(entry.trackId,
                                                      entry.placementId,
                                                      entry.newStartSeconds);
    }
}

// ============================================================================
// GainChangeAction
// ============================================================================

GainChangeAction::GainChangeAction(OpenTuneAudioProcessor& processor,
                                   int trackId, uint64_t placementId,
                                   float oldGain, float newGain)
    : processor_(processor)
    , trackId_(trackId)
    , placementId_(placementId)
    , oldGain_(oldGain)
    , newGain_(newGain)
{
}

void GainChangeAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    arrangement->setPlacementGain(trackId_, placementId_, oldGain_);
}

void GainChangeAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    arrangement->setPlacementGain(trackId_, placementId_, newGain_);
}

// ============================================================================
// TrimPlacementAction
// ============================================================================

TrimPlacementAction::TrimPlacementAction(OpenTuneAudioProcessor& processor,
                                         int trackId, uint64_t placementId,
                                         double oldClipInSeconds, double oldDurationSeconds,
                                         double newClipInSeconds, double newDurationSeconds,
                                         double oldTimelineStart, double newTimelineStart)
    : processor_(processor), trackId_(trackId), placementId_(placementId)
    , oldClipInSeconds_(oldClipInSeconds), oldDurationSeconds_(oldDurationSeconds)
    , newClipInSeconds_(newClipInSeconds), newDurationSeconds_(newDurationSeconds)
    , oldTimelineStart_(oldTimelineStart), newTimelineStart_(newTimelineStart) {}

void TrimPlacementAction::undo()
{
    auto* arr = processor_.getStandaloneArrangement();
    arr->setPlacementTrim(trackId_, placementId_, oldClipInSeconds_, oldDurationSeconds_);
    arr->setPlacementTimelineStartSeconds(trackId_, placementId_, oldTimelineStart_);
}

void TrimPlacementAction::redo()
{
    auto* arr = processor_.getStandaloneArrangement();
    arr->setPlacementTrim(trackId_, placementId_, newClipInSeconds_, newDurationSeconds_);
    arr->setPlacementTimelineStartSeconds(trackId_, placementId_, newTimelineStart_);
}

// ============================================================================
// FadeChangeAction
// ============================================================================

FadeChangeAction::FadeChangeAction(OpenTuneAudioProcessor& processor,
                                   int trackId, uint64_t placementId,
                                   double oldFadeIn, double oldFadeOut,
                                   double newFadeIn, double newFadeOut)
    : processor_(processor), trackId_(trackId), placementId_(placementId)
    , oldFadeIn_(oldFadeIn), oldFadeOut_(oldFadeOut)
    , newFadeIn_(newFadeIn), newFadeOut_(newFadeOut) {}

void FadeChangeAction::undo()
{
    auto* arr = processor_.getStandaloneArrangement();
    arr->setPlacementFade(trackId_, placementId_, oldFadeIn_, oldFadeOut_);
}

void FadeChangeAction::redo()
{
    auto* arr = processor_.getStandaloneArrangement();
    arr->setPlacementFade(trackId_, placementId_, newFadeIn_, newFadeOut_);
}

} // namespace OpenTune
