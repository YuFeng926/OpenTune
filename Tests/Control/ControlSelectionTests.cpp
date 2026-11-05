// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scenarios of ui.setSelection against the real processor without a window or an audio device. The test bridge
// selects as the editor does when the user clicks a clip (Source/Standalone/ControlEditorBridgeSelection.cpp, through
// the arrangement view's hook): it commits the selection to the arrangement; shows the clip in its piano roll, which in
// the openDyne scheme generates the notes of a clip whose notes were never generated, through the processor's own
// OpenTuneAudioProcessor::generateNotesOnlyByContentKey (PianoRollComponent::ensureOpenDyneNotesIfNeeded); shows the
// clip's key as the scale of the UI; and switches to the piano roll for openEditor. Contents get synthetic F0
// (ControlContentFixtures.h).
// Registered with the other processor scenarios in ControlProcessorTests.cpp.
//
// What SelectionMatrix proves: the test bridge and the prediction in SelectionOps.cpp follow the same rules, so a
// passing matrix shows that the two copies agree, not that the prediction agrees with the real editor. Drift of the
// real editor is caught by the mirror hashes (Source/Control/mirror-map.json) and by checks in the real Standalone.

#include "ControlContentFixtures.h"
#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneContentRepository.h"
#include "Control/ControlDispatcher.h"
#include "Control/Ops/OpsRegistry.h"
#include "PluginProcessor.h"
#include "Utils/AppPreferences.h"
#include "Utils/AudioEditingScheme.h"
#include "Utils/NoteGeneratorTypes.h"
#include "Utils/ScaleUiMapping.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace OpenTune {
namespace ControlTest {

namespace {

/** Bound of one vector's exchange while this thread pumps messages. */
constexpr int kVectorExchangeMs = 30000;

using UiScale = ControlUiBridge::UiScale;

JsonValue jsonOf(const std::string& text)
{
    auto parsed = parseJson(text);
    CONTROL_CHECK(parsed.ok);
    return parsed.value;
}

std::string quoted(const std::string& text)
{
    return toJsonString(JsonValue::string(text));
}

/** The test bridge of the editor's selection (see the top of this file). Message thread only. */
class SelectionBridge : public TestUiBridge
{
public:
    explicit SelectionBridge(OpenTuneAudioProcessor& processor) : processor_(processor)
    {
        view.workspace = "arrangement";
        // The keyboard focus is in the transport bar's BPM field (TestUiBridge::commitBpmFieldOnViewSwitch()): the
        // state of a window that has just started in the foreground. The scenarios of this file rely on it; the
        // vector runner sets the focus from the vector's given.keyboardFocus (runSelectionVector()).
        focusTempoField(processor);
    }

    bool canSelect() const override { return selects; }

    bool select(int trackId, std::uint64_t placementId, bool openEditor) override
    {
        ++selectCalls;
        if (breaksOffAfterFocusMove) {
            // The double click moved the keyboard focus (and the BPM field committed) and then the call broke off.
            commitBpmFieldOnViewSwitch();
            return false;
        }
        auto* arrangement = processor_.getStandaloneArrangement();
        // StandaloneArrangement::selectPlacement(): the active track and its selected clip, as the view commits a
        // click (commitPlacementSelection) and the editor commits a track with no clip
        // (applyPlacementSelectionContext).
        if (arrangement == nullptr || !arrangement->selectPlacement(trackId, placementId))
            return false;
        if (placementId == 0) {
            showInPianoRoll(0, ContentKey{});
            return true;
        }
        StandaloneArrangement::Placement placement;
        if (!arrangement->getPlacementById(trackId, placementId, placement))
            return false;
        showInPianoRoll(placementId, placement.contentKey);
        scale = scaleOf(placement.contentKey);
        // placementDoubleClicked(): the piano roll replaces the arrangement only when the arrangement is shown, and
        // only that switch moves the keyboard focus.
        if (openEditor && view.workspace != "pianoRoll") {
            view.workspace = "pianoRoll";
            commitBpmFieldOnViewSwitch();
        }
        return true;
    }

    std::optional<UiScale> scaleForSelection(int trackId, std::uint64_t placementId) const override
    {
        StandaloneArrangement::Placement placement;
        const auto* arrangement = processor_.getStandaloneArrangement();
        if (arrangement == nullptr || !arrangement->getPlacementById(trackId, placementId, placement))
            return std::nullopt;
        return scaleOf(placement.contentKey);
    }

    /** The selected clip of the active track, as the editor's bridge reports it. */
    std::optional<std::vector<std::uint64_t>> selectedPlacementIds() const override
    {
        std::vector<std::uint64_t> ids;
        if (const auto* arrangement = processor_.getStandaloneArrangement())
            if (const auto id = arrangement->getSelectedPlacementId(arrangement->getActiveTrackId()); id != 0)
                ids.push_back(id);
        return ids;
    }

    bool currentNoteGenerationParams(NoteGeneratorParams& out) const override
    {
        out = generatorParams;
        return true;
    }

    /** The editor's syncSharedAppPreferences() after a preference changed: when the editing scheme became the
        notes-primary one, the piano roll that shows a clip whose notes were never generated generates them
        (PianoRollComponent::ensureOpenDyneNotesIfNeeded). The caller of the write marks the project as changed. */
    void onPreferencesChanged() override
    {
        const bool notesPrimary = openDyne();
        const bool becameNotesPrimary = notesPrimary && !appliedNotesPrimary_;
        appliedNotesPrimary_ = notesPrimary;
        if (becameNotesPrimary && view.pianoRollPlacementId != 0)
            generateNotesOfPianoRoll();
    }

    /** The scheme the views run with follows the preference without a notification, as when a test sets the
        preference before it builds its project (the editor applies the scheme of a loaded project the same way). */
    void syncAppliedScheme() { appliedNotesPrimary_ = openDyne(); }

