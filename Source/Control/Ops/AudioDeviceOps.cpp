// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioDeviceOps.h"

#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Control/ControlUiBridge.h"
#include "Utils/AppLogger.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

/** Longest device type and device name (maxLength of the schemas, in code points). */
constexpr std::size_t kMaxTypeCodePoints = 64;
constexpr std::size_t kMaxDeviceCodePoints = 256;
/** Two sample rates closer than this are the same (the audio settings page compares with 0.01 Hz). */
constexpr double kSameRateHz = 0.01;

juce::String fromUtf8(const std::string& text)
{
    return juce::String::fromUTF8(text.c_str(), static_cast<int>(text.size()));
}

// ---- What the audio settings page shows ------------------------------------------------------------------

/** The output channels of a device as the page counts them: the Standalone application is configured for at most two
    (AudioDeviceSettingsContent::refreshChannelToggles and channelPairClicked). */
int outputChannelCount(juce::AudioIODevice& device)
{
    return juce::jmin(
        2, juce::jmax(device.getOutputChannelNames().size(), device.getActiveOutputChannels().getHighestBit() + 1));
}

/** How many output channel pairs the page offers (refreshChannelToggles): one toggle per pair of channels. */
int outputChannelPairCount(juce::AudioIODevice& device)
{
    const int channels = outputChannelCount(device);
    return channels <= 0 ? 0 : (channels + 1) / 2;
}

/** The device that runs, as audio.get reports it. */
struct DeviceView
{
    bool running = false;
    std::string deviceType;
    std::string outputDevice;
    double sampleRateHz = 0.0;
    int bufferSizeFrame = 0;
    /** nullopt when the device reports no active output channel. */
    std::optional<int> outputChannelPair;
};

DeviceView readDevice(juce::AudioDeviceManager& manager)
{
    DeviceView view;
    // AudioDeviceManager::getCurrentAudioDevice(): the open device, nullptr when none is; a device that is open but not
    // playing is not a running device.
    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr || !device->isPlaying())
        return view;
    view.running = true;
    view.deviceType = manager.getCurrentAudioDeviceType().toStdString();
    // The page selects the output device by the name in the setup.
    auto output = manager.getAudioDeviceSetup().outputDeviceName;
    if (output.isEmpty())
        output = device->getName();
    view.outputDevice = output.toStdString();
    view.sampleRateHz = device->getCurrentSampleRate();
    view.bufferSizeFrame = device->getCurrentBufferSizeSamples();
    const int firstChannel = device->getActiveOutputChannels().findNextSetBit(0);
    if (firstChannel >= 0)
        view.outputChannelPair = firstChannel / 2;
    return view;
}

/** What the page offers, for the current device type and device. */
struct Available
{
    std::vector<std::string> deviceTypes;
    std::vector<std::string> outputDevices;
    std::vector<double> sampleRatesHz;
    std::vector<int> bufferSizesFrame;
    int outputChannelPairCount = 0;
};

/** AudioDeviceSettingsContent::refresh() scans the current type for devices before it fills the selectors. */
void rescanDevices(juce::AudioDeviceManager& manager)
{
    if (auto* type = manager.getCurrentDeviceTypeObject())
        type->scanForDevices();
}

Available readAvailable(juce::AudioDeviceManager& manager)
{
    Available available;
    // refreshTypeSelector(): every device type of the manager.
    for (auto* type : manager.getAvailableDeviceTypes())
        if (type != nullptr)
            available.deviceTypes.push_back(type->getTypeName().toStdString());
    // refreshDeviceSelector(): the output devices of the current type.
    if (auto* type = manager.getCurrentDeviceTypeObject())
        for (const auto& name : type->getDeviceNames(false))
            available.outputDevices.push_back(name.toStdString());
    // refreshSampleRateSelector(), refreshBufferSizeSelector() and refreshChannelToggles(): what the open device
    // offers.
    if (auto* device = manager.getCurrentAudioDevice()) {
        for (const double rate : device->getAvailableSampleRates())
            available.sampleRatesHz.push_back(rate);
        for (const int size : device->getAvailableBufferSizes())
            available.bufferSizesFrame.push_back(size);
        available.outputChannelPairCount = outputChannelPairCount(*device);
    }
    return available;
}

