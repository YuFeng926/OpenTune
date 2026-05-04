#pragma once

#include "UndoManager.h"
#include "../PluginProcessor.h"
#include <cstdint>

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
    uint64_t originalMaterializationId_;
    uint64_t leadingPlacementId_;
    uint64_t leadingMaterializationId_;
    uint64_t trailingPlacementId_;
    uint64_t trailingMaterializationId_;
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
    uint64_t leadingMaterializationId_;
    uint64_t trailingPlacementId_;
    uint64_t trailingMaterializationId_;
    uint64_t mergedPlacementId_;
    uint64_t mergedMaterializationId_;
};

// Delete undo 使用 lineage retire/revive：
// redo 状态 = placement+materialization retired
// undo 状态 = placement+materialization active
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
    uint64_t materializationId_;
};

// Move undo: 恢复原始 timeline 位置（支持跨轨）
class MovePlacementAction : public UndoAction {
public:
    MovePlacementAction(OpenTuneAudioProcessor& processor,
                        int sourceTrackId, int targetTrackId,
                        uint64_t placementId,
                        double oldStartSeconds, double newStartSeconds);
    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return TRANS("移动片段"); }

private:
    OpenTuneAudioProcessor& processor_;
    int sourceTrackId_;
    int targetTrackId_;
    uint64_t placementId_;
    double oldStartSeconds_;
    double newStartSeconds_;
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

} // namespace OpenTune
