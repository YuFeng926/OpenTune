#pragma once

#include "UndoManager.h"
#include "TimeGrid.h"
#include "Content/ContentKey.h"
#include "Content/ContentEditCommands.h"
#include <cstdint>
#include <memory>

namespace OpenTune {

class TimeGridEditAction : public UndoAction {
public:
    TimeGridEditAction(std::shared_ptr<ContentEditCommands> commands,
                       ContentKey key,
                       juce::String description,
                       std::shared_ptr<const TimeGridSnapshot> oldSnapshot,
                       std::shared_ptr<const TimeGridSnapshot> newSnapshot,
                       int64_t affectedSrcStartFrame,
                       int64_t affectedSrcEndFrame);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }

    ContentKey getContentKey() const noexcept { return contentKey_; }
    int64_t  getAffectedSrcStartFrame() const noexcept { return affectedSrcStartFrame_; }
    int64_t  getAffectedSrcEndFrame() const noexcept { return affectedSrcEndFrame_; }

private:
    std::shared_ptr<ContentEditCommands> commands_;
    ContentKey contentKey_;
    juce::String                    description_;
    std::shared_ptr<const TimeGridSnapshot> oldSnapshot_;
    std::shared_ptr<const TimeGridSnapshot> newSnapshot_;
    int64_t                         affectedSrcStartFrame_{0};
    int64_t                         affectedSrcEndFrame_{0};
};

} // namespace OpenTune
