// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scenarios of the dispatcher and the read methods (session.info, project.get, content.getNotes,
// content.getPitchCurve, content.segmentNotes) against the real processor without a window or an audio
// device. Contents get a pitch analysis made of synthetic F0, committed the way OpenTune commits a
// finished analysis, so no model is needed. Requests go over a real session while the test thread, the
// message thread, pumps messages. Registered with the other processor scenarios in ControlProcessorTests.cpp.

#include "ControlContentFixtures.h"
#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneClipContent.h"
#include "Content/StandaloneContentRepository.h"
#include "Control/ControlDispatcher.h"
#include "Control/ControlMessageThread.h"
#include "Control/Ops/OpsRegistry.h"
#include "Utils/AppLogger.h"
#include "Utils/AppPreferences.h"
#include "Utils/DetectedKey.h"
#include "Utils/PitchCurve.h"
#include "Utils/PitchShiftSettings.h"
#include "Utils/PitchUtils.h"
#include "Utils/ProjectSession.h"
#include "Utils/TimeGrid.h"
#include "Utils/TuningConfig.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string_view>
#include <vector>

namespace OpenTune {
namespace ControlTest {

namespace {

/** Sample rate of the test audio (OpenTune stores audio at 44.1 kHz). */
constexpr double kAudioSampleRate = 44100.0;
/** Geometry of the synthetic pitch analysis, the one the F0 extractor produces: 16 kHz, hop 160. */
constexpr double kF0SampleRate = 16000.0;
constexpr int kF0HopSize = 160;
constexpr double kFrameSec = kF0HopSize / kF0SampleRate;
/** Pitch of the test audio, and of the analysis outside notes (A3). */
constexpr float kBaseHz = 220.0f;
/** Inside a note the detected pitch lies this far from the note's target, as a singer slightly flat. */
constexpr double kDetuneCents = -30.0;
/** The end of every analysed content is unvoiced (F0 0) for this long. */
constexpr double kUnvoicedTailSec = 0.5;
constexpr double kSecTolerance = 1e-6;
constexpr double kMidiTolerance = 1e-3;
/** A segment starts within this distance of the note the synthetic F0 holds (gap bridging, tails). */
constexpr double kSegmentEdgeTolerance = 0.06;
/** Number of analysis frames of a 6 s content. */
constexpr int kSixSecondFrames = 600;

// ---- Contents with a synthetic analysis --------------------------------------------------------

// NoteSpec, Analysis, ClipSpec and Clip: ControlContentFixtures.h.

float hzOf(double midi)
{
    return PitchUtils::midiToFreq(static_cast<float>(midi), TuningConfig::currentTuningHz());
}

double midiOf(float hz)
{
    return PitchUtils::freqToMidi(hz, TuningConfig::currentTuningHz());
}

double detectedMidiOf(double targetMidi)
{
    return targetMidi + kDetuneCents / 100.0;
}

juce::File juceFile(const std::filesystem::path& path)
{
    return juce::File(juce::String::fromUTF8(path.u8string().c_str()));
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream().release());
    if (stream == nullptr)
        return false;
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(kAudioSampleRate)
                             .withNumChannels(audio.getNumChannels())
                             .withBitsPerSample(16);
    auto writer = wav.createWriterFor(stream, options);
    return writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

/** F0 of a content: the base pitch, each note's detected pitch inside the note, an unvoiced tail. */
std::shared_ptr<PitchCurve> analysisOf(const ClipSpec& spec)
{
    const auto frames = static_cast<std::size_t>(std::llround(spec.durationSec / kFrameSec));
    std::vector<float> f0(frames, kBaseHz);
    for (const auto& note : spec.notes) {
        const auto first = static_cast<std::size_t>(std::ceil(note.startSec / kFrameSec - kSecTolerance));
        const auto end = static_cast<std::size_t>(std::ceil(note.endSec / kFrameSec - kSecTolerance));
        for (auto frame = first; frame < std::min(end, frames); ++frame)
            f0[frame] = hzOf(detectedMidiOf(note.targetMidi));
    }
    const auto tail = static_cast<std::size_t>(std::llround(kUnvoicedTailSec / kFrameSec));
    for (auto frame = frames > tail ? frames - tail : 0; frame < frames; ++frame)
        f0[frame] = 0.0f;
    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(kF0HopSize);
    curve->setSampleRate(kF0SampleRate);
    curve->setOriginalF0(f0);
    return curve;
}

std::vector<Note> notesOf(const ClipSpec& spec)
{
    std::vector<Note> notes;
    for (const auto& noteSpec : spec.notes) {
        Note note; // OpenTune's default note parameters
        note.startTime = noteSpec.startSec;
        note.endTime = noteSpec.endSec;
        note.pitch = hzOf(noteSpec.targetMidi);
        note.originalPitch = hzOf(detectedMidiOf(noteSpec.targetMidi));
        note.isVoiced = true;
        notes.push_back(note);
    }
    return notes;
}

/**
 * Imports a clip of spec.durationSec seconds of a voiced tone, written as a WAV file into the test's data
 * directory, the way the editor imports (prepareImport, then commitPreparedImportAsPlacement), and gives
 * its content the requested analysis state. A Ready analysis follows the steps of the F0 completion in
 * OpenTuneAudioProcessor::requestContentRefresh with a synthetic curve: the curve goes to the content
 * owner (StandaloneClipContent::applyOriginalF0, which the processor's private writeOriginalF0ToOwner
 * calls for a Standalone clip), then the notes and the key, then the state Ready.
 */
Clip addClip(OpenTuneAudioProcessor& processor, const ClipSpec& spec)
{
    Clip clip;
    const auto samples = static_cast<int>(std::llround(spec.durationSec * kAudioSampleRate));
    juce::AudioBuffer<float> audio(1, samples);
    for (int i = 0; i < samples; ++i)
        audio.setSample(
            0, i,
            0.25f
                * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * kBaseHz * i / kAudioSampleRate)));
    const auto media = isolatedDataDirectory() / "media";
    std::error_code ignored;
    std::filesystem::create_directories(media, ignored);
    const auto file = juceFile(media / (spec.name + ".wav"));
    CONTROL_CHECK(writeWav(file, audio));
    clip.sourcePath = file.getFullPathName().toStdString();

    OpenTuneAudioProcessor::PreparedImport prepared;
    if (!CONTROL_CHECK(processor.prepareImport(std::move(audio), kAudioSampleRate, juce::String(spec.name),
                                               file.getFullPathName(), prepared)))
        return clip;
    OpenTuneAudioProcessor::ImportPlacement placement;
    placement.trackId = spec.trackId;
    placement.timelineStartSeconds = spec.startTimelineSec;
    const auto committed = processor.commitPreparedImportAsPlacement(std::move(prepared), placement);
    if (!CONTROL_CHECK(committed.isValid()))
        return clip;
    clip.key = committed.contentKey;
    clip.placementId = committed.placementId;

    switch (spec.analysis) {
        case Analysis::NotRequested:
            break;
        case Analysis::Running:
            CONTROL_CHECK(processor.setContentOriginalF0State(clip.key, OriginalF0State::Extracting));
            break;
        case Analysis::Failed:
            CONTROL_CHECK(processor.setContentOriginalF0State(clip.key, OriginalF0State::Failed));
            break;
        case Analysis::Ready: {
            auto* owner = processor.getStandaloneContentRepository()->findClip(clip.key);
            if (!CONTROL_CHECK(owner != nullptr))
                return clip;
            owner->applyOriginalF0(analysisOf(spec));
            if (!spec.notes.empty())
                CONTROL_CHECK(processor.replaceContentNotesForFullMutation(clip.key, notesOf(spec)));
            if (spec.key)
                CONTROL_CHECK(processor.setContentDetectedKey(clip.key, *spec.key));
            if (spec.pitchShift) {
                // The edit state of a pitch shift: the settings with the notes (no correction segments).
                PitchShiftEditState state;
                state.settings = *spec.pitchShift;
                state.notes = notesOf(spec);
                CONTROL_CHECK(processor.applyContentPitchShiftState(clip.key, state));
            }
            if (!spec.gridHandlesContentSec.empty()) {
                // ClipStart, the inner handles, ClipEnd; ids count from 1, output times equal content times.
                std::vector<TimeHandle> handles;
                std::uint64_t nextId = 1;
                const auto add = [&handles, &nextId](double sec, HandleKind kind) {
                    TimeHandle handle;
                    handle.id = nextId++;
                    handle.source_seconds = sec;
                    handle.output_seconds = sec;
                    handle.kind = kind;
                    handles.push_back(handle);
                };
                add(0.0, HandleKind::ClipStart);
                for (const double sec : spec.gridHandlesContentSec) {
                    add(sec, HandleKind::UserAdded);
                    clip.handleIds.push_back(std::to_string(handles.back().id));
                }
                add(spec.durationSec, HandleKind::ClipEnd);
                owner->applyTimeGrid(TimeGridSnapshot::makeFromHandles(std::move(handles)));
            }
            CONTROL_CHECK(processor.setContentOriginalF0State(clip.key, OriginalF0State::Ready));
            break;
        }
    }
    return clip;
}

/** A sung clip: on track 0 from 2 s to 8 s, three notes, key A minor (set by hand). */
ClipSpec sungSpec()
{
    ClipSpec spec;
    spec.name = "sung";
    spec.trackId = 0;
    spec.startTimelineSec = 2.0;
    spec.notes = {{0.5, 1.5, 64.0}, {2.0, 3.0, 69.0}, {3.5, 4.5, 69.3}};
    DetectedKey key;
    key.root = Key::A;
    key.scale = Scale::Minor;
    key.origin = Origin::Manual;
    spec.key = key;
    return spec;
}

ClipSpec unanalysedSpec(const char* name, int trackId, double start, Analysis analysis)
{
    ClipSpec spec;
    spec.name = name;
    spec.trackId = trackId;
    spec.startTimelineSec = start;
    spec.durationSec = 2.0;
    spec.analysis = analysis;
    return spec;
}

/** Saves the project and opens it again with the editor's two-phase flows (ProjectSession::prepareSave,
    executeSaveToFile, prepareOpen, commitPreparedOpen): opening clears the content repository. */
bool saveAndReopen(OpenTuneAudioProcessor& processor, std::uint64_t renumberFrom = 0, std::uint64_t renumberTo = 0)
{
    ProjectSession session(processor, processor.getAppPreferences());
    const auto directory = isolatedDataDirectory() / "project";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    const auto file = juceFile(directory / "reads.otproj");
    auto task = session.prepareSave(file);
    if (!CONTROL_CHECK(ProjectSession::executeSaveToFile(task).ok()))
        return false;
    auto prepared = session.prepareOpen(file, Language::English);
    if (!CONTROL_CHECK(prepared.ok()))
        return false;
    if (renumberFrom == 0)
        return CONTROL_CHECK(session.commitPreparedOpen(std::move(prepared).value()).ok());
    // A project file stores a clip id as a signed 64-bit integer and OpenTune converts it back without a range check,
    // so a project can hold any id, the largest value (a stored -1) included. Give one clip of the opened project such
    // an id before it is committed, the way a file with that id would have.
    auto open = std::move(prepared).value();
    for (auto& content : open.snapshot.contents)
        if (content.contentKey.objectId == renumberFrom)
            content.contentKey.objectId = renumberTo;
    for (auto& track : open.snapshot.tracks)
        for (auto& placement : track.placements)
            if (placement.contentKey.objectId == renumberFrom)
                placement.contentKey.objectId = renumberTo;
    return CONTROL_CHECK(session.commitPreparedOpen(std::move(open)).ok());
}

// ---- Results ---------------------------------------------------------------------------------------

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

bool near(const JsonValue& value, double expected, double tolerance)
{
    return value.isNumber() && std::abs(value.asDouble() - expected) <= tolerance;
}

bool containsString(const JsonValue& array, const std::string& text)
{
    for (std::size_t i = 0; i < array.size(); ++i)
        if (array.at(i).isString() && array.at(i).text() == text)
            return true;
    return false;
}

const JsonValue& nullReason(const JsonValue& object, const char* member)
{
    return memberOf(memberOf(object, "nullReasons"), member);
}

// ClipHandles: ControlContentFixtures.h.

std::optional<ClipHandles> handlesOf(const JsonValue& project, int trackId, double startTimelineSec)
{
    const auto& placements = memberOf(project, "placements");
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const auto& placement = placements.at(i);
        if (memberOf(placement, "trackId").asDouble() != trackId
            || !near(memberOf(placement, "startTimelineSec"), startTimelineSec, kSecTolerance))
            continue;
        ClipHandles handles;
        handles.placement = placement;
        handles.contentId = memberOf(placement, "contentId").text();
        handles.placementId = memberOf(placement, "placementId").text();
        handles.geometryHash = memberOf(placement, "geometryHash").text();
        const auto& contents = memberOf(project, "contents");
        for (std::size_t c = 0; c < contents.size(); ++c)
            if (memberOf(contents.at(c), "contentId").text() == handles.contentId)
                handles.content = contents.at(c);
        const auto& tokens = memberOf(memberOf(project, "asOf"), "contents");
        for (std::size_t c = 0; c < tokens.size(); ++c)
            if (memberOf(tokens.at(c), "contentId").text() == handles.contentId)
                handles.tokens = tokens.at(c);
        return handles;
    }
    return std::nullopt;
}

