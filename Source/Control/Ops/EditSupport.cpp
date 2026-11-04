// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EditSupport.h"

#include "OpsJson.h"
#include "OpsSupport.h"

#include "Content/EditableContentSnapshot.h"
#include "PluginProcessor.h"
#include "Utils/PitchUtils.h"
#include "Utils/TuningConfig.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace OpenTune {

namespace {

const ControlContentVersions* findVersions(const ControlWorldVersions& world, const std::string& contentId)
{
    for (const auto& versions : world.contents)
        if (versions.contentId == contentId)
            return &versions;
    return nullptr;
}

const ControlEditState::Content* findContent(const ControlEditState& state, const std::string& contentId)
{
    for (const auto& content : state.contents)
        if (content.contentId == contentId)
            return &content;
    return nullptr;
}

const ControlPlacementRef* findPlacement(const ControlEditState& state, std::uint64_t placementId)
{
    for (const auto& placement : state.placements)
        if (placement.placement.placementId == placementId)
            return &placement;
    return nullptr;
}

/** Adds {before, after, unit} to fields when the values differ. */
void addField(JsonValue& fields, const char* name, JsonValue before, JsonValue after, const char* unit)
{
    if (toJsonString(before) == toJsonString(after))
        return;
    auto field = JsonValue::object();
    field.appendMember("before", std::move(before));
    field.appendMember("after", std::move(after));
    field.appendMember("unit", unit != nullptr ? JsonValue::string(unit) : JsonValue());
    fields.appendMember(name, std::move(field));
}

JsonValue contentRef(const std::string& contentId)
{
    auto ref = JsonValue::object();
    ref.appendMember("contentId", JsonValue::string(contentId));
    return ref;
}

JsonValue placementRef(const std::string& placementId)
{
    auto ref = JsonValue::object();
    ref.appendMember("placementId", JsonValue::string(placementId));
    return ref;
}

/** The origin of a key as common.schema.json#/$defs/key names it; null for a content without a key. */
JsonValue originJson(const DetectedKey& key)
{
    if (key.origin == Origin::Unset)
        return JsonValue();
    return JsonValue::string(key.origin == Origin::Manual ? "manual" : "automatic");
}

JsonValue colourJson(std::uint32_t argb)
{
    static const char* const digits = "0123456789ABCDEF";
    std::string text = "#";
    for (int shift = 20; shift >= 0; shift -= 4)
        text += digits[(argb >> shift) & 0xf];
    return JsonValue::string(text);
}

bool renderInputChanged(const ControlContentVersions& was, const ControlContentVersions& now)
{
    return was.notes != now.notes || was.pitch != now.pitch || was.content != now.content || was.grid != now.grid
           || was.pitchShift != now.pitchShift;
}

void describeContents(const ControlEditState& before, const ControlEditState& after, ControlWriteEnvelope& envelope)
{
    std::vector<std::string> ids;
    for (const auto& versions : before.versions.contents)
        ids.push_back(versions.contentId);
    for (const auto& versions : after.versions.contents)
        if (std::find(ids.begin(), ids.end(), versions.contentId) == ids.end())
            ids.push_back(versions.contentId);
    for (const auto& id : ids) {
        const auto* was = findVersions(before.versions, id);
        const auto* now = findVersions(after.versions, id);
        if (was == nullptr || now == nullptr) {
            // A content that a step brought back or that left with its clip.
            if (was == nullptr)
                envelope.addCreated("content", contentRef(id), JsonValue());
            else
                envelope.addRemoved("content", contentRef(id), JsonValue());
            envelope.touchContent(id);
            continue;
        }
        auto fields = JsonValue::object();
        addField(fields, "revision", JsonValue::string(was->revision()), JsonValue::string(now->revision()), nullptr);
        addField(fields, "gridRevision", JsonValue::string(was->grid), JsonValue::string(now->grid), nullptr);
        if (fields.size() != 0)
            envelope.addChange("content", contentRef(id), JsonValue(), std::move(fields));
        const auto* wasValues = findContent(before, id);
        const auto* nowValues = findContent(after, id);
        if (wasValues != nullptr && nowValues != nullptr) {
            auto key = JsonValue::object();
            addField(key, "root", JsonValue::integer(static_cast<int>(wasValues->key.root)),
                     JsonValue::integer(static_cast<int>(nowValues->key.root)), nullptr);
            addField(key, "scale", JsonValue::string(controlScaleName(wasValues->key.scale)),
                     JsonValue::string(controlScaleName(nowValues->key.scale)), nullptr);
            addField(key, "origin", originJson(wasValues->key), originJson(nowValues->key), nullptr);
            if (key.size() != 0)
                envelope.addChange("key", contentRef(id), JsonValue(), std::move(key));
            auto shift = JsonValue::object();
            addField(shift, "semitones", JsonValue::integer(wasValues->pitchShift.semitone),
                     JsonValue::integer(nowValues->pitchShift.semitone), "semitones");
            addField(shift, "cents", JsonValue::integer(wasValues->pitchShift.cents),
                     JsonValue::integer(nowValues->pitchShift.cents), "cents");
            if (shift.size() != 0)
                envelope.addChange("pitchShift", contentRef(id), JsonValue(), std::move(shift));
        }
        if (was->revision() != now->revision() || was->grid != now->grid || was->key != now->key
            || was->pitchShift != now->pitchShift)
            envelope.touchContent(id);
    }
}

void describePlacements(const ControlEditState& before, const ControlEditState& after, ControlWriteEnvelope& envelope)
{
    for (const auto& now : after.placements) {
        const auto* was = findPlacement(before, now.placement.placementId);
        if (was == nullptr) {
            envelope.addCreated("placement", placementRef(now.placementId), JsonValue());
            envelope.touchPlacement(now.placement.placementId);
            continue;
        }
        auto fields = controlPlacementChangeFields(*was, now);
        if (fields.size() == 0)
            continue;
        envelope.addChange("placement", placementRef(now.placementId), JsonValue(), std::move(fields));
        envelope.touchPlacement(now.placement.placementId);
    }
    for (const auto& was : before.placements)
        if (findPlacement(after, was.placement.placementId) == nullptr)
            envelope.addRemoved("placement", placementRef(was.placementId), JsonValue());
}

void describeTracks(const ControlEditState& before, const ControlEditState& after, ControlWriteEnvelope& envelope)
{
    const auto count = std::min(before.tracks.size(), after.tracks.size());
    for (std::size_t trackId = 0; trackId < count; ++trackId) {
        const auto& was = before.tracks[trackId];
        const auto& now = after.tracks[trackId];
        auto fields = JsonValue::object();
        addField(fields, "muted", JsonValue::boolean(was.muted), JsonValue::boolean(now.muted), nullptr);
        addField(fields, "solo", JsonValue::boolean(was.solo), JsonValue::boolean(now.solo), nullptr);
        addField(fields, "volumeLinear", controlJsonNumber(was.volumeLinear, 6), controlJsonNumber(now.volumeLinear, 6),
                 "linear");
        addField(fields, "colour", colourJson(was.colour), colourJson(now.colour), nullptr);
        addField(fields, "visible", JsonValue::boolean(was.visible), JsonValue::boolean(now.visible), nullptr);
        if (fields.size() == 0)
            continue;
        auto ref = JsonValue::object();
        ref.appendMember("trackId", JsonValue::integer(static_cast<std::int64_t>(trackId)));
        envelope.addChange("track", std::move(ref), JsonValue::string("Track " + std::to_string(trackId + 1)),
                           std::move(fields));
    }
}

} // namespace

