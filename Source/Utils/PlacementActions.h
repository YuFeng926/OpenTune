#pragma once

#include "UndoManager.h"
#include "LocalizationManager.h"
#include "../PluginProcessor.h"
#include "../Content/ContentKey.h"
#include "../Content/EditableContentSnapshot.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace OpenTune {

class OpenTuneAudioProcessor;

// Split undo 使用 lineage retire/revive：
// redo 状态 = 原始 retired, leading+trailing active
// undo 状态 = 原始 active, leading+trailing retired
class SplitPlacementAction : public UndoAction {
public:
    SplitPlacementAction(OpenTuneAudioProcessor& processor, const SplitOutcome& outcome);
    void undo() override;
    void redo() override;
    bool retainsRetiredPlacement(int trackId, uint64_t placementId) const override;
    bool retainsRetiredContent(ContentKey key) const override;
    juce::String getDescription() const override { return LOC(kSplitClip); }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t originalPlacementId_;
    ContentKey originalContentKey_;
    uint64_t leadingPlacementId_;
    ContentKey leadingContentKey_;
    uint64_t trailingPlacementId_;
    ContentKey trailingContentKey_;
    StandaloneArrangement::SelectionState selectionBefore_;
    StandaloneArrangement::SelectionState selectionAfter_;
};

// Merge undo 使用 lineage retire/revive：
// redo 状态 = leading+trailing retired, merged active
// undo 状态 = leading+trailing active, merged retired
class MergePlacementAction : public UndoAction {
public:
    MergePlacementAction(OpenTuneAudioProcessor& processor, const MergeOutcome& outcome);
    void undo() override;
    void redo() override;
    bool retainsRetiredPlacement(int trackId, uint64_t placementId) const override;
    bool retainsRetiredContent(ContentKey key) const override;
    juce::String getDescription() const override { return LOC(kMergeClips); }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t leadingPlacementId_;
    ContentKey leadingContentKey_;
    uint64_t trailingPlacementId_;
    ContentKey trailingContentKey_;
    uint64_t mergedPlacementId_;
    ContentKey mergedContentKey_;
    StandaloneArrangement::SelectionState selectionBefore_;
    StandaloneArrangement::SelectionState selectionAfter_;
};

// Delete undo 使用 lineage retire/revive：
// redo 状态 = placement+content retired
// undo 状态 = placement+content active
class DeletePlacementAction : public UndoAction {
public:
    DeletePlacementAction(OpenTuneAudioProcessor& processor, std::vector<DeleteOutcome> outcomes,
                          StandaloneArrangement::SelectionState selectionBefore,
                          StandaloneArrangement::SelectionState selectionAfter);
    void undo() override;
    void redo() override;
    bool retainsRetiredPlacement(int trackId, uint64_t placementId) const override;
    bool retainsRetiredContent(ContentKey key) const override;
    juce::String getDescription() const override { return LOC_RAW("Delete Clip"); }

private:
    struct Entry {
        int trackId;
        uint64_t placementId;
        ContentKey contentKey;
    };

    OpenTuneAudioProcessor& processor_;
    std::vector<Entry> entries_;
    StandaloneArrangement::SelectionState selectionBefore_;
    StandaloneArrangement::SelectionState selectionAfter_;
};

struct PlacementKey {
    int trackId = -1;
    uint64_t placementId = 0;
};

struct PlacementEditPlan {
    enum class Kind { Move, Trim, Gain, Fade };

    Kind kind = Kind::Move;
    bool trimStart = false;
    int trackBefore = 0;
    int trackAfter = 0;
    StandaloneArrangement::Placement before;
    StandaloneArrangement::Placement after;
};

enum class PlacementSplitIssue { None, OutOfRange, ContentUnavailable, TimeGridEdited, F0NotReady,
                                 InvalidSampleRate, BoundaryOutsideAudio };

struct PlacementSplitPlan {
    PlacementSplitIssue issue = PlacementSplitIssue::None;
    double timelineSeconds = 0.0;
};

struct PlacementSplitCommit {
    SplitOutcome outcome;
    std::unique_ptr<UndoAction> undoAction;
};

enum class PlacementMergeIssue { None, SamePlacement, DifferentTracks, NotAdjacent, MetadataMismatch,
                                 DifferentSource, SampleRateMismatch, SourceBoundaryMismatch,
                                 KeyMismatch, F0NotReady, TimeGridUnsupported, AudibleWindowMismatch,
                                 PitchCurveMismatch };

struct PlacementMergePlan {
    PlacementMergeIssue issue = PlacementMergeIssue::None;
    int trackId = -1;
    uint64_t leadingPlacementId = 0;
    uint64_t trailingPlacementId = 0;
};

struct PlacementMergeCommit {
    MergeOutcome outcome;
    std::unique_ptr<UndoAction> undoAction;
};

struct PlacementDeleteCommit {
    std::vector<DeleteOutcome> outcomes;
    std::unique_ptr<UndoAction> undoAction;
    StandaloneArrangement::SelectionState selectionBefore;
    StandaloneArrangement::SelectionState selectionAfter;
};

struct PlacementCopyRequest {
    StandaloneArrangement::Placement source;
    int targetTrackId = 0;
    double targetStartSeconds = 0.0;
    bool fullContent = false;
    bool selectCopies = false;
};

struct PlacementCopyResult {
    ContentKey contentKey;
    StandaloneArrangement::Placement placement;
};

struct PlacementCopyCommit {
    enum class Failure { None, ContentCopy, PlacementInsert };
    std::vector<PlacementCopyResult> copies;
    Failure failure = Failure::None;
};

std::optional<PlacementEditPlan> planPlacementMove(const StandaloneArrangement::Placement& placement,
                                                   int trackId, int targetTrackId, double targetStartSeconds);
