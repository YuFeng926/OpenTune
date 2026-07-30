#include "PitchShiftEditAction.h"
#include "Content/ContentEditCommands.h"

namespace OpenTune {

PitchShiftEditAction::PitchShiftEditAction(std::shared_ptr<ContentEditCommands> commands,
                                           ContentKey key,
                                           PitchShiftEditState before,
                                           PitchShiftEditState after)
    : commands_(std::move(commands))
    , contentKey_(key)
    , before_(std::move(before))
    , after_(std::move(after))
{
    const double totalOld = before_.settings.getTotalCents();
    const double totalNew = after_.settings.getTotalCents();
    if (totalNew > totalOld)
        description_ = juce::String::fromUTF8(u8"Pitch Shift +") + juce::String(after_.settings.semitone) + "st";
    else if (totalNew < totalOld)
        description_ = juce::String::fromUTF8(u8"Pitch Shift ") + juce::String(after_.settings.semitone) + "st";
    else
        description_ = "Pitch Shift";
}

void PitchShiftEditAction::undo()
{
    commands_->applyPitchShiftState(contentKey_, before_);
}

void PitchShiftEditAction::redo()
{
    commands_->applyPitchShiftState(contentKey_, after_);
}

} // namespace OpenTune
