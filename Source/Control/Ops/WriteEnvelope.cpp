// SPDX-License-Identifier: AGPL-3.0-or-later
#include "WriteEnvelope.h"

#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"

#include <algorithm>

namespace OpenTune {

namespace {

JsonValue revisionPair(const std::string* before, const std::string* after)
{
    auto pair = JsonValue::object();
    pair.appendMember("before", before != nullptr ? JsonValue::string(*before) : JsonValue());
    pair.appendMember("after", after != nullptr ? JsonValue::string(*after) : JsonValue());
    return pair;
}

/** The components of a content's versions (section 14.4), by protocol name. */
// clang-format off
const std::pair<const char*, std::string ControlContentVersions::*> kContentComponents[] = {
    {"notes", &ControlContentVersions::notes},
    {"pitch", &ControlContentVersions::pitch},
    {"content", &ControlContentVersions::content},
    {"grid", &ControlContentVersions::grid},
    {"key", &ControlContentVersions::key},
    {"pitchShift", &ControlContentVersions::pitchShift},
};
// clang-format on

const ControlContentVersions* findVersions(const ControlWorldVersions& world, const std::string& contentId)
{
    for (const auto& versions : world.contents)
        if (versions.contentId == contentId)
            return &versions;
    return nullptr;
}

bool isEmptyDiff(const JsonValue& diff)
{
    const JsonValue* arrangement = diff.find("arrangement");
    const JsonValue* trackState = diff.find("trackState");
    const JsonValue* contents = diff.find("contents");
    return arrangement != nullptr && arrangement->isNull() && trackState != nullptr && trackState->isNull()
           && contents != nullptr && contents->size() == 0;
}

JsonValue emptyDiff()
{
    auto diff = JsonValue::object();
    diff.appendMember("arrangement", JsonValue());
    diff.appendMember("trackState", JsonValue());
    diff.appendMember("contents", JsonValue::array());
    return diff;
}

bool hasWarning(const JsonValue& warnings, const char* code)
{
    for (std::size_t i = 0; i < warnings.size(); ++i)
        if (const JsonValue* existing = warnings.at(i).find("code"); existing != nullptr && existing->text() == code)
            return true;
    return false;
}

} // namespace

// ---- Checks before a write ------------------------------------------------------------------------

std::optional<Error> controlCheckBusy(ControlOpContext& context, ControlBusyScope scope)
{
    // ControlUiBridge::queryBusy(): the editor's own flags of gestures, modal dialogs and running
    // operations. The UI has priority: the write is refused rather than queued.
    auto* bridge = context.bridge();
    const auto busy = bridge != nullptr ? bridge->queryBusy() : std::nullopt;
    // OpenTuneAudioProcessor::isExportLockHeld(): the export lock that the editor's export and export.start
    // share. It belongs to the processor, so it is held also without an editor.
    const bool exportLocked = context.processor().isExportLockHeld();
    if (busy && busy->pointerDown)
        return controlOpError("BUSY", "pointer_down", "the user is in the middle of a mouse or pen gesture");
    if (busy && busy->modalOpen && scope != ControlBusyScope::DialogDismiss)
        return controlOpError("BUSY", "modal_open", "a modal dialog is open in OpenTune");
    if (busy && busy->importRunning)
        return controlOpError("BUSY", "import_in_progress", "an import started in the OpenTune UI is running");
    if (exportLocked || (busy && busy->exportRunning))
        return controlOpError("BUSY", "export_in_progress", "an export is running");
    if (busy && busy->operationRunning)
        return controlOpError("BUSY", "project_operation", "a project save or open is running");
    return std::nullopt;
}

// ---- Pieces of a write result ----------------------------------------------------------------------

bool controlProjectDirty(ControlOpContext& context)
{
    auto* bridge = context.bridge();
    if (bridge == nullptr)
        return false;
    const auto info = bridge->projectInfo();
    return info && info->dirty;
}

void controlMarkDirty(ControlOpContext& context)
{
    if (auto* bridge = context.bridge())
        bridge->markDirty();
}

bool controlReportsCapability(ControlOpContext& context, std::string_view capability)
{
    const auto& capabilities = context.facts().capabilities;
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

JsonValue controlRevertRecipe(std::string method, JsonValue params, bool requiresConsent)
{
    auto recipe = JsonValue::object();
    recipe.appendMember("method", JsonValue::string(std::move(method)));
    recipe.appendMember("params", params.isObject() ? std::move(params) : JsonValue::object());
    if (requiresConsent)
        recipe.appendMember("requiresConsent", JsonValue::boolean(true));
    return recipe;
}

JsonValue controlUiSnapshot(ControlOpContext& context, std::string& nullReason)
{
    auto* bridge = context.bridge();
    if (bridge == nullptr) {
        nullReason = "no_editor";
        return JsonValue();
    }
    const auto view = bridge->viewState();
    const auto selected = bridge->selectedPlacementIds();
    if (!view || !selected) {
        nullReason = "unavailable";
        return JsonValue();
    }
    auto json = JsonValue::object();
    json.appendMember("workspace", JsonValue::string(view->workspace == "pianoRoll" ? "pianoRoll" : "arrangement"));
    JsonValue pianoRollContent;
    if (view->pianoRollPlacementId != 0) {
        ControlHandle handle;
        handle.kind = ControlHandleKind::Placement;
        handle.instanceShort = context.instanceShort();
        handle.epoch = controlContentEpoch(context);
        handle.object = view->pianoRollPlacementId;
        const auto placement = controlResolvePlacement(context, handle);
        if (placement.ok())
            pianoRollContent = JsonValue::string(controlContentId(context, placement.value().placement.contentKey));
    }
    json.appendMember("pianoRollContentId", std::move(pianoRollContent));
    const auto* arrangement = context.processor().getStandaloneArrangement();
    const int activeTrack = arrangement != nullptr ? arrangement->getActiveTrackId() : -1;
    json.appendMember("activeTrackId", activeTrack >= 0 ? JsonValue::integer(activeTrack) : JsonValue());
    auto ids = JsonValue::array();
    for (const auto id : *selected)
        if (id != 0)
            ids.append(JsonValue::string(controlPlacementId(context, id)));
    json.appendMember("selectedPlacementIds", std::move(ids));
    bool derived = false;
    json.appendMember("visibleTrackCount", JsonValue::integer(controlVisibleTrackCount(context, derived)));
    return json;
}

JsonValue controlRevisionDiff(const ControlWorldVersions& before, const ControlWorldVersions& after,
                              const std::vector<std::string>* onlyContents)
{
    auto diff = JsonValue::object();
    diff.appendMember("arrangement", before.arrangement != after.arrangement
                                         ? revisionPair(&before.arrangement, &after.arrangement)
                                         : JsonValue());
    diff.appendMember("trackState", before.trackState != after.trackState
                                        ? revisionPair(&before.trackState, &after.trackState)
                                        : JsonValue());
    // Contents of both captures, in the order of the capture before, then the new ones.
    std::vector<std::string> ids;
    for (const auto& versions : before.contents)
        ids.push_back(versions.contentId);
    for (const auto& versions : after.contents)
        if (std::find(ids.begin(), ids.end(), versions.contentId) == ids.end())
            ids.push_back(versions.contentId);
    auto contents = JsonValue::array();
    for (const auto& id : ids) {
        if (onlyContents != nullptr && std::find(onlyContents->begin(), onlyContents->end(), id) == onlyContents->end())
            continue;
        const auto* was = findVersions(before, id);
        const auto* now = findVersions(after, id);
        auto changed = JsonValue::object();
        for (const auto& [name, member] : kContentComponents) {
            const std::string* wasValue = was != nullptr ? &(was->*member) : nullptr;
            const std::string* nowValue = now != nullptr ? &(now->*member) : nullptr;
            if (wasValue == nullptr || nowValue == nullptr || *wasValue != *nowValue)
                changed.appendMember(name, revisionPair(wasValue, nowValue));
        }
        if (changed.size() == 0)
            continue;
        auto entry = JsonValue::object();
        entry.appendMember("contentId", JsonValue::string(id));
        entry.appendMember("changed", std::move(changed));
        contents.append(std::move(entry));
    }
    diff.appendMember("contents", std::move(contents));
    return diff;
}

// ---- The write envelope ----------------------------------------------------------------------------

ControlWriteEnvelope::ControlWriteEnvelope(ControlOpContext& context)
    : before_(controlCaptureWorldVersions(context))
    , dirtyBefore_(controlProjectDirty(context))
{}

void ControlWriteEnvelope::addChange(std::string entity, JsonValue ref, JsonValue label, JsonValue fields)
{
    auto change = JsonValue::object();
    change.appendMember("entity", JsonValue::string(std::move(entity)));
    change.appendMember("ref", std::move(ref));
    change.appendMember("label", std::move(label));
    change.appendMember("fields", std::move(fields));
    changes_.append(std::move(change));
}

void ControlWriteEnvelope::addCreated(std::string entity, JsonValue ref, JsonValue label)
{
    auto item = JsonValue::object();
    item.appendMember("entity", JsonValue::string(std::move(entity)));
    item.appendMember("ref", std::move(ref));
    item.appendMember("label", std::move(label));
    created_.append(std::move(item));
}

void ControlWriteEnvelope::addRemoved(std::string entity, JsonValue ref, JsonValue label)
{
    auto item = JsonValue::object();
    item.appendMember("entity", JsonValue::string(std::move(entity)));
    item.appendMember("ref", std::move(ref));
    item.appendMember("label", std::move(label));
    removed_.append(std::move(item));
}

void ControlWriteEnvelope::addIdMapEntry(JsonValue entry)
{
    idMap_.append(std::move(entry));
}

void ControlWriteEnvelope::addWarning(std::string code, JsonValue subject, JsonValue data)
{
    auto warning = JsonValue::object();
    warning.appendMember("code", JsonValue::string(std::move(code)));
    warning.appendMember("subject", std::move(subject));
    warning.appendMember("data", std::move(data));
    warnings_.append(std::move(warning));
}

void ControlWriteEnvelope::addSideEffect(std::string name, JsonValue effect)
{
    if (!effect.isObject())
        effect = JsonValue::object();
    if (effect.find("revert") == nullptr)
        effect.appendMember("revert", JsonValue());
    sideEffects_.set(std::move(name), std::move(effect));
}

void ControlWriteEnvelope::touchContent(const std::string& contentId)
{
    if (std::find(touchedContents_.begin(), touchedContents_.end(), contentId) == touchedContents_.end())
        touchedContents_.push_back(contentId);
}

void ControlWriteEnvelope::touchPlacement(std::uint64_t placementId)
{
    if (std::find(touchedPlacements_.begin(), touchedPlacements_.end(), placementId) == touchedPlacements_.end())
        touchedPlacements_.push_back(placementId);
}

void ControlWriteEnvelope::setUndoable(std::string undoId, std::string description,
                                       std::vector<std::string> innerActions)
{
    undo_ = JsonValue::object();
    undo_.appendMember("undoable", JsonValue::boolean(true));
    undo_.appendMember("undoId", JsonValue::string(std::move(undoId)));
    undo_.appendMember("description", controlJsonText(description, 200));
    auto actions = JsonValue::array();
    for (auto& action : innerActions)
        actions.append(JsonValue::string(std::move(action)));
    undo_.appendMember("innerActions", std::move(actions));
    undo_.appendMember("steps", JsonValue::integer(1));
    undoSet_ = true;
    undoable_ = true;
    undoReason_.clear();
    revertNull_ = false;
}

void ControlWriteEnvelope::setNotUndoable(std::string reason, JsonValue revert)
{
    undoSet_ = true;
    undoable_ = false;
    undoReason_ = reason;
    revertNull_ = revert.isNull();
    undo_ = JsonValue::object();
    undo_.appendMember("undoable", JsonValue::boolean(false));
    undo_.appendMember("reason", JsonValue::string(std::move(reason)));
    undo_.appendMember("revert", std::move(revert));
}

JsonValue ControlWriteEnvelope::tokensJson(ControlOpContext& context, const ControlWorldVersions& after) const
{
    auto tokens = JsonValue::object();
    tokens.appendMember("arrangementRevision", JsonValue::string(after.arrangement));
    auto contents = JsonValue::array();
    for (const auto& id : touchedContents_)
        if (const auto* versions = findVersions(after, id))
            contents.append(versions->tokensJson());
    tokens.appendMember("contents", std::move(contents));
    auto placements = JsonValue::array();
    if (!touchedPlacements_.empty()) {
        for (const auto& placement : controlLivePlacements(context)) {
            if (std::find(touchedPlacements_.begin(), touchedPlacements_.end(), placement.placement.placementId)
                == touchedPlacements_.end())
                continue;
            auto entry = JsonValue::object();
            entry.appendMember("placementId", JsonValue::string(placement.placementId));
            entry.appendMember("geometryHash", JsonValue::string(controlGeometryHash(placement)));
            placements.append(std::move(entry));
        }
    }
    tokens.appendMember("placements", std::move(placements));
    return tokens;
}

JsonValue ControlWriteEnvelope::build(ControlOpContext& context, State state, JsonValue jobRef)
{
    // The state after the call, read back from the instance (section 10 rule 5).
    const auto after = controlCaptureWorldVersions(context);
    // A side effect is a change as well: a write that only had side effects is no no-op.
    const bool recorded = changes_.size() != 0 || created_.size() != 0 || removed_.size() != 0 || idMap_.size() != 0
                          || sideEffects_.size() != 0 || effective_;
    JsonValue diff = state == State::Predicted
                         ? emptyDiff()
                         : controlRevisionDiff(before_, after, limitDiff_ ? &touchedContents_ : nullptr);
    // Section 14.2 rule 3: a method hands its work to a job before it changes anything.
    jassert(state != State::HandedToJob || isEmptyDiff(diff));
    const bool noOp = state == State::Committed && !recorded && isEmptyDiff(diff);

    JsonValue undo;
    if (state == State::Committed) {
        if (noOp && !undoSet_) {
            undo = JsonValue::object();
            undo.appendMember("undoable", JsonValue::boolean(false));
            undo.appendMember("reason", JsonValue::string("no_op"));
            undo.appendMember("revert", JsonValue());
        } else {
            // With changes, or a no-op whose method set its own descriptor (section 15.3 rule 5).
            jassert(undo_.isObject());
            if (!undo_.isObject())
                setNotUndoable("no_upstream_undo");
            undo = undo_;
            // Neither undoable nor revertible: the client states it first (section 15.2 rule 2, section 18
            // rule 4); view state is exempt (section 15.2 rule 6), and so is a call that changed nothing.
            if (!noOp && !undoable_ && revertNull_ && undoReason_ != "not_persisted_state" && undoReason_ != "no_op"
                && !hasWarning(warnings_, "IRREVERSIBLE"))
                addWarning("IRREVERSIBLE");
        }
    }

    const bool dirtyAfter = state == State::Predicted ? dirtyBefore_ : controlProjectDirty(context);
    std::string uiNullReason;
    auto ui = controlUiSnapshot(context, uiNullReason);

    auto envelope = JsonValue::object();
    envelope.appendMember("applied", JsonValue::boolean(state == State::Committed));
    envelope.appendMember("noOp", JsonValue::boolean(noOp));
    envelope.appendMember("dryRun", JsonValue::boolean(state == State::Predicted));
    const bool listsChanges = state != State::HandedToJob && !noOp;
    envelope.appendMember("changes", listsChanges ? changes_ : JsonValue::array());
    envelope.appendMember("created", listsChanges ? created_ : JsonValue::array());
    envelope.appendMember("removed", listsChanges ? removed_ : JsonValue::array());
    envelope.appendMember("idMap", listsChanges ? idMap_ : JsonValue::array());
    envelope.appendMember("revisionDiff", std::move(diff));
    envelope.appendMember("tokens", tokensJson(context, after));
    envelope.appendMember("undo", std::move(undo));
    auto dirty = JsonValue::object();
    dirty.appendMember("before", JsonValue::boolean(dirtyBefore_));
    dirty.appendMember("after", JsonValue::boolean(dirtyAfter));
    envelope.appendMember("dirty", std::move(dirty));
    envelope.appendMember("ui", std::move(ui));
    envelope.appendMember("sideEffects", state == State::HandedToJob ? JsonValue::object() : sideEffects_);
    envelope.appendMember("warnings", warnings_);
    envelope.appendMember("job", state == State::HandedToJob ? std::move(jobRef) : JsonValue());
    if (!uiNullReason.empty()) {
        auto nullReasons = JsonValue::object();
        nullReasons.appendMember("ui", JsonValue::string(uiNullReason));
        envelope.appendMember("nullReasons", std::move(nullReasons));
    }
    return envelope;
}

JsonValue ControlWriteEnvelope::committed(ControlOpContext& context)
{
    return build(context, State::Committed, JsonValue());
}

JsonValue ControlWriteEnvelope::predicted(ControlOpContext& context)
{
    return build(context, State::Predicted, JsonValue());
}

JsonValue ControlWriteEnvelope::handedToJob(ControlOpContext& context, JsonValue jobRef)
{
    return build(context, State::HandedToJob, std::move(jobRef));
}

} // namespace OpenTune