JsonValue textArray(const std::vector<std::string>& values, std::size_t maxCodePoints)
{
    auto json = JsonValue::array();
    for (const auto& value : values)
        json.append(controlJsonText(value, maxCodePoints));
    return json;
}

JsonValue availableJson(const Available& available)
{
    auto json = JsonValue::object();
    json.appendMember("deviceTypes", textArray(available.deviceTypes, kMaxTypeCodePoints));
    json.appendMember("outputDevices", textArray(available.outputDevices, kMaxDeviceCodePoints));
    auto rates = JsonValue::array();
    for (const double rate : available.sampleRatesHz)
        rates.append(controlJsonNumber(rate, 3));
    json.appendMember("sampleRatesHz", std::move(rates));
    auto sizes = JsonValue::array();
    for (const int size : available.bufferSizesFrame)
        sizes.append(JsonValue::integer(size));
    json.appendMember("bufferSizesFrame", std::move(sizes));
    json.appendMember("outputChannelPairCount", JsonValue::integer(available.outputChannelPairCount));
    return json;
}

/** The device member of audio.get and audio.set (audio.get.schema.json $defs.device). nullReason says why the members
    are null when no device runs: no_audio_device, no_editor or unavailable. */
JsonValue deviceJson(const DeviceView& view, const std::string& nullReason)
{
    auto json = JsonValue::object();
    json.appendMember("running", JsonValue::boolean(view.running));
    auto nullReasons = JsonValue::object();
    const auto member = [&](const char* name, JsonValue value, bool present) {
        if (present) {
            json.appendMember(name, std::move(value));
            return;
        }
        json.appendMember(name, JsonValue());
        nullReasons.appendMember(name, JsonValue::string(view.running ? "unavailable" : nullReason));
    };
    member("deviceType", controlJsonText(view.deviceType, kMaxTypeCodePoints), view.running);
    member("outputDevice", controlJsonText(view.outputDevice, kMaxDeviceCodePoints), view.running);
    member("sampleRateHz", controlJsonNumber(view.sampleRateHz, 3), view.running && view.sampleRateHz > 0.0);
    member("bufferSizeFrame", JsonValue::integer(view.bufferSizeFrame), view.running && view.bufferSizeFrame > 0);
    member("outputChannelPair", JsonValue::integer(view.outputChannelPair.value_or(0)),
           view.running && view.outputChannelPair.has_value());
    if (nullReasons.size() != 0)
        json.appendMember("nullReasons", std::move(nullReasons));
    return json;
}

// ---- audio.get -------------------------------------------------------------------------------------------

Result<JsonValue> runAudioGet(ControlOpContext& context, const ControlNoParams&)
{
    auto* bridge = context.bridge();
    // ControlUiBridge::audioDevice(): the Standalone application's device manager, owned by the editor.
    auto* manager = bridge != nullptr ? bridge->audioDevice() : nullptr;
    auto result = JsonValue::object();
    if (manager == nullptr) {
        result.appendMember("device", deviceJson(DeviceView{}, bridge == nullptr ? "no_editor" : "unavailable"));
        result.appendMember("available", availableJson(Available{}));
        return result;
    }
    rescanDevices(*manager);
    result.appendMember("device", deviceJson(readDevice(*manager), "no_audio_device"));
    result.appendMember("available", availableJson(readAvailable(*manager)));
    return result;
}

// ---- audio.set: params -----------------------------------------------------------------------------------

/** methods/audio.set.schema.json $defs.params: the members that are present. */
struct AudioSetRequest
{
    std::optional<std::string> deviceType;
    std::optional<std::string> outputDevice;
    std::optional<double> sampleRateHz;
    std::optional<std::int64_t> bufferSizeFrame;
    std::optional<std::int64_t> outputChannelPair;
};