    /** canSelect(): false is a bridge without the selection. */
    bool selects = true;
    int selectCalls = 0;
    /** select() commits the BPM field, as the focus that the view switch moved does, and then fails, taking nothing
        else: an editor whose call breaks off after the focus left the field. */
    bool breaksOffAfterFocusMove = false;
    /** The parameters of the piano roll's note generation (getCurrentAutoTuneParams). */
    NoteGeneratorParams generatorParams;

private:
    bool openDyne() const
    {
        return AudioEditingScheme::usesNotesPrimaryScheme(
            processor_.getAppPreferences().getState().shared.audioEditingScheme);
    }

    /** PianoRollComponent::setEditedContentAndProjection(): nothing when the piano roll shows the clip already;
        otherwise ensureOpenDyneNotesIfNeeded(), after which the editor marks the project as changed. */
    void showInPianoRoll(std::uint64_t placementId, const ContentKey& key)
    {
        if (placementId == view.pianoRollPlacementId)
            return;
        view.pianoRollPlacementId = placementId;
        if (placementId == 0 || !openDyne())
            return;
        if (generateNotesIfNeeded(key))
            markDirty();
    }

    /** Generates the notes of the clip the piano roll shows (the scheme just became the notes-primary one). */
    void generateNotesOfPianoRoll()
    {
        StandaloneArrangement::Placement placement;
        for (int trackId = 0; trackId < OpenTuneAudioProcessor::MAX_TRACKS; ++trackId)
            if (processor_.getPlacementById(trackId, view.pianoRollPlacementId, placement)) {
                generateNotesIfNeeded(placement.contentKey);
                return;
            }
    }

    /** PianoRollComponent::ensureOpenDyneNotesIfNeeded(): the notes of a content whose notes were never generated and
        whose F0 exists; true when it generated them. */
    bool generateNotesIfNeeded(const ContentKey& key)
    {
        const auto snapshot = processor_.getContentSnapshot(key);
        if (snapshot == nullptr || snapshot->noteTopologyInitialized || snapshot->pitchCurve == nullptr
            || snapshot->pitchCurve->getOriginalF0().empty())
            return false;
        return processor_.generateNotesOnlyByContentKey(key, generatorParams);
    }

    /** OpenTuneAudioProcessorEditor::resolveScaleForPlacementContent(): the content's key, or C major. */
    UiScale scaleOf(const ContentKey& key) const
    {
        DetectedKey detected;
        if (const auto snapshot = processor_.getContentSnapshot(key);
            snapshot != nullptr && snapshot->detectedKey.origin != Origin::Unset)
            detected = snapshot->detectedKey;
        UiScale out;
        out.rootNote = static_cast<int>(detected.root);
        out.scaleType = scaleToUiScaleType(detected.scale);
        return out;
    }

    OpenTuneAudioProcessor& processor_;
    /** The editing scheme the views run with: notes-primary or not (OpenTuneAudioProcessorEditor's applied scheme). */
    bool appliedNotesPrimary_ = false;
};

/** A started processor with the selection bridge registered and an open session. */
struct SelectionFixture
{
    SelectionFixture()
    {
        processor = startedProcessor();
        if (!service().isRunning())
            return;
        bridge = std::make_unique<SelectionBridge>(*processor);
        service().setUiBridge(bridge.get());
        if (!api.open(service()))
            return;
        instanceShort = memberOf(api.discovery(), "instanceId").text().substr(0, 8);
        ready = true;
    }
    ~SelectionFixture()
    {
        if (processor != nullptr)
            service().setUiBridge(nullptr);
    }

    SelectionFixture(const SelectionFixture&) = delete;
    SelectionFixture& operator=(const SelectionFixture&) = delete;

    ControlService& service() { return processor->getControlService(); }
    StandaloneArrangement& arrangement() { return *processor->getStandaloneArrangement(); }

    /** The editing scheme and experimental features, which persist in the data directory between processors. */
    void setPreferences(bool openDyne, bool experimental)
    {
        auto& preferences = processor->getAppPreferences();
        preferences.setAudioEditingScheme(openDyne ? AudioEditingScheme::Scheme::NotesPrimary
                                                   : AudioEditingScheme::Scheme::CorrectedF0Primary);
        preferences.setExperimentalFeaturesEnabled(experimental);
        bridge->syncAppliedScheme();
    }

    /** Track 0 active with no clip selected on any track, as in a new project: an import selects the imported clip and
        makes its track the active one (OpenTuneAudioProcessor::commitPreparedImportAsPlacement). */
    void clearSelection()
    {
        for (int trackId = 0; trackId < arrangement().getNumTracks(); ++trackId)
            arrangement().setSelectedPlacementIndex(trackId, -1);
        arrangement().setActiveTrack(0);
    }

    std::string unknownPlacementId()
    {
        return "p:" + instanceShort + ":"
               + std::to_string(processor->getStandaloneContentRepository()->getContentEpoch()) + ":4294967295";
    }

