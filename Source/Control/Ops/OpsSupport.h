// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "OpsRegistry.h"

#include "Control/ContentHandles.h"
#include "Content/ContentKey.h"
#include "StandaloneArrangement.h"
#include "Utils/ContentAnalysisState.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenTune {

struct EditableContentSnapshot;
struct Note;

// ---- Building results ----------------------------------------------------------------------------

/** A JSON number rounded to the given number of decimals, written without exponent and without
    trailing zeros, independent of the C locale. Non-finite values are written as 0. */
JsonValue controlJsonNumber(double value, int decimals);
/** Seconds, rounded to 6 decimals as PROTOCOL.md section 8.2 recommends. */
inline JsonValue controlJsonSeconds(double value)
{
    return controlJsonNumber(value, 6);
}
/** MIDI note numbers and cents, rounded to 4 decimals (one ten-thousandth of a semitone). */
inline JsonValue controlJsonPitch(double value)
{
    return controlJsonNumber(value, 4);
}
/** The target pitch of a note in MIDI, as every method reports it: PitchUtils::freqToMidi() of
    Note::getAdjustedPitch() at tuningHz (MIDI 0 for a note without a base pitch). */
double controlNoteTargetMidi(const Note& note, float tuningHz);
/** The one place that rounds a note's target for the wire: content.getNotes and the targetMidi change of a write
    result both call it, so the two never round the same note differently. (That the "after" of a write result
    equals the value read back also needs the same note: controlListNoteChanges() takes it from the notes read back
    after the commit, at the index content.getNotes gives it.) */
JsonValue controlNoteTargetMidiJson(const Note& note, float tuningHz);
/** A note's cent offset (Note::pitchOffset in semitones, times 100), rounded as controlJsonPitch(); the one place
    for content.getNotes and write results. */
JsonValue controlNoteOffsetCentsJson(const Note& note);
/** The order in which content.getNotes counts the notes of a content and write results reference them: the
    positions in notes sorted by start time, notes that start together in stored order. OpenTune's own edits store
    the notes in this order (normalizeStoredNotes()), but not every store sorts them: a project is opened with its
    notes applied as the file holds them (StandaloneClipContent::applyNotes()). Pure. */
std::vector<std::size_t> controlNoteIndexOrder(const std::vector<Note>& notes);
/** A JSON string from UTF-8 text, at most maxCodePoints code points (text from projects is untrusted
    data: it is returned as a string value only, PROTOCOL.md section 20.2). */
JsonValue controlJsonText(const std::string& utf8, std::size_t maxCodePoints);
/** Gain in dB of a linear factor; nullopt for 0 (minus infinity dB). */
std::optional<double> controlLinearToDb(double linear);

/** The f0State string of an OpenTune analysis state (notRequested, extracting, ready, failed). */
const char* controlF0StateName(OriginalF0State state);

/** The display label of a note, for example "#12 A4 41.20-41.85s" (index, the target's name at the instance's
    tuning or "rest", content seconds). Display only (PROTOCOL.md section 9). */
std::string controlNoteLabel(std::size_t index, std::optional<double> targetMidi, double startSec, double endSec);

// ---- Checking params on the network thread --------------------------------------------------------

/**
 * Checks the params of one request against its closed params schema. Each check returns false and
 * keeps the first reason when the params do not validate. Network thread; touches no OpenTune state.
 */
class ControlParamReader
{
public:
    explicit ControlParamReader(const JsonValue& params) : params_(params) {}

    /** Fails when params has a member that is not in names (closed schemas, PROTOCOL.md section 2.3). */
    bool onlyMembers(std::initializer_list<std::string_view> names);
    /** The member, or nullptr when absent. */
    const JsonValue* member(std::string_view name) const { return params_.find(name); }

    /** An optional integer member in [min, max]. */
    bool optionalInteger(std::string_view name, std::int64_t min, std::int64_t max, std::optional<std::int64_t>& out);
    /** An optional number member in [min, max]. */
    bool optionalNumber(std::string_view name, double min, double max, std::optional<double>& out);
    /** An optional handle member of the given kind (form check only). */
    bool optionalHandle(std::string_view name, ControlHandleKind kind, std::optional<ControlHandle>& out);
    /** An optional cursor member (common.schema.json#/$defs/cursor) issued by the method of that kind. */
    bool optionalCursor(std::string_view name, char kind, std::optional<ControlCursor>& out);

