// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContentEditOps.h"

#include "EditSupport.h"
#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Content/ContentEditCommands.h"
#include "Content/EditableContentSnapshot.h"
#include "Control/Edit/ControlOriginAction.h"
#include "PluginProcessor.h"
#include "Utils/LocalizationManager.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PitchShiftEditAction.h"
#include "Utils/PitchUtils.h"
#include "Utils/ScaleUiMapping.h"
#include "Utils/TuningConfig.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

/** Changes lists at most this many notes. */
constexpr std::size_t kMaxListedNotes = 200;
/** A target that moves by less than this many semitones does not change (the reads round to 4 decimals). */
constexpr double kPitchTolerance = 1e-4;
/** A parameter that moves by less than this does not change (PianoRollComponent::editParameter()). */
constexpr double kParameterTolerance = 1e-6;

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

// ---- content.editNotes: params ---------------------------------------------------------------------

struct NoteSelection
{
    enum class Kind {
        Indices,
        Range,
        All
    };
    Kind kind = Kind::All;
    std::vector<std::size_t> indices;
    double startSec = 0.0;
    double endSec = 0.0;
};

enum class NoteOpKind {
    SetTargetPitch,
    ShiftCents,
    SnapToScale,
    SetParams
};

struct NoteOp
{
    NoteOpKind kind = NoteOpKind::SetTargetPitch;
    NoteSelection select;
    double targetMidi = 0.0;
    double deltaCents = 0.0;
    /** snapToScale: the explicit key, or none with clipKey. */
    std::optional<DetectedKey> key;
    bool clipKey = false;
    std::optional<double> retuneSpeed;
    std::optional<double> vibratoDepth;
    std::optional<double> vibratoRateHz;
    std::optional<double> pitchDriftScale;
};

struct EditNotesParams
{
    ControlHandle contentId;
    std::string expectedRevision;
    std::optional<std::string> expectedKeyRevision;
    std::vector<NoteOp> ops;
    bool dryRun = false;
};

/** $defs.select: exactly one of indices (1 to 1000 unique non-negative integers), range (a content range) and
    all (true). */
bool parseSelect(const JsonValue* value, NoteSelection& out, ControlParamReader& outer)
{
    if (value == nullptr || !value->isObject() || value->size() != 1)
        return outer.fail("select must be an object with exactly one of indices, range and all");
    if (const JsonValue* indices = value->find("indices")) {
        if (!indices->isArray() || indices->size() < 1 || indices->size() > 1000)
            return outer.fail("select.indices must be an array of 1 to 1000 note indexes");
        out.kind = NoteSelection::Kind::Indices;
        for (std::size_t i = 0; i < indices->size(); ++i) {
            const auto& item = indices->at(i);
            const double number = item.isNumber() ? item.asDouble() : -1.0;
            if (!item.isNumber() || !std::isfinite(number) || std::floor(number) != number || number < 0.0
                || number > 9.0e15)
                return outer.fail("select.indices must hold non-negative integers");
            const auto index = static_cast<std::size_t>(number);
            if (std::find(out.indices.begin(), out.indices.end(), index) != out.indices.end())
                return outer.fail("select.indices must not repeat an index");
            out.indices.push_back(index);
        }
        return true;
    }
    if (value->find("range") != nullptr) {
        ControlParamReader reader(*value);
        std::optional<ControlTimeRange> range;
        if (!controlParseRange(reader, "range", false, range) || !range)
            return outer.fail("select." + reader.why());
        out.kind = NoteSelection::Kind::Range;
        out.startSec = range->start;
        out.endSec = range->end;
        return true;
    }
    const JsonValue* all = value->find("all");
    if (all == nullptr || !all->isBoolean() || !all->asBoolean())
        return outer.fail("select must be an object with exactly one of indices, range and all (all: true)");
    out.kind = NoteSelection::Kind::All;
    return true;
}

