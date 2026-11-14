// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ArrangementOps.h"

#include "EditSupport.h"
#include "OpsSupport.h"
#include "TrackSupport.h"
#include "WriteEnvelope.h"

#include "Content/EditableContentSnapshot.h"
#include "Control/ControlUiBridge.h"
#include "Control/Edit/ControlOriginAction.h"
#include "PluginProcessor.h"
#include "Utils/PlacementActions.h"
#include "Utils/TimeCoordinate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace OpenTune {

namespace {

/** The longest list of ops, of clips in one op delete and of sources in one arrangement.copy that the schemas allow. */
constexpr std::size_t kMaxItems = 64;
/** Timeline positions this close are one: one sample at OpenTune's render rate, the tolerance of
    OpenTuneAudioProcessor::mergePlacements() and of the arrangement's timeline setters. */
constexpr double kTimelineEpsilon = 1.0 / TimeCoordinate::kRenderSampleRate;
// ---- Shared pieces ------------------------------------------------------------------------------

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

template <typename Placements>
auto findIn(Placements& placements, std::uint64_t placementId) -> decltype(&placements.front())
{
    for (auto& ref : placements)
        if (ref.placement.placementId == placementId)
            return &ref;
    return nullptr;
}

/** Records one change of entity placement for every clip that is in both lists and differs,
    in the order of after, and touches it so that the result reports its geometryHash. */
void describeChanges(ControlWriteEnvelope& envelope, const std::vector<ControlPlacementRef>& before,
                     const std::vector<ControlPlacementRef>& after)
{
    for (const auto& now : after) {
        const auto* was = findIn(before, now.placement.placementId);
        if (was == nullptr)
            continue;
        auto fields = controlPlacementChangeFields(*was, now);
        if (fields.size() == 0)
            continue;
        envelope.addChange("placement", placementRef(now.placementId), JsonValue::string(placementLabel(now)),
                           std::move(fields));
        envelope.touchPlacement(now.placement.placementId);
    }
}

/** Records a new clip of the arrangement in created and touches it; its label once it is found. */
std::string addCreatedClip(ControlWriteEnvelope& envelope, const std::vector<ControlPlacementRef>& placements,
                           std::uint64_t placementId)
{
    const auto* ref = findIn(placements, placementId);
    if (ref == nullptr)
        return {};
    const auto label = placementLabel(*ref);
    envelope.addCreated("placement", placementRef(ref->placementId), JsonValue::string(label));
    envelope.touchPlacement(placementId);
    return label;
}

void addIdMapEntry(ControlWriteEnvelope& envelope, const char* relation, const std::vector<std::string>& from,
                   const std::vector<std::string>& to)
{
    auto fromRefs = JsonValue::array();
    for (const auto& id : from)
        fromRefs.append(placementRef(id));
    auto toRefs = JsonValue::array();
    for (const auto& id : to)
        toRefs.append(placementRef(id));
    auto entry = JsonValue::object();
    entry.appendMember("entity", JsonValue::string("placement"));
    entry.appendMember("relation", JsonValue::string(relation));
    entry.appendMember("from", std::move(fromRefs));
    entry.appendMember("to", std::move(toRefs));
    envelope.addIdMapEntry(std::move(entry));
}

bool overlapsOnTrack(const ControlPlacementRef& a, const ControlPlacementRef& b)
{
    return a.trackId == b.trackId && a.placement.placementId != b.placement.placementId
           && std::min(a.placement.timelineEndSeconds(), b.placement.timelineEndSeconds())
                      - std::max(a.placement.timelineStartSeconds, b.placement.timelineStartSeconds)
                  > kTimelineEpsilon;
}

/** OVERLAPS_CLIP for every clip of ids that overlaps another clip on its track in placements; data.overlaps lists the
    clips it overlaps (placementIds). */
void warnOverlaps(ControlWriteEnvelope& envelope, const std::vector<ControlPlacementRef>& placements,
                  const std::vector<std::uint64_t>& ids)
{
    for (const auto id : ids) {
        const auto* ref = findIn(placements, id);
        if (ref == nullptr)
            continue;
        auto overlapped = JsonValue::array();
        for (const auto& other : placements)
            if (overlapsOnTrack(*ref, other))
                overlapped.append(JsonValue::string(other.placementId));
        if (overlapped.size() == 0)
            continue;
        auto data = JsonValue::object();
        data.appendMember("overlaps", std::move(overlapped));
        envelope.addWarning("OVERLAPS_CLIP", placementRef(ref->placementId), std::move(data));
    }
}

/**
 * A clip that goes to a hidden track shows the hidden tracks up to it, as the editor does when an import fills hidden
 * tracks (ControlUiBridge::revealTracks()). Records sideEffects.tracksRevealed (view state, revert null) and the
 * warning TRACK_AUTO_REVEALED for every track it shows; with predictOnly the bridge is not asked. Without an editor, or
 * an editor that cannot tell how many tracks it shows, nothing is shown.
 */
void revealTracks(ControlOpContext& context, ControlWriteEnvelope& envelope, const std::vector<int>& trackIds,
                  bool predictOnly)
{
    auto* bridge = context.bridge();
    if (bridge == nullptr || trackIds.empty())
        return;
    const auto visible = bridge->visibleTrackCount();
    const int highest = *std::max_element(trackIds.begin(), trackIds.end());
    if (!visible || highest < *visible)
        return;
    if (!predictOnly && !bridge->revealTracks(highest + 1))
        return;
    auto ids = JsonValue::array();
    for (int trackId = *visible; trackId <= highest; ++trackId) {
        envelope.addWarning("TRACK_AUTO_REVEALED", controlTrackRef(trackId));
        ids.append(JsonValue::integer(trackId));
    }
    auto effect = JsonValue::object();
    effect.appendMember("trackIds", std::move(ids));
    effect.appendMember("revert", JsonValue());
    envelope.addSideEffect("tracksRevealed", std::move(effect));
}

/** One of OpenTune's clip undo actions in a ControlOriginAction. Its content key stays invalid: a clip action puts
    back no aspect of a content, so no change of a content masks it. */
ControlOriginAction::Inner innerOf(std::unique_ptr<UndoAction> action, std::string name, std::vector<int> tracks)
{
    ControlOriginAction::Inner inner;
    inner.action = std::move(action);
    inner.name = std::move(name);
    inner.tracks = std::move(tracks);
    return inner;
}

struct PushedEntry
{
    std::string undoId;
    std::string description;
    std::vector<std::string> names;
};

/** Pushes the actions as one entry of OpenTune's undo history (UndoManager::addAction(), as the arrangement view
    pushes its actions), wrapped with a new undoId. The entry takes the given description, or that of its first
    action. */
PushedEntry pushEntry(ControlOpContext& context, const std::shared_ptr<ControlHistory>& history,
                      std::vector<ControlOriginAction::Inner> inner, std::optional<juce::String> name = std::nullopt)
{
    PushedEntry pushed;
    const auto description = name.value_or(inner.front().action->getDescription());
    pushed.undoId = history->nextUndoId(context.instanceShort());
    pushed.description = description.toStdString();
    auto action = std::make_unique<ControlOriginAction>(history, pushed.undoId, description, std::move(inner));
    pushed.names = action->innerActionNames();
    history->record(context.processor().getUndoManager(), std::move(action));
    return pushed;
}

/** After a change of the arrangement: the project is marked as changed and the editor redraws the arrangement. */
void afterChange(ControlOpContext& context)
{
    controlMarkDirty(context);
    if (auto* bridge = context.bridge())
        bridge->onStructureChanged();
}

/** A revert recipe using one arrangement.edit op at the current arrangement revision. null
    when the instance does not report arrangement.v1. */
JsonValue editRecipe(ControlOpContext& context, JsonValue op, bool requiresConsent)
{
    if (!controlReportsCapability(context, "arrangement.v1"))
        return JsonValue();
    auto ops = JsonValue::array();
    ops.append(std::move(op));
    auto params = JsonValue::object();
    params.appendMember("expectedArrangementRevision", JsonValue::string(controlArrangementRevision(context)));
    params.appendMember("ops", std::move(ops));
    return controlRevertRecipe("arrangement.edit", std::move(params), requiresConsent);
}

Error revisionConflict()
{
    return controlOpError("REVISION_CONFLICT", "arrangement_changed",
                          "the arrangement differs from expectedArrangementRevision; read it again");
}

// ---- Params -------------------------------------------------------------------------------------------

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

/** A track position: an integer of 0 or more. A position beyond the slots is a
    NOT_FOUND of the run; the bound keeps the conversion defined. */
bool readTrackId(ControlParamReader& reader, const char* name, std::optional<int>& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    if (!value->isNumber() || std::floor(value->asDouble()) != value->asDouble() || value->asDouble() < 0.0)
        return reader.fail(std::string(name) + " must be a non-negative integer");
    out = static_cast<int>(std::min(value->asDouble(), 1.0e6));
    return true;
}

bool readSeconds(ControlParamReader& reader, const char* name, bool required, std::optional<double>& out)
{
    if (!reader.optionalNumber(name, 0.0, std::numeric_limits<double>::max(), out))
        return false;
    if (required && !out)
        return reader.fail(std::string(name) + " is required");
    return true;
}

bool readPlacement(ControlParamReader& reader, const char* name, ControlHandle& out)
{
    std::optional<ControlHandle> handle;
    if (!reader.optionalHandle(name, ControlHandleKind::Placement, handle))
        return false;
    if (!handle)
        return reader.fail(std::string(name) + " is required");
    out = *handle;
    return true;
}

enum class OpKind {
    Move,
    Trim,
    SetGain,
    SetFade,
    Split,
    Merge,
    Delete
};

/** One op of arrangement.edit. */
struct EditOp
{
    OpKind kind = OpKind::Move;
    /** The clip of every op but merge (its leading clip) and delete. */
    ControlHandle placement;
    /** merge: the trailing clip. */
    ControlHandle trailing;
    /** delete: the clips. */
    std::vector<ControlHandle> placements;
    std::optional<int> toTrackId;
    std::optional<double> startTimelineSec;
    /** trim: the start edge (else the end edge). */
    bool trimStart = false;
    /** trim: toTimelineSec; split: atTimelineSec. */
    double timelineSec = 0.0;
    std::optional<double> gainDb;
    std::optional<double> gainLinear;
    std::optional<double> fadeInSec;
    std::optional<double> fadeOutSec;
};

struct EditParams
{
    std::string expectedArrangementRevision;
    std::vector<EditOp> ops;
    bool dryRun = false;
    bool confirm = false;
};

bool parseDeleteIds(ControlParamReader& reader, const JsonValue& item, EditOp& out)
{
    const JsonValue* ids = item.find("placementIds");
    if (ids == nullptr || !ids->isArray() || ids->size() == 0 || ids->size() > kMaxItems)
        return reader.fail("placementIds must hold 1 to 64 placementIds");
    std::set<std::string> seen;
    for (std::size_t i = 0; i < ids->size(); ++i) {
        const auto& id = ids->at(i);
        const auto handle = id.isString() ? controlParseHandle(ControlHandleKind::Placement, id.text()) : std::nullopt;
        if (!handle)
            return reader.fail("placementIds must hold placementIds");
        if (!seen.insert(id.text()).second)
            return reader.fail("placementIds must not repeat a placementId");
        out.placements.push_back(*handle);
    }
    return true;
}

bool parseOp(const JsonValue& item, EditOp& out, std::string& why)
{
    const JsonValue* op = item.isObject() ? item.find("op") : nullptr;
    if (op == nullptr || !op->isString()) {
        why = "every op must be an object with op";
        return false;
    }
    ControlParamReader reader(item);
    const auto& name = op->text();
    bool ok = false;
    if (name == "move") {
        out.kind = OpKind::Move;
        ok = reader.onlyMembers({"op", "placementId", "toTrackId", "startTimelineSec"})
             && readPlacement(reader, "placementId", out.placement) && readTrackId(reader, "toTrackId", out.toTrackId)
             && readSeconds(reader, "startTimelineSec", false, out.startTimelineSec);
        if (ok && !out.toTrackId && !out.startTimelineSec)
            ok = reader.fail("op move needs toTrackId or startTimelineSec");
    } else if (name == "trim") {
        out.kind = OpKind::Trim;
        std::optional<double> to;
        const JsonValue* edge = item.find("edge");
        ok = reader.onlyMembers({"op", "placementId", "edge", "toTimelineSec"})
             && readPlacement(reader, "placementId", out.placement) && readSeconds(reader, "toTimelineSec", true, to);
        if (ok && (edge == nullptr || !edge->isString() || (edge->text() != "start" && edge->text() != "end")))
            ok = reader.fail("edge must be start or end");
        if (ok) {
            out.trimStart = edge->text() == "start";
            out.timelineSec = *to;
        }
    } else if (name == "setGain") {
        out.kind = OpKind::SetGain;
        ok = reader.onlyMembers({"op", "placementId", "gainDb", "gainLinear"})
             && readPlacement(reader, "placementId", out.placement)
             && reader.optionalNumber("gainDb", -std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                      out.gainDb)
             && reader.optionalNumber("gainLinear", 0.0, std::numeric_limits<double>::max(), out.gainLinear);
        if (ok && out.gainDb.has_value() == out.gainLinear.has_value())
            ok = reader.fail("op setGain needs exactly one of gainDb and gainLinear");
    } else if (name == "setFade") {
        out.kind = OpKind::SetFade;
        ok = reader.onlyMembers({"op", "placementId", "fadeInSec", "fadeOutSec"})
             && readPlacement(reader, "placementId", out.placement)
             && readSeconds(reader, "fadeInSec", false, out.fadeInSec)
             && readSeconds(reader, "fadeOutSec", false, out.fadeOutSec);
        if (ok && !out.fadeInSec && !out.fadeOutSec)
            ok = reader.fail("op setFade needs fadeInSec or fadeOutSec");
    } else if (name == "split") {
        out.kind = OpKind::Split;
        std::optional<double> at;
        ok = reader.onlyMembers({"op", "placementId", "atTimelineSec"})
             && readPlacement(reader, "placementId", out.placement) && readSeconds(reader, "atTimelineSec", true, at);
        if (ok)
            out.timelineSec = *at;
    } else if (name == "merge") {
        out.kind = OpKind::Merge;
        ok = reader.onlyMembers({"op", "leadingPlacementId", "trailingPlacementId"})
             && readPlacement(reader, "leadingPlacementId", out.placement)
             && readPlacement(reader, "trailingPlacementId", out.trailing);
    } else if (name == "delete") {
        out.kind = OpKind::Delete;
        ok = reader.onlyMembers({"op", "placementIds"}) && parseDeleteIds(reader, item, out);
    } else {
        why = "op must be move, trim, setGain, setFade, split, merge or delete";
        return false;
    }
    if (!ok)
        why = reader.why();
    return ok;
}

bool parseEdit(const JsonValue& params, EditParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<std::string> expected;
    std::optional<bool> dryRun;
    std::optional<bool> confirm;
    if (!reader.onlyMembers({"expectedArrangementRevision", "ops", "dryRun", "confirm"})
        || !controlReadRevisionToken(reader, "expectedArrangementRevision", expected)
        || !readBoolean(reader, "dryRun", dryRun) || !readBoolean(reader, "confirm", confirm)) {
        why = reader.why();
        return false;
    }
    if (!expected) {
        why = "expectedArrangementRevision is required";
        return false;
    }
    const JsonValue* ops = reader.member("ops");
    if (ops == nullptr || !ops->isArray() || ops->size() == 0 || ops->size() > kMaxItems) {
        why = "ops must hold 1 to 64 ops";
        return false;
    }
    for (std::size_t i = 0; i < ops->size(); ++i) {
        EditOp op;
        if (!parseOp(ops->at(i), op, why))
            return false;
        out.ops.push_back(std::move(op));
    }
    const auto has = [&out](OpKind kind) {
        return std::any_of(out.ops.begin(), out.ops.end(), [kind](const EditOp& op) { return op.kind == kind; });
    };
    if ((has(OpKind::Split) || has(OpKind::Merge) || has(OpKind::Delete)) && out.ops.size() > 1) {
        why = "a call with op split, merge or delete has no other op";
        return false;
    }
    if (confirm && !has(OpKind::Delete)) {
        why = "only a call with op delete accepts confirm";
        return false;
    }
    out.expectedArrangementRevision = *expected;
    out.dryRun = dryRun.value_or(false);
    out.confirm = confirm.value_or(false);
    return true;
}

// ---- arrangement.edit: the checks ------------------------------------------------------------------

/** An op with the clips its handles name, resolved when the call starts. */
struct ResolvedOp
{
    const EditOp* op = nullptr;
    /** The clip of the op (merge: the leading clip). */
    ControlPlacementRef clip;
    /** merge: the trailing clip. */
    std::optional<ControlPlacementRef> other;
    /** delete: the clips. */
    std::vector<ControlPlacementRef> clips;
};

JsonValue fadeChange(double before, double after)
{
    if (before == after)
        return JsonValue();
    auto change = JsonValue::object();
    change.appendMember("before", controlJsonSeconds(before));
    change.appendMember("after", controlJsonSeconds(after));
    return change;
}

void warnClamped(ControlWriteEnvelope& envelope, const ControlPlacementRef& clip, const char* field,
                 JsonValue requested, JsonValue applied)
{
    auto data = JsonValue::object();
    data.appendMember("field", JsonValue::string(field));
    data.appendMember("requested", std::move(requested));
    data.appendMember("applied", std::move(applied));
    envelope.addWarning("CLAMPED", placementRef(clip.placementId), std::move(data));
}

Error splitIssueError(ControlOpContext& context, const ControlPlacementRef& clip,
                      const std::shared_ptr<const EditableContentSnapshot>& snapshot, PlacementSplitIssue issue)
{
    switch (issue) {
        case PlacementSplitIssue::OutOfRange:
            return controlOpError("INVALID_ARGUMENT", "split_out_of_range",
                                  "the split point must leave both parts longer than 0.1 s");
        case PlacementSplitIssue::ContentUnavailable:
            return controlOpError("INTERNAL", "", "the content of the clip is not available");
        case PlacementSplitIssue::TimeGridEdited:
            return controlOpError("UNSUPPORTED", "upstream_absent",
                                  "OpenTune does not split a clip whose time grid was edited");
        case PlacementSplitIssue::F0NotReady: {
            const ControlContentRef content{clip.placement.contentKey, controlContentId(context, clip.placement.contentKey),
                                            snapshot};
            if (auto notReady = controlCheckContentReady(context, content))
                return *notReady;
            return controlOpError("UNSUPPORTED", "upstream_absent", "the clip has no usable pitch analysis");
        }
        case PlacementSplitIssue::InvalidSampleRate:
            return controlOpError("UNSUPPORTED", "upstream_absent", "the audio of the clip has no sample rate");
        case PlacementSplitIssue::BoundaryOutsideAudio:
            return controlOpError("INVALID_ARGUMENT", "split_out_of_range",
                                  "the nearest analysis frame lies outside the audio of the clip");
        case PlacementSplitIssue::None:
            break;
    }
    return controlOpError("INTERNAL", "", "the split plan is invalid");
}

std::optional<Error> mergeIssueError(PlacementMergeIssue issue)
{
    switch (issue) {
        case PlacementMergeIssue::SamePlacement:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible", "the leading and trailing clips are the same clip");
        case PlacementMergeIssue::DifferentTracks:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible", "the clips are on different tracks");
        case PlacementMergeIssue::NotAdjacent:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible",
                                  "the trailing clip does not start where the leading clip ends");
        case PlacementMergeIssue::MetadataMismatch:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible", "the clips differ in gain or in name");
        case PlacementMergeIssue::DifferentSource:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible", "the clips do not show parts of one source");
        case PlacementMergeIssue::SampleRateMismatch:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible",
                                  "the audio of the clips has different sample rates");
        case PlacementMergeIssue::SourceBoundaryMismatch:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible",
                                  "the clips do not show adjacent parts of their source");
        case PlacementMergeIssue::KeyMismatch:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible", "the clips differ in their detected key");
        case PlacementMergeIssue::F0NotReady:
            return controlOpError("NOT_READY", "f0_not_ready", "the pitch analysis of a clip is running");
        case PlacementMergeIssue::TimeGridUnsupported:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible",
                                  "clips with edited time grids cannot be merged");
        case PlacementMergeIssue::AudibleWindowMismatch:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible",
                                  "the clips' audible source windows or seam fades do not join");
        case PlacementMergeIssue::PitchCurveMismatch:
            return controlOpError("INVALID_ARGUMENT", "merge_not_possible",
                                  "the clips' pitch analyses cannot be merged without data loss");
        case PlacementMergeIssue::None:
            return std::nullopt;
    }
    return controlOpError("INTERNAL", "", "the merge plan is invalid");
}

