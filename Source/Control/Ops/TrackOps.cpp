// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackOps.h"

#include "ClipIds.h"
#include "EditSupport.h"
#include "OpsSupport.h"
#include "TrackSupport.h"
#include "WriteEnvelope.h"

#include "Content/StandaloneContentRepository.h"
#include "Control/ControlUiBridge.h"
#include "Control/Edit/ControlOriginAction.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace OpenTune {

namespace {

/** The range of OpenTune's volume knob (VolumeKnob in Source/Standalone/UI/TrackPanelComponent.h): linear 0 to 4. */
constexpr double kMaxVolumeLinear = 4.0;
/** Longest list of tracks the schema allows in one track.set. */
constexpr std::size_t kMaxTracksPerSet = 64;

JsonValue fieldChange(JsonValue before, JsonValue after, const char* unit)
{
    auto field = JsonValue::object();
    field.appendMember("before", std::move(before));
    field.appendMember("after", std::move(after));
    field.appendMember("unit", unit != nullptr ? JsonValue::string(unit) : JsonValue());
    return field;
}

/** Adds {before, after, unit} to fields when the values differ. */
void addField(JsonValue& fields, const char* name, JsonValue before, JsonValue after, const char* unit)
{
    if (toJsonString(before) == toJsonString(after))
        return;
    fields.appendMember(name, fieldChange(std::move(before), std::move(after), unit));
}

JsonValue volumeDbJson(double linear)
{
    const auto db = controlLinearToDb(linear);
    return db ? controlJsonNumber(*db, 3) : JsonValue();
}

/** The members of a track that differ between two captures (PROTOCOL.md section 14.3), including audible, which
    follows the solo flags of the other tracks; withAudible false leaves it out. */
JsonValue trackFields(const ControlTrackState& was, const ControlTrackState& now, bool withAudible = true)
{
    auto fields = JsonValue::object();
    addField(fields, "muted", JsonValue::boolean(was.muted), JsonValue::boolean(now.muted), nullptr);
    addField(fields, "solo", JsonValue::boolean(was.solo), JsonValue::boolean(now.solo), nullptr);
    addField(fields, "volumeDb", volumeDbJson(was.volumeLinear), volumeDbJson(now.volumeLinear), "db");
    addField(fields, "volumeLinear", controlJsonNumber(was.volumeLinear, 6), controlJsonNumber(now.volumeLinear, 6),
             "linear");
    addField(fields, "colour", JsonValue::string(controlColourText(was.colourRgb)),
             JsonValue::string(controlColourText(now.colourRgb)), nullptr);
    addField(fields, "visible", JsonValue::boolean(was.visible), JsonValue::boolean(now.visible), nullptr);
    if (withAudible)
        addField(fields, "audible", JsonValue::boolean(was.audible), JsonValue::boolean(now.audible), nullptr);
    return fields;
}

/** Records one change of entity track for every track whose members differ between the captures, in track order.
    audible is listed for the tracks that are shown and those the call named (named): soloing a track changes what
    every other track plays, but a hidden track that the call did not name is not the client's business. */
void describeTracks(const std::vector<ControlTrackState>& before, const std::vector<ControlTrackState>& after,
                    ControlWriteEnvelope& envelope, const std::set<int>& named = {})
{
    const auto count = std::min(before.size(), after.size());
    for (std::size_t i = 0; i < count; ++i) {
        const bool withAudible = after[i].visible || named.count(after[i].trackId) != 0;
        auto fields = trackFields(before[i], after[i], withAudible);
        if (fields.size() != 0)
            envelope.addChange("track", controlTrackRef(after[i].trackId), controlTrackLabel(after[i].trackId),
                               std::move(fields));
    }
}

std::string fixed2(double value)
{
    const auto hundredths = static_cast<long long>(std::llround(std::max(0.0, value) * 100.0));
    std::string fraction = std::to_string(hundredths % 100);
    if (fraction.size() < 2)
        fraction.insert(0, 1, '0');
    return std::to_string(hundredths / 100) + "." + fraction;
}

/** The display label of a clip: its track and its time span, for example "Track 2 2.00-8.00s". */
std::string placementLabel(const ControlPlacementRef& ref)
{
    return "Track " + std::to_string(ref.trackId + 1) + " " + fixed2(ref.placement.timelineStartSeconds) + "-"
           + fixed2(ref.placement.timelineEndSeconds()) + "s";
}

JsonValue placementRef(const std::string& placementId)
{
    auto ref = JsonValue::object();
    ref.appendMember("placementId", JsonValue::string(placementId));
    return ref;
}

JsonValue contentRef(const std::string& contentId)
{
    auto ref = JsonValue::object();
    ref.appendMember("contentId", JsonValue::string(contentId));
    return ref;
}

/** The number of track slots the arrangement shows after the editor was told, read back from the bridge. */
int shownTracks(ControlOpContext& context)
{
    bool derived = false;
    return controlVisibleTrackCount(context, derived);
}

// ---- track.set ------------------------------------------------------------------------------------

struct TrackSetting
{
    int trackId = 0;
    std::optional<bool> muted;
    std::optional<bool> solo;
    std::optional<double> volumeDb;
    std::optional<double> volumeLinear;
    std::optional<std::uint32_t> colourRgb;
};

struct SetParams
{
    std::vector<TrackSetting> tracks;
};

bool parseColour(const JsonValue* value, std::uint32_t& out)
{
    if (value == nullptr || !value->isString() || value->text().size() != 7 || value->text()[0] != '#')
        return false;
    std::uint32_t rgb = 0;
    for (std::size_t i = 1; i < 7; ++i) {
        const char c = value->text()[i];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9')
            digit = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        else
            return false;
        rgb = rgb * 16 + digit;
    }
    out = rgb;
    return true;
}

/** The trackId member of common.schema.json#/$defs/trackId: an integer of 0 or more. */
bool readTrackId(ControlParamReader& reader, int& out)
{
    const JsonValue* value = reader.member("trackId");
    if (value == nullptr || !value->isNumber() || std::floor(value->asDouble()) != value->asDouble()
        || value->asDouble() < 0.0)
        return reader.fail("trackId is required and must be a non-negative integer");
    // A position beyond the slots is a NOT_FOUND of the run; the largest int keeps the conversion defined.
    out = static_cast<int>(std::min(value->asDouble(), 1.0e6));
    return true;
}

bool readBoolean(ControlParamReader& reader, const char* name, std::optional<bool>& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    if (!value->isBoolean())
        return reader.fail(std::string(name) + " must be a boolean");
    out = value->asBoolean();
    return true;
}

/** methods/track.set.schema.json $defs.trackSetting: closed, at least one member besides trackId, and not both
    volumeDb and volumeLinear. */
bool parseTrackSetting(const JsonValue& item, TrackSetting& out, std::string& why)
{
    if (!item.isObject()) {
        why = "tracks must hold objects";
        return false;
    }
    ControlParamReader reader(item);
    std::optional<double> db;
    std::optional<double> linear;
    if (!reader.onlyMembers({"trackId", "muted", "solo", "volumeDb", "volumeLinear", "colour"})
        || !readTrackId(reader, out.trackId) || !readBoolean(reader, "muted", out.muted)
        || !readBoolean(reader, "solo", out.solo)
        || !reader.optionalNumber("volumeDb", -std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                  db)
        || !reader.optionalNumber("volumeLinear", 0.0, std::numeric_limits<double>::max(), linear)) {
        why = reader.why();
        return false;
    }
    if (item.size() < 2 || (db && linear)) {
        why = "a track setting needs a member besides trackId, and at most one of volumeDb and volumeLinear";
        return false;
    }
    out.volumeDb = db;
    out.volumeLinear = linear;
    if (const JsonValue* colour = reader.member("colour")) {
        std::uint32_t rgb = 0;
        if (!parseColour(colour, rgb)) {
            why = "colour must be #RRGGBB";
            return false;
        }
        out.colourRgb = rgb;
    }
    return true;
}

bool parseSet(const JsonValue& params, SetParams& out, std::string& why)
{
    ControlParamReader reader(params);
    const JsonValue* tracks = reader.member("tracks");
    if (!reader.onlyMembers({"tracks"}) || tracks == nullptr || !tracks->isArray() || tracks->size() == 0
        || tracks->size() > kMaxTracksPerSet) {
        why = reader.why().empty() ? "tracks must be a list of 1 to 64 track settings" : reader.why();
        return false;
    }
    for (std::size_t i = 0; i < tracks->size(); ++i) {
        TrackSetting setting;
        if (!parseTrackSetting(tracks->at(i), setting, why))
            return false;
        out.tracks.push_back(setting);
    }
    return true;
}

/** The linear volume a setting asks for, limited to the knob's range; clamped is set with the requested and the
    applied value of the member the caller named, in that member's unit. */
struct RequestedVolume
{
    double linear = 1.0;
    bool clamped = false;
    const char* field = "";
    double requested = 0.0;
    double applied = 0.0;
};

RequestedVolume requestedVolume(const TrackSetting& setting)
{
    RequestedVolume volume;
    if (setting.volumeDb) {
        volume.field = "volumeDb";
        volume.requested = *setting.volumeDb;
        volume.linear = std::pow(10.0, *setting.volumeDb / 20.0);
        if (!(volume.linear <= kMaxVolumeLinear)) {
            volume.clamped = true;
            volume.linear = kMaxVolumeLinear;
            volume.applied = *controlLinearToDb(kMaxVolumeLinear);
        }
    } else {
        volume.field = "volumeLinear";
        volume.requested = *setting.volumeLinear;
        volume.linear = *setting.volumeLinear;
        if (volume.linear > kMaxVolumeLinear) {
            volume.clamped = true;
            volume.linear = kMaxVolumeLinear;
            volume.applied = kMaxVolumeLinear;
        }
    }
    return volume;
}

/** The members of a track that the revert recipe of a track.set sets back. */
JsonValue revertSetting(const ControlTrackState& was, const ControlTrackState& now)
{
    auto setting = JsonValue::object();
    setting.appendMember("trackId", JsonValue::integer(was.trackId));
    if (was.muted != now.muted)
        setting.appendMember("muted", JsonValue::boolean(was.muted));
    if (was.solo != now.solo)
        setting.appendMember("solo", JsonValue::boolean(was.solo));
    if (was.volumeLinear != now.volumeLinear) {
        // The gain in dB, from which the same float comes back; silence has no dB and is set linearly.
        const auto db = controlLinearToDb(was.volumeLinear);
        if (db)
            setting.appendMember("volumeDb", controlJsonNumber(*db, 6));
        else
            setting.appendMember("volumeLinear", controlJsonNumber(0.0, 6));
    }
    if (was.colourRgb != now.colourRgb)
        setting.appendMember("colour", JsonValue::string(controlColourText(was.colourRgb)));
    return setting;
}

Result<JsonValue> runSet(ControlOpContext& context, const SetParams& params)
{
    // The handles first, then the busy state (section 13.1 rule 4), then the validation of the list itself.
    for (const auto& setting : params.tracks)
        if (auto unknown = controlCheckTrackSlot(context, setting.trackId))
            return *unknown;
    if (auto busy = controlCheckBusy(context))
        return *busy;
    std::set<int> named;
    for (const auto& setting : params.tracks)
        if (!named.insert(setting.trackId).second)
            return controlOpError("INVALID_ARGUMENT", "duplicate_track", "a track is named more than once");

    auto* arrangement = context.processor().getStandaloneArrangement();
    if (arrangement == nullptr)
        return controlOpError("INTERNAL", "", "the arrangement is not available");
    ControlWriteEnvelope envelope(context);
    const int visible = shownTracks(context);
    const auto before = controlCaptureTracks(context, visible);

    // The setters of the track panel's controls (setStandaloneTrackMuted, setStandaloneTrackSolo,
    // setStandaloneTrackVolume), which the editor calls from trackMuteToggled(), trackSoloToggled() and
    // trackVolumeChanged(); the colour is the one the colour dialog sets (setStandaloneTrackColour), set directly.
    for (const auto& setting : params.tracks) {
        if (setting.muted)
            arrangement->setTrackMuted(setting.trackId, *setting.muted);
        if (setting.solo)
            arrangement->setTrackSolo(setting.trackId, *setting.solo);
        if (setting.volumeDb || setting.volumeLinear) {
            const auto volume = requestedVolume(setting);
            arrangement->setTrackVolume(setting.trackId, static_cast<float>(volume.linear));
            if (volume.clamped) {
                auto data = JsonValue::object();
                data.appendMember("field", JsonValue::string(volume.field));
                data.appendMember("requested", controlJsonNumber(volume.requested, 6));
                data.appendMember("applied", controlJsonNumber(volume.applied, 6));
                envelope.addWarning("CLAMPED", controlTrackRef(setting.trackId), std::move(data));
            }
        }
        if (setting.colourRgb)
            arrangement->setTrackColour(setting.trackId,
                                        juce::Colour(static_cast<juce::uint8>(*setting.colourRgb >> 16),
                                                     static_cast<juce::uint8>(*setting.colourRgb >> 8),
                                                     static_cast<juce::uint8>(*setting.colourRgb)));
    }
    const auto after = controlCaptureTracks(context, visible);

    // The tracks the call named whose own settings changed: what a revert sets back.
    auto revertTracks = JsonValue::array();
    bool changed = false;
    for (std::size_t i = 0; i < before.size(); ++i)
        if (trackFields(before[i], after[i]).size() != 0)
            changed = true;
    for (const auto& id : named) {
        const auto index = static_cast<std::size_t>(id);
        auto setting = revertSetting(before[index], after[index]);
        if (setting.size() > 1)
            revertTracks.append(std::move(setting));
    }
    describeTracks(before, after, envelope, named);
    if (changed) {
        // The settings are saved with the project, which the UI marks as changed after each of them.
        controlMarkDirty(context);
        if (auto* bridge = context.bridge())
            bridge->onTrackMixChanged();
        auto revert = JsonValue::object();
        revert.appendMember("tracks", std::move(revertTracks));
        envelope.setNotUndoable("ui_parity", controlRevertRecipe("track.set", std::move(revert)));
    }
    return envelope.committed(context);
}

// ---- track.add ------------------------------------------------------------------------------------

struct AddParams
{
    int count = 1;
};

bool parseAdd(const JsonValue& params, AddParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<std::int64_t> count;
    if (!reader.onlyMembers({"count"}) || !reader.optionalInteger("count", 1, 64, count)) {
        why = reader.why();
        return false;
    }
    out.count = count ? static_cast<int>(*count) : 1;
    return true;
}

Result<JsonValue> runAdd(ControlOpContext& context, const AddParams& params)
{
    if (auto busy = controlCheckBusy(context))
        return *busy;
    const auto visible = controlEditorVisibleTrackCount(context);
    if (!visible.ok())
        return visible.error();
    const int slots = controlTrackSlotCount(context);
    const int target = visible.value() + params.count;
    if (target > slots)
        return controlOpError("INVALID_ARGUMENT", "track_limit",
                              "fewer hidden track slots remain than the request needs; none was shown");

    ControlWriteEnvelope envelope(context);
    const auto before = controlCaptureTracks(context, visible.value());
    // TrackPanelComponent::showMoreTracks(), the + button, repeated count times: the track panel and the arrangement
    // show the same, larger count. A shown track keeps the clips it holds.
    auto* bridge = context.bridge();
    if (bridge == nullptr || !bridge->setVisibleTrackCount(target))
        return controlOpError("UNSUPPORTED", "capability_missing", "the editor cannot show more tracks");
    const auto after = controlCaptureTracks(context, shownTracks(context));
    describeTracks(before, after, envelope);
    // No undo entry and nothing to revert (PROTOCOL.md section 15.2 rule 6): the + button has no undo, and no request
    // restores the previous count (track.delete removes the track with its clips, one track per request, and needs the
    // end user's consent, so it is no revert recipe). The count is project state (saved with the project since upstream
    // 2c01847): the editor's panel writes it to the arrangement and marks the project when it changes
    // (visibleTrackCountChanged, reached through the bridge's setVisibleTrackCount()), as the + button does, so the
    // result's dirty flag turns true without this operation marking the project itself. Hence IRREVERSIBLE, with the
    // first track shown as its subject (vectors/track/add-ok.json).
    envelope.setNotUndoable("no_upstream_undo");
    envelope.addWarning("IRREVERSIBLE", controlTrackRef(visible.value()));
    auto result = envelope.committed(context);
    auto counts = JsonValue::object();
    counts.appendMember("before", JsonValue::integer(visible.value()));
    counts.appendMember("after", JsonValue::integer(shownTracks(context)));
    result.appendMember("visibleTrackCount", std::move(counts));
    return result;
}

// ---- track.duplicate ------------------------------------------------------------------------------

struct DuplicateParams
{
    int trackId = 0;
    bool share = false;
};

bool parseDuplicate(const JsonValue& params, DuplicateParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (!reader.onlyMembers({"trackId", "mode"}) || !readTrackId(reader, out.trackId)) {
        why = reader.why();
        return false;
    }
    if (const JsonValue* mode = reader.member("mode")) {
        if (!mode->isString() || (mode->text() != "clone" && mode->text() != "share")) {
            why = "mode must be clone or share";
            return false;
        }
        out.share = mode->text() == "share";
    }
    return true;
}

Result<JsonValue> runDuplicate(ControlOpContext& context, const DuplicateParams& params)
{
    if (auto unknown = controlCheckTrackSlot(context, params.trackId))
        return *unknown;
    if (auto busy = controlCheckBusy(context))
        return *busy;
    const auto visible = controlEditorVisibleTrackCount(context);
    if (!visible.ok())
        return visible.error();
    // OpenTune's Duplicate Track is offered on a shown track only, so a hidden source is refused with track_hidden
    // (registry: the reason covers duplication as well as deletion).
    if (params.trackId >= visible.value())
        return controlOpError("INVALID_ARGUMENT", "track_hidden", "a hidden track cannot be duplicated");
    if (visible.value() >= controlTrackSlotCount(context))
        return controlOpError("INVALID_ARGUMENT", "track_limit",
                              "every track slot is shown; there is no slot for the copy");
    // The copy goes to the first hidden slot, which a project that was opened can already fill with clips.
    const int target = visible.value();
    if (!controlPlacementsOnTrack(context, target).empty())
        return controlOpError("INVALID_ARGUMENT", "target_track_occupied",
                              "the first hidden track slot already holds clips");

    auto& processor = context.processor();
    auto* arrangement = processor.getStandaloneArrangement();
    auto* repository = processor.getStandaloneContentRepository();
    auto* bridge = context.bridge();
    if (arrangement == nullptr || repository == nullptr || bridge == nullptr)
        return controlOpError("INTERNAL", "", "the arrangement is not available");
    const auto sources = controlPlacementsOnTrack(context, params.trackId);

    ControlWriteEnvelope envelope(context);
    const auto before = controlCaptureTracks(context, visible.value());

    // Mode clone: every copied clip gets a content of its own first (OpenTuneAudioProcessor::cloneContent()), so
    // that a failure leaves the arrangement as it was; the contents made so far are retired again, as the
    // arrangement's clip duplicate does when its insert fails. Every clone makes a clip, so the clip id counter is
    // moved to where that many ids are free first, once for all of them (ClipIds.h); mode share makes none.
    if (!params.share && !sources.empty())
        controlSkipUsedClipIds(context, "track.duplicate", sources.size());
    std::vector<ContentKey> contents;
    for (const auto& source : sources) {
        if (params.share) {
            contents.push_back(source.placement.contentKey);
            continue;
        }
        const auto clone = processor.cloneContent(source.placement.contentKey);
        if (!clone.isValid()) {
            for (const auto& made : contents)
                repository->retireClip(made);
            return controlOpError("INTERNAL", "", "OpenTune could not copy the content of a clip");
        }
        controlRecordCommittedClipId(context, clone);
        contents.push_back(clone);
    }

    // trackDuplicateRequested(): the mix state of the track, then a copy of every clip with a new placement id.
    arrangement->setTrackMuted(target, arrangement->isTrackMuted(params.trackId));
    arrangement->setTrackSolo(target, arrangement->isTrackSolo(params.trackId));
    arrangement->setTrackVolume(target, arrangement->getTrackVolume(params.trackId));
    arrangement->setTrackColour(target, arrangement->getTrackColour(params.trackId));
    std::vector<std::pair<std::string, std::string>> copies;
    std::vector<std::uint64_t> copiedIds;
    std::vector<std::string> madeLabels;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        StandaloneArrangement::Placement placement = sources[i].placement;
        placement.placementId = 0;
        placement.contentKey = contents[i];
        if (!arrangement->insertPlacement(target, placement)) {
            // The copy stops here; what was inserted stays and is reported, so that the client reads the state.
            if (!params.share)
                for (std::size_t j = i; j < contents.size(); ++j)
                    repository->retireClip(contents[j]);
            bridge->setVisibleTrackCount(visible.value() + 1);
            bridge->onTrackMixChanged();
            controlMarkDirty(context);
            return controlOpErrorAfterChange("INTERNAL", "",
                                             "OpenTune could not insert a copied clip; read the project again");
        }
        copies.emplace_back(sources[i].placementId, controlPlacementId(context, placement.placementId));
        copiedIds.push_back(placement.placementId);
    }
    // The track panel and the arrangement show the new track, with its colour and mix.
    bridge->setVisibleTrackCount(visible.value() + 1);
    bridge->onTrackMixChanged();
    controlMarkDirty(context);