/** One item of ops ($defs.op): the op's closed form. */
bool parseOp(const JsonValue& value, NoteOp& out, std::string& why)
{
    if (!value.isObject()) {
        why = "every op must be an object";
        return false;
    }
    ControlParamReader reader(value);
    const JsonValue* op = reader.member("op");
    const std::string name = op != nullptr && op->isString() ? op->text() : std::string();
    if (name == "setTargetPitch") {
        out.kind = NoteOpKind::SetTargetPitch;
        std::optional<double> targetMidi;
        if (!reader.onlyMembers({"op", "select", "targetMidi"})
            || !reader.optionalNumber("targetMidi", 0.0, 127.0, targetMidi))
            return failWith(reader, why);
        if (!targetMidi)
            return reject(reader, why, "op setTargetPitch needs targetMidi");
        out.targetMidi = *targetMidi;
    } else if (name == "shiftCents") {
        out.kind = NoteOpKind::ShiftCents;
        std::optional<double> deltaCents;
        if (!reader.onlyMembers({"op", "select", "deltaCents"})
            || !reader.optionalNumber("deltaCents", -1200.0, 1200.0, deltaCents))
            return failWith(reader, why);
        if (!deltaCents)
            return reject(reader, why, "op shiftCents needs deltaCents");
        out.deltaCents = *deltaCents;
    } else if (name == "snapToScale") {
        out.kind = NoteOpKind::SnapToScale;
        if (!reader.onlyMembers({"op", "select", "key"}))
            return failWith(reader, why);
        const JsonValue* key = reader.member("key");
        DetectedKey parsed;
        if (key != nullptr && key->isString() && key->text() == "clipKey")
            out.clipKey = true;
        else if (key != nullptr && controlParseKeyInput(*key, parsed))
            out.key = parsed;
        else
            return reject(reader, why, "op snapToScale needs key {root: 0..11, scale} or \"clipKey\"");
    } else if (name == "setParams") {
        out.kind = NoteOpKind::SetParams;
        if (!reader.onlyMembers({"op", "select", "retuneSpeed", "vibratoDepth", "vibratoRateHz", "pitchDriftScale"})
            || !reader.optionalNumber("retuneSpeed", 0.0, 1.0, out.retuneSpeed)
            || !reader.optionalNumber("vibratoDepth", 0.0, 100.0, out.vibratoDepth)
            || !reader.optionalNumber("vibratoRateHz", 3.0, 12.0, out.vibratoRateHz)
            || !reader.optionalNumber("pitchDriftScale", -2.0, 3.0, out.pitchDriftScale))
            return failWith(reader, why);
        if (!out.retuneSpeed && !out.vibratoDepth && !out.vibratoRateHz && !out.pitchDriftScale)
            return reject(reader, why, "op setParams needs at least one parameter");
    } else {
        return reject(reader, why, "op must be setTargetPitch, shiftCents, snapToScale or setParams");
    }
    if (!parseSelect(reader.member("select"), out.select, reader))
        return failWith(reader, why);
    return true;
}