// ---- arrangement.edit: split, merge, delete ------------------------------------------------------------

Result<JsonValue> runSplit(ControlOpContext& context, const EditParams& params, const ResolvedOp& item)
{
    const auto& clip = item.clip;
    const double at = item.op->timelineSec;
    const auto snapshot = controlLiveSnapshot(context, clip.placement.contentKey);
    if (snapshot == nullptr)
        return controlOpError("INTERNAL", "", "the content of the clip is not available");
    const auto plan = planPlacementSplit(clip.placement, *snapshot, at);
    if (plan.issue != PlacementSplitIssue::None)
        return splitIssueError(context, clip, snapshot, plan.issue);
    auto& processor = context.processor();
    const auto history = context.history();
    if (history == nullptr)
        return controlOpError("INTERNAL", "", "the undo history of the control API is not available");

    ControlWriteEnvelope envelope(context);
    const auto label = placementLabel(clip);
    const auto addWarnings = [&] {
        if (std::abs(plan.timelineSeconds - at) > 1.0e-9) {
            auto data = JsonValue::object();
            data.appendMember("requestedTimelineSec", controlJsonSeconds(at));
            data.appendMember("appliedTimelineSec", controlJsonSeconds(plan.timelineSeconds));
            envelope.addWarning("SPLIT_SNAPPED", placementRef(clip.placementId), std::move(data));
        }
    };
    if (params.dryRun) {
        // The two new clips get their placementIds when they are made; the prediction names the clip they replace.
        envelope.addRemoved("placement", placementRef(clip.placementId), JsonValue::string(label));
        addWarnings();
        return envelope.predicted(context);
    }

    auto committed = commitPlacementSplit(processor, clip.trackId, clip.placement.placementId, plan);
    if (!committed)
        return controlOpError("UNSUPPORTED", "upstream_absent", "OpenTune did not split the clip at this position");
    std::vector<ControlOriginAction::Inner> inner;
    inner.push_back(innerOf(std::move(committed->undoAction), "SplitPlacementAction", {clip.trackId}));
    const auto pushed = pushEntry(context, history, std::move(inner));
    afterChange(context);

    const auto after = controlLivePlacements(context);
    envelope.addRemoved("placement", placementRef(clip.placementId), JsonValue::string(label));
    addCreatedClip(envelope, after, committed->outcome.leadingPlacementId);
    addCreatedClip(envelope, after, committed->outcome.trailingPlacementId);
    const auto leading = controlPlacementId(context, committed->outcome.leadingPlacementId);
    const auto trailing = controlPlacementId(context, committed->outcome.trailingPlacementId);
    addIdMapEntry(envelope, "split", {clip.placementId}, {leading, trailing});
    addWarnings();
    envelope.setUndoable(pushed.undoId, pushed.description, pushed.names);
    return envelope.committed(context);
}

