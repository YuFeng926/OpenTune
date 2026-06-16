#pragma once

#include "UndoManager.h"
#include "TimeGrid.h"
#include "Content/ContentKey.h"
#include "Content/ContentEditCommands.h"
#include <memory>

namespace OpenTune {

class TimeGridEditAction : public UndoAction {
public:
    TimeGridEditAction(std::shared_ptr<ContentEditCommands> commands,
                       ContentKey key,
                       juce::String description,
                       std::shared_ptr<const TimeGridSnapshot> oldSnapshot,
                       std::shared_ptr<const TimeGridSnapshot> newSnapshot);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

    ContentKey getContentKey() const noexcept { return contentKey_; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String                    description_;
    std::shared_ptr<const TimeGridSnapshot> oldSnapshot_;
    std::shared_ptr<const TimeGridSnapshot> newSnapshot_;
};

} // namespace OpenTune
