// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContentReadOps.h"

#include "OpsSupport.h"

#include "Content/EditableContentSnapshot.h"
#include "PluginProcessor.h"
#include "Utils/LegacyNoteGenerator.h"
#include "Utils/PitchControlConfig.h"
#include "Utils/PitchUtils.h"
#include "Utils/TuningConfig.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

constexpr std::int64_t kNotesDefaultLimit = 200;
constexpr std::int64_t kNotesMaxLimit = 500;
constexpr std::int64_t kCurveDefaultLimit = 10000;
constexpr std::int64_t kCurveMaxLimit = 10000;
constexpr std::int64_t kCurveMaxStepFrame = 1000;
/** Frame length of the pitch analysis before one exists: the F0 extractor's hop of 160 samples at
    16 kHz. Reported as derived. */
constexpr double kAnalysisHopSec = 160.0 / 16000.0;
/** Tolerance when a range bound falls on a frame time. */
constexpr double kFrameEpsilon = 1e-9;

constexpr char kNotesCursor = 'n';
constexpr char kCurveCursor = 'p';

double midiOf(float hz, float tuningHz)
{
    // PitchUtils::freqToMidi(): MIDI note number of a frequency at the given A4.
    return PitchUtils::freqToMidi(hz, tuningHz);
}

/** The asOf of a read of one content, and the hash a cursor of that read carries. */
struct ContentReadState
{
    JsonValue asOf;
    std::string tokensHash;
};

/**
 * The asOf of a read of one content, and the hash its cursors carry: the tokens of asOf and the content's pitch
 * shift. A pitch shift does not change `revision`, but it changes what both paged reads return: OpenTune scales
 * the stored notes and correction segments with it (commitPitchShiftEdit()), which moves every target of
 * content.getNotes, and renders the original F0 with its gain (EditableContentSnapshot::forEachEffectiveF0Span()),
 * which moves every correctedMidi of content.getPitchCurve. So a page read after a shift (or after its undo) must
 * not continue one read before it (PROTOCOL.md section 10 rule 6).
 */
ContentReadState readState(ControlOpContext& context, const std::string& contentId,
                           const EditableContentSnapshot& snapshot)
{
    ContentReadState state;
    const auto versions = controlContentVersions(context, contentId, snapshot);
    state.asOf = controlAsOf(controlArrangementRevision(context), {versions});
    auto bound = JsonValue::array();
    bound.append(state.asOf);
    bound.append(JsonValue::string(versions.pitchShift));
    state.tokensHash = controlHashJson('t', bound);
    return state;
}

Error cursorStale()
{
    return controlOpError("REVISION_CONFLICT", "cursor_stale",
                          "the cursor was issued for tokens that are no longer current; read again without it");
}

bool failWith(ControlParamReader& reader, std::string& why)
{
    why = reader.why();
    return false;
}

void addRange(ControlTokenHasher& hasher, const std::optional<ControlTimeRange>& range)
{
    hasher.add(range.has_value());
    if (range)
        hasher.add(static_cast<int>(range->clock)).add(range->start).add(range->end);
}

// ---- content.getNotes -----------------------------------------------------------------------------

struct NotesParams
{
    std::optional<ControlHandle> contentId;
    std::optional<ControlHandle> placementId;
    std::optional<ControlTimeRange> range;
    std::optional<ControlCursor> cursor;
    std::int64_t limit = kNotesDefaultLimit;
    std::string selectionHash;
};

