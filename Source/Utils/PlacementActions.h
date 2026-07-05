#pragma once

#include "UndoManager.h"
#include "../PluginProcessor.h"
#include "../Content/ContentKey.h"
#include <cstdint>
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
    juce::String getDescription() const override { return TRANS("分割片段"); }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t originalPlacementId_;
    ContentKey originalContentKey_;
    uint64_t leadingPlacementId_;
    ContentKey leadingContentKey_;
    uint64_t trailingPlacementId_;
    ContentKey trailingContentKey_;
};

// Merge undo 使用 lineage retire/revive：
// redo 状态 = leading+trailing retired, merged active
// undo 状态 = leading+trailing active, merged retired
class MergePlacementAction : public UndoAction {
public:
    MergePlacementAction(OpenTuneAudioProcessor& processor, const MergeOutcome& outcome);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return TRANS("合并片段"); }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t leadingPlacementId_;
    ContentKey leadingContentKey_;
    uint64_t trailingPlacementId_;
    ContentKey trailingContentKey_;
    uint64_t mergedPlacementId_;
    ContentKey mergedContentKey_;
};

// Delete undo 使用 lineage retire/revive：
// redo 状态 = placement+content retired
// undo 状态 = placement+content active
class DeletePlacementAction : public UndoAction {
public:
    DeletePlacementAction(OpenTuneAudioProcessor& processor, const DeleteOutcome& outcome);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return TRANS("删除片段"); }

private:
    OpenTuneAudioProcessor& processor_;
    int trackId_;
    uint64_t placementId_;
    ContentKey contentKey_;
};

class MultiMovePlacementAction : public UndoAction {
public:
    struct Entry {
        int sourceTrackId = -1;
        int targetTrackId = -1;
        uint64_t placementId = 0;
        double oldStartSeconds = 0.0;
        double newStartSeconds = 0.0;
    };

    MultiMovePlacementAction(OpenTuneAudioProcessor& processor, std::vector<Entry> entries);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return TRANS("移动片段"); }

private:
    OpenTuneAudioProcessor& processor_;
    std::vector<Entry> entries_;
};

// Gain undo: 恢复原始增益
class GainChangeAction : public UndoAction {
public:
    GainChangeAction(OpenTuneAudioProcessor& processor,
                     int trackId, uint64_t placementId,
                     float oldGain, float newGain);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return TRANS("调整增益"); }

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
                        double oldTimelineStart, double newTimelineStart);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return TRANS("裁剪片段"); }

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
    juce::String getDescription() const override { return TRANS("调整淡变"); }

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
