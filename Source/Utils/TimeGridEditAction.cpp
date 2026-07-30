#include "TimeGridEditAction.h"
#include "Content/ContentEditCommands.h"

namespace OpenTune {

TimeGridEditAction::TimeGridEditAction(std::shared_ptr<ContentEditCommands> commands,
                                       ContentKey key,
                                       juce::String description,
                                       std::shared_ptr<const TimeGridSnapshot> oldSnapshot,
                                       std::shared_ptr<const TimeGridSnapshot> newSnapshot)
    : commands_(commands)
    , contentKey_(key)
    , description_(std::move(description))
    , oldSnapshot_(std::move(oldSnapshot))
    , newSnapshot_(std::move(newSnapshot))
{
    jassert(contentKey_.isValid());
    jassert(oldSnapshot_ != nullptr);
    jassert(newSnapshot_ != nullptr);
}

void TimeGridEditAction::undo()
{
    commands_->setTimeGrid(contentKey_, oldSnapshot_);
}

void TimeGridEditAction::redo()
{
    commands_->setTimeGrid(contentKey_, newSnapshot_);
}

} // namespace OpenTune
