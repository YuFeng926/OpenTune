#include "PlacementActions.h"

#include "Utils/F0Timeline.h"
#include "Utils/TimeCoordinate.h"
#include "Utils/PlaybackSampleGeometry.h"

#include <algorithm>
#include <cmath>

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
    , selectionBefore_(outcome.selectionBefore)
    , selectionAfter_(outcome.selectionAfter)
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
    processor_.requestFullContentRender(originalContentKey_);
    arrangement->revivePlacement(trackId_, originalPlacementId_);
    arrangement->setSelectionState(selectionBefore_);
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
    processor_.requestFullContentRender(leadingContentKey_);
    repo->reviveClip(trailingContentKey_);
    processor_.requestFullContentRender(trailingContentKey_);
    arrangement->revivePlacement(trackId_, leadingPlacementId_);
    arrangement->revivePlacement(trackId_, trailingPlacementId_);
    arrangement->setSelectionState(selectionAfter_);
}

bool SplitPlacementAction::retainsRetiredPlacement(int trackId, uint64_t placementId) const
{
    return trackId == trackId_
        && (placementId == originalPlacementId_ || placementId == leadingPlacementId_
            || placementId == trailingPlacementId_);
}

bool SplitPlacementAction::retainsRetiredContent(ContentKey key) const
{
    return key == originalContentKey_ || key == leadingContentKey_ || key == trailingContentKey_;
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
    , selectionBefore_(outcome.selectionBefore)
    , selectionAfter_(outcome.selectionAfter)
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
    processor_.requestFullContentRender(leadingContentKey_);
    repo->reviveClip(trailingContentKey_);
    processor_.requestFullContentRender(trailingContentKey_);
    arrangement->revivePlacement(trackId_, leadingPlacementId_);
    arrangement->revivePlacement(trackId_, trailingPlacementId_);
    arrangement->setSelectionState(selectionBefore_);
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
    processor_.requestFullContentRender(mergedContentKey_);
    arrangement->revivePlacement(trackId_, mergedPlacementId_);
    arrangement->setSelectionState(selectionAfter_);
}

bool MergePlacementAction::retainsRetiredPlacement(int trackId, uint64_t placementId) const
{
    return trackId == trackId_
        && (placementId == leadingPlacementId_ || placementId == trailingPlacementId_
            || placementId == mergedPlacementId_);
}

bool MergePlacementAction::retainsRetiredContent(ContentKey key) const
{
    return key == leadingContentKey_ || key == trailingContentKey_ || key == mergedContentKey_;
}

// ============================================================================
// DeletePlacementAction
// ============================================================================

DeletePlacementAction::DeletePlacementAction(OpenTuneAudioProcessor& processor, std::vector<DeleteOutcome> outcomes,
                                             StandaloneArrangement::SelectionState selectionBefore,
                                             StandaloneArrangement::SelectionState selectionAfter)
    : processor_(processor)
    , selectionBefore_(std::move(selectionBefore))
    , selectionAfter_(std::move(selectionAfter))
{
    entries_.reserve(outcomes.size());
    for (const auto& outcome : outcomes)
        entries_.push_back({outcome.trackId, outcome.placementId, outcome.contentKey});
}

void DeletePlacementAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* repo = processor_.getStandaloneContentRepository();
    for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
        repo->reviveClip(entry->contentKey);
        arrangement->revivePlacement(entry->trackId, entry->placementId);
    }
    arrangement->setSelectionState(selectionBefore_);
}

void DeletePlacementAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    auto* repo = processor_.getStandaloneContentRepository();
    for (const auto& entry : entries_) {
        arrangement->retirePlacement(entry.trackId, entry.placementId);
        repo->retireClip(entry.contentKey);
    }
    arrangement->setSelectionState(selectionAfter_);
}

bool DeletePlacementAction::retainsRetiredPlacement(int trackId, uint64_t placementId) const
{
    return std::any_of(entries_.begin(), entries_.end(), [trackId, placementId](const Entry& entry) {
        return entry.trackId == trackId && entry.placementId == placementId;
    });
}

