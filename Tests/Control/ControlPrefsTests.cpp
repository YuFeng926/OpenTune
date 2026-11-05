// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scenarios of prefs.set, audio.get and audio.set against the real processor
// without a window. The preferences are the processor's own (AppPreferences in
// the test's data directory, which the harness redirects with
// OPENTUNE_DATA_DIR); the audio device is a juce::AudioDeviceManager with fake
// device types and devices, so that no scenario opens, lists or changes a real
// audio device. The test bridge stands in for the editor: it applies the
// editing scheme (with an optional delay or never), generates the notes of the
// clip open in the piano roll as the piano roll does, and records what the
// control API asks of the editor. The vocoder weight, the rendering priority and
// the pitch model reach the render runtime through ControlPrefsBackend: the
// scenarios replace it with a test double that reports the vocoder as queued,
// reloading and ready again after chosen delays, and lists the weight files of a
// models directory of the test. Registered with the other processor scenarios
// in ControlProcessorTests.cpp.

#include "ControlContentFixtures.h"
#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Control/ControlDispatcher.h"
#include "Control/ControlJobs.h"
#include "Control/Ops/PrefsModels.h"
#include "Control/Ops/PrefsOps.h"
#include "Inference/IF0Extractor.h"
#include "PluginProcessor.h"
#include "Utils/AppPreferences.h"
#include "Utils/AudioEditingScheme.h"
#include "Utils/NoteGeneratorTypes.h"
#include "Utils/TuningConfig.h"
#include "Utils/VocoderModelWeight.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace OpenTune {
namespace ControlTest {

namespace {

/** Bound of a client exchange that includes a job. */
constexpr int kJobExchangeMs = 60000;
/** The long poll of job.get, shorter than kJobExchangeMs. */
constexpr int kJobWaitMs = 30000;

JsonValue json(const std::string& text)
{
    auto parsed = parseJson(text);
    CONTROL_CHECK(parsed.ok);
    return parsed.value;
}

// ---- The fake audio device
// -----------------------------------------------------------------------------

/** What the fake devices do when opened; shared by every device and type of one
 * rig. Message thread only. */
struct FakeAudioBehaviour
{
    /** Opening a device with this sample rate fails (0: never). */
    double failRateHz = 0.0;
    /** Every open fails. */
    bool failAllOpens = false;
    /** Opening the device of this name fails. */
    juce::String failDeviceName;
    int opens = 0;
    int scans = 0;
};

class FakeAudioDevice final : public juce::AudioIODevice
{
public:
    FakeAudioDevice(const juce::String& name, const juce::String& typeName, FakeAudioBehaviour& behaviour)
        : juce::AudioIODevice(name, typeName)
        , behaviour_(behaviour)
    {}

    ~FakeAudioDevice() override = default;

    juce::StringArray getOutputChannelNames() override { return {"Left", "Right"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {44100.0, 48000.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {128, 256, 512}; }
    int getDefaultBufferSize() override { return 512; }

    juce::String open(const juce::BigInteger& /*inputChannels*/, const juce::BigInteger& outputChannels,
                      double sampleRate, int bufferSizeSamples) override
    {
        ++behaviour_.opens;
        opened_ = false;
        playing_ = false;
        if (behaviour_.failAllOpens || (behaviour_.failRateHz != 0.0 && sampleRate == behaviour_.failRateHz)
            || getName() == behaviour_.failDeviceName)
            return "fake device: cannot be opened";
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        bufferSize_ = bufferSizeSamples > 0 ? bufferSizeSamples : getDefaultBufferSize();
        outputChannels_ = outputChannels;
        opened_ = true;
        return {};
    }

    void close() override
    {
        opened_ = false;
        playing_ = false;
    }
    bool isOpen() override { return opened_; }
    void start(juce::AudioIODeviceCallback*) override { playing_ = opened_; }
    void stop() override { playing_ = false; }
    bool isPlaying() override { return playing_; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSize_; }
    double getCurrentSampleRate() override { return sampleRate_; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return outputChannels_; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

private:
    FakeAudioBehaviour& behaviour_;
    bool opened_ = false;
    bool playing_ = false;
    double sampleRate_ = 44100.0;
    int bufferSize_ = 512;
    juce::BigInteger outputChannels_;
};

class FakeAudioType final : public juce::AudioIODeviceType
{
public:
    FakeAudioType(const juce::String& typeName, juce::StringArray devices, FakeAudioBehaviour& behaviour)
        : juce::AudioIODeviceType(typeName)
        , devices_(std::move(devices))
        , behaviour_(behaviour)
    {}

    void scanForDevices() override { ++behaviour_.scans; }
    juce::StringArray getDeviceNames(bool /*wantInputNames*/) const override { return devices_; }
    int getDefaultDeviceIndex(bool /*forInput*/) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice* device, bool /*asInput*/) const override
    {
        return device == nullptr ? -1 : devices_.indexOf(device->getName());
    }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName,
                                      const juce::String& inputDeviceName) override
    {
        const auto name = outputDeviceName.isNotEmpty() ? outputDeviceName : inputDeviceName;
        if (!devices_.contains(name))
            return nullptr;
        return new FakeAudioDevice(name, getTypeName(), behaviour_);
    }

private:
    juce::StringArray devices_;
    FakeAudioBehaviour& behaviour_;
};

/**
 * A device manager that only knows two fake device types: the primary one with
 * the devices "Fake Output A" and "Fake Output B", and "Fake B" with "Fake B
 * Output". With running true it starts with Fake Output A at 44100 Hz and a
 * buffer of 512 frames. It never lists or opens a real device: the types are
 * added before anything could add the system's own.
 */
struct FakeAudioRig
{
    explicit FakeAudioRig(bool running = true, const char* primaryType = "Fake")
    {
        manager.addAudioDeviceType(std::make_unique<FakeAudioType>(
            primaryType, juce::StringArray{"Fake Output A", "Fake Output B"}, behaviour));
        manager.addAudioDeviceType(
            std::make_unique<FakeAudioType>("Fake B", juce::StringArray{"Fake B Output"}, behaviour));
        manager.setCurrentAudioDeviceType(primaryType, true);
        if (running) {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            setup.outputDeviceName = "Fake Output A";
            setup.inputDeviceName = "Fake Output A";
            setup.sampleRate = 44100.0;
            setup.bufferSize = 512;
            setup.useDefaultOutputChannels = true;
            CONTROL_CHECK(manager.setAudioDeviceSetup(setup, true).isEmpty());
        } else {
            manager.closeAudioDevice();
        }
        behaviour.opens = 0;
        behaviour.scans = 0;
    }
    ~FakeAudioRig() { manager.closeAudioDevice(); }

    /** The device type and output device in use, and another of each (a different device type that has an output
        device; another output device of the current type): the names that the vectors' given.audio.alternatives binds.
        valid is false when no device runs or an alternative is missing. */
    struct Alternatives
    {
        bool valid = false;
        std::string deviceType;
        std::string outputDevice;
        std::string otherDeviceType;
        std::string otherOutputDevice;
    };
    Alternatives alternatives()
    {
        Alternatives names;
        auto* device = manager.getCurrentAudioDevice();
        auto* currentType = manager.getCurrentDeviceTypeObject();
        if (device == nullptr || currentType == nullptr)
            return names;
        names.deviceType = currentType->getTypeName().toStdString();
        names.outputDevice = device->getName().toStdString();
        for (const auto& name : currentType->getDeviceNames(false))
            if (name != device->getName()) {
                names.otherOutputDevice = name.toStdString();
                break;
            }
        for (auto* type : manager.getAvailableDeviceTypes())
            if (type != currentType && type->getDeviceNames(false).size() != 0) {
                names.otherDeviceType = type->getTypeName().toStdString();
                break;
            }
        names.valid = !names.otherOutputDevice.empty() && !names.otherDeviceType.empty();
        return names;
    }

    FakeAudioBehaviour behaviour;
    juce::AudioDeviceManager manager;
};

// ---- The test bridge
// -----------------------------------------------------------------------------------

/**
 * The editor as far as these methods see it. onPreferencesChanged() hands the
 * preferences to the "views": the editing scheme is applied at once, after a
 * delay or never (an editor that never applies reports no scheme as applied,
 * as views stopped in the middle of a switch would, so that two jobs can wait
 * at once); with generateNotes, applying the notes-primary scheme
 * generates the notes of the clip open in the piano roll
 * (view.pianoRollPlacementId, pianoRollKey) when none were ever generated, as
 * PianoRollComponent::ensureOpenDyneNotesIfNeeded does.
 */
class PrefsBridge : public TestUiBridge
{
public:
    enum class Apply {
        Immediate,
        AfterDelay,
        Never
    };

    juce::AudioDeviceManager* audioDevice() override { return manager; }

    void onPreferencesChanged() override
    {
        ++preferenceNotifications;
        pendingNotesPrimary =
            AudioEditingScheme::usesNotesPrimaryScheme(preferences->getState().shared.audioEditingScheme);
        pendingSinceMs = juce::Time::getMillisecondCounter();
        hasPending = true;
        if (apply == Apply::Immediate)
            applyPending();
    }

    std::optional<bool> editingSchemeApplied(bool notesPrimary) const override
    {
        ++schemeQueries;
        if (apply == Apply::Never)
            return false;
        if (apply == Apply::AfterDelay && hasPending
            && juce::Time::getMillisecondCounter() - pendingSinceMs >= static_cast<std::uint32_t>(applyDelayMs))
            applyPending();
        return appliedNotesPrimary == notesPrimary;
    }

    /** With modalOnceJobStarted, a modal dialog is reported as open while a job of that registry is active: a dialog
        that opens after the call started its job and before the job's first step. It does not depend on how often
        the call or the job asks: the call's own check runs before its job exists. */
    std::optional<BusyState> queryBusy() const override
    {
        auto state = TestUiBridge::queryBusy();
        if (state && modalOnceJobStarted != nullptr && modalOnceJobStarted->activeCount() > 0)
            state->modalOpen = true;
        return state;
    }

    /** The editor's restore of the ASIO sample rate: it succeeds, or fails as asioRestoreSucceeds says. A failure names
        the device the manager runs and asioSystemRateHz, the rate the restore tried to set (not the rate of the
        device), as the real bridge does. */
    std::optional<AsioRateRestoreFailure> prepareAudioDeviceSwitch(juce::AudioDeviceManager& deviceManager,
                                                                   const std::string& reason) override
    {
        switchReasons.push_back(reason);
        if (asioRestoreSucceeds)
            return std::nullopt;
        AsioRateRestoreFailure failure;
        failure.deviceName = deviceManager.getAudioDeviceSetup().outputDeviceName.toStdString();
        failure.sampleRateHz = asioSystemRateHz;
        return failure;
    }

    juce::AudioDeviceManager* manager = nullptr;
    OpenTuneAudioProcessor* processor = nullptr;
    AppPreferences* preferences = nullptr;
    Apply apply = Apply::Immediate;
    int applyDelayMs = 0;
    bool generateNotes = false;
    ContentKey pianoRollKey;
    bool asioRestoreSucceeds = true;
    /** The rate that a restore of the ASIO sample rate aims at in these tests (the rate of the fake devices is 44100).
     */
    double asioSystemRateHz = 48000.0;
    std::shared_ptr<ControlJobs> modalOnceJobStarted;

    int preferenceNotifications = 0;
    std::vector<std::string> switchReasons;
    mutable int schemeQueries = 0;
    mutable bool appliedNotesPrimary = false;

private:
    void applyPending() const
    {
        const bool was = appliedNotesPrimary;
        appliedNotesPrimary = pendingNotesPrimary;
        hasPending = false;
        if (!generateNotes || !appliedNotesPrimary || was || processor == nullptr || !pianoRollKey.isValid())
            return;
        const auto snapshot = processor->getContentSnapshot(pianoRollKey);
        if (snapshot == nullptr || snapshot->noteTopologyInitialized || snapshot->pitchCurve == nullptr
            || snapshot->pitchCurve->getOriginalF0().empty())
            return;
        CONTROL_CHECK(processor->generateNotesOnlyByContentKey(pianoRollKey, NoteGeneratorParams{}));
    }

    mutable bool hasPending = false;
    mutable bool pendingNotesPrimary = false;
    mutable std::uint32_t pendingSinceMs = 0;
};

/** Registers a bridge with the service for the life of this object. */
class ScopedBridge
{
public:
    ScopedBridge(ControlService& service, ControlUiBridge& bridge) : service_(service)
    {
        service_.setUiBridge(&bridge);
    }
    ~ScopedBridge() { service_.setUiBridge(nullptr); }

    ScopedBridge(const ScopedBridge&) = delete;
    ScopedBridge& operator=(const ScopedBridge&) = delete;

private:
    ControlService& service_;
};

/** The preferences these methods change, put back to the defaults of a fresh
   installation: the data directory of a process outlives the processors of its
   vectors. */
void resetPreferences(OpenTuneAudioProcessor& processor, bool notesPrimary = false, bool hybrid = true,
                      bool experimental = false, float tuningHz = TuningConfig::kDefaultTuningHz)
{
    auto& preferences = processor.getAppPreferences();
    TuningConfig::TuningSettings tuning;
    tuning.tuningHz = tuningHz;
    preferences.setTuning(tuning);
    preferences.setAudioEditingScheme(notesPrimary ? AudioEditingScheme::Scheme::NotesPrimary
                                                   : AudioEditingScheme::Scheme::CorrectedF0Primary);
    preferences.setLightPitchCorrectionEnabled(hybrid);
    preferences.setExperimentalFeaturesEnabled(experimental);
    preferences.setVocoderModelWeight(kDefaultVocoderWeight);
    preferences.setRenderingPriority(RenderingPriority::GpuFirst);
    preferences.setF0ModelType(F0ModelType::FCPE);
}

/** A clip with a ready analysis and no notes, on track 0 from 2 s to 8 s. */
ClipSpec readyClip(const char* name)
{
    ClipSpec spec;
    spec.name = std::string("prefs-") + name;
    spec.trackId = 0;
    spec.startTimelineSec = 2.0;
    spec.durationSec = 6.0;
    return spec;
}

// ---- Requests and results
// -------------------------------------------------------------------------------

JsonValue waitJob(ApiSession& api, const std::string& jobId, int waitMs = kJobWaitMs)
{
    auto params = JsonValue::object();
    params.appendMember("jobId", JsonValue::string(jobId));
    params.appendMember("waitMs", JsonValue::integer(waitMs));
    return memberOf(api.result("job.get", std::move(params)), "job");
}

std::string jobIdOf(const JsonValue& result)
{
    return memberOf(memberOf(result, "job"), "jobId").text();
}

/** Sends a method, waits for its job and returns the job; the method must have
 * handed its work to a job. */
JsonValue runAsJob(ApiSession& api, const std::string& method, JsonValue params, const char* kind,
                   JsonValue* started = nullptr)
{
    const auto response = api.result(method, std::move(params));
    if (started != nullptr)
        *started = response;
    const auto jobId = jobIdOf(response);
    if (!CONTROL_CHECK(!jobId.empty() && memberOf(memberOf(response, "job"), "kind").text() == kind))
        return JsonValue();
    return waitJob(api, jobId);
}

bool hasWarning(const JsonValue& result, const std::string& code)
{
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        if (memberOf(warnings.at(i), "code").text() == code)
            return true;
    return false;
}

/** The first warning with this code, or null. */
const JsonValue* findWarning(const JsonValue& result, const std::string& code)
{
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        if (memberOf(warnings.at(i), "code").text() == code)
            return &warnings.at(i);
    return nullptr;
}

std::vector<std::string> warningCodes(const JsonValue& result)
{
    std::vector<std::string> codes;
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        codes.push_back(memberOf(warnings.at(i), "code").text());
    return codes;
}

/** The names that GLOBAL_PREFERENCE lists, in order. */
std::vector<std::string> globalPreferences(const JsonValue& result)
{
    std::vector<std::string> names;
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i) {
        if (memberOf(warnings.at(i), "code").text() != "GLOBAL_PREFERENCE")
            continue;
        const auto& list = memberOf(memberOf(warnings.at(i), "data"), "preferences");
        for (std::size_t n = 0; n < list.size(); ++n)
            names.push_back(list.at(n).text());
    }
    return names;
}

/** The change of the entity at index, and the field of it. */
const JsonValue& changeAt(const JsonValue& result, std::size_t index)
{
    return memberOf(result, "changes").at(index);
}

const JsonValue& fieldOf(const JsonValue& change, const char* name)
{
    return memberOf(memberOf(change, "fields"), name);
}

bool numberIs(const JsonValue& value, double expected)
{
    return value.isNumber() && value.asDouble() == expected;
}

bool undoIsUiParity(const JsonValue& result)
{
    const auto& undo = memberOf(result, "undo");
    return memberOf(undo, "undoable").isBoolean() && !memberOf(undo, "undoable").asBoolean()
           && memberOf(undo, "reason").text() == "ui_parity";
}

/** Sends the revert recipe of a result unchanged and returns the result. */
JsonValue replayRevert(ApiSession& api, const JsonValue& result)
{
    const auto& revert = memberOf(memberOf(result, "undo"), "revert");
    if (!CONTROL_CHECK(revert.isObject()))
        return JsonValue();
    return api.result(memberOf(revert, "method").text(), memberOf(revert, "params"));
}

bool processingEqual(const JsonValue& a, const JsonValue& b)
{
    return VectorMatcher::equal(memberOf(a, "processing"), memberOf(b, "processing"));
}

// ---- prefs.set
// -------------------------------------------------------------------------------------------

/** Every setting on its own: changes, warnings, the revert recipe sent back
   unchanged, the no-op, one call that changes several, the closed params, and
   BUSY. */
void checkSynchronousChanges(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge,
                             const ClipHandles& clip)
{
    const auto baseline = api.result("prefs.get");
    CONTROL_CHECK(numberIs(memberOf(memberOf(baseline, "processing"), "tuningHz"), 440.0));

    // The tuning: the process-wide A4 frequency changes with the preference;
    // results refer to it from now on.
    {
        const auto result = api.result("prefs.set", json(R"({"tuningHz":432})"));
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean());
        CONTROL_CHECK(memberOf(result, "changes").size() == 1);
        const auto& change = changeAt(result, 0);
        CONTROL_CHECK(memberOf(change, "entity").text() == "preference");
        CONTROL_CHECK(memberOf(memberOf(change, "ref"), "preference").text() == "tuningHz");
        CONTROL_CHECK(numberIs(memberOf(fieldOf(change, "tuningHz"), "before"), 440.0)
                      && numberIs(memberOf(fieldOf(change, "tuningHz"), "after"), 432.0)
                      && memberOf(fieldOf(change, "tuningHz"), "unit").text() == "hz");
        CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE", "NOT_RETROACTIVE"}));
        CONTROL_CHECK((globalPreferences(result) == std::vector<std::string>{"tuningHz"}));
        CONTROL_CHECK(undoIsUiParity(result));
        CONTROL_CHECK(TuningConfig::currentTuningHz() == 432.0f);
        CONTROL_CHECK(numberIs(memberOf(memberOf(api.result("session.info"), "processing"), "tuningHz"), 432.0));
        CONTROL_CHECK(numberIs(memberOf(memberOf(api.result("prefs.get"), "processing"), "tuningHz"), 432.0));
        CONTROL_CHECK(bridge.preferenceNotifications == 1);
        // The revert recipe, sent as it is, takes the tuning back; the same value
        // again is a no-op.
        const auto reverted = replayRevert(api, result);
        CONTROL_CHECK(memberOf(reverted, "applied").asBoolean() && !memberOf(reverted, "noOp").asBoolean());
        CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
        const auto again = api.result("prefs.set", json(R"({"tuningHz":440})"));
        CONTROL_CHECK(memberOf(again, "noOp").asBoolean() && memberOf(again, "changes").size() == 0
                      && memberOf(again, "warnings").size() == 0 && memberOf(again, "sideEffects").size() == 0);
        CONTROL_CHECK(memberOf(memberOf(again, "undo"), "reason").text() == "no_op");
        CONTROL_CHECK(bridge.preferenceNotifications == 2);
    }

