// SPDX-License-Identifier: AGPL-3.0-or-later
#include "OpsSupport.h"

#include "Control/ControlUiBridge.h"
#include "Control/Edit/ControlOriginAction.h"
#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneContentRepository.h"
#include "PluginProcessor.h"
#include "Utils/Note.h"
#include "Utils/PitchUtils.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace OpenTune {

namespace {

/** Largest magnitude controlJsonNumber writes exactly with the scaled-integer method. */
constexpr double kMaxScaledMagnitude = 9.0e15;

std::string handleKindName(ControlHandleKind kind)
{
    return kind == ControlHandleKind::Content ? "contentId" : "placementId";
}

Error staleHandle(ControlHandleKind kind)
{
    return controlOpError("NOT_FOUND", "stale_handle",
                          "the " + handleKindName(kind)
                              + " belongs to another instance or to an earlier content epoch (a project was opened)");
}

Error unknownHandle(ControlHandleKind kind)
{
    return controlOpError("NOT_FOUND", "unknown_handle",
                          "the " + handleKindName(kind) + " is current but names nothing that exists now");
}

bool isCurrent(ControlOpContext& context, const ControlHandle& handle)
{
    return handle.instanceShort == context.instanceShort() && !handle.epochOverflow
           && handle.epoch == controlContentEpoch(context);
}

} // namespace

// ---- Building results ----------------------------------------------------------------------------

JsonValue controlJsonNumber(double value, int decimals)
{
    if (!std::isfinite(value))
        value = 0.0;
    decimals = std::clamp(decimals, 0, 9);
    double scale = 1.0;
    for (int i = 0; i < decimals; ++i)
        scale *= 10.0;
    double scaled = std::round(value * scale);
    if (std::abs(scaled) > kMaxScaledMagnitude) {
        // Far outside the ranges of the protocol; keep whole units.
        scaled = std::round(std::clamp(value, -kMaxScaledMagnitude, kMaxScaledMagnitude));
        scale = 1.0;
        decimals = 0;
    }
    const auto integer = static_cast<long long>(scaled);
    const bool negative = integer < 0;
    std::string digits = std::to_string(negative ? -integer : integer);
    if (decimals > 0) {
        if (digits.size() <= static_cast<std::size_t>(decimals))
            digits.insert(0, static_cast<std::size_t>(decimals) + 1 - digits.size(), '0');
        digits.insert(digits.size() - static_cast<std::size_t>(decimals), 1, '.');
        while (digits.back() == '0')
            digits.pop_back();
        if (digits.back() == '.')
            digits.pop_back();
    }
    if (negative && digits != "0")
        digits.insert(0, 1, '-');
    return JsonValue::number(digits, digits == "0" ? 0.0 : static_cast<double>(integer) / scale);
}

double controlNoteTargetMidi(const Note& note, float tuningHz)
{
    // PitchUtils::freqToMidi(): MIDI note number of a frequency at the given A4.
    return PitchUtils::freqToMidi(note.getAdjustedPitch(), tuningHz);
}

JsonValue controlNoteTargetMidiJson(const Note& note, float tuningHz)
{
    return controlJsonPitch(controlNoteTargetMidi(note, tuningHz));
}

JsonValue controlNoteOffsetCentsJson(const Note& note)
{
    return controlJsonPitch(static_cast<double>(note.pitchOffset) * 100.0);
}

std::vector<std::size_t> controlNoteIndexOrder(const std::vector<Note>& notes)
{
    std::vector<std::size_t> order(notes.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&notes](std::size_t a, std::size_t b) { return notes[a].startTime < notes[b].startTime; });
    return order;
}

JsonValue controlJsonText(const std::string& utf8, std::size_t maxCodePoints)
{
    if (!isValidUtf8(utf8))
        return JsonValue::string(std::string());
    if (utf8CodePointCount(utf8) <= maxCodePoints)
        return JsonValue::string(utf8);
    std::size_t bytes = 0;
    for (std::size_t points = 0; bytes < utf8.size() && points < maxCodePoints; ++points) {
        const auto lead = static_cast<unsigned char>(utf8[bytes]);
        bytes += lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    }
    return JsonValue::string(utf8.substr(0, bytes));
}

