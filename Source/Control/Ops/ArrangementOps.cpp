// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ArrangementOps.h"

#include "ClipIds.h"
#include "EditSupport.h"
#include "OpsSupport.h"
#include "TrackSupport.h"
#include "WriteEnvelope.h"

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneContentRepository.h"
#include "Control/ControlUiBridge.h"
#include "Control/Edit/ControlOriginAction.h"
#include "PluginProcessor.h"
#include "Utils/F0Timeline.h"
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
/** The shortest clip OpenTune keeps: StandaloneArrangement::setPlacementTrim() and the arrangement view's edge drag
    (ArrangementViewComponent::mouseDrag) limit a clip to 0.01 s. */
constexpr double kMinClipSec = 0.01;
/** The fades of a clip cover at most this share of its length (StandaloneArrangement::setPlacementFade() and
    setPlacementTrim(): durationSeconds * 0.9). */
constexpr double kMaxFadeShare = 0.9;
/** OpenTuneAudioProcessor::splitPlacementAtSeconds() keeps both parts longer than this (its minDurationSeconds). */
constexpr double kMinSplitPartSec = 0.1;
/** Timeline positions this close are one: one sample at OpenTune's render rate, the tolerance of
    OpenTuneAudioProcessor::mergePlacements() and of the arrangement's timeline setters. */
constexpr double kTimelineEpsilon = 1.0 / TimeCoordinate::kRenderSampleRate;
/** splitPlacementAtSeconds(): an analysis frame boundary this close to a sample lies on the sample grid. */
constexpr double kSampleGridTolerance = 1.0e-9;
/** mergePlacements(): gains this close are equal. */
constexpr float kGainTolerance = 1.0e-6f;
/** The largest clip gain OpenTune's interface offers, as a linear factor. The interface's one bounded gain range is the
    volume knob (Source/Standalone/UI/TrackPanelComponent.h, setRange(0.0, 4.0, 0.001)); the clip-gain drag
    (ArrangementViewComponent.cpp, mouseDrag) has no bound of its own, so the API applies the interface's
    bounded range here, mirrored by the mirror entry `track-volume-range`. Zero is the lower end: the params
    schema keeps gainLinear at 0 or more and StandaloneArrangement::setPlacementGain() floors the stored value
    at 0. */
constexpr double kMaxClipGainLinear = 4.0;

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

/** Records one change of entity placement for every clip that is in both lists and differs (PROTOCOL.md section 14.3),
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

/** The revert recipe of a split or a merge: arrangement.edit with the one op, at the arrangement as it is now. null
    when the instance does not report arrangement.v1 (PROTOCOL.md section 15.2 rule 2). */
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

/** A track position (common.schema.json#/$defs/trackId): an integer of 0 or more. A position beyond the slots is a
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

/** One op of arrangement.edit (methods/arrangement.edit.schema.json $defs.op*). */
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

/** The output length of a content: its time grid's, or the length of its audio. */
double contentOutputSeconds(const EditableContentSnapshot& snapshot)
{
    if (snapshot.timeGrid != nullptr && snapshot.timeGrid->totalDurationSeconds() > 0.0)
        return snapshot.timeGrid->totalDurationSeconds();
    if (snapshot.audioBuffer != nullptr && snapshot.audioSampleRate > 0.0)
        return static_cast<double>(snapshot.audioBuffer->getNumSamples()) / snapshot.audioSampleRate;
    return 0.0;
}

/**
 * op trim on a copy of the clip, as the arrangement view's edge drag computes it (ArrangementViewComponent::mouseDrag,
 * TrimLeft and TrimRight): the start edge cuts the content window by the same output time through the content's time
 * grid (tauForward of clipIn, plus the cut, through tauInverse), the end edge changes the length only. Unlike the drag,
 * an edge beyond the audio of the content or past the other edge is refused (trim_out_of_range). The fades are limited
 * to the new length as StandaloneArrangement::setPlacementTrim() limits them.
 */
