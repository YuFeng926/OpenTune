#include "HostIntegration.h"
#include "../Standalone/UI/UIColors.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace OpenTune {

class HostIntegrationStandalone final : public HostIntegration {
public:
    void configureInitialState(OpenTuneAudioProcessor& processor) override
    {
        juce::ignoreUnused(processor);
    }

    bool processIfApplicable(OpenTuneAudioProcessor& processor,
                             juce::AudioBuffer<float>& buffer,
                             int totalNumInputChannels,
                             int totalNumOutputChannels,
                             int numSamples) override
    {
        juce::ignoreUnused(processor, buffer, totalNumInputChannels, totalNumOutputChannels, numSamples);
        return false;
    }

    void audioSettingsRequested(juce::AudioProcessorEditor& editor) override
    {
        juce::ignoreUnused(editor);
        if (auto* holder = juce::StandalonePluginHolder::getInstance()) {
            // Use custom AudioDeviceSelectorComponent to exclude "Mute Input" option
            // This replaces the default holder->showAudioSettingsDialog() which includes the unwanted option
            int minInputCh = 0;
            int maxInputCh = 256;
            int minOutputCh = 0;
            int maxOutputCh = 256;

            auto* selector = new juce::AudioDeviceSelectorComponent(
                holder->deviceManager,
                minInputCh, maxInputCh,
                minOutputCh, maxOutputCh,
                true, // showMidiInputOptions
                true, // showMidiOutputChannels
                true, // showChannelsAsStereoPairs
                false // hideAdvancedOptionsWithButton
            );

            selector->setSize(500, 450);

            juce::DialogWindow::LaunchOptions o;
            o.content.setOwned(selector);
            o.dialogTitle = "Audio Settings";
            o.dialogBackgroundColour = UIColors::backgroundDark;
            o.escapeKeyTriggersCloseButton = true;
            o.useNativeTitleBar = false;
            o.resizable = false;
            
            o.launchAsync();
            return;
        }

        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Audio Settings",
            "Audio settings are not available."
        );
    }
};

std::unique_ptr<HostIntegration> createHostIntegration()
{
    return std::make_unique<HostIntegrationStandalone>();
}

} // namespace OpenTune
