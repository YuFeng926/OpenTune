// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scenarios of the transport (transport.control, transport.setTempo, transport.setLoop) and of the tracks (track.set,
// track.add, track.duplicate, track.delete) against the real processor without a window. A test bridge stands in for
// the editor (the number of shown tracks, the dirty flag, the notifications) and a fake audio device in a real
// juce::AudioDeviceManager stands in for the sound card, so that a vector can say whether one runs. Also the barrier
// that a track deletion puts in OpenTune's undo history (PROTOCOL.md section 15.3 rule 6), with undo entries that
// address tracks (the clip move of OpenTune's own MultiMovePlacementAction, wrapped as the API wraps it) and
// entries of the UI. Registered with the other processor scenarios in ControlProcessorTests.cpp.

#include "ControlContentFixtures.h"
#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Content/EditableContentSnapshot.h"
#include "Control/ControlService.h"
#include "Control/Edit/ControlOriginAction.h"
#include "PluginProcessor.h"
#include "Utils/PlacementActions.h"
#include "Utils/UndoManager.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace OpenTune {
namespace ControlTest {

namespace {

constexpr int kVectorExchangeMs = 60000;
/** The number of track slots (OpenTuneAudioProcessor::MAX_TRACKS) and the number shown after a start. */
constexpr int kSlots = OpenTuneAudioProcessor::MAX_TRACKS;
constexpr int kShownAtStart = 2;

JsonValue json(const std::string& text)
{
    auto parsed = parseJson(text);
    CONTROL_CHECK(parsed.ok);
    return parsed.value;
}

std::string quoted(const std::string& text)
{
    std::string out;
    appendJsonQuoted(out, text);
    return out;
}

bool near(const JsonValue& value, double expected, double tolerance = 1e-6)
{
    return value.isNumber() && std::abs(value.asDouble() - expected) <= tolerance;
}

bool near(double value, double expected, double tolerance = 1e-6)
{
    return std::abs(value - expected) <= tolerance;
}

// ---- A fake audio device ---------------------------------------------------------------------------

/** An output device that runs at 44100 Hz with a buffer of 512 frames and opens and closes without a sound card. */
class FakeDevice final : public juce::AudioIODevice
{
public:
    FakeDevice() : juce::AudioIODevice("Fake output", "Fake") {}

    juce::StringArray getOutputChannelNames() override { return {"Left", "Right"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {44100.0, 48000.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {256, 512}; }
    int getDefaultBufferSize() override { return 512; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double sampleRate, int bufferSize) override
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        bufferSize_ = bufferSize > 0 ? bufferSize : 512;
        open_ = true;
        return {};
    }
    void close() override
    {
        open_ = false;
        playing_ = false;
    }
    bool isOpen() override { return open_; }
    void start(juce::AudioIODeviceCallback* callback) override
    {
        callback_ = callback;
        if (callback_ != nullptr)
            callback_->audioDeviceAboutToStart(this);
        playing_ = true;
    }
    void stop() override
    {
        if (callback_ != nullptr)
            callback_->audioDeviceStopped();
        callback_ = nullptr;
        playing_ = false;
    }
    bool isPlaying() override { return playing_; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSize_; }
    double getCurrentSampleRate() override { return sampleRate_; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override
    {
        juce::BigInteger channels;
        channels.setRange(0, 2, true);
        return channels;
    }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

private:
    bool open_ = false;
    bool playing_ = false;
    double sampleRate_ = 44100.0;
    int bufferSize_ = 512;
    juce::AudioIODeviceCallback* callback_ = nullptr;
};

class FakeDeviceType final : public juce::AudioIODeviceType
{
public:
    FakeDeviceType() : juce::AudioIODeviceType("Fake") {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool wantInputNames) const override
    {
        return wantInputNames ? juce::StringArray() : juce::StringArray("Fake output");
    }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice* device, bool) const override { return device != nullptr ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String& outputName, const juce::String&) override
    {
        return outputName.isNotEmpty() ? new FakeDevice() : nullptr;
    }
};

/** A device manager with the fake device running, or none open. */
class FakeAudio
{
public:
    explicit FakeAudio(bool running)
    {
        if (!running)
            return;
        manager.addAudioDeviceType(std::make_unique<FakeDeviceType>());
        const auto error = manager.initialise(0, 2, nullptr, true);
        CONTROL_CHECK(error.isEmpty());
        if (error.isNotEmpty())
            note("the fake audio device did not open: " + error.toStdString());
    }

    juce::AudioDeviceManager manager;
};

// ---- A bridge that follows the editor in what these methods need -----------------------------------

class TracksBridge : public TestUiBridge
{
public:
    juce::AudioDeviceManager* audioDevice() override { return audio; }
    /** TrackPanelComponent::setVisibleTrackCount() with the editor's listener: a changed count reaches
        OpenTuneAudioProcessorEditor::visibleTrackCountChanged(), which saves the count in the arrangement and marks the
        project (the number of visible tracks is project state). The mark is the panel's: dirtyMarks counts only the
        marks the control API makes itself, so that a test can tell an operation that marks the project from one that
        leaves it to the panel (panelDirtyMarks counts these). */
    bool setVisibleTrackCount(int count) override
    {
        count = std::clamp(count, 1, kSlots);
        shownCounts.push_back(count);
        if (count != visibleTracks) {
            visibleTracks = count;
            ++panelDirtyMarks;
            project.dirty = true;
            if (session != nullptr)
                session->markDirty();
        }
        return true;
    }
    void onTrackMixChanged() override { ++mixChanges; }
    void onTransportChanged() override { ++transportChanges; }
    void onTempoChanged() override { ++tempoChanges; }

    juce::AudioDeviceManager* audio = nullptr;
    std::vector<int> shownCounts;
    int panelDirtyMarks = 0;
    int mixChanges = 0;
    int transportChanges = 0;
    int tempoChanges = 0;
};

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

/** A started processor with a session, a test bridge (when asked) and a fake audio device (running or not). */
struct Fixture
{
    std::unique_ptr<OpenTuneAudioProcessor> processor;
    std::unique_ptr<FakeAudio> audio;
    TracksBridge bridge;
    std::unique_ptr<ScopedBridge> registered;
    ApiSession api;
    std::string instanceShort;

    bool open(bool audioRunning = true, bool withBridge = true)
    {
        processor = startedProcessor();
        auto& service = processor->getControlService();
        if (!service.isRunning())
            return false;
        audio = std::make_unique<FakeAudio>(audioRunning);
        bridge.audio = &audio->manager;
        bridge.visibleTracks = kShownAtStart;
        if (withBridge)
            registered = std::make_unique<ScopedBridge>(service, bridge);
        if (!api.open(service))
            return false;
        instanceShort = memberOf(api.discovery(), "instanceId").text().substr(0, 8);
        return true;
    }

    ControlService& service() { return processor->getControlService(); }
    StandaloneArrangement& arrangement() { return *processor->getStandaloneArrangement(); }

    /** A clip of 6 s from 2 s on a track, with a synthetic ready analysis and two notes. */
    Clip addClip(int trackId, double startSec = 2.0)
    {
        ClipSpec spec;
        spec.name = "clip" + std::to_string(trackId);
        spec.trackId = trackId;
        spec.startTimelineSec = startSec;
        spec.notes = {{0.5, 1.5, 64.0}, {2.0, 3.0, 69.0}};
        return fixtureAddClip(*processor, spec);
    }

    JsonValue project() { return api.result("project.get"); }
    std::string arrangementRevision() { return memberOf(memberOf(project(), "asOf"), "arrangementRevision").text(); }
    JsonValue transport() { return memberOf(api.result("transport.get"), "transport"); }