Result<JsonValue> runMerge(ControlOpContext& context, const EditParams& params, const ResolvedOp& item)
{
    const auto& leading = item.clip;
    const auto& trailing = *item.other;
    const auto first = controlLiveSnapshot(context, leading.placement.contentKey);
    const auto second = controlLiveSnapshot(context, trailing.placement.contentKey);
    if (first == nullptr || second == nullptr)
        return controlOpError("INVALID_ARGUMENT", "merge_not_possible", "the clips do not show parts of one source");
    const auto plan = planPlacementMerge(leading.trackId, leading.placement, *first,
                                         trailing.trackId, trailing.placement, *second);
    if (auto refused = mergeIssueError(plan.issue))
        return *refused;
    auto& processor = context.processor();
    const auto history = context.history();
    if (history == nullptr)
        return controlOpError("INTERNAL", "", "the undo history is not available");

    ControlWriteEnvelope envelope(context);
    const auto addRemoved = [&] {
        envelope.addRemoved("placement", placementRef(leading.placementId), JsonValue::string(placementLabel(leading)));
        envelope.addRemoved("placement", placementRef(trailing.placementId),
                            JsonValue::string(placementLabel(trailing)));
    };
    if (params.dryRun) {
        addRemoved();
        return envelope.predicted(context);
    }

    auto committed = commitPlacementMerge(processor, plan);
    if (!committed)
        return controlOpError("INVALID_ARGUMENT", "merge_not_possible",
                              "OpenTune did not merge the clips: their keys or their pitch analyses do not join");
    std::vector<ControlOriginAction::Inner> inner;
    inner.push_back(innerOf(std::move(committed->undoAction), "MergePlacementAction", {leading.trackId}));
    const auto pushed = pushEntry(context, history, std::move(inner));
    afterChange(context);

    const auto after = controlLivePlacements(context);
    addRemoved();
    addCreatedClip(envelope, after, committed->outcome.mergedPlacementId);
    const auto merged = controlPlacementId(context, committed->outcome.mergedPlacementId);
    addIdMapEntry(envelope, "merge", {leading.placementId, trailing.placementId}, {merged});
    envelope.setUndoable(pushed.undoId, pushed.description, pushed.names);
    return envelope.committed(context);
}

