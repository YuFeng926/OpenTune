// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>

#include <juce_audio_devices/juce_audio_devices.h>

namespace OpenTune::AudioDeviceSettings {

struct DeviceState
{
    bool open = false;
    bool running = false;
    juce::String deviceType;
    juce::String outputDevice;
    juce::String configuredOutputDevice;
    double sampleRateHz = 0.0;
    int bufferSizeFrame = 0;
    std::optional<int> outputChannelPair;
    juce::StringArray outputChannelNames;
    juce::BigInteger activeOutputChannels;
    int outputChannelCount = 0;
};

struct Available
{
    juce::StringArray deviceTypes;
    juce::StringArray outputDevices;
    juce::Array<double> sampleRatesHz;
    juce::Array<int> bufferSizesFrame;
    int outputChannelPairCount = 0;
};

struct Snapshot
{
    DeviceState device;
    Available available;
};

struct Request
{
    std::optional<juce::String> deviceType;
    std::optional<juce::String> outputDevice;
    std::optional<double> sampleRateHz;
    std::optional<int> bufferSizeFrame;
    // -1 disables every output channel; non-negative values select one pair.
    std::optional<int> outputChannelPair;
};

enum class RejectedSetting {
    none,
    deviceType,
    outputDevice,
    noAudioDevice,
    sampleRate,
    bufferSize,
    outputChannelPair
};

enum class ApplyKind {
    applied,
    unavailable,
    openFailed,
    priorSetupNotRestored
};

enum class Recovery {
    notApplicable,
    unchanged,
    restored,
    failed
};

struct ApplyResult
{
    ApplyKind kind = ApplyKind::applied;
    ApplyKind cause = ApplyKind::applied;
    RejectedSetting rejectedSetting = RejectedSetting::none;
    Recovery recovery = Recovery::notApplicable;
    juce::String deviceError;
    juce::String recoveryError;
    bool asioRateRestoreFailed = false;
    DeviceState before;
    DeviceState after;
};

using AsioRateRestore = std::function<bool(const juce::String& reason)>;

Snapshot scanAndRead(juce::AudioDeviceManager& manager, const juce::String& browsedType = {});
DeviceState readDevice(juce::AudioDeviceManager& manager);
int findSampleRateIndex(const juce::Array<double>& sampleRates, double sampleRateHz);
bool sameSampleRate(double firstHz, double secondHz);
ApplyResult apply(juce::AudioDeviceManager& manager, const Request& request,
                  const AsioRateRestore& restoreAsioRate = {});

} // namespace OpenTune::AudioDeviceSettings