    JsonValue control(const std::string& action, const std::string& extra = {})
    {
        return api.result("transport.control", json("{\"action\":" + quoted(action) + extra + "}"));
    }
    JsonValue setTracks(const std::string& tracks)
    {
        return api.result("track.set", json("{\"tracks\":" + tracks + "}"));
    }
    JsonValue deleteTrack(int trackId, bool confirm = true)
    {
        return api.result("track.delete",
                          json("{\"trackId\":" + std::to_string(trackId) + ",\"expectedArrangementRevision\":"
                               + quoted(arrangementRevision()) + (confirm ? ",\"confirm\":true" : "") + "}"));
    }
    bool deleteFails(int trackId, bool confirm, const std::string& kind, const std::string& reason,
                     const std::string& revision = {})
    {
        return api.fails("track.delete",
                         json("{\"trackId\":" + std::to_string(trackId) + ",\"expectedArrangementRevision\":"
                              + quoted(revision.empty() ? arrangementRevision() : revision)
                              + (confirm ? ",\"confirm\":true" : "") + "}"),
                         kind, reason);
    }
};

/** The change entry of an entity, or null. */
JsonValue changeOf(const JsonValue& result, const char* entity, const char* refMember, const JsonValue& refValue)
{
    const auto& changes = memberOf(result, "changes");
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const auto& change = changes.at(i);
        if (memberOf(change, "entity").text() != entity)
            continue;
        const JsonValue* value = memberOf(change, "ref").find(refMember);
        if (value != nullptr && VectorMatcher::equal(*value, refValue))
            return change;
    }
    return JsonValue();
}

JsonValue trackChange(const JsonValue& result, int trackId)
{
    return changeOf(result, "track", "trackId", JsonValue::integer(trackId));
}

bool hasWarning(const JsonValue& result, const char* code)
{
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        if (memberOf(warnings.at(i), "code").text() == code)
            return true;
    return false;
}

std::string warningCodes(const JsonValue& result)
{
    std::string codes;
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        codes += (codes.empty() ? "" : ",") + memberOf(warnings.at(i), "code").text();
    return codes;
}

/** The track entry of project.get. */
JsonValue trackOf(const JsonValue& project, int trackId)
{
    const auto& tracks = memberOf(project, "tracks");
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (static_cast<int>(memberOf(tracks.at(i), "trackId").asDouble()) == trackId)
            return tracks.at(i);
    return JsonValue();
}

/** The placement entries of project.get on a track. */
std::vector<JsonValue> placementsOn(const JsonValue& project, int trackId)
{
    std::vector<JsonValue> out;
    const auto& placements = memberOf(project, "placements");
    for (std::size_t i = 0; i < placements.size(); ++i)
        if (static_cast<int>(memberOf(placements.at(i), "trackId").asDouble()) == trackId)
            out.push_back(placements.at(i));
    return out;
}

bool listsContent(const JsonValue& project, const std::string& contentId)
{
    const auto& contents = memberOf(project, "contents");
    for (std::size_t i = 0; i < contents.size(); ++i)
        if (memberOf(contents.at(i), "contentId").text() == contentId)
            return true;
    return false;
}

// ---- Scenario: the transport -----------------------------------------------------------------------

void checkTransportActions(Fixture& f)
{
    const auto initial = f.transport();
    CONTROL_CHECK(!memberOf(initial, "playing").asBoolean() && near(memberOf(initial, "positionTimelineSec"), 0.0));
    CONTROL_CHECK(near(memberOf(initial, "bpm"), 120.0) && memberOf(initial, "audioDeviceRunning").asBoolean());
    CONTROL_CHECK(memberOf(initial, "lastControl").isNull() && !memberOf(initial, "loopEnabled").asBoolean());

    // seek: the position, and the position that play from start returns to; recorded as the last transport call.
    const int notifications = f.bridge.transportChanges;
    const auto seek = f.control("seek", R"(,"positionTimelineSec":3.5)");
    CONTROL_CHECK(memberOf(seek, "applied").asBoolean() && !memberOf(seek, "noOp").asBoolean());
    const auto change = changeOf(seek, "transport", "singleton", JsonValue::string("transport"));
    CONTROL_CHECK(near(memberOf(memberOf(memberOf(change, "fields"), "positionTimelineSec"), "after"), 3.5));
    CONTROL_CHECK(near(memberOf(memberOf(memberOf(change, "fields"), "playStartTimelineSec"), "after"), 3.5));
    CONTROL_CHECK(memberOf(memberOf(seek, "undo"), "reason").text() == "not_persisted_state"
                  && memberOf(memberOf(seek, "undo"), "revert").isNull() && !hasWarning(seek, "IRREVERSIBLE"));
    CONTROL_CHECK(near(memberOf(memberOf(seek, "transport"), "playStartTimelineSec"), 3.5));
    CONTROL_CHECK(memberOf(memberOf(memberOf(seek, "transport"), "lastControl"), "call").text() == "seek");
    CONTROL_CHECK(f.bridge.transportChanges == notifications + 1 && f.bridge.dirtyMarks == 0);
    CONTROL_CHECK(near(f.processor->getPlayStartPosition(), 3.5) && near(f.processor->getPosition(), 3.5));

    // play: from the current position; the play start position is the one of the last play.
    const auto play = f.control("play");
    CONTROL_CHECK(memberOf(memberOf(play, "transport"), "playing").asBoolean() && f.processor->isPlaying());
    CONTROL_CHECK(warningCodes(play).empty()
                  && memberOf(memberOf(memberOf(play, "transport"), "lastControl"), "call").text() == "play");
    // A play during playback is the pause button's job in the UI: nothing to do.
    const auto again = f.control("play");
    CONTROL_CHECK(memberOf(again, "applied").asBoolean() && memberOf(again, "noOp").asBoolean());
    CONTROL_CHECK(memberOf(memberOf(again, "undo"), "reason").text() == "no_op");

    // pause keeps the position; a second pause is a no-op.
    const auto pause = f.control("pause");
    CONTROL_CHECK(!memberOf(memberOf(pause, "transport"), "playing").asBoolean() && !f.processor->isPlaying());
    CONTROL_CHECK(near(memberOf(memberOf(pause, "transport"), "positionTimelineSec"), 3.5));
    CONTROL_CHECK(memberOf(f.control("pause"), "noOp").asBoolean());

    // playFromLastStart returns to the position of the last play or seek, not to 0.
    f.control("seek", R"(,"positionTimelineSec":1)");
    f.control("seek", R"(,"positionTimelineSec":4)");
    CONTROL_CHECK(near(f.processor->getPlayStartPosition(), 4.0));
    const auto fromStart = f.control("playFromLastStart");
    CONTROL_CHECK(memberOf(memberOf(fromStart, "transport"), "playing").asBoolean());
    CONTROL_CHECK(near(memberOf(memberOf(fromStart, "transport"), "positionTimelineSec"), 4.0)
                  && near(memberOf(memberOf(fromStart, "transport"), "playStartTimelineSec"), 4.0));

    // pauseAtLastStart pauses and goes back to the last start; stopped, it has nothing to do.
    const auto atStart = f.control("pauseAtLastStart");
    CONTROL_CHECK(!memberOf(memberOf(atStart, "transport"), "playing").asBoolean());
    CONTROL_CHECK(near(memberOf(memberOf(atStart, "transport"), "positionTimelineSec"), 4.0));
    CONTROL_CHECK(memberOf(f.control("pauseAtLastStart"), "noOp").asBoolean());

    // stop: the audio callback lands the transport on 0 when its fade-out completes (0.2 s). The result reports the
    // transport state right after the call, which is what transport.get returns now; a read after the fade returns 0.
    f.control("play");
    const auto stop = f.control("stop");
    CONTROL_CHECK(!memberOf(memberOf(stop, "transport"), "playing").asBoolean());
    const auto stoppedAt = f.transport();
    CONTROL_CHECK(near(memberOf(memberOf(stop, "transport"), "positionTimelineSec"),
                       memberOf(stoppedAt, "positionTimelineSec").asDouble()));
    CONTROL_CHECK(near(memberOf(memberOf(stop, "transport"), "playStartTimelineSec"), 4.0));
    CONTROL_CHECK(memberOf(memberOf(memberOf(stop, "transport"), "lastControl"), "call").text() == "stop");
    CONTROL_CHECK(f.bridge.dirtyMarks == 0);
}

/** The result of stop and the transport.get that follows it agree, a few times in a row: the position is the presented
    one the stop froze, not a landing value constructed before the read. The test device has no audio callback, so
    this only guards against one regression: putting `positionAfter = 0.0` back for stop. That the position reads 0
    after the 0.2 s fade is confirmed by the real-Standalone tour. */
void checkStopMatchesRead(Fixture& f)
{
    for (int i = 0; i < 5; ++i) {
        const double seekTo = 0.25 + static_cast<double>(i) * 0.01;
        f.control("seek", ",\"positionTimelineSec\":" + std::to_string(seekTo));
        const auto play = f.control("play");
        CONTROL_CHECK(memberOf(memberOf(play, "transport"), "playing").asBoolean());
        const auto stop = f.control("stop");
        const auto read = f.transport();
        CONTROL_CHECK(!memberOf(read, "playing").asBoolean());
        CONTROL_CHECK(near(memberOf(memberOf(stop, "transport"), "positionTimelineSec"),
                           memberOf(read, "positionTimelineSec").asDouble()));
        CONTROL_CHECK(near(memberOf(memberOf(stop, "transport"), "playStartTimelineSec"),
                           memberOf(read, "playStartTimelineSec").asDouble()));
        const auto change = changeOf(stop, "transport", "singleton", JsonValue::string("transport"));
        const auto& fields = memberOf(change, "fields");
        if (memberOf(fields, "positionTimelineSec").isObject()) {
            CONTROL_CHECK(near(memberOf(memberOf(fields, "positionTimelineSec"), "after"),
                               memberOf(read, "positionTimelineSec").asDouble()));
        }
    }
}

void checkTransportChecks(Fixture& f)
{
    const auto before = f.transport();
    for (const char* params :
         {R"({"action":"seek"})", R"({"action":"play","positionTimelineSec":1})", R"({"action":"rewind"})",
          R"({"action":"seek","positionTimelineSec":-1})", R"({})", R"({"action":"play","extra":1})"})
        CONTROL_CHECK(f.api.fails("transport.control", json(params), "INVALID_ARGUMENT", "invalid_params"));
    // While the user holds the mouse the transport is not moved under his hand.
    f.bridge.busy.pointerDown = true;
    CONTROL_CHECK(f.api.fails("transport.control", json(R"({"action":"play"})"), "BUSY", "pointer_down"));
    CONTROL_CHECK(f.api.fails("transport.setLoop", json(R"({"enabled":true})"), "BUSY", "pointer_down"));
    CONTROL_CHECK(f.api.fails("transport.setTempo", json(R"({"bpm":90})"), "BUSY", "pointer_down"));
    f.bridge.busy = {};
    CONTROL_CHECK(!f.processor->isPlaying() && !f.processor->isLoopEnabled() && near(f.processor->getBpm(), 120.0));
    CONTROL_CHECK(VectorMatcher::equal(memberOf(before, "playing"), memberOf(f.transport(), "playing")));
}

void checkTempo(Fixture& f)
{
    const auto set =
        f.api.result("transport.setTempo", json(R"({"bpm":96,"timeSignature":{"numerator":3,"denominator":4}})"));
    CONTROL_CHECK(memberOf(set, "applied").asBoolean() && !memberOf(set, "noOp").asBoolean());
    CONTROL_CHECK(near(f.processor->getBpm(), 96.0) && f.processor->getTimeSigNumerator() == 3
                  && f.processor->getTimeSigDenominator() == 4);
    CONTROL_CHECK(near(memberOf(memberOf(set, "tempo"), "bpm"), 96.0));
    CONTROL_CHECK(memberOf(memberOf(set, "dirty"), "after").asBoolean() && f.bridge.dirtyMarks == 1);
    CONTROL_CHECK(f.bridge.tempoChanges == 1);
    CONTROL_CHECK(memberOf(memberOf(set, "undo"), "reason").text() == "ui_parity" && !hasWarning(set, "IRREVERSIBLE"));
    const auto& revert = memberOf(memberOf(set, "undo"), "revert");
    CONTROL_CHECK(memberOf(revert, "method").text() == "transport.setTempo");
    CONTROL_CHECK(near(memberOf(memberOf(revert, "params"), "bpm"), 120.0));
    CONTROL_CHECK(near(memberOf(memberOf(memberOf(revert, "params"), "timeSignature"), "numerator"), 4.0));

    // The revert recipe, sent unchanged, puts the previous tempo and time signature back.
    const auto reverted = f.api.result("transport.setTempo", memberOf(revert, "params"));
    CONTROL_CHECK(memberOf(reverted, "applied").asBoolean() && !memberOf(reverted, "noOp").asBoolean());
    CONTROL_CHECK(near(f.processor->getBpm(), 120.0) && f.processor->getTimeSigNumerator() == 4
                  && f.processor->getTimeSigDenominator() == 4);

    // One member alone; the same values again; limits.
    const int marks = f.bridge.dirtyMarks;
    const auto onlyBpm = f.api.result("transport.setTempo", json(R"({"bpm":87.5})"));
    CONTROL_CHECK(near(f.processor->getBpm(), 87.5) && f.processor->getTimeSigNumerator() == 4);
    CONTROL_CHECK(!memberOf(onlyBpm, "noOp").asBoolean());
    // The bpm reported as changed is in the recipe (never an empty params), and the signature that did not change is
    // not.
    const auto& onlyBpmParams = memberOf(memberOf(memberOf(onlyBpm, "undo"), "revert"), "params");
    CONTROL_CHECK(near(memberOf(onlyBpmParams, "bpm"), 120.0) && onlyBpmParams.find("timeSignature") == nullptr);
    CONTROL_CHECK(memberOf(f.api.result("transport.setTempo", json(R"({"bpm":87.5})")), "noOp").asBoolean());
    CONTROL_CHECK(f.bridge.dirtyMarks == marks + 1);
    for (const char* params :
         {R"({})", R"({"bpm":0.5})", R"({"bpm":1000})", R"({"timeSignature":{"numerator":3,"denominator":3}})",
          R"({"timeSignature":{"numerator":33,"denominator":4}})", R"({"timeSignature":{"numerator":3}})",
          R"({"timeSignature":{"numerator":0,"denominator":4}})", R"({"bpm":"fast"})", R"({"tempo":90})"})
        CONTROL_CHECK(f.api.fails("transport.setTempo", json(params), "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(near(f.processor->getBpm(), 87.5));

    // A time signature that the schema cannot express (OpenTune accepts a denominator of 64 and a numerator up to
    // 64): a call that leaves it alone gets a recipe with the tempo only, one that changes it leaves no recipe.
    f.processor->setTimeSignature(5, 64);
    const auto keeps = f.api.result("transport.setTempo", json(R"({"bpm":100})"));
    const auto& keepsRevert = memberOf(memberOf(keeps, "undo"), "revert");
    CONTROL_CHECK(!hasWarning(keeps, "IRREVERSIBLE") && memberOf(keepsRevert, "params").find("timeSignature") == nullptr
                  && near(memberOf(memberOf(keepsRevert, "params"), "bpm"), 87.5));
    const auto changes =
        f.api.result("transport.setTempo", json(R"({"timeSignature":{"numerator":3,"denominator":4}})"));
    CONTROL_CHECK(memberOf(changes, "applied").asBoolean() && memberOf(memberOf(changes, "undo"), "revert").isNull()
                  && hasWarning(changes, "IRREVERSIBLE"));
    CONTROL_CHECK(f.processor->getTimeSigNumerator() == 3 && f.processor->getTimeSigDenominator() == 4);

    // The same for a numerator above 32, which the schema cannot express either. A call that leaves the signature
    // alone gets a recipe of the tempo only, and that recipe is a complete one: sent unchanged it puts the tempo
    // back and leaves the signature as it was.
    f.processor->setTimeSignature(40, 4);
    const double bpmBefore = f.processor->getBpm();
    const auto keepsBig = f.api.result("transport.setTempo", json(R"({"bpm":110})"));
    const auto& keepsBigRevert = memberOf(memberOf(keepsBig, "undo"), "revert");
    CONTROL_CHECK(memberOf(keepsBig, "applied").asBoolean() && !hasWarning(keepsBig, "IRREVERSIBLE")
                  && memberOf(keepsBigRevert, "params").find("timeSignature") == nullptr
                  && near(memberOf(memberOf(keepsBigRevert, "params"), "bpm"), bpmBefore));
    CONTROL_CHECK(
        memberOf(f.api.result("transport.setTempo", memberOf(keepsBigRevert, "params")), "applied").asBoolean());
    CONTROL_CHECK(near(f.processor->getBpm(), bpmBefore) && f.processor->getTimeSigNumerator() == 40
                  && f.processor->getTimeSigDenominator() == 4);
    // A call that changes the signature as well leaves no recipe at all, the tempo included: a recipe has to restore
    // the whole state, and this one could not restore the 40/4, so a recipe of the tempo alone would be a partial one.
    // The warning says which signature could not be put back.
    const auto both =
        f.api.result("transport.setTempo", json(R"({"bpm":90,"timeSignature":{"numerator":3,"denominator":4}})"));
    CONTROL_CHECK(memberOf(both, "applied").asBoolean() && memberOf(memberOf(both, "undo"), "revert").isNull()
                  && memberOf(memberOf(both, "undo"), "reason").text() == "ui_parity");
    CONTROL_CHECK(near(f.processor->getBpm(), 90.0) && f.processor->getTimeSigNumerator() == 3
                  && f.processor->getTimeSigDenominator() == 4);
    CONTROL_CHECK(warningCodes(both) == "IRREVERSIBLE");
    const auto& irreversible = memberOf(memberOf(both, "warnings").at(0), "data");
    CONTROL_CHECK(memberOf(irreversible, "reason").text() == "previous_time_signature_not_settable"
                  && near(memberOf(memberOf(irreversible, "timeSignature"), "numerator"), 40.0)
                  && near(memberOf(memberOf(irreversible, "timeSignature"), "denominator"), 4.0));
}

void checkLoop(Fixture& f)
{
    const int marks = f.bridge.dirtyMarks;
    const auto on = f.api.result("transport.setLoop", json(R"({"enabled":true})"));
    CONTROL_CHECK(f.processor->isLoopEnabled() && memberOf(memberOf(on, "transport"), "loopEnabled").asBoolean());
    CONTROL_CHECK(warningCodes(on) == "LOOP_NO_EFFECT_STANDALONE,NOT_PERSISTED");
    CONTROL_CHECK(memberOf(memberOf(on, "undo"), "reason").text() == "not_persisted_state");
    CONTROL_CHECK(!memberOf(memberOf(memberOf(memberOf(on, "undo"), "revert"), "params"), "enabled").asBoolean());
    CONTROL_CHECK(f.bridge.dirtyMarks == marks);
    CONTROL_CHECK(memberOf(f.api.result("transport.setLoop", json(R"({"enabled":true})")), "noOp").asBoolean());
    const auto off = f.api.result("transport.setLoop", json(R"({"enabled":false})"));
    CONTROL_CHECK(!f.processor->isLoopEnabled() && warningCodes(off) == "NOT_PERSISTED");
    for (const char* params : {R"({})", R"({"enabled":1})", R"({"enabled":true,"extra":0})"})
        CONTROL_CHECK(f.api.fails("transport.setLoop", json(params), "INVALID_ARGUMENT", "invalid_params"));
}

void scenarioTransportImpl()
{
    {
        Fixture f;
        if (!f.open(true))
            return;
        checkTransportActions(f);
        checkTransportChecks(f);
        checkStopMatchesRead(f);
        checkTempo(f);
        checkLoop(f);
    }
    // Without a running audio device nothing advances and nothing is heard: play says so.
    {
        Fixture f;
        if (!f.open(false))
            return;
        const auto play = f.control("play");
        CONTROL_CHECK(memberOf(memberOf(play, "transport"), "playing").asBoolean());
        CONTROL_CHECK(!memberOf(memberOf(play, "transport"), "audioDeviceRunning").asBoolean());
        CONTROL_CHECK(warningCodes(play) == "NO_AUDIO_DEVICE");
        CONTROL_CHECK(warningCodes(f.control("pause")).empty());
        CONTROL_CHECK(warningCodes(f.control("playFromLastStart")) == "NO_AUDIO_DEVICE");
    }
    // Without an editor the transport still works (it belongs to the processor), and no device is known.
    {
        Fixture f;
        if (!f.open(true, false))
            return;
        const auto play = f.control("play");
        CONTROL_CHECK(f.processor->isPlaying() && warningCodes(play) == "NO_AUDIO_DEVICE");
        f.control("stop");
        CONTROL_CHECK(!f.processor->isPlaying());
        const auto tempo = f.api.result("transport.setTempo", json(R"({"bpm":100})"));
        CONTROL_CHECK(near(f.processor->getBpm(), 100.0) && !memberOf(tempo, "noOp").asBoolean());
    }
}

// ---- Scenario: track.set, track.add, track.duplicate, track.delete ------------------------------------

void checkTrackSet(Fixture& f)
{
    auto& arrangement = f.arrangement();
    const auto set = f.setTracks(R"([{"trackId":0,"muted":true,"volumeDb":-6}])");
    CONTROL_CHECK(memberOf(set, "applied").asBoolean() && !memberOf(set, "noOp").asBoolean());
    CONTROL_CHECK(arrangement.isTrackMuted(0) && near(arrangement.getTrackVolume(0), 0.501187, 1e-5));
    const auto change = trackChange(set, 0);
    const auto& fields = memberOf(change, "fields");
    CONTROL_CHECK(memberOf(memberOf(fields, "audible"), "after").asBoolean() == false);
    CONTROL_CHECK(near(memberOf(memberOf(fields, "volumeDb"), "after"), -6.0, 1e-6));
    CONTROL_CHECK(f.bridge.dirtyMarks == 1 && f.bridge.mixChanges == 1);
    CONTROL_CHECK(memberOf(memberOf(set, "undo"), "reason").text() == "ui_parity");
    const auto& revert = memberOf(memberOf(set, "undo"), "revert");
    CONTROL_CHECK(memberOf(revert, "method").text() == "track.set");

    // The revert recipe restores the mute and the volume.
    f.api.result("track.set", memberOf(revert, "params"));
    CONTROL_CHECK(!arrangement.isTrackMuted(0) && near(arrangement.getTrackVolume(0), 1.0, 1e-5));

    // The same again is a no-op and marks nothing.
    const int marks = f.bridge.dirtyMarks;
    CONTROL_CHECK(memberOf(f.setTracks(R"([{"trackId":0,"muted":false}])"), "noOp").asBoolean());
    CONTROL_CHECK(f.bridge.dirtyMarks == marks);

    // The volume knob goes from 0 to 4 (linear): more is limited, with the requested and the applied value.
    const auto clamped = f.setTracks(R"([{"trackId":1,"volumeLinear":6}])");
    CONTROL_CHECK(near(arrangement.getTrackVolume(1), 4.0) && hasWarning(clamped, "CLAMPED"));
    const auto& warning = memberOf(clamped, "warnings").at(0);
    CONTROL_CHECK(memberOf(memberOf(warning, "data"), "field").text() == "volumeLinear"
                  && near(memberOf(memberOf(warning, "data"), "requested"), 6.0)
                  && near(memberOf(memberOf(warning, "data"), "applied"), 4.0));
    CONTROL_CHECK(near(memberOf(memberOf(warning, "subject"), "trackId"), 1.0));
    const auto clampedDb = f.setTracks(R"([{"trackId":1,"volumeDb":20}])");
    CONTROL_CHECK(near(arrangement.getTrackVolume(1), 4.0) && hasWarning(clampedDb, "CLAMPED"));
    CONTROL_CHECK(near(memberOf(memberOf(memberOf(clampedDb, "warnings").at(0), "data"), "applied"), 12.041199, 1e-5));
    f.setTracks(R"([{"trackId":1,"volumeLinear":0}])");
    CONTROL_CHECK(arrangement.getTrackVolume(1) == 0.0f);
    // Silence has no dB: the revert sets it linearly, and a later revert of the loud value uses dB.
    const auto loud = f.setTracks(R"([{"trackId":1,"volumeLinear":1}])");
    CONTROL_CHECK(
        near(memberOf(memberOf(memberOf(memberOf(memberOf(loud, "undo"), "revert"), "params"), "tracks").at(0),
                      "volumeLinear"),
             0.0));

    // Solo changes which other tracks are audible, which the result lists although the call did not name them.
    const auto solo = f.setTracks(R"([{"trackId":1,"solo":true}])");
    const auto other = memberOf(memberOf(trackChange(solo, 0), "fields"), "audible");
    CONTROL_CHECK(!memberOf(other, "after").asBoolean() && memberOf(other, "before").asBoolean());
    CONTROL_CHECK(!memberOf(trackOf(f.project(), 0), "audible").asBoolean());
    CONTROL_CHECK(memberOf(memberOf(memberOf(memberOf(solo, "undo"), "revert"), "params"), "tracks").size() == 1);
    f.setTracks(R"([{"trackId":1,"solo":false}])");

    // The colour is set directly, in any case of hex digits, and read back in capitals.
    const auto colour = f.setTracks(R"([{"trackId":0,"colour":"#3366ff"}])");
    CONTROL_CHECK(memberOf(memberOf(memberOf(trackChange(colour, 0), "fields"), "colour"), "after").text()
                  == "#3366FF");
    CONTROL_CHECK(memberOf(trackOf(f.project(), 0), "colour").text() == "#3366FF");

    // A hidden track can be set too; several tracks in one call.
    const auto several = f.setTracks(R"([{"trackId":5,"muted":true},{"trackId":3,"colour":"#010203"}])");
    CONTROL_CHECK(arrangement.isTrackMuted(5) && !trackChange(several, 3).isNull()
                  && !trackChange(several, 5).isNull());
    CONTROL_CHECK(memberOf(memberOf(memberOf(several, "undo"), "revert"), "params").find("tracks")->size() == 2);

    // Checks: nothing changes.
    const auto revision = f.arrangementRevision();
    CONTROL_CHECK(
        f.api.fails("track.set", json(R"({"tracks":[{"trackId":12,"muted":true}]})"), "NOT_FOUND", "unknown_handle"));
    CONTROL_CHECK(f.api.fails("track.set", json(R"({"tracks":[{"trackId":0,"muted":true},{"trackId":0,"solo":true}]})"),
                              "INVALID_ARGUMENT", "duplicate_track"));
    for (const char* params :
         {R"({"tracks":[]})", R"({"tracks":[{"trackId":0}]})",
          R"({"tracks":[{"trackId":0,"volumeDb":-3,"volumeLinear":0.7}]})",
          R"({"tracks":[{"trackId":0,"volumeLinear":-1}]})", R"({"tracks":[{"trackId":0,"colour":"red"}]})",
          R"({"tracks":[{"trackId":0,"colour":"#12345"}]})", R"({"tracks":[{"trackId":-1,"muted":true}]})",
          R"({"tracks":[{"trackId":0,"muted":1}]})", R"({})", R"({"tracks":[{"trackId":0,"muted":true,"name":"x"}]})"})
        CONTROL_CHECK(f.api.fails("track.set", json(params), "INVALID_ARGUMENT", "invalid_params"));
    f.bridge.busy.modalOpen = true;
    CONTROL_CHECK(f.api.fails("track.set", json(R"({"tracks":[{"trackId":0,"muted":true}]})"), "BUSY", "modal_open"));
    f.bridge.busy = {};
    CONTROL_CHECK(f.arrangementRevision() == revision && !arrangement.isTrackMuted(0));
}

/** The first warning with this code, or null. */
const JsonValue* findWarning(const JsonValue& result, const char* code)
{
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        if (memberOf(warnings.at(i), "code").text() == code)
            return &warnings.at(i);
    return nullptr;
}

/** The warning IRREVERSIBLE of a call of track.add: the only warning, with the first track shown as its subject
    (trackId) and null data. */
bool irreversibleOnTrack(const JsonValue& result, int trackId)
{
    const JsonValue* warning = findWarning(result, "IRREVERSIBLE");
    const bool ok = warning != nullptr && memberOf(result, "warnings").size() == 1
                    && toJsonString(memberOf(*warning, "subject")) == "{\"trackId\":" + std::to_string(trackId) + "}"
                    && memberOf(*warning, "data").isNull();
    if (!ok)
        note("IRREVERSIBLE is not as expected: " + toJsonString(memberOf(result, "warnings")));
    return ok;
}

void checkTrackAdd(Fixture& f)
{
    // The number of visible tracks is project state: the project file stores it, and the + button writes it back to the
    // project through the panel (visibleTrackCountChanged), which marks the project. The call does not mark the project
    // itself (dirtyMarks stays) and leaves it to the panel (panelDirtyMarks), as the real editor does. A project that
    // was not changed before the call is changed after it. The + button has no undo and no request restores the number
    // (track.delete is no revert recipe), so the call is irreversible: no_upstream_undo, revert null, and the warning
    // IRREVERSIBLE on the first track shown.
    f.bridge.project.dirty = false;
    const int marks = f.bridge.dirtyMarks;
    const int panelMarks = f.bridge.panelDirtyMarks;
    const auto added = f.api.result("track.add", json("{}"));
    CONTROL_CHECK(f.bridge.visibleTracks == 3);
    CONTROL_CHECK(near(memberOf(memberOf(added, "visibleTrackCount"), "before"), 2.0)
                  && near(memberOf(memberOf(added, "visibleTrackCount"), "after"), 3.0));
    CONTROL_CHECK(memberOf(memberOf(trackChange(added, 2), "fields"), "visible").size() == 3);
    CONTROL_CHECK(warningCodes(added) == "IRREVERSIBLE" && irreversibleOnTrack(added, 2));
    CONTROL_CHECK(!hasWarning(added, "NOT_PERSISTED"));
    const auto& undo = memberOf(added, "undo");
    CONTROL_CHECK(memberOf(undo, "undoable").isBoolean() && !memberOf(undo, "undoable").asBoolean()
                  && memberOf(undo, "reason").text() == "no_upstream_undo" && memberOf(undo, "revert").isNull());
    CONTROL_CHECK(!memberOf(memberOf(added, "dirty"), "before").asBoolean()
                  && memberOf(memberOf(added, "dirty"), "after").asBoolean());
    CONTROL_CHECK(f.bridge.dirtyMarks == marks && f.bridge.panelDirtyMarks == panelMarks + 1);
    // A shown track keeps the clips it holds: the project lists the track as visible.
    CONTROL_CHECK(memberOf(trackOf(f.project(), 2), "visible").asBoolean());

    // Several tracks at once: one warning, on the first track shown.
    const auto several = f.api.result("track.add", json(R"({"count":4})"));
    CONTROL_CHECK(f.bridge.visibleTracks == 7 && near(memberOf(memberOf(several, "visibleTrackCount"), "after"), 7.0));
    CONTROL_CHECK(warningCodes(several) == "IRREVERSIBLE" && irreversibleOnTrack(several, 3));
    CONTROL_CHECK(memberOf(memberOf(several, "undo"), "reason").text() == "no_upstream_undo");
    CONTROL_CHECK(f.bridge.dirtyMarks == marks);
    // More than the hidden slots: none is shown.
    const auto revision = f.arrangementRevision();
    CONTROL_CHECK(f.api.fails("track.add", json(R"({"count":6})"), "INVALID_ARGUMENT", "track_limit"));
    CONTROL_CHECK(f.bridge.visibleTracks == 7);
    CONTROL_CHECK(!memberOf(f.api.result("track.add", json(R"({"count":5})")), "noOp").asBoolean());
    CONTROL_CHECK(f.bridge.visibleTracks == kSlots);
    CONTROL_CHECK(f.api.fails("track.add", json("{}"), "INVALID_ARGUMENT", "track_limit"));
    for (const char* params :
         {R"({"count":0})", R"({"count":65})", R"({"count":1.5})", R"({"count":"2"})", R"({"n":1})"})
        CONTROL_CHECK(f.api.fails("track.add", json(params), "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(f.arrangementRevision() == revision);
}

void checkTrackDuplicate(Fixture& f)
{
    f.addClip(0);
    f.addClip(1, 2.0);
    settleBackgroundWork(*f.processor);
    f.setTracks(R"([{"trackId":0,"muted":true,"volumeLinear":0.5,"colour":"#ABCDEF"}])");
    const auto before = f.project();
    const auto sourceHandles = fixtureHandlesOf(before, 0, 2.0);
    const auto referencedHandles = fixtureHandlesOf(before, 1, 2.0);
    if (!CONTROL_CHECK(sourceHandles.has_value() && referencedHandles.has_value()))
        return;

    // Mode share, what OpenTune's UI does: the copy shows the same content.
    const int marks = f.bridge.dirtyMarks;
    const auto shared = f.api.result("track.duplicate", json(R"({"trackId":0,"mode":"share"})"));
    CONTROL_CHECK(near(memberOf(shared, "newTrackId"), 2.0) && f.bridge.visibleTracks == 3);
    CONTROL_CHECK(warningCodes(shared) == "SHARED_CONTENT" && memberOf(shared, "created").size() == 1);
    CONTROL_CHECK(f.bridge.dirtyMarks == marks + 1);
    auto project = f.project();
    const auto copies = placementsOn(project, 2);
    if (!CONTROL_CHECK(copies.size() == 1))
        return;
    CONTROL_CHECK(memberOf(copies[0], "contentId").text() == sourceHandles->contentId);
    CONTROL_CHECK(near(memberOf(copies[0], "startTimelineSec"), 2.0) && near(memberOf(copies[0], "durationSec"), 6.0));
    const auto track = trackOf(project, 2);
    CONTROL_CHECK(memberOf(track, "muted").asBoolean() && near(memberOf(track, "volumeLinear"), 0.5)
                  && memberOf(track, "colour").text() == "#ABCDEF" && memberOf(track, "visible").asBoolean());
    const auto& map = memberOf(shared, "idMap");
    CONTROL_CHECK(map.size() == 1 && memberOf(map.at(0), "relation").text() == "copy");
    CONTROL_CHECK(memberOf(memberOf(map.at(0), "from").at(0), "placementId").text() == sourceHandles->placementId);
    const auto& revert = memberOf(memberOf(shared, "undo"), "revert");
    CONTROL_CHECK(memberOf(revert, "method").text() == "track.delete"
                  && memberOf(revert, "requiresConsent").asBoolean());
    CONTROL_CHECK(memberOf(memberOf(revert, "params"), "confirm").isNull()
                  && near(memberOf(memberOf(revert, "params"), "trackId"), 2.0));
    CONTROL_CHECK(memberOf(memberOf(revert, "params"), "expectedArrangementRevision").text()
                  == f.arrangementRevision());

    // The slot of the next copy is the first hidden one.
    const auto cloned = f.api.result("track.duplicate", json(R"({"trackId":1})"));
    CONTROL_CHECK(near(memberOf(cloned, "newTrackId"), 3.0) && warningCodes(cloned).empty());
    project = f.project();
    const auto clones = placementsOn(project, 3);
    if (!CONTROL_CHECK(clones.size() == 1))
        return;
    CONTROL_CHECK(memberOf(clones[0], "contentId").text() != referencedHandles->contentId);
    CONTROL_CHECK(clones[0].find("referencePlacementId") == nullptr);
    // Mode clone gave the copy a content of its own, listed as created, with the notes of the source.
    bool createdContent = false;
    const auto& created = memberOf(cloned, "created");
    for (std::size_t i = 0; i < created.size(); ++i)
        createdContent = createdContent || memberOf(created.at(i), "entity").text() == "content";
    CONTROL_CHECK(createdContent && listsContent(project, memberOf(clones[0], "contentId").text()));
    const auto cloneDetails = f.api.result(
        "content.getNotes", json("{\"contentId\":" + quoted(memberOf(clones[0], "contentId").text()) + "}"));
    CONTROL_CHECK(memberOf(cloneDetails, "notes").size() == 2);
    // Both copies and their sources are in the project, and the copy of a track is a track of its own.
    CONTROL_CHECK(placementsOn(project, 0).size() == 1 && placementsOn(project, 1).size() == 1);

    // Checks: no slot, a clip in the slot, an unknown track.
    CONTROL_CHECK(f.api.fails("track.duplicate", json(R"({"trackId":99})"), "NOT_FOUND", "unknown_handle"));
    // A hidden source slot is refused (the UI offers Duplicate Track on a shown track only).
    CONTROL_CHECK(f.api.fails("track.duplicate", json(R"({"trackId":5})"), "INVALID_ARGUMENT", "track_hidden"));
    CONTROL_CHECK(
        f.api.fails("track.duplicate", json(R"({"trackId":0,"mode":"copy"})"), "INVALID_ARGUMENT", "invalid_params"));
    f.addClip(4);
    CONTROL_CHECK(
        f.api.fails("track.duplicate", json(R"({"trackId":0})"), "INVALID_ARGUMENT", "target_track_occupied"));
    f.bridge.visibleTracks = kSlots;
    CONTROL_CHECK(f.api.fails("track.duplicate", json(R"({"trackId":0})"), "INVALID_ARGUMENT", "track_limit"));
}

void checkTrackDelete(Fixture& f)
{
    auto& arrangement = f.arrangement();
    f.bridge.visibleTracks = 3;
    f.addClip(0);
    f.addClip(1);
    f.addClip(2);
    // A hidden slot that holds a clip: removeTrackAndShift() leaves it where it is.
    f.addClip(5);
    settleBackgroundWork(*f.processor);
    f.setTracks(R"([{"trackId":2,"muted":true,"volumeLinear":0.5,"colour":"#112233"},)"
                R"({"trackId":5,"muted":true,"volumeLinear":0.25,"colour":"#445566"}])");
    const auto before = f.project();
    const auto bHandles = fixtureHandlesOf(before, 1, 2.0);
    const auto cHandles = fixtureHandlesOf(before, 2, 2.0);
    const auto hiddenHandles = fixtureHandlesOf(before, 5, 2.0);
    if (!CONTROL_CHECK(bHandles.has_value() && cHandles.has_value() && hiddenHandles.has_value()))
        return;

    // The checks first; none of them changes anything.
    const auto revision = f.arrangementRevision();
    CONTROL_CHECK(f.deleteFails(1, false, "INVALID_ARGUMENT", "confirm_required"));
    CONTROL_CHECK(f.deleteFails(1, true, "REVISION_CONFLICT", "arrangement_changed", "r:stale"));
    CONTROL_CHECK(f.deleteFails(5, true, "INVALID_ARGUMENT", "track_hidden"));
    CONTROL_CHECK(f.deleteFails(12, true, "NOT_FOUND", "unknown_handle"));
    f.bridge.busy.exportRunning = true;
    CONTROL_CHECK(f.deleteFails(1, true, "BUSY", "export_in_progress"));
    f.bridge.busy = {};
    CONTROL_CHECK(f.arrangementRevision() == revision && f.bridge.visibleTracks == 3);

    const int marks = f.bridge.dirtyMarks;
    const auto deleted = f.deleteTrack(1);
    CONTROL_CHECK(memberOf(deleted, "applied").asBoolean() && !memberOf(deleted, "noOp").asBoolean());
    CONTROL_CHECK(f.bridge.visibleTracks == 2 && f.bridge.dirtyMarks == marks + 1 && f.bridge.mixChanges > 0);
    // The clip of track 2 moved to track 1 with its mix; the last slot was cleared.
    const auto project = f.project();
    CONTROL_CHECK(placementsOn(project, 1).size() == 1 && placementsOn(project, 2).empty());
    CONTROL_CHECK(memberOf(placementsOn(project, 1)[0], "placementId").text() == cHandles->placementId);
    CONTROL_CHECK(arrangement.isTrackMuted(1) && near(arrangement.getTrackVolume(1), 0.5)
                  && !arrangement.isTrackMuted(2) && near(arrangement.getTrackVolume(2), 1.0));
    // trackIdMap, the removed clip and content.
    const auto& map = memberOf(deleted, "trackIdMap");
    CONTROL_CHECK(map.size() == 2 && memberOf(map.at(0), "after").isNull() && near(memberOf(map.at(0), "before"), 1.0)
                  && near(memberOf(map.at(1), "before"), 2.0) && near(memberOf(map.at(1), "after"), 1.0));
    // The clip of the hidden slot stays on its track, with its mix, and is not in trackIdMap or in changes.
    for (std::size_t i = 0; i < map.size(); ++i)
        CONTROL_CHECK(!near(memberOf(map.at(i), "before"), 5.0));
    const auto hiddenClips = placementsOn(project, 5);
    CONTROL_CHECK(hiddenClips.size() == 1
                  && memberOf(hiddenClips[0], "placementId").text() == hiddenHandles->placementId);
    CONTROL_CHECK(arrangement.isTrackMuted(5) && near(arrangement.getTrackVolume(5), 0.25)
                  && !memberOf(trackOf(project, 5), "visible").asBoolean());
    CONTROL_CHECK(
        changeOf(deleted, "placement", "placementId", JsonValue::string(hiddenHandles->placementId)).isNull());
    const auto& removed = memberOf(deleted, "removed");
    CONTROL_CHECK(removed.size() == 2 && memberOf(removed.at(0), "entity").text() == "placement"
                  && memberOf(removed.at(1), "entity").text() == "content");
    CONTROL_CHECK(memberOf(memberOf(removed.at(1), "ref"), "contentId").text() == bHandles->contentId);
    CONTROL_CHECK(!listsContent(project, bHandles->contentId));
    // The retired members of the result are gone.
    CONTROL_CHECK(deleted.find("danglingReferences") == nullptr);
    const auto moved = changeOf(deleted, "placement", "placementId", JsonValue::string(cHandles->placementId));
    CONTROL_CHECK(near(memberOf(memberOf(memberOf(moved, "fields"), "trackId"), "after"), 1.0));
    CONTROL_CHECK(memberOf(memberOf(deleted, "undo"), "reason").text() == "no_upstream_undo"
                  && memberOf(memberOf(deleted, "undo"), "revert").isNull());
    CONTROL_CHECK(warningCodes(deleted) == "IRREVERSIBLE");
    CONTROL_CHECK(near(memberOf(memberOf(deleted, "possiblyStaleEntries"), "undo"), 0.0));
    // The content that left the project is no longer addressable, and the tokens agree with a fresh read.
    CONTROL_CHECK(f.api.fails("content.getNotes", json("{\"contentId\":" + quoted(bHandles->contentId) + "}"),
                              "NOT_FOUND", "unknown_handle"));
    CONTROL_CHECK(memberOf(memberOf(deleted, "tokens"), "arrangementRevision").text() == f.arrangementRevision());

    // The last shown track cannot be deleted.
    f.bridge.visibleTracks = 1;
    CONTROL_CHECK(f.deleteFails(0, true, "INVALID_ARGUMENT", "last_track"));
}

void checkTrackDeleteSharedContent(Fixture& f)
{
    f.addClip(0);
    settleBackgroundWork(*f.processor);
    const auto before = fixtureHandlesOf(f.project(), 0, 2.0);
    if (!CONTROL_CHECK(before.has_value()))
        return;
    f.api.result("track.duplicate", json(R"({"trackId":0,"mode":"share"})"));
    // The content is shown by a clip of track 0 as well, so deleting track 2 removes the clip and not the content.
    const auto deleted = f.deleteTrack(2);
    CONTROL_CHECK(memberOf(deleted, "removed").size() == 1 && listsContent(f.project(), before->contentId));
}

void scenarioTracksImpl()
{
    {
        Fixture f;
        if (f.open())
            checkTrackSet(f);
    }
    {
        Fixture f;
        if (f.open()) {
            checkTrackAdd(f);
        }
    }
    {
        Fixture f;
        if (f.open())
            checkTrackDuplicate(f);
    }
    {
        Fixture f;
        if (f.open())
            checkTrackDelete(f);
    }
    {
        Fixture f;
        if (f.open())
            checkTrackDeleteSharedContent(f);
    }
    // Without an editor the number of shown tracks cannot be told or changed: the three methods that need it say so;
    // track.set works on the arrangement alone.
    {
        Fixture f;
        if (!f.open(true, false))
            return;
        f.addClip(0);
        settleBackgroundWork(*f.processor);
        CONTROL_CHECK(f.api.fails("track.add", json("{}"), "UNSUPPORTED", "no_editor_window"));
        CONTROL_CHECK(f.api.fails("track.duplicate", json(R"({"trackId":0})"), "UNSUPPORTED", "no_editor_window"));
        CONTROL_CHECK(f.api.fails("track.delete",
                                  json("{\"trackId\":0,\"expectedArrangementRevision\":"
                                       + quoted(f.arrangementRevision()) + ",\"confirm\":true}"),
                                  "UNSUPPORTED", "no_editor_window"));
        CONTROL_CHECK(!memberOf(f.setTracks(R"([{"trackId":0,"muted":true}])"), "noOp").asBoolean());
        CONTROL_CHECK(f.arrangement().isTrackMuted(0));
    }
}

// ---- Scenario: the barrier of a track deletion in the undo history ---------------------------------

/** A clip move as OpenTune's clip drag records it: MultiMovePlacementAction on the clip's track. The clip is moved
    from oldStart to its start first, as the drag did. */
std::unique_ptr<MultiMovePlacementAction> clipMoveAction(Fixture& f, const Clip& clip, int recordedTrack,
                                                         int actualTrack, double oldStart)
{
    double newStart = 0.0;
    StandaloneArrangement::Placement placement;
    if (f.arrangement().getPlacementById(actualTrack, clip.placementId, placement))
        newStart = placement.timelineStartSeconds;
    MultiMovePlacementAction::Entry entry;
    entry.sourceTrackId = recordedTrack;
    entry.targetTrackId = recordedTrack;
    entry.placementId = clip.placementId;
    entry.oldStartSeconds = oldStart;
    entry.newStartSeconds = newStart;
    const PlacementKey key{recordedTrack, clip.placementId};
    return std::make_unique<MultiMovePlacementAction>(*f.processor, std::vector<MultiMovePlacementAction::Entry>{entry},
                                                      key, key);
}

/** Pushes a clip move recorded through the API; returns its undoId. */
std::string pushApiMove(Fixture& f, const Clip& clip, int track, double oldStart = 1.0)
{
    auto action = clipMoveAction(f, clip, track, track, oldStart);
    std::vector<ControlOriginAction::Inner> inner(1);
    const auto description = action->getDescription();
    inner[0].action = std::move(action);
    inner[0].name = "MultiMovePlacementAction";
    inner[0].content = clip.key;
    inner[0].tracks = {track};
    const auto undoId = f.service().history()->nextUndoId(f.instanceShort);
    f.service().history()->record(
        f.processor->getUndoManager(),
        std::make_unique<ControlOriginAction>(f.service().history(), undoId, description, std::move(inner)));
    return undoId;
}

/** Pushes a clip move across tracks recorded through the API, as arrangement.edit records it: the clip is on `to`
    now, `from` is where it was; the action keeps both slots for good, and the entry addresses both. Returns its
    undoId. */
std::string pushApiCrossMove(Fixture& f, const Clip& clip, int from, int to, double oldStart = 1.0)
{
    double newStart = 0.0;
    StandaloneArrangement::Placement placement;
    if (f.arrangement().getPlacementById(to, clip.placementId, placement))
        newStart = placement.timelineStartSeconds;
    MultiMovePlacementAction::Entry entry;
    entry.sourceTrackId = from;
    entry.targetTrackId = to;
    entry.placementId = clip.placementId;
    entry.oldStartSeconds = oldStart;
    entry.newStartSeconds = newStart;
    auto action = std::make_unique<MultiMovePlacementAction>(
        *f.processor, std::vector<MultiMovePlacementAction::Entry>{entry}, PlacementKey{from, clip.placementId},
        PlacementKey{to, clip.placementId});
    std::vector<ControlOriginAction::Inner> inner(1);
    const auto description = action->getDescription();
    inner[0].action = std::move(action);
    inner[0].name = "MultiMovePlacementAction";
    inner[0].content = clip.key;
    inner[0].tracks = {from, to};
    const auto undoId = f.service().history()->nextUndoId(f.instanceShort);
    f.service().history()->record(
        f.processor->getUndoManager(),
        std::make_unique<ControlOriginAction>(f.service().history(), undoId, description, std::move(inner)));
    return undoId;
}

void pushUiMove(Fixture& f, const Clip& clip, int track, double oldStart = 1.0)
{
    f.processor->getUndoManager().addAction(clipMoveAction(f, clip, track, track, oldStart));
}

JsonValue historyOf(Fixture& f)
{
    return memberOf(f.api.result("edit.getHistory"), "history");
}

std::string undoParams(const std::string& undoId)
{
    return "{\"expectedUndoId\":" + quoted(undoId) + "}";
}

void checkBarrierApiEntry(Fixture& f)
{
    f.bridge.visibleTracks = 3;
    const auto a = f.addClip(0);
    const auto c = f.addClip(2);
    settleBackgroundWork(*f.processor);
    const auto moveOfC = pushApiMove(f, c, 2);
    const auto moveOfA = pushApiMove(f, a, 0);
    CONTROL_CHECK(!memberOf(memberOf(historyOf(f), "undoTop"), "possiblyStale").asBoolean());

    // Deleting track 1 moves track 2 down: the entry of c is possibly stale, the one of a (track 0) is not.
    const auto deleted = f.deleteTrack(1);
    CONTROL_CHECK(near(memberOf(memberOf(deleted, "possiblyStaleEntries"), "undo"), 1.0)
                  && near(memberOf(memberOf(deleted, "possiblyStaleEntries"), "redo"), 0.0));
    CONTROL_CHECK(warningCodes(deleted) == "IRREVERSIBLE,UNDO_HISTORY_POSSIBLY_STALE");
    auto history = historyOf(f);
    CONTROL_CHECK(memberOf(memberOf(history, "undoTop"), "undoId").text() == moveOfA
                  && !memberOf(memberOf(history, "undoTop"), "possiblyStale").asBoolean());
    // The entry of a on top is applied as usual: no warning.
    const auto undoneA = f.api.result("edit.undo", json(undoParams(moveOfA)));
    CONTROL_CHECK(memberOf(undoneA, "applied").asBoolean() && !memberOf(undoneA, "noOp").asBoolean()
                  && warningCodes(undoneA).empty());
    // The entry of c is now on top and possibly stale: refused without force, and nothing changed.
    history = historyOf(f);
    CONTROL_CHECK(memberOf(memberOf(history, "undoTop"), "undoId").text() == moveOfC
                  && memberOf(memberOf(history, "undoTop"), "possiblyStale").asBoolean());
    const auto revision = f.arrangementRevision();
    const int marks = f.bridge.dirtyMarks;
    CONTROL_CHECK(f.api.fails("edit.undo", json(undoParams(moveOfC)), "UNDO_CONFLICT", "stale_entry"));
    CONTROL_CHECK(f.arrangementRevision() == revision && f.bridge.dirtyMarks == marks);
    CONTROL_CHECK(memberOf(memberOf(historyOf(f), "undoTop"), "possiblyStale").asBoolean());
    // With force the end user's request is carried out, without the stale warning (the clip is on another track
    // now, so the step has nothing to move: it is used up, as the UI uses it), and the entry is confirmed: the
    // redo that reverts the step, without force, is not refused.
    const auto forced = f.api.result("edit.undo", json(R"({"force":true})"));
    CONTROL_CHECK(memberOf(forced, "applied").asBoolean() && warningCodes(forced) == "UNDO_NO_EFFECT");
    CONTROL_CHECK(!memberOf(memberOf(forced, "entry"), "possiblyStale").asBoolean());
    CONTROL_CHECK(!memberOf(memberOf(historyOf(f), "redoTop"), "possiblyStale").asBoolean());
    const auto& recipe = memberOf(memberOf(forced, "undo"), "revert");
    const auto redone = f.api.result(memberOf(recipe, "method").text(), memberOf(recipe, "params"));
    CONTROL_CHECK(memberOf(redone, "applied").asBoolean() && !hasWarning(redone, "UNDO_ENTRY_POSSIBLY_STALE"));

    // Another deletion makes the confirmed entry possibly stale again, and counts it: the first deletion moved the
    // track the entry's clip is on from 2 to 1, and the barrier followed it (the slot the action is keyed by stays 2,
    // which is no longer shown, so only the followed position is in range).
    f.bridge.visibleTracks = 2;
    const auto again = f.deleteTrack(0);
    CONTROL_CHECK(near(memberOf(memberOf(again, "possiblyStaleEntries"), "undo"), 1.0));
    CONTROL_CHECK(memberOf(memberOf(historyOf(f), "undoTop"), "possiblyStale").asBoolean());
}

/** A clip move across tracks (3 to 0) and two deletions. The first deletion moves the shown tracks after it down, and
    the barrier renumbers where the clips are; but the OpenTune action keeps its recorded slots, so its undo() still
    moves the clip to slot 3. The second deletion removes slot 3: the entry is possibly stale again, although neither
    renumbered position (2 and 0) is among the tracks that deletion removed or moved. */
void checkBarrierMoveAcrossTracks(Fixture& f)
{
    f.bridge.visibleTracks = 5;
    const auto a = f.addClip(0);
    settleBackgroundWork(*f.processor);
    const auto move = pushApiCrossMove(f, a, 3, 0);
    CONTROL_CHECK(!memberOf(memberOf(historyOf(f), "undoTop"), "possiblyStale").asBoolean());

    // Deleting track 1 (5 shown) moves the shown tracks 2 to 4 down: slot 3 of the entry is among them.
    const auto first = f.deleteTrack(1);
    CONTROL_CHECK(near(memberOf(memberOf(first, "possiblyStaleEntries"), "undo"), 1.0) && f.bridge.visibleTracks == 4);
    auto top = memberOf(historyOf(f), "undoTop");
    CONTROL_CHECK(memberOf(top, "undoId").text() == move && memberOf(top, "possiblyStale").asBoolean());
    // The end user forces the undo and then the redo: the entry is confirmed.
    const auto forced = f.api.result("edit.undo", json(R"({"force":true})"));
    CONTROL_CHECK(memberOf(forced, "applied").asBoolean()
                  && !memberOf(memberOf(forced, "entry"), "possiblyStale").asBoolean());
    const auto redone = f.api.result("edit.redo", json(undoParams(move)));
    CONTROL_CHECK(memberOf(redone, "applied").asBoolean() && !hasWarning(redone, "UNDO_ENTRY_POSSIBLY_STALE"));
    top = memberOf(historyOf(f), "undoTop");
    CONTROL_CHECK(memberOf(top, "undoId").text() == move && !memberOf(top, "possiblyStale").asBoolean());

    // Deleting slot 3 (4 shown) removes the slot the action still writes to: possibly stale again, and counted.
    const auto second = f.deleteTrack(3);
    CONTROL_CHECK(near(memberOf(memberOf(second, "possiblyStaleEntries"), "undo"), 1.0));
    top = memberOf(historyOf(f), "undoTop");
    CONTROL_CHECK(memberOf(top, "undoId").text() == move && memberOf(top, "possiblyStale").asBoolean());
}

/** A clip move across tracks (5 to 0), two deletions of track 1, a forced undo, a deletion that moves the clip, a
    forced redo that finds nothing to move, and a last deletion that moves the clip down again. The slot of the action
    (5) and the renumbered position of the clip ({2, 0} after the third deletion: the value without the merge below,
    not what followed holds then) name neither the deleted track nor a track after it, yet the clip is moved: the
    entry must be reported (section 15.3 rule 6: too often rather than too rarely). A step in which the action was
    executed (not blocked by a mask) merges the slots of the action into the followed positions. */
void checkBarrierFollowedCoversSlots(Fixture& f)
{
    f.bridge.visibleTracks = 8;
    const auto a = f.addClip(0);
    settleBackgroundWork(*f.processor);
    const auto move = pushApiCrossMove(f, a, 5, 0);

    f.deleteTrack(1);
    f.deleteTrack(1);
    CONTROL_CHECK(f.bridge.visibleTracks == 6);
    // The forced undo moves the clip from 0 to slot 5.
    const auto undone = f.api.result("edit.undo", json(R"({"force":true})"));
    CONTROL_CHECK(memberOf(undone, "applied").asBoolean());
    // Deleting track 2 (6 shown) moves slot 5, where the clip is now, down to 4.
    const auto third = f.deleteTrack(2);
    CONTROL_CHECK(near(memberOf(memberOf(third, "possiblyStaleEntries"), "redo"), 1.0));
    // The forced redo finds no clip at slot 5 and changes nothing; the entry is confirmed all the same.
    f.api.result("edit.redo", json(R"({"force":true})"));
    CONTROL_CHECK(!memberOf(memberOf(historyOf(f), "undoTop"), "possiblyStale").asBoolean());
    // Deleting track 3 (5 shown) moves the clip from 4 to 3: the entry is possibly stale.
    const auto last = f.deleteTrack(3);
    CONTROL_CHECK(near(memberOf(memberOf(last, "possiblyStaleEntries"), "undo"), 1.0));
    const auto top = memberOf(historyOf(f), "undoTop");
    CONTROL_CHECK(memberOf(top, "undoId").text() == move && memberOf(top, "possiblyStale").asBoolean());
}

void checkBarrierLaterTrack(Fixture& f)
{
    f.bridge.visibleTracks = 3;
    const auto a = f.addClip(0);
    f.addClip(2);
    settleBackgroundWork(*f.processor);
    const auto moveOfA = pushApiMove(f, a, 0);
    // Deleting a track after the clip's track moves nothing the entry addresses.
    const auto deleted = f.deleteTrack(2);
    CONTROL_CHECK(near(memberOf(memberOf(deleted, "possiblyStaleEntries"), "undo"), 0.0)
                  && warningCodes(deleted) == "IRREVERSIBLE");
    CONTROL_CHECK(!memberOf(memberOf(historyOf(f), "undoTop"), "possiblyStale").asBoolean());
    const auto undone = f.api.result("edit.undo", json(undoParams(moveOfA)));
    CONTROL_CHECK(memberOf(undone, "applied").asBoolean() && !memberOf(undone, "noOp").asBoolean()
                  && warningCodes(undone).empty());
}

/** An entry of the API that addresses a hidden track: removeTrackAndShift() moves only the shown tracks, so a track
    deletion leaves the hidden slot, its clip and its mix where they are, and the entry is not possibly stale
    (PROTOCOL.md section 15.3 rule 6: only a deletion that removed the entry's track or moved it down counts). */
void checkBarrierHiddenTrack(Fixture& f)
{
    f.bridge.visibleTracks = 3;
    const auto hidden = f.addClip(5);
    settleBackgroundWork(*f.processor);
    const auto hiddenBefore = fixtureHandlesOf(f.project(), 5, 2.0);
    if (!CONTROL_CHECK(hiddenBefore.has_value()))
        return;
    f.setTracks(R"([{"trackId":5,"muted":true,"volumeLinear":0.25,"colour":"#445566"}])");
    const auto moveOfHidden = pushApiMove(f, hidden, 5);
    CONTROL_CHECK(!memberOf(memberOf(historyOf(f), "undoTop"), "possiblyStale").asBoolean());

    // Deleting a shown track moves track 2 down; hidden track 5 keeps its position, its clip and its mix.
    const auto deleted = f.deleteTrack(1);
    CONTROL_CHECK(near(memberOf(memberOf(deleted, "possiblyStaleEntries"), "undo"), 0.0)
                  && near(memberOf(memberOf(deleted, "possiblyStaleEntries"), "redo"), 0.0));
    CONTROL_CHECK(warningCodes(deleted) == "IRREVERSIBLE");
    auto& arrangement = f.arrangement();
    CONTROL_CHECK(arrangement.isTrackMuted(5) && near(arrangement.getTrackVolume(5), 0.25));
    const auto project = f.project();
    const auto hiddenAfter = fixtureHandlesOf(project, 5, 2.0);
    if (!CONTROL_CHECK(hiddenAfter.has_value()))
        return;
    CONTROL_CHECK(hiddenAfter->placementId == hiddenBefore->placementId
                  && !memberOf(trackOf(project, 5), "visible").asBoolean());
    CONTROL_CHECK(changeOf(deleted, "placement", "placementId", JsonValue::string(hiddenAfter->placementId)).isNull());
    // The entry that addresses the hidden track is still on top and is applied as usual, without a force.
    const auto top = memberOf(historyOf(f), "undoTop");
    CONTROL_CHECK(memberOf(top, "undoId").text() == moveOfHidden && !memberOf(top, "possiblyStale").asBoolean());
    const auto undone = f.api.result("edit.undo", json(undoParams(moveOfHidden)));
    CONTROL_CHECK(memberOf(undone, "applied").asBoolean() && !memberOf(undone, "noOp").asBoolean()
                  && warningCodes(undone).empty());
}

void checkBarrierUiEntry(Fixture& f)
{
    f.bridge.visibleTracks = 3;
    const auto a = f.addClip(0);
    f.addClip(2);
    settleBackgroundWork(*f.processor);
    pushUiMove(f, a, 0);
    // An entry of the UI cannot be inspected: after the deletion it is possibly stale, the track it moved on stays.
    const auto deleted = f.deleteTrack(2);
    CONTROL_CHECK(near(memberOf(memberOf(deleted, "possiblyStaleEntries"), "undo"), 1.0));
    auto top = memberOf(historyOf(f), "undoTop");
    CONTROL_CHECK(memberOf(top, "origin").text() == "ui" && memberOf(top, "possiblyStale").asBoolean()
                  && memberOf(top, "undoId").isNull());
    // With force: no warning, an undoId, and no longer possibly stale.
    const auto forced = f.api.result("edit.undo", json(R"({"force":true})"));
    CONTROL_CHECK(warningCodes(forced).empty() && !memberOf(memberOf(forced, "entry"), "possiblyStale").asBoolean());
    const auto uiId = memberOf(memberOf(forced, "entry"), "undoId").text();
    CONTROL_CHECK(!uiId.empty());
    CONTROL_CHECK(!memberOf(memberOf(historyOf(f), "redoTop"), "possiblyStale").asBoolean());

    // Another deletion: the confirmed entry on the redo side is possibly stale again (counted on the redo side), and
    // redo without force applies it with the warning.
    f.bridge.visibleTracks = 3;
    f.addClip(2, 20.0);
    const auto second = f.deleteTrack(2);
    CONTROL_CHECK(near(memberOf(memberOf(second, "possiblyStaleEntries"), "undo"), 0.0)
                  && near(memberOf(memberOf(second, "possiblyStaleEntries"), "redo"), 1.0));
    CONTROL_CHECK(warningCodes(second) == "IRREVERSIBLE,UNDO_HISTORY_POSSIBLY_STALE");
    CONTROL_CHECK(memberOf(memberOf(historyOf(f), "redoTop"), "possiblyStale").asBoolean());
    const auto redone = f.api.result("edit.redo", json(undoParams(uiId)));
    CONTROL_CHECK(memberOf(redone, "applied").asBoolean() && warningCodes(redone) == "UNDO_ENTRY_POSSIBLY_STALE");

    // An entry recorded after the deletion is not possibly stale.
    pushUiMove(f, a, 0, 3.0);
    CONTROL_CHECK(!memberOf(memberOf(historyOf(f), "undoTop"), "possiblyStale").asBoolean());
}

/** The history is undone below the mark of a deletion, the API records an entry there, the UI goes on editing, and
    another deletion follows: the entries recorded after the first deletion are all counted. (The entry of the API
    sits below the mark; it must not be taken off the count of the positions above it.) */
void checkBarrierUndoBelowMark(Fixture& f)
{
    f.bridge.visibleTracks = 4;
    const auto a = f.addClip(0);
    f.addClip(1);
    f.addClip(2);
    f.addClip(3);
    settleBackgroundWork(*f.processor);
    pushUiMove(f, a, 0, 1.0);
    pushUiMove(f, a, 0, 2.0);
    // Deleting the last track: the two entries of the UI were recorded before it.
    const auto first = f.deleteTrack(3);
    CONTROL_CHECK(near(memberOf(memberOf(first, "possiblyStaleEntries"), "undo"), 2.0));
    // The end user undoes one step (below the mark), the API records an entry at that position, and the UI goes on:
    // two more entries, all recorded after the first deletion.
    CONTROL_CHECK(f.processor->getUndoManager().undo() != nullptr);
    pushApiMove(f, a, 0, 3.0);
    pushUiMove(f, a, 0, 4.0);
    pushUiMove(f, a, 0, 5.0);
    f.bridge.visibleTracks = 3;
    const auto second = f.deleteTrack(2);
    // The entry of the API addresses track 0 and is not stale; the entry below the mark was counted by the first
    // deletion; the two entries of the UI that followed are newly stale.
    CONTROL_CHECK(near(memberOf(memberOf(second, "possiblyStaleEntries"), "undo"), 2.0));
}

void checkBarrierRemovedContent(Fixture& f)
{
    f.bridge.visibleTracks = 3;
    f.addClip(0);
    const auto b = f.addClip(1);
    settleBackgroundWork(*f.processor);
    const auto handles = fixtureHandlesOf(f.project(), 1, 2.0);
    if (!CONTROL_CHECK(handles.has_value()))
        return;
    // A note edit made through the API addresses its content, not a track: it is not possibly stale.
    const auto revision = memberOf(handles->tokens, "revision").text();
    const auto edited =
        f.api.result("content.editNotes",
                     json("{\"contentId\":" + quoted(handles->contentId) + ",\"expectedRevision\":" + quoted(revision)
                          + R"(,"ops":[{"op":"shiftCents","select":{"all":true},"deltaCents":30}]})"));
    const auto undoId = memberOf(memberOf(edited, "undo"), "undoId").text();
    if (!CONTROL_CHECK(!undoId.empty()))
        return;
    (void)b;
    // The deletion removes the only clip that showed the content.
    const auto deleted = f.deleteTrack(1);
    CONTROL_CHECK(near(memberOf(memberOf(deleted, "possiblyStaleEntries"), "undo"), 0.0));
    CONTROL_CHECK(!memberOf(memberOf(historyOf(f), "undoTop"), "possiblyStale").asBoolean());
    // The entry is applied as the UI applies it: the step is used up, nothing in the project changes.
    const int marks = f.bridge.dirtyMarks;
    const auto undone = f.api.result("edit.undo", json(undoParams(undoId)));
    CONTROL_CHECK(memberOf(undone, "applied").asBoolean() && memberOf(undone, "noOp").asBoolean());
    CONTROL_CHECK(warningCodes(undone) == "UNDO_NO_EFFECT" && f.bridge.dirtyMarks == marks + 1);
    CONTROL_CHECK(memberOf(memberOf(historyOf(f), "redoTop"), "undoId").text() == undoId);
}

/** A content that a track deletion took out of the project is listed again when an undo brings a clip back. */
void checkRemovedContentRevivedByUndo(Fixture& f)
{
    f.addClip(0);
    settleBackgroundWork(*f.processor);
    const auto source = fixtureHandlesOf(f.project(), 0, 2.0);
    if (!CONTROL_CHECK(source.has_value()))
        return;
    // Track 2 gets a second clip (B) that shows the same content as the clip A of track 0.
    CONTROL_CHECK(
        near(memberOf(f.api.result("track.duplicate", json(R"({"trackId":0,"mode":"share"})")), "newTrackId"), 2.0));
    // The UI deletes A (an undo entry of the UI), then the API deletes track 2: the content has no clip left.
    if (!CONTROL_CHECK(placementsOn(f.project(), 0).size() == 1))
        return;
    const auto outcome = f.processor->deletePlacement(0, 0);
    if (!CONTROL_CHECK(outcome.has_value()))
        return;
    f.processor->getUndoManager().addAction(std::make_unique<DeletePlacementAction>(*f.processor, *outcome));
    // OpenTune reclaims a retired clip on its next message-loop turn, and the undo is lost then; hold the sweep back
    // so that the undo still finds the clip, as it does when the end user presses Ctrl+Z at once.
    f.processor->cancelPendingUpdate();
    f.deleteTrack(2);
    f.processor->cancelPendingUpdate();
    const std::string params = "{\"contentId\":" + quoted(source->contentId) + "}";
    CONTROL_CHECK(f.api.fails("content.getNotes", json(params), "NOT_FOUND", "unknown_handle"));
    // edit.undo refuses a clip deletion (undo_unsafe_build); the end user's Ctrl+Z undoes it through the manager.
    f.processor->cancelPendingUpdate();
    CONTROL_CHECK(f.processor->getUndoManager().undo() != nullptr);
    // The undo brings A back with the content: the project and content.* agree again.
    const auto after = f.project();
    CONTROL_CHECK(placementsOn(after, 0).size() == 1);
    CONTROL_CHECK(listsContent(after, source->contentId));
    CONTROL_CHECK(memberOf(f.api.result("content.getNotes", json(params)), "notes").size() == 2);
}

void scenarioTrackBarrierImpl()
{
    for (const auto check : {checkBarrierApiEntry, checkBarrierMoveAcrossTracks, checkBarrierFollowedCoversSlots,
                             checkBarrierLaterTrack, checkBarrierHiddenTrack, checkBarrierUiEntry,
                             checkBarrierUndoBelowMark, checkBarrierRemovedContent, checkRemovedContentRevivedByUndo}) {
        Fixture f;
        if (f.open())
            check(f);
    }
}

// ---- Scenario: the specification's vectors --------------------------------------------------------------

/** The vectors of the transport and the tracks that must run and pass, by id. The barrier vectors of edit/ belong
    here: a track deletion is what makes their entries stale. */
const char* const kMustRunVectors[] = {
    "transport/control-pause-at-last-start",
    "transport/control-pause-stopped",
    "transport/control-pause",
    "transport/control-play-from-last-start",
    "transport/control-play-no-device",
    "transport/control-play",
    "transport/control-seek",
    "transport/control-stop",
    "transport/set-loop-no-op",
    "transport/set-loop-ok",
    "transport/set-tempo-no-op",
    "transport/set-tempo-ok",
    "transport/set-tempo-revert",
    "track/add-limit",
    "track/add-ok",
    "track/delete-barrier-later-track",
    "track/delete-barrier-ui-entry",
    "track/delete-barrier",
    "track/delete-confirm-required",
    "track/delete-last-track",
    "track/delete-not-found",
    "track/delete-ok",
    "track/delete-track-hidden",
    "track/duplicate-clone",
    "track/duplicate-limit",
    "track/duplicate-not-found",
    "track/duplicate-share",
    "track/duplicate-target-occupied",
    "track/duplicate-track-hidden",
    "track/set-clamped",
    "track/set-colour",
    "track/set-duplicate-track",
    "track/set-not-found",
    "track/set-ok",
    "track/set-solo",
    "edit/undo-content-removed-with-track",
    "edit/undo-no-effect",
    "edit/undo-stale-entry-revert",
    "edit/undo-stale-entry",
    "edit/undo-ui-entry-possibly-stale",
    "edit/undo-unmoved-control-entry",
};

const std::set<std::string> kVectorMethods = {
    "content.editNotes", "transport.control", "transport.setTempo", "transport.setLoop", "track.set", "track.add",
    "track.duplicate",   "track.delete",      "edit.getHistory",    "edit.undo",         "edit.redo", "project.get"};
const std::set<std::string> kVectorCapabilities = {"session.v1", "read.v1",      "history.v1",
                                                   "edit.v1",    "transport.v1", "tracks.v1"};

/** Why this runner does not run a vector, or empty when it does. */
std::string vectorSkipReason(const JsonValue& vector)
{
    const auto capability = memberOf(vector, "capability").text();
    if (capability != "transport.v1" && capability != "tracks.v1" && capability != "history.v1")
        return "covers capability " + capability + ", which another runner covers";
    const auto& extra = memberOf(vector, "extraCapabilities");
    for (std::size_t i = 0; i < extra.size(); ++i)
        if (kVectorCapabilities.count(extra.at(i).text()) == 0)
            return "needs capability " + extra.at(i).text() + ", which this runner does not cover";
    if (memberOf(vector, "session").text() != "authenticated")
        return "unauthenticated vectors test the listener (ControlUnitTests)";
    std::vector<const JsonValue*> requests;
    if (const JsonValue* request = vector.find("request"))
        requests.push_back(request);
    if (const JsonValue* steps = vector.find("steps"))
        for (std::size_t i = 0; i < steps->size(); ++i) {
            const auto& step = steps->at(i);
            if (step.find("connection") != nullptr)
                return "uses steps other than send, expect and pauseMs on the main connection";
            if (const JsonValue* send = step.find("send"))
                requests.push_back(send);
        }
    bool usesTracksOrTransport = false;
    for (const auto* request : requests) {
        const auto method = memberOf(*request, "method").text();
        if (kVectorMethods.count(method) == 0)
            return "uses " + method + ", which this runner does not cover";
        usesTracksOrTransport =
            usesTracksOrTransport || method.rfind("track.", 0) == 0 || method.rfind("transport.", 0) == 0;
    }
    // The vectors of edit/ that this runner takes are those that need a track deletion's barrier.
    const auto id = memberOf(vector, "id").text();
    if (capability == "history.v1" && !usesTracksOrTransport) {
        bool barrier = false;
        const auto& history = memberOf(memberOf(vector, "given"), "history");
        for (const char* side : {"undo", "redo"})
            for (std::size_t i = 0; i < memberOf(history, side).size(); ++i)
                barrier = barrier || memberOf(memberOf(history, side).at(i), "trackDeletedAfter").asBoolean()
                          || memberOf(memberOf(history, side).at(i), "laterTrackDeletedAfter").asBoolean();
        if (!barrier)
            return "covers a vector of the undo history without a track deletion, which the edit runner covers";
    }
    const auto& given = memberOf(vector, "given");
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& name = given.keyAt(i);
        if (name == "dirty" || name == "clips" || name == "visibleTrackCount" || name == "audio")
            continue;
        if (name == "history") {
            for (const char* side : {"undo", "redo"})
                for (std::size_t e = 0; e < memberOf(given.at(i), side).size(); ++e)
                    if (memberOf(memberOf(given.at(i), side).at(e), "action").text() != "clipMove")
                        return "history action " + memberOf(memberOf(given.at(i), side).at(e), "action").text()
                               + " is not established by this runner";
            continue;
        }
        return "precondition given." + name + " is not established by this runner";
    }
    const auto& clips = memberOf(given, "clips");
    for (std::size_t c = 0; c < clips.size(); ++c)
        if (memberOf(clips.at(c), "analysis").text() != "ready")
            return "a clip with analysis " + memberOf(clips.at(c), "analysis").text()
                   + " is not established by this runner";
    return {};
}

std::string runVector(const JsonValue& vector)
{
    Fixture f;
    const auto& given = memberOf(vector, "given");
    const JsonValue* audio = given.find("audio");
    // A device runs unless the vector says that none is open.
    if (!f.open(audio == nullptr || memberOf(*audio, "running").asBoolean()))
        return "the control API did not start";
    if (const JsonValue* shown = given.find("visibleTrackCount"))
        f.bridge.visibleTracks = shown->isString() ? kSlots : static_cast<int>(shown->asDouble());
    VectorMatcher matcher;
    std::map<std::string, Clip> clips;
    std::vector<std::pair<std::string, ClipSpec>> specs;
    const auto& givenClips = memberOf(given, "clips");
    for (std::size_t i = 0; i < givenClips.size(); ++i) {
        const auto& item = givenClips.at(i);
        ClipSpec spec;
        spec.name = memberOf(item, "bind").text();
        spec.trackId = static_cast<int>(memberOf(item, "trackId").asDouble());
        spec.startTimelineSec = memberOf(item, "startTimelineSec").asDouble();
        spec.durationSec = memberOf(item, "durationSec").asDouble();
        const auto& notes = memberOf(item, "notes");
        for (std::size_t n = 0; n < notes.size(); ++n)
            spec.notes.push_back({memberOf(notes.at(n), "startContentSec").asDouble(),
                                  memberOf(notes.at(n), "endContentSec").asDouble(),
                                  memberOf(notes.at(n), "targetMidi").asDouble()});
        clips[spec.name] = fixtureAddClip(*f.processor, spec);
        specs.emplace_back(spec.name, spec);
    }
    CONTROL_CHECK(settleBackgroundWork(*f.processor));

    // given.history: the undo entries oldest first; the redo entries pushed from the top down and undone. A clip move
    // is recorded one second before the clip's start. trackDeletedAfter / laterTrackDeletedAfter: the entry is
    // recorded for the position the clip had before a track before it was deleted (one more), or for a clip whose
    // track stayed while a later track was deleted; the deletion is then reported to the history.
    std::optional<int> deletedTrack;
    const auto& history = memberOf(given, "history");
    const auto pushEntry = [&](const JsonValue& entry) -> bool {
        const auto clip = clips.find(memberOf(entry, "clip").text());
        const auto spec = std::find_if(specs.begin(), specs.end(), [&entry](const auto& item) {
            return item.first == memberOf(entry, "clip").text();
        });
        if (clip == clips.end() || spec == specs.end())
            return false;
        const bool before = memberOf(entry, "trackDeletedAfter").asBoolean();
        const bool later = memberOf(entry, "laterTrackDeletedAfter").asBoolean();
        const int actual = spec->second.trackId;
        const int recorded = actual + (before ? 1 : 0);
        if (before)
            deletedTrack = 0;
        else if (later && !deletedTrack)
            deletedTrack = actual + 1;
        const double oldStart = spec->second.startTimelineSec - 1.0;
        if (memberOf(entry, "origin").text() == "control") {
            auto action = clipMoveAction(f, clip->second, recorded, actual, oldStart);
            std::vector<ControlOriginAction::Inner> inner(1);
            const auto description = action->getDescription();
            inner[0].action = std::move(action);
            inner[0].name = "MultiMovePlacementAction";
            inner[0].content = clip->second.key;
            inner[0].tracks = {recorded};
            const auto undoId = f.service().history()->nextUndoId(f.instanceShort);
            f.processor->getUndoManager().addAction(
                std::make_unique<ControlOriginAction>(f.service().history(), undoId, description, std::move(inner)));
            if (const JsonValue* bind = entry.find("bind"))
                matcher.bindings[bind->text()] = JsonValue::string(undoId);
        } else {
            f.processor->getUndoManager().addAction(clipMoveAction(f, clip->second, recorded, actual, oldStart));
        }
        return true;
    };
    const auto& undoEntries = memberOf(history, "undo");
    for (std::size_t i = 0; i < undoEntries.size(); ++i)
        if (!pushEntry(undoEntries.at(i)))
            return "given.history.undo could not be established";
    const auto& redoEntries = memberOf(history, "redo");
    for (std::size_t i = redoEntries.size(); i-- > 0;)
        if (!pushEntry(redoEntries.at(i)))
            return "given.history.redo could not be established";
    for (std::size_t i = 0; i < redoEntries.size(); ++i)
        if (f.processor->getUndoManager().undo() == nullptr)
            return "given.history.redo could not be established";
    if (deletedTrack) {
        // The deletion of given.history happened before the state the vector describes, so one more track was shown
        // when it happened: track.delete removes only a shown track and shows one track fewer afterwards.
        const int visibleAtDeletion = std::min(kSlots, f.bridge.visibleTracks + 1);
        f.service().history()->onTrackDeleted(f.processor->getUndoManager(), *deletedTrack, visibleAtDeletion,
                                              f.processor->getStandaloneContentRepository()->getContentEpoch());
    }
    f.bridge.project.dirty = memberOf(given, "dirty").asBoolean();

    const auto& discovery = f.api.discovery();
    for (std::size_t i = 0; i < discovery.size(); ++i)
        matcher.bindings["discovery." + discovery.keyAt(i)] = discovery.at(i);
    const auto project = f.project();
    const auto& tracks = memberOf(project, "tracks");
    const int lastTrack =
        tracks.size() > 0 ? static_cast<int>(memberOf(tracks.at(tracks.size() - 1), "trackId").asDouble()) : 0;
    matcher.bindings["runner.lastTrackId"] = JsonValue::integer(lastTrack);
    matcher.bindings["runner.unknownTrackId"] = JsonValue::integer(lastTrack + 1);
    matcher.bindings["runner.arrangementRevision"] =
        JsonValue::string(memberOf(memberOf(project, "asOf"), "arrangementRevision").text());
    for (const auto& [bind, spec] : specs) {
        const auto handles = fixtureHandlesOf(project, spec.trackId, spec.startTimelineSec);
        if (!handles)
            return "the clip " + bind + " is not in project.get";
        matcher.bindings[bind + ".contentId"] = JsonValue::string(handles->contentId);
        matcher.bindings[bind + ".placementId"] = JsonValue::string(handles->placementId);
        matcher.bindings[bind + ".geometryHash"] = JsonValue::string(handles->geometryHash);
        for (const char* token : {"revision", "gridRevision", "keyRevision"})
            matcher.bindings[bind + "." + token] = memberOf(handles->tokens, token);
    }

    std::string failure;
    runWhilePumping(
        [&] {
            auto& connection = f.api.client();
            if (memberOf(vector, "type").text() == "request") {
                const auto request = matcher.substitute(memberOf(vector, "request"), failure);
                if (!failure.empty())
                    return;
                if (!connection.sendLine(toJsonString(request))) {
                    failure = "cannot send the request";
                    return;
                }
                failure = matcher.expectLine(connection, memberOf(vector, "response"));
                if (failure.empty() && memberOf(vector, "connectionAfter").text() == "open"
                    && !connection.staysQuiet(kOpenCheckMs))
                    failure = "the connection closed or sent more";
                return;
            }
            const auto& steps = memberOf(vector, "steps");
            for (std::size_t i = 0; i < steps.size() && failure.empty(); ++i) {
                const auto& step = steps.at(i);
                if (const JsonValue* send = step.find("send")) {
                    const auto value = matcher.substitute(*send, failure);
                    if (failure.empty() && !connection.sendLine(toJsonString(value)))
                        failure = "send failed";
                } else if (const JsonValue* pause = step.find("pauseMs")) {
                    juce::Thread::sleep(static_cast<int>(pause->asDouble()));
                } else {
                    failure = matcher.expectLine(connection, memberOf(step, "expect"));
                }
                if (!failure.empty())
                    failure = "step " + std::to_string(i + 1) + ": " + failure;
            }
        },
        kVectorExchangeMs);
    return failure;
}

void scenarioTrackVectorsImpl()
{
    const auto root = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors";
    std::vector<std::filesystem::path> files;
    for (const char* group : {"transport", "track", "edit"})
        for (const auto& entry : std::filesystem::directory_iterator(root / group))
            if (entry.path().extension() == ".json")
                files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::set<std::string> passedIds;
    int skipped = 0;
    VectorReport report("ControlProcessorTests.TrackVectors");
    for (const auto& file : files) {
        const auto parsed = parseJson(readFile(file));
        if (!CONTROL_CHECK(parsed.ok))
            continue;
        const auto id = memberOf(parsed.value, "id").text();
        if (const auto reason = vectorSkipReason(parsed.value); !reason.empty()) {
            ++skipped;
            report.skip(id, reason);
            note("SKIP " + id + ": " + reason);
            continue;
        }
        const auto started = Clock::now();
        const auto failure = runVector(parsed.value);
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty()))
            passedIds.insert(id);
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    for (const char* id : kMustRunVectors)
        CONTROL_CHECK(passedIds.count(id) == 1);
    note(std::to_string(passedIds.size()) + " vectors passed, " + std::to_string(skipped) + " skipped");
}

} // namespace

void scenarioTransport()
{
    scenarioTransportImpl();
}

void scenarioTracks()
{
    scenarioTracksImpl();
}

void scenarioTrackBarrier()
{
    scenarioTrackBarrierImpl();
}

void scenarioTrackVectors()
{
    scenarioTrackVectorsImpl();
}

} // namespace ControlTest
} // namespace OpenTune