Result<JsonValue> runDelete(ControlOpContext& context, const EditParams& params, const ResolvedOp& item)
{
    if (!params.dryRun && !params.confirm)
        return controlOpError("INVALID_ARGUMENT", "confirm_required",
                              "deleting clips requires explicit confirmation, and confirm must be true");
    auto& processor = context.processor();
    const auto history = context.history();
    if (history == nullptr)
        return controlOpError("INTERNAL", "", "the undo history of the control API is not available");

    ControlWriteEnvelope envelope(context);
    PushedEntry pushed;
    PlacementDeleteCommit deletion;
    if (!params.dryRun) {
        std::vector<PlacementKey> keys;
        keys.reserve(item.clips.size());
        for (const auto& clip : item.clips)
            keys.push_back({clip.trackId, clip.placement.placementId});
        deletion = commitPlacementDelete(processor, keys);

        if (deletion.undoAction != nullptr) {
            std::vector<int> tracks;
            for (const auto& outcome : deletion.outcomes)
                if (std::find(tracks.begin(), tracks.end(), outcome.trackId) == tracks.end())
                    tracks.push_back(outcome.trackId);
            std::vector<ControlOriginAction::Inner> inner;
            inner.push_back(innerOf(std::move(deletion.undoAction), "DeletePlacementAction", std::move(tracks)));
            pushed = pushEntry(context, history, std::move(inner));
            afterChange(context);
        }
        if (pushed.undoId.empty() || item.clips.size() != deletion.outcomes.size()) {
            if (pushed.undoId.empty())
                return controlOpError("INTERNAL", "", "OpenTune did not delete the clip");
            return controlOpErrorAfterChange("INTERNAL", "",
                                             "OpenTune did not delete every clip; read the project again");
        }
    }

    // Removed: the clips, then the contents that left with them. deletePlacement()
    // retires the content of a deleted clip even when another clip shows it.
    std::vector<ContentKey> contents;
    for (const auto& clip : item.clips) {
        envelope.addRemoved("placement", placementRef(clip.placementId), JsonValue::string(placementLabel(clip)));
        if (std::find(contents.begin(), contents.end(), clip.placement.contentKey) == contents.end())
            contents.push_back(clip.placement.contentKey);
    }
    for (const auto& key : contents) {
        if (!params.dryRun && controlLiveSnapshot(context, key) != nullptr)
            continue;
        const auto clip = std::find_if(item.clips.begin(), item.clips.end(), [&key](const ControlPlacementRef& ref) {
            return ref.placement.contentKey == key;
        });
        envelope.addRemoved("content", contentRef(controlContentId(context, key)),
                            JsonValue::string("Content of " + placementLabel(*clip)));
    }
    if (params.dryRun)
        return envelope.predicted(context);
    envelope.setUndoable(pushed.undoId, pushed.description, pushed.names);
    return envelope.committed(context);
}