    // The hybrid switch: every content renders again; the previous value comes
    // back with the revert recipe.
    {
        const auto result = api.result("prefs.set", json(R"({"hybridMode":false})"));
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && memberOf(result, "changes").size() == 1);
        CONTROL_CHECK(memberOf(fieldOf(changeAt(result, 0), "hybridMode"), "before").asBoolean()
                      && !memberOf(fieldOf(changeAt(result, 0), "hybridMode"), "after").asBoolean());
        CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE"}));
        const auto& queued = memberOf(memberOf(memberOf(result, "sideEffects"), "reRenderQueued"), "contentIds");
        CONTROL_CHECK(queued.size() == 1 && queued.at(0).text() == clip.contentId);
        CONTROL_CHECK(!processor.getAppPreferences().getState().shared.lightPitchCorrectionEnabled);
        CONTROL_CHECK(!memberOf(memberOf(api.result("prefs.get"), "processing"), "hybridMode").asBoolean());
        replayRevert(api, result);
        CONTROL_CHECK(processor.getAppPreferences().getState().shared.lightPitchCorrectionEnabled);
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    }

    // The experimental features.
    {
        const auto result = api.result("prefs.set", json(R"({"experimentalFeatures":true})"));
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && memberOf(result, "changes").size() == 1);
        CONTROL_CHECK((globalPreferences(result) == std::vector<std::string>{"experimentalFeatures"}));
        CONTROL_CHECK(processor.getAppPreferences().getState().shared.experimentalFeaturesEnabled);
        CONTROL_CHECK(memberOf(memberOf(api.result("session.info"), "processing"), "experimentalFeatures").asBoolean());
        replayRevert(api, result);
        CONTROL_CHECK(!processor.getAppPreferences().getState().shared.experimentalFeaturesEnabled);
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    }

    // One call that changes several: the changes and the warning follow the order
    // of the schema, and the one revert recipe takes all of them back.
    {
        const auto result =
            api.result("prefs.set", json(R"({"experimentalFeatures":true,"hybridMode":false,"tuningHz":435.5})"));
        CONTROL_CHECK(memberOf(result, "changes").size() == 3);
        CONTROL_CHECK(
            (globalPreferences(result) == std::vector<std::string>{"tuningHz", "hybridMode", "experimentalFeatures"}));
        CONTROL_CHECK(TuningConfig::currentTuningHz() == 435.5f);
        const auto& params = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
        CONTROL_CHECK(params.size() == 3 && numberIs(memberOf(params, "tuningHz"), 440.0)
                      && memberOf(params, "hybridMode").asBoolean()
                      && !memberOf(params, "experimentalFeatures").asBoolean());
        replayRevert(api, result);
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    }

    // A call with some members equal to the current values changes only the
    // others.
    {
        const auto result = api.result("prefs.set", json(R"({"tuningHz":440,"hybridMode":false})"));
        CONTROL_CHECK(memberOf(result, "changes").size() == 1);
        CONTROL_CHECK((globalPreferences(result) == std::vector<std::string>{"hybridMode"}));
        replayRevert(api, result);
    }

    // The params schema is closed and has bounds; nothing changes.
    {
        const int notifications = bridge.preferenceNotifications;
        for (const char* params :
             {"{}", R"({"tuningHz":380})", R"({"tuningHz":500.5})", R"({"tuningHz":"440"})",
              R"({"editingScheme":"openJazz"})", R"({"hybridMode":"yes"})", R"({"experimentalFeatures":1})",
              R"({"vocoderWeight":""})", R"({"vocoderWeight":5})", R"({"renderingPriority":"fast"})",
              R"({"renderingPriority":1})", R"({"f0Model":""})", R"({"f0Model":true})",
              R"({"referenceAlignMode":"game"})", R"({"hybridMode":true,"theme":"dark"})"})
            CONTROL_CHECK(api.fails("prefs.set", json(params), "INVALID_ARGUMENT", "invalid_params"));
        CONTROL_CHECK(bridge.preferenceNotifications == notifications);
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    }

    // BUSY: a modal dialog is open; nothing changes, also not for a change of the
    // editing scheme (no job starts).
    {
        bridge.busy.modalOpen = true;
        CONTROL_CHECK(api.fails("prefs.set", json(R"({"tuningHz":433})"), "BUSY", "modal_open"));
        CONTROL_CHECK(api.fails("prefs.set", json(R"({"editingScheme":"openDyne"})"), "BUSY", "modal_open"));
        bridge.busy.modalOpen = false;
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    }
}

} // namespace

void scenarioPrefsRoundTrip()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    resetPreferences(*processor);
    PrefsBridge bridge;
    bridge.processor = processor.get();
    bridge.preferences = &processor->getAppPreferences();
    ScopedBridge registered(service, bridge);
    fixtureAddClip(*processor, readyClip("round-trip"));
    ApiSession api;
    if (!api.open(service))
        return;
    const auto handles = fixtureHandlesOf(api.result("project.get"), 0, 2.0);
    if (!CONTROL_CHECK(handles.has_value()))
        return;
    checkSynchronousChanges(api, *processor, bridge, *handles);
    resetPreferences(*processor);
    const auto dispatcher = service.dispatcher();
    CONTROL_CHECK(dispatcher != nullptr && dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

namespace {

/** The envelope of a job that has ended succeeded: the job and its result. */
bool jobSucceeded(const JsonValue& job)
{
    const bool ok = memberOf(job, "state").text() == "succeeded" && memberOf(job, "kind").text() == "prefsApply"
                    && memberOf(job, "cancellable").isBoolean() && !memberOf(job, "cancellable").asBoolean();
    if (!ok)
        note("the prefsApply job did not succeed: " + toJsonString(job));
    return ok;
}

/** A change of the editing scheme is a job: the call changes nothing, the job
   applies the scheme, and switching to openDyne generates the notes of the clip
   open in the piano roll. */
void checkSchemeJob(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge, const Clip& clip,
                    const ClipHandles& handles)
{
    auto& preferences = processor.getAppPreferences();
    bridge.generateNotes = true;
    bridge.pianoRollKey = clip.key;
    bridge.view.pianoRollPlacementId = clip.placementId;
    bridge.view.workspace = "pianoRoll";
    CONTROL_CHECK(!processor.getContentSnapshot(clip.key)->noteTopologyInitialized);

    // The editor takes its time: the call returned a job and no change, the job
    // runs until the editor reports the scheme.
    bridge.apply = PrefsBridge::Apply::AfterDelay;
    bridge.applyDelayMs = 400;
    JsonValue started;
    const auto taken = juce::Time::getMillisecondCounter();
    {
        const auto response =
            api.result("prefs.set", json(R"({"editingScheme":"openDyne","tuningHz":441,"hybridMode":false})"));
        started = response;
        CONTROL_CHECK(!memberOf(response, "applied").asBoolean() && !memberOf(response, "noOp").asBoolean());
        CONTROL_CHECK(memberOf(response, "changes").size() == 0 && memberOf(response, "sideEffects").size() == 0);
        CONTROL_CHECK(memberOf(response, "undo").isNull());
        CONTROL_CHECK(memberOf(memberOf(response, "job"), "kind").text() == "prefsApply");
        const auto jobId = jobIdOf(response);
        // While the editor has not applied the scheme the job is not over, and it
        // cannot be cancelled.
        auto running = JsonValue::object();
        running.appendMember("jobId", JsonValue::string(jobId));
        const auto early = memberOf(api.result("job.get", running), "job");
        const auto state = memberOf(early, "state").text();
        CONTROL_CHECK(state == "queued" || state == "running");
        CONTROL_CHECK(!memberOf(early, "cancellable").asBoolean());
        CONTROL_CHECK(api.fails("job.cancel", running, "UNSUPPORTED", "job_not_cancellable"));
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK(juce::Time::getMillisecondCounter() - taken >= 400);
        const auto& result = memberOf(job, "result");
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean());
        CONTROL_CHECK(memberOf(result, "job").isNull());
        CONTROL_CHECK(memberOf(result, "changes").size() == 3);
        CONTROL_CHECK(
            (globalPreferences(result) == std::vector<std::string>{"tuningHz", "editingScheme", "hybridMode"}));
        CONTROL_CHECK((warningCodes(result)
                       == std::vector<std::string>{"GLOBAL_PREFERENCE", "NOT_RETROACTIVE", "SCHEME_GENERATED_NOTES",
                                                   "IRREVERSIBLE"}));
        const auto& generated = memberOf(memberOf(result, "sideEffects"), "notesGenerated");
        CONTROL_CHECK(memberOf(generated, "contentId").text() == handles.contentId);
        CONTROL_CHECK(memberOf(generated, "noteCount").asDouble() > 0.0 && memberOf(generated, "revert").isNull());
        const auto& rerender = memberOf(memberOf(result, "sideEffects"), "reRenderQueued");
        CONTROL_CHECK(memberOf(rerender, "contentIds").size() == 1);
        CONTROL_CHECK(undoIsUiParity(result));
        const auto& params = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
        CONTROL_CHECK(params.size() == 3 && memberOf(params, "editingScheme").text() == "openTune");
        CONTROL_CHECK(!memberOf(memberOf(result, "dirty"), "before").asBoolean()
                      && memberOf(memberOf(result, "dirty"), "after").asBoolean());
        // The notes changed, which the revision diff of the touched content says.
        const auto& diff = memberOf(memberOf(result, "revisionDiff"), "contents");
        CONTROL_CHECK(diff.size() == 1 && memberOf(diff.at(0), "contentId").text() == handles.contentId
                      && memberOf(memberOf(diff.at(0), "changed"), "notes").isObject());
        CONTROL_CHECK(processor.getContentSnapshot(clip.key)->noteTopologyInitialized
                      && !processor.getContentSnapshot(clip.key)->notes.empty());
        CONTROL_CHECK(AudioEditingScheme::usesNotesPrimaryScheme(preferences.getState().shared.audioEditingScheme));
        CONTROL_CHECK(TuningConfig::currentTuningHz() == 441.0f);
        CONTROL_CHECK(bridge.project.dirty);
        // One notification when the preferences were changed, and the job asked
        // while the editor was applying.
        CONTROL_CHECK(bridge.preferenceNotifications == 1 && bridge.schemeQueries >= 2);
        // The revert recipe, sent unchanged, takes all three back; it changes the
        // scheme, so it is a job as well.
        const auto reverted = runAsJob(api, "prefs.set", params, "prefsApply");
        CONTROL_CHECK(jobSucceeded(reverted));
        const auto& revertedResult = memberOf(reverted, "result");
        CONTROL_CHECK(memberOf(revertedResult, "changes").size() == 3);
        // The notes stay (they have no undo), but no new notes were generated.
        CONTROL_CHECK(!hasWarning(revertedResult, "SCHEME_GENERATED_NOTES"));
        CONTROL_CHECK(memberOf(revertedResult, "sideEffects").find("notesGenerated") == nullptr);
        CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(preferences.getState().shared.audioEditingScheme));
        CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f
                      && preferences.getState().shared.lightPitchCorrectionEnabled);
    }

    // Back to openDyne: the notes exist already, so none are generated and no
    // warning about it is made.
    bridge.apply = PrefsBridge::Apply::Immediate;
    {
        const auto job = runAsJob(api, "prefs.set", json(R"({"editingScheme":"openDyne"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        const auto& result = memberOf(job, "result");
        CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE"}));
        CONTROL_CHECK(memberOf(result, "sideEffects").size() == 0);
        // The same scheme again: a no-op that is no job.
        const auto again = api.result("prefs.set", json(R"({"editingScheme":"openDyne"})"));
        CONTROL_CHECK(memberOf(again, "noOp").asBoolean() && memberOf(again, "job").isNull());
        CONTROL_CHECK(runAsJob(api, "prefs.set", json(R"({"editingScheme":"openTune"})"), "prefsApply").isObject());
    }
}

/** An editor that never applies the scheme: after the wait the job puts every
   preference of the call back and fails with INTERNAL; the same when the
   control API stops while the job waits. */
void checkSchemeTimeout(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge)
{
    auto& preferences = processor.getAppPreferences();
    bridge.apply = PrefsBridge::Apply::Never;
    controlSetPrefsApplyTimeoutForTesting(300);
    const auto notifications = bridge.preferenceNotifications;
    const auto job =
        runAsJob(api, "prefs.set",
                 json(R"({"editingScheme":"openDyne","tuningHz":433,"hybridMode":false,"experimentalFeatures":true})"),
                 "prefsApply");
    controlSetPrefsApplyTimeoutForTesting(0);
    const auto& error = memberOf(memberOf(job, "error"), "data");
    CONTROL_CHECK(memberOf(job, "state").text() == "failed" && memberOf(error, "kind").text() == "INTERNAL");
    CONTROL_CHECK(memberOf(error, "executed").isBoolean() && !memberOf(error, "executed").asBoolean());
    // Nothing of the call remains: the preferences are those from before, and the
    // editor was told twice (change, restore).
    const auto state = preferences.getState().shared;
    CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(state.audioEditingScheme)
                  && state.lightPitchCorrectionEnabled && !state.experimentalFeaturesEnabled);
    CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
    CONTROL_CHECK(bridge.preferenceNotifications == notifications + 2);
    const auto prefs = api.result("prefs.get");
    CONTROL_CHECK(numberIs(memberOf(memberOf(prefs, "processing"), "tuningHz"), 440.0)
                  && memberOf(memberOf(prefs, "processing"), "editingScheme").text() == "openTune");
}

/** How long the jobs of the two checks below wait for the editor: long enough
   that the next call is made while the first job still waits, also on a loaded
   machine. */
constexpr std::uint32_t kWaitingJobTimeoutMs = 1500;
/** Bound for a job's first step to make its changes. */
constexpr int kJobChangeMs = 5000;

/** The job ended failed with INTERNAL and this executed. */
bool jobFailedInternal(const JsonValue& job, bool executed)
{
    const auto& data = memberOf(memberOf(job, "error"), "data");
    const bool ok = memberOf(job, "state").text() == "failed" && memberOf(data, "kind").text() == "INTERNAL"
                    && memberOf(data, "executed").isBoolean() && memberOf(data, "executed").asBoolean() == executed;
    if (!ok)
        note(std::string("expected a prefsApply job failed with INTERNAL, executed ") + (executed ? "true" : "false")
             + ": " + toJsonString(job));
    return ok;
}

/** Two jobs wait at once, the second started from the scheme the first wrote.
   They time out in the order they started: the first leaves the scheme to the
   second (which changed it from the first's value) with the first's previous
   value, so after both every preference is back to the value before the first,
   and neither left a change (executed false). */
void checkOverlappingTimeouts(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge)
{
    auto& preferences = processor.getAppPreferences();
    const auto baseline = api.result("prefs.get");
    bridge.apply = PrefsBridge::Apply::Never;
    controlSetPrefsApplyTimeoutForTesting(kWaitingJobTimeoutMs);
    const auto notifications = bridge.preferenceNotifications;
    const auto first = jobIdOf(api.result("prefs.set", json(R"({"editingScheme":"openDyne","tuningHz":433})")));
    CONTROL_CHECK(!first.empty());
    CONTROL_CHECK(pumpMessagesUntil([] { return TuningConfig::currentTuningHz() == 433.0f; }, kJobChangeMs));
    // The second reads openDyne, the value the first wrote, as its previous scheme.
    const auto second = jobIdOf(api.result("prefs.set", json(R"({"editingScheme":"openTune","hybridMode":false})")));
    CONTROL_CHECK(!second.empty());
    CONTROL_CHECK(
        pumpMessagesUntil([&] { return !preferences.getState().shared.lightPitchCorrectionEnabled; }, kJobChangeMs));
    auto params = JsonValue::object();
    params.appendMember("jobId", JsonValue::string(first));
    const auto firstState = memberOf(memberOf(api.result("job.get", params), "job"), "state").text();
    CONTROL_CHECK(firstState == "queued" || firstState == "running");
    const auto firstJob = waitJob(api, first);
    const auto secondJob = waitJob(api, second);
    controlSetPrefsApplyTimeoutForTesting(0);
    CONTROL_CHECK(jobFailedInternal(firstJob, false));
    CONTROL_CHECK(jobFailedInternal(secondJob, false));
    const auto state = preferences.getState().shared;
    CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(state.audioEditingScheme)
                  && state.lightPitchCorrectionEnabled && !state.experimentalFeaturesEnabled);
    CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    // Two changes and two restores: the first restored the tuning, the second
    // the hybrid mode; the scheme already held the value before the first.
    CONTROL_CHECK(bridge.preferenceNotifications == notifications + 4);
}

/** Another write while a job waits: the job takes back what still holds its
   value and leaves the preference the write changed, which the detail names;
   executed is true, as the job did not roll its change back completely. */
void checkWriteDuringWait(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge)
{
    auto& preferences = processor.getAppPreferences();
    bridge.apply = PrefsBridge::Apply::Never;
    controlSetPrefsApplyTimeoutForTesting(kWaitingJobTimeoutMs);
    const auto jobId = jobIdOf(api.result("prefs.set", json(R"({"editingScheme":"openDyne","tuningHz":433})")));
    CONTROL_CHECK(!jobId.empty());
    CONTROL_CHECK(pumpMessagesUntil([] { return TuningConfig::currentTuningHz() == 433.0f; }, kJobChangeMs));
    // A call that changes only the tuning succeeds at once, while the job waits.
    const auto write = api.result("prefs.set", json(R"({"tuningHz":445})"));
    CONTROL_CHECK(memberOf(write, "applied").asBoolean() && memberOf(write, "job").isNull());
    const auto job = waitJob(api, jobId);
    controlSetPrefsApplyTimeoutForTesting(0);
    CONTROL_CHECK(jobFailedInternal(job, true));
    const auto detail = memberOf(memberOf(memberOf(job, "error"), "data"), "detail").text();
    CONTROL_CHECK(detail.find("tuningHz") != std::string::npos && detail.find("editingScheme") == std::string::npos);
    // The scheme is back; the tuning keeps the value of the later write.
    CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(preferences.getState().shared.audioEditingScheme));
    CONTROL_CHECK(TuningConfig::currentTuningHz() == 445.0f);
    const auto prefs = api.result("prefs.get");
    CONTROL_CHECK(numberIs(memberOf(memberOf(prefs, "processing"), "tuningHz"), 445.0)
                  && memberOf(memberOf(prefs, "processing"), "editingScheme").text() == "openTune");
    resetPreferences(processor);
}