std::optional<Error> planTrim(const EditOp& op, const EditableContentSnapshot& snapshot,
                              StandaloneArrangement::Placement& p)
{
    const auto outOfRange = [] {
        return controlOpError("INVALID_ARGUMENT", "trim_out_of_range",
                              "the edge would leave the audio of the clip's content or pass the clip's other edge");
    };
    const auto& grid = snapshot.timeGrid;
    const double outputStart = grid != nullptr ? grid->tauForward(p.clipInSeconds) : p.clipInSeconds;
    if (op.trimStart) {
        const double newDuration = p.timelineEndSeconds() - op.timelineSec;
        const double outputCut = outputStart + (p.durationSeconds - newDuration);
        if (newDuration < kMinClipSec || outputCut < -kTimelineEpsilon)
            return outOfRange();
        p.clipInSeconds = grid != nullptr ? grid->tauInverse(std::max(0.0, outputCut)) : std::max(0.0, outputCut);
        p.durationSeconds = newDuration;
        p.timelineStartSeconds = op.timelineSec;
    } else {
        const double newDuration = op.timelineSec - p.timelineStartSeconds;
        if (newDuration < kMinClipSec || outputStart + newDuration > contentOutputSeconds(snapshot) + kTimelineEpsilon)
            return outOfRange();
        p.durationSeconds = newDuration;
    }
    const double maxFade = p.durationSeconds * kMaxFadeShare;
    p.fadeInDuration = std::min(p.fadeInDuration, maxFade);
    p.fadeOutDuration = std::min(p.fadeOutDuration, maxFade);
    return std::nullopt;
}

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

/**
 * Where OpenTuneAudioProcessor::splitPlacementAtSeconds() cuts the clip, in timeline seconds, or why it would not:
 * its checks in its order, before anything changes. The position must lie inside the clip with both parts longer than
 * 0.1 s (split_out_of_range), the content's pitch analysis must be ready (NOT_READY), its time grid unedited, and the
 * nearest analysis frame boundary (F0Timeline::nearestFrameBoundary(), where the cut snaps to) must fall on an audio
 * sample and leave both parts longer than 0.1 s. OpenTune refuses the other cases without a word; the API names them
 * UNSUPPORTED(upstream_absent).
 */
Result<double> checkSplit(ControlOpContext& context, const ControlPlacementRef& clip, double atTimelineSec)
{
    const auto outOfRange = [](const char* detail) {
        return controlOpError("INVALID_ARGUMENT", "split_out_of_range", detail);
    };
    const auto& p = clip.placement;
    const double offset = atTimelineSec - p.timelineStartSeconds;
    if (offset <= kMinSplitPartSec || offset >= p.durationSeconds - kMinSplitPartSec)
        return outOfRange("atTimelineSec must lie inside the clip, more than 0.1 s from both edges");
    const auto snapshot = controlLiveSnapshot(context, p.contentKey);
    if (snapshot == nullptr || snapshot->audioBuffer == nullptr)
        return controlOpError("INTERNAL", "", "the content of the clip is not available");
    const ControlContentRef content{p.contentKey, controlContentId(context, p.contentKey), snapshot};
    if (auto notReady = controlCheckContentReady(context, content))
        return *notReady;
    if (snapshot->timeGrid != nullptr && !snapshot->timeGrid->isIdentity())
        return controlOpError("UNSUPPORTED", "upstream_absent",
                              "OpenTune does not split a clip whose time grid was edited");
    const double sampleRate = snapshot->audioSampleRate;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return controlOpError("UNSUPPORTED", "upstream_absent", "the audio of the clip has no sample rate");
    if (snapshot->pitchCurve == nullptr)
        return controlOpError("NOT_READY", "f0_not_ready", "the clip has no pitch analysis yet");
    const auto& curve = *snapshot->pitchCurve;
    const int frames = static_cast<int>(curve.getOriginalF0().size());
    const F0Timeline timeline(curve.getHopSize(), curve.getSampleRate(), frames);
    const int boundary = timeline.nearestFrameBoundary(p.clipInSeconds + offset);
    if (boundary <= 0 || boundary >= frames)
        return outOfRange("no analysis frame boundary inside the clip lies near atTimelineSec");
    const double splitSourceSeconds = timeline.timeAtFrame(boundary);
    const auto splitSample = TimeCoordinate::secondsToSamplesNearest(splitSourceSeconds, sampleRate);
    if (std::abs(TimeCoordinate::samplesToSeconds(splitSample, sampleRate) - splitSourceSeconds) > kSampleGridTolerance)
        return controlOpError("UNSUPPORTED", "upstream_absent",
                              "the analysis frame nearest to atTimelineSec does not fall on an audio sample, where "
                              "OpenTune cuts");
    if (splitSample <= 0 || splitSample >= snapshot->audioBuffer->getNumSamples())
        return outOfRange("the nearest analysis frame lies outside the audio of the clip");
    const double splitOffset = splitSourceSeconds - p.clipInSeconds;
    if (splitOffset <= kMinSplitPartSec || splitOffset >= p.durationSeconds - kMinSplitPartSec)
        return outOfRange("the nearest analysis frame lies within 0.1 s of an edge of the clip");
    return p.timelineStartSeconds + splitOffset;
}