// ---- arrangement.edit: the ops that change clips in place ----------------------------------------------

Result<JsonValue> runInPlace(ControlOpContext& context, const EditParams& params, const std::vector<ResolvedOp>& ops)
{
    auto& processor = context.processor();
    auto* arrangement = processor.getStandaloneArrangement();
    const auto history = context.history();
    if (arrangement == nullptr || (history == nullptr && !params.dryRun))
        return controlOpError("INTERNAL", "", "the arrangement or the undo history is not available");

    ControlWriteEnvelope envelope(context);
    const auto before = controlLivePlacements(context);
    // Every op is planned on a model of the arrangement first, in order, so that a call that fails changes nothing.
    auto model = before;
    std::vector<PlacementEditPlan> plan;
    std::vector<std::uint64_t> reshaped;
    // The tracks that clips move to: all of the plan for a prediction, only the steps OpenTune applied for a commit.
    std::vector<int> plannedReceiving;
    std::vector<int> receivingTracks;
    for (const auto& item : ops) {
        const auto& op = *item.op;
        auto* clip = findIn(model, item.clip.placement.placementId);
        if (clip == nullptr)
            return controlOpError("INTERNAL", "", "a clip of the call is not in the arrangement");
        PlacementEditPlan step;
        step.trackBefore = clip->trackId;
        step.before = clip->placement;
        step.trackAfter = clip->trackId;
        step.after = clip->placement;
        auto& p = step.after;
        switch (op.kind) {
            case OpKind::Move: {
                const int targetTrack = op.toTrackId.value_or(clip->trackId);
                const double targetStart = op.startTimelineSec.value_or(p.timelineStartSeconds);
                step = *planPlacementMove(p, clip->trackId, targetTrack, targetStart);
                clip->trackId = step.trackAfter;
                clip->placement = step.after;
                break;
            }
            case OpKind::Trim: {
                const auto snapshot = controlLiveSnapshot(context, p.contentKey);
                if (snapshot == nullptr)
                    return controlOpError("INTERNAL", "", "the content of the clip is not available");
                auto trimmed = planPlacementTrim(step.before, *snapshot, clip->trackId, op.trimStart, op.timelineSec);
                if (!trimmed)
                    return controlOpError("INVALID_ARGUMENT", "trim_out_of_range",
                                          "the edge would leave the audio of the clip's content or pass the clip's other edge");
                step = std::move(*trimmed);
                clip->placement = step.after;
                auto fadeIn = fadeChange(step.before.fadeInDuration, step.after.fadeInDuration);
                auto fadeOut = fadeChange(step.before.fadeOutDuration, step.after.fadeOutDuration);
                if (!fadeIn.isNull() || !fadeOut.isNull()) {
                    auto data = JsonValue::object();
                    data.appendMember("fadeInSec", std::move(fadeIn));
                    data.appendMember("fadeOutSec", std::move(fadeOut));
                    envelope.addWarning("FADES_RECLAMPED", placementRef(clip->placementId), std::move(data));
                }
                break;
            }
            case OpKind::SetGain: {
                const double linear = op.gainLinear ? *op.gainLinear : placementGainFromDb(*op.gainDb);
                auto gainPlan = planPlacementGain(step.before, clip->trackId, linear);
                if (!gainPlan)
                    return controlOpError("INVALID_ARGUMENT", "invalid_gain", "gain must be finite");
                step = std::move(*gainPlan);
                clip->placement = step.after;
                break;
            }
            case OpKind::SetFade: {
                step = planPlacementFade(step.before, clip->trackId, op.fadeInSec, op.fadeOutSec);
                clip->placement = step.after;
                if (op.fadeInSec && step.after.fadeInDuration < *op.fadeInSec)
                    warnClamped(envelope, *clip, "fadeInSec", controlJsonSeconds(*op.fadeInSec),
                                controlJsonSeconds(step.after.fadeInDuration));
                if (op.fadeOutSec && step.after.fadeOutDuration < *op.fadeOutSec)
                    warnClamped(envelope, *clip, "fadeOutSec", controlJsonSeconds(*op.fadeOutSec),
                                controlJsonSeconds(step.after.fadeOutDuration));
                break;
            }
            case OpKind::Split:
            case OpKind::Merge:
            case OpKind::Delete:
                return controlOpError("INTERNAL", "", "op not planned in place");
        }
        step.trackAfter = clip->trackId;
        const bool geometryChanged = step.trackBefore != step.trackAfter
                                     || step.before.timelineStartSeconds != step.after.timelineStartSeconds
                                     || step.before.durationSeconds != step.after.durationSeconds;
        if (geometryChanged && std::find(reshaped.begin(), reshaped.end(), step.after.placementId) == reshaped.end())
            reshaped.push_back(step.after.placementId);
        if (step.trackBefore != step.trackAfter)
            plannedReceiving.push_back(step.trackAfter);
        plan.push_back(step);
    }

    if (params.dryRun) {
        describeChanges(envelope, before, model);
        warnOverlaps(envelope, model, reshaped);
        revealTracks(context, envelope, plannedReceiving, true);
        return envelope.predicted(context);
    }

    std::vector<ControlOriginAction::Inner> inner;
    // The entry is described by the action of the first op that changed something (a trim's own action comes after
    // the record of its fades).
    std::optional<juce::String> description;
    bool refused = false;
    for (const auto& step : plan) {
        const auto count = inner.size();
        std::vector<std::unique_ptr<UndoAction>> undoActions;
        if (!commitPlacementEdit(processor, step, undoActions)) {
            refused = true;
            break;
        }
        for (auto& action : undoActions) {
            const auto name = controlUndoActionName(*action);
            inner.push_back(innerOf(std::move(action), name, {step.trackBefore, step.trackAfter}));
        }
        if (step.trackBefore != step.trackAfter)
            receivingTracks.push_back(step.trackAfter);
        if (!description && inner.size() > count)
            description = inner.back().action->getDescription();
    }
    PushedEntry pushed;
    const bool changed = !inner.empty();
    if (changed) {
        pushed = pushEntry(context, history, std::move(inner), description);
        afterChange(context);
        revealTracks(context, envelope, receivingTracks, false);
    }
    if (refused) {
        if (!changed)
            return controlOpError("INTERNAL", "", "OpenTune refused to change the clip");
        return controlOpErrorAfterChange(
            "INTERNAL", "",
            "OpenTune refused one of the ops after earlier ones changed clips; the changes "
            "made are one undo entry; read the project again");
    }
    const auto after = controlLivePlacements(context);
    describeChanges(envelope, before, after);
    warnOverlaps(envelope, after, reshaped);
    if (changed)
        envelope.setUndoable(pushed.undoId, pushed.description, pushed.names);
    return envelope.committed(context);
}