/** The contentEpoch part of a handle. */
std::string epochOf(const std::string& handle)
{
    const auto first = handle.find(':', 2);
    const auto second = handle.find(':', first + 1);
    return handle.substr(first + 1, second - first - 1);
}

/** A handle prefix of an instance that is not this one. */
std::string otherInstanceShort(const std::string& instanceShort)
{
    std::string other = instanceShort;
    other[0] = other[0] == '0' ? '1' : '0';
    return other;
}

// ---- The scenario of the reads ---------------------------------------------------------------------

void checkSessionInfo(ApiSession& api)
{
    const auto info = api.result("session.info");
    CONTROL_CHECK(memberOf(info, "protocol").text() == "1.0");
    CONTROL_CHECK(memberOf(info, "instanceId").text() == memberOf(api.discovery(), "instanceId").text());
    CONTROL_CHECK(memberOf(info, "appVersion").text() == memberOf(api.discovery(), "appVersion").text());
    CONTROL_CHECK(memberOf(info, "flavor").text() == "standalone");
    CONTROL_CHECK(VectorMatcher::equal(memberOf(info, "build"), memberOf(api.discovery(), "build")));
    CONTROL_CHECK(memberOf(info, "sourceUrl").text() == memberOf(api.discovery(), "sourceUrl").text());
    CONTROL_CHECK(memberOf(memberOf(info, "runtime"), "pid").asDouble()
                  == static_cast<double>(controlCurrentProcessId()));
    // Only the capabilities this build implements completely are reported.
    CONTROL_CHECK(
        toJsonString(memberOf(info, "capabilities"))
        == R"(["session.v1","read.v1","jobs.v1","import.v1","export.v1","ui.bridge","ui.onboardingClose","ui.workspace","edit.v1","history.v1","timing.v1","transport.v1","arrangement.v1","tracks.v1","project.io","project.copy","prefs.write","audio.device","app.quit"])");

    // Without an editor the audio device is unknown; never omitted.
    const auto& device = memberOf(info, "audioDevice");
    CONTROL_CHECK(memberOf(device, "running").isBoolean() && !memberOf(device, "running").asBoolean());
    CONTROL_CHECK(memberOf(device, "name").isNull() && nullReason(device, "name").text() == "no_editor");
    CONTROL_CHECK(memberOf(device, "sampleRateHz").isNull()
                  && nullReason(device, "sampleRateHz").text() == "no_editor");

    const auto& processing = memberOf(info, "processing");
    CONTROL_CHECK(near(memberOf(processing, "tuningHz"), TuningConfig::currentTuningHz(), 1e-3));
    const auto scheme = memberOf(processing, "editingScheme").text();
    CONTROL_CHECK(scheme == "openTune" || scheme == "openDyne");
    CONTROL_CHECK(memberOf(processing, "f0Model").text() == "fcpe");
    CONTROL_CHECK(memberOf(processing, "vocoderReady").isBoolean() && memberOf(processing, "hybridMode").isBoolean());
    // The options are reported by prefs.get only.
    CONTROL_CHECK(processing.find("options") == nullptr);

    CONTROL_CHECK(containsString(memberOf(memberOf(info, "import"), "extensions"), ".wav"));
    CONTROL_CHECK(memberOf(memberOf(info, "import"), "maxChannels").asDouble() == 2);
    CONTROL_CHECK(
        toJsonString(memberOf(info, "export"))
        == R"({"formats":["wav"],"sampleRateHz":44100,"channels":{"clip":1,"track":1,"master":2},"sampleFormat":"float32"})");
    CONTROL_CHECK(memberOf(memberOf(info, "limits"), "dispatchTimeoutMs").asDouble()
                  == ControlLimits{}.dispatchTimeoutMs);
    // Not a read of project state: no asOf and no derived (PROTOCOL.md section 16 rule 5).
    CONTROL_CHECK(info.find("asOf") == nullptr && info.find("derived") == nullptr);
    CONTROL_CHECK(api.fails("session.info", json(R"({"verbose":true})"), "INVALID_ARGUMENT", "invalid_params"));
}

/** project.get without an editor; returns the result. */
JsonValue checkProjectWithoutEditor(ApiSession& api, OpenTuneAudioProcessor& processor, const Clip& sung)
{
    const auto project = api.result("project.get");
    const auto& asOf = memberOf(project, "asOf");
    CONTROL_CHECK(memberOf(asOf, "arrangementRevision").isString());
    CONTROL_CHECK(memberOf(asOf, "contents").size() == 4);
    CONTROL_CHECK(memberOf(project, "placements").size() == 4 && memberOf(project, "contents").size() == 4);

    // Values that belong to the editor: null with a reason, or computed and listed in derived.
    const auto& derived = memberOf(project, "derived");
    CONTROL_CHECK(memberOf(memberOf(project, "project"), "path").isNull());
    CONTROL_CHECK(!memberOf(memberOf(project, "project"), "dirty").asBoolean());
    CONTROL_CHECK(containsString(derived, "project.path") && containsString(derived, "project.dirty"));
    CONTROL_CHECK(containsString(derived, "visibleTrackCount") && containsString(derived, "tracks[].label"));
    CONTROL_CHECK(memberOf(project, "selection").isNull() && nullReason(project, "selection").text() == "no_editor");
    CONTROL_CHECK(memberOf(project, "history").isNull()
                  && nullReason(project, "history").text() == "capability_missing");
    // Track slots 0 to 2 hold clips, so at least three are visible.
    CONTROL_CHECK(memberOf(project, "visibleTrackCount").asDouble() == 3);

    const auto& tracks = memberOf(project, "tracks");
    if (CONTROL_CHECK(tracks.size() == static_cast<std::size_t>(processor.getStandaloneArrangement()->getNumTracks())
                      && tracks.size() >= 3)) {
        const auto& first = tracks.at(0);
        CONTROL_CHECK(memberOf(first, "trackId").asDouble() == 0 && memberOf(first, "label").text() == "Track 1");
        CONTROL_CHECK(memberOf(first, "placementCount").asDouble() == 1);
        CONTROL_CHECK(memberOf(tracks.at(1), "placementCount").asDouble() == 2);
        CONTROL_CHECK(memberOf(first, "visible").asBoolean());
        CONTROL_CHECK(memberOf(first, "rmsDb").isNull() && nullReason(first, "rmsDb").text() == "no_editor");
        CONTROL_CHECK(memberOf(first, "colour").text().size() == 7 && memberOf(first, "colour").text()[0] == '#');
        CONTROL_CHECK(memberOf(first, "volumeLinear").asDouble() >= 0);
    }

    const auto sungHandles = handlesOf(project, 0, 2.0);
    if (CONTROL_CHECK(sungHandles.has_value())) {
        const auto& placement = sungHandles->placement;
        const auto instanceShort = memberOf(api.discovery(), "instanceId").text().substr(0, 8);
        CONTROL_CHECK(sungHandles->contentId.rfind("c:" + instanceShort + ":", 0) == 0);
        CONTROL_CHECK(sungHandles->placementId.rfind("p:" + instanceShort + ":", 0) == 0);
        CONTROL_CHECK(near(memberOf(placement, "endTimelineSec"), 8.0, kSecTolerance));
        CONTROL_CHECK(near(memberOf(placement, "durationSec"), 6.0, kSecTolerance));
        CONTROL_CHECK(near(memberOf(placement, "clipInContentSec"), 0.0, kSecTolerance));
        CONTROL_CHECK(near(memberOf(placement, "gainDb"), 0.0, kSecTolerance));
        CONTROL_CHECK(near(memberOf(placement, "gainLinear"), 1.0, kSecTolerance));
        CONTROL_CHECK(placement.find("referencePlacementId") == nullptr);
        CONTROL_CHECK(memberOf(placement, "name").text() == "sung");
        CONTROL_CHECK(memberOf(placement, "sourcePath").text() == sung.sourcePath);
        CONTROL_CHECK(!sungHandles->geometryHash.empty());

        const auto& content = sungHandles->content;
        CONTROL_CHECK(memberOf(content, "f0State").text() == "ready");
        CONTROL_CHECK(toJsonString(memberOf(content, "key"))
                      == R"({"root":9,"scale":"minor","origin":"manual","confidence":null})");
        CONTROL_CHECK(memberOf(content, "noteCount").asDouble() == 3);
        CONTROL_CHECK(toJsonString(memberOf(content, "pitchShift")) == R"({"semitones":0,"cents":0})");
        CONTROL_CHECK(memberOf(content, "timeGridIdentity").asBoolean());
        CONTROL_CHECK(memberOf(sungHandles->tokens, "revision").isString()
                      && memberOf(sungHandles->tokens, "gridRevision").isString()
                      && memberOf(sungHandles->tokens, "keyRevision").isString());
    }
    // Contents without a finished analysis: the state says why, the key is null with a reason.
    const std::pair<std::pair<int, double>, const char*> states[] = {
        {{1, 0.0}, "notRequested"}, {{1, 3.0}, "extracting"}, {{2, 0.0}, "failed"}};
    for (const auto& [where, state] : states) {
        const auto handles = handlesOf(project, where.first, where.second);
        if (!CONTROL_CHECK(handles.has_value()))
            continue;
        CONTROL_CHECK(memberOf(handles->content, "f0State").text() == state);
        CONTROL_CHECK(memberOf(handles->content, "key").isNull()
                      && nullReason(handles->content, "key").text() == "not_analyzed");
        CONTROL_CHECK(memberOf(handles->content, "noteCount").asDouble() == 0);
    }

    const auto& transport = memberOf(project, "transport");
    CONTROL_CHECK(!memberOf(transport, "playing").asBoolean()
                  && !memberOf(transport, "audioDeviceRunning").asBoolean());
    CONTROL_CHECK(near(memberOf(transport, "bpm"), processor.getBpm(), 1e-3));
    CONTROL_CHECK(memberOf(memberOf(transport, "timeSignature"), "numerator").asDouble()
                  == processor.getTimeSigNumerator());
    CONTROL_CHECK(project.find("lastControl") == nullptr && transport.find("lastControl") != nullptr);
    return project;
}

/** project.get with an editor bridge: the project file, the selection and the view come from it. */
void checkProjectWithEditor(ApiSession& api, ControlService& service, const Clip& sung, const ClipHandles& handles)
{
    TestUiBridge bridge;
    bridge.project.projectPath = (isolatedDataDirectory() / "song.otproj").u8string();
    bridge.project.dirty = true;
    bridge.selection = {sung.placementId};
    bridge.view.workspace = "pianoRoll";
    bridge.view.pianoRollPlacementId = sung.placementId;
    bridge.visibleTracks = 4;
    service.setUiBridge(&bridge);
    const auto project = api.result("project.get");
    service.setUiBridge(nullptr);

    const auto& derived = memberOf(project, "derived");
    CONTROL_CHECK(memberOf(memberOf(project, "project"), "path").text() == bridge.project.projectPath);
    CONTROL_CHECK(memberOf(memberOf(project, "project"), "dirty").asBoolean());
    CONTROL_CHECK(!containsString(derived, "project.path") && !containsString(derived, "visibleTrackCount"));
    const auto& selection = memberOf(project, "selection");
    CONTROL_CHECK(toJsonString(memberOf(selection, "selectedPlacementIds")) == "[" + quoted(handles.placementId) + "]");
    CONTROL_CHECK(memberOf(selection, "pianoRollContentId").text() == handles.contentId);
    CONTROL_CHECK(nullReason(project, "selection").isNull());
    CONTROL_CHECK(memberOf(project, "visibleTrackCount").asDouble() == 4);
    const auto& tracks = memberOf(project, "tracks");
    if (tracks.size() > 4)
        CONTROL_CHECK(memberOf(tracks.at(3), "visible").asBoolean() && !memberOf(tracks.at(4), "visible").asBoolean());
    // A bridge without access to the audio device: unknown, not "no editor".
    CONTROL_CHECK(nullReason(tracks.at(0), "rmsDb").text() == "unavailable");
}