// ---- Placement changes ------------------------------------------------------------------------------

JsonValue controlPlacementChangeFields(const ControlPlacementRef& was, const ControlPlacementRef& now)
{
    const auto gainDb = [](float gain) {
        const auto db = controlLinearToDb(gain);
        return db ? controlJsonNumber(*db, 3) : JsonValue();
    };
    auto fields = JsonValue::object();
    const auto& a = was.placement;
    const auto& b = now.placement;
    addField(fields, "trackId", JsonValue::integer(was.trackId), JsonValue::integer(now.trackId), nullptr);
    addField(fields, "startTimelineSec", controlJsonSeconds(a.timelineStartSeconds),
             controlJsonSeconds(b.timelineStartSeconds), "sec");
    addField(fields, "endTimelineSec", controlJsonSeconds(a.timelineEndSeconds()),
             controlJsonSeconds(b.timelineEndSeconds()), "sec");
    addField(fields, "durationSec", controlJsonSeconds(a.durationSeconds), controlJsonSeconds(b.durationSeconds),
             "sec");
    addField(fields, "clipInContentSec", controlJsonSeconds(a.clipInSeconds), controlJsonSeconds(b.clipInSeconds),
             "sec");
    addField(fields, "gainDb", gainDb(a.gain), gainDb(b.gain), "db");
    addField(fields, "gainLinear", controlJsonNumber(std::max(0.0f, a.gain), 6),
             controlJsonNumber(std::max(0.0f, b.gain), 6), "linear");
    addField(fields, "fadeInSec", controlJsonSeconds(a.fadeInDuration), controlJsonSeconds(b.fadeInDuration), "sec");
    addField(fields, "fadeOutSec", controlJsonSeconds(a.fadeOutDuration), controlJsonSeconds(b.fadeOutDuration), "sec");
    return fields;
}