    // The result: the new track's members, the new clips and, with mode clone, their contents.
    const auto after = controlCaptureTracks(context, shownTracks(context));
    describeTracks(before, after, envelope);
    const auto placements = controlLivePlacements(context);
    for (const auto id : copiedIds) {
        const auto found = std::find_if(placements.begin(), placements.end(), [id](const ControlPlacementRef& ref) {
            return ref.placement.placementId == id;
        });
        if (found == placements.end())
            continue;
        envelope.addCreated("placement", placementRef(found->placementId), JsonValue::string(placementLabel(*found)));
        envelope.touchPlacement(id);
        madeLabels.push_back(placementLabel(*found));
    }
    if (!params.share)
        for (std::size_t i = 0; i < contents.size(); ++i) {
            const auto contentId = controlContentId(context, contents[i]);
            envelope.addCreated(
                "content", contentRef(contentId),
                JsonValue::string("Content of " + (i < madeLabels.size() ? madeLabels[i] : std::string("a clip"))));
            envelope.touchContent(contentId);
        }
    if (!copies.empty()) {
        auto from = JsonValue::array();
        auto to = JsonValue::array();
        for (const auto& [source, copy] : copies) {
            from.append(placementRef(source));
            to.append(placementRef(copy));
        }
        auto entry = JsonValue::object();
        entry.appendMember("entity", JsonValue::string("placement"));
        entry.appendMember("relation", JsonValue::string("copy"));
        entry.appendMember("from", std::move(from));
        entry.appendMember("to", std::move(to));
        envelope.addIdMapEntry(std::move(entry));
        if (params.share)
            envelope.addWarning("SHARED_CONTENT", controlTrackRef(target));
    }
    // No undo entry, as in the UI; deleting the new track takes the copy back, which needs the end user's consent.
    auto revertParams = JsonValue::object();
    revertParams.appendMember("trackId", JsonValue::integer(target));
    revertParams.appendMember("expectedArrangementRevision", JsonValue::string(controlArrangementRevision(context)));
    envelope.setNotUndoable("ui_parity", controlRevertRecipe("track.delete", std::move(revertParams), true));
    auto result = envelope.committed(context);
    result.appendMember("newTrackId", JsonValue::integer(target));
    return result;
}

