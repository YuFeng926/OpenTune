// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UiOps.h"

#include "OpsJson.h"
#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"
#include "Utils/AppPreferences.h"
#include "Utils/ScaleUiMapping.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

using DialogInfo = ControlUiBridge::DialogInfo;

/** Longest dialog title reported (methods/ui.getState.schema.json $defs.dialog.title). */
constexpr std::size_t kMaxTitleCodePoints = 256;
/** The preference that the first-run overlay was shown (prefs.get onboardingShown). */
constexpr const char* kOnboardingPreference = "onboardingShown";

// ---- Params ---------------------------------------------------------------------------------------

struct DismissParams
{
    /** target onboarding; target dialogs otherwise. */
    bool onboarding = false;
    bool confirm = false;
};

/** methods/ui.dismiss.schema.json $defs.params, on the connection thread. */
bool parseDismiss(const JsonValue& params, DismissParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (!reader.onlyMembers({"target", "confirm"})) {
        why = reader.why();
        return false;
    }
    const JsonValue* target = reader.member("target");
    if (target == nullptr || !target->isString() || (target->text() != "dialogs" && target->text() != "onboarding")) {
        why = "target must be dialogs or onboarding";
        return false;
    }
    out.onboarding = target->text() == "onboarding";
    if (const JsonValue* confirm = reader.member("confirm")) {
        if (out.onboarding) {
            why = "confirm is accepted with target dialogs only";
            return false;
        }
        if (!confirm->isBoolean()) {
            why = "confirm must be a boolean";
            return false;
        }
        out.confirm = confirm->asBoolean();
    }
    return true;
}

// ---- Pieces shared by the methods ----------------------------------------------------------------

Error noEditor()
{
    return controlOpError("UNSUPPORTED", "no_editor_window",
                          "ui.bridge describes the OpenTune editor window, and no editor exists");
}

/** The error for a bridge that cannot serve ui.bridge: no_editor_window without a bridge, capability_missing for a
    bridge that does not report its dialogs; nothing when the bridge serves. */
std::optional<Error> windowError(ControlOpContext& context)
{
    const auto* bridge = context.bridge();
    if (bridge == nullptr)
        return noEditor();
    if (!bridge->openDialogs())
        return controlOpError("UNSUPPORTED", "capability_missing", "the editor does not report its dialogs");
    return std::nullopt;
}

/** The dialogs that ui.getState lists: every modal window but the file choosers of the operating system. */
std::size_t listedCount(const std::vector<DialogInfo>& windows)
{
    std::size_t count = 0;
    for (const auto& window : windows)
        if (!window.systemFileChooser)
            ++count;
    return count;
}

/** The dialogs that closing from the top reaches, the topmost first: every one down to the first file chooser of the
    operating system, which the protocol cannot close and which the dialogs below wait for. */
std::vector<DialogInfo> closableFromTop(const std::vector<DialogInfo>& windows)
{
    std::vector<DialogInfo> out;
    for (auto it = windows.rbegin(); it != windows.rend() && !it->systemFileChooser; ++it)
        out.push_back(*it);
    return out;
}

bool sameDialog(const DialogInfo& a, const DialogInfo& b)
{
    return a.kind == b.kind && a.trackId == b.trackId && a.systemFileChooser == b.systemFileChooser
           && a.title == b.title;
}

/**
 * The checks of ui.dismiss before it closes anything: every listed dialog must be reachable from the top (no file
 * chooser of the operating system above one: BUSY(modal_open), since only the user can close that chooser), and a
 * dialog whose closing commits a value needs confirm (INVALID_ARGUMENT(dialog_commits_on_close)).
 */
std::optional<Error> checkClosable(const std::vector<DialogInfo>& windows, const std::vector<DialogInfo>& closable,
                                   bool confirm)
{
    if (closable.size() < listedCount(windows))
        return controlOpError("BUSY", "modal_open",
                              "a file chooser of the operating system is open above a dialog; only the user can "
                              "close it");
    if (!confirm)
        for (const auto& dialog : closable)
            if (dialog.closeCommits)
                return controlOpError("INVALID_ARGUMENT", "dialog_commits_on_close",
                                      "closing the " + dialog.kind
                                          + " dialog commits the value it shows; set confirm true only when the end "
                                            "user accepts that value");
    return std::nullopt;
}

bool validTrack(int trackId)
{
    return trackId >= 0 && trackId < OpenTuneAudioProcessor::MAX_TRACKS;
}

JsonValue trackRef(int trackId)
{
    auto ref = JsonValue::object();
    ref.appendMember("trackId", JsonValue::integer(trackId));
    return ref;
}

