#pragma once

#include "UndoManager.h"
#include "PitchShiftSettings.h"
#include <cstdint>

namespace OpenTune {

class OpenTuneAudioProcessor;

/**
 * Pitch Shift 编辑动作（Undo/Redo）
 *
 * clip 级整体移调修饰器的撤销操作。修改 PitchShiftSettings 时记录前后状态，
 * undo/redo 时恢复对应状态并触发全 clip 重渲染。
 */
class PitchShiftEditAction : public UndoAction {
public:
    PitchShiftEditAction(OpenTuneAudioProcessor& processor,
                         uint64_t materializationId,
                         PitchShiftSettings oldSettings,
                         PitchShiftSettings newSettings);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

    uint64_t getMaterializationId() const { return materializationId_; }

private:
    void applySettings(const PitchShiftSettings& settings);

    OpenTuneAudioProcessor& processor_;
    uint64_t materializationId_;
    juce::String description_;
    PitchShiftSettings oldSettings_;
    PitchShiftSettings newSettings_;
};

} // namespace OpenTune