// ---- Params ---------------------------------------------------------------------------------------

bool controlReadRevisionToken(ControlParamReader& reader, std::string_view name, std::optional<std::string>& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    const auto valid = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'
               || c == ':' || c == '-';
    };
    if (!value->isString() || value->text().empty() || value->text().size() > 128
        || !std::all_of(value->text().begin(), value->text().end(), valid))
        return reader.fail(std::string(name) + " must be a revision token");
    out = value->text();
    return true;
}

std::optional<Scale> controlScaleFromName(std::string_view name)
{
    for (const auto scale : {Scale::Major, Scale::Minor, Scale::Chromatic, Scale::HarmonicMinor, Scale::Dorian,
                             Scale::Mixolydian, Scale::PentatonicMajor, Scale::PentatonicMinor})
        if (name == controlScaleName(scale))
            return scale;
    return std::nullopt;
}

bool controlParseKeyInput(const JsonValue& value, DetectedKey& out)
{
    const JsonValue* root = value.isObject() ? value.find("root") : nullptr;
    const JsonValue* scale = value.isObject() ? value.find("scale") : nullptr;
    if (root == nullptr || scale == nullptr || value.size() != 2 || !root->isNumber() || !scale->isString())
        return false;
    const double number = root->asDouble();
    const auto parsedScale = controlScaleFromName(scale->text());
    if (std::floor(number) != number || number < 0 || number > 11 || !parsedScale)
        return false;
    // makeDetectedKeyFromUi(): a key chosen by hand is manual, with confidence 1.
    out = DetectedKey{};
    out.root = static_cast<Key>(static_cast<int>(number));
    out.scale = *parsedScale;
    out.confidence = 1.0f;
    out.origin = Origin::Manual;
    return true;
}

// ---- Checks ---------------------------------------------------------------------------------------

std::optional<Error> controlCheckContentReady(ControlOpContext& context, const ControlContentRef& content)
{
    const auto& snapshot = *content.snapshot;
    switch (snapshot.originalF0State) {
        case OriginalF0State::NotRequested:
        case OriginalF0State::Extracting:
            return controlOpError("NOT_READY", "f0_not_ready", "the pitch analysis of the content has not finished");
        case OriginalF0State::Failed:
            // The failure message OpenTune keeps with the content tells an analysis without voiced frames.
            if (std::string_view(controlF0FailureCode(snapshot)) == "f0_empty")
                return controlOpError("NOT_READY", "f0_empty", "the pitch analysis found no voiced frame");
            return controlOpError("NOT_READY", "f0_failed", "the pitch analysis of the content failed");
        case OriginalF0State::Ready:
            break;
    }
    const bool voiced = snapshot.hasUsableOriginalF0()
                        && std::any_of(snapshot.pitchCurve->getOriginalF0().begin(),
                                       snapshot.pitchCurve->getOriginalF0().end(), [](float hz) { return hz > 0.0f; });
    if (!voiced)
        return controlOpError("NOT_READY", "f0_empty", "the pitch analysis found no voiced frame");
    return std::nullopt;
}