    std::unique_ptr<OpenTuneAudioProcessor> processor;
    std::unique_ptr<SelectionBridge> bridge;
    ApiSession api;
    std::string instanceShort;
    bool ready = false;
};

ClipSpec clipSpec(const std::string& name, int trackId, bool withNotes)
{
    ClipSpec spec;
    spec.name = name;
    spec.trackId = trackId;
    spec.startTimelineSec = 2.0;
    spec.durationSec = 6.0;
    if (withNotes)
        spec.notes = {{0.5, 1.5, 64.0}, {2.0, 3.0, 69.0}};
    return spec;
}

JsonValue selectClip(const std::string& placementId, bool openEditor = false, bool dryRun = false)
{
    auto params = JsonValue::object();
    params.appendMember("placementId", JsonValue::string(placementId));
    if (openEditor)
        params.appendMember("openEditor", JsonValue::boolean(true));
    if (dryRun)
        params.appendMember("dryRun", JsonValue::boolean(true));
    return params;
}

JsonValue selectTrack(int trackId, bool dryRun = false)
{
    auto params = JsonValue::object();
    params.appendMember("trackId", JsonValue::integer(trackId));
    if (dryRun)
        params.appendMember("dryRun", JsonValue::boolean(true));
    return params;
}

/** The fields of the one change of a result; null when it has not exactly one. */
JsonValue fieldsOf(const JsonValue& result)
{
    const auto& changes = memberOf(result, "changes");
    return changes.size() == 1 ? memberOf(changes.at(0), "fields") : JsonValue();
}

/** The state that a selection may change, field by field: the arrangement's selection on every track, the editor's
    view, scale and dirty flag as the bridge holds them, the bridge's calls, and every content's revisions, notes and
    analysis states. */
JsonValue captureState(SelectionFixture& f, const std::vector<ContentKey>& contents)
{
    auto state = JsonValue::object();
    state.appendMember("activeTrack", JsonValue::integer(f.arrangement().getActiveTrackId()));
    auto selected = JsonValue::array();
    for (int trackId = 0; trackId < f.arrangement().getNumTracks(); ++trackId)
        selected.append(JsonValue::integer(static_cast<std::int64_t>(f.arrangement().getSelectedPlacementId(trackId))));
    state.appendMember("selected", std::move(selected));
    state.appendMember("workspace", JsonValue::string(f.bridge->view.workspace));
    state.appendMember("pianoRoll", JsonValue::integer(static_cast<std::int64_t>(f.bridge->view.pianoRollPlacementId)));
    state.appendMember("scaleRoot", JsonValue::integer(f.bridge->scale ? f.bridge->scale->rootNote : -1));
    state.appendMember("scaleType", JsonValue::integer(f.bridge->scale ? f.bridge->scale->scaleType : -1));
    state.appendMember("dirty", JsonValue::boolean(f.bridge->project.dirty));
    state.appendMember("dirtyMarks", JsonValue::integer(f.bridge->dirtyMarks));
    state.appendMember("selectCalls", JsonValue::integer(f.bridge->selectCalls));
    auto items = JsonValue::array();
    for (const auto& key : contents) {
        const auto snapshot = f.processor->getContentSnapshot(key);
        auto item = JsonValue::object();
        if (snapshot != nullptr) {
            item.appendMember("notesRevision", JsonValue::integer(static_cast<std::int64_t>(snapshot->notesRevision)));
            item.appendMember("pitchRevision", JsonValue::integer(static_cast<std::int64_t>(snapshot->pitchRevision)));
            item.appendMember("audioRevision", JsonValue::integer(static_cast<std::int64_t>(snapshot->audioRevision)));
            item.appendMember("notes", JsonValue::integer(static_cast<std::int64_t>(snapshot->notes.size())));
            item.appendMember("topology", JsonValue::boolean(snapshot->noteTopologyInitialized));
            item.appendMember("f0", JsonValue::integer(static_cast<int>(snapshot->originalF0State)));
            item.appendMember("referenceStatus",
                              JsonValue::integer(static_cast<int>(snapshot->referenceFeatures.status)));
            item.appendMember("referenceRevision", JsonValue::integer(snapshot->referenceFeatures.analysisRevision));
            item.appendMember("keyRoot", JsonValue::integer(static_cast<int>(snapshot->detectedKey.root)));
            item.appendMember("keyScale", JsonValue::integer(static_cast<int>(snapshot->detectedKey.scale)));
        }
        items.append(std::move(item));
    }
    state.appendMember("contents", std::move(items));
    // What the reads report: the revision tokens of every content and the arrangement.
    state.appendMember("asOf", memberOf(f.api.result("project.get"), "asOf"));
    return state;
}

bool checkEqual(const JsonValue& expected, const JsonValue& actual, const std::string& what)
{
    if (CONTROL_CHECK(VectorMatcher::equal(expected, actual)))
        return true;
    note(what + ": expected " + toJsonString(expected) + ", got " + toJsonString(actual));
    return false;
}

JsonValue revertTo(const std::string& member, JsonValue value)
{
    auto params = JsonValue::object();
    params.appendMember(member, std::move(value));
    auto recipe = JsonValue::object();
    recipe.appendMember("method", JsonValue::string("ui.setSelection"));
    recipe.appendMember("params", std::move(params));
    return recipe;
}

JsonValue scalePair(int rootBefore, const char* scaleBefore, int rootAfter, const char* scaleAfter)
{
    const auto scale = [](int root, const char* name) {
        auto json = JsonValue::object();
        json.appendMember("root", JsonValue::integer(root));
        json.appendMember("scale", JsonValue::string(name));
        return json;
    };
    auto effect = JsonValue::object();
    effect.appendMember("before", scale(rootBefore, scaleBefore));
    effect.appendMember("after", scale(rootAfter, scaleAfter));
    effect.appendMember("revert", JsonValue());
    return effect;
}

// ---- Scenario: the method -------------------------------------------------------------------------------

/** The capability and its methods; without an editor, without the selection, busy. */
void checkCapabilityAndRefusals(SelectionFixture& f, const std::string& placementA)
{
    const auto reported = f.service().dispatcher()->capabilities();
    CONTROL_CHECK(std::find(reported.begin(), reported.end(), "ui.bridge") != reported.end());
    for (const char* method : {"ui.getState", "ui.dismiss", "ui.setSelection"})
        CONTROL_CHECK(f.service().dispatcher()->implements(method));

    // The params of the schema (closed; exactly one of placementId and trackId; openEditor with placementId only).
    const auto p = quoted(placementA);
    for (const std::string params :
         {std::string(R"({})"), R"({"placementId":)" + p + R"(,"trackId":0})",
          std::string(R"({"trackId":0,"openEditor":true})"), std::string(R"({"trackId":-1})"),
          std::string(R"({"trackId":1.5})"), std::string(R"({"trackId":"0"})"),
          std::string(R"({"placementId":"clip"})"), std::string(R"({"placementId":12})"),
          R"({"placementId":)" + p + R"(,"openEditor":"yes"})", R"({"placementId":)" + p + R"(,"dryRun":1})",
          R"({"placementId":)" + p + R"(,"select":true})"})
        CONTROL_CHECK(f.api.fails("ui.setSelection", jsonOf(params), "INVALID_ARGUMENT", "invalid_params"));

    // Handles that name nothing: an unknown clip, a clip of another instance, a track beyond the slots.
    CONTROL_CHECK(f.api.fails("ui.setSelection", selectClip(f.unknownPlacementId()), "NOT_FOUND", "unknown_handle"));
    const auto stale = "p:" + fixtureOtherInstanceShort(f.instanceShort) + ":"
                       + std::to_string(f.processor->getStandaloneContentRepository()->getContentEpoch()) + ":1";
    CONTROL_CHECK(f.api.fails("ui.setSelection", selectClip(stale), "NOT_FOUND", "stale_handle"));
    CONTROL_CHECK(
        f.api.fails("ui.setSelection", selectTrack(f.arrangement().getNumTracks()), "NOT_FOUND", "unknown_handle"));

    // Busy: refused before anything, also as dryRun.
    f.bridge->busy.pointerDown = true;
    CONTROL_CHECK(f.api.fails("ui.setSelection", selectClip(placementA), "BUSY", "pointer_down"));
    CONTROL_CHECK(f.api.fails("ui.setSelection", selectClip(placementA, false, true), "BUSY", "pointer_down"));
    f.bridge->busy.pointerDown = false;
    f.bridge->busy.modalOpen = true;
    CONTROL_CHECK(f.api.fails("ui.setSelection", selectTrack(0), "BUSY", "modal_open"));
    f.bridge->busy.modalOpen = false;

    // A bridge without the selection, and no editor at all.
    f.bridge->selects = false;
    CONTROL_CHECK(f.api.fails("ui.setSelection", selectClip(placementA), "UNSUPPORTED", "capability_missing"));
    f.bridge->selects = true;
    f.service().setUiBridge(nullptr);
    CONTROL_CHECK(f.api.fails("ui.setSelection", selectClip(placementA), "UNSUPPORTED", "no_editor_window"));
    f.service().setUiBridge(f.bridge.get());
    CONTROL_CHECK(f.bridge->selectCalls == 0);
}

void scenarioSelectionImpl()
{
    // The interface's defaults report "not supported".
    ControlUiBridge defaults;
    CONTROL_CHECK(!defaults.canSelect() && !defaults.select(0, 1, false) && !defaults.scaleForSelection(0, 1));

    SelectionFixture f;
    if (!CONTROL_CHECK(f.ready))
        return;
    f.setPreferences(false, false);
    // Clip a on track 0 in D minor, clip b on track 1 without a key (the editor shows C major for it).
    auto specA = clipSpec("a", 0, true);
    DetectedKey dMinor;
    dMinor.root = Key::D;
    dMinor.scale = Scale::Minor;
    dMinor.origin = Origin::Manual;
    specA.key = dMinor;
    const auto a = fixtureAddClip(*f.processor, specA);
    const auto b = fixtureAddClip(*f.processor, clipSpec("b", 1, true));
    CONTROL_CHECK(settleBackgroundWork(*f.processor));
    f.clearSelection();
    const auto project = f.api.result("project.get");
    const auto handlesA = fixtureHandlesOf(project, 0, 2.0);
    const auto handlesB = fixtureHandlesOf(project, 1, 2.0);
    if (!CONTROL_CHECK(handlesA.has_value() && handlesB.has_value()))
        return;
    const auto& idA = handlesA->placementId;
    const auto& idB = handlesB->placementId;
    const std::vector<ContentKey> contents{a.key, b.key};
    checkCapabilityAndRefusals(f, idA);

    // dryRun predicts and changes nothing, the selection included.
    const auto untouched = captureState(f, contents);
    const auto predicted = f.api.result("ui.setSelection", selectClip(idB, true, true));
    CONTROL_CHECK(!memberOf(predicted, "applied").asBoolean() && memberOf(predicted, "dryRun").asBoolean()
                  && memberOf(predicted, "undo").isNull());
    checkEqual(untouched, captureState(f, contents), "the state after dryRun");

    // Clip b on track 1: the active track and the selection change; the scale stays C major; no other effect.
    auto selected = f.api.result("ui.setSelection", selectClip(idB, true));
    CONTROL_CHECK(memberOf(selected, "applied").asBoolean() && !memberOf(selected, "noOp").asBoolean());
    checkEqual(memberOf(predicted, "changes"), memberOf(selected, "changes"), "the changes of the prediction");
    checkEqual(memberOf(predicted, "sideEffects"), memberOf(selected, "sideEffects"), "the effects of the prediction");
    checkEqual(jsonOf(R"({"before":0,"after":1,"unit":null})"), memberOf(fieldsOf(selected), "activeTrackId"),
               "activeTrackId");
    checkEqual(jsonOf(R"({"before":[],"after":[)" + quoted(idB) + R"(],"unit":null})"),
               memberOf(fieldsOf(selected), "selectedPlacementIds"), "selectedPlacementIds");
    checkEqual(jsonOf(R"({"workspaceSwitched":{"from":"arrangement","to":"pianoRoll","revert":null}})"),
               memberOf(selected, "sideEffects"), "the side effects of selecting b");
    // OpenTune always has an active track: the revert recipe selects track 0, which had no clip selected.
    auto undo = JsonValue::object();
    undo.appendMember("undoable", JsonValue::boolean(false));
    undo.appendMember("reason", JsonValue::string("not_persisted_state"));
    undo.appendMember("revert", revertTo("trackId", JsonValue::integer(0)));
    checkEqual(undo, memberOf(selected, "undo"), "the undo of selecting b");
    CONTROL_CHECK(memberOf(selected, "warnings").size() == 0
                  && !memberOf(memberOf(selected, "dirty"), "after").asBoolean());
    CONTROL_CHECK(f.bridge->selectCalls == 1 && f.bridge->view.workspace == "pianoRoll");

    // Clip a on track 0: the scale of the UI follows its key; ui.getState agrees.
    selected = f.api.result("ui.setSelection", selectClip(idA));
    checkEqual(jsonOf(R"({"before":1,"after":0,"unit":null})"), memberOf(fieldsOf(selected), "activeTrackId"),
               "activeTrackId");
    auto effects = JsonValue::object();
    effects.appendMember("uiScaleChanged", scalePair(0, "major", 2, "minor"));
    checkEqual(effects, memberOf(selected, "sideEffects"), "the side effects of selecting a");
    checkEqual(revertTo("placementId", JsonValue::string(idB)), memberOf(memberOf(selected, "undo"), "revert"),
               "the revert recipe of selecting a");
    const auto state = f.api.result("ui.getState");
    checkEqual(jsonOf(R"({"root":2,"scale":"minor"})"), memberOf(state, "uiScale"), "uiScale of ui.getState");
    CONTROL_CHECK(memberOf(memberOf(state, "ui"), "pianoRollContentId").text() == handlesA->contentId);

    // The same again: a no-op.
    const auto again = f.api.result("ui.setSelection", selectClip(idA, true));
    CONTROL_CHECK(memberOf(again, "noOp").asBoolean() && memberOf(again, "changes").size() == 0);
    checkEqual(jsonOf(R"({"undoable":false,"reason":"no_op","revert":null})"), memberOf(again, "undo"),
               "the undo of a no-op");

    // The revert recipe of selecting a, sent back unchanged, selects b again (the window stays in the piano roll).
    const auto reverted =
        f.api.result("ui.setSelection", memberOf(revertTo("placementId", JsonValue::string(idB)), "params"));
    checkEqual(jsonOf(R"({"before":[)" + quoted(idA) + R"(],"after":[)" + quoted(idB) + R"(],"unit":null})"),
               memberOf(fieldsOf(reverted), "selectedPlacementIds"), "the selection after the revert");
    CONTROL_CHECK(f.arrangement().getActiveTrackId() == 1 && f.bridge->view.workspace == "pianoRoll");

    // A track: the active track with no clip selected; the scale stays; the piano roll is empty.
    const auto track = f.api.result("ui.setSelection", selectTrack(0));
    checkEqual(jsonOf(R"({"before":1,"after":0,"unit":null})"), memberOf(fieldsOf(track), "activeTrackId"),
               "activeTrackId of a track");
    checkEqual(jsonOf(R"({"before":[)" + quoted(idB) + R"(],"after":[],"unit":null})"),
               memberOf(fieldsOf(track), "selectedPlacementIds"), "the selection of a track");
    CONTROL_CHECK(memberOf(track, "sideEffects").size() == 0);
    checkEqual(revertTo("placementId", JsonValue::string(idB)), memberOf(memberOf(track, "undo"), "revert"),
               "the revert recipe of a track");
    CONTROL_CHECK(f.arrangement().getSelectedPlacementId(0) == 0 && f.bridge->view.pianoRollPlacementId == 0);
    CONTROL_CHECK(f.bridge->selectCalls == 5);

    // The tempo. The double click that switches to the piano roll moves the keyboard focus, and the focus that leaves
    // the transport bar's BPM field commits the field, which shows the tempo with one decimal (the test bridge models
    // it: TestUiBridge::commitBpmFieldOnViewSwitch()). The result reports it as ui.setWorkspace does. 120.27 is no
    // value in the middle of two roundings: it becomes 120.3.
    const auto isNear = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    const auto readBpm = [&f] {
        return memberOf(memberOf(f.api.result("transport.get"), "transport"), "bpm").asDouble();
    };
    // The change that transport.setTempo reports for the same step, 120.27 to 120.3: the shape the tempo change of a
    // view switch has to have.
    f.processor->setBpm(120.27);
    const auto setTempo = f.api.result("transport.setTempo", jsonOf(R"({"bpm":120.3})"));
    const JsonValue* setTempoChange = findTempoChange(setTempo);
    const JsonValue reference = setTempoChange != nullptr ? *setTempoChange : JsonValue();
    CONTROL_CHECK(setTempoChange != nullptr);

    f.processor->setBpm(120.27);
    f.bridge->view.workspace = "arrangement";
    f.bridge->project.dirty = false;
    const int marksBefore = f.bridge->dirtyMarks;
    const auto rewritten = f.api.result("ui.setSelection", selectClip(idB, true));
    CONTROL_CHECK(memberOf(rewritten, "applied").asBoolean() && !memberOf(rewritten, "noOp").asBoolean());
    CONTROL_CHECK(isNear(f.processor->getBpm(), 120.3) && isNear(readBpm(), 120.3));
    if (const JsonValue* tempoChange = checkTempoRounded(rewritten, 120.27, 120.3, true))
        checkEqual(reference, *tempoChange, "the change of the tempo, as transport.setTempo reports it");
    const auto& rewrittenChanges = memberOf(rewritten, "changes");
    CONTROL_CHECK(rewrittenChanges.size() == 2 && memberOf(rewrittenChanges.at(0), "entity").text() == "selection"
                  && memberOf(rewrittenChanges.at(1), "entity").text() == "tempo");
    const auto& rewrittenEffects = memberOf(rewritten, "sideEffects");
    CONTROL_CHECK(rewrittenEffects.size() == 2 && rewrittenEffects.find("workspaceSwitched") != nullptr);
    // The tempo is saved with the project: the switch marks it as changed, and the result says so.
    CONTROL_CHECK(!memberOf(memberOf(rewritten, "dirty"), "before").asBoolean()
                  && memberOf(memberOf(rewritten, "dirty"), "after").asBoolean()
                  && f.bridge->dirtyMarks == marksBefore + 1);
    // The undo is the selection's; the way back of the tempo is the revert recipe of its side effect, sent as it came.
    CONTROL_CHECK(memberOf(memberOf(rewritten, "undo"), "reason").text() == "not_persisted_state"
                  && memberOf(memberOf(memberOf(rewritten, "undo"), "revert"), "method").text() == "ui.setSelection");
    const auto& tempoRevert = memberOf(memberOf(rewrittenEffects, "tempoRounded"), "revert");
    f.api.result(memberOf(tempoRevert, "method").text(), memberOf(tempoRevert, "params"));
    CONTROL_CHECK(isNear(f.processor->getBpm(), 120.27));

    // A tempo with one decimal is what the field shows: nothing is rewritten, and the result reports nothing about the
    // tempo (nor marks the project).
    f.processor->setBpm(120.3);
    f.bridge->view.workspace = "arrangement";
    f.bridge->project.dirty = false;
    const auto oneDecimal = f.api.result("ui.setSelection", selectClip(idA, true));
    CONTROL_CHECK(memberOf(oneDecimal, "applied").asBoolean() && reportsNoTempoChange(oneDecimal)
                  && memberOf(oneDecimal, "sideEffects").find("workspaceSwitched") != nullptr);
    CONTROL_CHECK(isNear(f.processor->getBpm(), 120.3) && isNear(readBpm(), 120.3));
    CONTROL_CHECK(!memberOf(memberOf(oneDecimal, "dirty"), "after").asBoolean());

    // The piano roll is shown already: the double click switches nothing, nothing moves the focus, the tempo stays as
    // it is and nothing about it is reported, and the project is not marked.
    f.processor->setBpm(120.27);
    f.bridge->project.dirty = false;
    CONTROL_CHECK(f.bridge->view.workspace == "pianoRoll");
    const auto shown = f.api.result("ui.setSelection", selectClip(idB, true));
    CONTROL_CHECK(memberOf(shown, "applied").asBoolean() && reportsNoTempoChange(shown)
                  && memberOf(shown, "sideEffects").find("workspaceSwitched") == nullptr);
    CONTROL_CHECK(isNear(f.processor->getBpm(), 120.27) && isNear(readBpm(), 120.27));
    CONTROL_CHECK(!memberOf(memberOf(shown, "dirty"), "after").asBoolean());

    // dryRun predicts the switch and changes nothing, the tempo included; it does not predict the tempo, which depends
    // on where the keyboard focus is.
    f.bridge->view.workspace = "arrangement";
    const auto dry = f.api.result("ui.setSelection", selectClip(idA, true, true));
    CONTROL_CHECK(memberOf(dry, "dryRun").asBoolean() && reportsNoTempoChange(dry)
                  && memberOf(dry, "sideEffects").find("workspaceSwitched") != nullptr);
    CONTROL_CHECK(isNear(f.processor->getBpm(), 120.27) && f.bridge->view.workspace == "arrangement");

    // The tempo field does not hold the keyboard focus (a window in the background, TestUiBridge::focusElsewhere()):
    // the double click switches the view and moves no focus out of the field, so the tempo with more decimals stays,
    // nothing about it is reported and the project is not marked as changed (PROTOCOL.md section 14.6). With the
    // focus back in the field the same double click rounds it.
    f.bridge->focusElsewhere();
    f.bridge->project.dirty = false;
    const auto unfocused = f.api.result("ui.setSelection", selectClip(idA, true));
    CONTROL_CHECK(memberOf(unfocused, "applied").asBoolean() && reportsNoTempoChange(unfocused)
                  && memberOf(unfocused, "sideEffects").find("workspaceSwitched") != nullptr);
    CONTROL_CHECK(isNear(f.processor->getBpm(), 120.27) && isNear(readBpm(), 120.27));
    CONTROL_CHECK(!memberOf(memberOf(unfocused, "dirty"), "after").asBoolean());
    f.bridge->focusTempoField(*f.processor);
    f.api.result("ui.setSelection", selectClip(idB));
    f.bridge->view.workspace = "arrangement";
    f.bridge->project.dirty = false;
    const auto refocused = f.api.result("ui.setSelection", selectClip(idA, true));
    if (const JsonValue* tempoChange = checkTempoRounded(refocused, 120.27, 120.3, true))
        checkEqual(reference, *tempoChange, "the change of the tempo with the focus back in the field");
    CONTROL_CHECK(isNear(f.processor->getBpm(), 120.3));
    f.processor->setBpm(120.27);
    f.api.result("ui.setSelection", selectClip(idB));
    f.bridge->view.workspace = "arrangement";
    f.bridge->project.dirty = false;

    // A selection that fails after the focus left the BPM field (SelectionBridge::breaksOffAfterFocusMove): the field
    // committed, so the tempo 120.27 was rewritten to 120.3 before the error. "Nothing changed" would be false: the
    // error says executed true and names the tempo (kind and reason as before).
    f.bridge->breaksOffAfterFocusMove = true;
    CONTROL_CHECK(f.api.failsAfterChange("ui.setSelection", selectClip(idA, true), "INTERNAL", "",
                                         "the tempo changed from 120.27 to 120.3 bpm"));
    CONTROL_CHECK(isNear(f.processor->getBpm(), 120.3));
    // With a tempo that the field leaves as it is, the failure still promises that nothing changed.
    CONTROL_CHECK(f.api.fails("ui.setSelection", selectClip(idA, true), "INTERNAL", ""));
    CONTROL_CHECK(isNear(f.processor->getBpm(), 120.3));
    f.bridge->breaksOffAfterFocusMove = false;
    CONTROL_CHECK(f.service().dispatcher()->threadProbe().otherThreadAccesses.load() == 0);
}

// ---- Scenario: prediction against the commit -------------------------------------------------------

void runMatrixCase(bool openDyne, bool withNotes)
{
    const std::string name = std::string(openDyne ? "openDyne" : "openTune") + (withNotes ? ", notes" : ", no notes");
    SelectionFixture f;
    if (!CONTROL_CHECK(f.ready))
        return;
    f.setPreferences(openDyne, true);
    const auto clip = fixtureAddClip(*f.processor, clipSpec("clip", 0, withNotes));
    const std::vector<ContentKey> contents{clip.key};
    CONTROL_CHECK(settleBackgroundWork(*f.processor));
    f.clearSelection();
    const auto snapshot = f.processor->getContentSnapshot(clip.key);
    if (!CONTROL_CHECK(snapshot != nullptr && snapshot->noteTopologyInitialized == withNotes))
        return;
    const auto handles = fixtureHandlesOf(f.api.result("project.get"), 0, 2.0);
    if (!CONTROL_CHECK(handles.has_value()))
        return;

    const auto before = captureState(f, contents);
    const auto predicted = f.api.result("ui.setSelection", selectClip(handles->placementId, false, true));
    checkEqual(before, captureState(f, contents), name + ": the state after dryRun");
    const auto committed = f.api.result("ui.setSelection", selectClip(handles->placementId));
    for (const char* member : {"changes", "sideEffects", "warnings"})
        checkEqual(memberOf(predicted, member), memberOf(committed, member), name + ": " + member);

    // What the case brings about: notes in openDyne for a clip without notes.
    const bool notesExpected = openDyne && !withNotes;
    const auto& sideEffects = memberOf(committed, "sideEffects");
    CONTROL_CHECK((sideEffects.find("notesGenerated") != nullptr) == notesExpected);
    // Notes bring the two warnings of generated notes; nothing else is reported.
    CONTROL_CHECK(sideEffects.find("referenceAnalysisQueued") == nullptr
                  && memberOf(committed, "warnings").size() == (notesExpected ? 2u : 0u));
    for (std::size_t i = 0; i < memberOf(committed, "warnings").size(); ++i)
        CONTROL_CHECK(memberOf(memberOf(committed, "warnings").at(i), "code").text() != "REFERENCE_ANALYSIS_QUEUED");
    CONTROL_CHECK(memberOf(memberOf(committed, "dirty"), "after").asBoolean() == notesExpected);
    if (notesExpected) {
        const auto after = f.processor->getContentSnapshot(clip.key);
        CONTROL_CHECK(after != nullptr && after->noteTopologyInitialized
                      && memberOf(memberOf(sideEffects, "notesGenerated"), "noteCount").asDouble()
                             == static_cast<double>(after->notes.size()));
    }
    const auto undo = memberOf(committed, "undo");
    CONTROL_CHECK(memberOf(undo, "reason").text() == "not_persisted_state" && !memberOf(undo, "revert").isNull());
    note(name + ": " + toJsonString(sideEffects));
}

// ---- Scenario: the specification's vectors ----------------------------------------------------------

/** The ids of the vectors that must run and pass. */
const char* const kMustRunSelectionVectors[] = {
    "ui/set-selection-clip",
    "ui/set-selection-dry-run",
    "ui/set-selection-generates-notes",
    "ui/set-selection-not-found",
    "ui/set-selection-open-editor",
    // The scenarios of ui.setWorkspace that need clips, the selection, the editing scheme or the keyboard focus (its
    // other vectors run in ControlUiTests.cpp).
    "ui/set-workspace-arrangement",
    "ui/set-workspace-piano-roll-opendyne",
    "ui/set-workspace-tempo-rounded",
};

/**
 * Why this runner does not run a vector, or empty when it establishes every member of its given. A precondition
 * that the runner cannot establish is a skip with that reason (vectors/README.md, Preconditions), never a pass: a
 * member that is not listed here would be ignored, and the vector would run in a state it does not describe.
 */
std::string selectionVectorSkip(const JsonValue& vector)
{
    const auto& given = memberOf(vector, "given");
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& name = given.keyAt(i);
        if (name == "keyboardFocus") {
            // tempoField is the only value of this version of the specification.
            if (given.at(i).text() != "tempoField")
                return "keyboardFocus " + given.at(i).text() + " is not established by this runner";
        } else if (name == "preferences") {
            // Only the experimental features are written (setPreferences()).
            for (std::size_t p = 0; p < given.at(i).size(); ++p)
                if (given.at(i).keyAt(p) != "experimentalFeatures")
                    return "precondition given.preferences." + given.at(i).keyAt(p)
                           + " is not established by this runner";
        } else if (name != "dirty" && name != "clips" && name != "editingScheme") {
            return "precondition given." + name + " is not established by this runner";
        }
    }
    return {};
}

std::string runSelectionVector(const JsonValue& vector)
{
    SelectionFixture f;
    if (!f.ready)
        return "the control API did not start";
    const auto& given = memberOf(vector, "given");
    f.setPreferences(memberOf(given, "editingScheme").text() == "openDyne",
                     memberOf(memberOf(given, "preferences"), "experimentalFeatures").asBoolean());
    // given.keyboardFocus (vectors/README.md, Preconditions): where the keyboard focus is when the vector starts.
    // tempoField puts it in the transport bar's BPM field, which commits the tempo at a switch of the view. Without
    // the member a vector assumes nothing about the focus, so the runner takes it away from the field (the fixture's
    // bridge has it there for its own scenarios): a vector that depends on the focus names it. None of the other
    // vectors of this runner rewrites a tempo, so none depends on it.
    f.bridge->focusElsewhere();
    if (memberOf(given, "keyboardFocus").text() == "tempoField")
        f.bridge->focusTempoField(*f.processor);

    struct Created
    {
        std::string bind;
        ClipSpec spec;
        Clip clip;
    };
    std::vector<Created> created;
    const auto& clips = memberOf(given, "clips");
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto& item = clips.at(i);
        ClipSpec spec;
        spec.name = memberOf(item, "bind").text();
        spec.trackId = static_cast<int>(memberOf(item, "trackId").asDouble());
        spec.startTimelineSec = memberOf(item, "startTimelineSec").asDouble();
        spec.durationSec = memberOf(item, "durationSec").asDouble();
        const auto analysis = memberOf(item, "analysis").text();
        spec.analysis = analysis == "ready"     ? Analysis::Ready
                        : analysis == "running" ? Analysis::Running
                                                : Analysis::Failed;
        const auto& notes = memberOf(item, "notes");
        for (std::size_t n = 0; n < notes.size(); ++n)
            spec.notes.push_back({memberOf(notes.at(n), "startContentSec").asDouble(),
                                  memberOf(notes.at(n), "endContentSec").asDouble(),
                                  memberOf(notes.at(n), "targetMidi").asDouble()});
        created.push_back({spec.name, spec, fixtureAddClip(*f.processor, spec)});
    }
    CONTROL_CHECK(settleBackgroundWork(*f.processor));
    // The vectors start with no clip selected.
    f.clearSelection();
    f.bridge->project.dirty = memberOf(given, "dirty").isBoolean() && memberOf(given, "dirty").asBoolean();