/** A modal dialog opens after the call started the job and before its first step: the job fails with BUSY, nothing
   executed and no preference changed. The dialog is open from the moment the job exists (the bridge asks the
   registry), so the call's own check passes and the first step's check sees it. */
void checkModalBeforeFirstStep(ApiSession& api, PrefsBridge& bridge, const std::shared_ptr<ControlJobs>& jobs)
{
    if (!CONTROL_CHECK(jobs != nullptr))
        return;
    // The jobs of the earlier checks have ended, so the only active job is the one this call starts.
    CONTROL_CHECK(pumpMessagesUntil([&jobs] { return jobs->activeCount() == 0; }, kJobWaitMs));
    const auto baseline = api.result("prefs.get");
    const auto notifications = bridge.preferenceNotifications;
    bridge.apply = PrefsBridge::Apply::Immediate;
    bridge.modalOnceJobStarted = jobs;
    const auto job = runAsJob(api, "prefs.set", json(R"({"editingScheme":"openDyne","tuningHz":433})"), "prefsApply");
    bridge.modalOnceJobStarted = nullptr;
    const auto& data = memberOf(memberOf(job, "error"), "data");
    CONTROL_CHECK(memberOf(job, "state").text() == "failed" && memberOf(data, "kind").text() == "BUSY"
                  && memberOf(data, "reason").text() == "modal_open" && memberOf(data, "executed").isBoolean()
                  && !memberOf(data, "executed").asBoolean());
    CONTROL_CHECK(bridge.preferenceNotifications == notifications);
    CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
}

} // namespace

void scenarioPrefsSchemeJob()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    resetPreferences(*processor);
    PrefsBridge bridge;
    bridge.processor = processor.get();
    bridge.preferences = &processor->getAppPreferences();
    auto registered = std::make_unique<ScopedBridge>(service, bridge);
    const auto clip = fixtureAddClip(*processor, readyClip("scheme-job"));
    std::optional<ApiSession> session;
    session.emplace();
    auto& api = *session;
    if (!api.open(service))
        return;
    const auto handles = fixtureHandlesOf(api.result("project.get"), 0, 2.0);
    if (!CONTROL_CHECK(handles.has_value()))
        return;
    checkSchemeJob(api, *processor, bridge, clip, *handles);
    checkSchemeTimeout(api, *processor, bridge);
    checkOverlappingTimeouts(api, *processor, bridge);
    checkWriteDuringWait(api, *processor, bridge);
    checkModalBeforeFirstStep(api, bridge, service.jobs());

    // Without an editor there is no one to wait for: the job applies the preference and ends.
    {
        service.setUiBridge(nullptr);
        const auto notifications = bridge.preferenceNotifications;
        const auto job = runAsJob(api, "prefs.set", json(R"({"editingScheme":"openDyne"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK(memberOf(memberOf(job, "result"), "changes").size() == 1);
        CONTROL_CHECK(memberOf(memberOf(job, "result"), "sideEffects").size() == 0);
        CONTROL_CHECK(AudioEditingScheme::usesNotesPrimaryScheme(
            processor->getAppPreferences().getState().shared.audioEditingScheme));
        CONTROL_CHECK(jobSucceeded(runAsJob(api, "prefs.set", json(R"({"editingScheme":"openTune"})"), "prefsApply")));
        CONTROL_CHECK(bridge.preferenceNotifications == notifications);
        service.setUiBridge(&bridge);
    }

    // The control API stops while a job waits for the editor: the previous
    // preferences are put back as well.
    bridge.apply = PrefsBridge::Apply::Never;
    const auto job = api.result("prefs.set", json(R"({"editingScheme":"openDyne","tuningHz":434})"));
    CONTROL_CHECK(!jobIdOf(job).empty());
    CONTROL_CHECK(pumpMessagesUntil([&] { return TuningConfig::currentTuningHz() == 434.0f; }, 5000));
    CONTROL_CHECK(AudioEditingScheme::usesNotesPrimaryScheme(
        processor->getAppPreferences().getState().shared.audioEditingScheme));
    session.reset();
    registered.reset();
    processor.reset();
    CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
    AppPreferences stored;
    stored.load();
    CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(stored.getState().shared.audioEditingScheme));
    CONTROL_CHECK(stored.getState().shared.tuning.tuningHz == 440.0f);
}

// ---- The vocoder weight, the rendering priority and the pitch model
// ---------------------------------

namespace {

/** Pitch model types OpenTune does not have, for the second and third entry of a fake model list. */
constexpr auto kOtherPitchModel = static_cast<F0ModelType>(2);
constexpr auto kGonePitchModel = static_cast<F0ModelType>(3);
constexpr const char* kWeightA = "aa-custom.onnx";
constexpr const char* kWeightZ = "zz-custom.onnx";

/**
 * The render runtime and the processor calls behind prefs.set, as far as the control API reaches them through
 * ControlPrefsBackend. A new vocoder weight or rendering priority reaches the runtime's control worker at once (the
 * command is queued); the worker then takes queuedMs to start it, during which the vocoder is still ready and not
 * reconfiguring but the worker is busy, then the vocoder is gone and reconfiguring for loadingMs, and after that it is
 * ready with an idle worker. That is the order of events of ProcessRenderRuntime, where the flag "reconfiguring" is
 * set by the worker thread some time after the processor call returned. loadingMs 0 is a weight the runtime already
 * holds (it answers without any flag). neverReady keeps the vocoder reconfiguring for good. failNextLoad makes the
 * reload end without a vocoder (not ready, not reconfiguring, idle worker). present false is a runtime without a
 * vocoder. calls lists the processor calls in order.
 *
 * The inference backend it reports (session.info processing.backend) follows the rendering priority the way the
 * runtime's detector does: a command for the priority reaches the detector when the control worker starts it (the
 * moment the vocoder goes away to reconfigure), and the backend is then cpu when the priority is cpuFirst or the GPU
 * cannot be used (gpuAvailable false), and directml otherwise. A job that does not wait for the vocoder (the vocoder
 * was not ready before the change) can therefore still read the backend of before the change.
 *
 * Installing it replaces the models directory (the real scan then runs on a directory of the test), the vocoder state,
 * the inference backend and the three processor calls; with fakePitchModels also the list of pitch models, to have
 * more than one.
 */
class FakeRuntime
{
public:
    FakeRuntime() = default;
    FakeRuntime(const FakeRuntime&) = delete;
    FakeRuntime& operator=(const FakeRuntime&) = delete;
    ~FakeRuntime()
    {
        controlSetPrefsBackendForTesting({});
        controlSetPrefsApplyTimeoutForTesting(0);
        controlSetPrefsVocoderWaitForTesting(0);
    }

    void install(const std::filesystem::path& models, bool fakePitchModels = false)
    {
        ControlPrefsBackend backend;
        backend.modelsDirectory = [dir = models.u8string()] { return dir; };
        backend.vocoderState = [this] { return state(); };
        backend.setVocoderWeight = [this](OpenTuneAudioProcessor&, const std::string& weight) {
            command();
            calls.push_back("weight:" + weight);
        };
        backend.resetInferenceBackend = [this](OpenTuneAudioProcessor&, bool forceCpu) {
            command();
            pendingBackend_ = backendFor(forceCpu);
            calls.push_back(forceCpu ? "priority:cpuFirst" : "priority:gpuFirst");
        };
        backend.inferenceBackend = [this] { return inferenceBackend(); };
        backend.setF0Model = [this](OpenTuneAudioProcessor&, F0ModelType type) {
            calls.push_back("f0:" + std::to_string(static_cast<int>(type)));
            return f0Accepts;
        };
        if (fakePitchModels)
            backend.f0Models = [] {
                return std::vector<F0ModelInfo>{{F0ModelType::FCPE, "fcpe", "FCPE", 0, true},
                                                {kOtherPitchModel, "other", "Other", 0, true},
                                                {kGonePitchModel, "gone", "Gone", 0, false}};
            };
        controlSetPrefsBackendForTesting(std::move(backend));
    }

    ControlVocoderState state() const
    {
        if (!present)
            return {false, false, true};
        if (!commandAtMs)
            return {true, false, true};
        const auto elapsed = juce::Time::getMillisecondCounter() - *commandAtMs;
        if (elapsed < queuedMs)
            return {true, false, false};
        if (neverReady || elapsed < queuedMs + loadingMs)
            return {false, true, false};
        // A load that failed leaves no vocoder, no reconfiguration and an idle worker.
        if (commandFails_)
            return {false, false, true};
        return {true, false, true};
    }

    /** The backend the detector has chosen now: the one that a priority command chose, once the worker has started
        that command. */
    const char* inferenceBackend() const
    {
        ++backendReads;
        if (pendingBackend_ != nullptr && commandAtMs
            && juce::Time::getMillisecondCounter() - *commandAtMs >= queuedMs) {
            backend_ = pendingBackend_;
            pendingBackend_ = nullptr;
        }
        return backend_;
    }

    /** The backend that runs at the start, before any command, under this rendering priority. */
    void startWith(bool cpuFirst)
    {
        backend_ = backendFor(cpuFirst);
        pendingBackend_ = nullptr;
    }

    /** The runtime has finished everything it was asked: a ready vocoder and an idle worker. */
    void settle()
    {
        if (pendingBackend_ != nullptr)
            backend_ = pendingBackend_;
        pendingBackend_ = nullptr;
        commandAtMs.reset();
        commandFails_ = false;
        failNextLoad = false;
        neverReady = false;
        present = true;
    }

    /** The runtime is in the middle of a reconfiguration that never ends. */
    void getStuck()
    {
        present = true;
        neverReady = true;
        queuedMs = 0;
        commandFails_ = false;
        commandAtMs = juce::Time::getMillisecondCounter();
    }

    bool present = true;
    bool neverReady = false;
    /** OpenTune can create its inference session on the GPU: a request for gpuFirst ends on directml, otherwise on
        cpu (the GPU could not be used). */
    bool gpuAvailable = true;
    /** The load of the next command fails (a damaged weight): after the reload there is no vocoder. A command after it,
        such as the one that puts the previous weight back, loads. */
    bool failNextLoad = false;
    std::uint32_t queuedMs = 300;
    std::uint32_t loadingMs = 300;
    bool f0Accepts = true;
    std::vector<std::string> calls;
    /** How often the backend was asked for (session.info and a prefsApply job that changed the priority to gpuFirst).
     */
    mutable int backendReads = 0;

private:
    void command()
    {
        commandAtMs = juce::Time::getMillisecondCounter();
        commandFails_ = failNextLoad;
        failNextLoad = false;
    }

    const char* backendFor(bool cpuFirst) const { return cpuFirst || !gpuAvailable ? "cpu" : "directml"; }

    std::optional<std::uint32_t> commandAtMs;
    bool commandFails_ = false;
    mutable const char* backend_ = "directml";
    mutable const char* pendingBackend_ = nullptr;
};

std::vector<std::string> optionList(const JsonValue& array)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < array.size(); ++i)
        out.push_back(array.at(i).text());
    return out;
}

const JsonValue& optionsOf(const JsonValue& prefs)
{
    return memberOf(memberOf(prefs, "processing"), "options");
}

/** An empty directory of this scenario under OPENTUNE_DATA_DIR: the test's own models directory. */
std::filesystem::path freshModelsDirectory(const char* name)
{
    const auto root = isolatedDataDirectory();
    if (!CONTROL_CHECK(!root.empty()))
        return {};
    const auto dir = root / "prefs-models" / name;
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir, ignored);
    return dir;
}

void touchFile(const std::filesystem::path& file)
{
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    std::ofstream stream(file, std::ios::binary);
    stream << "x";
    CONTROL_CHECK(stream.good());
}

/** The job ended failed with this error, executed and retryable. */
bool jobFailedWith(const JsonValue& job, const char* kind, const char* reason, bool executed, bool retryable)
{
    const auto& data = memberOf(memberOf(job, "error"), "data");
    const bool reasonOk =
        reason[0] == '\0' ? data.find("reason") == nullptr : memberOf(data, "reason").text() == reason;
    const bool ok = memberOf(job, "state").text() == "failed" && memberOf(data, "kind").text() == kind && reasonOk
                    && memberOf(data, "executed").isBoolean() && memberOf(data, "executed").asBoolean() == executed
                    && memberOf(data, "retryable").isBoolean() && memberOf(data, "retryable").asBoolean() == retryable;
    if (!ok)
        note(std::string("expected a prefsApply job failed with ") + kind + "(" + reason + "), executed "
             + (executed ? "true" : "false") + ": " + toJsonString(job));
    return ok;
}

/** The job has not ended (queued or running); a read that does not wait. */
bool jobIsActive(ApiSession& api, const std::string& jobId)
{
    auto params = JsonValue::object();
    params.appendMember("jobId", JsonValue::string(jobId));
    const auto state = memberOf(memberOf(api.result("job.get", std::move(params)), "job"), "state").text();
    return state == "queued" || state == "running";
}

/** The job is still waiting at this point in time, taken a short while after it started: asserted only while the
    time since it started leaves no room for the wait to be over on a loaded machine (the wait lasts at least
    minimumWaitMs). */
void checkJobStillWaiting(ApiSession& api, const std::string& jobId, std::uint32_t startedAtMs,
                          std::uint32_t minimumWaitMs)
{
    const bool early = juce::Time::getMillisecondCounter() - startedAtMs + 100 < minimumWaitMs;
    const bool active = jobIsActive(api, jobId);
    if (early)
        CONTROL_CHECK(active);
}

/** The members the changes of a result name, in order. */
std::vector<std::string> changedPreferences(const JsonValue& result)
{
    std::vector<std::string> names;
    const auto& changes = memberOf(result, "changes");
    for (std::size_t i = 0; i < changes.size(); ++i)
        names.push_back(memberOf(memberOf(changes.at(i), "ref"), "preference").text());
    return names;
}

/** One change of a result: the label and the two values of its only field. */
bool changeIs(const JsonValue& result, std::size_t index, const char* name, const char* label, const JsonValue& before,
              const JsonValue& after)
{
    const auto& change = changeAt(result, index);
    const auto& field = fieldOf(change, name);
    const bool ok =
        memberOf(change, "entity").text() == "preference"
        && memberOf(memberOf(change, "ref"), "preference").text() == name && memberOf(change, "label").text() == label
        && memberOf(change, "fields").size() == 1 && toJsonString(memberOf(field, "before")) == toJsonString(before)
        && toJsonString(memberOf(field, "after")) == toJsonString(after) && memberOf(field, "unit").isNull();
    if (!ok)
        note(std::string("the change of ") + name + " is not as expected: " + toJsonString(change));
    return ok;
}

/** A job that the revert recipe of a result starts, sent unchanged; returns the job. */
JsonValue replayRevertAsJob(ApiSession& api, const JsonValue& result)
{
    const auto& revert = memberOf(memberOf(result, "undo"), "revert");
    if (!CONTROL_CHECK(revert.isObject() && memberOf(revert, "method").text() == "prefs.set"))
        return JsonValue();
    return runAsJob(api, "prefs.set", memberOf(revert, "params"), "prefsApply");
}

/** The lists of processing.options: empty without models, in the order of the window with them. */
void checkOptionLists(ApiSession& api, const std::filesystem::path& models)
{
    // Nothing on disk: nothing to choose among the weights and the pitch models; the enumeration is complete.
    {
        const auto prefs = api.result("prefs.get");
        const auto& options = optionsOf(prefs);
        CONTROL_CHECK(memberOf(options, "vocoderWeight").isArray() && memberOf(options, "vocoderWeight").size() == 0);
        CONTROL_CHECK(memberOf(options, "f0Model").isArray() && memberOf(options, "f0Model").size() == 0);
        CONTROL_CHECK(
            (optionList(memberOf(options, "renderingPriority")) == std::vector<std::string>{"gpuFirst", "cpuFirst"}));
        // The reference alignment mode is retired: prefs.get does not list it.
        CONTROL_CHECK(options.find("referenceAlignMode") == nullptr);
        // session.info reports the same processing object without the options.
        CONTROL_CHECK(memberOf(memberOf(api.result("session.info"), "processing"), "options").isNull());
    }

    // The built-in weights in the window's order, then the files of vocoder_weights sorted by name; a file of the
    // folder with the name of a built-in weight is not listed twice; a name longer than 64 characters can never be
    // sent back, so it is left out. The pitch model is listed when its file exists.
    touchFile(models / "hifigan_coulin9.onnx");
    touchFile(models / "hifigan.onnx");
    touchFile(models / "vocoder_weights" / kWeightZ);
    touchFile(models / "vocoder_weights" / kWeightA);
    touchFile(models / "vocoder_weights" / "hifigan.onnx");
    touchFile(models / "vocoder_weights" / std::filesystem::u8path("\xE5\xA3\xB0.onnx"));
    touchFile(models / "vocoder_weights" / (std::string(65, 'x') + ".onnx"));
    touchFile(models / "vocoder_weights" / "readme.txt");
    touchFile(models / "fcpe.onnx");
    {
        const auto prefs = api.result("prefs.get");
        const auto& options = optionsOf(prefs);
        CONTROL_CHECK((optionList(memberOf(options, "vocoderWeight"))
                       == std::vector<std::string>{"hifigan.onnx", "hifigan_coulin9.onnx", kWeightA, kWeightZ,
                                                   "\xE5\xA3\xB0.onnx"}));
        CONTROL_CHECK((optionList(memberOf(options, "f0Model")) == std::vector<std::string>{"fcpe"}));
        CONTROL_CHECK(
            (optionList(memberOf(options, "renderingPriority")) == std::vector<std::string>{"gpuFirst", "cpuFirst"}));
    }
}

