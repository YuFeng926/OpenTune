#include "PlacementActions.h"

namespace OpenTune {

// ============================================================================
// SplitPlacementAction
// ============================================================================

SplitPlacementAction::SplitPlacementAction(OpenTuneAudioProcessor& processor, const SplitOutcome& outcome)
    : processor_(processor)
    , trackId_(outcome.trackId)
    , originalPlacementId_(outcome.originalPlacementId)
    , originalMaterializationId_(outcome.originalMaterializationId)
    , leadingPlacementId_(outcome.leadingPlacementId)
    , leadingMaterializationId_(outcome.leadingMaterializationId)
    , trailingPlacementId_(outcome.trailingPlacementId)
    , trailingMaterializationId_(outcome.trailingMaterializationId)
{
}

void SplitPlacementAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* store = processor_.getMaterializationStore();
    if (arrangement == nullptr || store == nullptr) return;

    // Retire the two new placements+materializations
    arrangement->retirePlacement(trackId_, leadingPlacementId_);
    arrangement->retirePlacement(trackId_, trailingPlacementId_);
    store->retireMaterialization(leadingMaterializationId_);
    store->retireMaterialization(trailingMaterializationId_);

    // Revive the original
    store->reviveMaterialization(originalMaterializationId_);
    arrangement->revivePlacement(trackId_, originalPlacementId_);
}

void SplitPlacementAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* store = processor_.getMaterializationStore();
    if (arrangement == nullptr || store == nullptr) return;

    // Retire the original
    arrangement->retirePlacement(trackId_, originalPlacementId_);
    store->retireMaterialization(originalMaterializationId_);

    // Revive the two new ones
    store->reviveMaterialization(leadingMaterializationId_);
    store->reviveMaterialization(trailingMaterializationId_);
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
    , leadingMaterializationId_(outcome.leadingMaterializationId)
    , trailingPlacementId_(outcome.trailingPlacementId)
    , trailingMaterializationId_(outcome.trailingMaterializationId)
    , mergedPlacementId_(outcome.mergedPlacementId)
    , mergedMaterializationId_(outcome.mergedMaterializationId)
{
}

void MergePlacementAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* store = processor_.getMaterializationStore();
    if (arrangement == nullptr || store == nullptr) return;

    // Retire merged
    arrangement->retirePlacement(trackId_, mergedPlacementId_);
    store->retireMaterialization(mergedMaterializationId_);

    // Revive originals
    store->reviveMaterialization(leadingMaterializationId_);
    store->reviveMaterialization(trailingMaterializationId_);
    arrangement->revivePlacement(trackId_, leadingPlacementId_);
    arrangement->revivePlacement(trackId_, trailingPlacementId_);
}

void MergePlacementAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* store = processor_.getMaterializationStore();
    if (arrangement == nullptr || store == nullptr) return;

    // Retire originals
    arrangement->retirePlacement(trackId_, leadingPlacementId_);
    arrangement->retirePlacement(trackId_, trailingPlacementId_);
    store->retireMaterialization(leadingMaterializationId_);
    store->retireMaterialization(trailingMaterializationId_);

    // Revive merged
    store->reviveMaterialization(mergedMaterializationId_);
    arrangement->revivePlacement(trackId_, mergedPlacementId_);
}

// ============================================================================
// DeletePlacementAction
// ============================================================================

DeletePlacementAction::DeletePlacementAction(OpenTuneAudioProcessor& processor, const DeleteOutcome& outcome)
    : processor_(processor)
    , trackId_(outcome.trackId)
    , placementId_(outcome.placementId)
    , materializationId_(outcome.materializationId)
{
}

void DeletePlacementAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* store = processor_.getMaterializationStore();
    if (arrangement == nullptr || store == nullptr) return;

    store->reviveMaterialization(materializationId_);
    arrangement->revivePlacement(trackId_, placementId_);
}

void DeletePlacementAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* store = processor_.getMaterializationStore();
    if (arrangement == nullptr || store == nullptr) return;

    arrangement->retirePlacement(trackId_, placementId_);
    store->retireMaterialization(materializationId_);
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
    if (arrangement != nullptr)
        arrangement->setPlacementGain(trackId_, placementId_, oldGain_);
}

void GainChangeAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    if (arrangement != nullptr)
        arrangement->setPlacementGain(trackId_, placementId_, newGain_);
}

} // namespace OpenTune
