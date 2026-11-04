// SPDX-License-Identifier: AGPL-3.0-or-later
#include "WorkspaceOps.h"

#include "OpsJson.h"
#include "OpsSupport.h"
#include "TransportOps.h"
#include "WriteEnvelope.h"

#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"

#include <optional>
#include <string>
#include <utility>

namespace OpenTune {

namespace {

constexpr const char* kArrangement = "arrangement";
constexpr const char* kPianoRoll = "pianoRoll";

struct WorkspaceParams
{
    /** True for the arrangement view, false for the piano roll. */
    bool arrangement = true;
};

/** methods/ui.setWorkspace.schema.json $defs.params, on the connection thread: workspace is required and the only
    member. */
bool parseSetWorkspace(const JsonValue& params, WorkspaceParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (!reader.onlyMembers({"workspace"})) {
        why = reader.why();
        return false;
    }
    const JsonValue* workspace = reader.member("workspace");
    if (workspace == nullptr || !workspace->isString()
        || (workspace->text() != kArrangement && workspace->text() != kPianoRoll)) {
        reader.fail("workspace must be \"arrangement\" or \"pianoRoll\"");
        why = reader.why();
        return false;
    }
    out.arrangement = workspace->text() == kArrangement;
    return true;
}

JsonValue workspaceRef()
{
    auto ref = JsonValue::object();
    ref.appendMember("singleton", JsonValue::string("workspace"));
    return ref;
}

/** The name the wire uses for a workspace; any name the bridge reports other than the piano roll is the arrangement,
    as the UI snapshot of every write reports it. */
const char* workspaceName(const std::string& reported)
{
    return reported == kPianoRoll ? kPianoRoll : kArrangement;
}

/** ui.setWorkspace back to the workspace the window showed before the call, only where the instance reports
    ui.workspace (PROTOCOL.md section 15.2 rule 2). */
JsonValue revertRecipe(ControlOpContext& context, const char* before)
{
    if (!controlReportsCapability(context, "ui.workspace"))
        return JsonValue();
    auto params = JsonValue::object();
    params.appendMember("workspace", JsonValue::string(before));
    return controlRevertRecipe("ui.setWorkspace", std::move(params));
}

Result<JsonValue> runSetWorkspace(ControlOpContext& context, const WorkspaceParams& params)
{
    auto* bridge = context.bridge();
    if (bridge == nullptr)
        return controlOpError("UNSUPPORTED", "no_editor_window",
                              "ui.workspace describes the OpenTune editor window, and no editor exists");
    const auto view = bridge->viewState();
    if (!view)
        return controlOpError("UNSUPPORTED", "capability_missing", "the editor does not report its workspace");
    if (auto busy = controlCheckBusy(context))
        return *busy;

    const char* const before = workspaceName(view->workspace);
    const char* const target = params.arrangement ? kArrangement : kPianoRoll;
    ControlWriteEnvelope envelope(context);
    if (std::string(before) == target)
        // The window shows it already: a no-op, with nothing recorded (the envelope says no_op by itself).
        return envelope.committed(context);

    // The tempo before the switch: the view switch moves the keyboard focus, and a transport bar whose BPM field loses
    // it then rewrites the tempo with the one decimal the field shows (see ControlEditorBridgeWorkspace.cpp).
    const double bpmBefore = context.processor().getBpm();
    // Both failures below come after the editor was asked, and the focus that the BPM field lost may already have
    // rewritten the tempo: controlViewSwitchError() says "nothing changed" only while the tempo is what it was.
    if (!bridge->setWorkspace(params.arrangement))
        return controlViewSwitchError(context, bpmBefore, "UNSUPPORTED", "capability_missing",
                                      "the editor does not switch the workspace");
    const double bpmAfter = context.processor().getBpm();
    const auto after = bridge->viewState();
    if (!after || std::string(workspaceName(after->workspace)) != target)
        return controlViewSwitchError(context, bpmBefore, "INTERNAL", "", "the editor did not switch the workspace");

    auto pair = JsonValue::object();
    pair.appendMember("before", JsonValue::string(before));
    pair.appendMember("after", JsonValue::string(target));
    pair.appendMember("unit", JsonValue());
    auto fields = JsonValue::object();
    fields.appendMember("workspace", std::move(pair));
    envelope.addChange("ui", workspaceRef(), JsonValue::string("Workspace"), std::move(fields));

    // The same side effect that ui.setSelection reports when it opens the editor.
    auto effect = JsonValue::object();
    effect.appendMember("from", JsonValue::string(before));
    effect.appendMember("to", JsonValue::string(target));
    effect.appendMember("revert", JsonValue());
    envelope.addSideEffect("workspaceSwitched", std::move(effect));
    // Upstream behaviour that the call reports as it happened: the project's tempo changed (the change of the singleton
    // tempo, as transport.setTempo reports it, and the side effect tempoRounded with the way back).
    if (controlBpmDiffers(bpmBefore, bpmAfter))
        controlReportTempoRounded(context, envelope, bpmBefore, bpmAfter);

    envelope.setNotUndoable("not_persisted_state", revertRecipe(context, before));
    return envelope.committed(context);
}

} // namespace

void registerWorkspaceOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<WorkspaceParams>("ui.setWorkspace", parseSetWorkspace, runSetWorkspace));
}

} // namespace OpenTune