bool DeletePlacementAction::retainsRetiredContent(ContentKey key) const
{
    return std::any_of(entries_.begin(), entries_.end(), [key](const Entry& entry) {
        return entry.contentKey == key;
    });
}

// ============================================================================
// MultiMovePlacementAction
// ============================================================================

MultiMovePlacementAction::MultiMovePlacementAction(OpenTuneAudioProcessor& processor,
                                                   std::vector<Entry> entries,
                                                   StandaloneArrangement::SelectionState selectionBefore,
                                                   StandaloneArrangement::SelectionState selectionAfter)
    : processor_(processor)
    , entries_(std::move(entries))
    , selectionBefore_(std::move(selectionBefore))
    , selectionAfter_(std::move(selectionAfter))
{
}

void MultiMovePlacementAction::undo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    std::vector<StandaloneArrangement::PlacementMove> moves;
    moves.reserve(entries_.size());
    for (const auto& entry : entries_)
        moves.push_back({entry.targetTrackId, entry.sourceTrackId, entry.placementId, entry.oldStartSeconds});
    if (arrangement->applyPlacementMoves(moves) != StandaloneArrangement::PlacementMoveResult::Rejected)
        arrangement->setSelectionState(selectionBefore_);
}

void MultiMovePlacementAction::redo()
{
    auto* arrangement = processor_.getStandaloneArrangement();
    std::vector<StandaloneArrangement::PlacementMove> moves;
    moves.reserve(entries_.size());
    for (const auto& entry : entries_)
        moves.push_back({entry.sourceTrackId, entry.targetTrackId, entry.placementId, entry.newStartSeconds});
    if (arrangement->applyPlacementMoves(moves) != StandaloneArrangement::PlacementMoveResult::Rejected)
        arrangement->setSelectionState(selectionAfter_);
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
                                         double oldTimelineStart, double newTimelineStart,
                                         double oldFadeIn, double oldFadeOut,
                                         double newFadeIn, double newFadeOut)
    : processor_(processor), trackId_(trackId), placementId_(placementId)
    , oldClipInSeconds_(oldClipInSeconds), oldDurationSeconds_(oldDurationSeconds)
    , newClipInSeconds_(newClipInSeconds), newDurationSeconds_(newDurationSeconds)
    , oldTimelineStart_(oldTimelineStart), newTimelineStart_(newTimelineStart)
    , oldFadeIn_(oldFadeIn), oldFadeOut_(oldFadeOut), newFadeIn_(newFadeIn), newFadeOut_(newFadeOut) {}

void TrimPlacementAction::undo()
{
    auto* arr = processor_.getStandaloneArrangement();
    arr->setPlacementTrimState(trackId_, placementId_, oldClipInSeconds_, oldDurationSeconds_, oldTimelineStart_,
                               oldFadeIn_, oldFadeOut_);
}

