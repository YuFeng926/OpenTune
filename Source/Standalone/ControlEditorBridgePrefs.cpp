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

#if JUCE_WINDOWS
#include <audioclient.h>
#include <mmdeviceapi.h>
#endif

namespace OpenTune {

namespace {

/** The sample rate of the Windows shared mode of the default render device, in Hz; 0 when it cannot be read. It is the
    rate that restoreAsioSampleRateToSystem() moves an ASIO device to (its getSystemDefaultRenderSampleRate(), a
   function of an unnamed namespace of StandaloneAudioDeviceSync.cpp that cannot be called from here, so the query is
   made again, with the same calls). Message thread. */
double getSystemDefaultRenderSampleRate()
{
#if JUCE_WINDOWS
    double result = 0.0;

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitHere = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) {
            IAudioClient* client = nullptr;
            if (SUCCEEDED(
                    device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
                WAVEFORMATEX* mixFormat = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&mixFormat)) && mixFormat != nullptr) {
                    result = static_cast<double>(mixFormat->nSamplesPerSec);
                    CoTaskMemFree(mixFormat);
                }
                client->Release();
            }
            device->Release();
        }
        enumerator->Release();
    }

    if (coInitHere)
        CoUninitialize();

    return result;
#else
    // restoreAsioSampleRateToSystem() restores nothing here, so no restore fails and nobody asks.
    return 0.0;
#endif
}

} // namespace

void ControlEditorBridge::onPreferencesChanged()
{
    // OpenTuneAudioProcessorEditor::syncSharedAppPreferences(): the call that
    // every preference page of the editor makes after one of its controls changed
    // a preference, and that the editor's heartbeat repeats; it hands the
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
        failure.sampleRateHz = device->getCurrentSampleRate();
    }
    // restoreAsioSampleRateToSystem(): the audio settings page calls it before it
    // closes an ASIO device for another type or output device. It acts on the
    // manager the call works on (audioDevice()). The page goes on when it fails
    // (it shows kAudioDeviceRestoreWarning), so the caller reports the result.
    if (restoreAsioSampleRateToSystem(manager, juce::String(reason)))
        return std::nullopt;
    // The rate the restore tried to set is the system's. If that rate cannot be read either, the restore tried no rate,
    // and the failure keeps the rate the device was running at.
    if (const double systemRate = getSystemDefaultRenderSampleRate(); systemRate > 0.0)
        failure.sampleRateHz = systemRate;
    return failure;
}

} // namespace OpenTune