std::optional<double> controlLinearToDb(double linear)
{
    if (!(linear > 0.0))
        return std::nullopt;
    return 20.0 * std::log10(linear);
}

const char* controlF0StateName(OriginalF0State state)
{
    switch (state) {
        case OriginalF0State::NotRequested:
            return "notRequested";
        case OriginalF0State::Extracting:
            return "extracting";
        case OriginalF0State::Ready:
            return "ready";
        case OriginalF0State::Failed:
            return "failed";
    }
    return "notRequested";
}

std::string controlNoteLabel(std::size_t index, std::optional<double> targetMidi, double startSec, double endSec)
{
    static const char* const pitchClassNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    // Fixed two decimals, independent of the C locale.
    const auto fixed2 = [](double value) {
        const auto hundredths = static_cast<long long>(std::llround(std::max(0.0, value) * 100.0));
        std::string fraction = std::to_string(hundredths % 100);
        if (fraction.size() < 2)
            fraction.insert(0, 1, '0');
        return std::to_string(hundredths / 100) + "." + fraction;
    };
    std::string name = "rest";
    if (targetMidi && *targetMidi > 0.0) {
        const int midi = static_cast<int>(std::lround(*targetMidi));
        name = std::string(pitchClassNames[((midi % 12) + 12) % 12]) + std::to_string(midi / 12 - 1);
    }
    return "#" + std::to_string(index) + " " + name + " " + fixed2(startSec) + "-" + fixed2(endSec) + "s";
}

// ---- Checking params ------------------------------------------------------------------------------

bool ControlParamReader::fail(std::string why)
{
    if (why_.empty())
        why_ = std::move(why);
    return false;
}

bool ControlParamReader::onlyMembers(std::initializer_list<std::string_view> names)
{
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (std::find(names.begin(), names.end(), std::string_view(params_.keyAt(i))) == names.end())
            return fail("params has a member the method does not define");
    return true;
}

bool ControlParamReader::optionalInteger(std::string_view name, std::int64_t min, std::int64_t max,
                                         std::optional<std::int64_t>& out)
{
    const JsonValue* value = member(name);
    if (value == nullptr)
        return true;
    const double number = value->asDouble();
    if (!value->isNumber() || std::floor(number) != number)
        return fail(std::string(name) + " must be an integer");
    if (number < static_cast<double>(min) || number > static_cast<double>(max))
        return fail(std::string(name) + " is out of range");
    out = static_cast<std::int64_t>(number);
    return true;
}

bool ControlParamReader::optionalNumber(std::string_view name, double min, double max, std::optional<double>& out)
{
    const JsonValue* value = member(name);
    if (value == nullptr)
        return true;
    if (!value->isNumber() || !std::isfinite(value->asDouble()))
        return fail(std::string(name) + " must be a number");
    if (value->asDouble() < min || value->asDouble() > max)
        return fail(std::string(name) + " is out of range");
    out = value->asDouble();
    return true;
}

bool ControlParamReader::optionalHandle(std::string_view name, ControlHandleKind kind,
                                        std::optional<ControlHandle>& out)
{
    const JsonValue* value = member(name);
    if (value == nullptr)
        return true;
    if (!value->isString())
        return fail(std::string(name) + " must be a string");
    out = controlParseHandle(kind, value->text());
    if (!out)
        return fail(std::string(name) + " does not have the form of a " + handleKindName(kind));
    return true;
}

bool ControlParamReader::optionalCursor(std::string_view name, char kind, std::optional<ControlCursor>& out)
{
    const JsonValue* value = member(name);
    if (value == nullptr)
        return true;
    if (!value->isString() || value->text().empty() || utf8CodePointCount(value->text()) > 512)
        return fail(std::string(name) + " must be a string of 1 to 512 characters");
    out = controlParseCursor(kind, value->text());
    if (!out)
        return fail(std::string(name) + " is not a cursor of this method");
    return true;
}