void TrimPlacementAction::redo()
{
    auto* arr = processor_.getStandaloneArrangement();
    arr->setPlacementTrimState(trackId_, placementId_, newClipInSeconds_, newDurationSeconds_, newTimelineStart_,
                               newFadeIn_, newFadeOut_);
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

namespace {

double placementContentOutputSeconds(const EditableContentSnapshot& content)
{
    if (content.timeGrid != nullptr && content.timeGrid->totalDurationSeconds() > 0.0)
        return content.timeGrid->totalDurationSeconds();
    if (content.audioBuffer != nullptr && content.audioSampleRate > 0.0)
        return static_cast<double>(content.audioBuffer->getNumSamples()) / content.audioSampleRate;
    return 0.0;
}

} // namespace

std::optional<PlacementEditPlan> planPlacementMove(const StandaloneArrangement::Placement& placement,
                                                   int trackId, int targetTrackId, double targetStartSeconds)
{
    PlacementEditPlan plan;
    plan.kind = PlacementEditPlan::Kind::Move;
    plan.trackBefore = trackId;
    plan.trackAfter = targetTrackId;
    plan.before = placement;
    plan.after = placement;
    plan.after.timelineStartSeconds = std::max(0.0, targetStartSeconds);
    return plan;
}

std::optional<PlacementEditPlan> planPlacementTrim(const StandaloneArrangement::Placement& placement,
                                                   const EditableContentSnapshot& content,
                                                   int trackId, bool trimStart, double targetTimelineSeconds)
{
    constexpr double minDurationSeconds = 0.01;
    constexpr double timelineEpsilon = 1.0 / TimeCoordinate::kRenderSampleRate;
    const auto& grid = content.timeGrid;
    const double outputStart = grid != nullptr ? grid->tauForward(placement.clipInSeconds)
                                                : placement.clipInSeconds;
    PlacementEditPlan plan;
    plan.kind = PlacementEditPlan::Kind::Trim;
    plan.trimStart = trimStart;
    plan.trackBefore = trackId;
    plan.trackAfter = trackId;
    plan.before = placement;
    plan.after = placement;
    auto& after = plan.after;

    if (trimStart) {
        const double newDuration = placement.timelineEndSeconds() - targetTimelineSeconds;
        const double outputCut = outputStart + (placement.durationSeconds - newDuration);
        if (newDuration < minDurationSeconds || outputCut < -timelineEpsilon)
            return std::nullopt;
        after.clipInSeconds = grid != nullptr ? grid->tauInverse(std::max(0.0, outputCut))
                                              : std::max(0.0, outputCut);
        after.durationSeconds = newDuration;
        after.timelineStartSeconds = targetTimelineSeconds;
    } else {
        const double newDuration = targetTimelineSeconds - placement.timelineStartSeconds;
        if (newDuration < minDurationSeconds
            || outputStart + newDuration > placementContentOutputSeconds(content) + timelineEpsilon)
            return std::nullopt;
        after.durationSeconds = newDuration;
    }

    const double maxFade = after.durationSeconds * 0.9;
    after.fadeInDuration = std::min(after.fadeInDuration, maxFade);
    after.fadeOutDuration = std::min(after.fadeOutDuration, maxFade);
    return plan;
}

std::optional<PlacementEditPlan> planPlacementGain(const StandaloneArrangement::Placement& placement, int trackId,
                                                   double gainLinear)
{
    if (!std::isfinite(gainLinear))
        return std::nullopt;
    PlacementEditPlan plan;
    plan.kind = PlacementEditPlan::Kind::Gain;
    plan.trackBefore = trackId;
    plan.trackAfter = trackId;
    plan.before = placement;
    plan.after = placement;
    plan.after.gain = static_cast<float>(std::max(0.0, gainLinear));
    if (!std::isfinite(plan.after.gain))
        return std::nullopt;
    return plan;
}

bool canMergePlacementPitchCurves(const std::shared_ptr<const PitchCurveSnapshot>& leadingCurve,
                                  OriginalF0State leadingState,
                                  const std::shared_ptr<const PitchCurveSnapshot>& trailingCurve,
                                  OriginalF0State trailingState)
{
    if (leadingState != trailingState)
        return false;
    if (leadingCurve == nullptr || trailingCurve == nullptr)
        return leadingCurve == nullptr && trailingCurve == nullptr;
    return leadingCurve->getHopSize() > 0
        && leadingCurve->getHopSize() == trailingCurve->getHopSize()
        && std::abs(leadingCurve->getSampleRate() - trailingCurve->getSampleRate()) <= 1.0e-6;
}

PlacementEditPlan planPlacementFade(const StandaloneArrangement::Placement& placement, int trackId,
                                    std::optional<double> fadeInSeconds,
                                    std::optional<double> fadeOutSeconds)
{
    PlacementEditPlan plan;
    plan.kind = PlacementEditPlan::Kind::Fade;
    plan.trackBefore = trackId;
    plan.trackAfter = trackId;
    plan.before = placement;
    plan.after = placement;
    const double maxFade = placement.durationSeconds * 0.9;
    if (fadeInSeconds)
        plan.after.fadeInDuration = std::clamp(*fadeInSeconds, 0.0, maxFade);
    if (fadeOutSeconds)
        plan.after.fadeOutDuration = std::clamp(*fadeOutSeconds, 0.0, maxFade);
    return plan;
}

double placementGainFromDb(double gainDb)
{
    return std::pow(10.0, gainDb / 20.0);
}

double placementGainFromDrag(float startingGain, double deltaY)
{
    return startingGain * std::pow(10.0, -deltaY / 200.0);
}

namespace {

std::optional<std::pair<double, double>> placementAudibleSourceWindow(
    const StandaloneArrangement::Placement& placement,
    const EditableContentSnapshot& content)
{
    if (content.timeGrid == nullptr || !std::isfinite(placement.clipInSeconds)
        || !std::isfinite(placement.durationSeconds) || placement.clipInSeconds < 0.0
        || placement.durationSeconds <= 0.0)
        return std::nullopt;

    const double outputStart = content.timeGrid->tauForward(placement.clipInSeconds);
    const double outputEnd = outputStart + placement.durationSeconds;
    if (!std::isfinite(outputStart) || !std::isfinite(outputEnd)
        || outputEnd > content.timeGrid->totalDurationSeconds())
        return std::nullopt;

    return std::make_pair(content.sourceWindow.sourceStartSeconds + placement.clipInSeconds,
                          content.sourceWindow.sourceStartSeconds + content.timeGrid->tauInverse(outputEnd));
}

} // namespace

PlacementSplitPlan planPlacementSplit(const StandaloneArrangement::Placement& placement,
                                      const EditableContentSnapshot& content, double timelineSeconds)
{
    constexpr double minDurationSeconds = 0.1;
    const double offset = timelineSeconds - placement.timelineStartSeconds;
    if (offset <= minDurationSeconds || offset >= placement.durationSeconds - minDurationSeconds)
        return {PlacementSplitIssue::OutOfRange, 0.0};
    if (content.audioBuffer == nullptr || content.nativeSource.buffer == nullptr)
        return {PlacementSplitIssue::ContentUnavailable, 0.0};
    if (content.timeGrid != nullptr && !content.timeGrid->isIdentity())
        return {PlacementSplitIssue::TimeGridEdited, 0.0};
    if (!content.hasUsableOriginalF0())
        return {PlacementSplitIssue::F0NotReady, 0.0};
    if (!std::isfinite(content.audioSampleRate) || content.audioSampleRate <= 0.0)
        return {PlacementSplitIssue::InvalidSampleRate, 0.0};

    const auto& curve = *content.pitchCurve;
    const int frames = static_cast<int>(curve.getOriginalF0().size());
    const F0Timeline timeline(curve.getHopSize(), curve.getSampleRate(), frames);
    const int boundary = timeline.nearestFrameBoundary(placement.clipInSeconds + offset);
    if (boundary <= 0 || boundary >= frames)
        return {PlacementSplitIssue::OutOfRange, 0.0};

    const double splitOffset = timeline.timeAtFrame(boundary) - placement.clipInSeconds;
    if (splitOffset <= minDurationSeconds || splitOffset >= placement.durationSeconds - minDurationSeconds)
        return {PlacementSplitIssue::OutOfRange, 0.0};

    const double boundarySourceSeconds = content.sourceWindow.sourceStartSeconds + timeline.timeAtFrame(boundary);
    const auto boundarySample = PlaybackSampleGeometry::projectToSample(boundarySourceSeconds * content.audioSampleRate);
    const auto leadingSamples = boundarySample - content.firstCanonicalSample;
    if (leadingSamples <= 0 || leadingSamples >= content.audioBuffer->getNumSamples())
        return {PlacementSplitIssue::BoundaryOutsideAudio, 0.0};
    return {PlacementSplitIssue::None, placement.timelineStartSeconds + splitOffset};
}

PlacementMergePlan planPlacementMerge(int leadingTrackId, const StandaloneArrangement::Placement& leading,
                                      const EditableContentSnapshot& leadingContent, int trailingTrackId,
                                      const StandaloneArrangement::Placement& trailing,
                                      const EditableContentSnapshot& trailingContent)
{
    constexpr double timelineEpsilon = 1.0 / TimeCoordinate::kRenderSampleRate;
    constexpr float gainTolerance = 1.0e-6f;
    constexpr float keyConfidenceTolerance = 1.0e-6f;
    PlacementMergePlan plan{PlacementMergeIssue::None, leadingTrackId,
                            leading.placementId, trailing.placementId};
    if (leading.placementId == trailing.placementId)
        plan.issue = PlacementMergeIssue::SamePlacement;
    else if (leadingTrackId != trailingTrackId)
        plan.issue = PlacementMergeIssue::DifferentTracks;
    else if (std::abs(leading.timelineEndSeconds() - trailing.timelineStartSeconds) > timelineEpsilon)
        plan.issue = PlacementMergeIssue::NotAdjacent;
    else if (std::abs(leading.gain - trailing.gain) > gainTolerance || leading.name != trailing.name)
        plan.issue = PlacementMergeIssue::MetadataMismatch;
    else if (leadingContent.audioBuffer == nullptr || trailingContent.audioBuffer == nullptr
             || leadingContent.nativeSource.buffer == nullptr || trailingContent.nativeSource.buffer == nullptr
             || leadingContent.sourceWindow.sourceId == 0
             || leadingContent.sourceWindow.sourceId != trailingContent.sourceWindow.sourceId
             || !(leadingContent.nativeSource == trailingContent.nativeSource))
        plan.issue = PlacementMergeIssue::DifferentSource;
    else if (!std::isfinite(leadingContent.audioSampleRate) || leadingContent.audioSampleRate <= 0.0
             || leadingContent.audioSampleRate != trailingContent.audioSampleRate)
        plan.issue = PlacementMergeIssue::SampleRateMismatch;
    else if (leadingContent.sourceWindow.sourceEndSeconds != trailingContent.sourceWindow.sourceStartSeconds)
        plan.issue = PlacementMergeIssue::SourceBoundaryMismatch;
    else if (leadingContent.detectedKey.root != trailingContent.detectedKey.root
             || leadingContent.detectedKey.scale != trailingContent.detectedKey.scale
             || leadingContent.detectedKey.origin != trailingContent.detectedKey.origin
             || std::abs(leadingContent.detectedKey.confidence - trailingContent.detectedKey.confidence)
                    > keyConfidenceTolerance)
        plan.issue = PlacementMergeIssue::KeyMismatch;
    else if (leadingContent.originalF0State == OriginalF0State::Extracting
             || trailingContent.originalF0State == OriginalF0State::Extracting)
        plan.issue = PlacementMergeIssue::F0NotReady;
    else if (leadingContent.timeGrid == nullptr || trailingContent.timeGrid == nullptr
             || !leadingContent.timeGrid->isIdentity() || !trailingContent.timeGrid->isIdentity()
             || leadingContent.timeGrid->totalDurationSeconds() != leadingContent.sourceWindow.durationSeconds()
             || trailingContent.timeGrid->totalDurationSeconds() != trailingContent.sourceWindow.durationSeconds())
        plan.issue = PlacementMergeIssue::TimeGridUnsupported;
    else {
        const auto leadingWindow = placementAudibleSourceWindow(leading, leadingContent);
        const auto trailingWindow = placementAudibleSourceWindow(trailing, trailingContent);
        if (!leadingWindow || !trailingWindow
            || leadingWindow->first != leadingContent.sourceWindow.sourceStartSeconds
            || leadingWindow->second != leadingContent.sourceWindow.sourceEndSeconds
            || trailingWindow->first != trailingContent.sourceWindow.sourceStartSeconds
            || trailingWindow->second != trailingContent.sourceWindow.sourceEndSeconds
            || leading.fadeOutDuration != 0.0 || trailing.fadeInDuration != 0.0) {
            plan.issue = PlacementMergeIssue::AudibleWindowMismatch;
        } else if (!canMergePlacementPitchCurves(leadingContent.pitchCurve,
                                                 leadingContent.originalF0State,
                                                 trailingContent.pitchCurve,
                                                 trailingContent.originalF0State)) {
            plan.issue = PlacementMergeIssue::PitchCurveMismatch;
        }
    }
    return plan;
}

bool applyPlacementEdit(OpenTuneAudioProcessor& processor, const PlacementEditPlan& plan)
{
    auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr)
        return false;
    const auto id = plan.before.placementId;
    bool applied = false;
    bool changed = false;
    switch (plan.kind) {
        case PlacementEditPlan::Kind::Move: {
            const auto result = arrangement->applyPlacementMoves({StandaloneArrangement::PlacementMove{
                plan.trackBefore, plan.trackAfter, id, plan.after.timelineStartSeconds}});
            if (result == StandaloneArrangement::PlacementMoveResult::Rejected)
                return false;
            applied = true;
            changed = result == StandaloneArrangement::PlacementMoveResult::Applied;
            break;
        }
        case PlacementEditPlan::Kind::Trim:
            applied = plan.trimStart
                          ? arrangement->setPlacementTrimAndTimelineStart(
                                plan.trackAfter, id, plan.after.clipInSeconds, plan.after.durationSeconds,
                                plan.after.timelineStartSeconds)
                          : arrangement->setPlacementTrim(plan.trackAfter, id, plan.after.clipInSeconds,
                                                          plan.after.durationSeconds);
            changed = plan.before.clipInSeconds != plan.after.clipInSeconds
                || plan.before.durationSeconds != plan.after.durationSeconds
                || (plan.trimStart && plan.before.timelineStartSeconds != plan.after.timelineStartSeconds)
                || plan.before.fadeInDuration != plan.after.fadeInDuration
                || plan.before.fadeOutDuration != plan.after.fadeOutDuration;
            break;
        case PlacementEditPlan::Kind::Gain:
            applied = arrangement->setPlacementGain(plan.trackAfter, id, plan.after.gain);
            changed = plan.before.gain != plan.after.gain;
            break;
        case PlacementEditPlan::Kind::Fade:
            applied = arrangement->setPlacementFade(plan.trackAfter, id, plan.after.fadeInDuration,
                                                      plan.after.fadeOutDuration);
            changed = plan.before.fadeInDuration != plan.after.fadeInDuration
                || plan.before.fadeOutDuration != plan.after.fadeOutDuration;
            break;
    }
    if (applied && changed)
        processor.notifyPlacementChanged();
    return applied;
}