/** Parses content.editNotes params. */
bool parseEditNotes(const JsonValue& params, EditNotesParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<ControlHandle> contentId;
    std::optional<std::string> expectedRevision;
    if (!reader.onlyMembers({"contentId", "expectedRevision", "expectedKeyRevision", "ops", "dryRun"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, contentId)
        || !controlReadRevisionToken(reader, "expectedRevision", expectedRevision)
        || !controlReadRevisionToken(reader, "expectedKeyRevision", out.expectedKeyRevision))
        return failWith(reader, why);
    if (!contentId || !expectedRevision)
        return reject(reader, why, "contentId, expectedRevision and ops are required");
    const JsonValue* ops = reader.member("ops");
    if (ops == nullptr || !ops->isArray() || ops->size() < 1 || ops->size() > 32)
        return reject(reader, why, "ops must be an array of 1 to 32 ops");
    bool clipKey = false;
    for (std::size_t i = 0; i < ops->size(); ++i) {
        NoteOp op;
        if (!parseOp(ops->at(i), op, why)) {
            why = "ops[" + std::to_string(i) + "]: " + why;
            return false;
        }
        clipKey = clipKey || op.clipKey;
        out.ops.push_back(std::move(op));
    }
    if (const JsonValue* dryRun = reader.member("dryRun")) {
        if (!dryRun->isBoolean())
            return reject(reader, why, "dryRun must be a boolean");
        out.dryRun = dryRun->asBoolean();
    }
    if (clipKey && !out.expectedKeyRevision)
        return reject(reader, why, "key clipKey needs expectedKeyRevision");
    out.contentId = *contentId;
    out.expectedRevision = *expectedRevision;
    return true;
}

// ---- content.editNotes: the ops --------------------------------------------------------------------------

/** Sets the note's target to midi with no cent offset, unless it is there already. The base pitch is
    PitchUtils::midiToFreq() of midi at the instance's tuning, the conversion OpenTune uses for note pitches; it
    gives 0 Hz for MIDI 0 (and below), so targetMidi 0 leaves the note without a base pitch: its target reads as
    MIDI 0, a cent shift changes only its offset (Note::getAdjustedPitch() stays 0), and a later setTargetPitch
    gives it a base pitch again. */
bool setTarget(Note& note, double midi, float tuningHz)
{
    const float pitch = PitchUtils::midiToFreq(static_cast<float>(midi), tuningHz);
    if (note.pitchOffset == 0.0f
        && std::abs(controlNoteTargetMidi(note, tuningHz) - PitchUtils::freqToMidi(pitch, tuningHz)) < kPitchTolerance)
        return false;
    note.pitch = pitch;
    note.pitchOffset = 0.0f;
    return true;
}

bool setParameter(float& field, const std::optional<double>& value)
{
    if (!value || std::abs(static_cast<double>(field) - *value) <= kParameterTolerance)
        return false;
    field = static_cast<float>(*value);
    return true;
}

/** Applies a direct note edit; scale snapping is handled by the shared content planner. */
bool applyOp(const NoteOp& op, Note& note, float tuningHz)
{
    bool changed = false;
    switch (op.kind) {
        case NoteOpKind::SetTargetPitch:
            changed = setTarget(note, op.targetMidi, tuningHz);
            break;
        case NoteOpKind::ShiftCents:
            // pitchOffset is in semitones; the target moves with it (Note::getAdjustedPitch()).
            if (std::abs(op.deltaCents) > kParameterTolerance) {
                note.pitchOffset += static_cast<float>(op.deltaCents / 100.0);
                changed = true;
            }
            break;
        case NoteOpKind::SnapToScale:
            break; // handled by planAutoSnap()
        case NoteOpKind::SetParams: {
            // Every given parameter, as the knobs and the modulation and drift tools set them.
            const bool retune = setParameter(note.retuneSpeed, op.retuneSpeed);
            const bool depth = setParameter(note.vibratoDepth, op.vibratoDepth);
            const bool rate = setParameter(note.vibratoRate, op.vibratoRateHz);
            const bool drift = setParameter(note.pitchDriftScale, op.pitchDriftScale);
            changed = retune || depth || rate || drift;
            break;
        }
    }
    // As the piano roll marks an edited note (its render marker).
    if (changed)
        note.dirty = true;
    return changed;
}

/** The undo description the UI gives the same edit: the pitch move and the snap of the pitch tool
    (PianoRollToolHandler), the parameter knobs (PianoRollComponent::editParameter()) and the drift tool. The UI
    takes the text with LOC_RAW (the language of the window it belongs to) at the time of the edit; so does this,
    on the message thread, with the same keys. */
juce::String descriptionOf(const NoteOp& op)
{
    switch (op.kind) {
        case NoteOpKind::SetTargetPitch:
        case NoteOpKind::ShiftCents:
            return LOC_RAW("Move note");
        case NoteOpKind::SnapToScale:
            return LOC_RAW("Pitch snap");
        case NoteOpKind::SetParams:
            if (op.pitchDriftScale && !op.retuneSpeed && !op.vibratoDepth && !op.vibratoRateHz)
                return LOC_RAW("Drift correction");
            return LOC_RAW("Edit note");
    }
    return LOC_RAW("Edit note");
}

/** The positions of the selected notes in storage, at the notes of expectedRevision. order maps a note index
    (start-time order, as content.getNotes counts) to its position. An index beyond the notes gives
    NOT_FOUND(unknown_handle). */
Result<std::vector<std::size_t>> resolveSelection(const NoteSelection& select, const std::vector<Note>& notes,
                                                  const std::vector<std::size_t>& order)
{
    std::vector<std::size_t> positions;
    switch (select.kind) {
        case NoteSelection::Kind::Indices:
            for (const auto index : select.indices) {
                if (index >= order.size())
                    return controlOpError("NOT_FOUND", "unknown_handle",
                                          "a note index is outside the notes at expectedRevision");
                positions.push_back(order[index]);
            }
            break;
        case NoteSelection::Kind::Range:
            // The notes that overlap the range, as content.getNotes selects them.
            for (const auto position : order)
                if (notes[position].endTime > select.startSec && notes[position].startTime < select.endSec)
                    positions.push_back(position);
            break;
        case NoteSelection::Kind::All:
            positions = order;
            break;
    }
    return positions;
}

Result<JsonValue> runEditNotes(ControlOpContext& context, const EditNotesParams& params)
{
    auto content = controlResolveContent(context, params.contentId);
    if (!content.ok())
        return content.error();
    const auto& ref = content.value();
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (auto notReady = controlCheckContentReady(context, ref))
        return *notReady;
    const auto versions = controlContentVersions(context, ref.contentId, *ref.snapshot);
    if (auto conflict = controlCheckExpectedTokens(versions, params.expectedRevision, params.expectedKeyRevision))
        return *conflict;
    const auto& snapshot = *ref.snapshot;
    // clipKey on a content without a key: there is no key to snap to (as content.autoTune refuses it).
    const bool usesClipKey =
        std::any_of(params.ops.begin(), params.ops.end(), [](const NoteOp& op) { return op.clipKey; });
    if (usesClipKey && snapshot.detectedKey.origin == Origin::Unset)
        return controlOpError("INVALID_ARGUMENT", "content_key_unset",
                              "key clipKey names the content's own key, but the content has no key; give the key "
                              "explicitly");
    // Note indexes count in start-time order, as content.getNotes counts them (controlNoteIndexOrder).
    const auto& before = snapshot.notes;
    EditableContentSnapshot planning = snapshot;
    const auto order = controlNoteIndexOrder(before);
    std::vector<std::vector<std::size_t>> selections;
    for (const auto& op : params.ops) {
        auto positions = resolveSelection(op.select, before, order);
        if (!positions.ok())
            return positions.error();
        selections.push_back(std::move(positions).value());
    }
    auto history = historyOf(context);
    if (!history.ok())
        return history.error();

    auto& processor = context.processor();
    auto commands = processor.getContentCommands();
    const float tuningHz = TuningConfig::currentTuningHz();
    const auto beforeWorld = controlCaptureWorldVersions(context);
    ControlWriteEnvelope envelope(context);
    envelope.touchContent(ref.contentId);
    std::vector<Note> working = before;
    juce::String description;
    std::size_t plannedOps = 0;
    F0FrameRange editRangeUnion;
    for (std::size_t i = 0; i < params.ops.size(); ++i) {
        const auto& op = params.ops[i];
        const auto previous = working;
        F0FrameRange editRange;
        std::optional<ContentNoteCorrectionPlan> correction;
        if (op.kind == NoteOpKind::SnapToScale) {
            const std::optional<std::vector<std::size_t>> targets{selections[i]};
            const auto scaleSnap = makeScaleSnapConfig(op.clipKey ? snapshot.detectedKey : *op.key)
                                       .value_or(ScaleSnapConfig{});
            auto snap = commands->planAutoSnap(planning, targets, scaleSnap, NoteGeneratorParams{});
            if (snap.status != ContentAutoSnapPlan::Status::Commit)
                continue;
            editRange = commands->noteFrameRange(*planning.pitchCurve, working, selections[i]);
            if (editRange.isEmpty())
                continue;
            correction = std::move(snap.correction);
        } else {
            std::vector<std::size_t> changed;
            for (const auto position : selections[i])
                if (applyOp(op, working[position], tuningHz))
                    changed.push_back(position);
            if (changed.empty())
                continue;
            // Keep the original note edit range; the correction planner expands it for the commit.
            editRange = commands->noteFrameRange(*planning.pitchCurve, working, changed);
            if (editRange.isEmpty()) {
                working = previous;
                continue;
            }
            correction = commands->planNoteCorrection(planning, working, editRange, NoteGeneratorParams{});
            if (!correction) {
                working = previous;
                continue;
            }
        }
        working = correction->notes;
        planning.notes = working;
        planning.pitchCurve = correction->correctedCurve;
        if (plannedOps == 0)
            description = descriptionOf(op);
        if (plannedOps++ == 0) {
            editRangeUnion = editRange;
        } else {
            editRangeUnion.startFrame = std::min(editRangeUnion.startFrame, editRange.startFrame);
            editRangeUnion.endFrameExclusive = std::max(editRangeUnion.endFrameExclusive, editRange.endFrameExclusive);
        }
    }

    std::vector<Note> after = planning.notes;
    std::string revisionAfter = versions.revision();
    if (plannedOps != 0) {
        const auto correction = commands->planNoteCorrection(
            planning, planning.notes, editRangeUnion, NoteGeneratorParams{});
        if (!correction)
            return controlOpError("INTERNAL", "", "OpenTune did not plan the note edit");
        after = correction->notes;
        planning.notes = after;
        planning.pitchCurve = correction->correctedCurve;
        if (!params.dryRun) {
            auto edit = commands->commitNotesAndSegmentsUndoable(
                ref.key, correction->notes, correction->segments,
                ContentEditRangeFrames{correction->affectedRange.startFrame,
                                       correction->affectedRange.endFrameExclusive},
                description);
            if (edit.snapshot == nullptr || edit.undoAction == nullptr)
                return controlOpError("INTERNAL", "", "OpenTune did not commit the note edit");

            auto& manager = processor.getUndoManager();
            const auto undoId = history.value()->assignUndoId(
                manager, *edit.undoAction, manager.cursor() - 1, controlContentEpoch(context),
                context.instanceShort());
            const std::vector<std::string> names{controlUndoActionName(*edit.undoAction)};
            envelope.setUndoable(undoId, description.toStdString(), names);
        }
    }
    if (!params.dryRun && plannedOps != 0) {
        // Read the state committed by the shared content command.
        auto committed = controlReadCommittedNotes(context, ref, before.size());
        if (!committed.ok())
            return committed.error();
        auto read = std::move(committed).value();
        after = std::move(read.notes);
        revisionAfter = std::move(read.revision);
    }
    const auto omitted = controlListNoteChanges(envelope, ref.contentId, before, after, revisionAfter, kMaxListedNotes);
    JsonValue result;
    if (params.dryRun) {
        // A commit of the predicted changes advances the notes revision, so it would report the content in
        // reRenderQueued: the prediction lists the commit's side effects.
        if (plannedOps != 0)
            controlAddPredictedReRenderQueued(envelope, ref.contentId);
        result = envelope.predicted(context);
    } else {
        controlAddReRenderQueued(envelope, beforeWorld, controlCaptureWorldVersions(context));
        result = envelope.committed(context);
    }
    result.appendMember("omittedChangeCount", JsonValue::integer(static_cast<std::int64_t>(omitted)));
    return result;
}

// ---- content.setPitchShift ------------------------------------------------------------------------

struct SetPitchShiftParams
{
    ControlHandle contentId;
    PitchShiftSettings shift;
};

/** Parses content.setPitchShift params, including pitchShift. */
bool parseSetPitchShift(const JsonValue& params, SetPitchShiftParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<ControlHandle> contentId;
    if (!reader.onlyMembers({"contentId", "pitchShift"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, contentId))
        return failWith(reader, why);
    const JsonValue* shift = reader.member("pitchShift");
    if (!contentId || shift == nullptr)
        return reject(reader, why, "contentId and pitchShift are required");
    if (!shift->isObject())
        return reject(reader, why, "pitchShift must be {semitones, cents}");
    ControlParamReader inner(*shift);
    std::optional<std::int64_t> semitones;
    std::optional<std::int64_t> cents;
    if (!inner.onlyMembers({"semitones", "cents"}) || !inner.optionalInteger("semitones", -24, 24, semitones)
        || !inner.optionalInteger("cents", -99, 99, cents))
        return reject(reader, why, "pitchShift." + inner.why());
    if (!semitones || !cents)
        return reject(reader, why, "pitchShift must be {semitones, cents}");
    out.contentId = *contentId;
    out.shift.semitone = static_cast<int>(*semitones);
    out.shift.cents = static_cast<int>(*cents);
    return true;
}

Result<JsonValue> runSetPitchShift(ControlOpContext& context, const SetPitchShiftParams& params)
{
    auto content = controlResolveContent(context, params.contentId);
    if (!content.ok())
        return content.error();
    const auto& ref = content.value();
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (auto notReady = controlCheckContentReady(context, ref))
        return *notReady;
    auto history = historyOf(context);
    if (!history.ok())
        return history.error();

    auto& processor = context.processor();
    ControlWriteEnvelope envelope(context);
    envelope.touchContent(ref.contentId);
    const auto before = controlCaptureEditState(context);
    // The shared command applies the shift and pushes its action; its non-owning result identifies that history entry.
    auto commands = processor.getContentCommands();
    auto* shift = commands->commitPitchShiftEdit(ref.key, params.shift);
    if (shift == nullptr)
        return envelope.committed(context);
    const auto description = shift->getDescription();
    auto& manager = processor.getUndoManager();
    const auto undoId = history.value()->assignUndoId(
        manager, *shift, manager.cursor() - 1, controlContentEpoch(context), context.instanceShort());
    const std::vector<std::string> names{controlUndoActionName(*shift)};

    // Pitch-shift-only edits keep the content revision; the pitchShift token reflects the change.
    controlDescribeEffect(context, before, controlCaptureEditState(context), envelope);
    envelope.addWarning("KEY_NOT_TRANSPOSED", contentRef(ref.contentId));
    envelope.setUndoable(undoId, description.toStdString(), names);
    return envelope.committed(context);
}

} // namespace

void registerContentEditOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<EditNotesParams>("content.editNotes", parseEditNotes, runEditNotes));
    table.add(controlWriteMethod<SetPitchShiftParams>("content.setPitchShift", parseSetPitchShift, runSetPitchShift));
}

} // namespace OpenTune
