// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioDeviceOps.h"

#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Control/ControlUiBridge.h"
#include "Utils/AudioDeviceSettings.h"

#include <juce_core/juce_core.h>

#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace OpenTune {

namespace {

/** Longest device type and device name (maxLength of the schemas, in code points). */
constexpr std::size_t kMaxTypeCodePoints = 64;
constexpr std::size_t kMaxDeviceCodePoints = 256;
juce::String fromUtf8(const std::string& text)
{
    return juce::String::fromUTF8(text.c_str(), static_cast<int>(text.size()));
}

JsonValue textArray(const juce::StringArray& values, std::size_t maxCodePoints)
{
    auto json = JsonValue::array();
    for (const auto& value : values)
        json.append(controlJsonText(value.toStdString(), maxCodePoints));
    return json;
}

JsonValue availableJson(const AudioDeviceSettings::Available& available)
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

/** The device member of audio.get and audio.set. nullReason says why the members
    are null when no device runs: no_audio_device, no_editor or unavailable. */
JsonValue deviceJson(const AudioDeviceSettings::DeviceState& view, const std::string& nullReason)
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
    member("deviceType", controlJsonText(view.deviceType.toStdString(), kMaxTypeCodePoints), view.running);
    member("outputDevice", controlJsonText(view.outputDevice.toStdString(), kMaxDeviceCodePoints), view.running);
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
        result.appendMember("device", deviceJson(AudioDeviceSettings::DeviceState{},
                                                  bridge == nullptr ? "no_editor" : "unavailable"));
        result.appendMember("available", availableJson(AudioDeviceSettings::Available{}));
        return result;
    }
    const auto snapshot = AudioDeviceSettings::scanAndRead(*manager);
    result.appendMember("device", deviceJson(snapshot.device, "no_audio_device"));
    result.appendMember("available", availableJson(snapshot.available));
    return result;
}

// ---- audio.set: params -----------------------------------------------------------------------------------