bool controlParseNoParams(const JsonValue& params, ControlNoParams&, std::string& why)
{
    ControlParamReader reader(params);
    if (reader.onlyMembers({}))
        return true;
    why = reader.why();
    return false;
}

bool controlParseRange(ControlParamReader& reader, std::string_view name, bool allowTimeline,
                       std::optional<ControlTimeRange>& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    if (!value->isObject() || value->size() != 2)
        return reader.fail(std::string(name) + " must be an object with a start and an end member");
    ControlTimeRange range;
    const JsonValue* start = value->find("startContentSec");
    const JsonValue* end = value->find("endContentSec");
    if (start == nullptr && end == nullptr && allowTimeline) {
        range.clock = ControlTimeRange::Clock::Timeline;
        start = value->find("startTimelineSec");
        end = value->find("endTimelineSec");
    }
    if (start == nullptr || end == nullptr)
        return reader.fail(std::string(name) + " must name its clock with startContentSec and endContentSec"
                           + (allowTimeline ? " or startTimelineSec and endTimelineSec" : ""));
    for (const JsonValue* bound : {start, end})
        if (!bound->isNumber() || !std::isfinite(bound->asDouble()) || bound->asDouble() < 0.0)
            return reader.fail(std::string(name) + " bounds must be non-negative numbers");
    range.start = start->asDouble();
    range.end = end->asDouble();
    out = range;
    return true;
}

// ---- Resolving handles ----------------------------------------------------------------------------

std::uint64_t controlContentEpoch(ControlOpContext& context)
{
    // StandaloneContentRepository::getContentEpoch(): the hook that counts clear(), which discards all
    // contents when a project is opened.
    const auto* repository = context.processor().getStandaloneContentRepository();
    return repository != nullptr ? repository->getContentEpoch() : 0;
}

std::string controlContentId(ControlOpContext& context, const ContentKey& key)
{
    return controlFormatHandle(ControlHandleKind::Content, context.instanceShort(), controlContentEpoch(context),
                               key.objectId);
}

std::string controlPlacementId(ControlOpContext& context, std::uint64_t placementId)
{
    return controlFormatHandle(ControlHandleKind::Placement, context.instanceShort(), controlContentEpoch(context),
                               placementId);
}

namespace {
/** A content that a clip of the arrangement shows is in the project, whatever an earlier track deletion marked. */
void reviveShownContents(ControlOpContext& context, ControlHistory& history)
{
    if (!history.hasRemovedContents())
        return;
    std::vector<ContentKey> shown;
    for (const auto& ref : controlLivePlacements(context))
        shown.push_back(ref.placement.contentKey);
    history.reviveShownContents(shown);
}
} // namespace

std::shared_ptr<const EditableContentSnapshot> controlLiveSnapshot(ControlOpContext& context, const ContentKey& key)
{
    auto& processor = context.processor();
    const auto* repository = processor.getStandaloneContentRepository();
    const auto* clip = repository != nullptr ? repository->findClip(key) : nullptr;
    if (clip == nullptr || clip->isRetired())
        return nullptr;
    // A content that a track deletion took out of the project is not live, although OpenTune leaks its clip.
    if (const auto history = context.history()) {
        reviveShownContents(context, *history);
        if (history->isContentRemoved(key, controlContentEpoch(context)))
            return nullptr;
    }
    return processor.getContentSnapshot(key);
}

const char* controlF0FailureCode(const EditableContentSnapshot& snapshot)
{
    // The message of the F0 extractor's result for audio without a voiced frame (F0ExtractionService::Result::
    // errorMessage), which requestContentRefresh stores unchanged.
    return snapshot.originalF0FailureReason == "f0_empty_or_unvoiced" ? "f0_empty" : "f0_failed";
}

