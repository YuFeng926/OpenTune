#include "PitchShiftEditAction.h"
#include "Content/ContentEditCommands.h"

namespace OpenTune {

PitchShiftEditAction::PitchShiftEditAction(std::shared_ptr<ContentEditCommands> commands,
                                           ContentKey key,
                                           PitchShiftSettings oldSettings,
                                           PitchShiftSettings newSettings)
    : commands_(std::move(commands))
    , contentKey_(key)
    , oldSettings_(oldSettings)
    , newSettings_(newSettings)
{
    const int totalOld = oldSettings_.semitone * 100 + oldSettings_.cents;
    const int totalNew = newSettings_.semitone * 100 + newSettings_.cents;
    if (totalNew > totalOld)
        description_ = juce::String::fromUTF8(u8"Pitch Shift +") + juce::String(newSettings_.semitone) + "st";
    else if (totalNew < totalOld)
        description_ = juce::String::fromUTF8(u8"Pitch Shift ") + juce::String(newSettings_.semitone) + "st";
    else
        description_ = "Pitch Shift";
}

void PitchShiftEditAction::undo()
{
    applySettings(oldSettings_);
}

void PitchShiftEditAction::redo()
{
    applySettings(newSettings_);
}

void PitchShiftEditAction::applySettings(const PitchShiftSettings& settings)
{
    if (commands_ != nullptr)
        commands_->setPitchShiftSettings(contentKey_, settings);
}

} // namespace OpenTune