std::vector<std::unique_ptr<UndoAction>> placementEditUndoActions(OpenTuneAudioProcessor& processor,
                                                                 const PlacementEditPlan& plan,
                                                                 const StandaloneArrangement::SelectionState& selectionBefore,
                                                                 const StandaloneArrangement::SelectionState& selectionAfter)
{
    using Kind = PlacementEditPlan::Kind;
    std::vector<std::unique_ptr<UndoAction>> actions;
    const auto& before = plan.before;
    const auto& after = plan.after;
    const auto id = before.placementId;
    const int track = plan.trackAfter;
    switch (plan.kind) {
        case Kind::Move:
            if (plan.trackBefore != plan.trackAfter
                || before.timelineStartSeconds != after.timelineStartSeconds) {
                MultiMovePlacementAction::Entry entry{plan.trackBefore, plan.trackAfter, id,
                                                       before.timelineStartSeconds, after.timelineStartSeconds};
                actions.push_back(std::make_unique<MultiMovePlacementAction>(
                    processor, std::vector<MultiMovePlacementAction::Entry>{entry},
                    selectionBefore, selectionAfter));
            }
            break;
        case Kind::Trim:
            if (before.clipInSeconds != after.clipInSeconds || before.durationSeconds != after.durationSeconds
                || before.timelineStartSeconds != after.timelineStartSeconds
                || before.fadeInDuration != after.fadeInDuration || before.fadeOutDuration != after.fadeOutDuration)
                actions.push_back(std::make_unique<TrimPlacementAction>(processor, track, id,
                    before.clipInSeconds, before.durationSeconds, after.clipInSeconds, after.durationSeconds,
                    before.timelineStartSeconds, after.timelineStartSeconds,
                    before.fadeInDuration, before.fadeOutDuration, after.fadeInDuration, after.fadeOutDuration));
            break;
        case Kind::Gain:
            if (before.gain != after.gain)
                actions.push_back(std::make_unique<GainChangeAction>(processor, track, id, before.gain, after.gain));
            break;
        case Kind::Fade:
            if (before.fadeInDuration != after.fadeInDuration || before.fadeOutDuration != after.fadeOutDuration)
                actions.push_back(std::make_unique<FadeChangeAction>(processor, track, id,
                    before.fadeInDuration, before.fadeOutDuration, after.fadeInDuration, after.fadeOutDuration));
            break;
    }
    return actions;
}