std::optional<Error> controlCheckExpectedTokens(const ControlContentVersions& versions,
                                                const std::optional<std::string>& expectedRevision,
                                                const std::optional<std::string>& expectedKeyRevision)
{
    if (expectedRevision && *expectedRevision != versions.revision())
        return controlOpError("REVISION_CONFLICT", "content_changed",
                              "the content's revision differs from expectedRevision; read it again");
    if (expectedKeyRevision && *expectedKeyRevision != versions.key)
        return controlOpError("REVISION_CONFLICT", "key_changed",
                              "the content's key revision differs from expectedKeyRevision; read it again");
    return std::nullopt;
}

// ---- Results ----------------------------------------------------------------------------------------

JsonValue controlNoteRefJson(const std::string& contentId, std::size_t index, const std::string& revision)
{
    auto note = JsonValue::object();
    note.appendMember("index", JsonValue::integer(static_cast<std::int64_t>(index)));
    note.appendMember("atRevision", JsonValue::string(revision));
    auto ref = JsonValue::object();
    ref.appendMember("contentId", JsonValue::string(contentId));
    ref.appendMember("note", std::move(note));
    return ref;
}

JsonValue controlNoteLabelJson(std::size_t index, const Note& note)
{
    std::optional<double> targetMidi;
    if (note.isVoiced && note.getAdjustedPitch() > 0.0f)
        targetMidi = controlNoteTargetMidi(note, TuningConfig::currentTuningHz());
    return JsonValue::string(controlNoteLabel(index, targetMidi, note.startTime, note.endTime));
}

void controlAddReRenderQueued(ControlWriteEnvelope& envelope, const ControlWorldVersions& before,
                              const ControlWorldVersions& after)
{
    auto ids = JsonValue::array();
    for (const auto& now : after.contents)
        if (const auto* was = findVersions(before, now.contentId); was != nullptr && renderInputChanged(*was, now))
            ids.append(JsonValue::string(now.contentId));
    if (ids.size() == 0)
        return;
    auto effect = JsonValue::object();
    effect.appendMember("contentIds", std::move(ids));
    effect.appendMember("revert", JsonValue());
    envelope.addSideEffect("reRenderQueued", std::move(effect));
}

void controlAddPredictedReRenderQueued(ControlWriteEnvelope& envelope, const std::string& contentId)
{
    auto ids = JsonValue::array();
    ids.append(JsonValue::string(contentId));
    auto effect = JsonValue::object();
    effect.appendMember("contentIds", std::move(ids));
    effect.appendMember("revert", JsonValue());
    envelope.addSideEffect("reRenderQueued", std::move(effect));
}