bool readName(ControlParamReader& reader, const char* name, std::size_t maxCodePoints, std::optional<std::string>& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    if (!value->isString() || value->text().empty() || utf8CodePointCount(value->text()) > maxCodePoints)
        return reader.fail(std::string(name) + " must be a string of 1 to " + std::to_string(maxCodePoints)
                           + " characters");
    out = value->text();
    return true;
}

/** Checks the params of audio.set on the connection thread: a closed object with at least one member. */
bool parseAudioSet(const JsonValue& params, AudioSetRequest& out, std::string& why)
{
    ControlParamReader reader(params);
    constexpr auto kMaxInteger = static_cast<std::int64_t>(std::numeric_limits<int>::max());
    if (!reader.onlyMembers({"deviceType", "outputDevice", "sampleRateHz", "bufferSizeFrame", "outputChannelPair"})
        || !readName(reader, "deviceType", kMaxTypeCodePoints, out.deviceType)
        || !readName(reader, "outputDevice", kMaxDeviceCodePoints, out.outputDevice)
        || !reader.optionalNumber("sampleRateHz", std::numeric_limits<double>::min(), 1.0e9, out.sampleRateHz)
        || !reader.optionalInteger("bufferSizeFrame", 1, kMaxInteger, out.bufferSizeFrame)
        || !reader.optionalInteger("outputChannelPair", 0, kMaxInteger, out.outputChannelPair)) {
        why = reader.why();
        return false;
    }
    if (!out.deviceType && !out.outputDevice && !out.sampleRateHz && !out.bufferSizeFrame && !out.outputChannelPair) {
        why = "params must have at least one member";
        return false;
    }
    return true;
}

// ---- audio.set: the steps of the settings page ----------------------------------------------------------

/** The device manager before the call: what a failed call restores. */
struct PriorSetup
{
    juce::String type;
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    /** A device was open (the page's hadDevice). */
    bool hadDevice = false;
    DeviceView view;
};

/** How the steps ended. */
struct Outcome
{
    enum class Kind {
        Ok,
        /** A value is not on offer; nothing is changed (the previous setup was put back where a step had begun). */
        Unavailable,
        /** The device could not be opened; the previous setup was put back. */
        OpenFailed,
        /** The device could not be opened, and the previous setup could not be put back either. */
        NotRestored
    };
    Kind kind = Kind::Ok;
    /** Printable ASCII, no names taken from the system. */
    std::string detail;
    /** Kind::Ok only: an ASIO device was left for another type or output device and its sample rate could not be
        restored to the system's. The change went on (the page treats the restore as best effort); the result says so
        with the warning ASIO_RATE_NOT_RESTORED, whose data is this device and rate. */
    std::optional<ControlUiBridge::AsioRateRestoreFailure> asioRateNotRestored;
};

/**
 * AudioDeviceSettingsContent::applySetup(): opens the device with the setup and treats it as the chosen one. The page
 * rolls back to the setup before the change when this fails; restoreToPrior() does so for the whole call. Returns the
 * manager's error text, empty on success.
 */
juce::String applySetup(juce::AudioDeviceManager& manager, const juce::AudioDeviceManager::AudioDeviceSetup& setup)
{
    auto error = manager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
        AppLogger::logNoThrow("control API: audio.set: the audio device could not be opened with the new setup");
    return error;
}

/**
 * Puts the device manager back to the setup of the call's start: the device type first (as typeChanged() does), then
 * the setup (as applySetup()'s rollback does). True when the previous setup is in place, with a device running if one
 * did before.
 */
bool restoreToPrior(juce::AudioDeviceManager& manager, const PriorSetup& prior)
{
    if (manager.getCurrentAudioDeviceType() != prior.type)
        manager.setCurrentAudioDeviceType(prior.type, true);
    if (!prior.hadDevice) {
        // No device ran before the call, so none runs after it: the setup that remained from an earlier failure is not
        // opened again.
        manager.closeAudioDevice();
        return true;
    }
    const auto error = manager.setAudioDeviceSetup(prior.setup, true);
    if (error.isNotEmpty()) {
        AppLogger::logNoThrow("control API: audio.set: the previous audio setup could not be restored");
        return false;
    }
    return manager.getCurrentAudioDevice() != nullptr;
}

