// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContentTuneOps.h"

#include "EditSupport.h"
#include "OpsJson.h"
#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Content/ContentEditCommands.h"
#include "Content/EditableContentSnapshot.h"
#include "Control/ControlUiBridge.h"
#include "Control/Edit/ControlOriginAction.h"
#include "PluginProcessor.h"
#include "Utils/F0Timeline.h"
#include "Utils/LocalizationManager.h"
#include "Utils/NoteGeneratorTypes.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PitchCurve.h"
#include "Utils/ScaleUiMapping.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace OpenTune {

namespace {

/** Changes, created and removed list at most this many entries each. */
constexpr std::size_t kMaxListedEntries = 200;

Result<std::shared_ptr<ControlHistory>> historyOf(ControlOpContext& context)
{
    auto history = context.history();
    if (history == nullptr)
        return controlOpError("INTERNAL", "", "the undo history of the control API is not available");
    return history;
}

JsonValue contentRef(const std::string& contentId)
{
    auto ref = JsonValue::object();
    ref.appendMember("contentId", JsonValue::string(contentId));
    return ref;
}

JsonValue keyInputJson(const DetectedKey& key)
{
    auto json = JsonValue::object();
    json.appendMember("root", JsonValue::integer(static_cast<int>(key.root)));
    json.appendMember("scale", JsonValue::string(controlScaleName(key.scale)));
    return json;
}

JsonValue field(JsonValue before, JsonValue after)
{
    auto json = JsonValue::object();
    json.appendMember("before", std::move(before));
    json.appendMember("after", std::move(after));
    json.appendMember("unit", JsonValue());
    return json;
}

/** Whether the UI shows this content's key: the selected clip of the active track shows it, and the transport
    bar follows it on the editor's next heartbeat (PluginEditor's applyResolvedScaleForPlacementContent). */
bool keyShownInUi(ControlOpContext& context, const ContentKey& key)
{
    auto* bridge = context.bridge();
    const auto selected = bridge != nullptr ? bridge->selectedPlacementIds() : std::nullopt;
    if (!selected)
        return false;
    for (const auto& placement : controlLivePlacements(context))
        if (placement.placement.contentKey == key
            && std::find(selected->begin(), selected->end(), placement.placement.placementId) != selected->end())
            return true;
    return false;
}

bool failWith(ControlParamReader& reader, std::string& why)
{
    why = reader.why();
    return false;
}

bool reject(ControlParamReader& reader, std::string& why, std::string message)
{
    reader.fail(std::move(message));
    return failWith(reader, why);
}

/** The undo description of AUTO regenerate, the text the piano roll gives its AUTO entry
    (PianoRollComponent::applyAutoTuneToSelection()): LOC_RAW("AUTO correction"), the text in the language of the
    window at the time of the edit. */
juce::String autoTuneDescription()
{
    return LOC_RAW("AUTO correction");
}

/** The undo description of SNAP, the text the piano roll gives the AUTO button's snap
    (PianoRollComponent::applyAutoSnapToAllNotes()) and PianoRollToolHandler its double-click snap:
    LOC_RAW("Pitch snap"). */
juce::String snapDescription()
{
    return LOC_RAW("Pitch snap");
}

// ---- content.setKey -------------------------------------------------------------------------------

struct SetKeyParams
{
    ControlHandle contentId;
    DetectedKey key;
    std::optional<std::string> expectedKeyRevision;
};

/** Parses content.setKey params. */
bool parseSetKey(const JsonValue& params, SetKeyParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<ControlHandle> contentId;
    if (!reader.onlyMembers({"contentId", "key", "expectedKeyRevision"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, contentId)
        || !controlReadRevisionToken(reader, "expectedKeyRevision", out.expectedKeyRevision))
        return failWith(reader, why);
    if (!contentId)
        return reject(reader, why, "contentId is required");
    const JsonValue* key = reader.member("key");
    if (key == nullptr || !controlParseKeyInput(*key, out.key))
        return reject(reader, why, "key must be {root: 0..11, scale}");
    out.contentId = *contentId;
    return true;
}

Result<JsonValue> runSetKey(ControlOpContext& context, const SetKeyParams& params)
{
    auto content = controlResolveContent(context, params.contentId);
    if (!content.ok())
        return content.error();
    const auto& ref = content.value();
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (auto notReady = controlCheckContentReady(context, ref))
        return *notReady;
    if (auto conflict = controlCheckExpectedTokens(controlContentVersions(context, ref.contentId, *ref.snapshot),
                                                   std::nullopt, params.expectedKeyRevision))
        return *conflict;
    auto history = historyOf(context);
    if (!history.ok())
        return history.error();

    ControlWriteEnvelope envelope(context);
    envelope.touchContent(ref.contentId);
    auto commands = context.processor().getContentCommands();
    const auto keyEdit = commands->setDetectedKey(ref.key, params.key);
    if (!keyEdit)
        return controlOpError("INTERNAL", "", "OpenTune did not accept the key");
    const DetectedKey previous = keyEdit->before;
    if (!keyEdit->changed)
        return envelope.committed(context);

    // setDetectedKey() already clears redo and notifies the product owner.
    history.value()->onChangeWithoutEntry(context.processor().getUndoManager());

    auto fields = JsonValue::object();
    if (previous.root != params.key.root || previous.origin == Origin::Unset)
        fields.appendMember(
            "root",
            field(previous.origin == Origin::Unset ? JsonValue() : JsonValue::integer(static_cast<int>(previous.root)),
                  JsonValue::integer(static_cast<int>(params.key.root))));
    if (previous.scale != params.key.scale || previous.origin == Origin::Unset)
        fields.appendMember(
            "scale",
            field(previous.origin == Origin::Unset ? JsonValue() : JsonValue::string(controlScaleName(previous.scale)),
                  JsonValue::string(controlScaleName(params.key.scale))));
    if (previous.origin != Origin::Manual)
        fields.appendMember("origin",
                            field(previous.origin == Origin::Automatic ? JsonValue::string("automatic") : JsonValue(),
                                  JsonValue::string("manual")));
    envelope.addChange("key", contentRef(ref.contentId), JsonValue(), std::move(fields));

    // The previous root and scale, with the key revision right after the change. A
    // content without a key before cannot get "no key" back; nor can an instance without edit.v1.
    JsonValue revert;
    const auto now = controlLiveSnapshot(context, ref.key);
    if (previous.origin != Origin::Unset && now != nullptr && controlReportsCapability(context, "edit.v1")) {
        auto revertParams = JsonValue::object();
        revertParams.appendMember("contentId", JsonValue::string(ref.contentId));
        revertParams.appendMember("key", keyInputJson(previous));
        revertParams.appendMember("expectedKeyRevision",
                                  JsonValue::string(controlContentVersions(context, ref.contentId, *now).key));
        revert = controlRevertRecipe("content.setKey", std::move(revertParams));
    }
    if (keyShownInUi(context, ref.key)) {
        auto effect = JsonValue::object();
        effect.appendMember("revert", revert);
        envelope.addSideEffect("uiScaleChanged", std::move(effect));
    }
    envelope.setNotUndoable("ui_parity", std::move(revert));
    return envelope.committed(context);
}

// ---- content.autoTune -----------------------------------------------------------------------------

enum class TuneMode {
    Regenerate,
    SnapNotes,
    NotesOnly
};

const char* modeName(TuneMode mode)
{
    switch (mode) {
        case TuneMode::Regenerate:
            return "regenerate";
        case TuneMode::SnapNotes:
            return "snapNotes";
        case TuneMode::NotesOnly:
            return "notesOnly";
    }
    return "regenerate";
}

struct AutoTuneParams
{
    ControlHandle contentId;
    TuneMode mode = TuneMode::Regenerate;
    std::string expectedRevision;
    /** Content seconds; empty for the whole content. */
    std::vector<std::pair<double, double>> ranges;
    std::optional<DetectedKey> key;
    bool clipKey = false;
    std::optional<std::string> expectedKeyRevision;
    std::optional<double> retuneSpeed;
    std::optional<double> vibratoDepth;
    std::optional<double> vibratoRateHz;
    std::optional<double> noteSplitCents;
    bool dryRun = false;
};

bool readRanges(ControlParamReader& reader, std::vector<std::pair<double, double>>& out)
{
    const JsonValue* ranges = reader.member("ranges");
    if (ranges == nullptr)
        return true;
    if (!ranges->isArray() || ranges->size() < 1 || ranges->size() > 16)
        return reader.fail("ranges must be an array of 1 to 16 ranges");
    for (std::size_t i = 0; i < ranges->size(); ++i) {
        const auto& range = ranges->at(i);
        const JsonValue* start = range.isObject() ? range.find("startContentSec") : nullptr;
        const JsonValue* end = range.isObject() ? range.find("endContentSec") : nullptr;
        if (start == nullptr || end == nullptr || range.size() != 2 || !start->isNumber() || !end->isNumber()
            || !std::isfinite(start->asDouble()) || !std::isfinite(end->asDouble()) || start->asDouble() < 0.0
            || end->asDouble() < 0.0 || end->asDouble() < start->asDouble())
            return reader.fail(
                "every range must be {startContentSec, endContentSec} of non-negative seconds with end >= start");
        out.emplace_back(start->asDouble(), end->asDouble());
    }
    return true;
}

bool readGeneratorParams(ControlParamReader& reader, AutoTuneParams& out)
{
    const JsonValue* params = reader.member("params");
    if (params == nullptr)
        return true;
    if (!params->isObject())
        return reader.fail("params must be an object");
    ControlParamReader inner(*params);
    if (!inner.onlyMembers({"retuneSpeed", "vibratoDepth", "vibratoRateHz", "noteSplitCents"})
        || !inner.optionalNumber("retuneSpeed", 0.0, 1.0, out.retuneSpeed)
        || !inner.optionalNumber("vibratoDepth", 0.0, 100.0, out.vibratoDepth)
        || !inner.optionalNumber("vibratoRateHz", 3.0, 12.0, out.vibratoRateHz)
        || !inner.optionalNumber("noteSplitCents", 0.0, 200.0, out.noteSplitCents))
        return reader.fail("params." + inner.why());
    return true;
}

/** Parses content.autoTune params, including the conditions per mode. */
bool parseAutoTune(const JsonValue& params, AutoTuneParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<ControlHandle> contentId;
    std::optional<std::string> expectedRevision;
    if (!reader.onlyMembers(
            {"contentId", "mode", "expectedRevision", "ranges", "key", "expectedKeyRevision", "params", "dryRun"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, contentId)
        || !controlReadRevisionToken(reader, "expectedRevision", expectedRevision)
        || !controlReadRevisionToken(reader, "expectedKeyRevision", out.expectedKeyRevision)
        || !readRanges(reader, out.ranges) || !readGeneratorParams(reader, out))
        return failWith(reader, why);
    if (!contentId || !expectedRevision)
        return reject(reader, why, "contentId, mode and expectedRevision are required");
    const JsonValue* mode = reader.member("mode");
    bool known = false;
    for (const auto candidate : {TuneMode::Regenerate, TuneMode::SnapNotes, TuneMode::NotesOnly})
        if (mode != nullptr && mode->isString() && mode->text() == modeName(candidate)) {
            out.mode = candidate;
            known = true;
        }
    if (!known)
        return reject(reader, why, "mode must be regenerate, snapNotes or notesOnly");
    if (const JsonValue* key = reader.member("key")) {
        DetectedKey parsed;
        if (key->isString() && key->text() == "clipKey")
            out.clipKey = true;
        else if (controlParseKeyInput(*key, parsed))
            out.key = parsed;
        else
            return reject(reader, why, "key must be {root: 0..11, scale} or \"clipKey\"");
    }
    if (const JsonValue* dryRun = reader.member("dryRun")) {
        if (!dryRun->isBoolean())
            return reject(reader, why, "dryRun must be a boolean");
        out.dryRun = dryRun->asBoolean();
    }
    const bool hasKey = reader.member("key") != nullptr;
    const bool hasRanges = reader.member("ranges") != nullptr;
    const bool hasParams = reader.member("params") != nullptr;
    switch (out.mode) {
        case TuneMode::Regenerate:
            if (!hasKey)
                return reject(reader, why, "mode regenerate needs key");
            break;
        case TuneMode::SnapNotes:
            if (!hasKey || hasParams)
                return reject(reader, why, "mode snapNotes needs key and takes no params");
            break;
        case TuneMode::NotesOnly:
            if (hasRanges || hasKey)
                return reject(reader, why, "mode notesOnly takes no ranges or key");
            break;
    }
    if (out.clipKey && !out.expectedKeyRevision)
        return reject(reader, why, "key clipKey needs expectedKeyRevision");
    out.contentId = *contentId;
    out.expectedRevision = *expectedRevision;
    return true;
}

/** NoteGeneratorParams{} with the given params: never the positions of the UI's knobs.
    noteSplitCents is the segmentation threshold that the UI's Note Split knob sets. */
NoteGeneratorParams generatorParams(const AutoTuneParams& params)
{
    NoteGeneratorParams generator;
    if (params.retuneSpeed)
        generator.retuneSpeed = static_cast<float>(*params.retuneSpeed);
    if (params.vibratoDepth)
        generator.vibratoDepth = static_cast<float>(*params.vibratoDepth);
    if (params.vibratoRateHz)
        generator.vibratoRate = static_cast<float>(*params.vibratoRateHz);
    if (params.noteSplitCents)
        generator.policy.transitionThresholdCents = static_cast<float>(*params.noteSplitCents);
    return generator;
}

JsonValue paramsUsedJson(const NoteGeneratorParams& generator)
{
    auto json = JsonValue::object();
    json.appendMember("retuneSpeed", controlJsonNumber(generator.retuneSpeed, 4));
    json.appendMember("vibratoDepth", controlJsonNumber(generator.vibratoDepth, 4));
    json.appendMember("vibratoRateHz", controlJsonNumber(generator.vibratoRate, 4));
    json.appendMember("noteSplitCents", controlJsonNumber(generator.policy.transitionThresholdCents, 4));
    return json;
}

bool sameNote(const Note& a, const Note& b)
{
    return a.startTime == b.startTime && a.endTime == b.endTime && a.pitch == b.pitch
           && a.originalPitch == b.originalPitch && a.pitchOffset == b.pitchOffset && a.retuneSpeed == b.retuneSpeed
           && a.vibratoDepth == b.vibratoDepth && a.vibratoRate == b.vibratoRate
           && a.pitchDriftScale == b.pitchDriftScale && a.isVoiced == b.isVoiced;
}

/**
 * The notes before the call that regenerate replaced (removed) and the notes after it that are new (created):
 * the shared regenerate command removes every note that overlaps a committed range and copies the
 * others unchanged, so a note after the call is new unless it equals one of the notes kept. A note inside a
 * range is replaced even when the generator happens to make an equal one.
 */
void diffNotes(const std::vector<Note>& before, const std::vector<Note>& after,
               const PitchCurveSnapshot& curve, const std::vector<F0FrameRange>& committed,
               std::vector<std::size_t>& removed, std::vector<std::size_t>& created)
{
    std::vector<const Note*> kept;
    for (std::size_t i = 0; i < before.size(); ++i) {
        const auto& note = before[i];
        const auto noteRange = f0FrameRangeForSeconds(curve.getHopSize(), curve.getSampleRate(),
                                                      static_cast<int>(curve.size()), note.startTime, note.endTime);
        const bool overlaps = noteRange && std::any_of(committed.begin(), committed.end(), [&noteRange](const auto& range) {
            return halfOpenRangesOverlap(noteRange->startFrame, noteRange->endFrameExclusive,
                                         range.startFrame, range.endFrameExclusive);
        });
        if (overlaps)
            removed.push_back(i);
        else
            kept.push_back(&note);
    }
    for (std::size_t j = 0; j < after.size(); ++j) {
        const auto match = std::find_if(kept.begin(), kept.end(),
                                        [&](const Note* note) { return note != nullptr && sameNote(*note, after[j]); });
        if (match != kept.end())
            *match = nullptr;
        else
            created.push_back(j);
    }
}

/** The position of every note in start-time order, as content.getNotes counts them (controlNoteIndexOrder):
    result[storageIndex] is the note index a client uses. */
std::vector<std::size_t> noteIndexes(const std::vector<Note>& notes)
{
    const auto order = controlNoteIndexOrder(notes);
    std::vector<std::size_t> index(notes.size());
    for (std::size_t position = 0; position < order.size(); ++position)
        index[order[position]] = position;
    return index;
}

/** Lists removed notes (at the revision before) and created notes (at the revision after; for a prediction at
    the current one), at most kMaxListedEntries each; returns {removed, created} omitted counts. removed and
    created hold positions in before and after as stored; the entries carry the content.getNotes index of each
    list, in that order. */
std::pair<std::size_t, std::size_t> listNotes(ControlWriteEnvelope& envelope, const std::string& contentId,
                                              const std::vector<Note>& before, const std::string& revisionBefore,
                                              const std::vector<Note>& after, const std::string& revisionAfter,
                                              const std::vector<std::size_t>& removedStored,
                                              const std::vector<std::size_t>& createdStored)
{
    const auto beforeIndex = noteIndexes(before);
    const auto afterIndex = noteIndexes(after);
    auto removed = removedStored;
    auto created = createdStored;
    std::sort(removed.begin(), removed.end(),
              [&beforeIndex](std::size_t a, std::size_t b) { return beforeIndex[a] < beforeIndex[b]; });
    std::sort(created.begin(), created.end(),
              [&afterIndex](std::size_t a, std::size_t b) { return afterIndex[a] < afterIndex[b]; });
    for (std::size_t i = 0; i < removed.size() && i < kMaxListedEntries; ++i)
        envelope.addRemoved("note", controlNoteRefJson(contentId, beforeIndex[removed[i]], revisionBefore),
                            controlNoteLabelJson(beforeIndex[removed[i]], before[removed[i]]));
    for (std::size_t i = 0; i < created.size() && i < kMaxListedEntries; ++i)
        envelope.addCreated("note", controlNoteRefJson(contentId, afterIndex[created[i]], revisionAfter),
                            controlNoteLabelJson(afterIndex[created[i]], after[created[i]]));
    return {removed.size() > kMaxListedEntries ? removed.size() - kMaxListedEntries : 0,
            created.size() > kMaxListedEntries ? created.size() - kMaxListedEntries : 0};
}

/** The members of the result besides the envelope. omitted is
    {removed, created}; generator is null for snapNotes, which takes no generator params. */
void appendAutoTuneMembers(JsonValue& result, TuneMode mode, const std::optional<DetectedKey>& keyUsed,
                           const std::optional<NoteGeneratorParams>& generator, std::size_t before, std::size_t after,
                           std::pair<std::size_t, std::size_t> omitted, std::size_t omittedChanges = 0)
{
    auto omittedCount = JsonValue::object();
    omittedCount.appendMember("changes", JsonValue::integer(static_cast<std::int64_t>(omittedChanges)));
    omittedCount.appendMember("created", JsonValue::integer(static_cast<std::int64_t>(omitted.second)));
    omittedCount.appendMember("removed", JsonValue::integer(static_cast<std::int64_t>(omitted.first)));
    result.appendMember("omittedCount", std::move(omittedCount));
    result.appendMember("mode", JsonValue::string(modeName(mode)));
    result.appendMember("keyUsed", keyUsed ? keyInputJson(*keyUsed) : JsonValue());
    result.appendMember("paramsUsed", generator ? paramsUsedJson(*generator) : JsonValue());
    auto noteCount = JsonValue::object();
    noteCount.appendMember("before", JsonValue::integer(static_cast<std::int64_t>(before)));
    noteCount.appendMember("after", JsonValue::integer(static_cast<std::int64_t>(after)));
    result.appendMember("noteCount", std::move(noteCount));
}

/** Frame ranges of the requested content ranges on the content's analysis timeline; the whole content without
    ranges. Empty and fully out-of-timeline ranges are left out. */
std::vector<F0FrameRange> frameRanges(const PitchCurveSnapshot& curve, const AutoTuneParams& params)
{
    std::vector<F0FrameRange> out;
    if (params.ranges.empty()) {
        if (curve.size() > 0)
            out.push_back({0, static_cast<int>(curve.size())});
    } else {
        for (const auto& [start, end] : params.ranges) {
            if (const auto range = f0FrameRangeForSeconds(curve.getHopSize(), curve.getSampleRate(),
                                                          static_cast<int>(curve.size()), start, end))
                out.push_back(*range);
        }
    }

    std::sort(out.begin(), out.end(), [](const F0FrameRange& lhs, const F0FrameRange& rhs) {
        return lhs.startFrame < rhs.startFrame;
    });
    std::vector<F0FrameRange> merged;
    for (const auto& range : out) {
        if (merged.empty() || range.startFrame > merged.back().endFrameExclusive) {
            merged.push_back(range);
            continue;
        }
        merged.back().endFrameExclusive = std::max(merged.back().endFrameExclusive,
                                                    range.endFrameExclusive);
    }
    return merged;
}

/** Merge requested ranges only when their pitch-correction boundary contexts meet. */
std::vector<F0FrameRange> correctionPlanningRanges(const PitchCurveSnapshot& curve,
                                                    const std::vector<F0FrameRange>& requestedRanges)
{
    std::vector<F0FrameRange> out;
    const int frameCount = static_cast<int>(curve.size());
    for (const auto& range : requestedRanges) {
        const auto expanded = PitchCurve::expandNoteBasedCorrectionRange(
            range.startFrame, range.endFrameExclusive, frameCount);
        if (expanded.isEmpty())
            continue;

        if (!out.empty()) {
            const auto previous = PitchCurve::expandNoteBasedCorrectionRange(
                out.back().startFrame, out.back().endFrameExclusive, frameCount);
            if (expanded.startFrame <= previous.endFrameExclusive) {
                out.back().endFrameExclusive = std::max(out.back().endFrameExclusive,
                                                        range.endFrameExclusive);
                continue;
            }
        }
        out.push_back(range);
    }
    return out;
}

Result<JsonValue> runRegenerate(ControlOpContext& context, const AutoTuneParams& params, const ControlContentRef& ref,
                                const ControlContentVersions& versions, const DetectedKey& key)
{
    auto history = historyOf(context);
    if (!history.ok())
        return history.error();
    auto& processor = context.processor();
    auto commands = processor.getContentCommands();
    const auto generator = generatorParams(params);
    // makeScaleSnapConfig(): the key's scale for the snap, none for chromatic, as the UI maps its scale.
    const auto scaleSnap = makeScaleSnapConfig(key);
    const auto& curve = *ref.snapshot->pitchCurve;
    const auto ranges = frameRanges(curve, params);
    const auto beforeNotes = ref.snapshot->notes;
    const auto beforeWorld = controlCaptureWorldVersions(context);

    ControlWriteEnvelope envelope(context);
    envelope.touchContent(ref.contentId);
    std::vector<F0FrameRange> committed;
    std::vector<Note> afterNotes = beforeNotes;
    const juce::String description = autoTuneDescription();
    EditableContentSnapshot planning = *ref.snapshot;
    for (const auto& range : ranges) {
        planning.notes = afterNotes;
        auto predicted = commands->planAutoTuneContentRange(
            planning, range.startFrame, range.endFrameExclusive, generator, scaleSnap);
        if (!predicted)
            continue;
        afterNotes = std::move(*predicted);
        committed.push_back(range);
    }

    const auto correctionPlanRanges = correctionPlanningRanges(curve, committed);
    std::vector<ContentEditRangeFrames> affectedRanges;
    for (const auto& range : correctionPlanRanges) {
        planning.notes = afterNotes;
        const auto correction = commands->planNoteCorrection(planning, afterNotes, range, generator);
        if (!correction)
            return controlOpError("INTERNAL", "", "OpenTune did not plan the generated notes");

        afterNotes = correction->notes;
        planning.notes = afterNotes;
        planning.pitchCurve = correction->correctedCurve;
        affectedRanges.push_back({correction->affectedRange.startFrame,
                                  correction->affectedRange.endFrameExclusive});
    }

    std::vector<PitchCorrectionSegment> segments;
    for (const auto& segment : planning.pitchCurve->getCorrectionSegments())
        if (std::any_of(affectedRanges.begin(), affectedRanges.end(), [&segment](const auto& range) {
                return segment.startFrame < range.endFrameExclusive
                    && segment.endFrame > range.startFrame;
            }))
            segments.push_back(segment);

    std::string revisionAfter = versions.revision();
    if (!affectedRanges.empty() && !params.dryRun) {
        auto edit = commands->commitNotesAndSegmentsUndoableInRanges(
            ref.key, afterNotes, segments, affectedRanges, description);
        if (edit.snapshot == nullptr || edit.undoAction == nullptr)
            return controlOpError("INTERNAL", "", "OpenTune did not commit the generated notes");
        afterNotes = edit.snapshot->notes;
        auto& manager = processor.getUndoManager();
        const auto undoId = history.value()->assignUndoId(
            manager, *edit.undoAction, manager.cursor() - 1, controlContentEpoch(context),
            context.instanceShort());
        const std::vector<std::string> names{controlUndoActionName(*edit.undoAction)};
        envelope.setUndoable(undoId, description.toStdString(), names);
        if (const auto now = controlLiveSnapshot(context, ref.key))
            revisionAfter = controlContentVersions(context, ref.contentId, *now).revision();
    }

    // Read a committed result back from the content owner for the response.
    std::optional<Error> unreadable;
    if (!params.dryRun && !committed.empty()) {
        auto read = controlReadCommittedNotes(context, ref, std::nullopt);
        if (read.ok())
            afterNotes = std::move(read).value().notes;
        else
            unreadable = read.error();
    }

    std::vector<std::size_t> removed;
    std::vector<std::size_t> created;
    diffNotes(beforeNotes, afterNotes, curve, committed, removed, created);
    // Notes that overlap a range but do not lie inside one are dropped, not regenerated: the commit removes
    // every note that overlaps the range and generates only inside it (an OpenTune behaviour).
    auto dropped = JsonValue::array();
    const auto beforeIndex = noteIndexes(beforeNotes);
    std::sort(removed.begin(), removed.end(),
              [&beforeIndex](std::size_t a, std::size_t b) { return beforeIndex[a] < beforeIndex[b]; });
    for (const auto stored : removed) {
        const auto index = beforeIndex[stored];
        const auto& note = beforeNotes[stored];
        const auto noteRange = f0FrameRangeForSeconds(curve.getHopSize(), curve.getSampleRate(),
                                                      static_cast<int>(curve.size()), note.startTime, note.endTime);
        const bool inside = noteRange && std::any_of(committed.begin(), committed.end(), [&noteRange](const auto& range) {
            return noteRange->startFrame >= range.startFrame
                   && noteRange->endFrameExclusive <= range.endFrameExclusive;
        });
        if (inside || dropped.size() >= kMaxListedEntries)
            continue;
        auto entry = JsonValue::object();
        auto noteRef = JsonValue::object();
        noteRef.appendMember("index", JsonValue::integer(static_cast<std::int64_t>(index)));
        noteRef.appendMember("atRevision", JsonValue::string(versions.revision()));
        entry.appendMember("note", std::move(noteRef));
        entry.appendMember("startContentSec", controlJsonSeconds(note.startTime));
        entry.appendMember("endContentSec", controlJsonSeconds(note.endTime));
        dropped.append(std::move(entry));
    }

    if (unreadable)
        return *unreadable;
    const auto omitted = listNotes(envelope, ref.contentId, beforeNotes, versions.revision(), afterNotes, revisionAfter,
                                   removed, created);
    if (dropped.size() != 0) {
        auto data = JsonValue::object();
        data.appendMember("notes", std::move(dropped));
        envelope.addWarning("DROPPED_BOUNDARY_NOTES", contentRef(ref.contentId), std::move(data));
    }
    JsonValue result;
    if (params.dryRun) {
        if (!committed.empty())
            controlAddPredictedReRenderQueued(envelope, ref.contentId);
        result = envelope.predicted(context);
    } else {
        controlAddReRenderQueued(envelope, beforeWorld, controlCaptureWorldVersions(context));
        result = envelope.committed(context);
    }
    appendAutoTuneMembers(result, TuneMode::Regenerate, key, generator, beforeNotes.size(), afterNotes.size(), omitted);
    return result;
}

Result<JsonValue> runNotesOnly(ControlOpContext& context, const AutoTuneParams& params, const ControlContentRef& ref,
                               const ControlContentVersions& versions)
{
    auto commands = context.processor().getContentCommands();
    const auto generator = generatorParams(params);
    const auto beforeNotes = ref.snapshot->notes;
    const auto beforeWorld = controlCaptureWorldVersions(context);
    std::vector<Note> afterNotes;
    if (params.dryRun) {
        auto patch = commands->planGenerateNotesOnly(*ref.snapshot, generator);
        if (!patch)
            return controlOpError("INTERNAL", "", "OpenTune's note generator returned no valid result");
        afterNotes = mergeNotesRange(beforeNotes, patch->affectedRange, patch->afterNotesInRange);
    }
    ControlWriteEnvelope envelope(context);
    envelope.touchContent(ref.contentId);
    std::string revisionAfter = versions.revision();
    if (!params.dryRun) {
        // Replaces all notes through the same command as OpenTune's note-generation entry. A generator that returns
        // no valid result changes nothing.
        if (!commands->generateNotesOnly(ref.key, generator))
            return controlOpError("INTERNAL", "", "OpenTune's note generator returned no valid result");
        auto committed = controlReadCommittedNotes(context, ref, std::nullopt);
        if (!committed.ok())
            return committed.error();
        auto read = std::move(committed).value();
        afterNotes = std::move(read.notes);
        revisionAfter = std::move(read.revision);
    }
    // The note-generation command replaces the notes of a range that covers the whole content: every note
    // before is removed, every note after is new.
    std::vector<std::size_t> removed(beforeNotes.size());
    std::vector<std::size_t> created(afterNotes.size());
    std::iota(removed.begin(), removed.end(), std::size_t{0});
    std::iota(created.begin(), created.end(), std::size_t{0});
    const auto omitted = listNotes(envelope, ref.contentId, beforeNotes, versions.revision(), afterNotes, revisionAfter,
                                   removed, created);
    auto counts = JsonValue::object();
    counts.appendMember("removedCount", JsonValue::integer(static_cast<std::int64_t>(removed.size())));
    counts.appendMember("createdCount", JsonValue::integer(static_cast<std::int64_t>(created.size())));
    envelope.addWarning("REPLACES_ALL_NOTES", contentRef(ref.contentId), std::move(counts));
    JsonValue result;
    if (params.dryRun) {
        // The topology commit always advances the notes revision, so the
        // commit reports the content in reRenderQueued: the prediction lists the side effects the commit would have.
        controlAddPredictedReRenderQueued(envelope, ref.contentId);
        result = envelope.predicted(context);
    } else {
        // No request can bring the previous notes back.
        envelope.addWarning("IRREVERSIBLE", contentRef(ref.contentId));
        envelope.setNotUndoable("ui_parity");
        controlAddReRenderQueued(envelope, beforeWorld, controlCaptureWorldVersions(context));
        result = envelope.committed(context);
    }
    appendAutoTuneMembers(result, TuneMode::NotesOnly, std::nullopt, generator, beforeNotes.size(), afterNotes.size(),
                          omitted);
    return result;
}

/**
 * Mode snapNotes: SNAP, the AUTO button in the openDyne scheme (PianoRollComponent::applyAutoSnapToAllNotes()),
 * through the shared content planner, with the explicit key. Without ranges it snaps every note, as the button does
 * without a note selection; ranges select the union of notes that overlap them. The complete edit is committed once
 * through the same owner as the button.
 */
Result<JsonValue> runSnapNotes(ControlOpContext& context, const AutoTuneParams& params, const ControlContentRef& ref,
                               const ControlContentVersions& versions, const DetectedKey& key)
{
    auto history = historyOf(context);
    if (!history.ok())
        return history.error();
    auto& processor = context.processor();
    auto commands = processor.getContentCommands();
    // makeScaleSnapConfig(); chromatic (no scale snap) quantizes to semitones, as the button's ScaleSnapConfig{}.
    const auto scaleSnap = makeScaleSnapConfig(key).value_or(ScaleSnapConfig{});
    const auto& curve = *ref.snapshot->pitchCurve;
    const auto beforeNotes = ref.snapshot->notes;
    std::optional<std::vector<std::size_t>> targets;
    if (!params.ranges.empty()) {
        targets.emplace();
        std::vector<std::pair<double, double>> activeRanges;
        for (const auto& [start, end] : params.ranges)
            if (f0FrameRangeForSeconds(curve.getHopSize(), curve.getSampleRate(),
                                       static_cast<int>(curve.size()), start, end))
                activeRanges.emplace_back(start, end);
        for (std::size_t index = 0; index < beforeNotes.size(); ++index) {
            const auto& note = beforeNotes[index];
            if (std::any_of(activeRanges.begin(), activeRanges.end(), [&](const auto& range) {
                    return halfOpenRangesOverlap(note.startTime, note.endTime, range.first, range.second);
                }))
                targets->push_back(index);
        }
    }
    const auto beforeWorld = controlCaptureWorldVersions(context);
    ControlWriteEnvelope envelope(context);
    envelope.touchContent(ref.contentId);
    const juce::String description = snapDescription();
    const auto plan = commands->planAutoSnap(
        *ref.snapshot, targets, scaleSnap, generatorParams(params));
    const bool changed = plan.status == ContentAutoSnapPlan::Status::Commit;
    std::vector<Note> afterNotes = plan.notes;
    std::string revisionAfter = versions.revision();
    if (!params.dryRun && changed) {
        auto edit = commands->commitNotesAndSegmentsUndoable(
            ref.key, plan.correction.notes, plan.correction.segments,
            ContentEditRangeFrames{plan.correction.affectedRange.startFrame,
                                   plan.correction.affectedRange.endFrameExclusive},
            description);
        if (edit.snapshot == nullptr || edit.undoAction == nullptr)
            return controlOpError("INTERNAL", "", "OpenTune did not commit the snap");
        afterNotes = edit.snapshot->notes;
        auto& manager = processor.getUndoManager();
        const auto undoId = history.value()->assignUndoId(
            manager, *edit.undoAction, manager.cursor() - 1, controlContentEpoch(context),
            context.instanceShort());
        const std::vector<std::string> names{controlUndoActionName(*edit.undoAction)};
        envelope.setUndoable(undoId, description.toStdString(), names);
        auto committed = controlReadCommittedNotes(context, ref, beforeNotes.size());
        if (!committed.ok())
            return committed.error();
        auto read = std::move(committed).value();
        afterNotes = std::move(read.notes);
        revisionAfter = std::move(read.revision);
    }
    const auto omittedChanges =
        controlListNoteChanges(envelope, ref.contentId, beforeNotes, afterNotes, revisionAfter, kMaxListedEntries);
    JsonValue result;
    if (params.dryRun) {
        if (changed)
            controlAddPredictedReRenderQueued(envelope, ref.contentId);
        result = envelope.predicted(context);
    } else {
        controlAddReRenderQueued(envelope, beforeWorld, controlCaptureWorldVersions(context));
        result = envelope.committed(context);
    }
    appendAutoTuneMembers(result, TuneMode::SnapNotes, key, std::nullopt, beforeNotes.size(), afterNotes.size(), {0, 0},
                          omittedChanges);
    return result;
}

Result<JsonValue> runAutoTune(ControlOpContext& context, const AutoTuneParams& params)
{
    auto content = controlResolveContent(context, params.contentId);
    if (!content.ok())
        return content.error();
    const auto& ref = content.value();
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (params.mode != TuneMode::NotesOnly || !ContentEditCommands::canGenerateNotesOnly(*ref.snapshot)) {
        if (auto notReady = controlCheckContentReady(context, ref))
            return *notReady;
    }
    const auto versions = controlContentVersions(context, ref.contentId, *ref.snapshot);
    if (auto conflict = controlCheckExpectedTokens(versions, params.expectedRevision, params.expectedKeyRevision))
        return *conflict;
    if (params.mode == TuneMode::NotesOnly)
        return runNotesOnly(context, params, ref, versions);
    // clipKey: the content's own key, guarded by expectedKeyRevision above. A content without a key
    // (content.getDetails reports key null) has none to use: the default value of DetectedKey would be
    // snapped to and reported as keyUsed as if it were the content's key.
    if (params.clipKey && ref.snapshot->detectedKey.origin == Origin::Unset)
        return controlOpError("INVALID_ARGUMENT", "content_key_unset",
                              "key clipKey names the content's own key, but the content has no key; give the key "
                              "explicitly");
    DetectedKey key = params.key.value_or(ref.snapshot->detectedKey);
    if (params.mode == TuneMode::SnapNotes)
        return runSnapNotes(context, params, ref, versions, key);
    return runRegenerate(context, params, ref, versions, key);
}

} // namespace

void registerContentTuneOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<SetKeyParams>("content.setKey", parseSetKey, runSetKey));
    table.add(controlWriteMethod<AutoTuneParams>("content.autoTune", parseAutoTune, runAutoTune));
}

} // namespace OpenTune