bool commitPlacementEdit(OpenTuneAudioProcessor& processor, const PlacementEditPlan& plan,
                         std::vector<std::unique_ptr<UndoAction>>& undoActions)
{
    auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr)
        return false;
    const auto selectionBefore = arrangement->getSelectionState();
    if (!applyPlacementEdit(processor, plan))
        return false;
    undoActions = placementEditUndoActions(processor, plan, selectionBefore, arrangement->getSelectionState());
    return true;
}

std::optional<PlacementSplitCommit> commitPlacementSplit(OpenTuneAudioProcessor& processor, int trackId,
                                                         uint64_t placementId,
                                                         const PlacementSplitPlan& plan)
{
    if (plan.issue != PlacementSplitIssue::None)
        return std::nullopt;
    const int index = processor.findPlacementIndexById(trackId, placementId);
    const auto outcome = processor.splitPlacementAtSeconds(trackId, index, plan.timelineSeconds);
    if (!outcome)
        return std::nullopt;
    return PlacementSplitCommit{*outcome, std::make_unique<SplitPlacementAction>(processor, *outcome)};
}

std::optional<PlacementMergeCommit> commitPlacementMerge(OpenTuneAudioProcessor& processor,
                                                          const PlacementMergePlan& plan)
{
    if (plan.issue != PlacementMergeIssue::None)
        return std::nullopt;
    const int index = processor.findPlacementIndexById(plan.trackId, plan.leadingPlacementId);
    const auto outcome = processor.mergePlacements(plan.trackId, plan.leadingPlacementId,
                                                   plan.trailingPlacementId, index);
    if (!outcome)
        return std::nullopt;
    return PlacementMergeCommit{*outcome, std::make_unique<MergePlacementAction>(processor, *outcome)};
}