bool parseNotesParams(const JsonValue& params, NotesParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<std::int64_t> limit;
    if (!reader.onlyMembers({"contentId", "placementId", "range", "cursor", "limit"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, out.contentId)
        || !reader.optionalHandle("placementId", ControlHandleKind::Placement, out.placementId))
        return failWith(reader, why);
    if (out.contentId.has_value() == out.placementId.has_value()) {
        reader.fail("exactly one of contentId and placementId is required");
        return failWith(reader, why);
    }
    // A timeline range needs a placement that maps it to the content (schema: if contentId then a
    // content range).
    if (!controlParseRange(reader, "range", out.placementId.has_value(), out.range)
        || !reader.optionalCursor("cursor", kNotesCursor, out.cursor)
        || !reader.optionalInteger("limit", 1, kNotesMaxLimit, limit))
        return failWith(reader, why);
    out.limit = limit.value_or(kNotesDefaultLimit);
    ControlTokenHasher hasher('q');
    hasher.add(std::string_view("content.getNotes"))
        .add(std::string_view(out.contentId ? params.find("contentId")->text() : params.find("placementId")->text()));
    addRange(hasher, out.range);
    out.selectionHash = hasher.token();
    if (out.cursor && out.cursor->paramsHash != out.selectionHash) {
        reader.fail("the cursor was issued for other params of this method");
        return failWith(reader, why);
    }
    return true;
}

JsonValue noteJson(std::size_t index, const Note& note, const EditableContentSnapshot& snapshot,
                   const std::optional<ControlPlacementRef>& placement, float tuningHz)
{
    auto json = JsonValue::object();
    const double targetMidi = controlNoteTargetMidi(note, tuningHz);
    const bool hasDetected = note.isVoiced && note.originalPitch > 0.0f;
    json.appendMember("index", JsonValue::integer(static_cast<std::int64_t>(index)));
    json.appendMember("label", JsonValue::string(controlNoteLabel(
                                   index, note.isVoiced ? std::optional<double>(targetMidi) : std::nullopt,
                                   note.startTime, note.endTime)));
    json.appendMember("startContentSec", controlJsonSeconds(std::max(0.0, note.startTime)));
    json.appendMember("endContentSec", controlJsonSeconds(std::max(0.0, note.endTime)));
    JsonValue startTimeline;
    JsonValue endTimeline;
    if (placement) {
        // Through the named placement; a time before the timeline start has no timeline position.
        const double start = controlContentToTimeline(*placement, snapshot, note.startTime);
        const double end = controlContentToTimeline(*placement, snapshot, note.endTime);
        if (start >= 0.0)
            startTimeline = controlJsonSeconds(start);
        if (end >= 0.0)
            endTimeline = controlJsonSeconds(end);
    }
    json.appendMember("startTimelineSec", std::move(startTimeline));
    json.appendMember("endTimelineSec", std::move(endTimeline));
    json.appendMember("detectedMidi",
                      hasDetected ? controlJsonPitch(midiOf(note.originalPitch, tuningHz)) : JsonValue());
    // The target includes pitchOffset, the part that comes from cent shifts (Note::getAdjustedPitch()).
    json.appendMember("targetMidi", controlNoteTargetMidiJson(note, tuningHz));
    json.appendMember("offsetCents", controlNoteOffsetCentsJson(note));
    json.appendMember("retuneSpeed", controlJsonNumber(note.retuneSpeed, 4));
    json.appendMember("vibratoDepth", controlJsonNumber(note.vibratoDepth, 4));
    json.appendMember("vibratoRateHz", controlJsonNumber(note.vibratoRate, 4));
    json.appendMember("pitchDriftScale", controlJsonNumber(note.pitchDriftScale, 4));
    json.appendMember("voiced", JsonValue::boolean(note.isVoiced));
    bool corrected = false;
    if (snapshot.hasUsableOriginalF0()) {
        const double hopSec = snapshot.pitchCurve->getHopSize() / snapshot.pitchCurve->getSampleRate();
        const int startFrame = static_cast<int>(std::floor(note.startTime / hopSec + kFrameEpsilon));
        const int endFrame = static_cast<int>(std::ceil(note.endTime / hopSec - kFrameEpsilon));
        // PitchCurveSnapshot::hasCorrectionInRange(): a correction segment covers part of the note.
        corrected =
            endFrame > startFrame && snapshot.pitchCurve->hasCorrectionInRange(std::max(0, startFrame), endFrame);
    }
    json.appendMember("corrected", JsonValue::boolean(corrected));
    return json;
}

Result<JsonValue> runGetNotes(ControlOpContext& context, const NotesParams& params)
{
    std::optional<ControlPlacementRef> placement;
    ControlContentRef content;
    if (params.placementId) {
        auto resolved = controlResolvePlacement(context, *params.placementId);
        if (!resolved.ok())
            return resolved.error();
        placement = std::move(resolved).value();
        content.key = placement->placement.contentKey;
        content.snapshot = controlLiveSnapshot(context, content.key);
        if (content.snapshot == nullptr)
            return controlOpError("NOT_FOUND", "unknown_handle", "the placement shows no live content");
        content.contentId = controlContentId(context, content.key);
    } else {
        auto resolved = controlResolveContent(context, *params.contentId);
        if (!resolved.ok())
            return resolved.error();
        content = std::move(resolved).value();
    }
    const auto& snapshot = *content.snapshot;
    const auto state = readState(context, content.contentId, snapshot);
    if (params.cursor && params.cursor->tokensHash != state.tokensHash)
        return cursorStale();

    // Index order: the notes ordered by start time, as write results reference them (controlNoteIndexOrder).
    const auto order = controlNoteIndexOrder(snapshot.notes);
    std::vector<std::size_t> selected; // positions in order
    for (std::size_t position = 0; position < order.size(); ++position) {
        const auto& note = snapshot.notes[order[position]];
        if (params.range) {
            double start = params.range->start;
            double end = params.range->end;
            if (params.range->clock == ControlTimeRange::Clock::Timeline) {
                start = controlTimelineToContent(*placement, snapshot, start);
                end = controlTimelineToContent(*placement, snapshot, end);
            }
            if (!(note.endTime > start && note.startTime < end))
                continue;
        }
        selected.push_back(position);
    }

    const auto total = static_cast<std::uint64_t>(selected.size());
    const auto offset = std::min<std::uint64_t>(params.cursor ? params.cursor->offset : 0, total);
    const auto end = std::min<std::uint64_t>(total, offset + static_cast<std::uint64_t>(params.limit));
    const float tuningHz = TuningConfig::currentTuningHz();
    auto notes = JsonValue::array();
    for (auto i = offset; i < end; ++i) {
        const auto position = selected[static_cast<std::size_t>(i)];
        notes.append(noteJson(position, snapshot.notes[order[position]], snapshot, placement, tuningHz));
    }

    auto result = JsonValue::object();
    result.appendMember("asOf", state.asOf);
    auto derived = JsonValue::array();
    for (const char* path : {"notes[].label", "notes[].targetMidi", "notes[].detectedMidi", "notes[].corrected"})
        derived.append(JsonValue::string(path));
    if (placement) {
        derived.append(JsonValue::string("notes[].startTimelineSec"));
        derived.append(JsonValue::string("notes[].endTimelineSec"));
    }
    result.appendMember("derived", std::move(derived));
    result.appendMember("contentId", JsonValue::string(content.contentId));
    result.appendMember("placementId", placement ? JsonValue::string(placement->placementId) : JsonValue());
    result.appendMember("tuningHz", controlJsonNumber(tuningHz, 3));
    result.appendMember("totalCount", JsonValue::integer(static_cast<std::int64_t>(total)));
    result.appendMember("notes", std::move(notes));
    result.appendMember("nextCursor", end < total ? JsonValue::string(controlFormatCursor(
                                                        {kNotesCursor, end, params.selectionHash, state.tokensHash}))
                                                  : JsonValue());
    return result;
}

// ---- content.getPitchCurve ------------------------------------------------------------------------

struct CurveParams
{
    std::optional<ControlHandle> contentId;
    std::optional<ControlTimeRange> range;
    std::optional<ControlCursor> cursor;
    std::int64_t stepFrame = 1;
    std::int64_t limit = kCurveDefaultLimit;
    std::string selectionHash;
};

bool parseCurveParams(const JsonValue& params, CurveParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<std::int64_t> stepFrame;
    std::optional<std::int64_t> limit;
    if (!reader.onlyMembers({"contentId", "range", "stepFrame", "cursor", "limit"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, out.contentId))
        return failWith(reader, why);
    if (!out.contentId) {
        reader.fail("contentId is required");
        return failWith(reader, why);
    }
    if (!controlParseRange(reader, "range", false, out.range)
        || !reader.optionalInteger("stepFrame", 1, kCurveMaxStepFrame, stepFrame)
        || !reader.optionalCursor("cursor", kCurveCursor, out.cursor)
        || !reader.optionalInteger("limit", 1, kCurveMaxLimit, limit))
        return failWith(reader, why);
    out.stepFrame = stepFrame.value_or(1);
    out.limit = limit.value_or(kCurveDefaultLimit);
    ControlTokenHasher hasher('q');
    hasher.add(std::string_view("content.getPitchCurve"))
        .add(std::string_view(params.find("contentId")->text()))
        .add(out.stepFrame);
    addRange(hasher, out.range);
    out.selectionHash = hasher.token();
    if (out.cursor && out.cursor->paramsHash != out.selectionHash) {
        reader.fail("the cursor was issued for other params of this method");
        return failWith(reader, why);
    }
    return true;
}

JsonValue midiOrNull(float hz, float tuningHz)
{
    return hz > 0.0f && std::isfinite(hz) ? controlJsonPitch(midiOf(hz, tuningHz)) : JsonValue();
}

Result<JsonValue> runGetPitchCurve(ControlOpContext& context, const CurveParams& params)
{
    auto resolved = controlResolveContent(context, *params.contentId);
    if (!resolved.ok())
        return resolved.error();
    const auto content = std::move(resolved).value();
    const auto& snapshot = *content.snapshot;
    const auto state = readState(context, content.contentId, snapshot);
    if (params.cursor && params.cursor->tokensHash != state.tokensHash)
        return cursorStale();

    const bool usable = snapshot.hasUsableOriginalF0();
    const double hopSec =
        usable ? snapshot.pitchCurve->getHopSize() / snapshot.pitchCurve->getSampleRate() : kAnalysisHopSec;
    const auto frameCount = usable ? static_cast<std::int64_t>(snapshot.pitchCurve->size()) : 0;
    std::int64_t firstFrame = 0;
    std::int64_t endFrame = frameCount; // exclusive
    if (params.range) {
        firstFrame = static_cast<std::int64_t>(std::ceil(params.range->start / hopSec - kFrameEpsilon));
        endFrame = std::min<std::int64_t>(
            frameCount, static_cast<std::int64_t>(std::ceil(params.range->end / hopSec - kFrameEpsilon)));
    }
    const std::int64_t step = params.stepFrame;
    const std::int64_t total = endFrame > firstFrame ? (endFrame - firstFrame + step - 1) / step : 0;
    const auto offset =
        std::min<std::int64_t>(params.cursor ? static_cast<std::int64_t>(params.cursor->offset) : 0, total);
    const auto count = std::min<std::int64_t>(params.limit, total - offset);
    const std::int64_t startFrame = firstFrame + offset * step;

    const float tuningHz = TuningConfig::currentTuningHz();
    auto original = JsonValue::array();
    auto corrected = JsonValue::array();
    if (count > 0) {
        const auto spanEnd = startFrame + (count - 1) * step + 1;
        // EditableContentSnapshot::forEachEffectiveF0Span(): the pitch OpenTune renders, the correction
        // where one exists and the original (with the clip pitch shift) elsewhere.
        std::vector<float> effective(static_cast<std::size_t>(spanEnd - startFrame), 0.0f);
        snapshot.forEachEffectiveF0Span(static_cast<int>(startFrame), static_cast<int>(spanEnd),
                                        [&](int spanStart, const float* values, int length, float gain) {
                                            for (int i = 0; i < length; ++i) {
                                                const auto at = static_cast<std::size_t>(spanStart - startFrame + i);
                                                if (at < effective.size())
                                                    effective[at] = values[i] * gain;
                                            }
                                        });
        const auto& f0 = snapshot.pitchCurve->getOriginalF0();
        for (std::int64_t i = 0; i < count; ++i) {
            const auto frame = startFrame + i * step;
            original.append(midiOrNull(f0[static_cast<std::size_t>(frame)], tuningHz));
            // An unvoiced frame has no pitch on either curve.
            const bool voiced = f0[static_cast<std::size_t>(frame)] > 0.0f;
            corrected.append(voiced ? midiOrNull(effective[static_cast<std::size_t>(frame - startFrame)], tuningHz)
                                    : JsonValue());
        }
    }

    auto result = JsonValue::object();
    result.appendMember("asOf", state.asOf);
    auto derived = JsonValue::array();
    derived.append(JsonValue::string("originalMidi"));
    derived.append(JsonValue::string("correctedMidi"));
    if (!usable)
        derived.append(JsonValue::string("hopSec"));
    result.appendMember("derived", std::move(derived));
    result.appendMember("contentId", JsonValue::string(content.contentId));
    result.appendMember("f0State", JsonValue::string(controlF0StateName(snapshot.originalF0State)));
    result.appendMember("tuningHz", controlJsonNumber(tuningHz, 3));
    result.appendMember("hopSec", controlJsonNumber(hopSec, 9));
    result.appendMember("stepFrame", JsonValue::integer(step));
    result.appendMember("startContentSec",
                        controlJsonSeconds(static_cast<double>(std::max<std::int64_t>(0, startFrame)) * hopSec));
    result.appendMember("pointCount", JsonValue::integer(std::max<std::int64_t>(0, count)));
    result.appendMember("originalMidi", std::move(original));
    result.appendMember("correctedMidi", std::move(corrected));
    const auto next = offset + std::max<std::int64_t>(0, count);
    result.appendMember("nextCursor",
                        next < total
                            ? JsonValue::string(controlFormatCursor({kCurveCursor, static_cast<std::uint64_t>(next),
                                                                     params.selectionHash, state.tokensHash}))
                            : JsonValue());
    return result;
}

// ---- content.segmentNotes -------------------------------------------------------------------------

struct SegmentParams
{
    std::optional<ControlHandle> contentId;
    std::optional<ControlTimeRange> range;
    double noteSplitCents = PitchControlConfig::kDefaultNoteSplitCents;
};

bool parseSegmentParams(const JsonValue& params, SegmentParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<double> split;
    if (!reader.onlyMembers({"contentId", "range", "noteSplitCents"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, out.contentId))
        return failWith(reader, why);
    if (!out.contentId) {
        reader.fail("contentId is required");
        return failWith(reader, why);
    }
    if (!controlParseRange(reader, "range", false, out.range)
        || !reader.optionalNumber("noteSplitCents", PitchControlConfig::kMinNoteSplitCents,
                                  PitchControlConfig::kMaxNoteSplitCents, split))
        return failWith(reader, why);
    // Absent: the generator's default (NoteSegmentationPolicy, the UI knob's default).
    out.noteSplitCents = split.value_or(NoteSegmentationPolicy{}.transitionThresholdCents);
    return true;
}

Result<JsonValue> runSegmentNotes(ControlOpContext& context, const SegmentParams& params)
{
    auto resolved = controlResolveContent(context, *params.contentId);
    if (!resolved.ok())
        return resolved.error();
    const auto content = std::move(resolved).value();
    const auto& snapshot = *content.snapshot;
    const auto state = readState(context, content.contentId, snapshot);
    const float tuningHz = TuningConfig::currentTuningHz();

    auto segments = JsonValue::array();
    if (snapshot.hasUsableOriginalF0()) {
        const auto& curve = *snapshot.pitchCurve;
        const auto& f0 = curve.getOriginalF0();
        NoteGeneratorParams generator;
        generator.policy.transitionThresholdCents = static_cast<float>(params.noteSplitCents);
        // LegacyNoteGenerator::generate() over the whole original F0, with no energy weights: the call
        // OpenTune makes for AUTO and for note generation (generateNotesFromOriginalF0).
        const auto notes = LegacyNoteGenerator::generate(f0.data(), static_cast<int>(f0.size()), nullptr, 0,
                                                         static_cast<int>(f0.size()), curve.getHopSize(),
                                                         curve.getSampleRate(), generator);
        for (const auto& note : notes) {
            if (params.range && !(note.endTime > params.range->start && note.startTime < params.range->end))
                continue;
            const float detected = note.originalPitch > 0.0f ? note.originalPitch : note.pitch;
            if (!(detected > 0.0f))
                continue;
            auto segment = JsonValue::object();
            segment.appendMember("startContentSec", controlJsonSeconds(std::max(0.0, note.startTime)));
            segment.appendMember("endContentSec", controlJsonSeconds(std::max(0.0, note.endTime)));
            segment.appendMember("detectedMidi", controlJsonPitch(midiOf(detected, tuningHz)));
            segments.append(std::move(segment));
        }
    }

    auto result = JsonValue::object();
    result.appendMember("asOf", state.asOf);
    auto derived = JsonValue::array();
    derived.append(JsonValue::string("segments"));
    result.appendMember("derived", std::move(derived));
    result.appendMember("contentId", JsonValue::string(content.contentId));
    result.appendMember("f0State", JsonValue::string(controlF0StateName(snapshot.originalF0State)));
    result.appendMember("tuningHz", controlJsonNumber(tuningHz, 3));
    result.appendMember("noteSplitCents", controlJsonNumber(params.noteSplitCents, 4));
    result.appendMember("segments", std::move(segments));
    return result;
}

} // namespace

void registerContentReadOps(ControlMethodTable& table)
{
    table.add(controlReadMethod<NotesParams>("content.getNotes", parseNotesParams, runGetNotes));
    table.add(controlReadMethod<CurveParams>("content.getPitchCurve", parseCurveParams, runGetPitchCurve));
    table.add(controlReadMethod<SegmentParams>("content.segmentNotes", parseSegmentParams, runSegmentNotes));
}

} // namespace OpenTune