Result<ControlContentRef> controlResolveContent(ControlOpContext& context, const ControlHandle& handle)
{
    if (!isCurrent(context, handle))
        return staleHandle(handle.kind);
    if (handle.objectOverflow || handle.object == 0)
        return unknownHandle(handle.kind);
    ControlContentRef ref;
    ref.key.domainKind = DomainKind::StandaloneClip;
    ref.key.objectId = handle.object;
    ref.snapshot = controlLiveSnapshot(context, ref.key);
    if (ref.snapshot == nullptr)
        return unknownHandle(handle.kind);
    ref.contentId = controlContentId(context, ref.key);
    return ref;
}

Result<ControlPlacementRef> controlResolvePlacement(ControlOpContext& context, const ControlHandle& handle)
{
    if (!isCurrent(context, handle))
        return staleHandle(handle.kind);
    if (handle.objectOverflow || handle.object == 0)
        return unknownHandle(handle.kind);
    const auto* arrangement = context.processor().getStandaloneArrangement();
    if (arrangement == nullptr)
        return unknownHandle(handle.kind);
    for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId) {
        ControlPlacementRef ref;
        if (arrangement->getPlacementById(trackId, handle.object, ref.placement) && !ref.placement.isRetired) {
            ref.trackId = trackId;
            ref.placementId = controlPlacementId(context, handle.object);
            return ref;
        }
    }
    return unknownHandle(handle.kind);
}

std::vector<ControlPlacementRef> controlLivePlacements(ControlOpContext& context)
{
    std::vector<ControlPlacementRef> out;
    const auto* arrangement = context.processor().getStandaloneArrangement();
    if (arrangement == nullptr)
        return out;
    for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId) {
        const int count = arrangement->getNumPlacements(trackId);
        for (int index = 0; index < count; ++index) {
            ControlPlacementRef ref;
            if (!arrangement->getPlacementByIndex(trackId, index, ref.placement) || ref.placement.isRetired)
                continue;
            ref.trackId = trackId;
            ref.placementId = controlPlacementId(context, ref.placement.placementId);
            out.push_back(std::move(ref));
        }
    }
    return out;
}

std::vector<ContentKey> controlLiveContents(ControlOpContext& context)
{
    std::vector<ContentKey> out;
    const auto* repository = context.processor().getStandaloneContentRepository();
    if (repository == nullptr)
        return out;
    const auto history = context.history();
    if (history != nullptr)
        reviveShownContents(context, *history);
    const auto epoch = controlContentEpoch(context);
    for (const auto& key : repository->getAllClips())
        if (!repository->isRetired(key) && !(history != nullptr && history->isContentRemoved(key, epoch)))
            out.push_back(key);
    return out;
}

double controlContentToTimeline(const ControlPlacementRef& placement, const EditableContentSnapshot& snapshot,
                                double contentSeconds)
{
    const auto& p = placement.placement;
    if (snapshot.timeGrid == nullptr)
        return p.timelineStartSeconds + contentSeconds - p.clipInSeconds;
    return p.timelineStartSeconds + snapshot.timeGrid->tauForward(contentSeconds)
           - snapshot.timeGrid->tauForward(p.clipInSeconds);
}

double controlTimelineToContent(const ControlPlacementRef& placement, const EditableContentSnapshot& snapshot,
                                double timelineSeconds)
{
    const auto& p = placement.placement;
    if (snapshot.timeGrid == nullptr)
        return timelineSeconds - p.timelineStartSeconds + p.clipInSeconds;
    return snapshot.timeGrid->tauInverse(snapshot.timeGrid->tauForward(p.clipInSeconds) + timelineSeconds
                                         - p.timelineStartSeconds);
}

int controlVisibleTrackCount(ControlOpContext& context, bool& derived)
{
    if (auto* bridge = context.bridge())
        if (const auto count = bridge->visibleTrackCount()) {
            derived = false;
            return std::max(0, *count);
        }
    derived = true;
    int lastTrackWithClip = -1;
    for (const auto& placement : controlLivePlacements(context))
        lastTrackWithClip = std::max(lastTrackWithClip, placement.trackId);
    return std::max(1, lastTrackWithClip + 1);
}

// ---- Revision tokens ------------------------------------------------------------------------------

std::string ControlContentVersions::revision() const
{
    return ControlTokenHasher('r').add(notes).add(pitch).add(content).token();
}