PlacementDeleteCommit commitPlacementDelete(OpenTuneAudioProcessor& processor,
                                             const std::vector<PlacementKey>& placements)
{
    PlacementDeleteCommit result;
    auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr || placements.empty())
        return result;

    result.selectionBefore = arrangement->getSelectionState();
    std::set<StandaloneArrangement::PlacementSelectionKey> unique;
    for (const auto& placement : placements) {
        StandaloneArrangement::Placement active;
        if (!unique.insert({placement.trackId, placement.placementId}).second
            || !arrangement->getPlacementById(placement.trackId, placement.placementId, active))
            return result;
    }

    result.outcomes.reserve(placements.size());
    for (const auto& placement : placements) {
        const int index = processor.findPlacementIndexById(placement.trackId, placement.placementId);
        const auto outcome = processor.deletePlacement(placement.trackId, index);
        if (!outcome)
            break;
        result.outcomes.push_back(*outcome);
    }
    if (!result.outcomes.empty()) {
        result.selectionAfter = arrangement->getSelectionState();
        result.selectionAfter.placements.clear();
        result.selectionAfter.selectedPlacementIds.fill(0);
        arrangement->setSelectionState(result.selectionAfter);
        result.undoAction = std::make_unique<DeletePlacementAction>(processor, result.outcomes,
                                                                   result.selectionBefore, result.selectionAfter);
    }
    return result;
}