void checkNotes(ApiSession& api, OpenTuneAudioProcessor& processor, const Clip& sung, const ClipHandles& handles)
{
    const auto byContent = "\"contentId\":" + quoted(handles.contentId);
    const auto byPlacement = "\"placementId\":" + quoted(handles.placementId);
    const auto spec = sungSpec();
    const char* const labels[] = {"#0 E4 0.50-1.50s", "#1 A4 2.00-3.00s", "#2 A4 3.50-4.50s"};

    const auto all = api.result("content.getNotes", json("{" + byContent + "}"));
    CONTROL_CHECK(memberOf(all, "contentId").text() == handles.contentId && memberOf(all, "placementId").isNull());
    CONTROL_CHECK(toJsonString(memberOf(memberOf(all, "asOf"), "contents"))
                  == "[" + toJsonString(handles.tokens) + "]");
    CONTROL_CHECK(near(memberOf(all, "tuningHz"), TuningConfig::currentTuningHz(), 1e-3));
    CONTROL_CHECK(memberOf(all, "totalCount").asDouble() == 3 && memberOf(all, "nextCursor").isNull());
    CONTROL_CHECK(containsString(memberOf(all, "derived"), "notes[].label"));
    const auto& notes = memberOf(all, "notes");
    if (CONTROL_CHECK(notes.size() == 3))
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& note = notes.at(i);
            CONTROL_CHECK(memberOf(note, "index").asDouble() == static_cast<double>(i));
            CONTROL_CHECK(memberOf(note, "label").text() == labels[i]);
            CONTROL_CHECK(near(memberOf(note, "startContentSec"), spec.notes[i].startSec, kSecTolerance));
            CONTROL_CHECK(near(memberOf(note, "endContentSec"), spec.notes[i].endSec, kSecTolerance));
            CONTROL_CHECK(memberOf(note, "startTimelineSec").isNull() && memberOf(note, "endTimelineSec").isNull());
            CONTROL_CHECK(near(memberOf(note, "targetMidi"), spec.notes[i].targetMidi, kMidiTolerance));
            CONTROL_CHECK(
                near(memberOf(note, "detectedMidi"), detectedMidiOf(spec.notes[i].targetMidi), kMidiTolerance));
            CONTROL_CHECK(near(memberOf(note, "offsetCents"), 0.0, kMidiTolerance));
            CONTROL_CHECK(memberOf(note, "voiced").asBoolean() && !memberOf(note, "corrected").asBoolean());
            CONTROL_CHECK(memberOf(note, "retuneSpeed").isNumber() && memberOf(note, "vibratoRateHz").isNumber());
        }

    // By placement: timeline times through it, and a timeline range (4.0-4.6 s is content 2.0-2.6 s).
    const auto timeline = api.result(
        "content.getNotes", json("{" + byPlacement + R"(,"range":{"startTimelineSec":4,"endTimelineSec":4.6}})"));
    CONTROL_CHECK(memberOf(timeline, "placementId").text() == handles.placementId);
    CONTROL_CHECK(memberOf(timeline, "contentId").text() == handles.contentId);
    CONTROL_CHECK(memberOf(timeline, "totalCount").asDouble() == 1);
    if (CONTROL_CHECK(memberOf(timeline, "notes").size() == 1)) {
        const auto& note = memberOf(timeline, "notes").at(0);
        CONTROL_CHECK(memberOf(note, "index").asDouble() == 1);
        CONTROL_CHECK(near(memberOf(note, "startTimelineSec"), 4.0, kSecTolerance));
        CONTROL_CHECK(near(memberOf(note, "endTimelineSec"), 5.0, kSecTolerance));
    }
    // A content range keeps the notes that overlap it; indexes stay those of all notes.
    const auto ranged = api.result("content.getNotes",
                                   json("{" + byContent + R"(,"range":{"startContentSec":1.8,"endContentSec":3.6}})"));
    CONTROL_CHECK(memberOf(ranged, "totalCount").asDouble() == 2);
    if (CONTROL_CHECK(memberOf(ranged, "notes").size() == 2))
        CONTROL_CHECK(memberOf(memberOf(ranged, "notes").at(0), "index").asDouble() == 1
                      && memberOf(memberOf(ranged, "notes").at(1), "index").asDouble() == 2);

    // Pagination: one note per page, the cursor carries the position.
    std::vector<double> indexes;
    JsonValue cursor;
    for (int page = 0; page < 3; ++page) {
        const auto params =
            "{" + byContent + R"(,"limit":1)" + (cursor.isString() ? ",\"cursor\":" + quoted(cursor.text()) : "") + "}";
        const auto result = api.result("content.getNotes", json(params));
        CONTROL_CHECK(memberOf(result, "totalCount").asDouble() == 3 && memberOf(result, "notes").size() == 1);
        if (memberOf(result, "notes").size() == 1)
            indexes.push_back(memberOf(memberOf(result, "notes").at(0), "index").asDouble());
        cursor = memberOf(result, "nextCursor");
        CONTROL_CHECK(cursor.isString() == (page < 2));
    }
    CONTROL_CHECK((indexes == std::vector<double>{0, 1, 2}));

    // A cursor is bound to the selection params of its method and to the tokens of its read.
    const auto first = api.result("content.getNotes", json("{" + byContent + R"(,"limit":1})"));
    const auto next = memberOf(first, "nextCursor").text();
    CONTROL_CHECK(api.fails(
        "content.getNotes",
        json("{" + byContent + R"(,"range":{"startContentSec":0,"endContentSec":9},"cursor":)" + quoted(next) + "}"),
        "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("content.getPitchCurve", json("{" + byContent + ",\"cursor\":" + quoted(next) + "}"),
                            "INVALID_ARGUMENT", "invalid_params"));
    // The notes change on the message thread (as an edit in the piano roll would change them): stale.
    auto changed = notesOf(spec);
    changed[2].retuneSpeed = 0.25f;
    CONTROL_CHECK(processor.replaceContentNotesForFullMutation(sung.key, changed));
    CONTROL_CHECK(api.fails("content.getNotes", json("{" + byContent + ",\"cursor\":" + quoted(next) + "}"),
                            "REVISION_CONFLICT", "cursor_stale"));
    const auto after = api.result("content.getNotes", json("{" + byContent + "}"));
    const auto& afterTokens = memberOf(memberOf(after, "asOf"), "contents").at(0);
    CONTROL_CHECK(memberOf(afterTokens, "revision").text() != memberOf(handles.tokens, "revision").text());
    // A notes change does not change the key or the grid (PROTOCOL.md section 10 rule 2).
    CONTROL_CHECK(memberOf(afterTokens, "keyRevision").text() == memberOf(handles.tokens, "keyRevision").text());
    CONTROL_CHECK(memberOf(afterTokens, "gridRevision").text() == memberOf(handles.tokens, "gridRevision").text());
}

void checkPitchCurve(ApiSession& api, const ClipHandles& handles)
{
    const auto byContent = "\"contentId\":" + quoted(handles.contentId);
    const double baseMidi = midiOf(kBaseHz);
    const double noteMidi = detectedMidiOf(64.0);

    const auto full = api.result("content.getPitchCurve", json("{" + byContent + "}"));
    CONTROL_CHECK(memberOf(full, "f0State").text() == "ready");
    CONTROL_CHECK(near(memberOf(full, "hopSec"), kFrameSec, 1e-9) && memberOf(full, "stepFrame").asDouble() == 1);
    CONTROL_CHECK(near(memberOf(full, "startContentSec"), 0.0, kSecTolerance));
    CONTROL_CHECK(memberOf(full, "pointCount").asDouble() == kSixSecondFrames && memberOf(full, "nextCursor").isNull());
    const auto& original = memberOf(full, "originalMidi");
    const auto& corrected = memberOf(full, "correctedMidi");
    if (CONTROL_CHECK(original.size() == kSixSecondFrames && corrected.size() == kSixSecondFrames)) {
        CONTROL_CHECK(near(original.at(10), baseMidi, kMidiTolerance));
        CONTROL_CHECK(near(original.at(60), noteMidi, kMidiTolerance));
        // Nothing is corrected: the rendered pitch is the original one.
        CONTROL_CHECK(near(corrected.at(60), noteMidi, kMidiTolerance)
                      && near(corrected.at(10), baseMidi, kMidiTolerance));
        // The unvoiced tail has no pitch on either curve.
        CONTROL_CHECK(original.at(kSixSecondFrames - 1).isNull() && corrected.at(kSixSecondFrames - 1).isNull());
    }
    CONTROL_CHECK(containsString(memberOf(full, "derived"), "originalMidi"));

    // Every 10th frame, and a range of 0.1 s at full resolution.
    const auto sparse = api.result("content.getPitchCurve", json("{" + byContent + R"(,"stepFrame":10})"));
    CONTROL_CHECK(memberOf(sparse, "pointCount").asDouble() == kSixSecondFrames / 10);
    CONTROL_CHECK(near(memberOf(sparse, "originalMidi").at(6), noteMidi, kMidiTolerance));
    const auto range = api.result("content.getPitchCurve",
                                  json("{" + byContent + R"(,"range":{"startContentSec":0.5,"endContentSec":0.6}})"));
    CONTROL_CHECK(near(memberOf(range, "startContentSec"), 0.5, kSecTolerance));
    CONTROL_CHECK(memberOf(range, "pointCount").asDouble() == 10);

    // Pages of 250 values: 250, 250, 100; each page starts where the previous one ended.
    JsonValue cursor;
    std::vector<double> counts;
    std::vector<double> starts;
    for (int page = 0; page < 3; ++page) {
        const auto params = "{" + byContent + R"(,"limit":250)"
                            + (cursor.isString() ? ",\"cursor\":" + quoted(cursor.text()) : "") + "}";
        const auto result = api.result("content.getPitchCurve", json(params));
        counts.push_back(memberOf(result, "pointCount").asDouble());
        starts.push_back(std::round(memberOf(result, "startContentSec").asDouble() * 1000.0) / 1000.0);
        cursor = memberOf(result, "nextCursor");
    }
    CONTROL_CHECK((counts == std::vector<double>{250, 250, 100}) && cursor.isNull());
    CONTROL_CHECK((starts == std::vector<double>{0.0, 2.5, 5.0}));
    // The cursor is bound to stepFrame, a selection param.
    const auto first = api.result("content.getPitchCurve", json("{" + byContent + R"(,"limit":250})"));
    CONTROL_CHECK(api.fails(
        "content.getPitchCurve",
        json("{" + byContent + R"(,"stepFrame":2,"cursor":)" + quoted(memberOf(first, "nextCursor").text()) + "}"),
        "INVALID_ARGUMENT", "invalid_params"));
}

void checkSegmentNotes(ApiSession& api, const ClipHandles& handles)
{
    const auto byContent = "\"contentId\":" + quoted(handles.contentId);
    const auto before = api.result("content.getNotes", json("{" + byContent + "}"));
    const auto result = api.result("content.segmentNotes", json("{" + byContent + R"(,"noteSplitCents":80})"));
    CONTROL_CHECK(memberOf(result, "f0State").text() == "ready" && near(memberOf(result, "noteSplitCents"), 80, 1e-9));
    const auto& segments = memberOf(result, "segments");
    CONTROL_CHECK(segments.size() >= 3);
    bool foundFirstNote = false;
    bool foundSecondNote = false;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments.at(i);
        const double start = memberOf(segment, "startContentSec").asDouble();
        const double end = memberOf(segment, "endContentSec").asDouble();
        const double midi = memberOf(segment, "detectedMidi").asDouble();
        CONTROL_CHECK(start >= 0.0 && end > start && end <= 6.0 + kSecTolerance);
        foundFirstNote =
            foundFirstNote
            || (std::abs(midi - detectedMidiOf(64.0)) < 0.05 && std::abs(start - 0.5) < kSegmentEdgeTolerance);
        foundSecondNote =
            foundSecondNote
            || (std::abs(midi - detectedMidiOf(69.0)) < 0.05 && std::abs(start - 2.0) < kSegmentEdgeTolerance);
    }
    CONTROL_CHECK(foundFirstNote && foundSecondNote);
    // Nothing is stored: the tokens of the content stay as they were.
    CONTROL_CHECK(toJsonString(memberOf(result, "asOf")) == toJsonString(memberOf(before, "asOf")));
    const auto after = api.result("content.getNotes", json("{" + byContent + "}"));
    CONTROL_CHECK(toJsonString(memberOf(after, "asOf")) == toJsonString(memberOf(before, "asOf")));

    // Without noteSplitCents the generator's default is used and reported; a range keeps overlapping segments.
    const auto ranged = api.result("content.segmentNotes",
                                   json("{" + byContent + R"(,"range":{"startContentSec":2,"endContentSec":3}})"));
    CONTROL_CHECK(near(memberOf(ranged, "noteSplitCents"), PitchControlConfig::kDefaultNoteSplitCents, 1e-9));
    const auto& kept = memberOf(ranged, "segments");
    CONTROL_CHECK(kept.size() >= 1);
    for (std::size_t i = 0; i < kept.size(); ++i)
        CONTROL_CHECK(memberOf(kept.at(i), "endContentSec").asDouble() > 2.0
                      && memberOf(kept.at(i), "startContentSec").asDouble() < 3.0);
}

/** Reads are not gated (PROTOCOL.md section 11): without a finished analysis the arrays are empty and
    f0State says why. */
void checkUnanalysed(ApiSession& api, const JsonValue& project)
{
    const std::pair<std::pair<int, double>, const char*> states[] = {
        {{1, 0.0}, "notRequested"}, {{1, 3.0}, "extracting"}, {{2, 0.0}, "failed"}};
    for (const auto& [where, state] : states) {
        const auto handles = handlesOf(project, where.first, where.second);
        if (!CONTROL_CHECK(handles.has_value()))
            continue;
        const auto byContent = "{\"contentId\":" + quoted(handles->contentId) + "}";
        const auto curve = api.result("content.getPitchCurve", json(byContent));
        CONTROL_CHECK(memberOf(curve, "f0State").text() == state);
        CONTROL_CHECK(memberOf(curve, "pointCount").asDouble() == 0 && memberOf(curve, "originalMidi").size() == 0
                      && memberOf(curve, "correctedMidi").size() == 0 && memberOf(curve, "nextCursor").isNull());
        CONTROL_CHECK(memberOf(curve, "hopSec").asDouble() > 0 && containsString(memberOf(curve, "derived"), "hopSec"));
        const auto segments = api.result("content.segmentNotes", json(byContent));
        CONTROL_CHECK(memberOf(segments, "f0State").text() == state && memberOf(segments, "segments").size() == 0);
        const auto notes = api.result("content.getNotes", json(byContent));
        CONTROL_CHECK(memberOf(notes, "totalCount").asDouble() == 0 && memberOf(notes, "notes").size() == 0);
    }
}