Result<JsonValue> runEdit(ControlOpContext& context, const EditParams& params)
{
    // The handles first (NOT_FOUND), with the track slot of op move, then busy and the arrangement revision.
    std::vector<ResolvedOp> resolved;
    for (const auto& op : params.ops) {
        ResolvedOp item;
        item.op = &op;
        if (op.kind == OpKind::Delete) {
            for (const auto& handle : op.placements) {
                auto clip = controlResolvePlacement(context, handle);
                if (!clip.ok())
                    return clip.error();
                item.clips.push_back(clip.value());
            }
        } else {
            auto clip = controlResolvePlacement(context, op.placement);
            if (!clip.ok())
                return clip.error();
            item.clip = clip.value();
            if (op.kind == OpKind::Merge) {
                auto otherClip = controlResolvePlacement(context, op.trailing);
                if (!otherClip.ok())
                    return otherClip.error();
                item.other = otherClip.value();
            }
            if (op.toTrackId)
                if (auto unknown = controlCheckTrackSlot(context, *op.toTrackId))
                    return *unknown;
        }
        resolved.push_back(std::move(item));
    }
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (controlArrangementRevision(context) != params.expectedArrangementRevision)
        return revisionConflict();
    switch (params.ops.front().kind) {
        case OpKind::Split:
            return runSplit(context, params, resolved.front());
        case OpKind::Merge:
            return runMerge(context, params, resolved.front());
        case OpKind::Delete:
            return runDelete(context, params, resolved.front());
        default:
            return runInPlace(context, params, resolved);
    }
}