JsonValue ControlContentVersions::tokensJson() const
{
    auto json = JsonValue::object();
    json.appendMember("contentId", JsonValue::string(contentId));
    json.appendMember("revision", JsonValue::string(revision()));
    json.appendMember("gridRevision", JsonValue::string(grid));
    json.appendMember("keyRevision", JsonValue::string(key));
    return json;
}

ControlContentVersions controlContentVersions(ControlOpContext& context, const std::string& contentId,
                                              const EditableContentSnapshot& snapshot)
{
    ControlContentVersions versions;
    versions.contentId = contentId;
    // The counters without the steps of OpenTune's pitch shifts, which advance both (applyPitchShiftState()).
    ControlPitchShiftLedger::Counters counters{snapshot.notesRevision, snapshot.pitchRevision};
    if (const auto history = context.history())
        counters = history->pitchShiftLedger().observe(contentId, controlContentEpoch(context), snapshot);
    versions.notes = ControlTokenHasher('n').add(counters.notes).token();
    versions.pitch = ControlTokenHasher('p').add(counters.pitch).token();
    const auto& curve = snapshot.pitchCurve;
    versions.content = ControlTokenHasher('c')
                           .add(snapshot.audioRevision)
                           .add(static_cast<int>(snapshot.originalF0State))
                           .add(static_cast<std::uint64_t>(curve != nullptr ? curve->size() : 0))
                           .add(curve != nullptr ? curve->getHopSize() : 0)
                           .add(curve != nullptr ? curve->getSampleRate() : 0.0)
                           .add(static_cast<std::uint64_t>(snapshot.silentGaps.size()))
                           .token();
    versions.grid = ControlTokenHasher('g').add(snapshot.timeGridRevision).token();
    const auto& key = snapshot.detectedKey;
    versions.key = ControlTokenHasher('k')
                       .add(static_cast<int>(key.root))
                       .add(static_cast<int>(key.scale))
                       .add(static_cast<int>(key.origin))
                       .add(static_cast<double>(key.confidence))
                       .token();
    versions.pitchShift = ControlTokenHasher('s')
                              .add(snapshot.pitchShiftSettings.semitone)
                              .add(snapshot.pitchShiftSettings.cents)
                              .token();
    return versions;
}

std::string controlArrangementRevision(ControlOpContext& context)
{
    ControlTokenHasher hasher('a');
    hasher.add(controlContentEpoch(context));
    for (const auto& ref : controlLivePlacements(context)) {
        const auto& p = ref.placement;
        hasher.add(ref.trackId)
            .add(p.placementId)
            .add(p.contentKey.objectId)
            .add(p.timelineStartSeconds)
            .add(p.durationSeconds)
            .add(p.clipInSeconds)
            .add(static_cast<double>(p.gain))
            .add(p.fadeInDuration)
            .add(p.fadeOutDuration)
            .add(std::string_view(p.name.toRawUTF8()));
    }
    return hasher.token();
}

std::string controlTrackStateVersion(ControlOpContext& context)
{
    ControlTokenHasher hasher('t');
    const auto* arrangement = context.processor().getStandaloneArrangement();
    if (arrangement == nullptr)
        return hasher.token();
    bool derived = false;
    const int visible = controlVisibleTrackCount(context, derived);
    for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId)
        hasher.add(arrangement->isTrackMuted(trackId))
            .add(arrangement->isTrackSolo(trackId))
            .add(static_cast<double>(arrangement->getTrackVolume(trackId)))
            .add(static_cast<std::uint64_t>(arrangement->getTrackColour(trackId).getARGB()))
            .add(trackId < visible);
    return hasher.token();
}

std::string controlGeometryHash(const ControlPlacementRef& placement)
{
    const auto& p = placement.placement;
    return ControlTokenHasher('h')
        .add(placement.trackId)
        .add(p.timelineStartSeconds)
        .add(p.durationSeconds)
        .add(p.clipInSeconds)
        .token();
}