/**
 * The conditions of OpenTuneAudioProcessor::mergePlacements() that the API can check before anything changes, in its
 * order: one track, the trailing clip starting where the leading clip ends, equal gain and name, one source, equal
 * sample rates, adjacent source windows to the sample, and no analysis running. mergePlacements() also refuses
 * contents with different keys and pitch curves it cannot join; the call reports its refusal with the same reason.
 */
std::optional<Error> checkMerge(ControlOpContext& context, const ControlPlacementRef& leading,
                                const ControlPlacementRef& trailing)
{
    const auto fail = [](const char* detail) {
        return controlOpError("INVALID_ARGUMENT", "merge_not_possible", detail);
    };
    const auto& a = leading.placement;
    const auto& b = trailing.placement;
    if (a.placementId == b.placementId)
        return fail("the leading and the trailing clip are the same clip");
    if (leading.trackId != trailing.trackId)
        return fail("the clips are on different tracks");
    if (std::abs(a.timelineEndSeconds() - b.timelineStartSeconds) > kTimelineEpsilon)
        return fail("the trailing clip does not start where the leading clip ends");
    if (std::abs(a.gain - b.gain) > kGainTolerance || a.name != b.name)
        return fail("the clips differ in gain or in name");
    const auto first = controlLiveSnapshot(context, a.contentKey);
    const auto second = controlLiveSnapshot(context, b.contentKey);
    if (first == nullptr || second == nullptr || first->audioBuffer == nullptr || second->audioBuffer == nullptr
        || first->sourceWindow.sourceId == 0 || first->sourceWindow.sourceId != second->sourceWindow.sourceId)
        return fail("the clips do not show parts of one source");
    if (!std::isfinite(first->audioSampleRate) || first->audioSampleRate <= 0.0
        || first->audioSampleRate != second->audioSampleRate)
        return fail("the audio of the clips has different sample rates");
    if (TimeCoordinate::secondsToSamplesNearest(first->sourceWindow.sourceEndSeconds, first->audioSampleRate)
        != TimeCoordinate::secondsToSamplesNearest(second->sourceWindow.sourceStartSeconds, second->audioSampleRate))
        return fail("the clips do not show adjacent parts of their source");
    if (first->originalF0State == OriginalF0State::Extracting || second->originalF0State == OriginalF0State::Extracting)
        return controlOpError("NOT_READY", "f0_not_ready", "the pitch analysis of a clip is running");
    return std::nullopt;
}

// ---- arrangement.edit: split, merge, delete ------------------------------------------------------------