std::optional<PlacementEditPlan> planPlacementTrim(const StandaloneArrangement::Placement& placement,
                                                   const EditableContentSnapshot& content,
                                                   int trackId, bool trimStart, double targetTimelineSeconds);
std::optional<PlacementEditPlan> planPlacementGain(const StandaloneArrangement::Placement& placement, int trackId,
                                                  double gainLinear);
bool canMergePlacementPitchCurves(const std::shared_ptr<const PitchCurveSnapshot>& leadingCurve,
                                  OriginalF0State leadingState,
                                  const std::shared_ptr<const PitchCurveSnapshot>& trailingCurve,
                                  OriginalF0State trailingState);
PlacementEditPlan planPlacementFade(const StandaloneArrangement::Placement& placement, int trackId,
                                    std::optional<double> fadeInSeconds,
                                    std::optional<double> fadeOutSeconds);
double placementGainFromDb(double gainDb);
double placementGainFromDrag(float startingGain, double deltaY);
PlacementSplitPlan planPlacementSplit(const StandaloneArrangement::Placement& placement,
                                      const EditableContentSnapshot& content, double timelineSeconds);
PlacementMergePlan planPlacementMerge(int leadingTrackId, const StandaloneArrangement::Placement& leading,
                                      const EditableContentSnapshot& leadingContent, int trailingTrackId,
                                      const StandaloneArrangement::Placement& trailing,
                                      const EditableContentSnapshot& trailingContent);
bool applyPlacementEdit(OpenTuneAudioProcessor& processor, const PlacementEditPlan& plan);
std::vector<std::unique_ptr<UndoAction>> placementEditUndoActions(OpenTuneAudioProcessor& processor,
                                                                 const PlacementEditPlan& plan,
                                                                 const StandaloneArrangement::SelectionState& selectionBefore,
                                                                 const StandaloneArrangement::SelectionState& selectionAfter);
bool commitPlacementEdit(OpenTuneAudioProcessor& processor, const PlacementEditPlan& plan,
                         std::vector<std::unique_ptr<UndoAction>>& undoActions);
std::optional<PlacementSplitCommit> commitPlacementSplit(OpenTuneAudioProcessor& processor, int trackId,
                                                         uint64_t placementId,
                                                         const PlacementSplitPlan& plan);
std::optional<PlacementMergeCommit> commitPlacementMerge(OpenTuneAudioProcessor& processor,
                                                          const PlacementMergePlan& plan);
PlacementDeleteCommit commitPlacementDelete(OpenTuneAudioProcessor& processor,
                                            const std::vector<PlacementKey>& placements);
PlacementCopyCommit commitPlacementCopies(OpenTuneAudioProcessor& processor,
                                          const std::vector<PlacementCopyRequest>& requests);

class MultiMovePlacementAction : public UndoAction {
public:
    struct Entry {
        int sourceTrackId = -1;
        int targetTrackId = -1;
        uint64_t placementId = 0;
        double oldStartSeconds = 0.0;
        double newStartSeconds = 0.0;
    };

    MultiMovePlacementAction(OpenTuneAudioProcessor& processor,
                             std::vector<Entry> entries,
                             StandaloneArrangement::SelectionState selectionBefore,
                             StandaloneArrangement::SelectionState selectionAfter);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return LOC_RAW("Move Clip"); }

private:
    OpenTuneAudioProcessor& processor_;
    std::vector<Entry> entries_;
    StandaloneArrangement::SelectionState selectionBefore_;
    StandaloneArrangement::SelectionState selectionAfter_;
};

bool applyPlacementMoveBatch(OpenTuneAudioProcessor& processor,
                             const std::vector<MultiMovePlacementAction::Entry>& entries,
                             PlacementKey primaryAfter,
                             std::unique_ptr<UndoAction>& undoAction);

// Gain undo: 恢复原始增益
class GainChangeAction : public UndoAction {
public:
    GainChangeAction(OpenTuneAudioProcessor& processor,
                     int trackId, uint64_t placementId,
                     float oldGain, float newGain);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return LOC_RAW("Adjust Gain"); }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t placementId_;
    float oldGain_;
    float newGain_;
};

// Trim undo: 恢复原始 clipInSeconds、durationSeconds 和 timelineStartSeconds
class TrimPlacementAction : public UndoAction {
public:
    TrimPlacementAction(OpenTuneAudioProcessor& processor,
                        int trackId, uint64_t placementId,
                        double oldClipInSeconds, double oldDurationSeconds,
                        double newClipInSeconds, double newDurationSeconds,
                        double oldTimelineStart, double newTimelineStart,
                        double oldFadeIn, double oldFadeOut,
                        double newFadeIn, double newFadeOut);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return LOC_RAW("Trim Clip"); }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t placementId_;
    double oldClipInSeconds_;
    double oldDurationSeconds_;
    double newClipInSeconds_;
    double newDurationSeconds_;
    double oldTimelineStart_;  // left trim also shifts timeline start
    double newTimelineStart_;
    double oldFadeIn_;
    double oldFadeOut_;
    double newFadeIn_;
    double newFadeOut_;
};

// Fade undo: 恢复原始 fadeIn/fadeOut duration
class FadeChangeAction : public UndoAction {
public:
    FadeChangeAction(OpenTuneAudioProcessor& processor,
                     int trackId, uint64_t placementId,
                     double oldFadeIn, double oldFadeOut,
                     double newFadeIn, double newFadeOut);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return LOC_RAW("Adjust Fade"); }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t placementId_;
    double oldFadeIn_;
    double oldFadeOut_;
    double newFadeIn_;
    double newFadeOut_;
};

} // namespace OpenTune