/** vocoderWeight: not on offer, no-op, the change as a job that waits for the vocoder, the revert recipe. */
void checkVocoderWeight(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge, FakeRuntime& runtime,
                        const ClipHandles& clip)
{
    auto& preferences = processor.getAppPreferences();
    const auto baseline = api.result("prefs.get");
    const auto notifications = bridge.preferenceNotifications;

    // A weight that is not on offer changes nothing, also the other members of the call; the detail names the member
    // and not the value, and the error is not retryable.
    for (const char* params : {R"({"vocoderWeight":"nope.onnx"})", R"({"vocoderWeight":"nope.onnx","tuningHz":433})",
                               R"({"vocoderWeight":"HIFIGAN.onnx"})"})
        CONTROL_CHECK(api.fails("prefs.set", json(params), "INVALID_ARGUMENT", "option_unavailable"));
    {
        const auto response = api.request("prefs.set", json(R"({"vocoderWeight":"nope.onnx"})"));
        const auto* data = errorData(response);
        CONTROL_CHECK(data != nullptr && memberOf(*data, "detail").text().find("vocoderWeight") != std::string::npos
                      && memberOf(*data, "detail").text().find("nope") == std::string::npos
                      && !memberOf(*data, "retryable").asBoolean());
    }
    // Text of more than 64 characters is not a valid value at all.
    {
        auto params = JsonValue::object();
        params.appendMember("vocoderWeight", JsonValue::string(std::string(65, 'w')));
        CONTROL_CHECK(api.fails("prefs.set", std::move(params), "INVALID_ARGUMENT", "invalid_params"));
    }
    CONTROL_CHECK(runtime.calls.empty() && bridge.preferenceNotifications == notifications);
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));

    // The weight the preferences hold counts as unavailable when its file is gone: the check does not look at the
    // current value.
    preferences.setVocoderModelWeight("missing.onnx");
    CONTROL_CHECK(
        api.fails("prefs.set", json(R"({"vocoderWeight":"missing.onnx"})"), "INVALID_ARGUMENT", "option_unavailable"));
    preferences.setVocoderModelWeight(kDefaultVocoderWeight);

    // The current weight: a no-op, no job, no call to the runtime, the editor is not told.
    {
        const auto result = api.result("prefs.set", json(R"({"vocoderWeight":"hifigan.onnx"})"));
        CONTROL_CHECK(memberOf(result, "noOp").asBoolean() && memberOf(result, "job").isNull()
                      && memberOf(result, "changes").size() == 0 && memberOf(result, "warnings").size() == 0);
        CONTROL_CHECK(memberOf(memberOf(result, "undo"), "reason").text() == "no_op");
        CONTROL_CHECK(runtime.calls.empty() && bridge.preferenceNotifications == notifications);
    }

    // Another weight is a job: the call changes nothing, the job stores the weight, asks the processor, and ends when
    // the vocoder is back (first queued, then reconfiguring, then ready with an idle worker).
    runtime.queuedMs = 300;
    runtime.loadingMs = 300;
    const auto taken = juce::Time::getMillisecondCounter();
    const auto response = api.result("prefs.set", json(R"({"vocoderWeight":"aa-custom.onnx"})"));
    CONTROL_CHECK(!memberOf(response, "applied").asBoolean() && memberOf(response, "changes").size() == 0
                  && memberOf(memberOf(response, "job"), "kind").text() == "prefsApply");
    CONTROL_CHECK(preferences.getState().shared.vocoderModelWeight == kDefaultVocoderWeight);
    const auto jobId = jobIdOf(response);
    CONTROL_CHECK(
        pumpMessagesUntil([&] { return preferences.getState().shared.vocoderModelWeight == kWeightA; }, kJobChangeMs));
    // The vocoder is still ready and not reconfiguring while the worker has not started the command (the flag is set
    // later, by the worker): the job is not over then, nor while the vocoder is reloading.
    pumpMessagesFor(150);
    checkJobStillWaiting(api, jobId, taken, 600);
    pumpMessagesFor(300);
    checkJobStillWaiting(api, jobId, taken, 600);
    const auto job = waitJob(api, jobId);
    CONTROL_CHECK(jobSucceeded(job));
    CONTROL_CHECK(juce::Time::getMillisecondCounter() - taken >= 600);
    const auto& result = memberOf(job, "result");
    CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean());
    CONTROL_CHECK(memberOf(result, "changes").size() == 1);
    CONTROL_CHECK(changeIs(result, 0, "vocoderWeight", "Vocoder weight", JsonValue::string("hifigan.onnx"),
                           JsonValue::string(kWeightA)));
    CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE"}));
    CONTROL_CHECK((globalPreferences(result) == std::vector<std::string>{"vocoderWeight"}));
    CONTROL_CHECK(undoIsUiParity(result));
    // The weight is stored and the processor was asked once: the runtime's completion renders every clip again.
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{std::string("weight:") + kWeightA}));
    CONTROL_CHECK(bridge.preferenceNotifications == notifications + 1);
    const auto& queued = memberOf(memberOf(memberOf(result, "sideEffects"), "reRenderQueued"), "contentIds");
    CONTROL_CHECK(queued.size() == 1 && queued.at(0).text() == clip.contentId);
    CONTROL_CHECK(memberOf(memberOf(memberOf(result, "sideEffects"), "reRenderQueued"), "revert").isNull());
    CONTROL_CHECK(memberOf(memberOf(api.result("prefs.get"), "processing"), "vocoderWeight").text() == kWeightA);
    CONTROL_CHECK(memberOf(memberOf(api.result("session.info"), "processing"), "vocoderWeight").text() == kWeightA);
    const auto& params = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
    CONTROL_CHECK(params.size() == 1 && memberOf(params, "vocoderWeight").text() == "hifigan.onnx");

    // The revert recipe, sent as it is, takes the weight back (another job); the preferences read as before.
    const auto reverted = replayRevertAsJob(api, result);
    CONTROL_CHECK(jobSucceeded(reverted));
    CONTROL_CHECK(memberOf(memberOf(reverted, "result"), "changes").size() == 1);
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    CONTROL_CHECK(
        (runtime.calls == std::vector<std::string>{std::string("weight:") + kWeightA, "weight:hifigan.onnx"}));
    runtime.calls.clear();
    runtime.settle();

    // A weight the runtime already holds is answered without any flag: the vocoder is never reconfiguring, and the job
    // still ends, when the worker is done with the command.
    runtime.queuedMs = 250;
    runtime.loadingMs = 0;
    const auto heldAt = juce::Time::getMillisecondCounter();
    const auto held = runAsJob(api, "prefs.set", json(R"({"vocoderWeight":"zz-custom.onnx"})"), "prefsApply");
    CONTROL_CHECK(jobSucceeded(held));
    CONTROL_CHECK(juce::Time::getMillisecondCounter() - heldAt >= 250);
    CONTROL_CHECK(memberOf(memberOf(held, "result"), "sideEffects").find("reRenderQueued") != nullptr);
    preferences.setVocoderModelWeight(kDefaultVocoderWeight);
    runtime.calls.clear();
    runtime.settle();
}

/** A stored vocoder weight whose name the wire cannot carry (here longer than 64 code points) is left out of the revert
    recipe. When nothing else changed, the recipe is null (reason ui_parity, and the warning IRREVERSIBLE), never a
    prefs.set without params; with another member changed, the recipe holds that member alone. */
void checkUnsendableStoredWeight(ApiSession& api, OpenTuneAudioProcessor& processor, FakeRuntime& runtime)
{
    auto& preferences = processor.getAppPreferences();
    runtime.queuedMs = 100;
    runtime.loadingMs = 100;
    const std::string stored(65, 'w');

    preferences.setVocoderModelWeight(stored);
    {
        const auto job = runAsJob(api, "prefs.set", json(R"({"vocoderWeight":"aa-custom.onnx"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        const auto& result = memberOf(job, "result");
        CONTROL_CHECK(memberOf(result, "changes").size() == 1);
        CONTROL_CHECK(memberOf(fieldOf(changeAt(result, 0), "vocoderWeight"), "before").isNull());
        CONTROL_CHECK(memberOf(fieldOf(changeAt(result, 0), "vocoderWeight"), "after").text() == kWeightA);
        CONTROL_CHECK(undoIsUiParity(result));
        CONTROL_CHECK(memberOf(memberOf(result, "undo"), "revert").isNull());
        CONTROL_CHECK(hasWarning(result, "IRREVERSIBLE"));
    }

    runtime.settle();
    preferences.setVocoderModelWeight(stored);
    {
        const auto job =
            runAsJob(api, "prefs.set", json(R"({"vocoderWeight":"aa-custom.onnx","tuningHz":433})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        const auto& result = memberOf(job, "result");
        CONTROL_CHECK(memberOf(result, "changes").size() == 2);
        CONTROL_CHECK(undoIsUiParity(result));
        const auto& params = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
        CONTROL_CHECK(params.size() == 1 && numberIs(memberOf(params, "tuningHz"), 440.0));
        CONTROL_CHECK(!hasWarning(result, "IRREVERSIBLE"));
        replayRevert(api, result);
        CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
    }
    runtime.calls.clear();
    runtime.settle();
    resetPreferences(processor);
}

/** renderingPriority: no-op, the change as a job that waits for the vocoder, no render again, the revert recipe. */
void checkRenderingPriority(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge,
                            FakeRuntime& runtime)
{
    auto& preferences = processor.getAppPreferences();
    const auto baseline = api.result("prefs.get");
    const auto notifications = bridge.preferenceNotifications;

    {
        const auto result = api.result("prefs.set", json(R"({"renderingPriority":"gpuFirst"})"));
        CONTROL_CHECK(memberOf(result, "noOp").asBoolean() && memberOf(result, "job").isNull());
        CONTROL_CHECK(runtime.calls.empty() && bridge.preferenceNotifications == notifications);
    }

    runtime.queuedMs = 250;
    runtime.loadingMs = 250;
    const auto taken = juce::Time::getMillisecondCounter();
    const auto job = runAsJob(api, "prefs.set", json(R"({"renderingPriority":"cpuFirst"})"), "prefsApply");
    CONTROL_CHECK(jobSucceeded(job));
    CONTROL_CHECK(juce::Time::getMillisecondCounter() - taken >= 500);
    const auto& result = memberOf(job, "result");
    CONTROL_CHECK(memberOf(result, "changes").size() == 1);
    CONTROL_CHECK(changeIs(result, 0, "renderingPriority", "Rendering priority", JsonValue::string("gpuFirst"),
                           JsonValue::string("cpuFirst")));
    CONTROL_CHECK((globalPreferences(result) == std::vector<std::string>{"renderingPriority"}));
    CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE"}));
    CONTROL_CHECK(undoIsUiParity(result));
    CONTROL_CHECK(preferences.getState().shared.renderingPriority == RenderingPriority::CpuFirst);
    CONTROL_CHECK(memberOf(memberOf(api.result("prefs.get"), "processing"), "renderingPriority").text() == "cpuFirst");
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{"priority:cpuFirst"}));
    // Resetting the inference backend clears no render cache: nothing is rendered again.
    CONTROL_CHECK(memberOf(result, "sideEffects").size() == 0);
    const auto& params = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
    CONTROL_CHECK(params.size() == 1 && memberOf(params, "renderingPriority").text() == "gpuFirst");

    CONTROL_CHECK(jobSucceeded(replayRevertAsJob(api, result)));
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{"priority:cpuFirst", "priority:gpuFirst"}));
    runtime.calls.clear();
    runtime.settle();
}

/**
 * BACKEND_FALLBACK (PROTOCOL.md section 12.7 rule 7): the result of the prefsApply job carries it when this call
 * changed the rendering priority to gpuFirst and the backend that is reported when the job ends is cpu, with a null
 * subject and the data {requested: gpuFirst, actual: cpu}, after GLOBAL_PREFERENCE. It does not when the GPU runs, when
 * the priority becomes cpuFirst or does not change, and the backend is read once (and not at all by a call that does
 * not change the priority to gpuFirst). A job that does not wait for the vocoder reads the backend of before the
 * change.
 */
void checkBackendFallback(ApiSession& api, OpenTuneAudioProcessor& processor, FakeRuntime& runtime)
{
    auto& preferences = processor.getAppPreferences();
    runtime.queuedMs = 150;
    runtime.loadingMs = 150;
    const auto processing = [&api] { return memberOf(api.result("session.info"), "processing"); };
    // A start under this priority, on a machine whose GPU can or cannot be used.
    const auto startUnder = [&](bool cpuFirst, bool gpu) {
        runtime.settle();
        runtime.gpuAvailable = gpu;
        runtime.startWith(cpuFirst);
        runtime.calls.clear();
        preferences.setRenderingPriority(cpuFirst ? RenderingPriority::CpuFirst : RenderingPriority::GpuFirst);
    };
    const auto fallbackIs = [](const JsonValue& result) {
        const JsonValue* fallback = findWarning(result, "BACKEND_FALLBACK");
        const bool ok = fallback != nullptr && memberOf(*fallback, "subject").isNull()
                        && toJsonString(memberOf(*fallback, "data")) == R"({"requested":"gpuFirst","actual":"cpu"})";
        if (!ok)
            note("BACKEND_FALLBACK is not as expected: " + toJsonString(memberOf(result, "warnings")));
        return ok;
    };

    // The GPU cannot be used: the change to gpuFirst ends on the CPU. The call succeeded and the preference is set; the
    // warning is the only sign, besides session.info, that the backend is not the one requested.
    startUnder(true, false);
    runtime.backendReads = 0;
    {
        const auto job = runAsJob(api, "prefs.set", json(R"({"renderingPriority":"gpuFirst"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        const auto& result = memberOf(job, "result");
        CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE", "BACKEND_FALLBACK"}));
        CONTROL_CHECK(fallbackIs(result));
        CONTROL_CHECK((globalPreferences(result) == std::vector<std::string>{"renderingPriority"}));
        CONTROL_CHECK(undoIsUiParity(result) && memberOf(result, "sideEffects").size() == 0);
        CONTROL_CHECK(runtime.backendReads == 1);
        CONTROL_CHECK(preferences.getState().shared.renderingPriority == RenderingPriority::GpuFirst);
        const auto after = processing();
        CONTROL_CHECK(memberOf(after, "renderingPriority").text() == "gpuFirst"
                      && memberOf(after, "backend").text() == "cpu");
    }

    // The GPU runs: no warning.
    startUnder(true, true);
    runtime.backendReads = 0;
    {
        const auto job = runAsJob(api, "prefs.set", json(R"({"renderingPriority":"gpuFirst"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        const auto& result = memberOf(job, "result");
        CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE"}));
        CONTROL_CHECK(runtime.backendReads == 1);
        const auto after = processing();
        CONTROL_CHECK(memberOf(after, "renderingPriority").text() == "gpuFirst"
                      && memberOf(after, "backend").text() == "directml");
    }

    // The change to cpuFirst on a machine without a GPU: the backend is cpu, as asked, so there is no warning and the
    // backend is not even read.
    startUnder(false, false);
    runtime.backendReads = 0;
    {
        const auto job = runAsJob(api, "prefs.set", json(R"({"renderingPriority":"cpuFirst"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK((warningCodes(memberOf(job, "result")) == std::vector<std::string>{"GLOBAL_PREFERENCE"}));
        CONTROL_CHECK(runtime.backendReads == 0);
    }

    // A call that does not change the priority carries no warning, whatever the backend is: the priority is gpuFirst
    // already (a no-op, which starts no job), and another preference changes on its own.
    startUnder(false, false);
    runtime.backendReads = 0;
    {
        const auto same = api.result("prefs.set", json(R"({"renderingPriority":"gpuFirst"})"));
        CONTROL_CHECK(memberOf(same, "noOp").asBoolean() && memberOf(same, "warnings").size() == 0);
        const auto other = api.result("prefs.set", json(R"({"experimentalFeatures":true})"));
        CONTROL_CHECK((warningCodes(other) == std::vector<std::string>{"GLOBAL_PREFERENCE"}));
        CONTROL_CHECK(runtime.backendReads == 0);
    }

    // One call that changes the tuning and the priority: the warning comes last, after those of the other preferences.
    startUnder(true, false);
    {
        const auto job =
            runAsJob(api, "prefs.set", json(R"({"tuningHz":432,"renderingPriority":"gpuFirst"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        const auto& result = memberOf(job, "result");
        CONTROL_CHECK((warningCodes(result)
                       == std::vector<std::string>{"GLOBAL_PREFERENCE", "NOT_RETROACTIVE", "BACKEND_FALLBACK"}));
        CONTROL_CHECK(fallbackIs(result));
    }

    // A job that does not wait for the vocoder (it was not ready before the change) reads the backend once, when it
    // ends, and that can be the backend from before the change: the worker has not started the command yet (the rule
    // says the warning is then decided from the earlier backend).
    startUnder(true, true);
    runtime.queuedMs = 60000;
    runtime.present = false;
    runtime.backendReads = 0;
    {
        const auto job = runAsJob(api, "prefs.set", json(R"({"renderingPriority":"gpuFirst"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK(fallbackIs(memberOf(job, "result")));
        CONTROL_CHECK(runtime.backendReads == 1);
        runtime.settle();
        CONTROL_CHECK(memberOf(processing(), "backend").text() == "directml");
    }

    runtime.settle();
    runtime.gpuAvailable = true;
    runtime.startWith(false);
    runtime.calls.clear();
    runtime.queuedMs = 300;
    runtime.loadingMs = 300;
    resetPreferences(processor);
}

/** f0Model: not on offer, no-op, the change, a processor that refuses (also inside a job), the order, the revert. */
void checkPitchModel(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge, FakeRuntime& runtime)
{
    auto& preferences = processor.getAppPreferences();
    const auto baseline = api.result("prefs.get");
    const auto notifications = bridge.preferenceNotifications;
    CONTROL_CHECK((optionList(memberOf(optionsOf(baseline), "f0Model")) == std::vector<std::string>{"fcpe", "other"}));

    // A model that is not on offer, or is listed without its file, changes nothing.
    for (const char* params : {R"({"f0Model":"nope"})", R"({"f0Model":"gone"})", R"({"f0Model":"FCPE"})",
                               R"({"f0Model":"other","vocoderWeight":"nope.onnx"})"})
        CONTROL_CHECK(api.fails("prefs.set", json(params), "INVALID_ARGUMENT", "option_unavailable"));
    CONTROL_CHECK(runtime.calls.empty() && bridge.preferenceNotifications == notifications);

    // The model in use: a no-op that does not even ask the processor.
    {
        const auto result = api.result("prefs.set", json(R"({"f0Model":"fcpe"})"));
        CONTROL_CHECK(memberOf(result, "noOp").asBoolean() && memberOf(result, "changes").size() == 0);
        CONTROL_CHECK(runtime.calls.empty() && bridge.preferenceNotifications == notifications);
    }

    // The processor refuses: the call fails with MODEL_UNAVAILABLE(model_missing), executed false, and changes
    // nothing, also not the other members of the call; the model is the first thing asked.
    runtime.f0Accepts = false;
    CONTROL_CHECK(api.fails("prefs.set", json(R"({"f0Model":"other","tuningHz":433,"hybridMode":false})"),
                            "MODEL_UNAVAILABLE", "model_missing"));
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{"f0:2"}));
    CONTROL_CHECK(bridge.preferenceNotifications == notifications && TuningConfig::currentTuningHz() == 440.0f);
    CONTROL_CHECK(preferences.getF0ModelType() == F0ModelType::FCPE);
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    // The same inside a job (the call also changes the scheme, the vocoder weight and the priority): the job fails
    // with the same error, executed false; the runtime is asked nothing else.
    runtime.calls.clear();
    const auto refused = runAsJob(
        api, "prefs.set",
        json(
            R"({"f0Model":"other","editingScheme":"openDyne","vocoderWeight":"aa-custom.onnx","renderingPriority":"cpuFirst"})"),
        "prefsApply");
    CONTROL_CHECK(jobFailedWith(refused, "MODEL_UNAVAILABLE", "model_missing", false, false));
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{"f0:2"}));
    CONTROL_CHECK(bridge.preferenceNotifications == notifications);
    CONTROL_CHECK(preferences.getF0ModelType() == F0ModelType::FCPE);
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(preferences.getState().shared.audioEditingScheme)
                  && preferences.getState().shared.vocoderModelWeight == kDefaultVocoderWeight
                  && preferences.getState().shared.renderingPriority == RenderingPriority::GpuFirst);
    // Nothing of the failed job stays pending: a later job is not held back.
    CONTROL_CHECK(jobSucceeded(runAsJob(api, "prefs.set", json(R"({"editingScheme":"openDyne"})"), "prefsApply")));
    CONTROL_CHECK(jobSucceeded(runAsJob(api, "prefs.set", json(R"({"editingScheme":"openTune"})"), "prefsApply")));
    runtime.calls.clear();
    const auto afterJobs = bridge.preferenceNotifications;

    // The processor accepts: the model is stored, the change is listed, the revert recipe takes it back.
    runtime.f0Accepts = true;
    const auto result = api.result("prefs.set", json(R"({"f0Model":"other"})"));
    CONTROL_CHECK(memberOf(result, "applied").asBoolean() && memberOf(result, "job").isNull());
    CONTROL_CHECK(memberOf(result, "changes").size() == 1);
    CONTROL_CHECK(changeIs(result, 0, "f0Model", "Pitch model", JsonValue::string("fcpe"), JsonValue::string("other")));
    CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE"}));
    CONTROL_CHECK((globalPreferences(result) == std::vector<std::string>{"f0Model"}));
    CONTROL_CHECK(undoIsUiParity(result) && memberOf(result, "sideEffects").size() == 0);
    CONTROL_CHECK(preferences.getF0ModelType() == kOtherPitchModel);
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{"f0:2"}));
    CONTROL_CHECK(bridge.preferenceNotifications == afterJobs + 1);
    const auto& params = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
    CONTROL_CHECK(params.size() == 1 && memberOf(params, "f0Model").text() == "fcpe");
    // The model in use is "other" now, so "fcpe" changes it back and "other" is the no-op.
    CONTROL_CHECK(memberOf(api.result("prefs.set", json(R"({"f0Model":"other"})")), "noOp").asBoolean());
    const auto reverted = replayRevert(api, result);
    CONTROL_CHECK(memberOf(reverted, "applied").asBoolean() && memberOf(reverted, "changes").size() == 1);
    CONTROL_CHECK(preferences.getF0ModelType() == F0ModelType::FCPE);
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    runtime.calls.clear();
}

/** All seven members in one call: the order of the changes, of GLOBAL_PREFERENCE and of the revert recipe, the order of
    the calls (the pitch model first), one reRenderQueued, and the revert recipe taking all seven back. */
void checkAllMembers(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge, FakeRuntime& runtime,
                     const ClipHandles& clip)
{
    auto& preferences = processor.getAppPreferences();
    const auto baseline = api.result("prefs.get");
    const auto notifications = bridge.preferenceNotifications;
    runtime.queuedMs = 100;
    runtime.loadingMs = 100;
    const auto job = runAsJob(
        api, "prefs.set",
        json(
            R"({"f0Model":"other","renderingPriority":"cpuFirst","vocoderWeight":"zz-custom.onnx","experimentalFeatures":true,"hybridMode":false,"editingScheme":"openDyne","tuningHz":435.5})"),
        "prefsApply");
    CONTROL_CHECK(jobSucceeded(job));
    // The editor is told once for the seven members.
    CONTROL_CHECK(bridge.preferenceNotifications == notifications + 1);
    const auto& result = memberOf(job, "result");
    const std::vector<std::string> order{"tuningHz",      "editingScheme",     "hybridMode", "experimentalFeatures",
                                         "vocoderWeight", "renderingPriority", "f0Model"};
    CONTROL_CHECK(changedPreferences(result) == order);
    CONTROL_CHECK(globalPreferences(result) == order);
    CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE", "NOT_RETROACTIVE"}));
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{"f0:2", "weight:zz-custom.onnx", "priority:cpuFirst"}));
    // The hybrid switch and the vocoder weight make one side effect, not two.
    CONTROL_CHECK(memberOf(result, "sideEffects").size() == 1);
    const auto& queued = memberOf(memberOf(memberOf(result, "sideEffects"), "reRenderQueued"), "contentIds");
    CONTROL_CHECK(queued.size() == 1 && queued.at(0).text() == clip.contentId);
    const auto& params = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
    CONTROL_CHECK(params.size() == 7);
    CONTROL_CHECK(
        numberIs(memberOf(params, "tuningHz"), 440.0) && memberOf(params, "editingScheme").text() == "openTune"
        && memberOf(params, "hybridMode").asBoolean() && !memberOf(params, "experimentalFeatures").asBoolean()
        && memberOf(params, "vocoderWeight").text() == "hifigan.onnx"
        && memberOf(params, "renderingPriority").text() == "gpuFirst" && memberOf(params, "f0Model").text() == "fcpe");
    const auto state = preferences.getState().shared;
    CONTROL_CHECK(AudioEditingScheme::usesNotesPrimaryScheme(state.audioEditingScheme)
                  && !state.lightPitchCorrectionEnabled && state.experimentalFeaturesEnabled
                  && state.vocoderModelWeight == kWeightZ && state.renderingPriority == RenderingPriority::CpuFirst
                  && state.f0ModelType == kOtherPitchModel);

    // Sent as it is, the recipe takes all seven back.
    runtime.calls.clear();
    const auto reverted = replayRevertAsJob(api, result);
    CONTROL_CHECK(jobSucceeded(reverted));
    CONTROL_CHECK(memberOf(memberOf(reverted, "result"), "changes").size() == 7);
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    CONTROL_CHECK(preferences.getF0ModelType() == F0ModelType::FCPE);
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{"f0:1", "weight:hifigan.onnx", "priority:gpuFirst"}));
    runtime.calls.clear();
    runtime.settle();
}

} // namespace

