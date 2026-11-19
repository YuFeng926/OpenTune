// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioDeviceSettings.h"

#include <cmath>

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

void scanType(juce::AudioIODeviceType* type)
{
    if (type != nullptr)
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
    if (currentType == "ASIO" && currentType != type) {
        restoreAsio("audio type rollback");
    } else if (currentType == "ASIO"
               && manager.getAudioDeviceSetup().outputDeviceName != setup.outputDeviceName) {
        restoreAsio("audio output rollback");
    }

    error = manager.setCurrentAudioDeviceTypeAndSetup(type, setup, true, hadDevice);
    if (error.isNotEmpty())
        return false;
    if (manager.getCurrentAudioDeviceType() != type) {
        error = "the previous audio device type could not be restored";
        return false;
    }
    if (hadDevice && manager.getCurrentAudioDevice() == nullptr) {
        error = "the previous audio device did not reopen";
        return false;
    }
    if (!hadDevice && manager.getCurrentAudioDevice() != nullptr) {
        error = "the previous audio device should remain closed";
        return false;
    }
    if (manager.getAudioDeviceSetup() != setup) {
        error = "the previous audio setup was not fully restored";
        return false;
    }
    return true;
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
    if (auto* type = manager.getCurrentDeviceTypeObject()) {
        const int outputIndex = type->getIndexOfDevice(device, false);
        const auto outputNames = type->getDeviceNames(false);
        if (outputIndex >= 0 && outputIndex < outputNames.size())
            state.outputDevice = outputNames[outputIndex];
    }
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

Snapshot scanAndRead(juce::AudioDeviceManager& manager, const juce::String& browsedType)
{
    Snapshot snapshot;
    snapshot.device = readDevice(manager);
    for (auto* type : manager.getAvailableDeviceTypes())
        if (type != nullptr)
            snapshot.available.deviceTypes.add(type->getTypeName());

    const auto typeName = browsedType.isEmpty() ? manager.getCurrentAudioDeviceType() : browsedType;
    if (auto* type = findType(manager, typeName)) {
        scanType(type);
        snapshot.available.outputDevices = type->getDeviceNames(false);
    }

    if (auto* device = manager.getCurrentAudioDevice()) {
        snapshot.available.sampleRatesHz = device->getAvailableSampleRates();
        snapshot.available.bufferSizesFrame = device->getAvailableBufferSizes();
        snapshot.available.outputChannelPairCount = outputChannelPairCount(*device);
    }
    return snapshot;
}

ApplyResult apply(juce::AudioDeviceManager& manager, const Request& request, const AsioRateRestore& restoreAsioRate)
{
    scanType(manager.getCurrentDeviceTypeObject());

    const auto priorType = manager.getCurrentAudioDeviceType();
    const auto priorSetup = manager.getAudioDeviceSetup();
    const bool hadDevice = manager.getCurrentAudioDevice() != nullptr;
    const auto before = readDevice(manager);

    auto rejected = [&before](RejectedSetting setting) {
        ApplyResult result;
        result.kind = ApplyKind::unavailable;
        result.cause = ApplyKind::unavailable;
        result.rejectedSetting = setting;
        result.before = before;
        result.after = before;
        return result;
    };

    juce::String targetType = priorType;
    juce::AudioIODeviceType* targetDeviceType = nullptr;
    if (request.outputChannelPair
        && (*request.outputChannelPair < -1 || *request.outputChannelPair > 0))
        return rejected(RejectedSetting::outputChannelPair);

    if (request.deviceType) {
        targetType = *request.deviceType;
        targetDeviceType = findType(manager, targetType);
        if (targetDeviceType == nullptr)
            return rejected(RejectedSetting::deviceType);
    }

    auto targetOutputDevice = request.outputDevice;
    if (request.deviceType && targetType != priorType) {
        scanType(targetDeviceType);

        const auto outputDevices = targetDeviceType->getDeviceNames(false);
        if (!targetOutputDevice) {
            if (outputDevices.size() != 1)
                return rejected(RejectedSetting::outputDevice);
            targetOutputDevice = outputDevices[0];
        }
        if (targetOutputDevice->isNotEmpty() && !outputDevices.contains(*targetOutputDevice))
            return rejected(RejectedSetting::outputDevice);
    }
    else if (targetOutputDevice && targetOutputDevice->isNotEmpty()) {
        auto* type = findType(manager, targetType);
        if (type == nullptr || !type->getDeviceNames(false).contains(*targetOutputDevice))
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
                manager.closeAudioDevice();
                result.kind = ApplyKind::priorSetupNotRestored;
                result.recovery = Recovery::failed;
            }
        }
        result.asioRateRestoreFailed = asioRateRestoreFailed;
        result.after = readDevice(manager);
        return result;
    };

    const bool typeChanged = targetType != priorType;
    const auto currentOutputDevice = hadDevice ? before.outputDevice : priorSetup.outputDeviceName;
    const bool outputChanged = request.outputDevice && *request.outputDevice != currentOutputDevice;
    const bool reopenClosedSelection = !hadDevice && request.outputDevice.has_value();
    const bool changesTarget = typeChanged || outputChanged;
    auto setup = changesTarget ? juce::AudioDeviceManager::AudioDeviceSetup{} : priorSetup;
    bool needsApply = changesTarget || reopenClosedSelection;

    if (typeChanged) {
        if (targetOutputDevice && targetOutputDevice->isNotEmpty()) {
            setup.outputDeviceName = *targetOutputDevice;
            if (!targetDeviceType->hasSeparateInputsAndOutputs())
                setup.inputDeviceName = *targetOutputDevice;
        }
        setup.sampleRate = request.sampleRateHz.value_or(0.0);
        setup.bufferSize = request.bufferSizeFrame.value_or(0);
    } else if (outputChanged) {
        const auto& name = *request.outputDevice;
        auto* type = manager.getCurrentDeviceTypeObject();
        setup.outputChannels.clear();
        setup.useDefaultOutputChannels = true;
        setup.outputDeviceName = name;
        setup.sampleRate = request.sampleRateHz.value_or(0.0);
        setup.bufferSize = request.bufferSizeFrame.value_or(0);
        if (name.isEmpty()) {
            if (type == nullptr || !type->hasSeparateInputsAndOutputs())
                setup.inputDeviceName.clear();
        } else if (type == nullptr || !type->hasSeparateInputsAndOutputs()) {
            setup.inputDeviceName = name;
        }
    }

    const bool modifyingCurrentDevice = !changesTarget && !reopenClosedSelection;
    auto* currentDevice = modifyingCurrentDevice ? manager.getCurrentAudioDevice() : nullptr;
    if (modifyingCurrentDevice
        && (request.sampleRateHz || request.bufferSizeFrame || request.outputChannelPair)
        && currentDevice == nullptr)
        return fail(ApplyKind::unavailable, RejectedSetting::noAudioDevice, {});

    if (request.sampleRateHz) {
        if (modifyingCurrentDevice) {
            const auto offered = offeredSampleRate(*currentDevice, *request.sampleRateHz);
            if (!offered)
                return fail(ApplyKind::unavailable, RejectedSetting::sampleRate, {});
            if (std::abs(*offered - currentDevice->getCurrentSampleRate()) >= kSameRateHz) {
                setup.sampleRate = *offered;
                needsApply = true;
            }
        } else if (!typeChanged && !outputChanged) {
            setup.sampleRate = *request.sampleRateHz;
            needsApply = true;
        }
    }

    if (request.bufferSizeFrame) {
        if (modifyingCurrentDevice) {
            const int size = *request.bufferSizeFrame;
            if (!currentDevice->getAvailableBufferSizes().contains(size))
                return fail(ApplyKind::unavailable, RejectedSetting::bufferSize, {});
            if (size != currentDevice->getCurrentBufferSizeSamples()) {
                setup.bufferSize = size;
                needsApply = true;
            }
        } else if (!typeChanged && !outputChanged) {
            setup.bufferSize = *request.bufferSizeFrame;
            needsApply = true;
        }
    }

    if (request.outputChannelPair) {
        const int pair = *request.outputChannelPair;
        if (modifyingCurrentDevice) {
            if (pair < -1 || (pair >= 0 && pair >= outputChannelPairCount(*currentDevice)))
                return fail(ApplyKind::unavailable, RejectedSetting::outputChannelPair, {});

            const int active = currentDevice->getActiveOutputChannels().findNextSetBit(0);
            if (pair < 0) {
                if (active >= 0) {
                    setup.outputChannels.clear();
                    setup.useDefaultOutputChannels = false;
                    needsApply = true;
                }
            } else if (active < 0 || active / 2 != pair) {
                setup.outputChannels.clear();
                setup.outputChannels.setBit(pair * 2);
                if (pair * 2 + 1 < outputChannelCount(*currentDevice))
                    setup.outputChannels.setBit(pair * 2 + 1);
                setup.useDefaultOutputChannels = false;
                needsApply = true;
            }
        } else {
            setup.outputChannels.clear();
            setup.useDefaultOutputChannels = false;
            if (pair >= 0) {
                setup.outputChannels.setBit(pair * 2);
                setup.outputChannels.setBit(pair * 2 + 1);
            }
            needsApply = true;
        }
    }

    if (needsApply) {
        if (typeChanged && priorType == "ASIO")
            restoreAsio("audio type switch");
        else if (outputChanged && priorType == "ASIO")
            restoreAsio("audio output switch");

        changed = true;
        const auto error = manager.setCurrentAudioDeviceTypeAndSetup(targetType, setup, true, true);
        if (error.isNotEmpty()) {
            const auto setting = typeChanged
                ? (request.outputDevice ? RejectedSetting::outputDevice : RejectedSetting::deviceType)
                : ((outputChanged || reopenClosedSelection) ? RejectedSetting::outputDevice
                                                             : RejectedSetting::none);
            return fail(ApplyKind::openFailed, setting, error);
        }
    }

    ApplyResult result;
    result.asioRateRestoreFailed = asioRateRestoreFailed;
    result.before = before;
    result.after = readDevice(manager);
    return result;
}

} // namespace OpenTune::AudioDeviceSettings