/** Errors of the checks before a method runs; every one has executed false. */
void checkErrors(ApiSession& api, const ClipHandles& handles)
{
    const auto instanceShort = memberOf(api.discovery(), "instanceId").text().substr(0, 8);
    const auto epoch = epochOf(handles.contentId);
    const auto unknownContent = quoted("c:" + instanceShort + ":" + epoch + ":4294967295");
    const auto unknownPlacement = quoted("p:" + instanceShort + ":" + epoch + ":4294967295");
    const auto otherContent = quoted("c:" + otherInstanceShort(instanceShort) + ":" + epoch + ":1");
    const auto otherEpoch = quoted("c:" + instanceShort + ":" + std::to_string(std::stoull(epoch) + 1) + ":1");
    const auto content = quoted(handles.contentId);
    const auto placement = quoted(handles.placementId);

    CONTROL_CHECK(
        api.fails("content.getNotes", json("{\"contentId\":" + unknownContent + "}"), "NOT_FOUND", "unknown_handle"));
    CONTROL_CHECK(api.fails("content.getNotes", json("{\"placementId\":" + unknownPlacement + "}"), "NOT_FOUND",
                            "unknown_handle"));
    CONTROL_CHECK(
        api.fails("content.getPitchCurve", json("{\"contentId\":" + otherContent + "}"), "NOT_FOUND", "stale_handle"));
    CONTROL_CHECK(
        api.fails("content.segmentNotes", json("{\"contentId\":" + otherEpoch + "}"), "NOT_FOUND", "stale_handle"));
    // A handle of one kind where the other is expected fails the form check (PROTOCOL.md section 9 rule 1).
    CONTROL_CHECK(
        api.fails("content.getNotes", json("{\"contentId\":" + placement + "}"), "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(
        api.fails("content.getNotes", json("{\"placementId\":" + content + "}"), "INVALID_ARGUMENT", "invalid_params"));
    const char* const invalidNotes[] = {
        "{}",
        R"({"contentId":CONTENT,"placementId":PLACEMENT})",
        R"({"contentId":CONTENT,"range":{"startTimelineSec":1,"endTimelineSec":2}})",
        R"({"contentId":CONTENT,"range":{"startContentSec":1}})",
        R"({"contentId":CONTENT,"range":{"startContentSec":-1,"endContentSec":2}})",
        R"({"contentId":CONTENT,"limit":0})",
        R"({"contentId":CONTENT,"limit":501})",
        R"({"contentId":CONTENT,"limit":1.5})",
        R"({"contentId":CONTENT,"cursor":""})",
        R"({"contentId":CONTENT,"cursor":"not-a-cursor"})",
        R"({"contentId":CONTENT,"verbose":true})",
    };
    for (std::string params : invalidNotes) {
        for (const auto& [name, value] :
             {std::pair<std::string, std::string>{"CONTENT", content}, {"PLACEMENT", placement}})
            for (auto at = params.find(name); at != std::string::npos; at = params.find(name))
                params.replace(at, name.size(), value);
        CONTROL_CHECK(api.fails("content.getNotes", json(params), "INVALID_ARGUMENT", "invalid_params"));
    }
    CONTROL_CHECK(api.fails("content.getPitchCurve", json("{\"contentId\":" + content + ",\"stepFrame\":0}"),
                            "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("content.getPitchCurve", json("{\"contentId\":" + content + ",\"limit\":10001}"),
                            "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("content.getPitchCurve", json("{\"placementId\":" + placement + "}"), "INVALID_ARGUMENT",
                            "invalid_params"));
    CONTROL_CHECK(api.fails("content.segmentNotes", json("{\"contentId\":" + content + ",\"noteSplitCents\":201}"),
                            "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("project.get", json(R"({"verbose":true})"), "INVALID_ARGUMENT", "invalid_params"));
    // A method of the protocol with a closed params schema, and a method the protocol does not define.
    CONTROL_CHECK(api.fails("prefs.set", JsonValue::object(), "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("track.rename", JsonValue::object(), "UNSUPPORTED", "unknown_method"));
}

/** Opening a project discards every content: the handles from before are stale, the new ones work. */
void checkReopen(ApiSession& api, OpenTuneAudioProcessor& processor, const ClipHandles& before)
{
    const auto epochBefore = processor.getStandaloneContentRepository()->getContentEpoch();
    if (!saveAndReopen(processor))
        return;
    CONTROL_CHECK(processor.getStandaloneContentRepository()->getContentEpoch() == epochBefore + 1);
    CONTROL_CHECK(api.fails("content.getNotes", json("{\"contentId\":" + quoted(before.contentId) + "}"), "NOT_FOUND",
                            "stale_handle"));
    CONTROL_CHECK(api.fails("content.getNotes", json("{\"placementId\":" + quoted(before.placementId) + "}"),
                            "NOT_FOUND", "stale_handle"));
    const auto project = api.result("project.get");
    const auto after = handlesOf(project, 0, 2.0);
    if (!CONTROL_CHECK(after.has_value()))
        return;
    CONTROL_CHECK(after->contentId != before.contentId && after->placementId != before.placementId);
    CONTROL_CHECK(epochOf(after->contentId) == std::to_string(epochBefore + 1));
    const auto notes = api.result("content.getNotes", json("{\"contentId\":" + quoted(after->contentId) + "}"));
    CONTROL_CHECK(memberOf(notes, "contentId").text() == after->contentId);
}

// ---- content.getDetails, transport.get, prefs.get and app.getLog -----------------------------------

std::set<std::string> memberNames(const JsonValue& object)
{
    std::set<std::string> names;
    for (std::size_t i = 0; i < object.size(); ++i)
        names.insert(object.keyAt(i));
    return names;
}

/** The clips of the details scenario. */
struct DetailsClips
{
    Clip sung;
    Clip ref;
    Clip failedEmpty;
    Clip failedPlain;
    Clip pending;
};

ClipSpec refSpec()
{
    ClipSpec spec;
    spec.name = "ref";
    spec.trackId = 1;
    spec.startTimelineSec = 2.0;
    return spec;
}

void checkDetailsOfSung(ApiSession& api, OpenTuneAudioProcessor& processor, const DetailsClips& clips,
                        const ClipHandles& handles, const ClipHandles& refHandles)
{
    const auto byContent = json("{\"contentId\":" + quoted(handles.contentId) + "}");
    const auto details = api.result("content.getDetails", byContent);
    CONTROL_CHECK(memberOf(details, "contentId").text() == handles.contentId);
    CONTROL_CHECK(memberOf(details, "viaPlacementId").text() == handles.placementId);
    CONTROL_CHECK(toJsonString(memberOf(details, "placementIds")) == "[" + quoted(handles.placementId) + "]");
    CONTROL_CHECK(near(memberOf(details, "durationSec"), 6.0, kSecTolerance));
    CONTROL_CHECK(toJsonString(memberOf(details, "key"))
                  == R"({"root":9,"scale":"minor","origin":"manual","confidence":null})");
    CONTROL_CHECK(toJsonString(memberOf(details, "pitchShift")) == R"({"semitones":-2,"cents":0})");
    // The tokens are those of project.get, for this content alone.
    CONTROL_CHECK(toJsonString(memberOf(memberOf(details, "asOf"), "contents"))
                  == "[" + toJsonString(handles.tokens) + "]");
    CONTROL_CHECK(containsString(memberOf(details, "derived"), "durationSec"));

    const auto& analysis = memberOf(details, "analysis");
    CONTROL_CHECK(toJsonString(memberOf(analysis, "f0State")) == "\"ready\"" && memberOf(analysis, "failure").isNull());
    CONTROL_CHECK(near(memberOf(analysis, "hopSec"), kFrameSec, 1e-9));
    CONTROL_CHECK(!containsString(memberOf(details, "derived"), "analysis.hopSec"));

    // The render: the token of what the render is made from, and a published token that equals it exactly
    // when the render is current.
    const auto& render = memberOf(details, "render");
    CONTROL_CHECK(memberOf(render, "current").isBoolean() && memberOf(render, "desiredRevision").isString());
    CONTROL_CHECK((memberOf(render, "publishedRevision").text() == memberOf(render, "desiredRevision").text())
                  == memberOf(render, "current").asBoolean());

    const auto& grid = memberOf(details, "timeGrid");
    CONTROL_CHECK(memberOf(grid, "seeded").asBoolean() && memberOf(grid, "identity").asBoolean());
    CONTROL_CHECK(near(memberOf(grid, "minHandleSpacingSec"), 0.03, 1e-9));
    const auto& gridHandles = memberOf(grid, "handles");
    if (CONTROL_CHECK(gridHandles.size() == 2 && clips.sung.handleIds.size() == 2)) {
        CONTROL_CHECK(memberOf(gridHandles.at(0), "handleId").text() == clips.sung.handleIds[0]);
        CONTROL_CHECK(near(memberOf(gridHandles.at(0), "contentSec"), 1.0, kSecTolerance)
                      && near(memberOf(gridHandles.at(0), "outputSec"), 1.0, kSecTolerance));
        CONTROL_CHECK(memberOf(gridHandles.at(1), "handleId").text() == clips.sung.handleIds[1]);
        CONTROL_CHECK(near(memberOf(gridHandles.at(1), "contentSec"), 2.5, kSecTolerance));
    }

    // The reference section is retired: the result has no such member.
    CONTROL_CHECK(details.find("reference") == nullptr);

    // The scheme is openTune, so the button regenerates (checkAutoModes has the rest).
    const auto& modes = memberOf(details, "autoModes");
    CONTROL_CHECK(memberOf(modes, "editingScheme").text() == "openTune"
                  && memberOf(modes, "uiButtonRuns").text() == "regenerate");
    CONTROL_CHECK(toJsonString(memberOf(modes, "available")) == R"(["regenerate","snapNotes","notesOnly"])");

    // By placement: the same content, through that placement; every member besides the placement the same.
    const auto byPlacement =
        api.result("content.getDetails", json("{\"placementId\":" + quoted(handles.placementId) + "}"));
    CONTROL_CHECK(toJsonString(byPlacement) == toJsonString(details));
    // The second clip is a content of its own (its button regenerates too) and has no time grid.
    const auto refDetails =
        api.result("content.getDetails", json("{\"contentId\":" + quoted(refHandles.contentId) + "}"));
    CONTROL_CHECK(memberOf(memberOf(refDetails, "autoModes"), "uiButtonRuns").text() == "regenerate");
    CONTROL_CHECK(!memberOf(memberOf(refDetails, "timeGrid"), "seeded").asBoolean()
                  && memberOf(memberOf(refDetails, "timeGrid"), "handles").size() == 0
                  && memberOf(memberOf(refDetails, "timeGrid"), "identity").asBoolean());
    CONTROL_CHECK(memberOf(refDetails, "key").isNull() && nullReason(refDetails, "key").text() == "not_analyzed");

    // The render input changes with an edit and with nothing else.
    const auto desiredBefore = memberOf(render, "desiredRevision").text();
    CONTROL_CHECK(memberOf(memberOf(api.result("content.getDetails", byContent), "render"), "desiredRevision").text()
                  == desiredBefore);
    auto changed = notesOf(sungSpec());
    changed[0].retuneSpeed = 0.5f;
    CONTROL_CHECK(processor.replaceContentNotesForFullMutation(clips.sung.key, changed));
    CONTROL_CHECK(memberOf(memberOf(api.result("content.getDetails", byContent), "render"), "desiredRevision").text()
                  != desiredBefore);
}

/**
 * The AUTO button (autoModes.uiButtonRuns): the mode the editor's button runs now. In the openDyne scheme it snaps
 * notes; in the openTune scheme it regenerates. OpenTune no longer has the reference alignment, so the button never
 * runs it, whatever the experimental features say, and "reference" is never reported.
 */
void checkAutoModes(ApiSession& api, OpenTuneAudioProcessor& processor, const ClipHandles& handles)
{
    const auto byContent = json("{\"contentId\":" + quoted(handles.contentId) + "}");
    const auto modesOf = [&] { return memberOf(api.result("content.getDetails", byContent), "autoModes"); };
    const std::string offered = R"(["regenerate","snapNotes","notesOnly"])";
    auto& preferences = processor.getAppPreferences();

    // openTune scheme: regenerate, with the experimental features off and on.
    auto modes = modesOf();
    CONTROL_CHECK(memberOf(modes, "editingScheme").text() == "openTune"
                  && memberOf(modes, "uiButtonRuns").text() == "regenerate");
    CONTROL_CHECK(toJsonString(memberOf(modes, "available")) == offered);
    preferences.setExperimentalFeaturesEnabled(true);
    modes = modesOf();
    CONTROL_CHECK(memberOf(modes, "uiButtonRuns").text() == "regenerate");
    CONTROL_CHECK(toJsonString(memberOf(modes, "available")) == offered);
    preferences.setExperimentalFeaturesEnabled(false);

    // Through the placement of the clip it is the same.
    CONTROL_CHECK(memberOf(memberOf(api.result("content.getDetails",
                                               json("{\"placementId\":" + quoted(handles.placementId) + "}")),
                                    "autoModes"),
                           "uiButtonRuns")
                      .text()
                  == "regenerate");

    // The openDyne scheme snaps notes.
    preferences.setAudioEditingScheme(AudioEditingScheme::Scheme::NotesPrimary);
    modes = modesOf();
    CONTROL_CHECK(memberOf(modes, "editingScheme").text() == "openDyne"
                  && memberOf(modes, "uiButtonRuns").text() == "snapNotes");
    CONTROL_CHECK(toJsonString(memberOf(modes, "available")) == offered);
    preferences.setAudioEditingScheme(AudioEditingScheme::Scheme::CorrectedF0Primary);
    CONTROL_CHECK(memberOf(modesOf(), "uiButtonRuns").text() == "regenerate");
}

/** Analysis states: the failure reason the processor recorded, and what a content without analysis offers. */
void checkDetailsOfUnanalysed(ApiSession& api, OpenTuneAudioProcessor& processor, const DetailsClips& clips,
                              const JsonValue& project)
{
    const auto detailsOf = [&](int trackId) {
        const auto handles = handlesOf(project, trackId, 0.0);
        if (!CONTROL_CHECK(handles.has_value()))
            return JsonValue();
        return api.result("content.getDetails", json("{\"contentId\":" + quoted(handles->contentId) + "}"));
    };
    // A failure with the message of an analysis without voiced frames says so; one without a message (loaded from a
    // project) or with another one is f0_failed.
    const auto empty = detailsOf(2);
    CONTROL_CHECK(memberOf(memberOf(empty, "analysis"), "f0State").text() == "failed");
    CONTROL_CHECK(memberOf(memberOf(empty, "analysis"), "failure").text() == "f0_empty");
    const auto plain = detailsOf(3);
    CONTROL_CHECK(memberOf(memberOf(plain, "analysis"), "f0State").text() == "failed");
    CONTROL_CHECK(memberOf(memberOf(plain, "analysis"), "failure").text() == "f0_failed");
    for (const auto& details : {empty, plain}) {
        CONTROL_CHECK(memberOf(details, "key").isNull() && nullReason(details, "key").text() == "not_analyzed");
        CONTROL_CHECK(memberOf(memberOf(details, "autoModes"), "available").size() == 0);
        // Without a pitch curve the hop is the analysis' own, and the result says it computed it.
        CONTROL_CHECK(near(memberOf(memberOf(details, "analysis"), "hopSec"), kFrameSec, 1e-9));
        CONTROL_CHECK(containsString(memberOf(details, "derived"), "analysis.hopSec"));
    }
    const auto pending = detailsOf(4);
    CONTROL_CHECK(memberOf(memberOf(pending, "analysis"), "f0State").text() == "notRequested");
    CONTROL_CHECK(memberOf(memberOf(pending, "analysis"), "failure").isNull());
    CONTROL_CHECK(memberOf(memberOf(pending, "autoModes"), "available").size() == 0);

    // OpenTune keeps the failure message with the content, clears it when the analysis state changes (an analysis
    // starts or succeeds), and the reason code is reported only while the analysis is failed.
    const auto messageOf = [&processor](const ContentKey& key) {
        const auto snapshot = processor.getContentSnapshot(key);
        return snapshot != nullptr ? snapshot->originalF0FailureReason : std::string("<no content>");
    };
    CONTROL_CHECK(messageOf(clips.failedEmpty.key) == kUnvoicedF0FailureMessage);
    CONTROL_CHECK(messageOf(clips.failedPlain.key).empty());
    CONTROL_CHECK(processor.setContentOriginalF0FailureReason(clips.failedEmpty.key, kOtherF0FailureMessage));
    CONTROL_CHECK(memberOf(memberOf(detailsOf(2), "analysis"), "failure").text() == "f0_failed");
    CONTROL_CHECK(processor.setContentOriginalF0FailureReason(clips.failedEmpty.key, kUnvoicedF0FailureMessage));
    CONTROL_CHECK(memberOf(memberOf(detailsOf(2), "analysis"), "failure").text() == "f0_empty");
    CONTROL_CHECK(processor.setContentOriginalF0State(clips.failedEmpty.key, OriginalF0State::Extracting));
    CONTROL_CHECK(memberOf(memberOf(detailsOf(2), "analysis"), "failure").isNull());
    CONTROL_CHECK(memberOf(memberOf(detailsOf(2), "analysis"), "f0State").text() == "extracting");
    CONTROL_CHECK(messageOf(clips.failedEmpty.key).empty());
    CONTROL_CHECK(failAnalysis(processor, clips.failedEmpty.key, true));
}

void checkDetailsErrors(ApiSession& api, const ClipHandles& handles)
{
    const auto instanceShort = memberOf(api.discovery(), "instanceId").text().substr(0, 8);
    const auto epoch = epochOf(handles.contentId);
    const auto content = quoted(handles.contentId);
    const auto placement = quoted(handles.placementId);
    CONTROL_CHECK(api.fails("content.getDetails",
                            json("{\"contentId\":" + quoted("c:" + instanceShort + ":" + epoch + ":4294967295") + "}"),
                            "NOT_FOUND", "unknown_handle"));
    CONTROL_CHECK(
        api.fails("content.getDetails",
                  json("{\"placementId\":" + quoted("p:" + instanceShort + ":" + epoch + ":4294967295") + "}"),
                  "NOT_FOUND", "unknown_handle"));
    CONTROL_CHECK(
        api.fails("content.getDetails",
                  json("{\"contentId\":" + quoted("c:" + otherInstanceShort(instanceShort) + ":" + epoch + ":1") + "}"),
                  "NOT_FOUND", "stale_handle"));
    CONTROL_CHECK(api.fails(
        "content.getDetails",
        json("{\"placementId\":" + quoted("p:" + otherInstanceShort(instanceShort) + ":" + epoch + ":1") + "}"),
        "NOT_FOUND", "stale_handle"));
    for (const std::string& params :
         {std::string("{}"), "{\"contentId\":" + content + ",\"placementId\":" + placement + "}",
          "{\"contentId\":" + placement + "}", "{\"placementId\":" + content + "}",
          "{\"contentId\":" + content + ",\"verbose\":true}"})
        CONTROL_CHECK(api.fails("content.getDetails", json(params), "INVALID_ARGUMENT", "invalid_params"));
}

void checkTransport(ApiSession& api, ControlService& service, const JsonValue& project)
{
    const auto result = api.result("transport.get");
    // Not a read of a content: asOf lists no content, and nothing is derived.
    CONTROL_CHECK(memberOf(memberOf(result, "asOf"), "contents").size() == 0);
    CONTROL_CHECK(memberOf(memberOf(result, "asOf"), "arrangementRevision").text()
                  == memberOf(memberOf(project, "asOf"), "arrangementRevision").text());
    CONTROL_CHECK(memberOf(result, "derived").isArray() && memberOf(result, "derived").size() == 0);
    CONTROL_CHECK(toJsonString(memberOf(result, "transport")) == toJsonString(memberOf(project, "transport")));
    const auto names = memberNames(memberOf(result, "transport"));
    CONTROL_CHECK((names
                   == std::set<std::string>{"playing", "positionTimelineSec", "playStartTimelineSec", "loopEnabled",
                                            "bpm", "timeSignature", "audioDeviceRunning", "lastControl"}));
    CONTROL_CHECK(!memberOf(memberOf(result, "transport"), "audioDeviceRunning").asBoolean());
    CONTROL_CHECK(api.fails("transport.get", json(R"({"verbose":true})"), "INVALID_ARGUMENT", "invalid_params"));

    // With a bridge that cannot see the audio device the transport still reports the device as not running.
    TestUiBridge bridge;
    service.setUiBridge(&bridge);
    const auto withBridge = api.result("transport.get");
    service.setUiBridge(nullptr);
    CONTROL_CHECK(!memberOf(memberOf(withBridge, "transport"), "audioDeviceRunning").asBoolean());
}

void checkPrefs(ApiSession& api, OpenTuneAudioProcessor& processor)
{
    auto& preferences = processor.getAppPreferences();
    const auto media = isolatedDataDirectory() / "prefs";
    std::error_code ignored;
    std::filesystem::create_directories(media, ignored);
    const auto existing = juceFile(media / "recent-a.otproj");
    CONTROL_CHECK(existing.replaceWithText("project"));
    const auto missing = juceFile(media / "recent-missing.otproj");
    missing.deleteFile();
    preferences.clearRecentProjects();
    preferences.pushRecentProject(existing.getFullPathName());
    preferences.pushRecentProject(missing.getFullPathName());
    preferences.setExperimentalFeaturesEnabled(true);
    preferences.setAudioEditingScheme(AudioEditingScheme::Scheme::NotesPrimary);

    const auto result = api.result("prefs.get");
    // Preferences are not project state.
    CONTROL_CHECK(result.find("asOf") == nullptr && result.find("derived") == nullptr);
    CONTROL_CHECK(
        (memberNames(result) == std::set<std::string>{"processing", "onboardingShown", "language", "recentProjects"}));
    const auto& processing = memberOf(result, "processing");
    CONTROL_CHECK((memberNames(processing)
                       == std::set<std::string>{"tuningHz", "editingScheme", "hybridMode", "vocoderWeight", "f0Model",
                                                "renderingPriority", "experimentalFeatures", "options", "nullReasons"}
                   || memberNames(processing)
                          == std::set<std::string>{"tuningHz", "editingScheme", "hybridMode", "vocoderWeight",
                                                   "f0Model", "renderingPriority", "experimentalFeatures", "options"}));
    // The options the window offers: two arrays of text, the enumeration of the rendering priority in full. Which
    // weight files and pitch models exist depends on the machine, so their content is checked by PrefsOptions with a
    // models directory of its own.
    {
        const auto& options = memberOf(processing, "options");
        CONTROL_CHECK((memberNames(options) == std::set<std::string>{"vocoderWeight", "f0Model", "renderingPriority"}));
        for (const char* name : {"vocoderWeight", "f0Model"}) {
            const auto& list = memberOf(options, name);
            CONTROL_CHECK(list.isArray() && list.size() <= 64);
            for (std::size_t i = 0; i < list.size(); ++i)
                CONTROL_CHECK(list.at(i).isString() && !list.at(i).text().empty());
        }
        CONTROL_CHECK(toJsonString(memberOf(options, "renderingPriority")) == R"(["gpuFirst","cpuFirst"])");
    }
    CONTROL_CHECK(near(memberOf(processing, "tuningHz"), TuningConfig::currentTuningHz(), 1e-3));
    CONTROL_CHECK(memberOf(processing, "editingScheme").text() == "openDyne");
    CONTROL_CHECK(memberOf(processing, "f0Model").text() == "fcpe");
    CONTROL_CHECK(memberOf(processing, "hybridMode").isBoolean());
    CONTROL_CHECK(memberOf(processing, "experimentalFeatures").asBoolean());
    const auto priority = memberOf(processing, "renderingPriority").text();
    CONTROL_CHECK(priority == "gpuFirst" || priority == "cpuFirst");
    CONTROL_CHECK(memberOf(processing, "vocoderWeight").isString()
                  || nullReason(processing, "vocoderWeight").text() == "unavailable");
    const auto language = memberOf(result, "language").text();
    CONTROL_CHECK(language == "en" || language == "zh" || language == "ja" || language == "ru" || language == "es");
    CONTROL_CHECK(memberOf(result, "onboardingShown").isBoolean());

    // Most recent first, with whether each file is still there.
    const auto& recent = memberOf(result, "recentProjects");
    if (CONTROL_CHECK(recent.size() == 2)) {
        CONTROL_CHECK(memberOf(recent.at(0), "path").text() == missing.getFullPathName().toStdString()
                      && !memberOf(recent.at(0), "exists").asBoolean());
        CONTROL_CHECK(memberOf(recent.at(1), "path").text() == existing.getFullPathName().toStdString()
                      && memberOf(recent.at(1), "exists").asBoolean());
    }
    CONTROL_CHECK(api.fails("prefs.get", json(R"({"verbose":true})"), "INVALID_ARGUMENT", "invalid_params"));

    preferences.clearRecentProjects();
    preferences.setExperimentalFeaturesEnabled(false);
    preferences.setAudioEditingScheme(AudioEditingScheme::Scheme::CorrectedF0Primary);
    CONTROL_CHECK(memberOf(api.result("prefs.get"), "recentProjects").size() == 0);
}

bool anyLineContains(const JsonValue& lines, const std::string& needle)
{
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines.at(i).isString() && lines.at(i).text().find(needle) != std::string::npos)
            return true;
    return false;
}

void checkAppLog(ApiSession& api)
{
    const auto token = memberOf(api.discovery(), "token").text();
    const auto proof = memberOf(api.discovery(), "serverProof").text();
    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName().toStdString();
    CONTROL_CHECK(token.size() >= 32 && proof.size() >= 32 && home.size() > 3);
    auto forward = home;
    std::replace(forward.begin(), forward.end(), '\\', '/');

    // Secrets and the home directory planted into the log, as an application bug could write them.
    const std::string marker = "details-scenario-marker";
    AppLogger::log(juce::String::fromUTF8((marker + " token=" + token + " proof=" + proof).c_str()));
    AppLogger::log(juce::String::fromUTF8((marker + " file=" + home + "\\Documents\\song.otproj").c_str()));
    AppLogger::log(juce::String::fromUTF8((marker + " file=" + forward + "/Documents/song.otproj").c_str()));
    std::string upperToken = token;
    for (auto& c : upperToken)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    AppLogger::log(juce::String::fromUTF8((marker + " upper=" + upperToken).c_str()));
    AppLogger::log(juce::String::fromUTF8((marker + " long=" + std::string(6000, 'x')).c_str()));
    AppLogger::log(juce::String::fromUTF8((marker + " end").c_str()));

    JsonValue result;
    CONTROL_CHECK(pumpMessagesUntil(
        [&] {
            result = api.result("app.getLog", json(R"({"maxLines":1000})"));
            return anyLineContains(memberOf(result, "lines"), marker + " end");
        },
        5000));
    const auto& lines = memberOf(result, "lines");
    CONTROL_CHECK(lines.isArray() && lines.size() >= 6 && lines.size() <= 1000);
    // Neither secret, nor the home directory in any spelling, appears anywhere in the result.
    const auto text = toJsonString(result);
    CONTROL_CHECK(text.find(token) == std::string::npos && text.find(proof) == std::string::npos
                  && text.find(upperToken) == std::string::npos);
    CONTROL_CHECK(!anyLineContains(lines, home) && !anyLineContains(lines, forward));
    CONTROL_CHECK(anyLineContains(lines, marker + " upper=<redacted>"));
    CONTROL_CHECK(anyLineContains(lines, marker + " token=<redacted> proof=<redacted>"));
    CONTROL_CHECK(anyLineContains(lines, marker + " file=~\\Documents\\song.otproj"));
    CONTROL_CHECK(anyLineContains(lines, marker + " file=~/Documents/song.otproj"));
    // A line is cut to 4096 characters.
    bool foundLong = false;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines.at(i).text().find(marker + " long=") != std::string::npos) {
            foundLong = true;
            CONTROL_CHECK(utf8CodePointCount(lines.at(i).text()) == 4096);
        }
    CONTROL_CHECK(foundLong);

    // maxLines limits the tail to the newest lines, oldest first, and says that earlier lines exist.
    const auto last = api.result("app.getLog", json(R"({"maxLines":2})"));
    CONTROL_CHECK(memberOf(last, "lines").size() == 2 && memberOf(last, "truncated").asBoolean());
    // The log is not project state.
    CONTROL_CHECK(last.find("asOf") == nullptr && last.find("derived") == nullptr);
    CONTROL_CHECK(memberOf(api.result("app.getLog"), "lines").size() <= 200);

    for (const char* params :
         {R"({"maxLines":0})", R"({"maxLines":1001})", R"({"maxLines":1.5})", R"({"maxLines":"3"})", R"({"limit":3})"})
        CONTROL_CHECK(api.fails("app.getLog", json(params), "INVALID_ARGUMENT", "invalid_params"));
}

// ---- The specification's vectors ------------------------------------------------------------------

/** The methods this build implements; the runner runs the vectors that use only these. */
const std::set<std::string> kImplementedMethods = {"session.info",
                                                   "project.get",
                                                   "content.getNotes",
                                                   "content.getPitchCurve",
                                                   "content.segmentNotes",
                                                   "content.getDetails",
                                                   "transport.get",
                                                   "prefs.get",
                                                   "app.getLog"};
/** The capabilities whose vectors this runner runs; ControlJobTests.cpp (JobVectors) runs those of
    jobs.v1 and import.v1, ControlExportTests.cpp (ExportVectors) those of export.v1. */
const std::set<std::string> kReportedCapabilities = {"session.v1", "read.v1"};

/**
 * Methods of a capability the instance reports that are not registered in OpsRegistry. An instance
 * must implement every method of every capability it reports (PROTOCOL.md section 3.3 rule 5 case 10), so
 * this list must stay empty, and read.v1 may never be on it. A method that gets registered is removed from
 * the list (scenarioCompleteness fails on a stale entry). An entry would carry a comment that names why the
 * method is missing. There is none.
 */
const std::set<std::string> kUnimplementedReportedMethods = {};

/** The vectors of the specification that run against the processor and must pass, by id. */
const char* const kMustRunVectors[] = {
    "app/get-log-ok",
    "content/get-details-ok",
    "content/get-details-stale-handle",
    "content/get-notes-handle-kind",
    "content/get-notes-ok",
    "content/get-notes-stale-handle",
    "content/get-notes-timeline",
    "content/get-pitch-curve-not-found",
    "content/get-pitch-curve-ok",
    "content/segment-notes-ok",
    "content/segment-notes-stale-handle",
    "prefs/get-ok",
    "project/get-ok",
    "session/batch-rejected",
    "session/info-ok",
    "session/info-unknown-param",
    "session/unknown-method",
    "transport/get-ok",
};

/** The methods a vector sends. */
std::vector<std::string> methodsOf(const JsonValue& vector)
{
    std::vector<std::string> methods;
    if (const JsonValue* request = vector.find("request"))
        methods.push_back(memberOf(*request, "method").text());
    if (const JsonValue* steps = vector.find("steps"))
        for (std::size_t i = 0; i < steps->size(); ++i)
            if (const JsonValue* send = steps->at(i).find("send"))
                methods.push_back(memberOf(*send, "method").text());
    return methods;
}

/** Why this runner does not run a vector, or empty when it does. */
std::string vectorSkipReason(const JsonValue& vector)
{
    // app.quit ends the instance and needs a bridge that can close its window: ControlQuitTests.cpp (AppQuitVectors).
    for (const auto& method : methodsOf(vector))
        if (method == "app.quit")
            return "runs in ControlProcessorTests.AppQuitVectors: the vector needs a bridge that quits, and some end "
                   "the instance";
    if (kReportedCapabilities.count(memberOf(vector, "capability").text()) == 0)
        return "needs capability " + memberOf(vector, "capability").text() + ", which this runner does not cover";
    const auto& extra = memberOf(vector, "extraCapabilities");
    for (std::size_t i = 0; i < extra.size(); ++i)
        if (kReportedCapabilities.count(extra.at(i).text()) == 0)
            return "needs capability " + extra.at(i).text() + ", which this runner does not cover";
    const auto methods = methodsOf(vector);
    bool usesImplemented = false;
    for (const auto& method : methods) {
        if (kImplementedMethods.count(method) != 0)
            usesImplemented = true;
        else if (controlMethodCapability(method) != nullptr && method != "session.hello")
            return "uses " + method + ", which this runner does not cover";
    }
    if (!usesImplemented)
        return "tests the listener only (ControlUnitTests runs it against the channel)";
    if (memberOf(vector, "session").text() != "authenticated")
        return "unauthenticated vectors test the listener (ControlUnitTests)";
    if (const JsonValue* steps = vector.find("steps"))
        for (std::size_t i = 0; i < steps->size(); ++i) {
            const auto& step = steps->at(i);
            if ((step.find("send") == nullptr && step.find("expect") == nullptr) || step.find("connection") != nullptr)
                return "uses steps other than send and expect on the main connection";
        }
    const auto& given = memberOf(vector, "given");
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& name = given.keyAt(i);
        if (name == "dirty" && !given.at(i).asBoolean())
            continue; // without an editor the project has no unsaved changes
        if (name == "editingScheme")
            continue; // set in the preferences by runVector
        if (name == "preferences")
            return "precondition given.preferences (and the options of the installation) is established by "
                   "ControlProcessorTests.PrefsVectors, not by this runner";
        if (name != "clips")
            return "precondition given." + name + " is not established by this runner";
        for (std::size_t c = 0; c < given.at(i).size(); ++c) {
            const auto& clip = given.at(i).at(c);
            if (memberOf(clip, "analysis").text() != "ready")
                return "a clip with analysis " + memberOf(clip, "analysis").text()
                       + " is not established by this runner";
        }
    }
    return {};
}

std::optional<Scale> scaleOf(const std::string& name)
{
    const std::pair<const char*, Scale> scales[] = {{"major", Scale::Major},
                                                    {"minor", Scale::Minor},
                                                    {"chromatic", Scale::Chromatic},
                                                    {"harmonicMinor", Scale::HarmonicMinor},
                                                    {"dorian", Scale::Dorian},
                                                    {"mixolydian", Scale::Mixolydian},
                                                    {"pentatonicMajor", Scale::PentatonicMajor},
                                                    {"pentatonicMinor", Scale::PentatonicMinor}};
    for (const auto& [text, scale] : scales)
        if (name == text)
            return scale;
    return std::nullopt;
}

/**
 * Runs one vector (request, or scenario of send and expect steps) against a fresh processor: establishes
 * given.clips, binds the names of vectors/README.md (discovery.*, runner.*, <clip>.*), then sends and
 * matches on a client thread while this thread pumps messages. Empty on success.
 */
std::string runVector(const JsonValue& vector)
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return "the control API did not start";
    VectorMatcher matcher;

    // The preferences persist in the data directory between vectors: set the scheme every time.
    const auto scheme = memberOf(memberOf(vector, "given"), "editingScheme").text();
    processor->getAppPreferences().setAudioEditingScheme(scheme == "openDyne"
                                                             ? AudioEditingScheme::Scheme::NotesPrimary
                                                             : AudioEditingScheme::Scheme::CorrectedF0Primary);

    std::vector<std::pair<std::string, ClipSpec>> specs;
    std::vector<std::vector<std::string>> handleBinds; // per clip: the names of its grid handles
    const auto& clips = memberOf(memberOf(vector, "given"), "clips");
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto& given = clips.at(i);
        ClipSpec spec;
        spec.name = memberOf(given, "bind").text();
        spec.trackId = static_cast<int>(memberOf(given, "trackId").asDouble());
        spec.startTimelineSec = memberOf(given, "startTimelineSec").asDouble();
        spec.durationSec = memberOf(given, "durationSec").asDouble();
        const auto& notes = memberOf(given, "notes");
        for (std::size_t n = 0; n < notes.size(); ++n)
            spec.notes.push_back({memberOf(notes.at(n), "startContentSec").asDouble(),
                                  memberOf(notes.at(n), "endContentSec").asDouble(),
                                  memberOf(notes.at(n), "targetMidi").asDouble()});
        if (const JsonValue* key = given.find("key")) {
            DetectedKey detected;
            detected.root = static_cast<Key>(static_cast<int>(memberOf(*key, "root").asDouble()));
            detected.scale = scaleOf(memberOf(*key, "scale").text()).value_or(Scale::Major);
            detected.origin = Origin::Manual;
            spec.key = detected;
        }
        if (const JsonValue* shift = given.find("pitchShift")) {
            PitchShiftSettings settings;
            settings.semitone = static_cast<int>(memberOf(*shift, "semitones").asDouble());
            settings.cents = static_cast<int>(memberOf(*shift, "cents").asDouble());
            spec.pitchShift = settings;
        }
        handleBinds.emplace_back();
        const auto& grid = memberOf(given, "timeGrid");
        for (std::size_t h = 0; h < grid.size(); ++h) {
            spec.gridHandlesContentSec.push_back(memberOf(grid.at(h), "contentSec").asDouble());
            handleBinds.back().push_back(memberOf(grid.at(h), "bind").text());
        }
        specs.emplace_back(spec.name, spec);
    }
    std::vector<std::pair<std::string, Clip>> created;
    for (const auto& [bind, spec] : specs)
        created.emplace_back(bind, addClip(*processor, spec));
    for (std::size_t i = 0; i < created.size(); ++i)
        for (std::size_t h = 0; h < handleBinds[i].size() && h < created[i].second.handleIds.size(); ++h)
            matcher.bindings[handleBinds[i][h]] = JsonValue::string(created[i].second.handleIds[h]);

    ApiSession api;
    if (!api.open(service))
        return "session.hello failed";
    const auto& discovery = api.discovery();
    for (std::size_t i = 0; i < discovery.size(); ++i)
        matcher.bindings["discovery." + discovery.keyAt(i)] = discovery.at(i);
    const auto instanceShort = memberOf(discovery, "instanceId").text().substr(0, 8);
    const auto epoch = std::to_string(processor->getStandaloneContentRepository()->getContentEpoch());
    const auto other = otherInstanceShort(instanceShort);
    matcher.bindings["runner.unknownContentId"] = JsonValue::string("c:" + instanceShort + ":" + epoch + ":4294967295");
    matcher.bindings["runner.unknownPlacementId"] =
        JsonValue::string("p:" + instanceShort + ":" + epoch + ":4294967295");
    matcher.bindings["runner.staleContentId"] = JsonValue::string("c:" + other + ":" + epoch + ":1");
    matcher.bindings["runner.stalePlacementId"] = JsonValue::string("p:" + other + ":" + epoch + ":1");
    const int lastTrack = processor->getStandaloneArrangement()->getNumTracks() - 1;
    matcher.bindings["runner.lastTrackId"] = JsonValue::integer(lastTrack);
    matcher.bindings["runner.unknownTrackId"] = JsonValue::integer(lastTrack + 1);

    // The handles and tokens of the clips after the preconditions, as the instance reports them.
    if (!created.empty()) {
        const auto project = api.result("project.get");
        for (std::size_t i = 0; i < created.size(); ++i) {
            const auto& [bind, clip] = created[i];
            const auto handles = handlesOf(project, specs[i].second.trackId, specs[i].second.startTimelineSec);
            if (!handles)
                return "the clip " + bind + " is not in project.get";
            matcher.bindings[bind + ".contentId"] = JsonValue::string(handles->contentId);
            matcher.bindings[bind + ".placementId"] = JsonValue::string(handles->placementId);
            matcher.bindings[bind + ".geometryHash"] = JsonValue::string(handles->geometryHash);
            matcher.bindings[bind + ".sourcePath"] = JsonValue::string(clip.sourcePath);
            for (const char* token : {"revision", "gridRevision", "keyRevision"})
                matcher.bindings[bind + "." + token] = memberOf(handles->tokens, token);
        }
    }

    std::string failure;
    runWhilePumping([&] {
        auto& connection = api.client();
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
            } else {
                failure = matcher.expectLine(connection, memberOf(step, "expect"));
            }
            if (!failure.empty())
                failure = "step " + std::to_string(i + 1) + ": " + failure;
        }
    });
    processor.reset();
    return failure;
}

} // namespace