    /** Records a failure; returns false. */
    bool fail(std::string why);
    const std::string& why() const noexcept { return why_; }

private:
    const JsonValue& params_;
    std::string why_;
};

/** The params of a method that takes none: an empty, closed object. */
struct ControlNoParams
{};

/** Checks that params is empty. */
bool controlParseNoParams(const JsonValue& params, ControlNoParams& out, std::string& why);

/** A time range with the clock its member names state (common.schema.json#/$defs/timeRange). */
struct ControlTimeRange
{
    enum class Clock {
        Content,
        Timeline
    };
    Clock clock = Clock::Content;
    double start = 0.0;
    double end = 0.0;
};

/** Parses an optional range member that may use the given clocks (content and/or timeline). */
bool controlParseRange(ControlParamReader& reader, std::string_view name, bool allowTimeline,
                       std::optional<ControlTimeRange>& out);

// ---- Resolving handles on the message thread ----------------------------------------------------

/** A live content that a contentId names. */
struct ControlContentRef
{
    ContentKey key;
    std::string contentId;
    std::shared_ptr<const EditableContentSnapshot> snapshot;
};

/** A live placement that a placementId names. */
struct ControlPlacementRef
{
    int trackId = 0;
    StandaloneArrangement::Placement placement;
    std::string placementId;
};

/** The content epoch of the instance: it changes whenever all contents are discarded at once. */
std::uint64_t controlContentEpoch(ControlOpContext& context);
/** The contentId of a content key in the current epoch. */
std::string controlContentId(ControlOpContext& context, const ContentKey& key);
/** The placementId of a placement in the current epoch. */
std::string controlPlacementId(ControlOpContext& context, std::uint64_t placementId);

/**
 * PROTOCOL.md section 9 rule 1 after the form check: a handle of another instance or of an earlier
 * epoch gives NOT_FOUND(stale_handle), a current handle that names no live content gives
 * NOT_FOUND(unknown_handle). A retired content (its clip was deleted) is not live.
 */
Result<ControlContentRef> controlResolveContent(ControlOpContext& context, const ControlHandle& handle);
/** As controlResolveContent, for placements. A retired placement (deleted clip) is not live. */
Result<ControlPlacementRef> controlResolvePlacement(ControlOpContext& context, const ControlHandle& handle);
/** The live content of a key, or nullptr. */
std::shared_ptr<const EditableContentSnapshot> controlLiveSnapshot(ControlOpContext& context, const ContentKey& key);

/**
 * The reason code of a failed pitch analysis (common.schema.json reasonCode), from the failure message that OpenTune
 * keeps with the content (EditableContentSnapshot::originalF0FailureReason, set by
 * OpenTuneAudioProcessor::setContentOriginalF0FailureReason when the analysis fails): "f0_empty" when the F0
 * extraction found no voiced frame (it fails with the message "f0_empty_or_unvoiced"), "f0_failed" for every other
 * failure, also one without a message, as a failure loaded from a project has. Only meaningful while the analysis
 * state is Failed (OpenTune clears the message when the state changes).
 */
const char* controlF0FailureCode(const EditableContentSnapshot& snapshot);

/** Every live placement, by track and then in the order of the track. */
std::vector<ControlPlacementRef> controlLivePlacements(ControlOpContext& context);
/** Every live content key, in the order of the content repository. */
std::vector<ContentKey> controlLiveContents(ControlOpContext& context);

/**
 * Timeline seconds of a content time through a placement: the placement's window starts at
 * tauForward(clipIn) of the content's time grid, as the piano roll projects it
 * (makePianoRollProjection in Source/Standalone/PluginEditor.cpp).
 */
double controlContentToTimeline(const ControlPlacementRef& placement, const EditableContentSnapshot& snapshot,
                                double contentSeconds);
/** The inverse of controlContentToTimeline. */
double controlTimelineToContent(const ControlPlacementRef& placement, const EditableContentSnapshot& snapshot,
                                double timelineSeconds);

/** How many track slots the arrangement shows: the editor's count, or, without an editor, the slots up
    to the last one that holds a clip (at least 1). derived is set when the value was computed. */
