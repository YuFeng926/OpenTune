// VST3-only TU (attached to OpenTune_VST3); a wrong-target attachment fails fast.
#include "EditorFactory.h"
#include "Plugin/PluginEditor.h"
#include "PluginProcessor.h"

namespace OpenTune {

juce::AudioProcessorEditor* createOpenTuneEditor(OpenTuneAudioProcessor& processor)
{
    return new PluginUI::OpenTuneAudioProcessorEditor(processor);
}

} // namespace OpenTune