    VectorMatcher matcher;
    const auto& discovery = f.api.discovery();
    for (std::size_t i = 0; i < discovery.size(); ++i)
        matcher.bindings["discovery." + discovery.keyAt(i)] = discovery.at(i);
    matcher.bindings["runner.unknownPlacementId"] = JsonValue::string(f.unknownPlacementId());
    if (!created.empty()) {
        const auto project = f.api.result("project.get");
        for (const auto& item : created) {
            const auto handles = fixtureHandlesOf(project, item.spec.trackId, item.spec.startTimelineSec);
            if (!handles)
                return "the clip " + item.bind + " is not in project.get";
            matcher.bindings[item.bind + ".contentId"] = JsonValue::string(handles->contentId);
            matcher.bindings[item.bind + ".placementId"] = JsonValue::string(handles->placementId);
        }
    }
    // The vectors state what OpenTune does (an active track always exists), so the expected response is the vector's
    // own, unchanged.
    const auto& expected = memberOf(vector, "response");
    const bool scenario = memberOf(vector, "type").text() == "scenario";

    std::string failure;
    runWhilePumping(
        [&] {
            auto& connection = f.api.client();
            if (scenario) {
                // Send and expect steps on the main connection, as the other scenario runners run them.
                const auto& steps = memberOf(vector, "steps");
                for (std::size_t i = 0; i < steps.size() && failure.empty(); ++i) {
                    const auto& step = steps.at(i);
                    if (const JsonValue* send = step.find("send")) {
                        const auto value = matcher.substitute(*send, failure);
                        if (failure.empty() && !connection.sendLine(toJsonString(value)))
                            failure = "send failed";
                    } else if (step.find("expect") != nullptr) {
                        failure = matcher.expectLine(connection, memberOf(step, "expect"));
                    } else {
                        failure = "a step other than send and expect";
                    }
                    if (!failure.empty())
                        failure = "step " + std::to_string(i + 1) + ": " + failure;
                }
                return;
            }
            const auto request = matcher.substitute(memberOf(vector, "request"), failure);
            if (!failure.empty())
                return;
            if (!connection.sendLine(toJsonString(request))) {
                failure = "cannot send the request";
                return;
            }
            failure = matcher.expectLine(connection, expected);
            if (failure.empty() && memberOf(vector, "connectionAfter").text() == "open"
                && !connection.staysQuiet(kOpenCheckMs))
                failure = "the connection closed or sent more";
        },
        kVectorExchangeMs);
    f.setPreferences(false, false);
    return failure;
}

} // namespace

