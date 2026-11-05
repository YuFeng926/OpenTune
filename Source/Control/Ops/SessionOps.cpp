// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SessionOps.h"

#include "OpsJson.h"
#include "OpsSupport.h"
#include "PrefsModels.h"

#include "Audio/AudioFormatRegistry.h"
#include "PluginProcessor.h"
#include "Utils/AppPreferences.h"

#include <algorithm>

namespace OpenTune {

namespace {

/** Channels an import accepts: prepareImport rejects more than 2 (multichannel-not-supported). */
constexpr int kImportMaxChannels = 2;
/** Longest audio the pitch analysis accepts, in seconds: the F0 extractor's limit (10 minutes, the
    private FCPEExtractor::kMaxAudioDurationSec). */
constexpr double kImportMaxAnalysisSec = 600.0;
/** The export contract (PROTOCOL.md section 19): WAV, 44100 Hz, float32, mono clips and tracks, a stereo
    master. */
constexpr double kExportSampleRateHz = 44100.0;

const char* osName()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char* archName()
{
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

JsonValue optionalString(const std::optional<std::string>& value)
{
    return value ? JsonValue::string(*value) : JsonValue();
}

/** The file extensions of the formats the import registers (AudioFormatRegistry, as the UI's file
    chooser lists them), as ".ext" in lower case. */
JsonValue importExtensions()
{
    auto list = JsonValue::array();
    std::vector<std::string> seen;
    for (const auto& pattern :
         juce::StringArray::fromTokens(AudioFormatRegistry::getImportWildcardFilter(), ";,", "")) {
        auto extension = pattern.trim().fromLastOccurrenceOf(".", true, false).toLowerCase().toStdString();
        const bool valid = extension.size() >= 2 && extension.size() <= 9 && extension[0] == '.'
                           && std::all_of(extension.begin() + 1, extension.end(),
                                          [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
        if (!valid || std::find(seen.begin(), seen.end(), extension) != seen.end())
            continue;
        seen.push_back(extension);
        list.append(JsonValue::string(extension));
    }
    if (list.size() == 0)
        list.append(JsonValue::string(".wav")); // the WAV reader is always registered
    return list;
}

JsonValue audioDeviceJson(ControlOpContext& context)
{
    const auto device = controlAudioDeviceState(context);
    auto json = JsonValue::object();
    auto nullReasons = JsonValue::object();
    json.appendMember("running", JsonValue::boolean(device.running));
    json.appendMember("name", device.name ? controlJsonText(*device.name, 256) : JsonValue());
    json.appendMember("sampleRateHz", device.sampleRateHz ? controlJsonNumber(*device.sampleRateHz, 3) : JsonValue());
    json.appendMember("bufferSizeFrame",
                      device.bufferSizeFrame ? JsonValue::integer(*device.bufferSizeFrame) : JsonValue());
    if (!device.name)
        nullReasons.appendMember("name", JsonValue::string(device.nullReason));
    if (!device.sampleRateHz)
        nullReasons.appendMember("sampleRateHz", JsonValue::string(device.nullReason));
    if (!device.bufferSizeFrame)
        nullReasons.appendMember("bufferSizeFrame", JsonValue::string(device.nullReason));
    if (nullReasons.size() != 0)
        json.appendMember("nullReasons", std::move(nullReasons));
    return json;
}

/** The state of the inference runtime that session.info reports with the processing preferences. */
ControlProcessingRuntime processingRuntime()
{
    ControlProcessingRuntime runtime;
    // AccelerationDetector::getSelection(): the inference backend in use (CPU until one was detected). Read through
    // controlInferenceBackendName(), which asks the detector unless a test replaced it, so that the tests of prefs.set
    // see in session.info the backend their fake runtime ends up with.
    runtime.backend = controlInferenceBackendName();
    // ProcessRenderRuntime::isVocoderReady() / isVocoderReconfiguring(): the process-wide vocoder, polled without a
    // hook. Read through controlVocoderState(), which asks the runtime unless a test replaced it, so that the tests of
    // prefs.set see what session.info reports while their fake runtime reloads the vocoder.
    const auto vocoder = controlVocoderState();
    runtime.vocoderReady = vocoder.ready;
    runtime.vocoderReconfiguring = vocoder.reconfiguring;
    return runtime;
}

JsonValue exportJson()
{
    auto json = JsonValue::object();
    auto formats = JsonValue::array();
    formats.append(JsonValue::string("wav"));
    json.appendMember("formats", std::move(formats));
    json.appendMember("sampleRateHz", controlJsonNumber(kExportSampleRateHz, 0));
    auto channels = JsonValue::object();
    channels.appendMember("clip", JsonValue::integer(1));
    channels.appendMember("track", JsonValue::integer(1));
    channels.appendMember("master", JsonValue::integer(2));
    json.appendMember("channels", std::move(channels));
    json.appendMember("sampleFormat", JsonValue::string("float32"));
    return json;
}

Result<JsonValue> runSessionInfo(ControlOpContext& context, const ControlNoParams&)
{
    const auto& facts = context.facts();
    // AppPreferences::getState(): a copy taken under the preferences' own mutex. The runtime (and with
    // it the preferences) is initialised before the listener starts.
    const auto preferences = context.processor().getAppPreferences().getState().shared;

    auto result = JsonValue::object();
    result.appendMember("protocol", JsonValue::string(kControlProtocolVersion));
    result.appendMember("instanceId", JsonValue::string(facts.instanceId));
    result.appendMember("appVersion", JsonValue::string(facts.build.appVersion));
    auto build = JsonValue::object();
    build.appendMember("commit", JsonValue::string(facts.build.commit));
    build.appendMember("tag", optionalString(facts.build.commitTag));
    result.appendMember("build", std::move(build));
    result.appendMember("buildTag", optionalString(facts.build.buildTag));
    result.appendMember("sourceUrl", JsonValue::string(facts.build.sourceUrl));
    result.appendMember("flavor", JsonValue::string("standalone"));

    auto runtime = JsonValue::object();
    runtime.appendMember("os", JsonValue::string(osName()));
    runtime.appendMember("arch", JsonValue::string(archName()));
    runtime.appendMember("pid", JsonValue::integer(controlCurrentProcessId()));
    result.appendMember("runtime", std::move(runtime));

    result.appendMember("audioDevice", audioDeviceJson(context));
    const auto inference = processingRuntime();
    result.appendMember("processing", controlProcessingJson(preferences, &inference));
    result.appendMember("language", JsonValue::string(controlLanguageTag(preferences.language)));

    auto capabilities = JsonValue::array();
    for (const auto& capability : facts.capabilities)
        capabilities.append(JsonValue::string(capability));
    result.appendMember("capabilities", std::move(capabilities));

    auto import = JsonValue::object();
    import.appendMember("extensions", importExtensions());
    import.appendMember("maxChannels", JsonValue::integer(kImportMaxChannels));
    import.appendMember("maxAnalysisSec", controlJsonNumber(kImportMaxAnalysisSec, 0));
    result.appendMember("import", std::move(import));
    result.appendMember("export", exportJson());
    result.appendMember("limits", controlLimitsToJson(facts.limits));
    return result;
}

} // namespace

ControlProcessingRuntime controlProcessingRuntime()
{
    return processingRuntime();
}

void registerSessionOps(ControlMethodTable& table)
{
    table.add(controlReadMethod<ControlNoParams>("session.info", controlParseNoParams, runSessionInfo));
}

} // namespace OpenTune
