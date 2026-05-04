#if JucePlugin_Build_Standalone

#include "Editor/EditorFactory.h"
#include "Standalone/PluginEditor.h"
#include "PluginProcessor.h"

namespace OpenTune {

juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor& processor)
{
    return new OpenTuneAudioProcessorEditor(processor);
}

} // namespace OpenTune

#endif // JucePlugin_Build_Standalone
