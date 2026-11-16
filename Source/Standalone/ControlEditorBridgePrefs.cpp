// SPDX-License-Identifier: AGPL-3.0-or-later
// The preferences and the audio device of the editor's side of ControlUiBridge:
// what the editor does around a change of the shared preferences or of the
// audio setup that the control API makes through AppPreferences and the device
// manager itself (Control/Ops/PrefsOps.cpp, Control/Ops/AudioDeviceOps.cpp).
#include "ControlEditorBridge.h"

#include "PluginEditor.h"
#include "StandaloneAudioDeviceSync.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
// After the modules it uses, as in PluginEditor.cpp.
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace OpenTune {

void ControlEditorBridge::onPreferencesChanged()
{
    // OpenTuneAudioProcessorEditor::syncSharedAppPreferences(): the call that
    // every preference page of the editor makes after one of its controls changed
    // a preference; it hands the
    // preferences to the piano roll, the parameter panel, the arrangement and the
    // menu bar.
    editor_.syncSharedAppPreferences();
}

std::optional<bool> ControlEditorBridge::editingSchemeApplied(bool notesPrimary) const
{
    // appliedAudioEditingScheme_ is what syncSharedAppPreferences() last handed
    // to the views.
    return AudioEditingScheme::usesNotesPrimaryScheme(editor_.appliedAudioEditingScheme_) == notesPrimary;
}

std::optional<ControlUiBridge::AsioRateRestoreFailure>
ControlEditorBridge::prepareAudioDeviceSwitch(juce::AudioDeviceManager& manager, const std::string& reason)
{
    // The device the restore is about to act on, named as audio.get names it (the output device of the setup), and the
    // rate it runs at: read before the restore, which reopens the device.
    AsioRateRestoreFailure failure;
    if (auto* device = manager.getCurrentAudioDevice()) {
        auto output = manager.getAudioDeviceSetup().outputDeviceName;
        if (output.isEmpty())
            output = device->getName();
        failure.deviceName = output.toStdString();
    }
    // restoreAsioSampleRateToSystem(): the audio settings page calls it before it
    // closes an ASIO device for another type or output device. It acts on the
    // manager the call works on (audioDevice()). The page goes on when it fails
    // (it shows kAudioDeviceRestoreWarning), so the caller reports the result.
    if (restoreAsioSampleRateToSystem(manager, juce::String(reason), &failure.sampleRateHz))
        return std::nullopt;
    return failure;
}

} // namespace OpenTune
