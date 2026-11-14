// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioDeviceSettings.h"

#include <cmath>

#include "Utils/AppLogger.h"

namespace OpenTune::AudioDeviceSettings {

constexpr double kSameRateHz = 0.01;

int findSampleRateIndex(const juce::Array<double>& sampleRates, double sampleRateHz)
{
    for (int i = 0; i < sampleRates.size(); ++i)
        if (std::abs(sampleRates[i] - sampleRateHz) < kSameRateHz)
            return i;
    return -1;
}

bool sameSampleRate(double firstHz, double secondHz)
{
    return std::abs(firstHz - secondHz) < kSameRateHz;
}

namespace {

int outputChannelCount(juce::AudioIODevice& device)
{
    return juce::jmin(2, juce::jmax(device.getOutputChannelNames().size(),
                                   device.getActiveOutputChannels().getHighestBit() + 1));
}

int outputChannelPairCount(juce::AudioIODevice& device)
{
    const int channels = outputChannelCount(device);
    return channels <= 0 ? 0 : (channels + 1) / 2;
}

void scanCurrentType(juce::AudioDeviceManager& manager)
{
    if (auto* type = manager.getCurrentDeviceTypeObject())
        type->scanForDevices();
}

juce::AudioIODeviceType* findType(juce::AudioDeviceManager& manager, const juce::String& name)
{
    for (auto* type : manager.getAvailableDeviceTypes())
        if (type != nullptr && type->getTypeName() == name)
            return type;
    return nullptr;
}

std::optional<double> offeredSampleRate(juce::AudioIODevice& device, double rate)
{
    const auto sampleRates = device.getAvailableSampleRates();
    const int index = findSampleRateIndex(sampleRates, rate);
    return index >= 0 ? std::optional<double>(sampleRates[index]) : std::nullopt;
}

bool restoreToPrior(juce::AudioDeviceManager& manager, const juce::String& type,
                    const juce::AudioDeviceManager::AudioDeviceSetup& setup, bool hadDevice,
                    const AsioRateRestore& restoreAsioRate, bool& asioRateRestoreFailed, juce::String& error)
{
    const auto restoreAsio = [&](const char* reason) {
        if (restoreAsioRate && !restoreAsioRate(reason))
            asioRateRestoreFailed = true;
    };

    auto currentType = manager.getCurrentAudioDeviceType();
    if (currentType == "ASIO" && currentType != type)
        restoreAsio("audio type rollback");
    if (currentType != type)
        manager.setCurrentAudioDeviceType(type, true);

    if (manager.getCurrentAudioDeviceType() != type) {
        error = "the previous audio device type could not be restored";
        if (!hadDevice)
            manager.closeAudioDevice();
        return false;
    }

    currentType = manager.getCurrentAudioDeviceType();
    const auto currentSetup = manager.getAudioDeviceSetup();
    if (currentType == "ASIO" && currentSetup.outputDeviceName != setup.outputDeviceName)
        restoreAsio("audio output rollback");

    error = manager.setAudioDeviceSetup(setup, true);
    if (!hadDevice)
        manager.closeAudioDevice();
    if (error.isNotEmpty()) {
        AppLogger::logNoThrow("audio device settings: the previous setup could not be restored");
        return false;
    }
    if (hadDevice && manager.getCurrentAudioDevice() == nullptr) {
        error = "the previous audio device did not reopen";
        return false;
    }
    if (manager.getAudioDeviceSetup() != setup) {
        error = "the previous audio setup was not fully restored";
        return false;
    }
    return true;
}

juce::String applySetup(juce::AudioDeviceManager& manager,
                        const juce::AudioDeviceManager::AudioDeviceSetup& setup)
{
    auto error = manager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
        AppLogger::logNoThrow("audio device settings: the requested setup could not be opened");
    return error;
}

} // namespace

DeviceState readDevice(juce::AudioDeviceManager& manager)
{
    DeviceState state;
    state.deviceType = manager.getCurrentAudioDeviceType();
    state.configuredOutputDevice = manager.getAudioDeviceSetup().outputDeviceName;

    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
        return state;

    state.open = true;
    state.running = device->isPlaying();
    state.outputDevice = state.configuredOutputDevice.isEmpty()
        ? device->getName()
        : state.configuredOutputDevice;
    state.sampleRateHz = device->getCurrentSampleRate();
    state.bufferSizeFrame = device->getCurrentBufferSizeSamples();
    state.outputChannelNames = device->getOutputChannelNames();
    state.activeOutputChannels = device->getActiveOutputChannels();
    state.outputChannelCount = outputChannelCount(*device);
    const int firstChannel = state.activeOutputChannels.findNextSetBit(0);
    if (firstChannel >= 0)
        state.outputChannelPair = firstChannel / 2;
    return state;
}

Snapshot scanAndRead(juce::AudioDeviceManager& manager)
{
    scanCurrentType(manager);

    Snapshot snapshot;
    snapshot.device = readDevice(manager);
    for (auto* type : manager.getAvailableDeviceTypes())
        if (type != nullptr)
            snapshot.available.deviceTypes.add(type->getTypeName());

    if (auto* type = manager.getCurrentDeviceTypeObject())
        snapshot.available.outputDevices = type->getDeviceNames(false);

    if (auto* device = manager.getCurrentAudioDevice()) {
        snapshot.available.sampleRatesHz = device->getAvailableSampleRates();
        snapshot.available.bufferSizesFrame = device->getAvailableBufferSizes();
        snapshot.available.outputChannelPairCount = outputChannelPairCount(*device);
    }
    return snapshot;
}

ApplyResult apply(juce::AudioDeviceManager& manager, const Request& request, const AsioRateRestore& restoreAsioRate)
{
    scanCurrentType(manager);

    const auto priorType = manager.getCurrentAudioDeviceType();
    const auto priorSetup = manager.getAudioDeviceSetup();
    const bool hadDevice = manager.getCurrentAudioDevice() != nullptr;
    const auto before = readDevice(manager);

    auto rejected = [](RejectedSetting setting) {
        ApplyResult result;
        result.kind = ApplyKind::unavailable;
        result.cause = ApplyKind::unavailable;
        result.rejectedSetting = setting;
        return result;
    };

    juce::String targetType = priorType;
    if (request.deviceType) {
        targetType = *request.deviceType;
        if (findType(manager, targetType) == nullptr)
            return rejected(RejectedSetting::deviceType);
    }

    if (request.outputDevice && request.outputDevice->isNotEmpty()) {
        auto* type = findType(manager, targetType);
        if (type != nullptr && targetType != priorType)
            type->scanForDevices();
        if (type == nullptr || !type->getDeviceNames(false).contains(*request.outputDevice))
            return rejected(RejectedSetting::outputDevice);
    }

    if (before.running && (!request.deviceType || *request.deviceType == before.deviceType)
        && (!request.outputDevice || *request.outputDevice == before.outputDevice)
        && (!request.sampleRateHz || sameSampleRate(*request.sampleRateHz, before.sampleRateHz))
        && (!request.bufferSizeFrame || *request.bufferSizeFrame == before.bufferSizeFrame)
        && (!request.outputChannelPair || before.outputChannelPair == request.outputChannelPair)) {
        ApplyResult result;
        result.before = before;
        result.after = before;
        return result;
    }

    bool changed = false;
    bool asioRateRestoreFailed = false;
    const auto restoreAsio = [&](const char* reason) {
        if (restoreAsioRate && !restoreAsioRate(reason))
            asioRateRestoreFailed = true;
    };
    const auto fail = [&](ApplyKind kind, RejectedSetting setting, const juce::String& deviceError) {
        ApplyResult result;
        result.kind = kind;
        result.cause = kind;
        result.rejectedSetting = setting;
        result.recovery = Recovery::unchanged;
        result.deviceError = deviceError;
        result.before = before;
        if (changed) {
            result.recovery = Recovery::restored;
            if (!restoreToPrior(manager, priorType, priorSetup, hadDevice, restoreAsioRate,
                                asioRateRestoreFailed, result.recoveryError)) {
                result.kind = ApplyKind::priorSetupNotRestored;
                result.recovery = Recovery::failed;
            }
        }
        result.asioRateRestoreFailed = asioRateRestoreFailed;
        result.after = readDevice(manager);
        return result;
    };

    if (request.deviceType && targetType != priorType) {
        const bool hadOpenDevice = manager.getCurrentAudioDevice() != nullptr;
        if (priorType == "ASIO")
            restoreAsio("audio type switch");
        manager.setCurrentAudioDeviceType(targetType, true);
        changed = true;

        auto* newType = manager.getCurrentDeviceTypeObject();
        const bool targetHasDevices = newType != nullptr
            && (!newType->getDeviceNames(false).isEmpty() || !newType->getDeviceNames(true).isEmpty());
        const bool openFailed = manager.getCurrentAudioDevice() == nullptr && (hadOpenDevice || targetHasDevices);
        if (manager.getCurrentAudioDeviceType() != targetType || openFailed)
            return fail(ApplyKind::openFailed, RejectedSetting::deviceType, {});
    }

    if (request.outputDevice) {
        const auto currentSetup = manager.getAudioDeviceSetup();
        const auto& name = *request.outputDevice;
        if (manager.getCurrentAudioDevice() == nullptr || currentSetup.outputDeviceName != name) {
            auto setup = currentSetup;
            auto* type = manager.getCurrentDeviceTypeObject();
            setup.outputChannels.clear();
            setup.useDefaultOutputChannels = true;
            setup.outputDeviceName = name;
            if (name.isEmpty()) {
                if (type == nullptr || !type->hasSeparateInputsAndOutputs())
                    setup.inputDeviceName.clear();
            } else if (type == nullptr || !type->hasSeparateInputsAndOutputs()) {
                setup.inputDeviceName = name;
            }

            if (manager.getCurrentAudioDeviceType() == "ASIO" && name != currentSetup.outputDeviceName)
                restoreAsio("audio output switch");
            changed = true;
            const auto error = applySetup(manager, setup);
            if (error.isNotEmpty())
                return fail(ApplyKind::openFailed, RejectedSetting::outputDevice, error);
        }
    }

    if (request.sampleRateHz || request.bufferSizeFrame || request.outputChannelPair) {
        auto* device = manager.getCurrentAudioDevice();
        if (device == nullptr)
            return fail(ApplyKind::unavailable, RejectedSetting::noAudioDevice, {});

        auto setup = manager.getAudioDeviceSetup();
        bool differs = false;
        if (request.sampleRateHz) {
            const auto offered = offeredSampleRate(*device, *request.sampleRateHz);
            if (!offered)
                return fail(ApplyKind::unavailable, RejectedSetting::sampleRate, {});
            if (std::abs(*offered - device->getCurrentSampleRate()) >= kSameRateHz) {
                setup.sampleRate = *offered;
                differs = true;
            }
        }
        if (request.bufferSizeFrame) {
            const int size = *request.bufferSizeFrame;
            if (!device->getAvailableBufferSizes().contains(size))
                return fail(ApplyKind::unavailable, RejectedSetting::bufferSize, {});
            if (size != device->getCurrentBufferSizeSamples()) {
                setup.bufferSize = size;
                differs = true;
            }
        }
        if (request.outputChannelPair) {
            const int pair = *request.outputChannelPair;
            if (pair < -1 || (pair >= 0 && pair >= outputChannelPairCount(*device)))
                return fail(ApplyKind::unavailable, RejectedSetting::outputChannelPair, {});

            const int active = device->getActiveOutputChannels().findNextSetBit(0);
            if (pair < 0) {
                if (active >= 0) {
                    setup.outputChannels.clear();
                    setup.useDefaultOutputChannels = false;
                    differs = true;
                }
            } else if (active < 0 || active / 2 != pair) {
                juce::BigInteger channels;
                channels.setBit(pair * 2);
                if (pair * 2 + 1 < outputChannelCount(*device))
                    channels.setBit(pair * 2 + 1);
                setup.outputChannels = channels;
                setup.useDefaultOutputChannels = false;
                differs = true;
            }
        }
        if (differs) {
            changed = true;
            const auto error = applySetup(manager, setup);
            if (error.isNotEmpty())
                return fail(ApplyKind::openFailed, RejectedSetting::none, error);
        }
    }

    ApplyResult result;
    result.asioRateRestoreFailed = asioRateRestoreFailed;
    result.before = before;
    result.after = readDevice(manager);
    return result;
}

} // namespace OpenTune::AudioDeviceSettings