PlacementCopyCommit commitPlacementCopies(OpenTuneAudioProcessor& processor,
                                          const std::vector<PlacementCopyRequest>& requests)
{
    PlacementCopyCommit result;
    auto* arrangement = processor.getStandaloneArrangement();
    auto* repository = processor.getStandaloneContentRepository();
    if (arrangement == nullptr || repository == nullptr || requests.empty())
        return result;

    const int targetTrackId = requests.front().targetTrackId;
    if (targetTrackId < 0 || targetTrackId >= arrangement->getNumTracks()) {
        result.failure = PlacementCopyCommit::Failure::PlacementInsert;
        return result;
    }
    if (std::any_of(requests.begin(), requests.end(), [targetTrackId](const PlacementCopyRequest& request) {
            return request.targetTrackId != targetTrackId;
        })) {
        result.failure = PlacementCopyCommit::Failure::PlacementInsert;
        return result;
    }

    std::vector<ContentKey> contents;
    contents.reserve(requests.size());
    for (const auto& request : requests) {
        const auto& source = request.source;
        const auto content = request.fullContent
                                 ? processor.cloneContent(source.contentKey, source.name + " Copy")
                                 : processor.copyContentRange(source.contentKey, source.clipInSeconds,
                                                              source.durationSeconds);
        if (!content.isValid()) {
            for (const auto& made : contents)
                repository->retireClip(made);
            if (!contents.empty())
                processor.scheduleReclaimSweep();
            result.failure = PlacementCopyCommit::Failure::ContentCopy;
            return result;
        }
        contents.push_back(content);
    }

    std::vector<StandaloneArrangement::Placement> placements;
    placements.reserve(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        const auto& source = request.source;
        auto placement = source;
        if (!request.fullContent) {
            placement.durationSeconds = source.durationSeconds;
            placement.gain = source.gain;
            placement.fadeInDuration = source.fadeInDuration;
            placement.fadeOutDuration = source.fadeOutDuration;
            placement.name = source.name;
            placement.clipInSeconds = 0.0;
        }
        placement.placementId = 0;
        placement.contentKey = contents[i];
        placement.timelineStartSeconds = request.targetStartSeconds;
        placement.isRetired = false;
        placements.push_back(std::move(placement));
    }

    if (!arrangement->insertPlacements(targetTrackId, placements)) {
        for (const auto& content : contents)
            repository->retireClip(content);
        processor.scheduleReclaimSweep();
        result.failure = PlacementCopyCommit::Failure::PlacementInsert;
        return result;
    }

    result.copies.reserve(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i)
        result.copies.push_back({contents[i], std::move(placements[i])});

    processor.notifyPlacementChanged();

    if (std::any_of(requests.begin(), requests.end(), [](const PlacementCopyRequest& request) {
            return request.selectCopies;
        })) {
        auto selection = arrangement->getSelectionState();
        selection.placements.clear();
        selection.activeTrackId = targetTrackId;
        for (const auto& copy : result.copies)
            selection.placements.insert({targetTrackId, copy.placement.placementId});
        selection.selectedPlacementIds[static_cast<size_t>(targetTrackId)] =
            result.copies.back().placement.placementId;
        arrangement->setSelectionState(selection);
    }
    return result;
}