Result<JsonValue> runSplit(ControlOpContext& context, const EditParams& params, const ResolvedOp& item)
{
    const auto& clip = item.clip;
    const double at = item.op->timelineSec;
    const auto point = checkSplit(context, clip, at);
    if (!point.ok())
        return point.error();
    auto& processor = context.processor();
    const auto history = context.history();
    if (history == nullptr)
        return controlOpError("INTERNAL", "", "the undo history of the control API is not available");

    ControlWriteEnvelope envelope(context);
    const auto label = placementLabel(clip);
    const auto addWarnings = [&] {
        if (std::abs(point.value() - at) > kSampleGridTolerance) {
            auto data = JsonValue::object();
            data.appendMember("requestedTimelineSec", controlJsonSeconds(at));
            data.appendMember("appliedTimelineSec", controlJsonSeconds(point.value()));
            envelope.addWarning("SPLIT_SNAPPED", placementRef(clip.placementId), std::move(data));
        }
        envelope.addWarning("UPSTREAM_UNDO_DEFECT", placementRef(clip.placementId));
    };
    if (params.dryRun) {
        // The two new clips get their placementIds when they are made; the prediction names the clip they replace.
        envelope.addRemoved("placement", placementRef(clip.placementId), JsonValue::string(label));
        addWarnings();
        return envelope.predicted(context);
    }

    // The split key of the arrangement view: splitPlacementAtSeconds() at the clip's position on its track, then
    // SplitPlacementAction on the undo history. The call makes two clips; after a project was opened OpenTune's clip
    // id counter can stand on the ids of the opened clips, so the counter is moved to where the next two ids are free
    // first (ClipIds.h): the call would refuse the split without a word, and when only one of its two ids is in use,
    // it would leave the other new clip behind as a content that no clip shows.
    controlSkipUsedClipIds(context, "arrangement.edit split", 2);
    const int index = processor.findPlacementIndexById(clip.trackId, clip.placement.placementId);
    const auto outcome = processor.splitPlacementAtSeconds(clip.trackId, index, at);
    if (!outcome)
        return controlOpError("UNSUPPORTED", "upstream_absent", "OpenTune did not split the clip at this position");
    controlRecordCommittedClipId(context, outcome->leadingContentKey);
    controlRecordCommittedClipId(context, outcome->trailingContentKey);
    std::vector<ControlOriginAction::Inner> inner;
    inner.push_back(
        innerOf(std::make_unique<SplitPlacementAction>(processor, *outcome), "SplitPlacementAction", {clip.trackId}));
    pushEntry(context, history, std::move(inner));
    afterChange(context);

    const auto after = controlLivePlacements(context);
    envelope.addRemoved("placement", placementRef(clip.placementId), JsonValue::string(label));
    addCreatedClip(envelope, after, outcome->leadingPlacementId);
    addCreatedClip(envelope, after, outcome->trailingPlacementId);
    const auto leading = controlPlacementId(context, outcome->leadingPlacementId);
    const auto trailing = controlPlacementId(context, outcome->trailingPlacementId);
    addIdMapEntry(envelope, "split", {clip.placementId}, {leading, trailing});
    addWarnings();
    auto merge = JsonValue::object();
    merge.appendMember("op", JsonValue::string("merge"));
    merge.appendMember("leadingPlacementId", JsonValue::string(leading));
    merge.appendMember("trailingPlacementId", JsonValue::string(trailing));
    envelope.setNotUndoable("upstream_undo_defect", editRecipe(context, std::move(merge), false));
    return envelope.committed(context);
}