/** A track's colour as project.get reports it: "#RRGGBB". */
std::optional<std::string> trackColour(ControlOpContext& context, int trackId)
{
    const auto* arrangement = context.processor().getStandaloneArrangement();
    if (arrangement == nullptr || !validTrack(trackId))
        return std::nullopt;
    return "#" + arrangement->getTrackColour(trackId).toDisplayString(false).toStdString();
}

/** What closing dialogs did, for the write envelope. */
struct ClosedDialogs
{
    /** sideEffects.dialogClosed.dialogs: {kind, trackId}, the topmost first. */
    JsonValue dialogs = JsonValue::array();
    std::size_t count = 0;
    /** sideEffects.dialogCommitted of the first track-colour dialog that committed its colour; null when none did. */
    JsonValue committed;
};

/**
 * Closes the dialogs from the top as Escape does (ControlUiBridge::closeTopDialog()), and records in the envelope what
 * closing a track-colour dialog committed: the change of the track's colour, which has no undo entry in OpenTune
 * (ui_parity) and whose revert recipe is track.set where the instance reports tracks.v1 (PROTOCOL.md section 15.2
 * rule 2), and the warning DIALOG_CLOSE_COMMITTED. It stops early when the dialog on top is no longer the one the
 * checks saw, which only the reaction of a dialog it closed can bring about: the dialogs left open then are open in
 * the result's view, unchecked and untouched.
 */
std::optional<Error> closeDialogs(ControlOpContext& context, ControlUiBridge& bridge,
                                  const std::vector<DialogInfo>& closable, ControlWriteEnvelope& envelope,
                                  ClosedDialogs& out)
{
    const bool revertible = controlReportsCapability(context, "tracks.v1");
    for (const auto& dialog : closable) {
        const auto now = bridge.openDialogs();
        if (!now || now->empty() || !sameDialog(now->back(), dialog))
            break;
        const bool colourDialog = dialog.kind == "trackColour" && validTrack(dialog.trackId);
        const auto before = colourDialog ? trackColour(context, dialog.trackId) : std::nullopt;
        if (!bridge.closeTopDialog()) {
            const std::string detail = "the " + dialog.kind + " dialog on top did not close";
            return out.count == 0 ? controlOpError("INTERNAL", "", detail)
                                  : controlOpErrorAfterChange("INTERNAL", "", detail);
        }
        ++out.count;
        auto closed = JsonValue::object();
        closed.appendMember("kind", JsonValue::string(dialog.kind));
        closed.appendMember("trackId", validTrack(dialog.trackId) ? JsonValue::integer(dialog.trackId) : JsonValue());
        out.dialogs.append(std::move(closed));
        if (!colourDialog || !before)
            continue;
        // The track-colour dialog commits the colour it shows when it closes, also with Escape (PROTOCOL.md section
        // 15.4 rule 4): its content sets the track's colour and marks the project as changed.
        const auto after = trackColour(context, dialog.trackId).value_or(*before);
        auto revertParams = JsonValue::object();
        auto tracks = JsonValue::array();
        auto entry = trackRef(dialog.trackId);
        entry.appendMember("colour", JsonValue::string(*before));
        tracks.append(std::move(entry));
        revertParams.appendMember("tracks", std::move(tracks));
        const auto revert = revertible ? controlRevertRecipe("track.set", std::move(revertParams)) : JsonValue();
        if (after != *before) {
            auto pair = JsonValue::object();
            pair.appendMember("before", JsonValue::string(*before));
            pair.appendMember("after", JsonValue::string(after));
            pair.appendMember("unit", JsonValue());
            auto fields = JsonValue::object();
            fields.appendMember("colour", std::move(pair));
            envelope.addChange("track", trackRef(dialog.trackId),
                               JsonValue::string("Track " + std::to_string(dialog.trackId + 1)), std::move(fields));
        }
        if (out.committed.isNull()) {
            auto committed = JsonValue::object();
            committed.appendMember("kind", JsonValue::string(dialog.kind));
            committed.appendMember("trackId", JsonValue::integer(dialog.trackId));
            committed.appendMember("colourBefore", JsonValue::string(*before));
            committed.appendMember("colourAfter", JsonValue::string(after));
            committed.appendMember("revert", revert);
            out.committed = std::move(committed);
            // ui_parity: OpenTune records no undo entry for a colour change either.
            envelope.setNotUndoable("ui_parity", revert);
        }
        auto data = JsonValue::object();
        data.appendMember("kind", JsonValue::string(dialog.kind));
        envelope.addWarning("DIALOG_CLOSE_COMMITTED", trackRef(dialog.trackId), std::move(data));
        if (revert.isNull())
            envelope.addWarning("IRREVERSIBLE", trackRef(dialog.trackId));
        // The dialog's content marks the project as changed itself; the API marks it as well, so that the colour,
        // which the project file stores, never stays unsaved without the flag (section 15.5).
        controlMarkDirty(context);
    }
    return std::nullopt;
}