int controlVisibleTrackCount(ControlOpContext& context, bool& derived);

// ---- Revision tokens and the world revision vector ------------------------------------------------

/**
 * The components of the world revision vector of one content (PROTOCOL.md section 14.4), as opaque
 * tokens read from the content's current snapshot:
 * - notes: the notes and their parameters (the content's notes revision, without the steps of its pitch
 *   shifts: ControlPitchShiftLedger);
 * - pitch: the corrected pitch curve (pitch revision, likewise without the pitch-shift steps);
 * - content: the audio and the analysis data (audio and analysis revisions and the pitch-analysis state);
 * - grid: the time grid (time-grid revision);
 * - key: the key (root, scale, origin and confidence);
 * - pitchShift: the clip pitch shift (semitones and cents).
 */
struct ControlContentVersions
{
    std::string contentId;
    std::string notes;
    std::string pitch;
    std::string content;
    std::string grid;
    std::string key;
    std::string pitchShift;

    /** The content's `revision` token: changes exactly when notes, pitch or content changes. */
    std::string revision() const;
    /** {contentId, revision, gridRevision, keyRevision} (common.schema.json#/$defs/contentTokens). */
    JsonValue tokensJson() const;
};

/** The world revision vector: arrangement, track states and every live content. */
struct ControlWorldVersions
{
    std::string arrangement;
    std::string trackState;
    std::vector<ControlContentVersions> contents;
};

/**
 * The versions of one content from its snapshot. A pitch shift must not change `revision` (PROTOCOL.md section
 * 10 rule 2), but OpenTune's applyPitchShiftState() advances the notes and pitch counters: the instance's
 * ControlPitchShiftLedger (ControlOpContext::history()) observes the snapshot and leaves those steps out.
 * Without a history (a context of the unit tests) the counters are used as they are.
 */
ControlContentVersions controlContentVersions(ControlOpContext& context, const std::string& contentId,
                                              const EditableContentSnapshot& snapshot);
/** The arrangementRevision token: every live placement with all its properties, and the content epoch. */
std::string controlArrangementRevision(ControlOpContext& context);
/** The trackState component: mute, solo, volume, colour and visibility of every track slot. */
std::string controlTrackStateVersion(ControlOpContext& context);
/** The geometryHash of a placement: its track, timeline start, length and source window. */
std::string controlGeometryHash(const ControlPlacementRef& placement);
/** Captures the whole world revision vector, on the message thread, from one consistent state. Shared
    by the reads (asOf) and the write envelopes (revisionDiff). */
ControlWorldVersions controlCaptureWorldVersions(ControlOpContext& context);

/** The asOf member of a read result (common.schema.json#/$defs/asOf). */
JsonValue controlAsOf(const std::string& arrangementRevision, const std::vector<ControlContentVersions>& contents);

/** A hash of a JSON value as a token, for cursors (selection params and the asOf of a read). */
std::string controlHashJson(char prefix, const JsonValue& value);

// ---- Audio device ---------------------------------------------------------------------------------

/** The audio device as far as the control API can see it. */
struct ControlAudioDeviceState
{
    /** The device manager was reachable, so running and the null members are facts, not unknowns. */
    bool known = false;
    bool running = false;
    std::optional<std::string> name;
    std::optional<double> sampleRateHz;
    std::optional<int> bufferSizeFrame;
    /** Reason of the nulls (registry/reasons.json, scope null): no_editor without a bridge, unavailable
        when the bridge gives no device manager or the device does not report a value, no_audio_device
        when no device is open. */
    std::string nullReason;
};

/** The audio device through the editor's bridge (ControlUiBridge::audioDevice()). Without it nothing is
    known: running is false and the other members are null. Message thread. */
ControlAudioDeviceState controlAudioDeviceState(ControlOpContext& context);

/** The transport object (common.schema.json#/$defs/transport) from the processor's transport state. With
    positionOverrideSec the position member is that value instead of the processor's: transport.control reports
    the position that a command it has just issued is about to give the audio thread (OpenTune's transport
    commands are carried out by the audio callback, a block later). */
JsonValue controlTransportJson(ControlOpContext& context, std::optional<double> positionOverrideSec = std::nullopt);

} // namespace OpenTune
