#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "StandaloneAudioDeviceSync.h"

#include <cmath>

#include "Utils/AppLogger.h"

#if JUCE_WINDOWS
#include <audioclient.h>
#include <mmdeviceapi.h>
#endif

namespace OpenTune {

namespace {

#if JUCE_WINDOWS
double getSystemDefaultRenderSampleRate()
{
    double result = 0.0;

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitHere = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator))))
    {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)))
        {
            IAudioClient* client = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&client))))
            {
                WAVEFORMATEX* mixFormat = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&mixFormat)) && mixFormat != nullptr)
                {
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
}
#endif

juce::String reasonSuffix(const juce::String& reason)
{
    return reason.isNotEmpty() ? " (" + reason + ")" : juce::String();
}

} // namespace

bool restoreAsioSampleRateToSystem(juce::AudioDeviceManager& deviceManager,
                                   const juce::String& reason)
{
#if JUCE_WINDOWS
    if (deviceManager.getCurrentAudioDeviceType() != "ASIO")
        return true;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return true;

    const auto suffix = reasonSuffix(reason);
    const double systemRate = getSystemDefaultRenderSampleRate();
    if (systemRate <= 0.0)
    {
        AppLogger::error("[AudioDeviceSync] cannot read Windows shared render rate" + suffix);
        return false;
    }

    const double currentRate = device->getCurrentSampleRate();
    if (currentRate > 0.0 && std::abs(systemRate - currentRate) < 1.0)
        return true;

    bool supported = false;
    for (const auto rate : device->getAvailableSampleRates())
    {
        if (std::abs(rate - systemRate) < 1.0)
        {
            supported = true;
            break;
        }
    }

    if (!supported)
    {
        AppLogger::error("[AudioDeviceSync] ASIO device cannot use Windows shared render rate: "
                         + juce::String(systemRate, 1) + " Hz" + suffix);
        return false;
    }

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.sampleRate = systemRate;
    const auto error = deviceManager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
    {
        AppLogger::error("[AudioDeviceSync] ASIO sample-rate restore failed: " + error + suffix);
        return false;
    }

    auto* reopenedDevice = deviceManager.getCurrentAudioDevice();
    const double actualRate = reopenedDevice != nullptr
        ? reopenedDevice->getCurrentSampleRate()
        : 0.0;
    if (actualRate <= 0.0 || std::abs(actualRate - systemRate) >= 1.0)
    {
        AppLogger::error("[AudioDeviceSync] ASIO sample-rate restore was not applied: requested="
                         + juce::String(systemRate, 1) + " Hz actual="
                         + juce::String(actualRate, 1) + " Hz" + suffix);
        return false;
    }

    AppLogger::log("[AudioDeviceSync] ASIO sample rate restored: "
                   + juce::String(actualRate, 1) + " Hz" + suffix);
    return true;
#else
    juce::ignoreUnused(deviceManager, reason);
    return true;
#endif
}

} // namespace OpenTune