// ---- Shared with ControlEditTests.cpp (ControlContentFixtures.h) -----------------------------------

Clip fixtureAddClip(OpenTuneAudioProcessor& processor, const ClipSpec& spec)
{
    return addClip(processor, spec);
}

std::optional<ClipHandles> fixtureHandlesOf(const JsonValue& project, int trackId, double startTimelineSec)
{
    return handlesOf(project, trackId, startTimelineSec);
}

std::string fixtureOtherInstanceShort(const std::string& instanceShort)
{
    return otherInstanceShort(instanceShort);
}

std::optional<Scale> fixtureScaleOf(const std::string& name)
{
    return scaleOf(name);
}

bool fixtureSaveAndReopen(OpenTuneAudioProcessor& processor)
{
    return saveAndReopen(processor);
}

bool fixtureReopenWithAnalysis(OpenTuneAudioProcessor& processor, std::uint64_t renumberFrom, std::uint64_t renumberTo)
{
    if (!saveAndReopen(processor, renumberFrom, renumberTo))
        return false;
    auto* repository = processor.getStandaloneContentRepository();
    // Opening requests the pitch analysis of every clip (ProjectSession::commitPreparedOpen). Without the models it
    // fails on a worker thread and the failure is committed from a later message; wait for it, or it would replace
    // the analysis given below.
    const bool ended = pumpMessagesUntil(
        [&processor, repository] {
            for (const auto& key : repository->getAllClips()) {
                const auto snapshot = processor.getContentSnapshot(key);
                if (snapshot != nullptr && snapshot->originalF0State == OriginalF0State::Extracting)
                    return false;
            }
            return true;
        },
        kSettleTimeoutMs);
    if (!CONTROL_CHECK(ended))
        return false;
    for (const auto& key : repository->getAllClips()) {
        const auto snapshot = processor.getContentSnapshot(key);
        auto* owner = repository->findClip(key);
        if (snapshot == nullptr || snapshot->audioBuffer == nullptr || owner == nullptr)
            continue;
        ClipSpec spec;
        spec.durationSec = snapshot->audioBuffer->getNumSamples() / kAudioSampleRate;
        owner->applyOriginalF0(analysisOf(spec));
        CONTROL_CHECK(processor.setContentOriginalF0State(key, OriginalF0State::Ready));
    }
    return CONTROL_CHECK(settleBackgroundWork(processor));
}

