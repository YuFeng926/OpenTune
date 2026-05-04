#if !JucePlugin_Build_Standalone

#include "EditorFactory.h"
#include "Plugin/PluginEditor.h"
#include "PluginProcessor.h"

namespace OpenTune {

juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor& processor)
{
    return new PluginUI::OpenTuneAudioProcessorEditor(processor);
}

} // namespace OpenTune

#endif // !JucePlugin_Build_Standalone
