// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContentDetailsOps.h"

#include "OpsJson.h"
#include "OpsSupport.h"

#include "Content/EditableContentSnapshot.h"
#include "Inference/RenderCache.h"
#include "PluginProcessor.h"
#include "Render/ContentRenderService.h"
#include "Utils/AppPreferences.h"
#include "Utils/TimeGrid.h"

namespace OpenTune {

namespace {

/** Frame length of the pitch analysis before one exists: the F0 extractor's hop of 160 samples at
    16 kHz. Reported as derived. */
constexpr double kAnalysisHopSec = 160.0 / 16000.0;
/** publishedRevision of a render that is not current: no published render matches desiredRevision. */
constexpr const char* kUnsettledRenderToken = "unsettled";

struct DetailsParams
{
    std::optional<ControlHandle> contentId;
    std::optional<ControlHandle> placementId;
};

bool parseDetailsParams(const JsonValue& params, DetailsParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (!reader.onlyMembers({"contentId", "placementId"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, out.contentId)
        || !reader.optionalHandle("placementId", ControlHandleKind::Placement, out.placementId)) {
        why = reader.why();
        return false;
    }
    if (out.contentId.has_value() == out.placementId.has_value()) {
        why = "exactly one of contentId and placementId is required";
        return false;
    }
    return true;
}

/** The length of the content's audio in seconds: the audio buffer, else the time grid's total. */
double contentDurationSec(const EditableContentSnapshot& snapshot)
{
    if (snapshot.audioBuffer != nullptr && snapshot.audioSampleRate > 0.0)
        return snapshot.audioBuffer->getNumSamples() / snapshot.audioSampleRate;
    return snapshot.timeGrid != nullptr ? snapshot.timeGrid->totalDurationSeconds() : 0.0;
}

/** The token of everything the audible render is made from: the notes, the corrected pitch, the audio
    and analysis, the time grid and the pitch shift of the content (not the key, which is no input of the
    render). Two equal tokens mean the same render input. */
std::string renderInputToken(const ControlContentVersions& versions)
{
    ControlTokenHasher hasher('r');
    hasher.add(std::string_view(versions.notes))
        .add(std::string_view(versions.pitch))
        .add(std::string_view(versions.content))
        .add(std::string_view(versions.grid))
        .add(std::string_view(versions.pitchShift));
    return hasher.token();
}

JsonValue analysisJson(const EditableContentSnapshot& snapshot, std::vector<std::string>& derived)
{
    auto json = JsonValue::object();
    json.appendMember("f0State", JsonValue::string(controlF0StateName(snapshot.originalF0State)));
    // The reason code of the failure message that OpenTune keeps with the content; a failure loaded from a project has
    // no message and reads f0_failed.
    if (snapshot.originalF0State == OriginalF0State::Failed)
        json.appendMember("failure", JsonValue::string(controlF0FailureCode(snapshot)));
    else
        json.appendMember("failure", JsonValue());
    const bool usable = snapshot.hasUsableOriginalF0();
    if (!usable)
        derived.emplace_back("analysis.hopSec");
    json.appendMember(
        "hopSec",
        controlJsonNumber(
            usable ? snapshot.pitchCurve->getHopSize() / snapshot.pitchCurve->getSampleRate() : kAnalysisHopSec, 9));
    return json;
}

/** Whether the audible render is current, through the render cache of the content (RenderCache::
    isCanonicalSettled: no render work pending or running and every block published at its desired
    revision). The cache's own revisions are not readable, so the tokens are made from the content's
    versions: publishedRevision equals desiredRevision exactly when the render is current. */
JsonValue renderJson(OpenTuneAudioProcessor& processor, const ContentKey& key, const ControlContentVersions& versions)
{
    bool current = false;
    if (const auto* service = processor.getContentRenderService())
        if (const auto cache = service->getRenderCache(key))
            current = cache->isCanonicalSettled();
    const auto desired = renderInputToken(versions);
    auto json = JsonValue::object();
    json.appendMember("current", JsonValue::boolean(current));
    json.appendMember("publishedRevision", JsonValue::string(current ? desired : std::string(kUnsettledRenderToken)));
    json.appendMember("desiredRevision", JsonValue::string(desired));
    return json;
}

/** The time grid: handles between the two end points, whose ids are the grid's own stable ids. */
JsonValue timeGridJson(const EditableContentSnapshot& snapshot)
{
    auto json = JsonValue::object();
    auto handles = JsonValue::array();
    bool identity = true;
    if (snapshot.timeGrid != nullptr) {
        identity = snapshot.timeGrid->isIdentity();
        for (const auto& handle : snapshot.timeGrid->handles()) {
            if (handle.isEndpoint())
                continue;
            auto entry = JsonValue::object();
            entry.appendMember("handleId", JsonValue::string(std::to_string(handle.id)));
            entry.appendMember("contentSec", controlJsonSeconds(handle.source_seconds));
            entry.appendMember("outputSec", controlJsonSeconds(handle.output_seconds));
            handles.append(std::move(entry));
        }
    }
    json.appendMember("seeded", JsonValue::boolean(handles.size() != 0));
    json.appendMember("identity", JsonValue::boolean(identity));
    // The smallest distance OpenTune keeps between the output times of neighbouring handles.
    json.appendMember("minHandleSpacingSec", controlJsonSeconds(TimeGridSnapshot::kMinOutputSpacingSeconds));
    json.appendMember("handles", std::move(handles));
    return json;
}

Result<JsonValue> runGetDetails(ControlOpContext& context, const DetailsParams& params)
{
    ControlContentRef content;
    std::optional<ControlPlacementRef> named;
    if (params.placementId) {
        auto resolved = controlResolvePlacement(context, *params.placementId);
        if (!resolved.ok())
            return resolved.error();
        named = std::move(resolved).value();
        content.key = named->placement.contentKey;
        content.snapshot = controlLiveSnapshot(context, content.key);
        if (content.snapshot == nullptr)
            return controlOpError("NOT_FOUND", "unknown_handle", "the placement shows no live content");
        content.contentId = controlContentId(context, content.key);
    } else {
        auto resolved = controlResolveContent(context, *params.contentId);
        if (!resolved.ok())
            return resolved.error();
        content = std::move(resolved).value();
    }
    const auto& snapshot = *content.snapshot;

    // Every placement that shows the content, in the order of the arrangement; the values that depend
    // on a placement are taken through the named one, or else the first.
    const auto livePlacements = controlLivePlacements(context);
    std::vector<ControlPlacementRef> showing;
    for (const auto& placement : livePlacements)
        if (placement.placement.contentKey == content.key)
            showing.push_back(placement);
    if (showing.empty())
        return controlOpError("NOT_FOUND", "unknown_handle", "no placement shows the content");
    const ControlPlacementRef via = named ? *named : showing.front();

    auto& processor = context.processor();
    const auto versions = controlContentVersions(context, content.contentId, snapshot);
    const auto preferences = processor.getAppPreferences().getState().shared;
    std::vector<std::string> derived{"durationSec",
                                     "timeGrid.seeded",
                                     "timeGrid.minHandleSpacingSec",
                                     "render.current",
                                     "render.publishedRevision",
                                     "render.desiredRevision",
                                     "autoModes.uiButtonRuns",
                                     "autoModes.available"};
    auto nullReasons = JsonValue::object();

    auto placementIds = JsonValue::array();
    for (const auto& placement : showing)
        placementIds.append(JsonValue::string(placement.placementId));

    auto key = controlKeyJson(snapshot.detectedKey);
    if (key.isNull())
        nullReasons.appendMember("key", JsonValue::string("not_analyzed"));

    // The mode the AUTO button in the window runs now: snapping in the openDyne scheme, regeneration in the
    // openTune scheme. OpenTune no longer has the reference alignment (AUTO Ref), so the button never runs it
    // and uiButtonRuns is never "reference" (that value stays in the specification for builds whose window
    // still offers it). It is no mode of content.autoTune either, so autoModes.available never lists it.
    const bool notesPrimary = AudioEditingScheme::usesNotesPrimaryScheme(preferences.audioEditingScheme);
    auto available = JsonValue::array();
    if (snapshot.originalF0State == OriginalF0State::Ready && snapshot.hasUsableOriginalF0())
        for (const char* mode : {"regenerate", "snapNotes", "notesOnly"})
            available.append(JsonValue::string(mode));
    auto autoModes = JsonValue::object();
    autoModes.appendMember("editingScheme",
                           JsonValue::string(controlEditingSchemeName(preferences.audioEditingScheme)));
    autoModes.appendMember("uiButtonRuns", JsonValue::string(notesPrimary ? "snapNotes" : "regenerate"));
    autoModes.appendMember("available", std::move(available));

    auto analysis = analysisJson(snapshot, derived);

    auto derivedJson = JsonValue::array();
    for (const auto& path : derived)
        derivedJson.append(JsonValue::string(path));

    auto result = JsonValue::object();
    result.appendMember("asOf", controlAsOf(controlArrangementRevision(context), {versions}));
    result.appendMember("derived", std::move(derivedJson));
    result.appendMember("contentId", JsonValue::string(content.contentId));
    result.appendMember("viaPlacementId", JsonValue::string(via.placementId));
    result.appendMember("placementIds", std::move(placementIds));
    result.appendMember("durationSec", controlJsonSeconds(contentDurationSec(snapshot)));
    result.appendMember("key", std::move(key));
    result.appendMember("pitchShift", controlPitchShiftJson(snapshot.pitchShiftSettings));
    result.appendMember("analysis", std::move(analysis));
    result.appendMember("render", renderJson(processor, content.key, versions));
    result.appendMember("timeGrid", timeGridJson(snapshot));
    result.appendMember("autoModes", std::move(autoModes));
    if (nullReasons.size() != 0)
        result.appendMember("nullReasons", std::move(nullReasons));
    return result;
}

} // namespace

void registerContentDetailsOps(ControlMethodTable& table)
{
    table.add(controlReadMethod<DetailsParams>("content.getDetails", parseDetailsParams, runGetDetails));
}

} // namespace OpenTune
