// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectReadOps.h"

#include "OpsJson.h"
#include "OpsSupport.h"

#include "Control/ControlUiBridge.h"
#include "Content/EditableContentSnapshot.h"
#include "PluginProcessor.h"

#include <algorithm>

namespace OpenTune {

namespace {

/** OpenTune stores a track's level as -100 dB while the track is silent. */
constexpr float kSilentRmsDb = -99.9f;
/** Longest path and clip name returned (common.schema.json#/$defs/path, placement.name). */
constexpr std::size_t kMaxTextCodePoints = 4096;

JsonValue optionalNumber(const std::optional<double>& value, int decimals)
{
    return value ? controlJsonNumber(*value, decimals) : JsonValue();
}

JsonValue trackJson(const StandaloneArrangement& arrangement, int trackId, bool visible, bool anySolo,
                    int placementCount, const ControlAudioDeviceState& device)
{
    auto json = JsonValue::object();
    auto nullReasons = JsonValue::object();
    const bool muted = arrangement.isTrackMuted(trackId);
    const bool solo = arrangement.isTrackSolo(trackId);
    const double volume = arrangement.getTrackVolume(trackId);
    json.appendMember("trackId", JsonValue::integer(trackId));
    // OpenTune identifies tracks by position only and labels them "Track N" (PROTOCOL.md section 15.4).
    json.appendMember("label", JsonValue::string("Track " + std::to_string(trackId + 1)));
    json.appendMember("visible", JsonValue::boolean(visible));
    json.appendMember("muted", JsonValue::boolean(muted));
    json.appendMember("solo", JsonValue::boolean(solo));
    json.appendMember("audible", JsonValue::boolean(!muted && (solo || !anySolo)));
    json.appendMember("volumeDb", optionalNumber(controlLinearToDb(volume), 3));
    json.appendMember("volumeLinear", controlJsonNumber(std::max(0.0, volume), 6));
    json.appendMember(
        "colour", JsonValue::string("#" + arrangement.getTrackColour(trackId).toDisplayString(false).toStdString()));
    if (!device.running) {
        // No level is measured while no audio device runs; when the device cannot be seen, say why.
        json.appendMember("rmsDb", JsonValue());
        nullReasons.appendMember("rmsDb", JsonValue::string(device.known ? "no_audio_device" : device.nullReason));
    } else {
        // getTrackRmsDb(): the level the audio thread last measured.
        const float rmsDb = arrangement.getTrackRmsDb(trackId);
        json.appendMember("rmsDb", rmsDb <= kSilentRmsDb ? JsonValue() : controlJsonNumber(rmsDb, 2));
    }
    json.appendMember("placementCount", JsonValue::integer(placementCount));
    if (nullReasons.size() != 0)
        json.appendMember("nullReasons", std::move(nullReasons));
    return json;
}

JsonValue placementJson(ControlOpContext& context, const ControlPlacementRef& ref)
{
    auto& processor = context.processor();
    const auto& p = ref.placement;
    auto json = JsonValue::object();
    auto nullReasons = JsonValue::object();
    json.appendMember("placementId", JsonValue::string(ref.placementId));
    json.appendMember("trackId", JsonValue::integer(ref.trackId));
    json.appendMember("contentId", JsonValue::string(controlContentId(context, p.contentKey)));
    json.appendMember("name",
                      p.name.isEmpty() ? JsonValue() : controlJsonText(p.name.toStdString(), kMaxTextCodePoints));
    json.appendMember("startTimelineSec", controlJsonSeconds(p.timelineStartSeconds));
    json.appendMember("endTimelineSec", controlJsonSeconds(p.timelineEndSeconds()));
    json.appendMember("durationSec", controlJsonSeconds(p.durationSeconds));
    json.appendMember("clipInContentSec", controlJsonSeconds(p.clipInSeconds));
    json.appendMember("gainDb", optionalNumber(controlLinearToDb(p.gain), 3));
    json.appendMember("gainLinear", controlJsonNumber(std::max(0.0f, p.gain), 6));
    json.appendMember("fadeInSec", controlJsonSeconds(p.fadeInDuration));
    json.appendMember("fadeOutSec", controlJsonSeconds(p.fadeOutDuration));
    // The source file of the content: SourceStore::getSnapshot() of the content's source window.
    std::string sourcePath;
    if (const auto snapshot = processor.getContentSnapshot(p.contentKey))
        if (const auto* sources = processor.getSourceStore()) {
            SourceStore::SourceSnapshot source;
            if (sources->getSnapshot(snapshot->sourceWindow.sourceId, source))
                sourcePath = source.sourceFilePath.toStdString();
        }
    if (sourcePath.empty()) {
        json.appendMember("sourcePath", JsonValue());
        nullReasons.appendMember("sourcePath", JsonValue::string("not_recorded"));
    } else {
        json.appendMember("sourcePath", controlJsonText(sourcePath, kMaxTextCodePoints));
    }
    json.appendMember("geometryHash", JsonValue::string(controlGeometryHash(ref)));
    if (nullReasons.size() != 0)
        json.appendMember("nullReasons", std::move(nullReasons));
    return json;
}

JsonValue contentSummaryJson(const std::string& contentId, const EditableContentSnapshot& snapshot)
{
    auto json = JsonValue::object();
    auto nullReasons = JsonValue::object();
    json.appendMember("contentId", JsonValue::string(contentId));
    json.appendMember("f0State", JsonValue::string(controlF0StateName(snapshot.originalF0State)));
    auto key = controlKeyJson(snapshot.detectedKey);
    if (key.isNull())
        nullReasons.appendMember("key", JsonValue::string("not_analyzed"));
    json.appendMember("key", std::move(key));
    json.appendMember("noteCount", JsonValue::integer(static_cast<std::int64_t>(snapshot.notes.size())));
    json.appendMember("pitchShift", controlPitchShiftJson(snapshot.pitchShiftSettings));
    json.appendMember("timeGridIdentity",
                      JsonValue::boolean(snapshot.timeGrid == nullptr || snapshot.timeGrid->isIdentity()));
    if (nullReasons.size() != 0)
        json.appendMember("nullReasons", std::move(nullReasons));
    return json;
}

/** The selection, through the bridge: null with no_editor without one, unavailable when it cannot tell. */
JsonValue selectionJson(ControlOpContext& context, std::string& nullReason)
{
    auto* bridge = context.bridge();
    if (bridge == nullptr) {
        nullReason = "no_editor";
        return JsonValue();
    }
    const auto selected = bridge->selectedPlacementIds();
    const auto view = bridge->viewState();
    if (!selected || !view) {
        nullReason = "unavailable";
        return JsonValue();
    }
    auto json = JsonValue::object();
    auto ids = JsonValue::array();
    for (const auto id : *selected)
        if (id != 0)
            ids.append(JsonValue::string(controlPlacementId(context, id)));
    json.appendMember("selectedPlacementIds", std::move(ids));
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
    return json;
}

Result<JsonValue> runProjectGet(ControlOpContext& context, const ControlNoParams&)
{
    auto& processor = context.processor();
    const auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr)
        return controlOpError("INTERNAL", "", "the arrangement is not available");
    auto* bridge = context.bridge();
    auto derived = JsonValue::array();
    for (const char* path : {"tracks[].label", "tracks[].audible", "tracks[].volumeDb", "placements[].endTimelineSec",
                             "placements[].gainDb", "contents[].timeGridIdentity"})
        derived.append(JsonValue::string(path));
    auto nullReasons = JsonValue::object();

