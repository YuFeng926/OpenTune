#include "../Source/Editor/EditorFactory.h"

namespace OpenTune {

juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor&)
{
    return nullptr;
}

} // namespace OpenTune
