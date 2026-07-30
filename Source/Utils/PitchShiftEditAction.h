#pragma once

#include "UndoManager.h"
#include "PitchShiftSettings.h"
#include "Content/ContentKey.h"
#include <memory>
#include <cstdint>

namespace OpenTune {

class ContentEditCommands;

class PitchShiftEditAction : public UndoAction {
public:
    PitchShiftEditAction(std::shared_ptr<ContentEditCommands> commands,
                         ContentKey key,
                         PitchShiftEditState before,
                         PitchShiftEditState after);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    PitchShiftEditState before_;
    PitchShiftEditState after_;
};

} // namespace OpenTune