// ---- track.delete ---------------------------------------------------------------------------------

struct DeleteParams
{
    int trackId = 0;
    std::string expectedArrangementRevision;
    bool confirm = false;
};

bool parseDelete(const JsonValue& params, DeleteParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<std::string> expected;
    std::optional<bool> confirm;
    if (!reader.onlyMembers({"trackId", "expectedArrangementRevision", "confirm"}) || !readTrackId(reader, out.trackId)
        || !controlReadRevisionToken(reader, "expectedArrangementRevision", expected) || !expected
        || !readBoolean(reader, "confirm", confirm)) {
        why = reader.why().empty() ? "expectedArrangementRevision is required" : reader.why();
        return false;
    }
    out.expectedArrangementRevision = *expected;
    out.confirm = confirm.value_or(false);
    return true;
}

Result<JsonValue> runDelete(ControlOpContext& context, const DeleteParams& params)
{
    if (auto unknown = controlCheckTrackSlot(context, params.trackId))
        return *unknown;
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (controlArrangementRevision(context) != params.expectedArrangementRevision)
        return controlOpError("REVISION_CONFLICT", "arrangement_changed",
                              "the arrangement differs from expectedArrangementRevision; read it again");
    const auto visibleResult = controlEditorVisibleTrackCount(context);
    if (!visibleResult.ok())
        return visibleResult.error();
    const int visible = visibleResult.value();
    // OpenTune deletes only a shown track, and never the last one (trackDeleteRequested() returns without a word).
    if (params.trackId >= visible)
        return controlOpError("INVALID_ARGUMENT", "track_hidden", "a hidden track cannot be deleted");
    if (visible <= 1)
        return controlOpError("INVALID_ARGUMENT", "last_track", "the only shown track cannot be deleted");
    if (!params.confirm)
        return controlOpError("INVALID_ARGUMENT", "confirm_required",
                              "deleting a track cannot be undone; the end user must agree, and confirm must be true");

    auto& processor = context.processor();
    auto* arrangement = processor.getStandaloneArrangement();
    auto* bridge = context.bridge();
    const auto history = context.history();
    if (arrangement == nullptr || bridge == nullptr || history == nullptr)
        return controlOpError("INTERNAL", "", "the arrangement is not available");

    ControlWriteEnvelope envelope(context);
    const auto clipsBefore = controlLivePlacements(context);
    std::vector<ControlPlacementRef> removed;
    for (const auto& ref : clipsBefore)
        if (ref.trackId == params.trackId)
            removed.push_back(ref);

    // trackDeleteRequested(): the arrangement removes the track and moves only the later shown tracks down by one
    // position (their clips and mix); a hidden slot keeps its position, its clips and its mix. Then the panel shows
    // the mix of the arrangement for the tracks that remain and one track fewer.
    if (!arrangement->removeTrackAndShift(params.trackId, visible))
        return controlOpError("INTERNAL", "", "OpenTune did not delete the track");
    bridge->onTrackMixChanged();
    bridge->setVisibleTrackCount(visible - 1);
    controlMarkDirty(context);

    const auto clipsAfter = controlLivePlacements(context);

    // The contents that no clip shows any more left the project (OpenTune leaks their clips, see TrackOps.h).
    std::set<std::uint64_t> stillShown;
    for (const auto& ref : clipsAfter)
        stillShown.insert(ref.placement.contentKey.objectId);
    std::vector<ContentKey> leftProject;
    for (const auto& ref : removed)
        if (stillShown.count(ref.placement.contentKey.objectId) == 0
            && std::find(leftProject.begin(), leftProject.end(), ref.placement.contentKey) == leftProject.end())
            leftProject.push_back(ref.placement.contentKey);
    const auto epoch = controlContentEpoch(context);
    history->markContentsRemoved(leftProject, epoch);

    // The barrier in the undo and redo histories (section 15.3 rule 6): an entry is possibly stale when it addresses
    // the deleted track or a shown track after it, which removeTrackAndShift() moved down. A hidden track keeps its
    // position and its clips, so an entry that addresses only hidden tracks stays valid. visible is the number of
    // shown tracks before the deletion.
    const auto stale = history->onTrackDeleted(processor.getUndoManager(), params.trackId, visible, epoch);

    // Removed: the clips, then the contents that left with them (section 14.4 rule 6).
    for (const auto& ref : removed)
        envelope.addRemoved("placement", placementRef(ref.placementId), JsonValue::string(placementLabel(ref)));
    for (const auto& key : leftProject) {
        const auto found = std::find_if(removed.begin(), removed.end(), [&key](const ControlPlacementRef& ref) {
            return ref.placement.contentKey == key;
        });
        envelope.addRemoved("content", contentRef(controlContentId(context, key)),
                            JsonValue::string("Content of " + placementLabel(*found)));
    }
    // Changed: every clip that moved to another track.
    for (const auto& now : clipsAfter) {
        const auto was = std::find_if(clipsBefore.begin(), clipsBefore.end(), [&now](const ControlPlacementRef& ref) {
            return ref.placement.placementId == now.placement.placementId;
        });
        if (was == clipsBefore.end())
            continue;
        auto fields = JsonValue::object();
        addField(fields, "trackId", JsonValue::integer(was->trackId), JsonValue::integer(now.trackId), nullptr);
        if (fields.size() == 0)
            continue;
        envelope.addChange("placement", placementRef(now.placementId), JsonValue::string(placementLabel(now)),
                           std::move(fields));
        envelope.touchPlacement(now.placement.placementId);
    }

    // trackIdMap: the deleted track, and every later shown track. removeTrackAndShift() moves only the shown tracks
    // after the deleted one down; the hidden slots keep their position, their clips and their mix.
    auto trackIdMap = JsonValue::array();
    const auto mapEntry = [](int from, JsonValue to) {
        auto entry = JsonValue::object();
        entry.appendMember("before", JsonValue::integer(from));
        entry.appendMember("after", std::move(to));
        return entry;
    };
    trackIdMap.append(mapEntry(params.trackId, JsonValue()));
    for (int trackId = params.trackId + 1; trackId < visible; ++trackId)
        trackIdMap.append(mapEntry(trackId, JsonValue::integer(trackId - 1)));

    // No undo and no revert: OpenTune has no way to bring the track back.
    envelope.addWarning("IRREVERSIBLE", controlTrackRef(params.trackId));
    auto staleData = JsonValue::object();
    staleData.appendMember("undo", JsonValue::integer(stale.undo));
    staleData.appendMember("redo", JsonValue::integer(stale.redo));
    if (stale.undo + stale.redo > 0)
        envelope.addWarning("UNDO_HISTORY_POSSIBLY_STALE", JsonValue(), staleData);
    envelope.setNotUndoable("no_upstream_undo");
    auto result = envelope.committed(context);
    result.appendMember("trackIdMap", std::move(trackIdMap));
    result.appendMember("possiblyStaleEntries", std::move(staleData));
    return result;
}

} // namespace

void registerTrackOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<SetParams>("track.set", parseSet, runSet));
    table.add(controlWriteMethod<AddParams>("track.add", parseAdd, runAdd));
    table.add(controlWriteMethod<DuplicateParams>("track.duplicate", parseDuplicate, runDuplicate));
    table.add(controlWriteMethod<DeleteParams>("track.delete", parseDelete, runDelete));
}

} // namespace OpenTune