// ---- Scenarios -------------------------------------------------------------------------------------

void scenarioReads()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    const auto dispatcher = service.dispatcher();
    if (!CONTROL_CHECK(dispatcher != nullptr))
        return;
    for (const auto& method : kImplementedMethods)
        CONTROL_CHECK(dispatcher->implements(method));
    // The listener answers session.hello itself; the dispatcher never sees it.
    CONTROL_CHECK(!dispatcher->implements("session.hello"));
    const auto sung = addClip(*processor, sungSpec());
    addClip(*processor, unanalysedSpec("pending", 1, 0.0, Analysis::NotRequested));
    addClip(*processor, unanalysedSpec("running", 1, 3.0, Analysis::Running));
    addClip(*processor, unanalysedSpec("failed", 2, 0.0, Analysis::Failed));

    ApiSession api;
    if (!api.open(service))
        return;
    checkSessionInfo(api);
    const auto project = checkProjectWithoutEditor(api, *processor, sung);
    const auto handles = handlesOf(project, 0, 2.0);
    if (!CONTROL_CHECK(handles.has_value()))
        return;
    checkProjectWithEditor(api, service, sung, *handles);
    checkPitchCurve(api, *handles);
    checkSegmentNotes(api, *handles);
    checkUnanalysed(api, project);
    checkErrors(api, *handles);
    checkNotes(api, *processor, sung, *handles);
    checkReopen(api, *processor, *handles);

    // Every access of the operations to OpenTune state happened on the message thread.
    CONTROL_CHECK(dispatcher->threadProbe().otherThreadAccesses.load() == 0);
    CONTROL_CHECK(dispatcher->threadProbe().messageThreadAccesses.load() > 0 && dispatcher->executedCount() > 0);
    note(std::to_string(dispatcher->executedCount()) + " methods ran, "
         + std::to_string(dispatcher->threadProbe().messageThreadAccesses.load()) + " accesses on the message thread");
}