/** True when the rate is one of the device's (the page lists them and picks by index). */
std::optional<double> offeredSampleRate(juce::AudioIODevice& device, double rate)
{
    for (const double offered : device.getAvailableSampleRates())
        if (std::abs(offered - rate) < kSameRateHz)
            return offered;
    return std::nullopt;
}

/**
 * Makes the changes of a request one step at a time, as the page does for each selector: the device type
 * (typeChanged), the output device (outputDeviceChanged), then the sample rate, buffer size and output channel pair
 * (sampleRateChanged, bufferSizeChanged, channelPairClicked) in one setup, so that the device opens once for them. A
 * step that fails puts the setup of the call's start back. The type and the output device were checked against the
 * lists before the call began; the values of the last step are checked against the device that is open by then.
 */
Outcome applyRequest(juce::AudioDeviceManager& manager, ControlUiBridge& bridge, const AudioSetRequest& request,
                     const PriorSetup& prior)
{
    // A step touched the device: a failure must put the previous setup back.
    bool changed = false;
    // The detail says what went wrong in this step; what became of the previous setup is added here, so that a value
    // that is not on offer is not reported as a device that could not be opened.
    const auto fail = [&](Outcome::Kind kind, const std::string& reason) {
        if (!changed)
            return Outcome{kind, reason + "; the current setup is unchanged"};
        if (!restoreToPrior(manager, prior))
            return Outcome{Outcome::Kind::NotRestored,
                           reason + ", and the previous setup could not be restored; no device runs"};
        return Outcome{kind, reason + "; the previous setup was restored"};
    };
    const std::string kOpenFailed = "the audio device could not be opened with the new setup";
    // The first failed restore of the call (at most one can happen: the restore leaves an ASIO device, and the
    // second step runs on the type the first one switched to).
    std::optional<ControlUiBridge::AsioRateRestoreFailure> asioRateNotRestored;
    const auto restoreAsioRate = [&](const char* reason) {
        auto failure = bridge.prepareAudioDeviceSwitch(manager, reason);
        if (failure && !asioRateNotRestored)
            asioRateNotRestored = std::move(failure);
    };

    // typeChanged(): ASIO is restored to the system sample rate before it is closed for another type, as best effort:
    // a restore that fails is reported to the user (kAudioDeviceRestoreWarning) and does not stop the switch. The
    // device manager then switches type and opens that type's default device, and a type that does not take over, or a
    // device that does not open although the old one did (or the new type has devices), rolls the switch back.
    if (request.deviceType) {
        const auto requestedType = fromUtf8(*request.deviceType);
        const auto currentType = manager.getCurrentAudioDeviceType();
        if (requestedType != currentType) {
            const bool hadDevice = manager.getCurrentAudioDevice() != nullptr;
            if (currentType == "ASIO")
                restoreAsioRate("audio type switch");
            manager.setCurrentAudioDeviceType(requestedType, true);
            changed = true;
            const bool typeMismatch = manager.getCurrentAudioDeviceType() != requestedType;
            auto* targetType = manager.getCurrentDeviceTypeObject();
            const bool targetHasDevices =
                targetType != nullptr
                && (!targetType->getDeviceNames(false).isEmpty() || !targetType->getDeviceNames(true).isEmpty());
            const bool openFailed = manager.getCurrentAudioDevice() == nullptr && (hadDevice || targetHasDevices);
            if (typeMismatch || openFailed) {
                AppLogger::logNoThrow("control API: audio.set: the device type could not be switched");
                return fail(Outcome::Kind::OpenFailed, kOpenFailed);
            }
        }
    }

    // outputDeviceChanged(): the new device may offer another channel layout, so it opens with its default channels;
    // a device type without separate inputs and outputs takes the output device as its input device as well.
    if (request.outputDevice) {
        const auto name = fromUtf8(*request.outputDevice);
        const auto currentSetup = manager.getAudioDeviceSetup();
        if (manager.getCurrentAudioDevice() == nullptr || currentSetup.outputDeviceName != name) {
            auto setup = currentSetup;
            auto* type = manager.getCurrentDeviceTypeObject();
            setup.outputChannels.clear();
            setup.useDefaultOutputChannels = true;
            setup.outputDeviceName = name;
            if (type == nullptr || !type->hasSeparateInputsAndOutputs())
                setup.inputDeviceName = setup.outputDeviceName;
            // As the type switch: a restore that fails is reported and the change goes on.
            if (manager.getCurrentAudioDeviceType() == "ASIO"
                && setup.outputDeviceName != currentSetup.outputDeviceName)
                restoreAsioRate("audio output switch");
            changed = true;
            if (applySetup(manager, setup).isNotEmpty())
                return fail(Outcome::Kind::OpenFailed, kOpenFailed);
        }
    }

    // sampleRateChanged(), bufferSizeChanged(), channelPairClicked(): the page offers what the open device lists and
    // sets the chosen one into the setup; only what differs from the device is set.
    if (request.sampleRateHz || request.bufferSizeFrame || request.outputChannelPair) {
        auto* device = manager.getCurrentAudioDevice();
        if (device == nullptr)
            return fail(Outcome::Kind::Unavailable, "no audio device is open, so no sample rate, buffer size or "
                                                    "output channel pair is on offer");
        auto setup = manager.getAudioDeviceSetup();
        bool differs = false;
        if (request.sampleRateHz) {
            const auto offered = offeredSampleRate(*device, *request.sampleRateHz);
            if (!offered)
                return fail(Outcome::Kind::Unavailable, "the sample rate is not offered by the audio device");
            if (std::abs(*offered - device->getCurrentSampleRate()) >= kSameRateHz) {
                setup.sampleRate = *offered;
                differs = true;
            }
        }
        if (request.bufferSizeFrame) {
            const int size = static_cast<int>(*request.bufferSizeFrame);
            if (!device->getAvailableBufferSizes().contains(size))
                return fail(Outcome::Kind::Unavailable, "the buffer size is not offered by the audio device");
            if (size != device->getCurrentBufferSizeSamples()) {
                setup.bufferSize = size;
                differs = true;
            }
        }
        if (request.outputChannelPair) {
            const int pair = static_cast<int>(*request.outputChannelPair);
            if (pair >= outputChannelPairCount(*device))
                return fail(Outcome::Kind::Unavailable, "the output channel pair is not offered by the audio device");
            const auto active = device->getActiveOutputChannels().findNextSetBit(0);
            if (active < 0 || active / 2 != pair) {
                // At most two output channels: only the chosen pair is active.
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
            if (applySetup(manager, setup).isNotEmpty())
                return fail(Outcome::Kind::OpenFailed, kOpenFailed);
        }
    }
    Outcome done;
    done.asioRateNotRestored = std::move(asioRateNotRestored);
    return done;
}

// ---- audio.set: the result ------------------------------------------------------------------------------

/** One member of the change of the audioDevice entity: {before, after, unit}; before is null when no device ran. */
void addMemberChange(JsonValue& fields, const char* name, JsonValue before, JsonValue after, JsonValue unit)
{
    auto pair = JsonValue::object();
    pair.appendMember("before", std::move(before));
    pair.appendMember("after", std::move(after));
    pair.appendMember("unit", std::move(unit));
    fields.appendMember(name, std::move(pair));
}

/** Records the change from the device before the call to the device after it: the members that differ, the warning
    GLOBAL_PREFERENCE, and the undo descriptor (no undo entry; the revert recipe is the same call with the previous
    values, when a device ran before). */
void recordChange(ControlOpContext& context, ControlWriteEnvelope& envelope, const DeviceView& before,
                  const DeviceView& after)
{
    if (!before.running && !after.running)
        return;
    auto fields = JsonValue::object();
    auto revert = JsonValue::object();
    // Without a device before the call there is nothing a request could set the previous values from.
    bool revertable = before.running;
    const auto textMember = [&](const char* name, const std::string& was, const std::string& now, std::size_t max) {
        if (before.running && after.running && was == now)
            return;
        addMemberChange(fields, name, before.running ? controlJsonText(was, max) : JsonValue(),
                        after.running ? controlJsonText(now, max) : JsonValue(), JsonValue());
        if (before.running && !was.empty())
            revert.appendMember(name, controlJsonText(was, max));
        else
            revertable = false;
    };
    textMember("deviceType", before.deviceType, after.deviceType, kMaxTypeCodePoints);
    textMember("outputDevice", before.outputDevice, after.outputDevice, kMaxDeviceCodePoints);
    if (!before.running || !after.running || std::abs(before.sampleRateHz - after.sampleRateHz) >= kSameRateHz) {
        addMemberChange(
            fields, "sampleRateHz", before.running ? controlJsonNumber(before.sampleRateHz, 3) : JsonValue(),
            after.running ? controlJsonNumber(after.sampleRateHz, 3) : JsonValue(), JsonValue::string("hz"));
        if (before.running && before.sampleRateHz > 0.0)
            revert.appendMember("sampleRateHz", controlJsonNumber(before.sampleRateHz, 3));
        else
            revertable = false;
    }
    if (!before.running || !after.running || before.bufferSizeFrame != after.bufferSizeFrame) {
        addMemberChange(
            fields, "bufferSizeFrame", before.running ? JsonValue::integer(before.bufferSizeFrame) : JsonValue(),
            after.running ? JsonValue::integer(after.bufferSizeFrame) : JsonValue(), JsonValue::string("frame"));
        if (before.running && before.bufferSizeFrame > 0)
            revert.appendMember("bufferSizeFrame", JsonValue::integer(before.bufferSizeFrame));
        else
            revertable = false;
    }
    if (!before.running || !after.running || before.outputChannelPair != after.outputChannelPair) {
        addMemberChange(fields, "outputChannelPair",
                        before.outputChannelPair ? JsonValue::integer(*before.outputChannelPair) : JsonValue(),
                        after.outputChannelPair ? JsonValue::integer(*after.outputChannelPair) : JsonValue(),
                        JsonValue());
        if (before.outputChannelPair)
            revert.appendMember("outputChannelPair", JsonValue::integer(*before.outputChannelPair));
        else
            revertable = false;
    }
    if (fields.size() == 0)
        return;
    auto ref = JsonValue::object();
    ref.appendMember("singleton", JsonValue::string("audioDevice"));
    envelope.addChange("audioDevice", std::move(ref), JsonValue::string("Audio device"), std::move(fields));
    auto names = JsonValue::array();
    names.append(JsonValue::string("audioDevice"));
    auto data = JsonValue::object();
    data.appendMember("preferences", std::move(names));
    envelope.addWarning("GLOBAL_PREFERENCE", JsonValue(), std::move(data));
    envelope.setNotUndoable("ui_parity", revertable && controlReportsCapability(context, "audio.device")
                                             ? controlRevertRecipe("audio.set", std::move(revert))
                                             : JsonValue());
}

// ---- audio.set ------------------------------------------------------------------------------------------

Result<JsonValue> runAudioSet(ControlOpContext& context, const AudioSetRequest& request)
{
    if (auto busy = controlCheckBusy(context))
        return *busy;
    auto* bridge = context.bridge();
    // PROTOCOL.md section 7 rule 6: without an editor window the method fails with no_editor_window.
    if (bridge == nullptr)
        return controlOpError("UNSUPPORTED", "no_editor_window",
                              "audio.set acts through the editor window, and no editor exists");
    auto* manager = bridge->audioDevice();
    if (manager == nullptr)
        return controlOpError("UNSUPPORTED", "capability_missing",
                              "audio.device needs the editor's audio device manager, and none exists");

    // As the page's refresh(): the lists come from a fresh scan of the current device type.
    rescanDevices(*manager);
    PriorSetup prior;
    prior.type = manager->getCurrentAudioDeviceType();
    prior.setup = manager->getAudioDeviceSetup();
    prior.hadDevice = manager->getCurrentAudioDevice() != nullptr;
    prior.view = readDevice(*manager);

    // The device type and the output device are checked against the lists before anything changes. The values of the
    // device itself (rate, buffer size, channel pair) can only be checked once the device that will run is open.
    juce::String targetType = prior.type;
    if (request.deviceType) {
        targetType = fromUtf8(*request.deviceType);
        bool offered = false;
        for (auto* type : manager->getAvailableDeviceTypes())
            offered = offered || (type != nullptr && type->getTypeName() == targetType);
        if (!offered)
            return controlOpError("INVALID_ARGUMENT", "audio_setting_unavailable",
                                  "the audio device type is not offered by this instance");
    }
    if (request.outputDevice) {
        juce::AudioIODeviceType* type = nullptr;
        for (auto* candidate : manager->getAvailableDeviceTypes())
            if (candidate != nullptr && candidate->getTypeName() == targetType)
                type = candidate;
        if (type != nullptr && targetType != prior.type)
            type->scanForDevices();
        if (type == nullptr || !type->getDeviceNames(false).contains(fromUtf8(*request.outputDevice)))
            return controlOpError("INVALID_ARGUMENT", "audio_setting_unavailable",
                                  "the output device is not offered by the audio device type");
    }

    ControlWriteEnvelope envelope(context);
    const bool sameAsCurrent =
        prior.view.running && (!request.deviceType || *request.deviceType == prior.view.deviceType)
        && (!request.outputDevice || *request.outputDevice == prior.view.outputDevice)
        && (!request.sampleRateHz || std::abs(*request.sampleRateHz - prior.view.sampleRateHz) < kSameRateHz)
        && (!request.bufferSizeFrame || *request.bufferSizeFrame == prior.view.bufferSizeFrame)
        && (!request.outputChannelPair || prior.view.outputChannelPair == request.outputChannelPair);
    std::optional<ControlUiBridge::AsioRateRestoreFailure> asioRateNotRestored;
    if (!sameAsCurrent) {
        const auto outcome = applyRequest(*manager, *bridge, request, prior);
        switch (outcome.kind) {
            case Outcome::Kind::Ok:
                asioRateNotRestored = outcome.asioRateNotRestored;
                break;
            case Outcome::Kind::Unavailable:
                return controlOpError("INVALID_ARGUMENT", "audio_setting_unavailable", outcome.detail);
            case Outcome::Kind::OpenFailed:
                return controlOpError("IO_ERROR", "device_open_failed", outcome.detail);
            case Outcome::Kind::NotRestored:
                return controlOpErrorAfterChange("IO_ERROR", "device_open_failed", outcome.detail);
        }
    }
    const auto after = readDevice(*manager);
    recordChange(context, envelope, prior.view, after);
    // The sample rate of the ASIO device that was left stays where the device had put it; the switch itself went on.
    // registry/warnings.json: subject {singleton: audioDevice}, data {deviceName, sampleRateHz}.
    if (asioRateNotRestored) {
        auto subject = JsonValue::object();
        subject.appendMember("singleton", JsonValue::string("audioDevice"));
        auto data = JsonValue::object();
        data.appendMember("deviceName", controlJsonText(asioRateNotRestored->deviceName, kMaxDeviceCodePoints));
        data.appendMember("sampleRateHz", controlJsonNumber(asioRateNotRestored->sampleRateHz, 3));
        envelope.addWarning("ASIO_RATE_NOT_RESTORED", std::move(subject), std::move(data));
    }
    auto result = envelope.committed(context);
    result.appendMember("device", deviceJson(after, "no_audio_device"));
    return result;
}

} // namespace

void registerAudioDeviceOps(ControlMethodTable& table)
{
    table.add(controlReadMethod<ControlNoParams>("audio.get", controlParseNoParams, runAudioGet));
    table.add(controlWriteMethod<AudioSetRequest>("audio.set", parseAudioSet, runAudioSet));
}

} // namespace OpenTune
