#pragma once

#include "UndoManager.h"
#include "PitchShiftSettings.h"
#include "Content/ContentKey.h"
#include "Content/ContentEditCommands.h"
#include <memory>
#include <cstdint>

namespace OpenTune {

class PitchShiftEditAction : public UndoAction {
public:
    PitchShiftEditAction(std::shared_ptr<ContentEditCommands> commands,
                         ContentKey key,
                         PitchShiftSettings oldSettings,
                         PitchShiftSettings newSettings);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

    ContentKey getContentKey() const { return contentKey_; }

private:
    void applySettings(const PitchShiftSettings& settings);

    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String description_;
    PitchShiftSettings oldSettings_;
    PitchShiftSettings newSettings_;
};

} // namespace OpenTune