void scenarioThreadProbe()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    const auto dispatcher = service.dispatcher();
    if (!CONTROL_CHECK(dispatcher != nullptr))
        return;
    addClip(*processor, sungSpec());
    ApiSession api;
    if (!api.open(service))
        return;
    const auto project = api.result("project.get");
    const auto handles = handlesOf(project, 0, 2.0);
    if (!CONTROL_CHECK(handles.has_value()))
        return;
    const auto byContent = json("{\"contentId\":" + quoted(handles->contentId) + "}");
    const int accessesBefore = dispatcher->threadProbe().messageThreadAccesses.load();
    const int executedBefore = dispatcher->executedCount();
    api.result("session.info");
    api.result("content.getNotes", byContent);
    api.result("content.getPitchCurve", byContent);
    api.result("content.segmentNotes", byContent);
    CONTROL_CHECK(dispatcher->executedCount() == executedBefore + 4);
    CONTROL_CHECK(dispatcher->threadProbe().messageThreadAccesses.load() > accessesBefore);
    CONTROL_CHECK(dispatcher->threadProbe().otherThreadAccesses.load() == 0);

    // The check is always on, also in release builds: an operation context used on another thread
    // refuses to hand out the processor and counts the attempt.
    ControlThreadProbe probe;
    ControlInstanceFacts facts;
    facts.instanceId = memberOf(api.discovery(), "instanceId").text();
    bool refused = false;
    std::thread other([&] {
        ControlOpContext context(*processor, nullptr, facts, probe);
        try {
            context.processor();
        } catch (const ControlThreadViolation&) {
            refused = true;
        }
    });
    other.join();
    CONTROL_CHECK(refused && probe.otherThreadAccesses.load() == 1 && probe.messageThreadAccesses.load() == 0);
    ControlOpContext onMessageThread(*processor, nullptr, facts, probe);
    CONTROL_CHECK(&onMessageThread.processor() == processor.get() && probe.messageThreadAccesses.load() == 1);
    CONTROL_CHECK(onMessageThread.instanceShort() == facts.instanceId.substr(0, 8));
}

void scenarioDispatchShutdown()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    const auto dispatcher = service.dispatcher();
    const auto messageThread = service.messageThread();
    ApiSession api;
    if (!CONTROL_CHECK(dispatcher != nullptr) || !api.open(service))
        return;

    // This thread does not pump messages, so the request waits in ControlMessageThread::call().
    std::optional<std::string> response;
    std::thread client([&api, &response] {
        if (api.client().sendLine(R"({"jsonrpc":"2.0","id":7,"method":"project.get","params":{}})"))
            response = api.client().readLine();
    });
    CONTROL_CHECK(waitUntil([&messageThread] { return messageThread->waitingCalls() == 1; }, kWaiterSettleMs));

    const auto started = Clock::now();
    processor.reset();
    const auto elapsed = millisecondsSince(started);
    note("processor destroyed in " + std::to_string(elapsed) + " ms");
    CONTROL_CHECK(elapsed <= kShutdownBoundMs);
    client.join();
    // Woken by the stop: SHUTTING_DOWN, never executed. (A TIMEOUT would also promise executed false.)
    if (CONTROL_CHECK(response.has_value())) {
        note("response: " + *response);
        CONTROL_CHECK(isError(response, "SHUTTING_DOWN", "") || isError(response, "TIMEOUT", "dispatch_timeout"));
        const auto parsed = parseJson(*response);
        const JsonValue* data = errorData(parsed.value);
        CONTROL_CHECK(data != nullptr && memberOf(*data, "executed").isBoolean()
                      && !memberOf(*data, "executed").asBoolean());
        CONTROL_CHECK(memberOf(parsed.value, "id").asDouble() == 7);
    }
    CONTROL_CHECK(dispatcher->isStopping() && messageThread->waitingCalls() == 0);
    // The queued function reaches the message loop now and finds the liveness token dead.
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(dispatcher->executedCount() == 0);
}

void scenarioDispatchTimeout()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    const auto dispatcher = service.dispatcher();
    ApiSession api;
    if (!CONTROL_CHECK(dispatcher != nullptr) || !api.open(service))
        return;

    // This thread does not pump messages for longer than dispatchTimeoutMs.
    const int dispatchTimeoutMs = ControlLimits{}.dispatchTimeoutMs;
    std::optional<std::string> response;
    std::atomic<bool> answered{false};
    std::thread client([&] {
        if (api.client().sendLine(R"({"jsonrpc":"2.0","id":8,"method":"project.get","params":{}})"))
            response = api.client().readLine(dispatchTimeoutMs + kReadTimeoutMs);
        answered = true;
    });
    const auto started = Clock::now();
    CONTROL_CHECK(
        waitUntil([&answered] { return answered.load(); }, dispatchTimeoutMs + kReadTimeoutMs + kCloseSeenMs));
    client.join();
    const auto elapsed = millisecondsSince(started);
    note("answered after " + std::to_string(elapsed) + " ms");
    CONTROL_CHECK(elapsed >= dispatchTimeoutMs);
    if (CONTROL_CHECK(response.has_value())) {
        CONTROL_CHECK(isError(response, "TIMEOUT", "dispatch_timeout"));
        const auto parsed = parseJson(*response);
        const JsonValue* data = errorData(parsed.value);
        CONTROL_CHECK(data != nullptr && !memberOf(*data, "executed").asBoolean()
                      && memberOf(*data, "retryable").asBoolean());
    }
    // The abandoned request reaches the message loop now and never runs; the next one does.
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(dispatcher->executedCount() == 0);
    CONTROL_CHECK(memberOf(api.result("project.get"), "placements").isArray());
    CONTROL_CHECK(dispatcher->executedCount() == 1);

    // The same for a write: content.setKey is abandoned too, never runs later, and the content keeps its key.
    const auto clip = addClip(*processor, sungSpec());
    CONTROL_CHECK(settleBackgroundWork(*processor));
    const auto handles = handlesOf(api.result("project.get"), 0, 2.0);
    if (!CONTROL_CHECK(handles.has_value()))
        return;
    const auto executedBefore = dispatcher->executedCount();
    const auto keyBefore = processor->getContentSnapshot(clip.key)->detectedKey;
    const auto tokensBefore = toJsonString(handles->tokens);
    const std::string setKey = R"({"jsonrpc":"2.0","id":9,"method":"content.setKey","params":{"contentId":")"
                               + handles->contentId + R"(","key":{"root":0,"scale":"major"}}})";
    std::optional<std::string> writeResponse;
    std::atomic<bool> writeAnswered{false};
    std::thread writer([&] {
        if (api.client().sendLine(setKey))
            writeResponse = api.client().readLine(dispatchTimeoutMs + kReadTimeoutMs);
        writeAnswered = true;
    });
    CONTROL_CHECK(waitUntil([&writeAnswered] { return writeAnswered.load(); },
                            dispatchTimeoutMs + kReadTimeoutMs + kCloseSeenMs));
    writer.join();
    if (CONTROL_CHECK(writeResponse.has_value())) {
        CONTROL_CHECK(isError(writeResponse, "TIMEOUT", "dispatch_timeout"));
        const auto parsed = parseJson(*writeResponse);
        const JsonValue* data = errorData(parsed.value);
        CONTROL_CHECK(data != nullptr && !memberOf(*data, "executed").asBoolean());
    }
    // The abandoned write reaches the message loop now and never runs: no method ran, the key and its token are as
    // they were.
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(dispatcher->executedCount() == executedBefore);
    const auto keyAfter = processor->getContentSnapshot(clip.key)->detectedKey;
    CONTROL_CHECK(keyAfter.root == keyBefore.root && keyAfter.scale == keyBefore.scale
                  && keyAfter.origin == keyBefore.origin);
    const auto handlesAfter = handlesOf(api.result("project.get"), 0, 2.0);
    CONTROL_CHECK(handlesAfter.has_value() && toJsonString(handlesAfter->tokens) == tokensBefore);
    // The next write runs.
    const auto set = api.result(
        "content.setKey", json("{\"contentId\":\"" + handles->contentId + R"(","key":{"root":0,"scale":"major"}})"));
    CONTROL_CHECK(memberOf(set, "applied").asBoolean() && !memberOf(set, "noOp").asBoolean());
    CONTROL_CHECK(processor->getContentSnapshot(clip.key)->detectedKey.root == Key::C);
}