void scenarioPrefsOptions()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    resetPreferences(*processor);
    PrefsBridge bridge;
    bridge.processor = processor.get();
    bridge.preferences = &processor->getAppPreferences();
    ScopedBridge registered(service, bridge);
    fixtureAddClip(*processor, readyClip("options"));
    ApiSession api;
    if (!api.open(service))
        return;
    const auto handles = fixtureHandlesOf(api.result("project.get"), 0, 2.0);
    const auto models = freshModelsDirectory("options");
    if (!CONTROL_CHECK(handles.has_value() && !models.empty()))
        return;
    FakeRuntime runtime;
    runtime.install(models);
    checkOptionLists(api, models);
    checkVocoderWeight(api, *processor, bridge, runtime, *handles);
    checkRenderingPriority(api, *processor, bridge, runtime);
    checkBackendFallback(api, *processor, runtime);
    // From here the pitch models are a fake list of three: fcpe, other and gone (not available).
    runtime.install(models, true);
    checkPitchModel(api, *processor, bridge, runtime);
    checkAllMembers(api, *processor, bridge, runtime, *handles);
    resetPreferences(*processor);
    const auto dispatcher = service.dispatcher();
    CONTROL_CHECK(dispatcher != nullptr && dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

// ---- The wait of a job for the vocoder
// ----------------------------------------------------------------------------

namespace {

/** The models directory of the vocoder scenarios: the two built-in weights and two custom ones. */
std::filesystem::path vocoderModels(const char* name)
{
    const auto models = freshModelsDirectory(name);
    if (models.empty())
        return models;
    touchFile(models / "hifigan.onnx");
    touchFile(models / "hifigan_coulin9.onnx");
    touchFile(models / "vocoder_weights" / kWeightA);
    touchFile(models / "vocoder_weights" / kWeightZ);
    return models;
}

/** Puts the runtime, the preferences and the timeouts back for the next check. */
void settleVocoderScenario(OpenTuneAudioProcessor& processor, FakeRuntime& runtime)
{
    controlSetPrefsApplyTimeoutForTesting(0);
    controlSetPrefsVocoderWaitForTesting(0);
    runtime.settle();
    runtime.calls.clear();
    runtime.queuedMs = 300;
    runtime.loadingMs = 300;
    resetPreferences(processor);
}

/** The vocoder was ready before the change, so the job waits for it; when it was not (no vocoder in the runtime, or
    one that is reloading already) the job does not wait and ends as soon as the changes are made: a job that waited
    would fail here, as the vocoder never gets ready. It reports the render of every content for a new weight all the
    same: OpenTune renders once the weight has loaded, which can be after the job ended. A new rendering priority
    renders nothing again. */
void checkNoWaitWhenNotReady(ApiSession& api, OpenTuneAudioProcessor& processor, FakeRuntime& runtime)
{
    auto& preferences = processor.getAppPreferences();
    controlSetPrefsVocoderWaitForTesting(800);
    const auto reRenderQueued = [](const JsonValue& job) -> const JsonValue& {
        return memberOf(memberOf(memberOf(job, "result"), "sideEffects"), "reRenderQueued");
    };

    // A runtime without a vocoder.
    runtime.present = false;
    {
        const auto job = runAsJob(api, "prefs.set", json(R"({"vocoderWeight":"aa-custom.onnx"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK(memberOf(memberOf(job, "result"), "changes").size() == 1);
        CONTROL_CHECK(memberOf(reRenderQueued(job), "contentIds").size() == 1
                      && memberOf(reRenderQueued(job), "revert").isNull());
        CONTROL_CHECK(preferences.getState().shared.vocoderModelWeight == kWeightA);
        CONTROL_CHECK((runtime.calls == std::vector<std::string>{std::string("weight:") + kWeightA}));
    }
    {
        const auto job = runAsJob(api, "prefs.set", json(R"({"renderingPriority":"cpuFirst"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK(preferences.getState().shared.renderingPriority == RenderingPriority::CpuFirst);
        CONTROL_CHECK(memberOf(memberOf(job, "result"), "sideEffects").size() == 0);
    }

    // A vocoder that is reconfiguring already, whatever the reason.
    runtime.getStuck();
    runtime.calls.clear();
    {
        const auto job =
            runAsJob(api, "prefs.set", json(R"({"vocoderWeight":"hifigan.onnx","tuningHz":434})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK(memberOf(memberOf(job, "result"), "changes").size() == 2);
        CONTROL_CHECK(memberOf(reRenderQueued(job), "contentIds").size() == 1);
        CONTROL_CHECK((runtime.calls == std::vector<std::string>{"weight:hifigan.onnx"}));
    }

    // A change that touches neither the weight nor the priority never looks at the vocoder.
    {
        const auto job = runAsJob(api, "prefs.set", json(R"({"editingScheme":"openDyne"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK(AudioEditingScheme::usesNotesPrimaryScheme(preferences.getState().shared.audioEditingScheme));
        CONTROL_CHECK(jobSucceeded(runAsJob(api, "prefs.set", json(R"({"editingScheme":"openTune"})"), "prefsApply")));
    }
    settleVocoderScenario(processor, runtime);
}

/** The vocoder is not ready again in time: the job takes the preferences back, tells the editor, asks the runtime for
    the previous weight or priority again, and fails with MODEL_UNAVAILABLE(vocoder_not_ready), retryable and with
    executed false; asked again once the vocoder works, the same call succeeds. */
void checkVocoderTimeout(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge, FakeRuntime& runtime)
{
    const auto baseline = api.result("prefs.get");
    constexpr std::uint32_t kLimitMs = 600;
    struct Case
    {
        const char* params;
        std::vector<std::string> calls;
    };
    const Case cases[] = {
        {R"({"vocoderWeight":"aa-custom.onnx","tuningHz":433})",
         {std::string("weight:") + kWeightA, "weight:hifigan.onnx"}},
        {R"({"renderingPriority":"cpuFirst","hybridMode":false})", {"priority:cpuFirst", "priority:gpuFirst"}},
        {R"({"vocoderWeight":"zz-custom.onnx","renderingPriority":"cpuFirst"})",
         {std::string("weight:") + kWeightZ, "priority:cpuFirst", "weight:hifigan.onnx", "priority:gpuFirst"}},
    };
    for (const auto& item : cases) {
        controlSetPrefsVocoderWaitForTesting(kLimitMs);
        runtime.settle();
        runtime.calls.clear();
        runtime.neverReady = true;
        const auto notifications = bridge.preferenceNotifications;
        const auto startedAt = juce::Time::getMillisecondCounter();
        const auto job = runAsJob(api, "prefs.set", json(item.params), "prefsApply");
        CONTROL_CHECK(jobFailedWith(job, "MODEL_UNAVAILABLE", "vocoder_not_ready", false, true));
        CONTROL_CHECK(juce::Time::getMillisecondCounter() - startedAt >= kLimitMs);
        // The detail is fixed text: it names no file.
        const auto detail = memberOf(memberOf(memberOf(job, "error"), "data"), "detail").text();
        CONTROL_CHECK(detail.find("vocoder") != std::string::npos && detail.find("custom") == std::string::npos);
        CONTROL_CHECK(runtime.calls == item.calls);
        CONTROL_CHECK(bridge.preferenceNotifications == notifications + 2);
        CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    }

    // The vocoder works again: the call that failed succeeds and its revert recipe takes everything back.
    controlSetPrefsVocoderWaitForTesting(0);
    runtime.settle();
    runtime.calls.clear();
    runtime.queuedMs = 100;
    runtime.loadingMs = 100;
    const auto job = runAsJob(api, "prefs.set", json(cases[0].params), "prefsApply");
    CONTROL_CHECK(jobSucceeded(job));
    CONTROL_CHECK(memberOf(memberOf(job, "result"), "changes").size() == 2);
    const auto reverted = replayRevertAsJob(api, memberOf(job, "result"));
    CONTROL_CHECK(jobSucceeded(reverted));
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    settleVocoderScenario(processor, runtime);
}

/** The runtime ends the switch without a vocoder (the weight could not be loaded): the job does not wait for its
    limit, it takes the preferences back and fails at once with MODEL_UNAVAILABLE(vocoder_load_failed), not retryable
    and with executed false; the runtime is asked for the previous weight or priority again, which loads, and the same
    call succeeds once the weight loads. */
void checkVocoderLoadFailure(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge,
                             FakeRuntime& runtime)
{
    const auto baseline = api.result("prefs.get");
    // A limit that a job reaching it would fail with vocoder_not_ready after, far later than the failure is seen.
    constexpr std::uint32_t kLimitMs = 4000;
    constexpr std::uint32_t kSwitchMs = 400;
    struct Case
    {
        const char* params;
        std::vector<std::string> calls;
    };
    const Case cases[] = {
        {R"({"vocoderWeight":"aa-custom.onnx","tuningHz":433})",
         {std::string("weight:") + kWeightA, "weight:hifigan.onnx"}},
        {R"({"renderingPriority":"cpuFirst","hybridMode":false})", {"priority:cpuFirst", "priority:gpuFirst"}},
        {R"({"vocoderWeight":"zz-custom.onnx","experimentalFeatures":true})",
         {std::string("weight:") + kWeightZ, "weight:hifigan.onnx"}},
    };
    for (const auto& item : cases) {
        controlSetPrefsVocoderWaitForTesting(kLimitMs);
        runtime.settle();
        runtime.calls.clear();
        runtime.queuedMs = kSwitchMs / 2;
        runtime.loadingMs = kSwitchMs / 2;
        runtime.failNextLoad = true;
        const auto notifications = bridge.preferenceNotifications;
        const auto startedAt = juce::Time::getMillisecondCounter();
        const auto job = runAsJob(api, "prefs.set", json(item.params), "prefsApply");
        const auto elapsed = juce::Time::getMillisecondCounter() - startedAt;
        CONTROL_CHECK(jobFailedWith(job, "MODEL_UNAVAILABLE", "vocoder_load_failed", false, false));
        // The failure is seen once the runtime has ended the switch, not when the limit is reached.
        CONTROL_CHECK(elapsed >= kSwitchMs && elapsed < kLimitMs);
        // The detail is fixed text: it names no file.
        const auto detail = memberOf(memberOf(memberOf(job, "error"), "data"), "detail").text();
        CONTROL_CHECK(detail.find("vocoder") != std::string::npos && detail.find("custom") == std::string::npos);
        CONTROL_CHECK(runtime.calls == item.calls);
        CONTROL_CHECK(bridge.preferenceNotifications == notifications + 2);
        CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    }

    // The scheme and the vocoder in one job: the failure of the vocoder ends the job while the editor is still busy
    // with the scheme, and the scheme goes back too.
    {
        bridge.apply = PrefsBridge::Apply::AfterDelay;
        bridge.applyDelayMs = 100;
        controlSetPrefsVocoderWaitForTesting(kLimitMs);
        runtime.settle();
        runtime.calls.clear();
        runtime.failNextLoad = true;
        const auto job = runAsJob(
            api, "prefs.set", json(R"({"editingScheme":"openDyne","vocoderWeight":"aa-custom.onnx"})"), "prefsApply");
        CONTROL_CHECK(jobFailedWith(job, "MODEL_UNAVAILABLE", "vocoder_load_failed", false, false));
        CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(
            processor.getAppPreferences().getState().shared.audioEditingScheme));
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
        bridge.apply = PrefsBridge::Apply::Immediate;
        bridge.applyDelayMs = 0;
    }

    // The weight loads now: the call that failed succeeds, and its revert recipe takes everything back.
    controlSetPrefsVocoderWaitForTesting(0);
    runtime.settle();
    runtime.calls.clear();
    runtime.queuedMs = 100;
    runtime.loadingMs = 100;
    const auto job = runAsJob(api, "prefs.set", json(cases[0].params), "prefsApply");
    CONTROL_CHECK(jobSucceeded(job));
    CONTROL_CHECK(memberOf(memberOf(job, "result"), "changes").size() == 2);
    const auto reverted = replayRevertAsJob(api, memberOf(job, "result"));
    CONTROL_CHECK(jobSucceeded(reverted));
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    settleVocoderScenario(processor, runtime);
}

} // namespace

void scenarioPrefsVocoderJob()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    resetPreferences(*processor);
    PrefsBridge bridge;
    bridge.processor = processor.get();
    bridge.preferences = &processor->getAppPreferences();
    ScopedBridge registered(service, bridge);
    fixtureAddClip(*processor, readyClip("vocoder-job"));
    ApiSession api;
    if (!api.open(service))
        return;
    const auto models = vocoderModels("vocoder-job");
    if (!CONTROL_CHECK(!models.empty()))
        return;
    FakeRuntime runtime;
    runtime.install(models);
    checkNoWaitWhenNotReady(api, *processor, runtime);
    checkVocoderTimeout(api, *processor, bridge, runtime);
    checkVocoderLoadFailure(api, *processor, bridge, runtime);
    checkUnsendableStoredWeight(api, *processor, runtime);
    resetPreferences(*processor);
    const auto dispatcher = service.dispatcher();
    CONTROL_CHECK(dispatcher != nullptr && dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

namespace {

/** The scheme and the vocoder in one job: it ends when both hold, whichever comes last. */
void checkSchemeAndVocoder(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge,
                           FakeRuntime& runtime)
{
    auto& preferences = processor.getAppPreferences();
    bridge.apply = PrefsBridge::Apply::AfterDelay;

    // The editor is the slow one (700 ms; the vocoder is back after 300).
    runtime.queuedMs = 150;
    runtime.loadingMs = 150;
    bridge.applyDelayMs = 700;
    {
        const auto startedAt = juce::Time::getMillisecondCounter();
        const auto job = runAsJob(
            api, "prefs.set", json(R"({"editingScheme":"openDyne","vocoderWeight":"aa-custom.onnx"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK(juce::Time::getMillisecondCounter() - startedAt >= 700);
        CONTROL_CHECK(memberOf(memberOf(job, "result"), "changes").size() == 2);
        CONTROL_CHECK(AudioEditingScheme::usesNotesPrimaryScheme(preferences.getState().shared.audioEditingScheme));
        CONTROL_CHECK(preferences.getState().shared.vocoderModelWeight == kWeightA);
    }

    // The vocoder is the slow one (800 ms; the editor applies after 200). The runtime has finished the first command.
    runtime.settle();
    runtime.queuedMs = 400;
    runtime.loadingMs = 400;
    bridge.applyDelayMs = 200;
    {
        const auto startedAt = juce::Time::getMillisecondCounter();
        const auto job = runAsJob(api, "prefs.set",
                                  json(R"({"editingScheme":"openTune","vocoderWeight":"hifigan.onnx"})"), "prefsApply");
        CONTROL_CHECK(jobSucceeded(job));
        CONTROL_CHECK(juce::Time::getMillisecondCounter() - startedAt >= 800);
        CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(preferences.getState().shared.audioEditingScheme));
        CONTROL_CHECK(preferences.getState().shared.vocoderModelWeight == kDefaultVocoderWeight);
    }
    settleVocoderScenario(processor, runtime);
    bridge.apply = PrefsBridge::Apply::Immediate;
    bridge.applyDelayMs = 0;
}

/** The editor never applies the scheme although the vocoder is back: INTERNAL after the scheme's limit, not the
    vocoder's error, and both the preferences and the runtime are put back. */
void checkSchemeTimesOutWhileVocoderIsBack(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge,
                                           FakeRuntime& runtime)
{
    const auto baseline = api.result("prefs.get");
    bridge.apply = PrefsBridge::Apply::Never;
    controlSetPrefsApplyTimeoutForTesting(kWaitingJobTimeoutMs);
    runtime.queuedMs = 100;
    runtime.loadingMs = 100;
    const auto startedAt = juce::Time::getMillisecondCounter();
    const auto job = runAsJob(api, "prefs.set",
                              json(R"({"editingScheme":"openDyne","vocoderWeight":"aa-custom.onnx"})"), "prefsApply");
    CONTROL_CHECK(jobFailedInternal(job, false));
    CONTROL_CHECK(juce::Time::getMillisecondCounter() - startedAt >= kWaitingJobTimeoutMs);
    CONTROL_CHECK(
        (runtime.calls == std::vector<std::string>{std::string("weight:") + kWeightA, "weight:hifigan.onnx"}));
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    settleVocoderScenario(processor, runtime);
    bridge.apply = PrefsBridge::Apply::Immediate;
}

/** The editor applies the scheme but the vocoder does not come back: the vocoder's error, and the scheme is taken back
    too. */
void checkVocoderTimesOutWhileSchemeIsApplied(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge,
                                              FakeRuntime& runtime)
{
    const auto baseline = api.result("prefs.get");
    bridge.apply = PrefsBridge::Apply::Immediate;
    controlSetPrefsVocoderWaitForTesting(600);
    runtime.neverReady = true;
    const auto notifications = bridge.preferenceNotifications;
    const auto job = runAsJob(api, "prefs.set",
                              json(R"({"editingScheme":"openDyne","vocoderWeight":"aa-custom.onnx"})"), "prefsApply");
    CONTROL_CHECK(jobFailedWith(job, "MODEL_UNAVAILABLE", "vocoder_not_ready", false, true));
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
    CONTROL_CHECK(bridge.preferenceNotifications == notifications + 2);
    CONTROL_CHECK(
        (runtime.calls == std::vector<std::string>{std::string("weight:") + kWeightA, "weight:hifigan.onnx"}));
    settleVocoderScenario(processor, runtime);
}

/** Another write while the job waits for the vocoder: the job takes back what still holds its value and leaves the
    weight the write changed, which the detail names; executed is true. */
void checkWriteDuringVocoderWait(ApiSession& api, OpenTuneAudioProcessor& processor, FakeRuntime& runtime)
{
    auto& preferences = processor.getAppPreferences();
    controlSetPrefsVocoderWaitForTesting(kWaitingJobTimeoutMs);
    runtime.neverReady = true;
    const auto jobId = jobIdOf(api.result("prefs.set", json(R"({"vocoderWeight":"aa-custom.onnx","tuningHz":433})")));
    CONTROL_CHECK(!jobId.empty());
    CONTROL_CHECK(pumpMessagesUntil([] { return TuningConfig::currentTuningHz() == 433.0f; }, kJobChangeMs));
    preferences.setVocoderModelWeight(kWeightZ);
    const auto job = waitJob(api, jobId);
    CONTROL_CHECK(jobFailedWith(job, "MODEL_UNAVAILABLE", "vocoder_not_ready", true, true));
    const auto detail = memberOf(memberOf(memberOf(job, "error"), "data"), "detail").text();
    CONTROL_CHECK(detail.find("vocoderWeight") != std::string::npos && detail.find("tuningHz") == std::string::npos);
    // The tuning is back; the weight keeps the value of the later write, and the runtime was not asked to take the
    // job's weight back over it.
    CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
    CONTROL_CHECK(preferences.getState().shared.vocoderModelWeight == kWeightZ);
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{std::string("weight:") + kWeightA}));
    settleVocoderScenario(processor, runtime);
}

/** A modal dialog opens before the job's first step: BUSY, nothing executed, the runtime was not asked anything. */
void checkModalBeforeVocoderChange(ApiSession& api, PrefsBridge& bridge, FakeRuntime& runtime,
                                   const std::shared_ptr<ControlJobs>& jobs)
{
    if (!CONTROL_CHECK(jobs != nullptr))
        return;
    CONTROL_CHECK(pumpMessagesUntil([&jobs] { return jobs->activeCount() == 0; }, kJobWaitMs));
    const auto baseline = api.result("prefs.get");
    const auto notifications = bridge.preferenceNotifications;
    bridge.modalOnceJobStarted = jobs;
    const auto job = runAsJob(
        api, "prefs.set", json(R"({"vocoderWeight":"aa-custom.onnx","renderingPriority":"cpuFirst"})"), "prefsApply");
    bridge.modalOnceJobStarted = nullptr;
    const auto& data = memberOf(memberOf(job, "error"), "data");
    CONTROL_CHECK(memberOf(job, "state").text() == "failed" && memberOf(data, "kind").text() == "BUSY"
                  && memberOf(data, "reason").text() == "modal_open" && memberOf(data, "executed").isBoolean()
                  && !memberOf(data, "executed").asBoolean());
    CONTROL_CHECK(runtime.calls.empty() && bridge.preferenceNotifications == notifications);
    CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
}

/** Polls session.info until processing reports this vocoderReady and vocoderReconfiguring, for at most timeoutMs. */
bool sessionVocoderBecomes(ApiSession& api, bool ready, bool reconfiguring, int timeoutMs)
{
    const auto startedAt = juce::Time::getMillisecondCounter();
    while (juce::Time::getMillisecondCounter() - startedAt < static_cast<std::uint32_t>(timeoutMs)) {
        const auto info = api.result("session.info");
        const auto& processing = memberOf(info, "processing");
        if (memberOf(processing, "vocoderReady").isBoolean()
            && memberOf(processing, "vocoderReady").asBoolean() == ready
            && memberOf(processing, "vocoderReconfiguring").isBoolean()
            && memberOf(processing, "vocoderReconfiguring").asBoolean() == reconfiguring)
            return true;
        pumpMessagesFor(10);
    }
    return false;
}

/** The job's failure when the editor generated the notes of the clip open in the piano roll: executed is true, the
   detail says that notes were generated and kept and names the content (a handle), the notes and the dirty flag stay,
   and everything else is back. */
bool notesKeptInFailure(const JsonValue& job, const std::string& contentId)
{
    const auto& data = memberOf(memberOf(job, "error"), "data");
    const auto detail = memberOf(data, "detail").text();
    const bool ok = memberOf(data, "executed").isBoolean() && memberOf(data, "executed").asBoolean()
                    && detail.find("generated notes") != std::string::npos && detail.find("kept") != std::string::npos
                    && detail.find(contentId) != std::string::npos && detail.find(".wav") == std::string::npos;
    if (!ok)
        note("the failed job does not say that generated notes were kept: " + toJsonString(job));
    return ok;
}

/** Switching to openDyne made the editor generate the notes of the clip open in the piano roll (no undo entry), and
    then the vocoder fails to come back: the job takes the preferences back, and the notes stay, the project stays
    marked as changed, the detail says so and executed is true. Once for the limit that ran out (retryable
    vocoder_not_ready), once for a weight that cannot be loaded (vocoder_load_failed); then the restored weight loads
    again, and session.info reports that while it does. */
void checkGeneratedNotesStay(ApiSession& api, OpenTuneAudioProcessor& processor, PrefsBridge& bridge,
                             FakeRuntime& runtime, const Clip& waitClip, const ClipHandles& waitHandles,
                             const Clip& failClip, const ClipHandles& failHandles)
{
    auto& preferences = processor.getAppPreferences();
    const auto baseline = api.result("prefs.get");
    const auto savedView = bridge.view;
    bridge.apply = PrefsBridge::Apply::Immediate;
    bridge.generateNotes = true;
    bridge.view.workspace = "pianoRoll";

    // The limit runs out: the vocoder never comes back.
    {
        bridge.pianoRollKey = waitClip.key;
        bridge.view.pianoRollPlacementId = waitClip.placementId;
        bridge.project.dirty = false;
        CONTROL_CHECK(!processor.getContentSnapshot(waitClip.key)->noteTopologyInitialized);
        controlSetPrefsVocoderWaitForTesting(600);
        runtime.neverReady = true;
        const auto job = runAsJob(
            api, "prefs.set", json(R"({"editingScheme":"openDyne","vocoderWeight":"aa-custom.onnx"})"), "prefsApply");
        CONTROL_CHECK(jobFailedWith(job, "MODEL_UNAVAILABLE", "vocoder_not_ready", true, true));
        CONTROL_CHECK(notesKeptInFailure(job, waitHandles.contentId));
        CONTROL_CHECK(processor.getContentSnapshot(waitClip.key)->noteTopologyInitialized
                      && !processor.getContentSnapshot(waitClip.key)->notes.empty());
        CONTROL_CHECK(bridge.project.dirty);
        CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(preferences.getState().shared.audioEditingScheme));
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
        CONTROL_CHECK(
            (runtime.calls == std::vector<std::string>{std::string("weight:") + kWeightA, "weight:hifigan.onnx"}));
        settleVocoderScenario(processor, runtime);
    }

    // The weight cannot be loaded: the job fails at once, with the same consequences for the notes. The previous
    // weight is handed back, and while it loads session.info reports a vocoder that is reconfiguring.
    {
        bridge.pianoRollKey = failClip.key;
        bridge.view.pianoRollPlacementId = failClip.placementId;
        bridge.project.dirty = false;
        CONTROL_CHECK(!processor.getContentSnapshot(failClip.key)->noteTopologyInitialized);
        controlSetPrefsVocoderWaitForTesting(8000);
        runtime.queuedMs = 100;
        runtime.loadingMs = 600;
        runtime.failNextLoad = true;
        const auto job = runAsJob(
            api, "prefs.set", json(R"({"editingScheme":"openDyne","vocoderWeight":"aa-custom.onnx"})"), "prefsApply");
        CONTROL_CHECK(jobFailedWith(job, "MODEL_UNAVAILABLE", "vocoder_load_failed", true, false));
        CONTROL_CHECK(notesKeptInFailure(job, failHandles.contentId));
        CONTROL_CHECK(processor.getContentSnapshot(failClip.key)->noteTopologyInitialized);
        CONTROL_CHECK(bridge.project.dirty);
        CONTROL_CHECK(!AudioEditingScheme::usesNotesPrimaryScheme(preferences.getState().shared.audioEditingScheme));
        CONTROL_CHECK(processingEqual(api.result("prefs.get"), baseline));
        CONTROL_CHECK(
            (runtime.calls == std::vector<std::string>{std::string("weight:") + kWeightA, "weight:hifigan.onnx"}));
        // The runtime was asked for the previous weight: it reloads (reconfiguring), and then the vocoder is back.
        CONTROL_CHECK(sessionVocoderBecomes(api, false, true, 2000));
        CONTROL_CHECK(sessionVocoderBecomes(api, true, false, 4000));
        settleVocoderScenario(processor, runtime);
    }
    bridge.view = savedView;
    bridge.generateNotes = false;
}

} // namespace

void scenarioPrefsVocoderOverlap()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    resetPreferences(*processor);
    PrefsBridge bridge;
    bridge.processor = processor.get();
    bridge.preferences = &processor->getAppPreferences();
    auto registered = std::make_unique<ScopedBridge>(service, bridge);
    fixtureAddClip(*processor, readyClip("vocoder-overlap"));
    // Two more clips, each to be opened in the piano roll once by checkGeneratedNotesStay (the notes of a clip are
    // generated only once).
    auto waitSpec = readyClip("vocoder-notes-wait");
    waitSpec.trackId = 1;
    auto failSpec = readyClip("vocoder-notes-fail");
    failSpec.trackId = 2;
    const auto waitClip = fixtureAddClip(*processor, waitSpec);
    const auto failClip = fixtureAddClip(*processor, failSpec);
    std::optional<ApiSession> session;
    session.emplace();
    auto& api = *session;
    if (!api.open(service))
        return;
    const auto models = vocoderModels("vocoder-overlap");
    if (!CONTROL_CHECK(!models.empty()))
        return;
    FakeRuntime runtime;
    runtime.install(models);
    checkSchemeAndVocoder(api, *processor, bridge, runtime);
    checkSchemeTimesOutWhileVocoderIsBack(api, *processor, bridge, runtime);
    checkVocoderTimesOutWhileSchemeIsApplied(api, *processor, bridge, runtime);
    checkWriteDuringVocoderWait(api, *processor, runtime);
    checkModalBeforeVocoderChange(api, bridge, runtime, service.jobs());
    {
        const auto project = api.result("project.get");
        const auto waitHandles = fixtureHandlesOf(project, 1, 2.0);
        const auto failHandles = fixtureHandlesOf(project, 2, 2.0);
        if (CONTROL_CHECK(waitHandles.has_value() && failHandles.has_value()))
            checkGeneratedNotesStay(api, *processor, bridge, runtime, waitClip, *waitHandles, failClip, *failHandles);
    }

    // The control API stops while a job waits for the vocoder: the stored preferences are put back, and the runtime is
    // not asked again (there is no context any more to ask with).
    bridge.apply = PrefsBridge::Apply::Immediate;
    runtime.neverReady = true;
    runtime.calls.clear();
    const auto job = api.result("prefs.set", json(R"({"vocoderWeight":"aa-custom.onnx","tuningHz":434})"));
    CONTROL_CHECK(!jobIdOf(job).empty());
    CONTROL_CHECK(pumpMessagesUntil([&] { return TuningConfig::currentTuningHz() == 434.0f; }, kJobChangeMs));
    CONTROL_CHECK(processor->getAppPreferences().getState().shared.vocoderModelWeight == kWeightA);
    session.reset();
    registered.reset();
    processor.reset();
    CONTROL_CHECK(TuningConfig::currentTuningHz() == 440.0f);
    CONTROL_CHECK((runtime.calls == std::vector<std::string>{std::string("weight:") + kWeightA}));
    AppPreferences stored;
    stored.load();
    CONTROL_CHECK(stored.getState().shared.vocoderModelWeight == kDefaultVocoderWeight);
    CONTROL_CHECK(stored.getState().shared.tuning.tuningHz == 440.0f);
}

// ---- audio.get and audio.set
// ----------------------------------------------------------------------------

namespace {

const JsonValue& deviceOf(const JsonValue& result)
{
    return memberOf(result, "device");
}

/** The setup of the fake manager as a short text, to compare before and after a
 * failed call. */
std::string setupText(juce::AudioDeviceManager& manager)
{
    const auto setup = manager.getAudioDeviceSetup();
    auto* device = manager.getCurrentAudioDevice();
    return (manager.getCurrentAudioDeviceType() + "|" + setup.outputDeviceName + "|"
            + juce::String(device != nullptr ? device->getCurrentSampleRate() : 0.0) + "|"
            + juce::String(device != nullptr ? device->getCurrentBufferSizeSamples() : 0))
        .toStdString();
}

bool deviceIs(const JsonValue& device, const std::string& type, const std::string& output, double rate, double buffer)
{
    return memberOf(device, "running").asBoolean() && memberOf(device, "deviceType").text() == type
           && memberOf(device, "outputDevice").text() == output && numberIs(memberOf(device, "sampleRateHz"), rate)
           && numberIs(memberOf(device, "bufferSizeFrame"), buffer);
}

std::vector<std::string> textsOf(const JsonValue& array)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < array.size(); ++i)
        out.push_back(array.at(i).text());
    return out;
}

bool failsWith(ApiSession& api, const char* params, const char* kind, const char* reason)
{
    return api.fails("audio.set", json(params), kind, reason);
}

/** A failed call with executed true (the previous setup could not be restored).
 */
bool failsAfterChange(ApiSession& api, const char* params, const char* kind, const char* reason)
{
    const auto response = api.request("audio.set", json(params));
    const JsonValue none;
    const JsonValue* error = response.find("error");
    const auto& data = memberOf(error != nullptr ? *error : none, "data");
    const bool ok = memberOf(data, "kind").text() == kind && memberOf(data, "reason").text() == reason
                    && memberOf(data, "executed").isBoolean() && memberOf(data, "executed").asBoolean();
    if (!ok)
        note(std::string("audio.set: expected ") + kind + "(" + reason + ") with executed true, got "
             + toJsonString(response));
    return ok;
}

/** Without a device manager: audio.get says why, audio.set says UNSUPPORTED. */
void checkWithoutDevice(ApiSession& api, PrefsBridge& bridge, ControlService& service)
{
    const auto readNullReason = [&](const char* expected) {
        const auto result = api.result("audio.get");
        const auto& device = deviceOf(result);
        CONTROL_CHECK(!memberOf(device, "running").asBoolean() && memberOf(device, "deviceType").isNull()
                      && memberOf(device, "sampleRateHz").isNull());
        CONTROL_CHECK(memberOf(memberOf(device, "nullReasons"), "outputDevice").text() == expected);
        CONTROL_CHECK(memberOf(memberOf(result, "available"), "deviceTypes").size() == 0);
        CONTROL_CHECK(numberIs(memberOf(memberOf(result, "available"), "outputChannelPairCount"), 0.0));
    };
    // The editor's bridge has no device manager.
    readNullReason("unavailable");
    CONTROL_CHECK(api.fails("audio.set", json(R"({"bufferSizeFrame":256})"), "UNSUPPORTED", "capability_missing"));
    // No editor at all.
    service.setUiBridge(nullptr);
    readNullReason("no_editor");
    CONTROL_CHECK(api.fails("audio.set", json(R"({"bufferSizeFrame":256})"), "UNSUPPORTED", "no_editor_window"));
    service.setUiBridge(&bridge);
}

/** What the page offers, the closed params, the values that are not on offer
 * and the changes of the running device. */
void checkRunningDevice(ApiSession& api, FakeAudioRig& rig, PrefsBridge& bridge)
{
    const auto before = api.result("audio.get");
    CONTROL_CHECK(deviceIs(deviceOf(before), "Fake", "Fake Output A", 44100.0, 512.0));
    CONTROL_CHECK(numberIs(memberOf(deviceOf(before), "outputChannelPair"), 0.0));
    CONTROL_CHECK(memberOf(deviceOf(before), "nullReasons").isNull());
    const auto& available = memberOf(before, "available");
    CONTROL_CHECK((textsOf(memberOf(available, "deviceTypes")) == std::vector<std::string>{"Fake", "Fake B"}));
    CONTROL_CHECK(
        (textsOf(memberOf(available, "outputDevices")) == std::vector<std::string>{"Fake Output A", "Fake Output B"}));
    CONTROL_CHECK(memberOf(available, "sampleRatesHz").size() == 2
                  && memberOf(available, "bufferSizesFrame").size() == 3);
    CONTROL_CHECK(numberIs(memberOf(available, "outputChannelPairCount"), 1.0));
    CONTROL_CHECK(rig.behaviour.scans >= 1);
    const auto setupBefore = setupText(rig.manager);

    // The closed params.
    for (const char* params : {"{}", R"({"sampleRateHz":0})", R"({"bufferSizeFrame":0})", R"({"bufferSizeFrame":2.5})",
                               R"({"outputChannelPair":-1})", R"({"deviceType":""})", R"({"outputDevice":5})",
                               R"({"bufferSizeFrame":256,"latency":3})"})
        CONTROL_CHECK(failsWith(api, params, "INVALID_ARGUMENT", "invalid_params"));

    // Values that are not on offer change nothing and do not even reopen the
    // device.
    for (const char* params : {R"({"sampleRateHz":12345})", R"({"bufferSizeFrame":999})", R"({"outputChannelPair":1})",
                               R"({"outputDevice":"No Such Output"})", R"({"deviceType":"No Such Type"})",
                               R"({"deviceType":"Fake B","outputDevice":"Fake Output A"})",
                               R"({"outputDevice":"Fake Output B","sampleRateHz":12345})"})
        CONTROL_CHECK(failsWith(api, params, "INVALID_ARGUMENT", "audio_setting_unavailable"));
    CONTROL_CHECK(setupText(rig.manager) == setupBefore);

    // A setup equal to the current one is a no-op.
    {
        const auto opens = rig.behaviour.opens;
        const auto result = api.result(
            "audio.set",
            json(
                R"({"bufferSizeFrame":512,"sampleRateHz":44100,"outputChannelPair":0,"deviceType":"Fake","outputDevice":"Fake Output A"})"));
        CONTROL_CHECK(memberOf(result, "noOp").asBoolean() && memberOf(result, "changes").size() == 0
                      && memberOf(result, "warnings").size() == 0);
        CONTROL_CHECK(memberOf(memberOf(result, "undo"), "reason").text() == "no_op");
        CONTROL_CHECK(deviceIs(deviceOf(result), "Fake", "Fake Output A", 44100.0, 512.0));
        CONTROL_CHECK(rig.behaviour.opens == opens);
    }

    // The buffer size: the device opens again with it; the revert recipe takes it
    // back.
    {
        const auto result = api.result("audio.set", json(R"({"bufferSizeFrame":256})"));
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean());
        CONTROL_CHECK(memberOf(result, "changes").size() == 1);
        const auto& change = changeAt(result, 0);
        CONTROL_CHECK(memberOf(change, "entity").text() == "audioDevice"
                      && memberOf(memberOf(change, "ref"), "singleton").text() == "audioDevice");
        CONTROL_CHECK(memberOf(change, "fields").size() == 1);
        CONTROL_CHECK(numberIs(memberOf(fieldOf(change, "bufferSizeFrame"), "before"), 512.0)
                      && numberIs(memberOf(fieldOf(change, "bufferSizeFrame"), "after"), 256.0)
                      && memberOf(fieldOf(change, "bufferSizeFrame"), "unit").text() == "frame");
        CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"GLOBAL_PREFERENCE"}));
        CONTROL_CHECK((globalPreferences(result) == std::vector<std::string>{"audioDevice"}));
        CONTROL_CHECK(undoIsUiParity(result));
        CONTROL_CHECK(deviceIs(deviceOf(result), "Fake", "Fake Output A", 44100.0, 256.0));
        CONTROL_CHECK(deviceIs(deviceOf(api.result("audio.get")), "Fake", "Fake Output A", 44100.0, 256.0));
        CONTROL_CHECK(rig.manager.getCurrentAudioDevice()->getCurrentBufferSizeSamples() == 256);
        const auto& params = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
        CONTROL_CHECK(params.size() == 1 && numberIs(memberOf(params, "bufferSizeFrame"), 512.0));
        const auto reverted = replayRevert(api, result);
        CONTROL_CHECK(deviceIs(deviceOf(reverted), "Fake", "Fake Output A", 44100.0, 512.0));
        CONTROL_CHECK(setupText(rig.manager) == setupBefore);
    }

    // The sample rate and the buffer size together open the device once.
    {
        const auto opensBefore = rig.behaviour.opens;
        const auto result = api.result("audio.set", json(R"({"sampleRateHz":48000,"bufferSizeFrame":128})"));
        CONTROL_CHECK(memberOf(result, "changes").size() == 1 && memberOf(changeAt(result, 0), "fields").size() == 2);
        CONTROL_CHECK(deviceIs(deviceOf(result), "Fake", "Fake Output A", 48000.0, 128.0));
        CONTROL_CHECK(rig.behaviour.opens == opensBefore + 1);
        replayRevert(api, result);
        CONTROL_CHECK(setupText(rig.manager) == setupBefore);
    }

    // Another output device: it opens with the device's default channels.
    {
        const auto result = api.result("audio.set", json(R"({"outputDevice":"Fake Output B"})"));
        CONTROL_CHECK(memberOf(result, "changes").size() == 1);
        CONTROL_CHECK(memberOf(fieldOf(changeAt(result, 0), "outputDevice"), "after").text() == "Fake Output B");
        CONTROL_CHECK(deviceIs(deviceOf(result), "Fake", "Fake Output B", 44100.0, 512.0));
        replayRevert(api, result);
        CONTROL_CHECK(setupText(rig.manager) == setupBefore);
    }

    // Another device type: its default device opens; the revert recipe names the
    // type and the device.
    {
        const auto result = api.result("audio.set", json(R"({"deviceType":"Fake B"})"));
        CONTROL_CHECK(memberOf(result, "changes").size() == 1);
        CONTROL_CHECK(deviceIs(deviceOf(result), "Fake B", "Fake B Output", 44100.0, 512.0));
        const auto& params = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
        CONTROL_CHECK(memberOf(params, "deviceType").text() == "Fake"
                      && memberOf(params, "outputDevice").text() == "Fake Output A");
        replayRevert(api, result);
        CONTROL_CHECK(setupText(rig.manager) == setupBefore);
    }

    // Busy: a dialog is open.
    {
        bridge.busy.modalOpen = true;
        CONTROL_CHECK(api.fails("audio.set", json(R"({"bufferSizeFrame":256})"), "BUSY", "modal_open"));
        bridge.busy.modalOpen = false;
        CONTROL_CHECK(setupText(rig.manager) == setupBefore);
    }
}

/** The device cannot be opened with the new setup: the previous setup is
 * restored, as the settings page does. */
void checkRollback(ApiSession& api, FakeAudioRig& rig)
{
    const auto setupBefore = setupText(rig.manager);

    // The new sample rate does not open: restored, executed false, the device
    // runs as before.
    rig.behaviour.failRateHz = 48000.0;
    CONTROL_CHECK(failsWith(api, R"({"sampleRateHz":48000})", "IO_ERROR", "device_open_failed"));
    CONTROL_CHECK(setupText(rig.manager) == setupBefore);
    CONTROL_CHECK(deviceIs(deviceOf(api.result("audio.get")), "Fake", "Fake Output A", 44100.0, 512.0));

    // The first step succeeded and the second does not: the whole call is taken
    // back, also the output device.
    CONTROL_CHECK(failsWith(api, R"({"outputDevice":"Fake Output B","sampleRateHz":48000,"bufferSizeFrame":256})",
                            "IO_ERROR", "device_open_failed"));
    CONTROL_CHECK(setupText(rig.manager) == setupBefore);

    // A device that does not open after a switch of the type: the type is
    // switched back.
    rig.behaviour.failRateHz = 0.0;
    rig.behaviour.failDeviceName = "Fake B Output";
    CONTROL_CHECK(failsWith(api, R"({"deviceType":"Fake B"})", "IO_ERROR", "device_open_failed"));
    CONTROL_CHECK(rig.manager.getCurrentAudioDeviceType() == "Fake" && setupText(rig.manager) == setupBefore);
    rig.behaviour.failDeviceName = {};

    // The previous setup does not open either: executed true, no device runs, and
    // audio.get says so.
    rig.behaviour.failAllOpens = true;
    CONTROL_CHECK(failsAfterChange(api, R"({"bufferSizeFrame":256})", "IO_ERROR", "device_open_failed"));
    rig.behaviour.failAllOpens = false;
    CONTROL_CHECK(rig.manager.getCurrentAudioDevice() == nullptr);
    const auto none = api.result("audio.get");
    CONTROL_CHECK(!memberOf(deviceOf(none), "running").asBoolean() && memberOf(deviceOf(none), "outputDevice").isNull()
                  && memberOf(memberOf(deviceOf(none), "nullReasons"), "bufferSizeFrame").text() == "no_audio_device");
    CONTROL_CHECK(memberOf(memberOf(none, "available"), "sampleRatesHz").size() == 0
                  && memberOf(memberOf(none, "available"), "outputDevices").size() == 2);

    // With no device nothing of the device is on offer, and no device is no state
    // that a request can restore; opening one is a change that cannot be
    // reverted.
    CONTROL_CHECK(failsWith(api, R"({"sampleRateHz":44100})", "INVALID_ARGUMENT", "audio_setting_unavailable"));
    {
        const auto result = api.result("audio.set", json(R"({"outputDevice":"Fake Output B"})"));
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && memberOf(result, "changes").size() == 1);
        CONTROL_CHECK(memberOf(fieldOf(changeAt(result, 0), "outputDevice"), "before").isNull());
        CONTROL_CHECK(memberOf(memberOf(result, "undo"), "revert").isNull() && hasWarning(result, "IRREVERSIBLE"));
        CONTROL_CHECK(memberOf(deviceOf(result), "running").asBoolean()
                      && memberOf(deviceOf(result), "outputDevice").text() == "Fake Output B");
    }
}