Result<JsonValue> runMerge(ControlOpContext& context, const EditParams& params, const ResolvedOp& item)
{
    const auto& leading = item.clip;
    const auto& trailing = *item.other;
    if (auto refused = checkMerge(context, leading, trailing))
        return *refused;
    auto& processor = context.processor();
    auto* arrangement = processor.getStandaloneArrangement();
    const auto history = context.history();
    if (arrangement == nullptr || history == nullptr)
        return controlOpError("INTERNAL", "", "the arrangement or the undo history is not available");

    ControlWriteEnvelope envelope(context);
    const auto addRemoved = [&] {
        envelope.addRemoved("placement", placementRef(leading.placementId), JsonValue::string(placementLabel(leading)));
        envelope.addRemoved("placement", placementRef(trailing.placementId),
                            JsonValue::string(placementLabel(trailing)));
    };
    if (params.dryRun) {
        addRemoved();
        envelope.addWarning("UPSTREAM_UNDO_DEFECT", placementRef(leading.placementId));
        return envelope.predicted(context);
    }

    // The merge key of the arrangement view: mergePlacements() with the leading clip's position on its track, then
    // MergePlacementAction on the undo history. The call makes one clip, so the clip id counter is moved to where the
    // next id is free first, as for a split (ClipIds.h).
    controlSkipUsedClipIds(context, "arrangement.edit merge", 1);
    const int index = arrangement->findPlacementIndexById(leading.trackId, leading.placement.placementId);
    const auto outcome = processor.mergePlacements(leading.trackId, leading.placement.placementId,
                                                   trailing.placement.placementId, index);
    if (!outcome)
        return controlOpError("INVALID_ARGUMENT", "merge_not_possible",
                              "OpenTune did not merge the clips: their keys or their pitch analyses do not join");
    controlRecordCommittedClipId(context, outcome->mergedContentKey);
    std::vector<ControlOriginAction::Inner> inner;
    inner.push_back(innerOf(std::make_unique<MergePlacementAction>(processor, *outcome), "MergePlacementAction",
                            {leading.trackId}));
    pushEntry(context, history, std::move(inner));
    afterChange(context);

    const auto after = controlLivePlacements(context);
    addRemoved();
    addCreatedClip(envelope, after, outcome->mergedPlacementId);
    const auto merged = controlPlacementId(context, outcome->mergedPlacementId);
    addIdMapEntry(envelope, "merge", {leading.placementId, trailing.placementId}, {merged});
    envelope.addWarning("UPSTREAM_UNDO_DEFECT", placementRef(leading.placementId));
    // Splitting the merged clip at the former boundary takes it back, with new placementIds, but a split snaps to an
    // analysis frame boundary. checkSplit() takes an absolute timeline position (as runSplit() passes it) and says
    // where the cut would fall on the merged clip:
    //  - it cuts elsewhere than the boundary: no request restores the former clips (PROTOCOL.md section 15.2), so
    //    IRREVERSIBLE with no revert;
    //  - the check fails for a lasting reason (time grid edited, boundary near an edge, ...): the split cannot be
    //    done either, IRREVERSIBLE with the check's kind and reason in data;
    //  - the check fails with NOT_READY (the pitch analysis of the merged content is still pending): a passing
    //    state, so the split recipe is given as usual and answers NOT_READY itself until the analysis is done.
    //    That recipe is optimistic: the cut point could not be checked yet, so once the analysis is ready the split
    //    may snap to another frame than the former boundary (the split then reports SPLIT_SNAPPED) or be refused
    //    (split_out_of_range). It is not guaranteed to restore the former clips' boundary.
    const double boundary = trailing.placement.timelineStartSeconds;
    const auto* mergedClip = findIn(after, outcome->mergedPlacementId);
    bool giveRecipe = false;
    std::string cutKind;
    std::string cutReason;
    if (mergedClip != nullptr) {
        const auto cut = checkSplit(context, *mergedClip, boundary);
        if (cut.ok()) {
            giveRecipe = std::abs(cut.value() - boundary) <= kSampleGridTolerance;
        } else {
            const auto reply = controlMapError(cut.error());
            cutKind = reply.kind;
            cutReason = reply.reason;
            giveRecipe = cutKind == "NOT_READY";
        }
    }
    if (giveRecipe) {
        auto split = JsonValue::object();
        split.appendMember("op", JsonValue::string("split"));
        split.appendMember("placementId", JsonValue::string(merged));
        split.appendMember("atTimelineSec", controlJsonSeconds(boundary));
        envelope.setNotUndoable("upstream_undo_defect", editRecipe(context, std::move(split), false));
    } else {
        auto data = JsonValue::object();
        data.appendMember("reason", JsonValue::string("split_cannot_restore_boundary"));
        if (!cutKind.empty()) {
            data.appendMember("checkKind", JsonValue::string(cutKind));
            data.appendMember("checkReason", JsonValue::string(cutReason));
        }
        envelope.addWarning("IRREVERSIBLE", placementRef(merged), std::move(data));
        envelope.setNotUndoable("upstream_undo_defect");
    }
    return envelope.committed(context);
}

Result<JsonValue> runDelete(ControlOpContext& context, const EditParams& params, const ResolvedOp& item)
{
    if (!params.dryRun && !params.confirm)
        return controlOpError("INVALID_ARGUMENT", "confirm_required",
                              "deleting clips cannot be undone; the end user must agree, and confirm must be true");
    auto& processor = context.processor();
    const auto history = context.history();
    if (history == nullptr)
        return controlOpError("INTERNAL", "", "the undo history of the control API is not available");

    ControlWriteEnvelope envelope(context);
    std::vector<std::size_t> deleted;
    std::vector<ControlOriginAction::Inner> inner;
    if (!params.dryRun) {
        // The delete key of the arrangement view: deletePlacement() for every clip, each with a DeletePlacementAction;
        // the API puts them in one undo entry.
        for (std::size_t i = 0; i < item.clips.size(); ++i) {
            const auto& clip = item.clips[i];
            const int index = processor.findPlacementIndexById(clip.trackId, clip.placement.placementId);
            const auto outcome = processor.deletePlacement(clip.trackId, index);
            if (!outcome)
                break;
            inner.push_back(innerOf(std::make_unique<DeletePlacementAction>(processor, *outcome),
                                    "DeletePlacementAction", {clip.trackId}));
            deleted.push_back(i);
        }
        if (!inner.empty()) {
            pushEntry(context, history, std::move(inner));
            afterChange(context);
        }
        if (deleted.size() != item.clips.size()) {
            if (deleted.empty())
                return controlOpError("INTERNAL", "", "OpenTune did not delete the clip");
            return controlOpErrorAfterChange("INTERNAL", "",
                                             "OpenTune did not delete every clip; read the project again");
        }
    }

    // Removed: the clips, then the contents that left with them (PROTOCOL.md section 14.4 rule 6). deletePlacement()
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
    for (const auto& clip : item.clips) {
        envelope.addWarning("UPSTREAM_UNDO_DEFECT", placementRef(clip.placementId));
        envelope.addWarning("IRREVERSIBLE", placementRef(clip.placementId));
    }
    if (params.dryRun)
        return envelope.predicted(context);
    envelope.setNotUndoable("upstream_undo_defect");
    return envelope.committed(context);
}