// ---- arrangement.copy -----------------------------------------------------------------------------------

struct CopyParams
{
    std::vector<ControlHandle> sources;
    bool full = false;
    int toTrackId = 0;
    double startTimelineSec = 0.0;
    std::string expectedArrangementRevision;
};

bool parseCopy(const JsonValue& params, CopyParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<std::string> expected;
    std::optional<int> track;
    std::optional<double> start;
    if (!reader.onlyMembers({"sources", "mode", "toTrackId", "startTimelineSec", "expectedArrangementRevision"})
        || !controlReadRevisionToken(reader, "expectedArrangementRevision", expected)
        || !readTrackId(reader, "toTrackId", track) || !readSeconds(reader, "startTimelineSec", true, start)) {
        why = reader.why();
        return false;
    }
    if (!expected || !track) {
        why = "expectedArrangementRevision and toTrackId are required";
        return false;
    }
    const JsonValue* mode = reader.member("mode");
    if (mode == nullptr || !mode->isString() || (mode->text() != "window" && mode->text() != "full")) {
        why = "mode must be window or full";
        return false;
    }
    const JsonValue* sources = reader.member("sources");
    if (sources == nullptr || !sources->isArray() || sources->size() == 0 || sources->size() > kMaxItems) {
        why = "sources must hold 1 to 64 sources";
        return false;
    }
    for (std::size_t i = 0; i < sources->size(); ++i) {
        const auto& source = sources->at(i);
        if (!source.isObject()) {
            why = "sources must hold objects";
            return false;
        }
        ControlParamReader sourceReader(source);
        ControlHandle handle;
        if (!sourceReader.onlyMembers({"placementId"}) || !readPlacement(sourceReader, "placementId", handle)) {
            why = sourceReader.why();
            return false;
        }
        out.sources.push_back(handle);
    }
    out.full = mode->text() == "full";
    out.toTrackId = *track;
    out.startTimelineSec = *start;
    out.expectedArrangementRevision = *expected;
    return true;
}