/** Records the dialogs closed in sideEffects (dialogClosed; dialogCommitted when a colour was committed). Dialogs are
    view state (section 15.2 rule 6): closing one that commits nothing is not_persisted_state with revert null. */
void recordClosed(ControlWriteEnvelope& envelope, ClosedDialogs& closed, bool otherUndoSet)
{
    if (closed.count == 0)
        return;
    auto effect = JsonValue::object();
    effect.appendMember("dialogs", std::move(closed.dialogs));
    effect.appendMember("revert", JsonValue());
    envelope.addSideEffect("dialogClosed", std::move(effect));
    if (!closed.committed.isNull())
        envelope.addSideEffect("dialogCommitted", std::move(closed.committed));
    else if (!otherUndoSet)
        envelope.setNotUndoable("not_persisted_state");
}

JsonValue preferenceRef()
{
    auto ref = JsonValue::object();
    ref.appendMember("preference", JsonValue::string(kOnboardingPreference));
    return ref;
}

// ---- ui.getState ---------------------------------------------------------------------------------

Result<JsonValue> runGetState(ControlOpContext& context, const ControlNoParams&)
{
    if (auto error = windowError(context))
        return *error;
    auto* bridge = context.bridge();
    const auto windows = *bridge->openDialogs();
    const auto scale = bridge->uiScale();
    std::string uiNullReason;
    auto ui = controlUiSnapshot(context, uiNullReason);
    if (ui.isNull() || !scale)
        return controlOpError("UNSUPPORTED", "capability_missing", "the editor does not report its view or its scale");

    // ControlUiBridge::queryBusy(): the editor's flags that controlCheckBusy() refuses writes with. As there, the
    // processor's export lock counts as an export, whoever holds it.
    const auto busyState = bridge->queryBusy().value_or(ControlUiBridge::BusyState{});
    auto busy = JsonValue::object();
    busy.appendMember("pointerDown", JsonValue::boolean(busyState.pointerDown));
    busy.appendMember("modalOpen", JsonValue::boolean(busyState.modalOpen));
    busy.appendMember("importInProgress", JsonValue::boolean(busyState.importRunning));
    busy.appendMember("exportInProgress",
                      JsonValue::boolean(busyState.exportRunning || context.processor().isExportLockHeld()));
    busy.appendMember("projectOperation", JsonValue::boolean(busyState.operationRunning));

    auto dialogs = JsonValue::array();
    for (const auto& window : windows) {
        // File choosers of the operating system are modal (busy.modalOpen) but not listed (schema description).
        if (window.systemFileChooser)
            continue;
        auto dialog = JsonValue::object();
        dialog.appendMember("kind", JsonValue::string(window.kind));
        // Titles come from OpenTune and may name files and projects: untrusted data (section 20.2).
        dialog.appendMember("title", controlJsonText(window.title, kMaxTitleCodePoints));
        dialog.appendMember("closeCommits", JsonValue::boolean(window.closeCommits));
        dialog.appendMember("trackId", validTrack(window.trackId) ? JsonValue::integer(window.trackId) : JsonValue());
        dialogs.append(std::move(dialog));
    }

    // AppPreferences: the preference the editor checks before it starts the overlay (shared.onboardingShown).
    auto onboarding = JsonValue::object();
    onboarding.appendMember(
        "shown", JsonValue::boolean(context.processor().getAppPreferences().getState().shared.onboardingShown));
    onboarding.appendMember("showing", JsonValue::boolean(bridge->onboardingShowing().value_or(false)));

    auto uiScale = JsonValue::object();
    uiScale.appendMember("root", JsonValue::integer(juce::jlimit(0, 11, scale->rootNote)));
    // uiScaleTypeToScale(): the UI's scale types 1 to 8 as OpenTune's Scale (Utils/ScaleUiMapping.h).
    uiScale.appendMember("scale", JsonValue::string(controlScaleName(uiScaleTypeToScale(scale->scaleType))));

    auto result = JsonValue::object();
    result.appendMember("busy", std::move(busy));
    result.appendMember("dialogs", std::move(dialogs));
    result.appendMember("onboarding", std::move(onboarding));
    result.appendMember("ui", std::move(ui));
    result.appendMember("uiScale", std::move(uiScale));
    return result;
}

// ---- ui.dismiss ----------------------------------------------------------------------------------

