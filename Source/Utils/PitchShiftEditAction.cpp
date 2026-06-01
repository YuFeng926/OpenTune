#include "PitchShiftEditAction.h"
#include "../PluginProcessor.h"

namespace OpenTune {

PitchShiftEditAction::PitchShiftEditAction(OpenTuneAudioProcessor& processor,
                                           uint64_t materializationId,
                                           PitchShiftSettings oldSettings,
                                           PitchShiftSettings newSettings)
    : processor_(processor)
    , materializationId_(materializationId)
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
    processor_.setPitchShiftSettings(materializationId_, settings);
}

} // namespace OpenTune