/** A failure with no device before the call leaves no device. */
void checkFailureWithoutDevice(ApiSession& api, FakeAudioRig& rig)
{
    CONTROL_CHECK(rig.manager.getCurrentAudioDevice() == nullptr);
    rig.behaviour.failDeviceName = "Fake Output B";
    CONTROL_CHECK(failsWith(api, R"({"outputDevice":"Fake Output B"})", "IO_ERROR", "device_open_failed"));
    CONTROL_CHECK(rig.manager.getCurrentAudioDevice() == nullptr);
    rig.behaviour.failDeviceName = {};
    CONTROL_CHECK(!memberOf(deviceOf(api.result("audio.get")), "running").asBoolean());
}

/** The detail of a failed audio.set, or empty. */
std::string errorDetail(const JsonValue& response)
{
    const JsonValue* data = errorData(response);
    const JsonValue* detail = data != nullptr ? data->find("detail") : nullptr;
    return detail != nullptr ? detail->text() : std::string();
}

/** A value that is not on offer once an earlier step of the call changed the
   device: the previous setup is put back. When that fails too, the call fails
   with IO_ERROR(device_open_failed) and executed true, and the detail names the
   value that is not on offer, not a device that could not be opened. */
void checkUnavailableAfterChange(ApiSession& api, FakeAudioRig& rig)
{
    const auto setupBefore = setupText(rig.manager);
    const char* params = R"({"outputDevice":"Fake Output B","sampleRateHz":12345})";
    const auto contains = [](const std::string& text, const char* part) {
        return text.find(part) != std::string::npos;
    };

    // Restored: nothing changed, and the detail says so.
    {
        const auto response = api.request("audio.set", json(params));
        const JsonValue* data = errorData(response);
        CONTROL_CHECK(data != nullptr && memberOf(*data, "kind").text() == "INVALID_ARGUMENT"
                      && memberOf(*data, "reason").text() == "audio_setting_unavailable"
                      && !memberOf(*data, "executed").asBoolean());
        const auto detail = errorDetail(response);
        CONTROL_CHECK(contains(detail, "sample rate is not offered")
                      && contains(detail, "the previous setup was restored")
                      && !contains(detail, "could not be opened"));
        CONTROL_CHECK(setupText(rig.manager) == setupBefore);
    }

    // The previous device does not open again: executed true, no device runs.
    rig.behaviour.failDeviceName = "Fake Output A";
    {
        const auto response = api.request("audio.set", json(params));
        const JsonValue* data = errorData(response);
        CONTROL_CHECK(data != nullptr && memberOf(*data, "kind").text() == "IO_ERROR"
                      && memberOf(*data, "reason").text() == "device_open_failed"
                      && memberOf(*data, "executed").isBoolean() && memberOf(*data, "executed").asBoolean());
        const auto detail = errorDetail(response);
        CONTROL_CHECK(contains(detail, "sample rate is not offered")
                      && contains(detail, "the previous setup could not be restored")
                      && !contains(detail, "could not be opened"));
        CONTROL_CHECK(rig.manager.getCurrentAudioDevice() == nullptr);
    }
    rig.behaviour.failDeviceName = {};
}