Result<JsonValue> runCopy(ControlOpContext& context, const CopyParams& params)
{
    std::vector<ControlPlacementRef> sources;
    for (const auto& handle : params.sources) {
        auto clip = controlResolvePlacement(context, handle);
        if (!clip.ok())
            return clip.error();
        sources.push_back(clip.value());
    }
    if (auto unknown = controlCheckTrackSlot(context, params.toTrackId))
        return *unknown;
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (controlArrangementRevision(context) != params.expectedArrangementRevision)
        return revisionConflict();
    auto& processor = context.processor();
    if (processor.getStandaloneArrangement() == nullptr || processor.getStandaloneContentRepository() == nullptr)
        return controlOpError("INTERNAL", "", "the arrangement is not available");

    ControlWriteEnvelope envelope(context);
    double earliest = std::numeric_limits<double>::max();
    for (const auto& source : sources)
        earliest = std::min(earliest, source.placement.timelineStartSeconds);

    std::vector<PlacementCopyRequest> requests;
    requests.reserve(sources.size());
    for (const auto& source : sources) {
        requests.push_back({source.placement, params.toTrackId,
                            params.startTimelineSec + (source.placement.timelineStartSeconds - earliest), params.full});
    }
    auto copies = commitPlacementCopies(processor, requests);
    if (copies.failure == PlacementCopyCommit::Failure::ContentCopy)
        return controlOpError("INTERNAL", "", "OpenTune could not copy the content of a clip");
    if (copies.failure == PlacementCopyCommit::Failure::PlacementInsert)
        return controlOpError("INTERNAL", "", "OpenTune could not insert a copied clip");
    afterChange(context);

    // created: the new clips, then their contents; idMap relates every source to its copy.
    const auto after = controlLivePlacements(context);
    std::vector<std::string> labels;
    std::vector<std::string> from;
    std::vector<std::string> to;
    std::vector<ContentKey> contents;
    std::vector<std::uint64_t> made;
    for (std::size_t i = 0; i < copies.copies.size(); ++i) {
        const auto placementId = copies.copies[i].placement.placementId;
        made.push_back(placementId);
        contents.push_back(copies.copies[i].contentKey);
        labels.push_back(addCreatedClip(envelope, after, placementId));
        from.push_back(sources[i].placementId);
        to.push_back(controlPlacementId(context, placementId));
    }
    for (std::size_t i = 0; i < contents.size(); ++i) {
        const auto contentId = controlContentId(context, contents[i]);
        envelope.addCreated("content", contentRef(contentId), JsonValue::string("Content of " + labels[i]));
        envelope.touchContent(contentId);
    }
    addIdMapEntry(envelope, "copy", from, to);
    for (std::size_t i = 0; i < made.size(); ++i)
        if (std::abs(copies.copies[i].placement.timelineStartSeconds - sources[i].placement.timelineStartSeconds)
            <= kTimelineEpsilon)
            envelope.addWarning("STACKED_ON_SOURCE", placementRef(to[i]));
    warnOverlaps(envelope, after, made);
    revealTracks(context, envelope, {params.toTrackId}, false);

    // No undo entry, as in the UI; deleting the copies takes them back, which needs the end user's consent.
    auto ids = JsonValue::array();
    for (const auto& id : to)
        ids.append(JsonValue::string(id));
    auto remove = JsonValue::object();
    remove.appendMember("op", JsonValue::string("delete"));
    remove.appendMember("placementIds", std::move(ids));
    envelope.setNotUndoable("ui_parity", editRecipe(context, std::move(remove), true));
    return envelope.committed(context);
}

} // namespace

void registerArrangementOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<EditParams>("arrangement.edit", parseEdit, runEdit));
    table.add(controlWriteMethod<CopyParams>("arrangement.copy", parseCopy, runCopy));
}

} // namespace OpenTune