ControlWorldVersions controlCaptureWorldVersions(ControlOpContext& context)
{
    ControlWorldVersions world;
    world.arrangement = controlArrangementRevision(context);
    world.trackState = controlTrackStateVersion(context);
    for (const auto& key : controlLiveContents(context))
        if (const auto snapshot = controlLiveSnapshot(context, key))
            world.contents.push_back(controlContentVersions(context, controlContentId(context, key), *snapshot));
    return world;
}

JsonValue controlAsOf(const std::string& arrangementRevision, const std::vector<ControlContentVersions>& contents)
{
    auto asOf = JsonValue::object();
    asOf.appendMember("arrangementRevision", JsonValue::string(arrangementRevision));
    auto list = JsonValue::array();
    for (const auto& versions : contents)
        list.append(versions.tokensJson());
    asOf.appendMember("contents", std::move(list));
    return asOf;
}

std::string controlHashJson(char prefix, const JsonValue& value)
{
    return ControlTokenHasher(prefix).add(toJsonString(value)).token();
}

// ---- Audio device ---------------------------------------------------------------------------------

ControlAudioDeviceState controlAudioDeviceState(ControlOpContext& context)
{
    ControlAudioDeviceState state;
    auto* bridge = context.bridge();
    if (bridge == nullptr) {
        state.nullReason = "no_editor";
        return state;
    }
    // ControlUiBridge::audioDevice(): the Standalone application's device manager, owned by the editor.
    auto* manager = bridge->audioDevice();
    if (manager == nullptr) {
        state.nullReason = "unavailable";
        return state;
    }
    state.known = true;
    auto* device = manager->getCurrentAudioDevice();
    if (device == nullptr) {
        state.nullReason = "no_audio_device";
        return state;
    }
    state.running = device->isPlaying();
    state.name = device->getName().toStdString();
    if (device->getCurrentSampleRate() > 0.0)
        state.sampleRateHz = device->getCurrentSampleRate();
    if (device->getCurrentBufferSizeSamples() > 0)
        state.bufferSizeFrame = device->getCurrentBufferSizeSamples();
    state.nullReason = "unavailable";
    return state;
}

JsonValue controlTransportJson(ControlOpContext& context, std::optional<double> positionOverrideSec)
{
    auto& processor = context.processor();
    auto json = JsonValue::object();
    // The Standalone transport accessors of the processor (canonical PlayHeadState, BPM, time signature).
    json.appendMember("playing", JsonValue::boolean(processor.isPlaying()));
    json.appendMember(
        "positionTimelineSec",
        controlJsonSeconds(std::max(0.0, positionOverrideSec ? *positionOverrideSec : processor.getPosition())));
    json.appendMember("playStartTimelineSec", controlJsonSeconds(std::max(0.0, processor.getPlayStartPosition())));
    json.appendMember("loopEnabled", JsonValue::boolean(processor.isLoopEnabled()));
    json.appendMember("bpm", controlJsonNumber(processor.getBpm(), 3));
    auto signature = JsonValue::object();
    signature.appendMember("numerator", JsonValue::integer(processor.getTimeSigNumerator()));
    signature.appendMember("denominator", JsonValue::integer(processor.getTimeSigDenominator()));
    json.appendMember("timeSignature", std::move(signature));
    json.appendMember("audioDeviceRunning", JsonValue::boolean(controlAudioDeviceState(context).running));
    // getDiagnosticInfo(): the last play, pause, stop or seek as recordControlCall() noted it. Track -1 reads
    // only these fields.
    const auto diagnostics = processor.getDiagnosticInfo(-1, 0);
    const auto call = diagnostics.lastControlCall.toStdString();
    if (call.empty() || call == "none" || diagnostics.lastControlTimestamp <= 0) {
        json.appendMember("lastControl", JsonValue());
    } else {
        auto lastControl = JsonValue::object();
        lastControl.appendMember("call", controlJsonText(call, 64));
        lastControl.appendMember("atMs", JsonValue::integer(diagnostics.lastControlTimestamp));
        json.appendMember("lastControl", std::move(lastControl));
    }
    return json;
}

} // namespace OpenTune