std::size_t controlListNoteChanges(ControlWriteEnvelope& envelope, const std::string& contentId,
                                   const std::vector<Note>& before, const std::vector<Note>& after,
                                   const std::string& revision, std::size_t maxListed)
{
    jassert(before.size() == after.size());
    const auto count = std::min(before.size(), after.size());
    // Note indexes count in start-time order, as content.getNotes counts them, each list in its own: the commit
    // stores the notes sorted, also when they were stored in another order before.
    const auto beforeOrder = controlNoteIndexOrder(before);
    const auto afterOrder = controlNoteIndexOrder(after);
    const float tuningHz = TuningConfig::currentTuningHz();
    std::size_t listed = 0;
    std::size_t omitted = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& was = before[beforeOrder[index]];
        const auto& now = after[afterOrder[index]];
        auto fields = JsonValue::object();
        // The commit trims a note that overlaps its successor (normalizeStoredNotes()): the count of notes stays,
        // but the times differ from what was read before. Listed as content.getNotes reports them.
        addField(fields, "startContentSec", controlJsonSeconds(std::max(0.0, was.startTime)),
                 controlJsonSeconds(std::max(0.0, now.startTime)), "sec");
        addField(fields, "endContentSec", controlJsonSeconds(std::max(0.0, was.endTime)),
                 controlJsonSeconds(std::max(0.0, now.endTime)), "sec");
        addField(fields, "targetMidi", controlNoteTargetMidiJson(was, tuningHz),
                 controlNoteTargetMidiJson(now, tuningHz), "midi");
        addField(fields, "offsetCents", controlNoteOffsetCentsJson(was), controlNoteOffsetCentsJson(now), "cents");
        addField(fields, "retuneSpeed", controlJsonNumber(was.retuneSpeed, 4), controlJsonNumber(now.retuneSpeed, 4),
                 nullptr);
        addField(fields, "vibratoDepth", controlJsonNumber(was.vibratoDepth, 4), controlJsonNumber(now.vibratoDepth, 4),
                 nullptr);
        addField(fields, "vibratoRateHz", controlJsonNumber(was.vibratoRate, 4), controlJsonNumber(now.vibratoRate, 4),
                 "hz");
        addField(fields, "pitchDriftScale", controlJsonNumber(was.pitchDriftScale, 4),
                 controlJsonNumber(now.pitchDriftScale, 4), nullptr);
        if (fields.size() == 0)
            continue;
        if (listed >= maxListed) {
            ++omitted;
            continue;
        }
        ++listed;
        envelope.addChange("note", controlNoteRefJson(contentId, index, revision), controlNoteLabelJson(index, now),
                           std::move(fields));
    }
    return omitted;
}

Result<ControlCommittedNotes> controlReadCommittedNotes(ControlOpContext& context, const ControlContentRef& content,
                                                        std::optional<std::size_t> expectedCount)
{
    const auto now = controlLiveSnapshot(context, content.key);
    if (now == nullptr)
        return controlOpErrorAfterChange("INTERNAL", "",
                                         "the content cannot be read back after the commit; read it again");
    if (expectedCount && now->notes.size() != *expectedCount)
        return controlOpErrorAfterChange("INTERNAL", "",
                                         "OpenTune stored another number of notes than the edit gave it; read the "
                                         "notes again");
    ControlCommittedNotes committed;
    committed.notes = now->notes;
    committed.revision = controlContentVersions(context, content.contentId, *now).revision();
    return committed;
}

// ---- The effect of an undo or redo step --------------------------------------------------------------

ControlEditState controlCaptureEditState(ControlOpContext& context)
{
    ControlEditState state;
    state.versions = controlCaptureWorldVersions(context);
    for (const auto& key : controlLiveContents(context))
        if (const auto snapshot = controlLiveSnapshot(context, key))
            state.contents.push_back(
                {controlContentId(context, key), snapshot->detectedKey, snapshot->pitchShiftSettings});
    state.placements = controlLivePlacements(context);
    if (const auto* arrangement = context.processor().getStandaloneArrangement()) {
        bool derived = false;
        const int visible = controlVisibleTrackCount(context, derived);
        for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId) {
            ControlEditState::Track track;
            track.muted = arrangement->isTrackMuted(trackId);
            track.solo = arrangement->isTrackSolo(trackId);
            track.volumeLinear = arrangement->getTrackVolume(trackId);
            track.colour = arrangement->getTrackColour(trackId).getARGB();
            track.visible = trackId < visible;
            state.tracks.push_back(track);
        }
    }
    return state;
}

void controlDescribeEffect(ControlOpContext& context, const ControlEditState& before, const ControlEditState& after,
                           ControlWriteEnvelope& envelope)
{
    describeContents(before, after, envelope);
    describePlacements(before, after, envelope);
    describeTracks(before, after, envelope);
    controlAddReRenderQueued(envelope, before.versions, after.versions);
}

} // namespace OpenTune