    // The project file and its unsaved-changes flag belong to the editor's ProjectSession.
    auto project = JsonValue::object();
    const auto info = bridge != nullptr ? bridge->projectInfo() : std::nullopt;
    if (info) {
        project.appendMember(
            "path", info->projectPath.empty() ? JsonValue() : controlJsonText(info->projectPath, kMaxTextCodePoints));
        project.appendMember("dirty", JsonValue::boolean(info->dirty));
    } else {
        // Without an editor there is no project session: no file and no unsaved changes to report.
        project.appendMember("path", JsonValue());
        project.appendMember("dirty", JsonValue::boolean(false));
        derived.append(JsonValue::string("project.path"));
        derived.append(JsonValue::string("project.dirty"));
    }

    const auto placements = controlLivePlacements(context);
    bool visibleDerived = false;
    const int visibleTracks = controlVisibleTrackCount(context, visibleDerived);
    if (visibleDerived) {
        derived.append(JsonValue::string("visibleTrackCount"));
        derived.append(JsonValue::string("tracks[].visible"));
    }
    const auto device = controlAudioDeviceState(context);
    bool anySolo = false;
    for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId)
        anySolo = anySolo || arrangement->isTrackSolo(trackId);
    auto tracks = JsonValue::array();
    for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId) {
        const auto count = std::count_if(placements.begin(), placements.end(),
                                         [trackId](const ControlPlacementRef& p) { return p.trackId == trackId; });
        tracks.append(
            trackJson(*arrangement, trackId, trackId < visibleTracks, anySolo, static_cast<int>(count), device));
    }

    auto placementList = JsonValue::array();
    for (const auto& placement : placements)
        placementList.append(placementJson(context, placement));

    auto contents = JsonValue::array();
    std::vector<ControlContentVersions> versions;
    for (const auto& key : controlLiveContents(context)) {
        const auto snapshot = controlLiveSnapshot(context, key);
        if (snapshot == nullptr)
            continue;
        const auto contentId = controlContentId(context, key);
        contents.append(contentSummaryJson(contentId, *snapshot));
        versions.push_back(controlContentVersions(context, contentId, *snapshot));
    }

    std::string selectionReason;
    auto selection = selectionJson(context, selectionReason);
    if (selection.isNull())
        nullReasons.appendMember("selection", JsonValue::string(selectionReason));
    // The undo history is reported once the history capability exists.
    nullReasons.appendMember("history", JsonValue::string("capability_missing"));

    auto result = JsonValue::object();
    result.appendMember("asOf", controlAsOf(controlArrangementRevision(context), versions));
    result.appendMember("derived", std::move(derived));
    result.appendMember("project", std::move(project));
    result.appendMember("tracks", std::move(tracks));
    result.appendMember("visibleTrackCount", JsonValue::integer(visibleTracks));
    const int activeTrack = arrangement->getActiveTrackId();
    result.appendMember("activeTrackId", activeTrack >= 0 ? JsonValue::integer(activeTrack) : JsonValue());
    result.appendMember("placements", std::move(placementList));
    result.appendMember("contents", std::move(contents));
    result.appendMember("selection", std::move(selection));
    result.appendMember("transport", controlTransportJson(context));
    result.appendMember("history", JsonValue());
    result.appendMember("nullReasons", std::move(nullReasons));
    return result;
}

Result<JsonValue> runTransportGet(ControlOpContext& context, const ControlNoParams&)
{
    // The transport describes no content: asOf lists none, and nothing in the result is derived.
    auto result = JsonValue::object();
    result.appendMember("asOf", controlAsOf(controlArrangementRevision(context), {}));
    result.appendMember("derived", JsonValue::array());
    result.appendMember("transport", controlTransportJson(context));
    return result;
}

} // namespace

void registerProjectReadOps(ControlMethodTable& table)
{
    table.add(controlReadMethod<ControlNoParams>("project.get", controlParseNoParams, runProjectGet));
    table.add(controlReadMethod<ControlNoParams>("transport.get", controlParseNoParams, runTransportGet));
}

} // namespace OpenTune