void scenarioSelection()
{
    scenarioSelectionImpl();
}

void scenarioSelectionMatrix()
{
    for (const bool openDyne : {false, true})
        for (const bool withNotes : {false, true})
            runMatrixCase(openDyne, withNotes);
}

void scenarioSelectionVectors()
{
    const auto root = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors" / "ui";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.path().extension() == ".json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    // The skip rule on vectors of its own: a precondition that is not established is a skip, an established one is not.
    CONTROL_CHECK(selectionVectorSkip(jsonOf(R"({"given":{"dirty":false,"keyboardFocus":"tempoField"}})")).empty());
    CONTROL_CHECK(selectionVectorSkip(jsonOf(R"({"given":{"preferences":{"experimentalFeatures":true}}})")).empty());
    CONTROL_CHECK(!selectionVectorSkip(jsonOf(R"({"given":{"keyboardFocus":"trackPanel"}})")).empty());
    CONTROL_CHECK(!selectionVectorSkip(jsonOf(R"({"given":{"audio":{"running":true}}})")).empty());
    CONTROL_CHECK(!selectionVectorSkip(jsonOf(R"({"given":{"preferences":{"tuningHz":442}}})")).empty());

    std::set<std::string> passedIds;
    int passed = 0;
    int skipped = 0;
    VectorReport report("ControlProcessorTests.SelectionVectors");
    for (const auto& file : files) {
        const auto parsed = parseJson(readFile(file));
        if (!CONTROL_CHECK(parsed.ok))
            continue;
        const auto& vector = parsed.value;
        const bool workspaceScenario =
            memberOf(vector, "capability").text() == "ui.workspace" && memberOf(vector, "type").text() == "scenario";
        if (memberOf(memberOf(vector, "request"), "method").text() != "ui.setSelection" && !workspaceScenario)
            continue;
        const auto id = memberOf(vector, "id").text();
        if (const auto skip = selectionVectorSkip(vector); !skip.empty()) {
            ++skipped;
            report.skip(id, skip);
            note("SKIP " + id + ": " + skip);
            continue;
        }
        const auto started = Clock::now();
        const auto failure = runSelectionVector(vector);
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty())) {
            ++passed;
            passedIds.insert(id);
        }
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    for (const char* id : kMustRunSelectionVectors)
        CONTROL_CHECK(passedIds.count(id) == 1);
    note(std::to_string(passed) + " vectors passed, " + std::to_string(skipped) + " skipped");
}

} // namespace ControlTest
} // namespace OpenTune