/** Parses audio.set params, retaining only the members that are present. */
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
void recordChange(ControlOpContext& context, ControlWriteEnvelope& envelope,
                  const AudioDeviceSettings::DeviceState& before, const AudioDeviceSettings::DeviceState& after)
{
    if (!before.running && !after.running)
        return;
    auto fields = JsonValue::object();
    auto revert = JsonValue::object();
    // Without a device before the call there is nothing a request could set the previous values from.
    bool revertable = before.running;
    const auto textMember = [&](const char* name, const juce::String& was, const juce::String& now, std::size_t max) {
        if (before.running && after.running && was == now)
            return;
        addMemberChange(fields, name, before.running ? controlJsonText(was.toStdString(), max) : JsonValue(),
                        after.running ? controlJsonText(now.toStdString(), max) : JsonValue(), JsonValue());
        if (before.running && was.isNotEmpty())
            revert.appendMember(name, controlJsonText(was.toStdString(), max));
        else
            revertable = false;
    };
    textMember("deviceType", before.deviceType, after.deviceType, kMaxTypeCodePoints);
    textMember("outputDevice", before.outputDevice, after.outputDevice, kMaxDeviceCodePoints);
    if (!before.running || !after.running
        || !AudioDeviceSettings::sameSampleRate(before.sampleRateHz, after.sampleRateHz)) {
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

const char* unavailableMessage(AudioDeviceSettings::RejectedSetting setting)
{
    switch (setting) {
        case AudioDeviceSettings::RejectedSetting::deviceType:
            return "the audio device type is not offered by this instance";
        case AudioDeviceSettings::RejectedSetting::outputDevice:
            return "specify an output device offered by the requested audio device type";
        case AudioDeviceSettings::RejectedSetting::noAudioDevice:
            return "no audio device is open, so no sample rate, buffer size or output channel pair is on offer";
        case AudioDeviceSettings::RejectedSetting::sampleRate:
            return "the sample rate is not offered by the audio device";
        case AudioDeviceSettings::RejectedSetting::bufferSize:
            return "the buffer size is not offered by the audio device";
        case AudioDeviceSettings::RejectedSetting::outputChannelPair:
            return "the output channel pair is not offered by the audio device";
        case AudioDeviceSettings::RejectedSetting::none:
            return "the audio setting is not offered by the device";
    }
    return "the audio setting is not offered by the device";
}

std::string applyFailureMessage(const AudioDeviceSettings::ApplyResult& result)
{
    std::string detail = result.cause == AudioDeviceSettings::ApplyKind::unavailable
        ? unavailableMessage(result.rejectedSetting)
        : "the audio device could not be opened with the new setup";
    switch (result.recovery) {
        case AudioDeviceSettings::Recovery::notApplicable:
            break;
        case AudioDeviceSettings::Recovery::unchanged:
            detail += "; the current setup is unchanged";
            break;
        case AudioDeviceSettings::Recovery::restored:
            detail += "; the previous setup was restored";
            break;
        case AudioDeviceSettings::Recovery::failed:
            detail += ", and the previous setup could not be restored";
            detail += result.after.open ? "; an audio device remains open" : "; no audio device is open";
            break;
    }
    if (result.asioRateRestoreFailed)
        detail += "; the ASIO sample rate could not be restored to the system";
    return detail;
}

void setAudioFailureData(ControlOpContext& context, juce::AudioDeviceManager& manager,
                         const AudioSetRequest& request, const AudioDeviceSettings::ApplyResult& outcome)
{
    const auto browsedType = request.deviceType ? fromUtf8(*request.deviceType) : outcome.before.deviceType;
    const auto snapshot = AudioDeviceSettings::scanAndRead(manager, browsedType);

    auto audioDevice = JsonValue::object();
    audioDevice.appendMember("device", deviceJson(outcome.after, "no_audio_device"));
    audioDevice.appendMember("deviceError", JsonValue::string(outcome.deviceError.toStdString()));
    audioDevice.appendMember("recoveryError", JsonValue::string(outcome.recoveryError.toStdString()));
    auto available = JsonValue::object();
    available.appendMember("outputDevices", textArray(snapshot.available.outputDevices, kMaxDeviceCodePoints));
    audioDevice.appendMember("available", std::move(available));

    auto extraData = JsonValue::object();
    extraData.appendMember("audioDevice", std::move(audioDevice));
    context.setErrorData(std::move(extraData));
}

Result<JsonValue> runAudioSet(ControlOpContext& context, const AudioSetRequest& request)
{
    if (auto busy = controlCheckBusy(context))
        return *busy;
    auto* bridge = context.bridge();
    // Without an editor window the method fails with no_editor_window.
    if (bridge == nullptr)
        return controlOpError("UNSUPPORTED", "no_editor_window",
                              "audio.set acts through the editor window, and no editor exists");
    auto* manager = bridge->audioDevice();
    if (manager == nullptr)
        return controlOpError("UNSUPPORTED", "capability_missing",
                              "audio.device needs the editor's audio device manager, and none exists");

    ControlWriteEnvelope envelope(context);
    AudioDeviceSettings::Request productRequest;
    if (request.deviceType)
        productRequest.deviceType = fromUtf8(*request.deviceType);
    if (request.outputDevice)
        productRequest.outputDevice = fromUtf8(*request.outputDevice);
    productRequest.sampleRateHz = request.sampleRateHz;
    if (request.bufferSizeFrame)
        productRequest.bufferSizeFrame = static_cast<int>(*request.bufferSizeFrame);
    if (request.outputChannelPair)
        productRequest.outputChannelPair = static_cast<int>(*request.outputChannelPair);

    std::optional<ControlUiBridge::AsioRateRestoreFailure> asioRateNotRestored;
    const auto outcome = AudioDeviceSettings::apply(
        *manager, productRequest, [&](const juce::String& reason) {
            auto failure = bridge->prepareAudioDeviceSwitch(*manager, reason.toStdString());
            const bool restored = !failure;
            if (failure && !asioRateNotRestored)
                asioRateNotRestored = std::move(failure);
            return restored;
        });
    switch (outcome.kind) {
        case AudioDeviceSettings::ApplyKind::applied:
            break;
        case AudioDeviceSettings::ApplyKind::unavailable:
            setAudioFailureData(context, *manager, request, outcome);
            return controlOpError("INVALID_ARGUMENT", "audio_setting_unavailable",
                                  applyFailureMessage(outcome));
        case AudioDeviceSettings::ApplyKind::openFailed:
            setAudioFailureData(context, *manager, request, outcome);
            return controlOpError("IO_ERROR", "device_open_failed", applyFailureMessage(outcome));
        case AudioDeviceSettings::ApplyKind::priorSetupNotRestored:
            setAudioFailureData(context, *manager, request, outcome);
            return controlOpErrorAfterChange("IO_ERROR", "device_open_failed", applyFailureMessage(outcome));
    }
    recordChange(context, envelope, outcome.before, outcome.after);
    // The sample rate of the ASIO device that was left stays where the device had put it; the switch itself went on.
    // The warning reports the device name and sample rate for audioDevice.
    if (outcome.asioRateRestoreFailed && asioRateNotRestored) {
        auto subject = JsonValue::object();
        subject.appendMember("singleton", JsonValue::string("audioDevice"));
        auto data = JsonValue::object();
        data.appendMember("deviceName", controlJsonText(asioRateNotRestored->deviceName, kMaxDeviceCodePoints));
        data.appendMember("sampleRateHz", controlJsonNumber(asioRateNotRestored->sampleRateHz, 3));
        envelope.addWarning("ASIO_RATE_NOT_RESTORED", std::move(subject), std::move(data));
    }
    auto result = envelope.committed(context);
    result.appendMember("device", deviceJson(outcome.after, "no_audio_device"));
    return result;
}

} // namespace

void registerAudioDeviceOps(ControlMethodTable& table)
{
    table.add(controlReadMethod<ControlNoParams>("audio.get", controlParseNoParams, runAudioGet));
    table.add(controlWriteMethod<AudioSetRequest>("audio.set", parseAudioSet, runAudioSet));
}

} // namespace OpenTune
