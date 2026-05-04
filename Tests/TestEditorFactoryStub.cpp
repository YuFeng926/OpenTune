#include "Editor/EditorFactory.h"
#include "PluginProcessor.h"

namespace OpenTune {

juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor&)
{
    // Unit tests link shared processor code but never instantiate the real UI shells.
    jassertfalse;
    return nullptr;
}

} // namespace OpenTune