/** The warning ASIO_RATE_NOT_RESTORED of registry/warnings.json: it comes after GLOBAL_PREFERENCE, its subject is the
    singleton audioDevice and its data names the ASIO device whose rate was to be restored and the rate in Hz that the
    restore tried to set. */
bool asioWarningIs(const JsonValue& result, const char* deviceName, double sampleRateHz)
{
    const JsonValue* warning = findWarning(result, "ASIO_RATE_NOT_RESTORED");
    const auto& warnings = memberOf(result, "warnings");
    bool ok = warning != nullptr && warnings.size() == 2
              && memberOf(warnings.at(0), "code").text() == "GLOBAL_PREFERENCE" && warning == &warnings.at(1)
              && toJsonString(memberOf(*warning, "subject")) == R"({"singleton":"audioDevice"})";
    if (ok) {
        const auto& data = memberOf(*warning, "data");
        ok = data.isObject() && data.size() == 2 && memberOf(data, "deviceName").text() == deviceName
             && numberIs(memberOf(data, "sampleRateHz"), sampleRateHz);
    }
    if (!ok)
        note("ASIO_RATE_NOT_RESTORED is not as expected: " + toJsonString(warnings));
    return ok;
}

/** The ASIO transitions: the editor restores the system sample rate first, as best effort: a restore that fails does
   not stop the switch (the page continues and shows kAudioDeviceRestoreWarning); the result carries the warning
   ASIO_RATE_NOT_RESTORED, with the singleton audioDevice as its subject and the device and the rate in its data. */
void checkAsioSwitch(ApiSession& api, PrefsBridge& bridge)
{
    // Other changes of an ASIO device do not close it for another one: no restore.
    bridge.asioRestoreSucceeds = true;
    const auto buffer = api.result("audio.set", json(R"({"bufferSizeFrame":256})"));
    CONTROL_CHECK(memberOf(buffer, "applied").asBoolean() && bridge.switchReasons.empty());
    CONTROL_CHECK(!hasWarning(buffer, "ASIO_RATE_NOT_RESTORED"));
    replayRevert(api, buffer);

    // A restore that fails does not stop the switch: the page shows its warning and goes on. The result carries the
    // warning ASIO_RATE_NOT_RESTORED, and the device is the new one.
    bridge.asioRestoreSucceeds = false;
    const auto output = api.result("audio.set", json(R"({"outputDevice":"Fake Output B"})"));
    CONTROL_CHECK(memberOf(output, "applied").asBoolean() && hasWarning(output, "ASIO_RATE_NOT_RESTORED")
                  && hasWarning(output, "GLOBAL_PREFERENCE"));
    // The device that was left (the one the restore acted on) and the rate it tried to set, not the rate of the device.
    CONTROL_CHECK(asioWarningIs(output, "Fake Output A", 48000.0));
    CONTROL_CHECK(deviceIs(deviceOf(output), "ASIO", "Fake Output B", 44100.0, 512.0));
    CONTROL_CHECK((bridge.switchReasons == std::vector<std::string>{"audio output switch"}));
    // The revert recipe sets the previous output device back; this time the restore succeeds and there is no warning.
    bridge.asioRestoreSucceeds = true;
    bridge.switchReasons.clear();
    const auto back = replayRevert(api, output);
    CONTROL_CHECK(memberOf(back, "applied").asBoolean() && !hasWarning(back, "ASIO_RATE_NOT_RESTORED"));
    CONTROL_CHECK((bridge.switchReasons == std::vector<std::string>{"audio output switch"}));
    CONTROL_CHECK(deviceIs(deviceOf(back), "ASIO", "Fake Output A", 44100.0, 512.0));

    // The type switch with a restore that succeeds: no warning.
    bridge.switchReasons.clear();
    const auto result = api.result("audio.set", json(R"({"deviceType":"Fake B"})"));
    CONTROL_CHECK(memberOf(result, "applied").asBoolean() && bridge.switchReasons.size() == 1);
    CONTROL_CHECK(!hasWarning(result, "ASIO_RATE_NOT_RESTORED"));
    CONTROL_CHECK(deviceIs(deviceOf(result), "Fake B", "Fake B Output", 44100.0, 512.0));
    // Leaving a device type other than ASIO restores nothing.
    bridge.switchReasons.clear();
    const auto toAsio = replayRevert(api, result);
    CONTROL_CHECK(memberOf(toAsio, "applied").asBoolean() && bridge.switchReasons.empty());
    CONTROL_CHECK(deviceIs(deviceOf(toAsio), "ASIO", "Fake Output A", 44100.0, 512.0));

    // The type switch with a restore that fails: the switch goes on, with the warning.
    bridge.asioRestoreSucceeds = false;
    const auto type = api.result("audio.set", json(R"({"deviceType":"Fake B"})"));
    CONTROL_CHECK(memberOf(type, "applied").asBoolean() && hasWarning(type, "ASIO_RATE_NOT_RESTORED"));
    CONTROL_CHECK(asioWarningIs(type, "Fake Output A", 48000.0));
    CONTROL_CHECK((bridge.switchReasons == std::vector<std::string>{"audio type switch"}));
    CONTROL_CHECK(deviceIs(deviceOf(type), "Fake B", "Fake B Output", 44100.0, 512.0));
    bridge.asioRestoreSucceeds = true;
}

} // namespace

