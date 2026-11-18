// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UndoOps.h"

#include "EditSupport.h"
#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Control/ControlUiBridge.h"
#include "Control/Edit/ControlOriginAction.h"
#include "PluginProcessor.h"

#include <algorithm>

namespace OpenTune {

namespace {

struct StepParams
{
    std::optional<std::string> expectedUndoId;
    bool force = false;
};

/** An undoId has the form u:<8 lowercase hexadecimal digits>:<seq from 1>. */
bool isUndoId(const std::string& text)
{
    if (text.size() < 12 || text.compare(0, 2, "u:") != 0 || text[10] != ':' || text[11] == '0')
        return false;
    const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    return std::all_of(text.begin() + 2, text.begin() + 10, hex) && std::all_of(text.begin() + 11, text.end(), digit);
}

/** With force true expectedUndoId is neither parsed nor checked; without it, it is required. */
bool parseStepParams(const JsonValue& params, StepParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (!reader.onlyMembers({"expectedUndoId", "force"})) {
        why = reader.why();
        return false;
    }
    if (const JsonValue* force = reader.member("force")) {
        if (!force->isBoolean()) {
            why = "force must be a boolean";
            return false;
        }
        out.force = force->asBoolean();
    }
    if (out.force)
        return true;
    const JsonValue* expected = reader.member("expectedUndoId");
    if (expected == nullptr || !expected->isString() || !isUndoId(expected->text())) {
        why = "expectedUndoId is required without force and must be an undoId";
        return false;
    }
    out.expectedUndoId = expected->text();
    return true;
}

JsonValue entryJson(const ControlHistory::Entry& entry)
{
    auto json = JsonValue::object();
    json.appendMember("undoId", entry.undoId ? JsonValue::string(*entry.undoId) : JsonValue());
    json.appendMember("description", controlJsonText(entry.description.toStdString(), 200));
    json.appendMember("origin", JsonValue::string(entry.control ? "control" : "ui"));
    json.appendMember("unsafe", JsonValue::boolean(entry.unsafe));
    return json;
}

Result<std::shared_ptr<ControlHistory>> historyOf(ControlOpContext& context)
{
    auto history = context.history();
    if (history == nullptr)
        return controlOpError("INTERNAL", "", "the undo history of the control API is not available");
    return history;
}

Result<JsonValue> runGetHistory(ControlOpContext& context, const ControlNoParams&)
{
    auto history = historyOf(context);
    if (!history.ok())
        return history.error();
    // UndoManager::peekUndo() / peekRedo() / size() / cursor(): the read hook on OpenTune's history.
    auto& manager = context.processor().getUndoManager();
    const auto epoch = controlContentEpoch(context);
    auto json = JsonValue::object();
    json.appendMember("canUndo", JsonValue::boolean(manager.canUndo()));
    json.appendMember("canRedo", JsonValue::boolean(manager.canRedo()));
    json.appendMember("undoCount", JsonValue::integer(manager.cursor()));
    json.appendMember("redoCount", JsonValue::integer(manager.size() - manager.cursor()));
    const auto* undoTop = manager.peekUndo();
    const auto* redoTop = manager.peekRedo();
    json.appendMember("undoTop", undoTop != nullptr ? entryJson(history.value()->describe(manager, *undoTop,
                                                                                          manager.cursor() - 1, epoch))
                                                    : JsonValue());
    json.appendMember("redoTop", redoTop != nullptr
                                     ? entryJson(history.value()->describe(manager, *redoTop, manager.cursor(), epoch))
                                     : JsonValue());
    auto result = JsonValue::object();
    result.appendMember("asOf", controlAsOf(controlArrangementRevision(context), {}));
    result.appendMember("derived", JsonValue::array());
    result.appendMember("history", std::move(json));
    return result;
}

bool isEmptyDiff(const ControlWorldVersions& before, const ControlWorldVersions& after)
{
    const auto diff = controlRevisionDiff(before, after);
    const JsonValue* arrangement = diff.find("arrangement");
    const JsonValue* trackState = diff.find("trackState");
    const JsonValue* contents = diff.find("contents");
    return arrangement != nullptr && arrangement->isNull() && trackState != nullptr && trackState->isNull()
           && contents != nullptr && contents->size() == 0;
}

/** Runs edit.undo (undo true) or edit.redo. */
Result<JsonValue> runStep(ControlOpContext& context, const StepParams& params, bool undo)
{
    const char* const what = undo ? "undo" : "redo";
    if (auto busy = controlCheckBusy(context))
        return *busy;
    auto history = historyOf(context);
    if (!history.ok())
        return history.error();
    auto* bridge = context.bridge();
    if (bridge == nullptr)
        return controlOpError("UNSUPPORTED", "no_editor_window", "undo and redo run through the editor command");

    auto& manager = context.processor().getUndoManager();
    std::optional<ControlHistory::Entry> entry;
    std::optional<ControlEditState> before;
    std::optional<ControlWriteEnvelope> envelope;
    std::optional<Error> refusal;
    int index = 0;
    std::uint64_t epoch = 0;
    UndoAction* selectedTop = nullptr;
    auto* applied = bridge->performUndoRedo(undo, [&](UndoAction* top) {
        if (top == nullptr) {
            refusal = controlOpError("UNDO_CONFLICT", "history_empty", std::string("there is nothing to ") + what);
            return false;
        }
        epoch = controlContentEpoch(context);
        // The entry keeps its position in the history: after an undo it is the top of the redo history.
        index = undo ? manager.cursor() - 1 : manager.cursor();
        entry.emplace(history.value()->describe(manager, *top, index, epoch));
        if (!params.force && (!entry->undoId || *entry->undoId != *params.expectedUndoId)) {
            refusal = controlOpError("UNDO_CONFLICT", "top_mismatch",
                                     std::string("the top of the ") + what
                                         + " history is not the expected entry; the history changed since it was read");
            return false;
        }
        selectedTop = top;
        before.emplace(controlCaptureEditState(context));
        envelope.emplace(context);
        return true;
    });
    if (applied == nullptr) {
        if (refusal)
            return *refusal;
        return controlOpError("UNDO_CONFLICT", "history_empty", std::string("there is nothing to ") + what);
    }
    jassert(applied == selectedTop);
    jassert((undo ? manager.peekRedo() : manager.peekUndo()) == applied);
    // The step moved the cursor, no entry: the history as it is now is the one describe() checked.
    history.value()->noteOwnStep(manager);
    // An entry of the UI applied with force gets an undoId now.
    const std::string undoId =
        entry->undoId ? *entry->undoId
                      : history.value()->assignUndoId(manager, *applied, index, epoch, context.instanceShort());
    const auto after = controlCaptureEditState(context);
    controlDescribeEffect(context, *before, after, *envelope);
    const bool emptyDiff = isEmptyDiff(before->versions, after.versions);
    if (emptyDiff)
        envelope->addWarning("UNDO_NO_EFFECT");
    auto revertParams = JsonValue::object();
    revertParams.appendMember("expectedUndoId", JsonValue::string(undoId));
    envelope->setNotUndoable("ui_parity",
                             controlRevertRecipe(undo ? "edit.redo" : "edit.undo", std::move(revertParams)));
    auto result = envelope->committed(context);
    result.appendMember("entry", entryJson(history.value()->describe(manager, *applied, index, epoch)));
    return result;
}

Result<JsonValue> runUndo(ControlOpContext& context, const StepParams& params)
{
    return runStep(context, params, true);
}

Result<JsonValue> runRedo(ControlOpContext& context, const StepParams& params)
{
    return runStep(context, params, false);
}

} // namespace

void registerUndoOps(ControlMethodTable& table)
{
    table.add(controlReadMethod<ControlNoParams>("edit.getHistory", controlParseNoParams, runGetHistory));
    table.add(controlWriteMethod<StepParams>("edit.undo", parseStepParams, runUndo));
    table.add(controlWriteMethod<StepParams>("edit.redo", parseStepParams, runRedo));
}

} // namespace OpenTune