bool applyPlacementMoveBatch(OpenTuneAudioProcessor& processor,
                             const std::vector<MultiMovePlacementAction::Entry>& entries,
                             PlacementKey primaryAfter,
                             std::unique_ptr<UndoAction>& undoAction)
{
    auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr)
        return false;
    const auto selectionBefore = arrangement->getSelectionState();
    const auto result = arrangement->applyPlacementMoves([&entries] {
        std::vector<StandaloneArrangement::PlacementMove> moves;
        moves.reserve(entries.size());
        for (const auto& entry : entries)
            moves.push_back({entry.sourceTrackId, entry.targetTrackId, entry.placementId, entry.newStartSeconds});
        return moves;
    }());
    if (result == StandaloneArrangement::PlacementMoveResult::Rejected)
        return false;
    if (result == StandaloneArrangement::PlacementMoveResult::Applied) {
        auto selectionAfter = arrangement->getSelectionState();
        selectionAfter.activeTrackId = primaryAfter.trackId;
        selectionAfter.selectedPlacementIds[static_cast<size_t>(primaryAfter.trackId)] = primaryAfter.placementId;
        arrangement->setSelectionState(selectionAfter);
        undoAction = std::make_unique<MultiMovePlacementAction>(processor, entries, selectionBefore, selectionAfter);
        processor.notifyPlacementChanged();
    }
    return true;
}

} // namespace OpenTune