// ---- arrangement.edit: the ops that change clips in place ----------------------------------------------

/** One in-place op as planned on the model: the clip before and after it. */
struct Planned
{
    OpKind kind = OpKind::Move;
    bool trimStart = false;
    int trackBefore = 0;
    int trackAfter = 0;
    StandaloneArrangement::Placement before;
    StandaloneArrangement::Placement after;
};

/** Applies one planned op through the arrangement's setter the arrangement view calls and records the undo action it
    pushes. False when OpenTune refused the change. */
bool applyPlanned(OpenTuneAudioProcessor& processor, StandaloneArrangement& arrangement, const Planned& step,
                  std::vector<ControlOriginAction::Inner>& inner)
{
    const auto& was = step.before;
    const auto& now = step.after;
    const auto id = was.placementId;
    const int track = step.trackAfter;
    switch (step.kind) {
        case OpKind::Move: {
            if (step.trackBefore == step.trackAfter && was.timelineStartSeconds == now.timelineStartSeconds)
                return true;
            // The clip drag: applyPlacementMoves(), recorded as MultiMovePlacementAction.
            const auto result = arrangement.applyPlacementMoves({StandaloneArrangement::PlacementMove{
                step.trackBefore, step.trackAfter, id, now.timelineStartSeconds}});
            if (result == StandaloneArrangement::PlacementMoveResult::Rejected)
                return false;
            if (result == StandaloneArrangement::PlacementMoveResult::Unchanged)
                return true;
            MultiMovePlacementAction::Entry entry;
            entry.sourceTrackId = step.trackBefore;
            entry.targetTrackId = step.trackAfter;
            entry.placementId = id;
            entry.oldStartSeconds = was.timelineStartSeconds;
            entry.newStartSeconds = now.timelineStartSeconds;
            inner.push_back(innerOf(std::make_unique<MultiMovePlacementAction>(
                                        processor, std::vector<MultiMovePlacementAction::Entry>{entry},
                                        PlacementKey{step.trackBefore, id}, PlacementKey{step.trackAfter, id}),
                                    "MultiMovePlacementAction", {step.trackBefore, step.trackAfter}));
            return true;
        }
        case OpKind::Trim: {
            if (was.clipInSeconds == now.clipInSeconds && was.durationSeconds == now.durationSeconds
                && was.timelineStartSeconds == now.timelineStartSeconds)
                return true;
            // The edge drag: setPlacementTrimAndTimelineStart() for the start edge, setPlacementTrim() for the end.
            const bool applied = step.trimStart
                                     ? arrangement.setPlacementTrimAndTimelineStart(
                                           track, id, now.clipInSeconds, now.durationSeconds, now.timelineStartSeconds)
                                     : arrangement.setPlacementTrim(track, id, now.clipInSeconds, now.durationSeconds);
            StandaloneArrangement::Placement result;
            if (!applied || !arrangement.getPlacementById(track, id, result))
                return false;
            // The fades the trim shortened, recorded before the trim, so that undoing the entry restores them.
            if (result.fadeInDuration != was.fadeInDuration || result.fadeOutDuration != was.fadeOutDuration)
                inner.push_back(innerOf(std::make_unique<FadeChangeAction>(processor, track, id, was.fadeInDuration,
                                                                           was.fadeOutDuration, result.fadeInDuration,
                                                                           result.fadeOutDuration),
                                        "FadeChangeAction", {track}));
            inner.push_back(innerOf(
                std::make_unique<TrimPlacementAction>(processor, track, id, was.clipInSeconds, was.durationSeconds,
                                                      result.clipInSeconds, result.durationSeconds,
                                                      was.timelineStartSeconds, result.timelineStartSeconds),
                "TrimPlacementAction", {track}));
            return true;
        }
        case OpKind::SetGain:
            if (was.gain == now.gain)
                return true;
            if (!arrangement.setPlacementGain(track, id, now.gain))
                return false;
            inner.push_back(innerOf(std::make_unique<GainChangeAction>(processor, track, id, was.gain, now.gain),
                                    "GainChangeAction", {track}));
            return true;
        case OpKind::SetFade:
            if (was.fadeInDuration == now.fadeInDuration && was.fadeOutDuration == now.fadeOutDuration)
                return true;
            if (!arrangement.setPlacementFade(track, id, now.fadeInDuration, now.fadeOutDuration))
                return false;
            inner.push_back(innerOf(std::make_unique<FadeChangeAction>(processor, track, id, was.fadeInDuration,
                                                                       was.fadeOutDuration, now.fadeInDuration,
                                                                       now.fadeOutDuration),
                                    "FadeChangeAction", {track}));
            return true;
        case OpKind::Split:
        case OpKind::Merge:
        case OpKind::Delete:
            break;
    }
    return false;
}

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
    std::vector<Planned> plan;
    std::vector<std::uint64_t> reshaped;
    // The tracks that clips move to: all of the plan for a prediction, only the steps OpenTune applied for a commit.
    std::vector<int> plannedReceiving;
    std::vector<int> receivingTracks;
    for (const auto& item : ops) {
        const auto& op = *item.op;
        auto* clip = findIn(model, item.clip.placement.placementId);
        if (clip == nullptr)
            return controlOpError("INTERNAL", "", "a clip of the call is not in the arrangement");
        Planned step;
        step.kind = op.kind;
        step.trimStart = op.trimStart;
        step.trackBefore = clip->trackId;
        step.before = clip->placement;
        auto& p = clip->placement;
        switch (op.kind) {
            case OpKind::Move:
                clip->trackId = op.toTrackId.value_or(clip->trackId);
                p.timelineStartSeconds = op.startTimelineSec.value_or(p.timelineStartSeconds);
                break;
            case OpKind::Trim: {
                const auto snapshot = controlLiveSnapshot(context, p.contentKey);
                if (snapshot == nullptr)
                    return controlOpError("INTERNAL", "", "the content of the clip is not available");
                if (auto error = planTrim(op, *snapshot, p))
                    return *error;
                auto fadeIn = fadeChange(step.before.fadeInDuration, p.fadeInDuration);
                auto fadeOut = fadeChange(step.before.fadeOutDuration, p.fadeOutDuration);
                if (!fadeIn.isNull() || !fadeOut.isNull()) {
                    auto data = JsonValue::object();
                    data.appendMember("fadeInSec", std::move(fadeIn));
                    data.appendMember("fadeOutSec", std::move(fadeOut));
                    envelope.addWarning("FADES_RECLAMPED", placementRef(clip->placementId), std::move(data));
                }
                break;
            }
            case OpKind::SetGain: {
                // OpenTune's interface offers the gain range 0 to 4 linear (the volume knob, TrackPanelComponent.h);
                // the clip-gain drag itself has no bound (ArrangementViewComponent.cpp, mouseDrag), so the
                // API applies the interface's bounded range and reports CLAMPED in the unit the caller named. The
                // lower end 0 is kept by the params schema (gainLinear minimum 0) and setPlacementGain()'s floor.
                double linear = op.gainLinear ? *op.gainLinear : std::pow(10.0, *op.gainDb / 20.0);
                // The negated test also clamps NaN (a JSON number cannot be NaN; this only guards the arithmetic).
                if (!(linear <= kMaxClipGainLinear)) {
                    linear = kMaxClipGainLinear;
                    if (op.gainDb)
                        warnClamped(envelope, *clip, "gainDb", controlJsonNumber(*op.gainDb, 3),
                                    controlJsonNumber(*controlLinearToDb(linear), 3));
                    else
                        warnClamped(envelope, *clip, "gainLinear", controlJsonNumber(*op.gainLinear, 6),
                                    controlJsonNumber(linear, 6));
                }
                p.gain = static_cast<float>(linear);
                break;
            }
            case OpKind::SetFade: {
                const double maxFade = p.durationSeconds * kMaxFadeShare;
                const auto limit = [&](const char* field, const std::optional<double>& requested, double& value) {
                    if (!requested)
                        return;
                    value = std::min(*requested, maxFade);
                    if (value < *requested)
                        warnClamped(envelope, *clip, field, controlJsonSeconds(*requested), controlJsonSeconds(value));
                };
                limit("fadeInSec", op.fadeInSec, p.fadeInDuration);
                limit("fadeOutSec", op.fadeOutSec, p.fadeOutDuration);
                break;
            }
            case OpKind::Split:
            case OpKind::Merge:
            case OpKind::Delete:
                return controlOpError("INTERNAL", "", "op not planned in place");
        }
        step.trackAfter = clip->trackId;
        step.after = p;
        const bool geometryChanged = step.trackBefore != step.trackAfter
                                     || step.before.timelineStartSeconds != p.timelineStartSeconds
                                     || step.before.durationSeconds != p.durationSeconds;
        if (geometryChanged && std::find(reshaped.begin(), reshaped.end(), p.placementId) == reshaped.end())
            reshaped.push_back(p.placementId);
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
        if (!applyPlanned(processor, *arrangement, step, inner)) {
            refused = true;
            break;
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
    auto* arrangement = processor.getStandaloneArrangement();
    auto* repository = processor.getStandaloneContentRepository();
    if (arrangement == nullptr || repository == nullptr)
        return controlOpError("INTERNAL", "", "the arrangement is not available");

    ControlWriteEnvelope envelope(context);
    double earliest = std::numeric_limits<double>::max();
    for (const auto& source : sources)
        earliest = std::min(earliest, source.placement.timelineStartSeconds);

    // The contents first, so that a failure leaves the arrangement as it was: paste copies the part of the content the
    // clip shows (copyContentRange()), duplicate the whole content (cloneContent()), as the arrangement view's keys do.
    // Each of the two makes a clip per source; the clip id counter is moved to where that many ids are free first, so
    // that none of the calls meets an id in use (ClipIds.h).
    controlSkipUsedClipIds(context, params.full ? "arrangement.copy full" : "arrangement.copy window", sources.size());
    std::vector<ContentKey> contents;
    for (const auto& source : sources) {
        const auto& p = source.placement;
        const auto key = params.full ? processor.cloneContent(p.contentKey, p.name + " Copy")
                                     : processor.copyContentRange(p.contentKey, p.clipInSeconds, p.durationSeconds);
        if (!key.isValid()) {
            for (const auto& made : contents)
                repository->retireClip(made);
            return controlOpError("INTERNAL", "", "OpenTune could not copy the content of a clip");
        }
        controlRecordCommittedClipId(context, key);
        contents.push_back(key);
    }
    std::vector<std::uint64_t> made;
    std::vector<double> starts;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto& source = sources[i].placement;
        StandaloneArrangement::Placement placement;
        if (params.full) {
            placement = source;
        } else {
            // Paste: a clip that shows its whole new content, with the gain, fades and name of the source.
            placement.durationSeconds = source.durationSeconds;
            placement.gain = source.gain;
            placement.fadeInDuration = source.fadeInDuration;
            placement.fadeOutDuration = source.fadeOutDuration;
            placement.name = source.name;
            placement.clipInSeconds = 0.0;
        }
        placement.placementId = 0;
        placement.contentKey = contents[i];
        placement.timelineStartSeconds = params.startTimelineSec + (source.timelineStartSeconds - earliest);
        if (!arrangement->insertPlacement(params.toTrackId, placement)) {
            for (std::size_t j = i; j < contents.size(); ++j)
                repository->retireClip(contents[j]);
            if (made.empty())
                return controlOpError("INTERNAL", "", "OpenTune could not insert a copied clip");
            afterChange(context);
            return controlOpErrorAfterChange("INTERNAL", "",
                                             "OpenTune could not insert a copied clip; read the project again");
        }
        made.push_back(placement.placementId);
        starts.push_back(placement.timelineStartSeconds);
    }
    afterChange(context);

    // created: the new clips, then their contents; idMap relates every source to its copy.
    const auto after = controlLivePlacements(context);
    std::vector<std::string> labels;
    std::vector<std::string> from;
    std::vector<std::string> to;
    for (std::size_t i = 0; i < made.size(); ++i) {
        labels.push_back(addCreatedClip(envelope, after, made[i]));
        from.push_back(sources[i].placementId);
        to.push_back(controlPlacementId(context, made[i]));
    }
    for (std::size_t i = 0; i < contents.size(); ++i) {
        const auto contentId = controlContentId(context, contents[i]);
        envelope.addCreated("content", contentRef(contentId), JsonValue::string("Content of " + labels[i]));
        envelope.touchContent(contentId);
    }
    addIdMapEntry(envelope, "copy", from, to);
    for (std::size_t i = 0; i < made.size(); ++i)
        if (std::abs(starts[i] - sources[i].placement.timelineStartSeconds) <= kTimelineEpsilon)
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