void scenarioDetails()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    const auto dispatcher = service.dispatcher();
    if (!CONTROL_CHECK(dispatcher != nullptr))
        return;

    DetailsClips clips;
    auto sung = sungSpec();
    sung.pitchShift = PitchShiftSettings{-2, 0};
    sung.gridHandlesContentSec = {1.0, 2.5};
    clips.sung = addClip(*processor, sung);
    clips.ref = addClip(*processor, refSpec());
    clips.failedEmpty = addClip(*processor, unanalysedSpec("failedEmpty", 2, 0.0, Analysis::Failed));
    clips.failedPlain = addClip(*processor, unanalysedSpec("failedPlain", 3, 0.0, Analysis::Failed));
    clips.pending = addClip(*processor, unanalysedSpec("pending", 4, 0.0, Analysis::NotRequested));
    // What the analysis callbacks store when they fail it: the message of the extraction for audio without a voiced
    // frame (the other failed clip has none, as a failure loaded from a project).
    CONTROL_CHECK(failAnalysis(*processor, clips.failedEmpty.key, true));
    // The render that importing the sung clip requested, and what it posts when it fails without the models (an
    // upstream diagnostic dialog built on this thread), are over before the first timed request.
    CONTROL_CHECK(settleBackgroundWork(*processor));

    ApiSession api;
    if (!api.open(service))
        return;
    const auto project = api.result("project.get");
    const auto sungHandles = handlesOf(project, 0, 2.0);
    const auto refHandles = handlesOf(project, 1, 2.0);
    if (!CONTROL_CHECK(sungHandles.has_value() && refHandles.has_value()))
        return;

    checkDetailsOfSung(api, *processor, clips, *sungHandles, *refHandles);
    checkAutoModes(api, *processor, *sungHandles);
    checkDetailsOfUnanalysed(api, *processor, clips, project);
    checkDetailsErrors(api, *sungHandles);
    checkTransport(api, service, project);
    checkPrefs(api, *processor);
    checkAppLog(api);
    CONTROL_CHECK(dispatcher->threadProbe().otherThreadAccesses.load() == 0);

    // Opening a project starts a new content epoch and clip ids restart at 1: the message of a failure in the old
    // project is not found with a content of the new one that has the same key (it lives in the content).
    const auto failedKey = clips.failedEmpty.key;
    const auto failedBefore = processor->getContentSnapshot(failedKey);
    CONTROL_CHECK(failedBefore != nullptr && failedBefore->originalF0FailureReason == kUnvoicedF0FailureMessage);
    if (saveAndReopen(*processor)) {
        const auto failedAfter = processor->getContentSnapshot(failedKey);
        CONTROL_CHECK(failedAfter == nullptr || failedAfter->originalF0FailureReason.empty());
    }
}

void scenarioVectors()
{
    const auto root = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors";
    std::vector<std::filesystem::path> files;
    for (const char* area : {"session", "project", "content", "transport", "prefs", "app"})
        for (const auto& entry : std::filesystem::directory_iterator(root / area))
            if (entry.path().extension() == ".json")
                files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    int passed = 0;
    int skipped = 0;
    std::set<std::string> passedIds;
    VectorReport report("ControlProcessorTests.Vectors");
    for (const auto& file : files) {
        const auto parsed = parseJson(readFile(file));
        if (!CONTROL_CHECK(parsed.ok))
            continue;
        const auto& vector = parsed.value;
        const auto id = memberOf(vector, "id").text();
        if (const auto reason = vectorSkipReason(vector); !reason.empty()) {
            ++skipped;
            report.skip(id, reason);
            note("SKIP " + id + ": " + reason);
            continue;
        }
        const auto started = Clock::now();
        const auto failure = runVector(vector);
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty())) {
            ++passed;
            passedIds.insert(id);
        }
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    // Every vector of the list runs and passes: a vector that is skipped or gone fails here, instead of
    // a count that hides it. A newly runnable vector of the specification is added to the list on purpose.
    for (const char* id : kMustRunVectors)
        CONTROL_CHECK(passedIds.count(id) == 1);
    for (const auto& id : passedIds)
        if (std::find_if(std::begin(kMustRunVectors), std::end(kMustRunVectors),
                         [&id](const char* listed) { return id == listed; })
            == std::end(kMustRunVectors))
            note("passes but is not in kMustRunVectors: " + id);
    note(std::to_string(passed) + " vectors passed, " + std::to_string(skipped) + " skipped");
}

void scenarioCompleteness()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    const auto dispatcher = service.dispatcher();
    if (!CONTROL_CHECK(dispatcher != nullptr))
        return;
    const auto parsed =
        parseJson(readFile(std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "methods" / "index.json"));
    if (!CONTROL_CHECK(parsed.ok))
        return;
    const auto& methods = memberOf(parsed.value, "methods");
    const auto reported = dispatcher->capabilities();
    for (const char* capability :
         {"session.v1", "read.v1", "jobs.v1", "import.v1", "export.v1", "ui.bridge", "ui.workspace", "edit.v1",
          "history.v1", "timing.v1", "transport.v1", "arrangement.v1", "tracks.v1", "project.io", "project.copy",
          "prefs.write", "audio.device", "app.quit"})
        CONTROL_CHECK(std::find(reported.begin(), reported.end(), capability) != reported.end());

    // The listener answers session.hello itself, so the dispatcher's method table has no entry for it.
    std::set<std::string> indexed;
    for (std::size_t i = 0; i < methods.size(); ++i) {
        const auto& entry = methods.at(i);
        const auto method = memberOf(entry, "method").text();
        const auto capability = memberOf(entry, "capability").text();
        if (std::find(reported.begin(), reported.end(), capability) == reported.end() || method == "session.hello")
            continue;
        indexed.insert(method);
        // The C++ method table of the protocol must agree with the index as well.
        const char* tableCapability = controlMethodCapability(method);
        if (!CONTROL_CHECK(tableCapability != nullptr && capability == tableCapability))
            note(method + ": capability of the index differs from the method table of the protocol");
        const bool exempt = kUnimplementedReportedMethods.count(method) != 0;
        if (!CONTROL_CHECK(dispatcher->implements(method) || exempt))
            note("capability " + capability + " is reported, but " + method + " is not registered in OpsRegistry");
        // An exemption of a method that exists is stale: the list shrinks, it does not rot.
        CONTROL_CHECK(!(exempt && dispatcher->implements(method)));
    }
    // Index, schemas and the C++ method table are three views of one list: the capability each names for a method
    // must agree, and the set of methods the schemas tie to a reported capability must equal the index's set, so
    // that a drift between the files of the locked copy cannot hide a missing method.
    std::set<std::string> schemaMethods;
    for (const auto& file :
         std::filesystem::directory_iterator(std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "methods")) {
        const auto name = file.path().filename().u8string();
        constexpr std::string_view kSuffix = ".schema.json";
        if (name.size() <= kSuffix.size() || name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        const auto schema = parseJson(readFile(file.path()));
        if (!CONTROL_CHECK(schema.ok))
            continue;
        const auto& tag = memberOf(schema.value, "x-otcp");
        const auto method = memberOf(tag, "method").text();
        const auto capability = memberOf(tag, "capability").text();
        const char* schemaTableCapability = controlMethodCapability(method);
        if (!CONTROL_CHECK(schemaTableCapability != nullptr && capability == schemaTableCapability))
            note(name + ": capability of the schema differs from the method table of the protocol");
        if (std::find(reported.begin(), reported.end(), capability) == reported.end() || method == "session.hello")
            continue;
        schemaMethods.insert(method);
        const bool exempt = kUnimplementedReportedMethods.count(method) != 0;
        if (!CONTROL_CHECK(dispatcher->implements(method) || exempt))
            note("capability " + capability + " is reported, but " + method + " is not registered in OpsRegistry");
    }
    if (!CONTROL_CHECK(schemaMethods == indexed)) {
        for (const auto& method : indexed)
            if (schemaMethods.count(method) == 0)
                note(method + ": listed in the index, but no schema ties it to a reported capability");
        for (const auto& method : schemaMethods)
            if (indexed.count(method) == 0)
                note(method + ": has a schema, but the index does not list it under a reported capability");
    }
    // The exemption must be for a method of a reported capability, or it exempts nothing.
    for (const auto& method : kUnimplementedReportedMethods) {
        const char* capability = controlMethodCapability(method);
        CONTROL_CHECK(capability != nullptr
                      && std::find(reported.begin(), reported.end(), capability) != reported.end());
    }
    // read.v1 is complete: nothing of it may be exempt.
    for (const auto& method : kUnimplementedReportedMethods) {
        const char* capability = controlMethodCapability(method);
        CONTROL_CHECK(capability == nullptr || std::string(capability) != "read.v1");
    }
    // Every method the build registers except session.hello (which the listener answers): session.info, the eight
    // reads, job.get, job.cancel, import.start, export.start, ui.getState, ui.dismiss, ui.setSelection,
    // ui.setWorkspace, the four methods of edit.v1, the three of the undo history, the two of timing.v1, the three of
    // transport.v1, the two of arrangement.v1, the four of tracks.v1, the four of project.io, project.saveCopy,
    // prefs.set, the two of audio.device and app.quit: 44 in all.
    CONTROL_CHECK(indexed.size() >= 44);
    if (!kUnimplementedReportedMethods.empty())
        note("exempt methods remain; the list must stay empty");
    note(std::to_string(indexed.size()) + " methods of the reported capabilities are registered");
}

void scenarioOffMainThread()
{
    // The method table follows the schemas: a method runs off the message thread exactly when its schema
    // says x-otcp.mainThread false (PROTOCOL.md section 13.3 rule 2), and then has no run on it.
    ControlMethodTable table;
    registerControlMethods(table);
    const auto methodsDir = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "methods";
    const auto index = parseJson(readFile(methodsDir / "index.json"));
    if (!CONTROL_CHECK(index.ok))
        return;
    const auto& methods = memberOf(index.value, "methods");
    int registered = 0;
    std::vector<std::string> offMainThread;
    for (std::size_t i = 0; i < methods.size(); ++i) {
        const auto name = memberOf(methods.at(i), "method").text();
        const ControlMethod* method = table.find(name);
        if (method == nullptr)
            continue;
        ++registered;
        const auto schema = parseJson(readFile(methodsDir / (name + ".schema.json")));
        if (!CONTROL_CHECK(schema.ok))
            continue;
        const auto& mainThread = memberOf(memberOf(schema.value, "x-otcp"), "mainThread");
        if (!CONTROL_CHECK(mainThread.isBoolean() && mainThread.asBoolean() == method->mainThread))
            note(name + ": mainThread of the method table differs from its schema");
        // Off the message thread: exactly one of the two functions, the facts one (app.getLog) or the jobs
        // one (job.get, job.cancel).
        const bool withFacts = static_cast<bool>(method->runOffMainThread);
        const bool withJobs = static_cast<bool>(method->runWithoutMessageThread);
        CONTROL_CHECK(method->mainThread ? (method->run && !withFacts && !withJobs)
                                         : (withFacts != withJobs && !method->run && !method->write));
        if (!method->mainThread)
            offMainThread.push_back(name);
    }
    CONTROL_CHECK(registered >= 11);
    for (const char* name : {"app.getLog", "job.get", "job.cancel"})
        CONTROL_CHECK(std::find(offMainThread.begin(), offMainThread.end(), name) != offMainThread.end());

    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    const auto dispatcher = service.dispatcher();
    const auto messageThread = service.messageThread();
    ApiSession api;
    if (!CONTROL_CHECK(dispatcher != nullptr) || !api.open(service))
        return;
    const std::string marker = "off-main-thread-scenario-marker";
    AppLogger::log(marker);

    // This thread, the message thread, pumps no messages while the request runs, as when a modal loop or a
    // long synchronous operation holds it. app.getLog still answers at once, never with TIMEOUT.
    const int dispatchTimeoutMs = ControlLimits{}.dispatchTimeoutMs;
    const int executedBefore = dispatcher->executedCount();
    std::optional<std::string> response;
    std::atomic<bool> answered{false};
    std::thread client([&] {
        if (api.client().sendLine(R"({"jsonrpc":"2.0","id":9,"method":"app.getLog","params":{"maxLines":50}})"))
            response = api.client().readLine(dispatchTimeoutMs + kReadTimeoutMs);
        answered = true;
    });
    const auto started = Clock::now();
    CONTROL_CHECK(
        waitUntil([&answered] { return answered.load(); }, dispatchTimeoutMs + kReadTimeoutMs + kCloseSeenMs));
    client.join();
    const auto elapsed = millisecondsSince(started);
    note("answered after " + std::to_string(elapsed) + " ms while the message thread was held");
    CONTROL_CHECK(elapsed < dispatchTimeoutMs);
    if (CONTROL_CHECK(response.has_value())) {
        const auto parsed = parseJson(*response);
        const JsonValue* result = parsed.ok ? parsed.value.find("result") : nullptr;
        if (CONTROL_CHECK(result != nullptr))
            CONTROL_CHECK(anyLineContains(memberOf(*result, "lines"), marker));
        else
            note("response: " + *response);
    }
    // Nothing went to the message thread, and no operation context was made.
    CONTROL_CHECK(messageThread->waitingCalls() == 0);
    CONTROL_CHECK(dispatcher->executedCount() == executedBefore && dispatcher->offMainThreadCount() == 1);
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(dispatcher->executedCount() == executedBefore);
    CONTROL_CHECK(dispatcher->threadProbe().otherThreadAccesses.load() == 0);

    // With the message thread free again, the same request through the pumping helper.
    CONTROL_CHECK(memberOf(api.result("app.getLog", json(R"({"maxLines":1})")), "lines").size() == 1);
    CONTROL_CHECK(dispatcher->offMainThreadCount() == 2 && dispatcher->executedCount() == executedBefore);
}

} // namespace ControlTest
} // namespace OpenTune