void scenarioAudioDevice()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    ApiSession api;
    if (!api.open(service))
        return;
    PrefsBridge bridge;
    bridge.processor = processor.get();
    bridge.preferences = &processor->getAppPreferences();
    ScopedBridge registered(service, bridge);
    {
        // The bridge of the editor knows no device manager; then no editor at all.
        checkWithoutDevice(api, bridge, service);
    }
    {
        FakeAudioRig rig;
        bridge.manager = &rig.manager;
        checkRunningDevice(api, rig, bridge);
        checkRollback(api, rig);
        bridge.manager = nullptr;
    }
    {
        FakeAudioRig rig(false);
        bridge.manager = &rig.manager;
        checkFailureWithoutDevice(api, rig);
        bridge.manager = nullptr;
    }
    {
        FakeAudioRig rig;
        bridge.manager = &rig.manager;
        checkUnavailableAfterChange(api, rig);
        bridge.manager = nullptr;
    }
    {
        FakeAudioRig rig(true, "ASIO");
        bridge.manager = &rig.manager;
        checkAsioSwitch(api, bridge);
        bridge.manager = nullptr;
    }
    const auto dispatcher = service.dispatcher();
    CONTROL_CHECK(dispatcher != nullptr && dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

// ---- The specification's vectors
// ---------------------------------------------------------------------------

namespace {

const std::set<std::string> kPrefsVectorMethods = {"prefs.get", "prefs.set", "session.info", "job.get",
                                                   "ui.setSelection"};
const std::set<std::string> kAudioVectorMethods = {"audio.get", "audio.set"};
/** The vectors of prefs.set, and the vectors of prefs.get that need preferences established (read.v1; the other
    vectors of prefs.get run in the Vectors scenario of ControlDispatcherTests.cpp). */
const std::set<std::string> kPrefsVectorCapabilities = {"prefs.write", "read.v1"};
const std::set<std::string> kAudioVectorCapabilities = {"audio.device"};

/** Why this runner does not run the preference given by a vector (a member of given.preferences), or empty when it
    establishes it. */
std::string preferencesSkipReason(const JsonValue& preferences)
{
    for (std::size_t i = 0; i < preferences.size(); ++i) {
        const auto& member = preferences.keyAt(i);
        if (member == "options") {
            const auto& options = preferences.at(i);
            for (std::size_t o = 0; o < options.size(); ++o) {
                const auto& list = options.keyAt(o);
                if (list == "f0Model") {
                    for (std::size_t n = 0; n < options.at(o).size(); ++n)
                        if (options.at(o).at(n).text() != "fcpe")
                            return "the pitch model " + options.at(o).at(n).text()
                                   + " is not established by this runner (OpenTune has fcpe only)";
                } else if (list != "vocoderWeight") {
                    return "precondition given.preferences.options." + list + " is not established by this runner";
                }
            }
            continue;
        }
        if (member == "f0Model") {
            if (preferences.at(i).text() != "fcpe")
                return "the pitch model " + preferences.at(i).text()
                       + " is not established by this runner (OpenTune has fcpe only)";
            continue;
        }
        if (member == "tuningHz" || member == "hybridMode" || member == "experimentalFeatures"
            || member == "vocoderWeight" || member == "renderingPriority")
            continue;
        return "precondition given.preferences." + member + " is not established by this runner";
    }
    return {};
}

/** Why this runner does not run a vector, or empty when it does. */
std::string vectorSkipReason(const JsonValue& vector, const std::set<std::string>& capabilities,
                             const std::set<std::string>& methods)
{
    if (capabilities.count(memberOf(vector, "capability").text()) == 0)
        return "runs in the runner of its own capability";
    if (memberOf(vector, "capability").text() == "read.v1"
        && !memberOf(memberOf(vector, "given"), "preferences").isObject())
        return "a read without preferences of its own runs in ControlProcessorTests.Vectors";
    const auto& extra = memberOf(vector, "extraCapabilities");
    for (std::size_t i = 0; i < extra.size(); ++i)
        if (extra.at(i).text() != "jobs.v1" && extra.at(i).text() != "ui.bridge")
            return "needs capability " + extra.at(i).text() + ", which this runner does not cover";
    if (memberOf(vector, "session").text() != "authenticated")
        return "unauthenticated vectors test the listener (ControlUnitTests)";
    std::vector<std::string> used;
    if (const JsonValue* request = vector.find("request"))
        used.push_back(memberOf(*request, "method").text());
    if (const JsonValue* steps = vector.find("steps"))
        for (std::size_t i = 0; i < steps->size(); ++i) {
            const auto& step = steps->at(i);
            if ((step.find("send") == nullptr && step.find("expect") == nullptr) || step.find("connection") != nullptr)
                return "uses steps other than send and expect on the main connection";
            if (const JsonValue* send = step.find("send"))
                used.push_back(memberOf(*send, "method").text());
        }
    for (const auto& method : used)
        if (methods.count(method) == 0)
            return "uses " + method + ", which this runner does not cover";
    const auto& given = memberOf(vector, "given");
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& name = given.keyAt(i);
        if (name == "preferences") {
            if (const auto reason = preferencesSkipReason(given.at(i)); !reason.empty())
                return reason;
            continue;
        }
        // given.gpu (available or unavailable) is the fake runtime's gpuAvailable; given.audio, with
        // asioRateRestoreFails, is a rig whose device type is ASIO and a bridge whose restore of the ASIO sample rate
        // fails.
        if (name == "dirty" || name == "editingScheme" || name == "audio")
            continue;
        if (name == "gpu") {
            if (given.at(i).text() != "available" && given.at(i).text() != "unavailable")
                return "precondition given.gpu " + given.at(i).text() + " is not established by this runner";
            continue;
        }
        if (name == "jobs") {
            for (std::size_t j = 0; j < given.at(i).size(); ++j)
                if (memberOf(given.at(i).at(j), "state").text() != "failed")
                    return "a job that has not failed is not established by this runner";
            continue;
        }
        if (name != "clips")
            return "precondition given." + name + " is not established by this runner";
        for (std::size_t c = 0; c < given.at(i).size(); ++c)
            if (memberOf(given.at(i).at(c), "analysis").text() != "ready")
                return "a clip with analysis " + memberOf(given.at(i).at(c), "analysis").text()
                       + " is not established by this runner";
    }
    return {};
}

/**
 * Runs one vector against a fresh processor, a test bridge with a fake device
 * manager and the preferences of the vector's preconditions. ui.setSelection
 * (whose scenarios are in ControlSelectionTests.cpp) is not sent: its effect in
 * the vectors of prefs.set, the clip being open in the piano roll, is made on
 * the test bridge and its response is not expected. Empty on success.
 */
std::string runVector(const JsonValue& vector, bool withClips)
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return "the control API did not start";
    const auto& given = memberOf(vector, "given");
    const auto& preferencesGiven = memberOf(given, "preferences");
    const auto booleanGiven = [&preferencesGiven](const char* name, bool otherwise) {
        const JsonValue* value = preferencesGiven.find(name);
        return value != nullptr ? value->asBoolean() : otherwise;
    };
    resetPreferences(*processor, memberOf(given, "editingScheme").text() == "openDyne",
                     booleanGiven("hybridMode", true), booleanGiven("experimentalFeatures", false),
                     preferencesGiven.find("tuningHz") != nullptr
                         ? static_cast<float>(memberOf(preferencesGiven, "tuningHz").asDouble())
                         : TuningConfig::kDefaultTuningHz);
    // The processing preferences of the vector (the pitch model is fcpe, the only one OpenTune has, as the defaults).
    auto& preferences = processor->getAppPreferences();
    if (const JsonValue* weight = preferencesGiven.find("vocoderWeight"))
        preferences.setVocoderModelWeight(weight->text());
    if (const JsonValue* priority = preferencesGiven.find("renderingPriority"))
        preferences.setRenderingPriority(priority->text() == "cpuFirst" ? RenderingPriority::CpuFirst
                                                                        : RenderingPriority::GpuFirst);
    // The models of the installation (given.preferences.options) are files in a models directory of the test, and the
    // render runtime is a test double that reloads the vocoder after a short while, so that no real model is loaded and
    // no real inference backend is reset. Without options the directory is empty, as on an installation without models.
    const auto models = freshModelsDirectory("vector");
    if (models.empty())
        return "the models directory of the vector could not be made";
    if (const JsonValue* options = preferencesGiven.find("options")) {
        const auto& weights = memberOf(*options, "vocoderWeight");
        for (std::size_t i = 0; i < weights.size(); ++i) {
            const auto name = weights.at(i).text();
            const bool builtIn = name == kVocoderWeightCommunity || name == kVocoderWeightCoulin9;
            touchFile(builtIn ? models / std::filesystem::u8path(name)
                              : models / "vocoder_weights" / std::filesystem::u8path(name));
        }
        // The only pitch model that can be listed is fcpe (vectorSkipReason skips a vector that names another).
        if (memberOf(*options, "f0Model").size() != 0)
            touchFile(models / "fcpe.onnx");
    }
    FakeRuntime runtime;
    runtime.queuedMs = 100;
    runtime.loadingMs = 100;
    // given.gpu: whether OpenTune can create its inference session on the GPU. Without the member the vector assumes
    // nothing about the GPU, and the runtime's default (it can) stands. The backend at the start is the one that the
    // rendering priority of the vector gives.
    if (const JsonValue* gpu = given.find("gpu"))
        runtime.gpuAvailable = gpu->text() == "available";
    runtime.startWith(preferences.getState().shared.renderingPriority == RenderingPriority::CpuFirst);
    runtime.install(models);
    const auto& audioGiven = memberOf(given, "audio");
    const bool audioRunning = audioGiven.find("running") == nullptr || memberOf(audioGiven, "running").asBoolean();
    // given.audio.asioRateRestoreFails: the device type that runs is ASIO (the editor restores the sample rate of an
    // ASIO device only), and the restore fails.
    const bool asioRestoreFails =
        audioGiven.find("asioRateRestoreFails") != nullptr && memberOf(audioGiven, "asioRateRestoreFails").asBoolean();
    FakeAudioRig rig(audioRunning, asioRestoreFails ? "ASIO" : "Fake");
    if (audioGiven.find("failsToOpen") != nullptr && memberOf(audioGiven, "failsToOpen").asBoolean())
        rig.behaviour.failRateHz = 48000.0;
    PrefsBridge bridge;
    bridge.processor = processor.get();
    bridge.preferences = &processor->getAppPreferences();
    bridge.manager = &rig.manager;
    bridge.asioRestoreSucceeds = !asioRestoreFails;
    bridge.generateNotes = true;
    bridge.project.dirty = memberOf(given, "dirty").isBoolean() && memberOf(given, "dirty").asBoolean();
    ScopedBridge registered(service, bridge);

    VectorMatcher matcher;
    if (const JsonValue* alternatives = audioGiven.find("alternatives"); alternatives && alternatives->asBoolean()) {
        // given.audio.alternatives: the current device type and output device, and another of each. The rig always
        // offers a second device type with an output device and a second output device on the current type.
        const auto names = rig.alternatives();
        if (!names.valid)
            return "the fake audio devices offer no alternatives";
        matcher.bindings["audio.deviceType"] = JsonValue::string(names.deviceType);
        matcher.bindings["audio.outputDevice"] = JsonValue::string(names.outputDevice);
        matcher.bindings["audio.otherDeviceType"] = JsonValue::string(names.otherDeviceType);
        matcher.bindings["audio.otherOutputDevice"] = JsonValue::string(names.otherOutputDevice);
    }
    std::vector<Clip> created;
    const auto& clips = memberOf(given, "clips");
    std::vector<std::string> binds;
    for (std::size_t i = 0; withClips && i < clips.size(); ++i) {
        const auto& spec = clips.at(i);
        ClipSpec clipSpec = readyClip(("vector-" + memberOf(spec, "bind").text()).c_str());
        clipSpec.trackId = static_cast<int>(memberOf(spec, "trackId").asDouble());
        clipSpec.startTimelineSec = memberOf(spec, "startTimelineSec").asDouble();
        clipSpec.durationSec = memberOf(spec, "durationSec").asDouble();
        created.push_back(fixtureAddClip(*processor, clipSpec));
        if (!created.back().key.isValid())
            return "the clip could not be added";
        binds.push_back(memberOf(spec, "bind").text());
    }

    ApiSession api;
    if (!api.open(service))
        return "session.hello failed";
    if (const JsonValue* jobsGiven = given.find("jobs")) {
        // Failed jobs of the vector (given.jobs[].error), made on the message thread like the jobs of a call.
        if (service.jobs() == nullptr || !establishFailedJobs(*service.jobs(), *jobsGiven, matcher))
            return "given.jobs could not be established";
    }
    if (!created.empty()) {
        const auto project = api.result("project.get");
        for (std::size_t i = 0; i < created.size(); ++i) {
            const auto handles =
                fixtureHandlesOf(project, static_cast<int>(memberOf(clips.at(i), "trackId").asDouble()),
                                 memberOf(clips.at(i), "startTimelineSec").asDouble());
            if (!handles)
                return "project.get does not list the clip " + binds[i];
            matcher.bindings[binds[i] + ".contentId"] = JsonValue::string(handles->contentId);
            matcher.bindings[binds[i] + ".placementId"] = JsonValue::string(handles->placementId);
        }
    }

    std::string failure;
    runWhilePumping(
        [&] {
            auto& connection = api.client();
            const auto sendAndExpect = [&](const JsonValue& send, const JsonValue* expect) {
                const auto value = matcher.substitute(send, failure);
                if (!failure.empty())
                    return;
                if (!connection.sendLine(toJsonString(value))) {
                    failure = "cannot send the request";
                    return;
                }
                if (expect != nullptr)
                    failure = matcher.expectLine(connection, *expect);
            };
            if (memberOf(vector, "type").text() == "request") {
                sendAndExpect(memberOf(vector, "request"), &memberOf(vector, "response"));
                if (failure.empty() && memberOf(vector, "connectionAfter").text() == "open"
                    && !connection.staysQuiet(kOpenCheckMs))
                    failure = "the connection closed or sent more";
                return;
            }
            const auto& steps = memberOf(vector, "steps");
            for (std::size_t i = 0; i < steps.size() && failure.empty(); ++i) {
                const auto& step = steps.at(i);
                if (const JsonValue* send = step.find("send")) {
                    if (memberOf(*send, "method").text() == "ui.setSelection") {
                        // Opening the clip in the piano roll, on the test bridge; the
                        // next step expects its response.
                        bridge.view.workspace = "pianoRoll";
                        bridge.pianoRollKey = created.empty() ? ContentKey{} : created.front().key;
                        bridge.view.pianoRollPlacementId = created.empty() ? 0 : created.front().placementId;
                        ++i;
                        continue;
                    }
                    sendAndExpect(*send, nullptr);
                } else {
                    failure = matcher.expectLine(connection, memberOf(step, "expect"));
                }
                if (!failure.empty())
                    failure = "step " + std::to_string(i + 1) + ": " + failure;
            }
        },
        kJobExchangeMs);
    // The next vector starts from the defaults again.
    resetPreferences(*processor);
    return failure;
}

void runVectorsOf(const char* testName, const char* directory, const std::set<std::string>& capabilities,
                  const std::set<std::string>& methods, const std::vector<std::string>& mustRun, bool withClips)
{
    VectorReport report(testName);
    const auto root = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors" / directory;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.path().extension() == ".json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    int passed = 0;
    int skipped = 0;
    std::set<std::string> passedIds;
    for (const auto& file : files) {
        const auto parsed = parseJson(readFile(file));
        if (!CONTROL_CHECK(parsed.ok))
            continue;
        const auto& vector = parsed.value;
        const auto id = memberOf(vector, "id").text();
        if (const auto reason = vectorSkipReason(vector, capabilities, methods); !reason.empty()) {
            ++skipped;
            report.skip(id, reason);
            note("SKIP " + id + ": " + reason);
            continue;
        }
        const auto started = Clock::now();
        const auto failure = runVector(vector, withClips);
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty())) {
            ++passed;
            passedIds.insert(id);
        }
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    for (const auto& id : mustRun)
        CONTROL_CHECK(mustRunHolds(passedIds, id));
    note(std::to_string(passed) + " vectors passed, " + std::to_string(skipped) + " skipped");
}

} // namespace

void scenarioPrefsVectors()
{
    // prefs/get-ok is a read without preconditions, run by the Vectors scenario; prefs/get-options needs the options
    // of the installation and runs here.
    runVectorsOf("ControlProcessorTests.PrefsVectors", "prefs", kPrefsVectorCapabilities, kPrefsVectorMethods,
                 {"prefs/get-options", "prefs/set-experimental-features", "prefs/set-f0-model-noop", "prefs/set-hybrid",
                  "prefs/set-job-failures", "prefs/set-no-op", "prefs/set-option-unavailable",
                  "prefs/set-option-unavailable-unchanged", "prefs/set-out-of-range", "prefs/set-rendering-priority",
                  "prefs/set-rendering-priority-fallback", "prefs/set-rendering-priority-gpu", "prefs/set-scheme",
                  "prefs/set-scheme-job", "prefs/set-tuning", "prefs/set-tuning-revert",
                  "prefs/set-vocoder-load-failed", "prefs/set-vocoder-not-ready", "prefs/set-vocoder-weight"},
                 true);
}

void scenarioAudioVectors()
{
    runVectorsOf("ControlProcessorTests.AudioVectors", "audio", kAudioVectorCapabilities, kAudioVectorMethods,
                 {"audio/get-no-device", "audio/get-ok", "audio/set-asio-rate-not-needed",
                  "audio/set-asio-rate-not-restored", "audio/set-device-type", "audio/set-ok",
                  "audio/set-output-channel-pair", "audio/set-output-device", "audio/set-rollback",
                  "audio/set-unavailable"},
                 false);
}

} // namespace ControlTest
} // namespace OpenTune
