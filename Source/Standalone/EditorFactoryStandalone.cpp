#include "Editor/EditorFactory.h"
#include "Standalone/PluginEditor.h"
#include "PluginProcessor.h"

namespace OpenTune {

juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor& processor)
{
    return new OpenTuneAudioProcessorEditor(processor);
}

} // namespace OpenTune