Result<JsonValue> dismissDialogs(ControlOpContext& context, ControlUiBridge& bridge, bool confirm)
{
    const auto windows = *bridge.openDialogs();
    const auto closable = closableFromTop(windows);
    if (auto refused = checkClosable(windows, closable, confirm))
        return *refused;
    ControlWriteEnvelope envelope(context);
    ClosedDialogs closed;
    if (auto failed = closeDialogs(context, bridge, closable, envelope, closed))
        return *failed;
    recordClosed(envelope, closed, false);
    // With no dialog open nothing was recorded: a no-op (undo reason no_op).
    return envelope.committed(context);
}

/**
 * target onboarding. Not showing: marks onboarding as shown, the preference the editor checks before it starts the
 * overlay, which prevents it (a user-global preference that no request resets: no_upstream_undo, IRREVERSIBLE).
 * Showing: closes the overlay as pressing Escape until it is gone does, which needs ui.onboardingClose: each Escape
 * closes the dialog on top, the overlay's own choice dialog finishes the overlay when it closes (its dismiss callback),
 * and with no dialog left the overlay takes the key itself. Then marks onboarding as shown where the overlay has not
 * (OpenTune marks it when the overlay shows its choice dialog).
 */
Result<JsonValue> dismissOnboarding(ControlOpContext& context, ControlUiBridge& bridge)
{
    auto& preferences = context.processor().getAppPreferences();
    const bool shownBefore = preferences.getState().shared.onboardingShown;
    const bool showing = bridge.onboardingShowing().value_or(false);
    ClosedDialogs closed;
    std::optional<ControlWriteEnvelope> envelope;
    if (showing) {
        if (!controlReportsCapability(context, "ui.onboardingClose"))
            return controlOpError("UNSUPPORTED", "capability_missing",
                                  "closing the onboarding overlay that is showing needs ui.onboardingClose");
        const auto windows = *bridge.openDialogs();
        const auto closable = closableFromTop(windows);
        // The dialogs above the overlay close first, as with Escape; the call has no confirm, so a dialog that would
        // commit a value refuses it.
        if (auto refused = checkClosable(windows, closable, false))
            return *refused;
        envelope.emplace(context);
        for (const auto& dialog : closable) {
            if (!bridge.onboardingShowing().value_or(false))
                break;
            const std::vector<DialogInfo> one{dialog};
            if (auto failed = closeDialogs(context, bridge, one, *envelope, closed))
                return *failed;
        }
        if (bridge.onboardingShowing().value_or(false) && !bridge.closeOnboarding()) {
            const std::string detail = "the onboarding overlay did not close";
            return closed.count == 0 ? controlOpError("INTERNAL", "", detail)
                                     : controlOpErrorAfterChange("INTERNAL", "", detail);
        }
        // The overlay closed: an effect on the view even when no dialog closed and the preference was already set.
        envelope->markEffective();
    } else {
        envelope.emplace(context);
    }
    // AppPreferences::setOnboardingShown(), as OpenTuneAudioProcessorEditor's overlay calls it when it shows.
    if (!preferences.getState().shared.onboardingShown)
        preferences.setOnboardingShown(true);
    const bool shownAfter = preferences.getState().shared.onboardingShown;
    const bool preferenceChanged = shownAfter != shownBefore;
    if (preferenceChanged) {
        auto pair = JsonValue::object();
        pair.appendMember("before", JsonValue::boolean(shownBefore));
        pair.appendMember("after", JsonValue::boolean(shownAfter));
        pair.appendMember("unit", JsonValue());
        auto fields = JsonValue::object();
        fields.appendMember(kOnboardingPreference, std::move(pair));
        envelope->addChange("preference", preferenceRef(), JsonValue::string("Onboarding shown"), std::move(fields));
        envelope->setNotUndoable("no_upstream_undo");
        envelope->addWarning("IRREVERSIBLE", preferenceRef());
    } else if (showing) {
        // Only the view changed: the overlay and the dialogs above it are view state (section 15.2 rule 6).
        envelope->setNotUndoable("not_persisted_state");
    }
    recordClosed(*envelope, closed, true);
    return envelope->committed(context);
}

Result<JsonValue> runDismiss(ControlOpContext& context, const DismissParams& params)
{
    if (auto error = windowError(context))
        return *error;
    auto* bridge = context.bridge();
    // Closing dialogs is the purpose of this method, so an open dialog does not refuse it; every other busy condition
    // does, as for every write (PROTOCOL.md section 13.2).
    if (auto busy = controlCheckBusy(context, ControlBusyScope::DialogDismiss))
        return *busy;
    return params.onboarding ? dismissOnboarding(context, *bridge) : dismissDialogs(context, *bridge, params.confirm);
}

} // namespace

void registerUiOps(ControlMethodTable& table)
{
    table.add(controlReadMethod<ControlNoParams>("ui.getState", controlParseNoParams, runGetState));
    table.add(controlWriteMethod<DismissParams>("ui.dismiss", parseDismiss, runDismiss));
}

} // namespace OpenTune
