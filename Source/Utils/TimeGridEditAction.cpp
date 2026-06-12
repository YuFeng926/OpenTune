#include "TimeGridEditAction.h"
#include "Content/ContentEditCommands.h"
#include "AppLogger.h"

namespace OpenTune {

TimeGridEditAction::TimeGridEditAction(std::shared_ptr<ContentEditCommands> commands,
                                       ContentKey key,
                                       juce::String description,
                                       std::shared_ptr<const TimeGridSnapshot> oldSnapshot,
                                       std::shared_ptr<const TimeGridSnapshot> newSnapshot,
                                       int64_t affectedSrcStartFrame,
                                       int64_t affectedSrcEndFrame)
    : commands_(commands)
    , contentKey_(key)
    , description_(std::move(description))
    , oldSnapshot_(std::move(oldSnapshot))
    , newSnapshot_(std::move(newSnapshot))
    , affectedSrcStartFrame_(affectedSrcStartFrame)
    , affectedSrcEndFrame_(affectedSrcEndFrame)
{
    jassert(affectedSrcStartFrame_ >= 0);
    jassert(affectedSrcEndFrame_   >= affectedSrcStartFrame_);
    jassert(contentKey_.isValid());
}

void TimeGridEditAction::undo()
{
    if (oldSnapshot_ == nullptr) {
        AppLogger::warn("[TimeGridEditAction] undo skipped: oldSnapshot_ is null");
        return;
    }
    if (commands_ != nullptr)
        commands_->setTimeGrid(contentKey_,
                               oldSnapshot_,
                               affectedSrcStartFrame_,
                               affectedSrcEndFrame_);
}

void TimeGridEditAction::redo()
{
    if (newSnapshot_ == nullptr) {
        AppLogger::warn("[TimeGridEditAction] redo skipped: newSnapshot_ is null");
        return;
    }
    if (commands_ != nullptr)
        commands_->setTimeGrid(contentKey_,
                               newSnapshot_,
                               affectedSrcStartFrame_,
                               affectedSrcEndFrame_);
}

} // namespace OpenTune
