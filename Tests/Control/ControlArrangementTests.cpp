// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scenarios of the arrangement methods (arrangement.edit, arrangement.copy) against the real processor without a
// window. A test bridge stands in for the editor (the number of shown tracks, the dirty flag, the busy flags and the
// notifications). Registered with the other processor scenarios in ControlProcessorTests.cpp.

#include "ControlContentFixtures.h"
#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneContentRepository.h"
#include "Control/ControlService.h"
#include "Control/Ops/ClipIds.h"
#include "Control/Ops/OpsRegistry.h"
#include "PluginProcessor.h"
#include "StandaloneArrangement.h"
#include "Utils/AppLogger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace OpenTune {
namespace ControlTest {

namespace {

constexpr int kVectorExchangeMs = 60000;
/** The number of track slots (OpenTuneAudioProcessor::MAX_TRACKS) and the number shown after a start. */
constexpr int kSlots = OpenTuneAudioProcessor::MAX_TRACKS;
constexpr int kShownAtStart = 2;

JsonValue json(const std::string& text)
{
    auto parsed = parseJson(text);
    CONTROL_CHECK(parsed.ok);
    return parsed.value;
}

std::string quoted(const std::string& text)
{
    std::string out;
    appendJsonQuoted(out, text);
    return out;
}

bool near(const JsonValue& value, double expected, double tolerance = 1e-6)
{
    return value.isNumber() && std::abs(value.asDouble() - expected) <= tolerance;
}

bool near(double value, double expected, double tolerance = 1e-6)
{
    return std::abs(value - expected) <= tolerance;
}

class ScopedBridge
{
public:
    ScopedBridge(ControlService& service, ControlUiBridge& bridge) : service_(service)
    {
        service_.setUiBridge(&bridge);
    }
    ~ScopedBridge() { service_.setUiBridge(nullptr); }

    ScopedBridge(const ScopedBridge&) = delete;
    ScopedBridge& operator=(const ScopedBridge&) = delete;

private:
    ControlService& service_;
};

/** A clip of the arrangement where it is now. */
struct Located
{
    int trackId = -1;
    StandaloneArrangement::Placement placement;
};

/** A started processor with a session and a test bridge that shows two tracks. */
struct Fixture
{
    std::unique_ptr<OpenTuneAudioProcessor> processor;
    TestUiBridge bridge;
    std::unique_ptr<ScopedBridge> registered;
    ApiSession api;
    std::string instanceShort;

    bool open()
    {
        processor = startedProcessor();
        auto& service = processor->getControlService();
        if (!service.isRunning())
            return false;
        bridge.visibleTracks = kShownAtStart;
        registered = std::make_unique<ScopedBridge>(service, bridge);
        if (!api.open(service))
            return false;
        instanceShort = memberOf(api.discovery(), "instanceId").text().substr(0, 8);
        return true;
    }

    StandaloneArrangement& arrangement() { return *processor->getStandaloneArrangement(); }

    /** A clip of a voiced tone with a synthetic analysis (10 ms frames, audio at 44.1 kHz). */
    Clip addClip(const std::string& name, int trackId, double startSec, double durationSec = 6.0,
                 Analysis analysis = Analysis::Ready)
    {
        ClipSpec spec;
        spec.name = name;
        spec.trackId = trackId;
        spec.startTimelineSec = startSec;
        spec.durationSec = durationSec;
        spec.analysis = analysis;
        if (durationSec >= 4.0)
            spec.notes = {{0.5, 1.5, 64.0}, {2.0, 3.0, 69.0}};
        return fixtureAddClip(*processor, spec);
    }

    std::string handle(std::uint64_t placementId)
    {
        return "p:" + instanceShort + ":"
               + std::to_string(processor->getStandaloneContentRepository()->getContentEpoch()) + ":"
               + std::to_string(placementId);
    }
    std::string pid(const Clip& clip) { return handle(clip.placementId); }

    Located locate(std::uint64_t placementId)
    {
        Located found;
        for (int trackId = 0; trackId < kSlots; ++trackId)
            if (arrangement().getPlacementById(trackId, placementId, found.placement) && !found.placement.isRetired) {
                found.trackId = trackId;
                return found;
            }
        return {};
    }

    std::string revision()
    {
        return memberOf(memberOf(api.result("project.get"), "asOf"), "arrangementRevision").text();
    }

    std::string editParams(const std::string& ops, const std::string& extra, const std::string& revisionToken)
    {
        return "{\"expectedArrangementRevision\":" + quoted(revisionToken.empty() ? revision() : revisionToken)
               + ",\"ops\":[" + ops + "]" + extra + "}";
    }
    JsonValue edit(const std::string& ops, const std::string& extra = {})
    {
        return api.result("arrangement.edit", json(editParams(ops, extra, {})));
    }
    bool editFails(const std::string& ops, const std::string& kind, const std::string& reason,
                   const std::string& extra = {}, const std::string& revisionToken = {})
    {
        return api.fails("arrangement.edit", json(editParams(ops, extra, revisionToken)), kind, reason);
    }

    std::string op(const char* name, const Clip& clip, const std::string& members)
    {
        return std::string("{\"op\":\"") + name + "\",\"placementId\":" + quoted(pid(clip))
               + (members.empty() ? "" : "," + members) + "}";
    }

    JsonValue history() { return memberOf(api.result("edit.getHistory"), "history"); }
};

std::string undoParams(const std::string& undoId)
{
    return "{\"expectedUndoId\":" + quoted(undoId) + "}";
}

std::string warningCodes(const JsonValue& result)
{
    std::string codes;
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        codes += (codes.empty() ? "" : ",") + memberOf(warnings.at(i), "code").text();
    return codes;
}

JsonValue warningOf(const JsonValue& result, const char* code)
{
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        if (memberOf(warnings.at(i), "code").text() == code)
            return warnings.at(i);
    return JsonValue();
}

/** The change entry of a placement, or null. */
JsonValue changeOf(const JsonValue& result, const std::string& placementId)
{
    const auto& changes = memberOf(result, "changes");
    for (std::size_t i = 0; i < changes.size(); ++i)
        if (memberOf(memberOf(changes.at(i), "ref"), "placementId").text() == placementId)
            return changes.at(i);
    return JsonValue();
}

const JsonValue& fieldOf(const JsonValue& change, const char* name)
{
    return memberOf(memberOf(change, "fields"), name);
}

std::string innerActions(const JsonValue& result)
{
    std::string names;
    const auto& actions = memberOf(memberOf(result, "undo"), "innerActions");
    for (std::size_t i = 0; i < actions.size(); ++i)
        names += (names.empty() ? "" : ",") + actions.at(i).text();
    return names;
}

/** "entity:ref" for every entry of created or removed, for example "placement,placement,content". */
std::string entities(const JsonValue& result, const char* member)
{
    std::string out;
    const auto& list = memberOf(result, member);
    for (std::size_t i = 0; i < list.size(); ++i)
        out += (out.empty() ? "" : ",") + memberOf(list.at(i), "entity").text();
    return out;
}

std::string refAt(const JsonValue& result, const char* member, std::size_t index, const char* refMember)
{
    const auto& list = memberOf(result, member);
    // A call that failed has no list: answer with empty text, so the checks that follow fail instead of the process.
    if (index >= list.size())
        return {};
    return memberOf(memberOf(list.at(index), "ref"), refMember).text();
}

// ---- Scenario: the ops of arrangement.edit ----------------------------------------------------------------

void checkMoves(Fixture& f, const Clip& a, const Clip& b)
{
    // Start only: one change with start and end, one undo entry of OpenTune's clip drag.
    const auto structure = f.bridge.structureChanges;
    auto result = f.edit(f.op("move", a, "\"startTimelineSec\":4"));
    auto change = changeOf(result, f.pid(a));
    CONTROL_CHECK(memberOf(result, "changes").size() == 1
                  && near(memberOf(fieldOf(change, "startTimelineSec"), "after"), 4.0)
                  && near(memberOf(fieldOf(change, "endTimelineSec"), "after"), 10.0));
    CONTROL_CHECK(memberOf(memberOf(result, "undo"), "undoable").asBoolean()
                  && innerActions(result) == "MultiMovePlacementAction");
    CONTROL_CHECK(warningCodes(result).empty() && f.bridge.structureChanges == structure + 1 && f.bridge.project.dirty);
    CONTROL_CHECK(memberOf(memberOf(result, "tokens"), "placements").size() == 1);
    CONTROL_CHECK(near(f.locate(a.placementId).placement.timelineStartSeconds, 4.0));

    // Onto another track where it overlaps a clip: OVERLAPS_CLIP.
    result = f.edit(f.op("move", a, "\"toTrackId\":1"));
    change = changeOf(result, f.pid(a));
    CONTROL_CHECK(near(memberOf(fieldOf(change, "trackId"), "after"), 1.0) && warningCodes(result) == "OVERLAPS_CLIP");
    CONTROL_CHECK(f.locate(a.placementId).trackId == 1 && f.locate(b.placementId).trackId == 1);
    // The warning names the clip it overlaps.
    const auto overlapWarning = warningOf(result, "OVERLAPS_CLIP");
    const auto& overlaps = memberOf(memberOf(overlapWarning, "data"), "overlaps");
    CONTROL_CHECK(overlaps.size() == 1 && overlaps.at(0).text() == f.pid(b));

    // Onto a hidden track: the tracks up to it are shown, each with TRACK_AUTO_REVEALED.
    result = f.edit(f.op("move", a, "\"toTrackId\":3,\"startTimelineSec\":2"));
    CONTROL_CHECK(warningCodes(result) == "TRACK_AUTO_REVEALED,TRACK_AUTO_REVEALED");
    const auto& revealed = memberOf(memberOf(memberOf(result, "sideEffects"), "tracksRevealed"), "trackIds");
    CONTROL_CHECK(revealed.size() == 2 && near(revealed.at(0), 2.0) && near(revealed.at(1), 3.0));
    CONTROL_CHECK(f.bridge.visibleTracks == 4 && !memberOf(memberOf(result, "revisionDiff"), "trackState").isNull());
    CONTROL_CHECK(f.edit(f.op("move", a, "\"toTrackId\":0")).isObject() && f.locate(a.placementId).trackId == 0);

    // A move to where the clip is changes nothing.
    result = f.edit(f.op("move", a, "\"toTrackId\":0,\"startTimelineSec\":2"));
    CONTROL_CHECK(memberOf(result, "noOp").asBoolean()
                  && memberOf(memberOf(result, "undo"), "reason").text() == "no_op");
}

void checkTrims(Fixture& f, const Clip& a)
{
    // Beyond the audio of the content, past the other edge, before the content's start.
    CONTROL_CHECK(
        f.editFails(f.op("trim", a, "\"edge\":\"end\",\"toTimelineSec\":9"), "INVALID_ARGUMENT", "trim_out_of_range"));
    CONTROL_CHECK(f.editFails(f.op("trim", a, "\"edge\":\"end\",\"toTimelineSec\":2.005"), "INVALID_ARGUMENT",
                              "trim_out_of_range"));
    CONTROL_CHECK(f.editFails(f.op("trim", a, "\"edge\":\"start\",\"toTimelineSec\":1"), "INVALID_ARGUMENT",
                              "trim_out_of_range"));
    CONTROL_CHECK(f.editFails(f.op("trim", a, "\"edge\":\"start\",\"toTimelineSec\":8"), "INVALID_ARGUMENT",
                              "trim_out_of_range"));

    // The start edge moves the content window with it; back to where it was.
    auto result = f.edit(f.op("trim", a, "\"edge\":\"start\",\"toTimelineSec\":3"));
    auto change = changeOf(result, f.pid(a));
    CONTROL_CHECK(near(memberOf(fieldOf(change, "clipInContentSec"), "after"), 1.0)
                  && near(memberOf(fieldOf(change, "durationSec"), "after"), 5.0)
                  && near(memberOf(fieldOf(change, "startTimelineSec"), "after"), 3.0)
                  && memberOf(memberOf(change, "fields"), "endTimelineSec").isNull());
    CONTROL_CHECK(innerActions(result) == "TrimPlacementAction" && warningCodes(result).empty());
    result = f.edit(f.op("trim", a, "\"edge\":\"start\",\"toTimelineSec\":2"));
    CONTROL_CHECK(near(f.locate(a.placementId).placement.clipInSeconds, 0.0)
                  && near(f.locate(a.placementId).placement.durationSeconds, 6.0));

    // A trim that shortens the fades records them first (FADES_RECLAMPED).
    CONTROL_CHECK(f.edit(f.op("setFade", a, "\"fadeOutSec\":3")).isObject());
    result = f.edit(f.op("trim", a, "\"edge\":\"end\",\"toTimelineSec\":4"));
    const auto reclamped = warningOf(result, "FADES_RECLAMPED");
    CONTROL_CHECK(memberOf(memberOf(reclamped, "data"), "fadeInSec").isNull()
                  && near(memberOf(memberOf(memberOf(reclamped, "data"), "fadeOutSec"), "after"), 1.8));
    CONTROL_CHECK(innerActions(result) == "FadeChangeAction,TrimPlacementAction");
    CONTROL_CHECK(near(f.locate(a.placementId).placement.fadeOutDuration, 1.8));
    CONTROL_CHECK(f.edit(f.op("trim", a, "\"edge\":\"end\",\"toTimelineSec\":8")).isObject());
    CONTROL_CHECK(f.edit(f.op("setFade", a, "\"fadeOutSec\":0")).isObject());
}

void checkGainAndFades(Fixture& f, const Clip& a)
{
    auto result = f.edit(f.op("setGain", a, "\"gainDb\":-6"));
    auto change = changeOf(result, f.pid(a));
    CONTROL_CHECK(near(memberOf(fieldOf(change, "gainDb"), "after"), -6.0)
                  && near(memberOf(fieldOf(change, "gainLinear"), "after"), 0.501187)
                  && innerActions(result) == "GainChangeAction");
    result = f.edit(f.op("setGain", a, "\"gainLinear\":2"));
    CONTROL_CHECK(near(memberOf(fieldOf(changeOf(result, f.pid(a)), "gainDb"), "after"), 6.021, 1e-3));

    // The clip gain is limited to the interface's linear range 0 to 4 (the volume knob's setRange(0.0, 4.0, 0.001)).
    // The tour's 200 dB is applied as 4x, reported in the unit asked for; dryRun predicts the same value and warning
    // without storing it.
    result = f.edit(f.op("setGain", a, "\"gainDb\":200"), ",\"dryRun\":true");
    const auto predicted = warningOf(result, "CLAMPED");
    CONTROL_CHECK(memberOf(result, "dryRun").asBoolean()
                  && memberOf(memberOf(predicted, "data"), "field").text() == "gainDb"
                  && near(memberOf(memberOf(predicted, "data"), "requested"), 200.0)
                  && near(memberOf(memberOf(predicted, "data"), "applied"), 12.041, 1e-3)
                  && near(memberOf(fieldOf(changeOf(result, f.pid(a)), "gainDb"), "after"), 12.041, 1e-3)
                  && near(f.locate(a.placementId).placement.gain, 2.0));
    result = f.edit(f.op("setGain", a, "\"gainDb\":200"));
    const auto clamped = warningOf(result, "CLAMPED");
    CONTROL_CHECK(memberOf(memberOf(clamped, "data"), "field").text() == "gainDb"
                  && near(memberOf(memberOf(clamped, "data"), "requested"), 200.0)
                  && near(memberOf(memberOf(clamped, "data"), "applied"), 12.041, 1e-3)
                  && near(memberOf(fieldOf(changeOf(result, f.pid(a)), "gainDb"), "after"), 12.041, 1e-3)
                  && near(f.locate(a.placementId).placement.gain, 4.0));
    // A linear factor above the range is limited too, with the applied value as a factor.
    result = f.edit(f.op("setGain", a, "\"gainLinear\":5"));
    const auto linearClamped = warningOf(result, "CLAMPED");
    CONTROL_CHECK(memberOf(memberOf(linearClamped, "data"), "field").text() == "gainLinear"
                  && near(memberOf(memberOf(linearClamped, "data"), "requested"), 5.0)
                  && near(memberOf(memberOf(linearClamped, "data"), "applied"), 4.0)
                  && near(f.locate(a.placementId).placement.gain, 4.0));
    // The lower end is 0, which the params schema accepts (silence); a negative linear factor is not a schema value.
    CONTROL_CHECK(f.edit(f.op("setGain", a, "\"gainLinear\":0")).isObject()
                  && near(f.locate(a.placementId).placement.gain, 0.0));
    CONTROL_CHECK(f.editFails(f.op("setGain", a, "\"gainLinear\":-1"), "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(f.edit(f.op("setGain", a, "\"gainLinear\":1")).isObject());

    // Fades longer than 90 % of the clip are limited (CLAMPED).
    result = f.edit(f.op("setFade", a, "\"fadeInSec\":10"));
    const auto fade = warningOf(result, "CLAMPED");
    CONTROL_CHECK(memberOf(memberOf(fade, "data"), "field").text() == "fadeInSec"
                  && near(memberOf(memberOf(fade, "data"), "applied"), 5.4)
                  && near(f.locate(a.placementId).placement.fadeInDuration, 5.4));
    CONTROL_CHECK(innerActions(result) == "FadeChangeAction");
    CONTROL_CHECK(f.edit(f.op("setFade", a, "\"fadeInSec\":0")).isObject());
}

void checkCallRules(Fixture& f, const Clip& a, const Clip& b)
{
    // Several ops are one undo entry; the changes of one clip are one entry.
    auto result = f.edit(f.op("move", a, "\"startTimelineSec\":3") + "," + f.op("setGain", a, "\"gainDb\":-3") + ","
                         + f.op("setFade", a, "\"fadeInSec\":0.5"));
    CONTROL_CHECK(innerActions(result) == "MultiMovePlacementAction,GainChangeAction,FadeChangeAction"
                  && memberOf(result, "changes").size() == 1
                  && memberOf(memberOf(result, "undo"), "steps").asDouble() == 1);
    const auto entry = memberOf(memberOf(result, "undo"), "undoId").text();
    CONTROL_CHECK(memberOf(memberOf(f.history(), "undoTop"), "undoId").text() == entry);

    // A later op that fails leaves the clips as they were.
    const auto before = f.revision();
    CONTROL_CHECK(f.editFails(f.op("move", a, "\"startTimelineSec\":5") + ","
                                  + f.op("trim", a, "\"edge\":\"end\",\"toTimelineSec\":100"),
                              "INVALID_ARGUMENT", "trim_out_of_range"));
    CONTROL_CHECK(f.revision() == before && near(f.locate(a.placementId).placement.timelineStartSeconds, 3.0));

    // dryRun predicts and changes nothing.
    result = f.edit(f.op("move", a, "\"startTimelineSec\":7"), ",\"dryRun\":true");
    CONTROL_CHECK(memberOf(result, "dryRun").asBoolean() && !memberOf(result, "applied").asBoolean()
                  && memberOf(result, "undo").isNull());
    CONTROL_CHECK(near(memberOf(fieldOf(changeOf(result, f.pid(a)), "startTimelineSec"), "after"), 7.0));
    CONTROL_CHECK(f.revision() == before && near(f.locate(a.placementId).placement.timelineStartSeconds, 3.0));
    CONTROL_CHECK(memberOf(memberOf(f.history(), "undoTop"), "undoId").text() == entry);

    // The order of the checks: handles, busy, the revision, then the ops.
    const auto unknown =
        "{\"op\":\"move\",\"placementId\":" + quoted(f.handle(4294967295ULL)) + ",\"startTimelineSec\":1}";
    f.bridge.busy.pointerDown = true;
    CONTROL_CHECK(f.editFails(unknown, "NOT_FOUND", "unknown_handle"));
    CONTROL_CHECK(
        f.editFails(f.op("move", a, "\"toTrackId\":" + std::to_string(kSlots)), "NOT_FOUND", "unknown_handle"));
    CONTROL_CHECK(f.editFails(f.op("move", a, "\"startTimelineSec\":1"), "BUSY", "pointer_down", {}, "stale"));
    f.bridge.busy.pointerDown = false;
    CONTROL_CHECK(f.editFails(f.op("trim", a, "\"edge\":\"end\",\"toTimelineSec\":100"), "REVISION_CONFLICT",
                              "arrangement_changed", {}, "stale"));
    // A change of a track's mix keeps the arrangement revision.
    CONTROL_CHECK(f.api.result("track.set", json(R"({"tracks":[{"trackId":0,"muted":true}]})")).isObject());
    CONTROL_CHECK(f.revision() == before);

    // The params: closed, op rules, confirm only with delete.
    const auto revision = quoted(before);
    for (const auto& ops : {f.op("split", a, "\"atTimelineSec\":4") + "," + f.op("move", b, "\"startTimelineSec\":1"),
                            f.op("move", a, ""), f.op("setGain", a, "\"gainDb\":1,\"gainLinear\":1"),
                            f.op("setFade", a, ""), f.op("trim", a, "\"edge\":\"middle\",\"toTimelineSec\":4"),
                            f.op("move", a, "\"startTimelineSec\":-1"), f.op("move", a, "\"toTrackId\":1.5"),
                            std::string("{\"op\":\"rotate\"}"), f.op("move", a, "\"startTimelineSec\":1,\"extra\":1"),
                            "{\"op\":\"delete\",\"placementIds\":[" + quoted(f.pid(a)) + "," + quoted(f.pid(a)) + "]}",
                            std::string("{\"op\":\"delete\",\"placementIds\":[]}")})
        CONTROL_CHECK(f.api.fails("arrangement.edit",
                                  json("{\"expectedArrangementRevision\":" + revision + ",\"ops\":[" + ops + "]}"),
                                  "INVALID_ARGUMENT", "invalid_params"));
    for (const auto& params : {"{\"expectedArrangementRevision\":" + revision + ",\"ops\":["
                                   + f.op("move", a, "\"startTimelineSec\":1") + "],\"confirm\":true}",
                               "{\"expectedArrangementRevision\":" + revision + ",\"ops\":[]}",
                               "{\"ops\":[" + f.op("move", a, "\"startTimelineSec\":1") + "]}",
                               "{\"expectedArrangementRevision\":" + revision + ",\"ops\":["
                                   + f.op("move", a, "\"startTimelineSec\":1") + "],\"more\":1}"})
        CONTROL_CHECK(f.api.fails("arrangement.edit", json(params), "INVALID_ARGUMENT", "invalid_params"));
    std::string many;
    for (int i = 0; i < 65; ++i)
        many += (many.empty() ? "" : ",") + f.op("setGain", a, "\"gainDb\":0");
    CONTROL_CHECK(f.api.fails("arrangement.edit",
                              json("{\"expectedArrangementRevision\":" + revision + ",\"ops\":[" + many + "]}"),
                              "INVALID_ARGUMENT", "invalid_params"));
}

void checkSplitAndMerge(Fixture& f, const Clip& a, const Clip& c, const Clip& pending)
{
    CONTROL_CHECK(f.editFails(f.op("split", pending, "\"atTimelineSec\":3"), "NOT_READY", "f0_not_ready"));
    CONTROL_CHECK(f.editFails(f.op("split", c, "\"atTimelineSec\":20.05"), "INVALID_ARGUMENT", "split_out_of_range"));
    CONTROL_CHECK(f.editFails(f.op("split", c, "\"atTimelineSec\":26"), "INVALID_ARGUMENT", "split_out_of_range"));
    CONTROL_CHECK(f.editFails(f.op("split", c, "\"atTimelineSec\":30"), "INVALID_ARGUMENT", "split_out_of_range"));

    // dryRun names the clip that would be replaced.
    const auto before = f.revision();
    auto result = f.edit(f.op("split", c, "\"atTimelineSec\":23"), ",\"dryRun\":true");
    CONTROL_CHECK(entities(result, "removed") == "placement" && entities(result, "created").empty()
                  && f.revision() == before);

    // Between two analysis frames: the cut snaps to the nearest one (SPLIT_SNAPPED).
    result = f.edit(f.op("split", c, "\"atTimelineSec\":23.004"));
    CONTROL_CHECK(warningCodes(result) == "SPLIT_SNAPPED,UPSTREAM_UNDO_DEFECT");
    const auto snapped = memberOf(warningOf(result, "SPLIT_SNAPPED"), "data");
    CONTROL_CHECK(near(memberOf(snapped, "requestedTimelineSec"), 23.004)
                  && near(memberOf(snapped, "appliedTimelineSec"), 23.0));
    CONTROL_CHECK(entities(result, "created") == "placement,placement" && entities(result, "removed") == "placement"
                  && refAt(result, "removed", 0, "placementId") == f.pid(c));
    CONTROL_CHECK(memberOf(memberOf(result, "idMap").at(0), "relation").text() == "split");
    const auto undo = memberOf(result, "undo");
    CONTROL_CHECK(!memberOf(undo, "undoable").asBoolean() && memberOf(undo, "reason").text() == "upstream_undo_defect");
    CONTROL_CHECK(memberOf(memberOf(f.history(), "undoTop"), "unsafe").asBoolean());
    const auto left = refAt(result, "created", 0, "placementId");
    const auto right = refAt(result, "created", 1, "placementId");

    // The revert recipe merges the two clips again, and the merge's recipe splits the merged clip again.
    const auto revert = memberOf(undo, "revert");
    CONTROL_CHECK(memberOf(revert, "method").text() == "arrangement.edit"
                  && memberOf(revert, "requiresConsent").isNull());
    result = f.api.result("arrangement.edit", memberOf(revert, "params"));
    CONTROL_CHECK(entities(result, "removed") == "placement,placement" && entities(result, "created") == "placement"
                  && refAt(result, "removed", 0, "placementId") == left
                  && refAt(result, "removed", 1, "placementId") == right);
    CONTROL_CHECK(memberOf(memberOf(result, "idMap").at(0), "relation").text() == "merge"
                  && warningCodes(result) == "UPSTREAM_UNDO_DEFECT");
    const auto mergeRevert = memberOf(memberOf(result, "undo"), "revert");
    const auto& split = memberOf(memberOf(mergeRevert, "params"), "ops").at(0);
    CONTROL_CHECK(memberOf(split, "op").text() == "split" && near(memberOf(split, "atTimelineSec"), 23.0));
    result = f.api.result("arrangement.edit", memberOf(mergeRevert, "params"));
    CONTROL_CHECK(entities(result, "created") == "placement,placement");

    // Clips that cannot be merged: on different tracks, not adjacent, of different sources.
    const auto merge = [](const std::string& leading, const std::string& trailing) {
        return "{\"op\":\"merge\",\"leadingPlacementId\":" + quoted(leading)
               + ",\"trailingPlacementId\":" + quoted(trailing) + "}";
    };
    CONTROL_CHECK(f.editFails(merge(f.pid(a), f.pid(pending)), "INVALID_ARGUMENT", "merge_not_possible"));
    const auto pieces = memberOf(result, "created");
    const auto first = memberOf(memberOf(pieces.at(0), "ref"), "placementId").text();
    const auto second = memberOf(memberOf(pieces.at(1), "ref"), "placementId").text();
    CONTROL_CHECK(f.editFails(merge(second, first), "INVALID_ARGUMENT", "merge_not_possible"));
    CONTROL_CHECK(f.editFails(merge(f.pid(a), first), "INVALID_ARGUMENT", "merge_not_possible"));
    CONTROL_CHECK(f.editFails(merge(first, first), "INVALID_ARGUMENT", "merge_not_possible"));

    // A merge whose former boundary the merged clip cannot be split at again (the leading part is trimmed to 0.05 s,
    // less than the 0.1 s a split leaves on each side): IRREVERSIBLE and no revert recipe.
    CONTROL_CHECK(
        f.edit("{\"op\":\"trim\",\"placementId\":" + quoted(first) + ",\"edge\":\"start\",\"toTimelineSec\":22.95}")
            .isObject());
    result = f.edit(merge(first, second));
    CONTROL_CHECK(warningCodes(result) == "UPSTREAM_UNDO_DEFECT,IRREVERSIBLE");
    const auto irreversible = memberOf(warningOf(result, "IRREVERSIBLE"), "data");
    CONTROL_CHECK(memberOf(irreversible, "reason").text() == "split_cannot_restore_boundary"
                  && memberOf(irreversible, "checkReason").text() == "split_out_of_range");
    CONTROL_CHECK(!memberOf(memberOf(result, "undo"), "undoable").asBoolean()
                  && memberOf(memberOf(result, "undo"), "revert").isNull());
}

void checkDelete(Fixture& f, const Clip& a, const Clip& b)
{
    const auto ids = "{\"op\":\"delete\",\"placementIds\":[" + quoted(f.pid(a)) + "," + quoted(f.pid(b)) + "]}";
    CONTROL_CHECK(f.editFails(ids, "INVALID_ARGUMENT", "confirm_required"));
    CONTROL_CHECK(f.editFails(ids, "INVALID_ARGUMENT", "confirm_required", ",\"confirm\":false"));
    // A prediction needs no confirm.
    const auto before = f.revision();
    auto result = f.edit(ids, ",\"dryRun\":true");
    CONTROL_CHECK(entities(result, "removed") == "placement,placement,content,content" && f.revision() == before);

    result = f.edit(ids, ",\"confirm\":true");
    CONTROL_CHECK(entities(result, "removed") == "placement,placement,content,content");
    CONTROL_CHECK(refAt(result, "removed", 2, "contentId").rfind("c:", 0) == 0);
    CONTROL_CHECK(warningCodes(result) == "UPSTREAM_UNDO_DEFECT,IRREVERSIBLE,UPSTREAM_UNDO_DEFECT,IRREVERSIBLE");
    CONTROL_CHECK(memberOf(memberOf(result, "undo"), "reason").text() == "upstream_undo_defect"
                  && memberOf(memberOf(result, "undo"), "revert").isNull());
    CONTROL_CHECK(f.locate(a.placementId).trackId < 0 && f.locate(b.placementId).trackId < 0);
    // One undo entry, which edit.undo refuses.
    const auto top = memberOf(f.history(), "undoTop");
    CONTROL_CHECK(memberOf(top, "unsafe").asBoolean());
    CONTROL_CHECK(
        f.api.fails("edit.undo", json(undoParams(memberOf(top, "undoId").text())), "UNSUPPORTED", "undo_unsafe_build"));
    // A deleted clip is not found any more.
    CONTROL_CHECK(f.editFails(ids, "NOT_FOUND", "unknown_handle", ",\"confirm\":true"));
}

void scenarioArrangementImpl()
{
    Fixture f;
    if (!CONTROL_CHECK(f.open()))
        return;
    const auto a = f.addClip("a", 0, 2.0);
    const auto b = f.addClip("b", 1, 2.0);
    const auto c = f.addClip("c", 0, 20.0);
    const auto pending = f.addClip("pending", 5, 0.0, 6.0, Analysis::Running);
    CONTROL_CHECK(settleBackgroundWork(*f.processor));
    checkMoves(f, a, b);
    checkTrims(f, a);
    checkGainAndFades(f, a);
    checkCallRules(f, a, b);
    checkSplitAndMerge(f, a, c, pending);
    checkDelete(f, a, b);
}

// ---- Scenario: undo entries and revert recipes ----------------------------------------------------------

void scenarioArrangementUndoImpl()
{
    Fixture f;
    if (!CONTROL_CHECK(f.open()))
        return;
    const auto a = f.addClip("a", 0, 2.0);
    f.addClip("c", 1, 2.0);
    CONTROL_CHECK(settleBackgroundWork(*f.processor));
    CONTROL_CHECK(f.arrangement().setPlacementFade(0, a.placementId, 0.0, 3.0));

    // Undoing a trim restores the fades it shortened, redoing it shortens them again.
    auto result = f.edit(f.op("trim", a, "\"edge\":\"end\",\"toTimelineSec\":4"));
    const auto trim = memberOf(memberOf(result, "undo"), "undoId").text();
    const auto undone = f.api.result("edit.undo", json(undoParams(trim)));
    auto change = changeOf(undone, f.pid(a));
    CONTROL_CHECK(near(memberOf(fieldOf(change, "durationSec"), "after"), 6.0)
                  && near(memberOf(fieldOf(change, "fadeOutSec"), "after"), 3.0)
                  && near(memberOf(fieldOf(change, "endTimelineSec"), "after"), 8.0));
    CONTROL_CHECK(near(f.locate(a.placementId).placement.fadeOutDuration, 3.0));
    const auto redone = f.api.result("edit.redo", json(undoParams(trim)));
    CONTROL_CHECK(near(f.locate(a.placementId).placement.fadeOutDuration, 1.8)
                  && near(f.locate(a.placementId).placement.durationSeconds, 2.0) && redone.isObject());
    CONTROL_CHECK(near(memberOf(fieldOf(changeOf(redone, f.pid(a)), "endTimelineSec"), "after"), 4.0)
                  && memberOf(memberOf(changeOf(redone, f.pid(a)), "fields"), "clipInSec").isNull());
    CONTROL_CHECK(memberOf(memberOf(change, "fields"), "clipInSec").isNull());
    CONTROL_CHECK(f.api.result("edit.undo", json(undoParams(trim))).isObject());

    // The placement changes of edit.undo and edit.redo use the field names of project.get: clipInContentSec, gainDb
    // and endTimelineSec (checked with the trim above), never the former clipInSec.
    const auto windowed = f.edit(f.op("trim", a, "\"edge\":\"start\",\"toTimelineSec\":3") + ","
                                 + f.op("setGain", a, "\"gainLinear\":0.5"));
    const auto windowId = memberOf(memberOf(windowed, "undo"), "undoId").text();
    const auto checkFieldNames = [&](const JsonValue& answer, double clipIn, double gainDb) {
        const auto placement = changeOf(answer, f.pid(a));
        CONTROL_CHECK(memberOf(placement, "fields").find("clipInSec") == nullptr);
        CONTROL_CHECK(near(memberOf(fieldOf(placement, "clipInContentSec"), "after"), clipIn));
        CONTROL_CHECK(near(memberOf(fieldOf(placement, "gainDb"), "after"), gainDb, 0.01));
        CONTROL_CHECK(near(memberOf(fieldOf(placement, "gainLinear"), "after"), gainDb == 0.0 ? 1.0 : 0.5, 1e-3));
    };
    checkFieldNames(f.api.result("edit.undo", json(undoParams(windowId))), 0.0, 0.0);
    checkFieldNames(f.api.result("edit.redo", json(undoParams(windowId))), 1.0, -6.02);
    CONTROL_CHECK(f.api.result("edit.undo", json(undoParams(windowId))).isObject());

    // A call of several ops is undone in one step.
    result = f.edit(f.op("move", a, "\"toTrackId\":1,\"startTimelineSec\":9") + ","
                    + f.op("setGain", a, "\"gainLinear\":0.5") + "," + f.op("setFade", a, "\"fadeInSec\":1"));
    const auto several = memberOf(memberOf(result, "undo"), "undoId").text();
    CONTROL_CHECK(f.locate(a.placementId).trackId == 1);
    CONTROL_CHECK(f.api.result("edit.undo", json(undoParams(several))).isObject());
    const auto back = f.locate(a.placementId);
    CONTROL_CHECK(back.trackId == 0 && near(back.placement.timelineStartSeconds, 2.0) && near(back.placement.gain, 1.0)
                  && near(back.placement.fadeInDuration, 0.0) && near(back.placement.fadeOutDuration, 3.0));

    // Split and merge push the UI's entries, which edit.undo refuses: OpenTune's undo does not bring the clips back.
    result = f.edit(f.op("split", a, "\"atTimelineSec\":5"));
    auto top = memberOf(f.history(), "undoTop");
    CONTROL_CHECK(memberOf(top, "origin").text() == "control" && memberOf(top, "unsafe").asBoolean());
    CONTROL_CHECK(
        f.api.fails("edit.undo", json(undoParams(memberOf(top, "undoId").text())), "UNSUPPORTED", "undo_unsafe_build"));
    CONTROL_CHECK(f.api.fails("edit.undo", json(R"({"force":true})"), "UNSUPPORTED", "undo_unsafe_build"));
    result = f.api.result("arrangement.edit", memberOf(memberOf(memberOf(result, "undo"), "revert"), "params"));
    top = memberOf(f.history(), "undoTop");
    CONTROL_CHECK(memberOf(top, "unsafe").asBoolean() && entities(result, "created") == "placement");
    const auto merged = f.locate(std::stoull(
        refAt(result, "created", 0, "placementId").substr(refAt(result, "created", 0, "placementId").rfind(':') + 1)));
    CONTROL_CHECK(merged.trackId == 0 && near(merged.placement.timelineStartSeconds, 2.0)
                  && near(merged.placement.durationSeconds, 6.0));
}

// ---- Scenario: arrangement.copy -----------------------------------------------------------------------------

void scenarioArrangementCopyImpl()
{
    Fixture f;
    if (!CONTROL_CHECK(f.open()))
        return;
    const auto a = f.addClip("a", 0, 2.0);
    const auto b = f.addClip("b", 1, 2.0);
    CONTROL_CHECK(settleBackgroundWork(*f.processor));
    CONTROL_CHECK(f.arrangement().setPlacementGain(0, a.placementId, 0.5f));
    CONTROL_CHECK(f.arrangement().setPlacementFade(0, a.placementId, 0.2, 0.3));
    const auto copy = [&f](const std::string& sources, const std::string& mode, int track, double start,
                           const std::string& revisionToken = {}) {
        return json("{\"sources\":[" + sources + "],\"mode\":" + quoted(mode)
                    + ",\"toTrackId\":" + std::to_string(track) + ",\"startTimelineSec\":" + std::to_string(start)
                    + ",\"expectedArrangementRevision\":" + quoted(revisionToken.empty() ? f.revision() : revisionToken)
                    + "}");
    };
    const auto source = [&f](const Clip& clip) { return "{\"placementId\":" + quoted(f.pid(clip)) + "}"; };

    // Mode full on its own track at its own start: it plays with the source and overlaps it.
    auto result = f.api.result("arrangement.copy", copy(source(a), "full", 0, 2.0));
    CONTROL_CHECK(entities(result, "created") == "placement,content");
    CONTROL_CHECK(warningCodes(result) == "STACKED_ON_SOURCE,OVERLAPS_CLIP");
    CONTROL_CHECK(memberOf(memberOf(result, "idMap").at(0), "relation").text() == "copy");
    const auto fullCopy = refAt(result, "created", 0, "placementId");
    const auto fullId = std::stoull(fullCopy.substr(fullCopy.rfind(':') + 1));
    const auto full = f.locate(fullId);
    CONTROL_CHECK(full.trackId == 0 && near(full.placement.gain, 0.5) && near(full.placement.fadeOutDuration, 0.3)
                  && !(full.placement.contentKey == a.key));
    const auto& undo = memberOf(result, "undo");
    CONTROL_CHECK(memberOf(undo, "reason").text() == "ui_parity"
                  && memberOf(memberOf(undo, "revert"), "requiresConsent").asBoolean());
    CONTROL_CHECK(f.bridge.project.dirty);

    // The revert recipe, with the end user's consent added, removes the copy and its content.
    auto params = memberOf(memberOf(undo, "revert"), "params");
    params.appendMember("confirm", JsonValue::boolean(true));
    const auto reverted = f.api.result("arrangement.edit", params);
    CONTROL_CHECK(entities(reverted, "removed") == "placement,content" && f.locate(fullId).trackId < 0);

    // Mode window copies the part the clip shows: the copy shows its whole new content.
    CONTROL_CHECK(f.edit(f.op("trim", a, "\"edge\":\"start\",\"toTimelineSec\":3")).isObject());
    result = f.api.result("arrangement.copy", copy(source(a), "window", 2, 10.0));
    CONTROL_CHECK(warningCodes(result) == "TRACK_AUTO_REVEALED" && f.bridge.visibleTracks == 3);
    const auto windowCopy = refAt(result, "created", 0, "placementId");
    const auto window = f.locate(std::stoull(windowCopy.substr(windowCopy.rfind(':') + 1)));
    CONTROL_CHECK(window.trackId == 2 && near(window.placement.clipInSeconds, 0.0)
                  && near(window.placement.durationSeconds, 5.0) && near(window.placement.timelineStartSeconds, 10.0));
    const auto snapshot = f.processor->getContentSnapshot(window.placement.contentKey);
    CONTROL_CHECK(snapshot != nullptr && snapshot->audioBuffer != nullptr
                  && near(snapshot->audioBuffer->getNumSamples() / snapshot->audioSampleRate, 5.0, 1e-3));

    // Several sources keep their distances from the earliest.
    result = f.api.result("arrangement.copy", copy(source(a) + "," + source(b), "full", 3, 20.0));
    CONTROL_CHECK(entities(result, "created") == "placement,placement,content,content");
    const auto& to = memberOf(memberOf(result, "idMap").at(0), "to");
    const auto copyOfA = f.locate(std::stoull(
        memberOf(to.at(0), "placementId").text().substr(memberOf(to.at(0), "placementId").text().rfind(':') + 1)));
    const auto copyOfB = f.locate(std::stoull(
        memberOf(to.at(1), "placementId").text().substr(memberOf(to.at(1), "placementId").text().rfind(':') + 1)));
    CONTROL_CHECK(near(copyOfA.placement.timelineStartSeconds, 21.0)
                  && near(copyOfB.placement.timelineStartSeconds, 20.0));
    CONTROL_CHECK(f.bridge.visibleTracks == 4);

    // Tokens: a stale revision conflicts; a change elsewhere keeps it valid.
    const auto revision = f.revision();
    CONTROL_CHECK(f.api.result("track.set", json(R"({"tracks":[{"trackId":1,"solo":true}]})")).isObject());
    CONTROL_CHECK(f.api.result("arrangement.copy", copy(source(b), "window", 1, 30.0, revision)).isObject());
    CONTROL_CHECK(f.api.fails("arrangement.copy", copy(source(b), "window", 1, 40.0, revision), "REVISION_CONFLICT",
                              "arrangement_changed"));

    // Handles, busy, params.
    CONTROL_CHECK(f.api.fails("arrangement.copy",
                              copy("{\"placementId\":" + quoted(f.handle(4294967295ULL)) + "}", "full", 0, 1.0),
                              "NOT_FOUND", "unknown_handle"));
    CONTROL_CHECK(f.api.fails("arrangement.copy", copy(source(a), "full", kSlots, 1.0), "NOT_FOUND", "unknown_handle"));
    f.bridge.busy.modalOpen = true;
    CONTROL_CHECK(f.api.fails("arrangement.copy", copy(source(a), "full", 0, 1.0), "BUSY", "modal_open"));
    f.bridge.busy.modalOpen = false;
    const auto rev = quoted(f.revision());
    for (const auto& text :
         {"{\"sources\":[],\"mode\":\"full\",\"toTrackId\":0,\"startTimelineSec\":1,\"expectedArrangementRevision\":"
              + rev + "}",
          "{\"sources\":[" + source(a) + "],\"mode\":\"all\",\"toTrackId\":0,\"startTimelineSec\":1,"
              + "\"expectedArrangementRevision\":" + rev + "}",
          "{\"sources\":[" + source(a)
              + "],\"mode\":\"full\",\"startTimelineSec\":1,\"expectedArrangementRevision\":" + rev + "}",
          "{\"sources\":[{\"placementId\":" + quoted(f.pid(a)) + ",\"x\":1}],\"mode\":\"full\",\"toTrackId\":0,"
              + "\"startTimelineSec\":1,\"expectedArrangementRevision\":" + rev + "}",
          "{\"sources\":[" + source(a) + "],\"mode\":\"full\",\"toTrackId\":0,\"startTimelineSec\":1,"
              + "\"expectedArrangementRevision\":" + rev + ",\"dryRun\":true}"})
        CONTROL_CHECK(f.api.fails("arrangement.copy", json(text), "INVALID_ARGUMENT", "invalid_params"));
}

// ---- Scenarios: clips made after a project was opened ----------------------------------------------------------
//
// Opening a project restarts OpenTune's counter for the ids of new clips at 1 while the opened clips keep theirs
// (Control/Ops/ClipIds.h). Every method that makes clips moves the counter past those ids first; these scenarios open
// a saved project and make clips with each method.

std::string logText()
{
    const auto file = AppLogger::getCurrentLogFile();
    return file == juce::File{} ? std::string() : file.loadFileAsString().toStdString();
}

std::size_t occurrences(const std::string& text, const std::string& part)
{
    std::size_t count = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
        ++count;
    return count;
}

/** How many times a method moved the counter past ids in use (the line ClipIds.cpp logs). */
std::size_t skipsLogged()
{
    return occurrences(logText(), "clip id counter was moved past");
}

/** The ids of the clips of the repository, retired clips that are not reclaimed yet included. */
std::set<std::uint64_t> clipIds(OpenTuneAudioProcessor& processor)
{
    std::set<std::uint64_t> ids;
    for (const auto& key : processor.getStandaloneContentRepository()->getAllClips())
        ids.insert(key.objectId);
    return ids;
}

/** The ids of the clips that are not retired. */
std::set<std::uint64_t> liveClipIds(OpenTuneAudioProcessor& processor)
{
    std::set<std::uint64_t> ids;
    const auto* repository = processor.getStandaloneContentRepository();
    for (const auto& key : repository->getAllClips())
        if (!repository->isRetired(key))
            ids.insert(key.objectId);
    return ids;
}

/** How many live clips of the repository no live clip of the arrangement shows: what a clip that OpenTune made and
    did not use leaves behind, listed by project.get as a content without a clip. */
std::size_t unshownContents(Fixture& f)
{
    std::set<ContentKey> shown;
    for (int trackId = 0; trackId < kSlots; ++trackId)
        for (int index = 0; index < f.arrangement().getNumPlacements(trackId); ++index) {
            StandaloneArrangement::Placement placement;
            if (f.arrangement().getPlacementByIndex(trackId, index, placement) && !placement.isRetired)
                shown.insert(placement.contentKey);
        }
    const auto* repository = f.processor->getStandaloneContentRepository();
    std::size_t unshown = 0;
    for (const auto& key : repository->getAllClips())
        if (!repository->isRetired(key) && shown.count(key) == 0)
            ++unshown;
    return unshown;
}

/** Takes the next id from the counter, as a new clip would, and says whether it lies above the id of every clip, that
    is, whether the counter has passed them all. A free id is taken by an empty clip that is deleted again at once.
    The call itself uses up an id, so it only belongs where nothing follows that could depend on the counter: a
    scenario that checks what the counter did to a later call reads the ids of the clips that call made (and the log
    line, written when ids were stepped over) instead. */
bool counterPastEveryClipId(OpenTuneAudioProcessor& processor)
{
    auto* repository = processor.getStandaloneContentRepository();
    std::uint64_t highest = 0;
    for (const auto& key : repository->getAllClips())
        highest = std::max(highest, key.objectId);
    const auto probe = repository->createClip();
    if (!probe.isValid())
        return false;
    repository->retireClip(probe);
    repository->releaseClip(probe);
    return probe.objectId > highest;
}

/** The numeric id at the end of a handle (c:<instance>:<epoch>:<id>, p:...), 0 for text that is not a handle (so a
    scenario whose earlier call failed goes on to fail its checks instead of throwing). */
std::uint64_t idOfHandle(const std::string& handle)
{
    const auto colon = handle.rfind(':');
    if (colon == std::string::npos || colon + 1 >= handle.size())
        return 0;
    return std::strtoull(handle.c_str() + colon + 1, nullptr, 10);
}

std::string mergeOp(const std::string& leading, const std::string& trailing)
{
    return "{\"op\":\"merge\",\"leadingPlacementId\":" + quoted(leading)
           + ",\"trailingPlacementId\":" + quoted(trailing) + "}";
}

std::string splitOp(const std::string& placementHandle, double atTimelineSec)
{
    return "{\"op\":\"split\",\"placementId\":" + quoted(placementHandle)
           + ",\"atTimelineSec\":" + std::to_string(atTimelineSec) + "}";
}

std::string copyParams(Fixture& f, const std::string& sources, const char* mode, int track, double start)
{
    return "{\"sources\":[" + sources + "],\"mode\":" + quoted(mode) + ",\"toTrackId\":" + std::to_string(track)
           + ",\"startTimelineSec\":" + std::to_string(start)
           + ",\"expectedArrangementRevision\":" + quoted(f.revision()) + "}";
}

std::string copySource(const std::string& placementHandle)
{
    return "{\"placementId\":" + quoted(placementHandle) + "}";
}

/** Three clips of six seconds: a and b on track 0 (at 0 s and 7 s) and c on track 1, with the clip ids 1, 2 and 3. */
bool addThreeClips(Fixture& f, Clip& a, Clip& b, Clip& c)
{
    a = f.addClip("a", 0, 0.0);
    b = f.addClip("b", 0, 7.0);
    c = f.addClip("c", 1, 0.0);
    CONTROL_CHECK(a.key.objectId == 1 && b.key.objectId == 2 && c.key.objectId == 3);
    return CONTROL_CHECK(settleBackgroundWork(*f.processor));
}

/** Saves the project and opens it again: the clips keep their ids and the counter for new ids is back at 1. With
    renumberFrom not 0 the clip with that id comes back with the id renumberTo. */
bool reopen(Fixture& f, std::uint64_t renumberFrom = 0, std::uint64_t renumberTo = 0)
{
    const auto epoch = f.processor->getStandaloneContentRepository()->getContentEpoch();
    return CONTROL_CHECK(fixtureReopenWithAnalysis(*f.processor, renumberFrom, renumberTo))
           && CONTROL_CHECK(f.processor->getStandaloneContentRepository()->getContentEpoch() == epoch + 1);
}

/** The id of the content that a placement shows (the placement is given by its handle). */
std::uint64_t contentIdOfPlacement(Fixture& f, const std::string& placementHandle)
{
    return f.locate(idOfHandle(placementHandle)).placement.contentKey.objectId;
}

/** project.get lists as many contents as there are live clips (none without a clip) and as many placements. */
void checkListed(Fixture& f, std::size_t clips, std::size_t placements)
{
    const auto project = f.api.result("project.get");
    CONTROL_CHECK(memberOf(project, "contents").size() == clips
                  && memberOf(project, "placements").size() == placements);
    CONTROL_CHECK(liveClipIds(*f.processor).size() == clips && unshownContents(f) == 0);
}

void scenarioClipIdsAfterOpenImpl()
{
    Fixture f;
    if (!CONTROL_CHECK(f.open()))
        return;
    Clip a;
    Clip b;
    Clip c;
    if (!addThreeClips(f, a, b, c))
        return;
    auto& processor = *f.processor;

    // ---- arrangement.edit split: one call, two new clips, and the ids of the opened clips are skipped.
    if (!reopen(f))
        return;
    CONTROL_CHECK(clipIds(processor) == (std::set<std::uint64_t>{1, 2, 3}));
    const auto skipsBefore = skipsLogged();
    // A refused call and a prediction leave the counter where it was. The counter cannot be read without using up an
    // id (a probe of this scenario would move it), so this is read from what the skip leaves behind: it writes a log
    // line whenever it steps over ids, so a skip by the refused call or the prediction would show as a line here; and
    // the real split below finds the counter still at 1 (it steps over 1 to 3 with one line and probes 4, so the split
    // gets 5 and 6), where a counter that had been moved before would give it higher ids.
    CONTROL_CHECK(f.editFails(f.op("split", c, "\"atTimelineSec\":0.05"), "INVALID_ARGUMENT", "split_out_of_range"));
    auto result = f.edit(f.op("split", c, "\"atTimelineSec\":3"), ",\"dryRun\":true");
    CONTROL_CHECK(memberOf(result, "dryRun").asBoolean() && entities(result, "created").empty());
    CONTROL_CHECK(clipIds(processor) == (std::set<std::uint64_t>{1, 2, 3}) && skipsLogged() == skipsBefore);
    result = f.edit(f.op("split", c, "\"atTimelineSec\":3"));
    const bool splitMade = CONTROL_CHECK(entities(result, "created") == "placement,placement"
                                         && entities(result, "removed") == "placement");
    CONTROL_CHECK(skipsLogged() == skipsBefore + 1);
    checkListed(f, 4, 4);
    if (!splitMade)
        return;
    const auto left = refAt(result, "created", 0, "placementId");
    const auto right = refAt(result, "created", 1, "placementId");
    CONTROL_CHECK(contentIdOfPlacement(f, left) == 5 && contentIdOfPlacement(f, right) == 6);
    // The ids the split took are recorded: splitting the right part again finds nothing in its way, takes no id for a
    // probe (its clips get the next two ids, 7 and 8) and writes no line. This keeps the low ids 1 and 2 in use, so
    // that the calls below, each on a project that was just opened, meet a real collision.
    result = f.edit(splitOp(right, 4.5));
    if (!CONTROL_CHECK(entities(result, "created") == "placement,placement"))
        return;
    CONTROL_CHECK(skipsLogged() == skipsBefore + 1);
    const auto rightLeft = refAt(result, "created", 0, "placementId");
    const auto rightRight = refAt(result, "created", 1, "placementId");
    CONTROL_CHECK(contentIdOfPlacement(f, rightLeft) == 7 && contentIdOfPlacement(f, rightRight) == 8);
    checkListed(f, 5, 5);

    // ---- arrangement.edit merge: the two parts of the right part again.
    if (!reopen(f))
        return;
    CONTROL_CHECK(clipIds(processor) == (std::set<std::uint64_t>{1, 2, 5, 7, 8}));
    const auto merge = mergeOp(f.handle(idOfHandle(rightLeft)), f.handle(idOfHandle(rightRight)));
    // A prediction leaves the counter where it was: no line, and the merge below still gets the id that follows from
    // a counter at 1.
    result = f.edit(merge, ",\"dryRun\":true");
    CONTROL_CHECK(memberOf(result, "dryRun").asBoolean() && skipsLogged() == skipsBefore + 1);
    result = f.edit(merge);
    const bool mergeMade = CONTROL_CHECK(entities(result, "created") == "placement"
                                         && entities(result, "removed") == "placement,placement");
    CONTROL_CHECK(skipsLogged() == skipsBefore + 2);
    checkListed(f, 4, 4);
    if (!mergeMade)
        return;
    // The merge makes one clip, so it needs a probe with a free id and a free id behind it. The counter stands at 1: 1
    // and 2 are in use, 3 is free with 4 behind it. The probe takes 3 and the merged clip gets 4.
    CONTROL_CHECK(contentIdOfPlacement(f, refAt(result, "created", 0, "placementId")) == 4);
    const auto mergedId = idOfHandle(refAt(result, "created", 0, "placementId"));

    // ---- arrangement.copy: both modes, each on a project that was just opened; several sources get one skip.
    if (!reopen(f))
        return;
    result = f.api.result("arrangement.copy", json(copyParams(f, copySource(f.pid(b)), "full", 1, 20.0)));
    CONTROL_CHECK(entities(result, "created") == "placement,content" && skipsLogged() == skipsBefore + 3);
    // The opened clips are 1, 2, 4 and 5; 3 is free but 4 stands behind it, so the probe is 6 and the copy gets 7.
    CONTROL_CHECK(idOfHandle(refAt(result, "created", 1, "contentId")) == 7);
    checkListed(f, 5, 5);
    if (!reopen(f))
        return;
    result = f.api.result(
        "arrangement.copy",
        json(copyParams(f, copySource(f.pid(b)) + "," + copySource(f.handle(mergedId)), "window", 1, 30.0)));
    CONTROL_CHECK(entities(result, "created") == "placement,placement,content,content");
    CONTROL_CHECK(skipsLogged() == skipsBefore + 4);
    // Two sources need two free ids in a row: the opened clips are 1, 2, 4, 5 and 7, so the first such run is 9, 10.
    CONTROL_CHECK(idOfHandle(refAt(result, "created", 2, "contentId")) == 9
                  && idOfHandle(refAt(result, "created", 3, "contentId")) == 10);
    checkListed(f, 7, 7);
    // Nothing is in the way any more.
    result = f.api.result("arrangement.copy", json(copyParams(f, copySource(f.pid(b)), "full", 1, 40.0)));
    CONTROL_CHECK(entities(result, "created") == "placement,content" && skipsLogged() == skipsBefore + 4);
    CONTROL_CHECK(idOfHandle(refAt(result, "created", 1, "contentId")) == 11);
    checkListed(f, 8, 8);

    // ---- track.duplicate, mode clone: the two clips of track 0 are copied, with one skip for both.
    if (!reopen(f))
        return;
    const auto duplicated = f.api.result("track.duplicate", json(R"({"trackId":0,"mode":"clone"})"));
    CONTROL_CHECK(near(memberOf(duplicated, "newTrackId"), 2.0));
    CONTROL_CHECK(entities(duplicated, "created") == "placement,placement,content,content");
    CONTROL_CHECK(skipsLogged() == skipsBefore + 5);
    // Two clones need two free ids in a row: the opened clips are 1, 2, 4, 5, 7, 9, 10 and 11, so they get 13 and 14.
    CONTROL_CHECK(idOfHandle(refAt(duplicated, "created", 2, "contentId")) == 13
                  && idOfHandle(refAt(duplicated, "created", 3, "contentId")) == 14);
    checkListed(f, 10, 10);
}

/** Ids with a hole: 1 and 3 are in use, 2 is free. The split takes two ids; without the skip, the first one meets the
    id 1 and the second one gets the free id 2, which the failed split leaves behind as a clip that no clip shows. */
void scenarioClipIdsHoleImpl()
{
    Fixture f;
    if (!CONTROL_CHECK(f.open()))
        return;
    Clip a;
    Clip b;
    Clip c;
    if (!addThreeClips(f, a, b, c))
        return;
    auto& processor = *f.processor;
    CONTROL_CHECK(
        f.edit("{\"op\":\"delete\",\"placementIds\":[" + quoted(f.pid(b)) + "]}", ",\"confirm\":true").isObject());
    CONTROL_CHECK(settleBackgroundWork(processor));
    if (!reopen(f))
        return;
    CONTROL_CHECK(clipIds(processor) == (std::set<std::uint64_t>{1, 3}));

    const auto skipsBefore = skipsLogged();
    const auto logBefore = occurrences(logText(), "moved past 4 id(s) (2 of them used by clips)");
    const auto result = f.edit(f.op("split", a, "\"atTimelineSec\":3"));
    CONTROL_CHECK(entities(result, "created") == "placement,placement");
    CONTROL_CHECK(skipsLogged() == skipsBefore + 1
                  && occurrences(logText(), "moved past 4 id(s) (2 of them used by clips)") == logBefore + 1);
    // The split needs two free ids in a row. The counter stood at 1: 1 and 3 are in use, 2 is free but 3 stands behind
    // it, and 4 is free with 5 behind it. So it was moved past 1 to 4 (the probes with the free ids 2 and 4 are gone
    // again) and the split got 5 and 6: nothing is left of the clips taken with 2 and 4, and the project has no
    // content without a clip.
    CONTROL_CHECK(liveClipIds(processor) == (std::set<std::uint64_t>{3, 5, 6}));
    for (const auto id : clipIds(processor))
        CONTROL_CHECK(id == 1 || id == 3 || id == 5 || id == 6);
    checkListed(f, 3, 3);
}

/** A project file stores a clip id as a signed 64-bit integer that OpenTune reads back without a range check, so an
    opened clip can have any id: the largest value (a stored -1), or 2^40. The first split after opening must still end
    at once, with the clips of the split at the first free ids and no content without a clip. */
void scenarioClipIdsHugeImpl()
{
    constexpr std::uint64_t kLargest = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t kHuge = std::uint64_t{1} << 40;
    for (const std::uint64_t huge : {kLargest, kHuge}) {
        Fixture f;
        if (!CONTROL_CHECK(f.open()))
            return;
        Clip a;
        Clip b;
        Clip c;
        if (!addThreeClips(f, a, b, c))
            return;
        auto& processor = *f.processor;
        if (!reopen(f, 3, huge))
            return;
        CONTROL_CHECK(clipIds(processor) == (std::set<std::uint64_t>{1, 2, huge}));

        const auto started = juce::Time::getMillisecondCounter();
        const auto result = f.edit(f.op("split", a, "\"atTimelineSec\":3"));
        // The counter stood at 1; 1 and 2 are in use and 3 is free with 4 and 5 behind it: the split gets 4 and 5.
        // The opened clip with the huge id does not matter to it.
        if (!CONTROL_CHECK(entities(result, "created") == "placement,placement"))
            return;
        CONTROL_CHECK(juce::Time::getMillisecondCounter() - started < 20000);
        CONTROL_CHECK(contentIdOfPlacement(f, refAt(result, "created", 0, "placementId")) == 4
                      && contentIdOfPlacement(f, refAt(result, "created", 1, "placementId")) == 5);
        CONTROL_CHECK(liveClipIds(processor) == (std::set<std::uint64_t>{2, 4, 5, huge}));
        checkListed(f, 4, 4);
        // The clip with the huge id keeps counting as a possible collision, so a merge probes once more and still ends
        // at once; its clip gets the next id.
        const auto merge =
            mergeOp(refAt(result, "created", 0, "placementId"), refAt(result, "created", 1, "placementId"));
        const auto merged = f.edit(merge);
        if (!CONTROL_CHECK(entities(merged, "created") == "placement"))
            return;
        CONTROL_CHECK(contentIdOfPlacement(f, refAt(merged, "created", 0, "placementId")) == 7);
        checkListed(f, 3, 3);
    }
}

/** No project was opened: the counter is past every id, so nothing is skipped and the ids are consecutive. */
void scenarioClipIdsWithoutOpenImpl()
{
    Fixture f;
    if (!CONTROL_CHECK(f.open()))
        return;
    Clip a;
    Clip b;
    Clip c;
    if (!addThreeClips(f, a, b, c))
        return;
    auto& processor = *f.processor;
    CONTROL_CHECK(processor.getStandaloneContentRepository()->getContentEpoch() == 0);

    auto result = f.edit(f.op("split", c, "\"atTimelineSec\":3"));
    if (!CONTROL_CHECK(entities(result, "created") == "placement,placement"))
        return;
    const auto left = refAt(result, "created", 0, "placementId");
    const auto right = refAt(result, "created", 1, "placementId");
    CONTROL_CHECK(contentIdOfPlacement(f, left) == 4 && contentIdOfPlacement(f, right) == 5);
    result = f.edit(mergeOp(left, right));
    if (!CONTROL_CHECK(entities(result, "created") == "placement"))
        return;
    CONTROL_CHECK(contentIdOfPlacement(f, refAt(result, "created", 0, "placementId")) == 6);
    result = f.api.result("arrangement.copy", json(copyParams(f, copySource(f.pid(a)), "full", 1, 20.0)));
    if (!CONTROL_CHECK(entities(result, "created") == "placement,content"))
        return;
    CONTROL_CHECK(idOfHandle(refAt(result, "created", 1, "contentId")) == 7);
    result = f.api.result("arrangement.copy", json(copyParams(f, copySource(f.pid(b)), "window", 1, 30.0)));
    if (!CONTROL_CHECK(entities(result, "created") == "placement,content"))
        return;
    CONTROL_CHECK(idOfHandle(refAt(result, "created", 1, "contentId")) == 8);
    result = f.api.result("track.duplicate", json(R"({"trackId":0,"mode":"clone"})"));
    if (!CONTROL_CHECK(entities(result, "created") == "placement,placement,content,content"))
        return;
    CONTROL_CHECK(idOfHandle(refAt(result, "created", 2, "contentId")) == 9
                  && idOfHandle(refAt(result, "created", 3, "contentId")) == 10);
    // Nothing was skipped and no id was taken for it: the highest id is the tenth clip.
    CONTROL_CHECK(skipsLogged() == 0 && *clipIds(processor).rbegin() == 10);
    CONTROL_CHECK(counterPastEveryClipId(processor));
}

/** The functions of ClipIds.h on a repository that holds ids with holes and a retired clip that was not reclaimed. */
void scenarioClipIdSkipImpl()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    ApiSession api;
    if (!service.isRunning() || !api.open(service))
        return;
    TestUiBridge bridge;
    ControlThreadProbe probe;
    ControlInstanceFacts facts;
    facts.instanceId = memberOf(api.discovery(), "instanceId").text();
    ControlOpContext context(*processor, &bridge, facts, probe);
    auto* repository = processor->getStandaloneContentRepository();

    // No content epoch: the counter is past every id, nothing is skipped, and no id is taken.
    CONTROL_CHECK(repository->getContentEpoch() == 0 && controlPossibleClipIdCollisions(context) == 0);
    CONTROL_CHECK(controlSkipUsedClipIds(context, "test", 1) == 0);
    auto first = repository->createClip();
    CONTROL_CHECK(first.objectId == 1);
    repository->retireClip(first);
    repository->releaseClip(first);

    // An opened project: clips 1 and 2, a hole at 3, and clip 4, which is retired and not reclaimed (createClip()
    // refuses its id as well). The counter is back at 1.
    repository->clear();
    CONTROL_CHECK(repository->getContentEpoch() == 1);
    CONTROL_CHECK(repository->createClip(1).isValid() && repository->createClip(2).isValid());
    const auto retired = repository->createClip(4);
    CONTROL_CHECK(retired.isValid() && repository->retireClip(retired) && repository->isRetired(retired));
    CONTROL_CHECK(controlPossibleClipIdCollisions(context) == 3);

    // No clip to make: nothing is done and the counter stays where it was (the call below starts at 1).
    CONTROL_CHECK(controlSkipUsedClipIds(context, "test", 0) == 0);
    // One clip to make. The probes: 1 and 2 are in use, 3 is free but 4 stands behind it, 4 is in use, and 5 is free
    // with 6 behind it free. That is 5 probes, 3 of them met an id in use.
    const auto skippedFirst = controlSkipUsedClipIds(context, "test", 1);
    CONTROL_CHECK(skippedFirst == 5);
    CONTROL_CHECK(clipIds(*processor) == (std::set<std::uint64_t>{1, 2, 4}));
    CONTROL_CHECK(repository->getRetiredClips().size() == 1 && controlPossibleClipIdCollisions(context) == 0);
    CONTROL_CHECK(logText().find("test: OpenTune's clip id counter was moved past 5 id(s) (3 of them used by clips) "
                                 "so that the 1 new clip id(s) are free")
                  != std::string::npos);
    // The next clip gets 6: the probe took 5 and was deleted. Nothing is skipped or taken when no id is in the way.
    const auto next = repository->createClip();
    CONTROL_CHECK(next.objectId == 6);
    controlRecordCommittedClipId(context, next);
    CONTROL_CHECK(controlSkipUsedClipIds(context, "test", 1) == 0);
    const auto after = repository->createClip();
    CONTROL_CHECK(after.objectId == 7);
    repository->retireClip(next);
    repository->releaseClip(next);
    repository->retireClip(after);
    repository->releaseClip(after);

    // A clip with a higher id appears (the editor made it, or a project was merged in): the counter stands at 8, whose
    // run is blocked by 9; 9 is in use; 10 is free with 11 behind it.
    CONTROL_CHECK(repository->createClip(9).isValid());
    CONTROL_CHECK(controlPossibleClipIdCollisions(context) == 1);
    CONTROL_CHECK(controlSkipUsedClipIds(context, "test", 1) == 3);
    CONTROL_CHECK(clipIds(*processor) == (std::set<std::uint64_t>{1, 2, 4, 9}));

    // Ids with a hole, for a split (two clips): 1 and 3 are in use. 1 is in use, 2 is free but 3 stands behind it, 3
    // is in use, 4 is free with 5 and 6 behind it. Another content epoch forgets what was recorded, and the counter
    // is back at 1.
    repository->clear();
    CONTROL_CHECK(repository->createClip(1).isValid() && repository->createClip(3).isValid());
    CONTROL_CHECK(controlPossibleClipIdCollisions(context) == 2);
    CONTROL_CHECK(controlSkipUsedClipIds(context, "test", 2) == 4);
    CONTROL_CHECK(clipIds(*processor) == (std::set<std::uint64_t>{1, 3}));
    const auto firstOfTwo = repository->createClip();
    const auto secondOfTwo = repository->createClip();
    CONTROL_CHECK(firstOfTwo.objectId == 5 && secondOfTwo.objectId == 6);
    for (const auto& key : {firstOfTwo, secondOfTwo}) {
        repository->retireClip(key);
        repository->releaseClip(key);
    }

    // A project file stores an id as a signed 64-bit integer that is read back without a range check, so a clip can
    // have any id: 2^40, or the largest value (a stored -1). The search must not depend on how high the ids lie (it
    // used to count up to the highest id), and must not run over the end of the numbers.
    repository->clear();
    constexpr std::uint64_t kHuge = std::uint64_t{1} << 40;
    constexpr std::uint64_t kLargest = std::numeric_limits<std::uint64_t>::max();
    CONTROL_CHECK(repository->createClip(1).isValid() && repository->createClip(kHuge).isValid()
                  && repository->createClip(kLargest).isValid());
    CONTROL_CHECK(controlPossibleClipIdCollisions(context) == 3);
    const auto started = juce::Time::getMillisecondCounter();
    // 1 is in use and 2 is free with 3 to 5 behind it: two probes.
    CONTROL_CHECK(controlSkipUsedClipIds(context, "test", 3) == 2);
    CONTROL_CHECK(controlSkipUsedClipIds(context, "test", 1) == 1);
    CONTROL_CHECK(juce::Time::getMillisecondCounter() - started < 5000);
    CONTROL_CHECK(clipIds(*processor) == (std::set<std::uint64_t>{1, kHuge, kLargest}));
    // The two big clips stay above what was recorded, so every later call probes once more: one clip, no more.
    CONTROL_CHECK(controlPossibleClipIdCollisions(context) == 2);
    // The probe of the second call took 3 and was deleted again, so the next clip gets 4.
    CONTROL_CHECK(repository->createClip().objectId == 4);

    // Another content epoch forgets what was recorded.
    repository->clear();
    CONTROL_CHECK(repository->createClip(2).isValid());
    CONTROL_CHECK(controlPossibleClipIdCollisions(context) == 1);
}

// ---- Scenario: the specification's vectors ---------------------------------------------------------------

/** Every vector of arrangement/, and the track deletion whose arrangement revision an arrangement.edit made stale. */
const char* const kMustRunVectors[] = {
    "arrangement/copy-arrangement-changed",
    "arrangement/copy-full",
    "arrangement/copy-not-found",
    "arrangement/copy-window",
    "arrangement/edit-arrangement-changed",
    "arrangement/edit-delete-confirm-required",
    "arrangement/edit-delete",
    "arrangement/edit-merge-not-possible",
    "arrangement/edit-move-hidden-track",
    "arrangement/edit-move",
    "arrangement/edit-not-found",
    "arrangement/edit-set-fade",
    "arrangement/edit-set-gain",
    "arrangement/edit-split-merge",
    "arrangement/edit-split-not-ready",
    "arrangement/edit-split-out-of-range",
    "arrangement/edit-split-undo-refused",
    "arrangement/edit-split",
    "arrangement/edit-trim-fades",
    "arrangement/edit-trim-out-of-range",
    "arrangement/edit-trim-start",
    "track/delete-arrangement-changed",
};

const std::set<std::string> kVectorMethods = {"arrangement.edit", "arrangement.copy", "edit.getHistory", "edit.undo",
                                              "edit.redo",        "project.get",      "track.delete"};
const std::set<std::string> kVectorCapabilities = {"session.v1", "read.v1", "history.v1", "tracks.v1",
                                                   "arrangement.v1"};

/** Why this runner does not run a vector, or empty when it does. */
std::string vectorSkipReason(const JsonValue& vector)
{
    const auto id = memberOf(vector, "id").text();
    if (memberOf(vector, "capability").text() != "arrangement.v1" && id != "track/delete-arrangement-changed")
        return "covers capability " + memberOf(vector, "capability").text() + ", which another runner covers";
    const auto& extra = memberOf(vector, "extraCapabilities");
    for (std::size_t i = 0; i < extra.size(); ++i)
        if (kVectorCapabilities.count(extra.at(i).text()) == 0)
            return "needs capability " + extra.at(i).text() + ", which this runner does not cover";
    if (memberOf(vector, "session").text() != "authenticated")
        return "unauthenticated vectors test the listener (ControlUnitTests)";
    std::vector<const JsonValue*> requests;
    if (const JsonValue* request = vector.find("request"))
        requests.push_back(request);
    if (const JsonValue* steps = vector.find("steps"))
        for (std::size_t i = 0; i < steps->size(); ++i) {
            if (steps->at(i).find("connection") != nullptr)
                return "uses steps other than send, expect and pauseMs on the main connection";
            if (const JsonValue* send = steps->at(i).find("send"))
                requests.push_back(send);
        }
    for (const auto* request : requests)
        if (kVectorMethods.count(memberOf(*request, "method").text()) == 0)
            return "uses " + memberOf(*request, "method").text() + ", which another runner covers";
    const auto& given = memberOf(vector, "given");
    for (std::size_t i = 0; i < given.size(); ++i)
        if (given.keyAt(i) != "dirty" && given.keyAt(i) != "clips" && given.keyAt(i) != "visibleTrackCount")
            return "precondition given." + given.keyAt(i) + " is not established by this runner";
    const auto& clips = memberOf(given, "clips");
    for (std::size_t c = 0; c < clips.size(); ++c) {
        const auto analysis = memberOf(clips.at(c), "analysis").text();
        if (analysis != "ready" && analysis != "running")
            return "a clip with analysis " + analysis + " is not established by this runner";
    }
    return {};
}

std::string runVector(const JsonValue& vector)
{
    Fixture f;
    if (!f.open())
        return "the control API did not start";
    const auto& given = memberOf(vector, "given");
    if (const JsonValue* shown = given.find("visibleTrackCount"))
        f.bridge.visibleTracks = shown->isString() ? kSlots : static_cast<int>(shown->asDouble());
    VectorMatcher matcher;
    std::vector<std::pair<std::string, ClipSpec>> specs;
    const auto& clips = memberOf(given, "clips");
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto& item = clips.at(i);
        ClipSpec spec;
        spec.name = memberOf(item, "bind").text();
        spec.trackId = static_cast<int>(memberOf(item, "trackId").asDouble());
        spec.startTimelineSec = memberOf(item, "startTimelineSec").asDouble();
        spec.durationSec = memberOf(item, "durationSec").asDouble();
        spec.analysis = memberOf(item, "analysis").text() == "running" ? Analysis::Running : Analysis::Ready;
        const auto clip = fixtureAddClip(*f.processor, spec);
        if (const JsonValue* fadeOut = item.find("fadeOutSec"))
            if (!f.arrangement().setPlacementFade(spec.trackId, clip.placementId, 0.0, fadeOut->asDouble()))
                return "the fades of " + spec.name + " could not be set";
        specs.emplace_back(spec.name, spec);
    }
    CONTROL_CHECK(settleBackgroundWork(*f.processor));
    f.bridge.project.dirty = memberOf(given, "dirty").asBoolean();

    const auto& discovery = f.api.discovery();
    for (std::size_t i = 0; i < discovery.size(); ++i)
        matcher.bindings["discovery." + discovery.keyAt(i)] = discovery.at(i);
    const auto project = f.api.result("project.get");
    matcher.bindings["runner.arrangementRevision"] =
        JsonValue::string(memberOf(memberOf(project, "asOf"), "arrangementRevision").text());
    matcher.bindings["runner.unknownPlacementId"] = JsonValue::string(f.handle(4294967295ULL));
    for (const auto& [bind, spec] : specs) {
        const auto handles = fixtureHandlesOf(project, spec.trackId, spec.startTimelineSec);
        if (!handles)
            return "the clip " + bind + " is not in project.get";
        matcher.bindings[bind + ".contentId"] = JsonValue::string(handles->contentId);
        matcher.bindings[bind + ".placementId"] = JsonValue::string(handles->placementId);
        matcher.bindings[bind + ".geometryHash"] = JsonValue::string(handles->geometryHash);
    }

    std::string failure;
    runWhilePumping(
        [&] {
            auto& connection = f.api.client();
            if (memberOf(vector, "type").text() == "request") {
                const auto request = matcher.substitute(memberOf(vector, "request"), failure);
                if (!failure.empty())
                    return;
                if (!connection.sendLine(toJsonString(request))) {
                    failure = "cannot send the request";
                    return;
                }
                failure = matcher.expectLine(connection, memberOf(vector, "response"));
                if (failure.empty() && memberOf(vector, "connectionAfter").text() == "open"
                    && !connection.staysQuiet(kOpenCheckMs))
                    failure = "the connection closed or sent more";
                return;
            }
            const auto& steps = memberOf(vector, "steps");
            for (std::size_t i = 0; i < steps.size() && failure.empty(); ++i) {
                const auto& step = steps.at(i);
                if (const JsonValue* send = step.find("send")) {
                    const auto value = matcher.substitute(*send, failure);
                    if (failure.empty() && !connection.sendLine(toJsonString(value)))
                        failure = "send failed";
                } else if (const JsonValue* pause = step.find("pauseMs")) {
                    juce::Thread::sleep(static_cast<int>(pause->asDouble()));
                } else {
                    failure = matcher.expectLine(connection, memberOf(step, "expect"));
                }
                if (!failure.empty())
                    failure = "step " + std::to_string(i + 1) + ": " + failure;
            }
        },
        kVectorExchangeMs);
    return failure;
}

void scenarioArrangementVectorsImpl()
{
    const auto root = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root / "arrangement"))
        if (entry.path().extension() == ".json")
            files.push_back(entry.path());
    files.push_back(root / "track" / "delete-arrangement-changed.json");
    std::sort(files.begin(), files.end());
    std::set<std::string> passedIds;
    int skipped = 0;
    VectorReport report("ControlProcessorTests.ArrangementVectors");
    for (const auto& file : files) {
        const auto parsed = parseJson(readFile(file));
        if (!CONTROL_CHECK(parsed.ok))
            continue;
        const auto id = memberOf(parsed.value, "id").text();
        if (const auto reason = vectorSkipReason(parsed.value); !reason.empty()) {
            ++skipped;
            report.skip(id, reason);
            note("SKIP " + id + ": " + reason);
            continue;
        }
        const auto started = Clock::now();
        const auto failure = runVector(parsed.value);
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty()))
            passedIds.insert(id);
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    for (const char* id : kMustRunVectors)
        CONTROL_CHECK(passedIds.count(id) == 1);
    note(std::to_string(passedIds.size()) + " vectors passed, " + std::to_string(skipped) + " skipped");
}

} // namespace

void scenarioArrangement()
{
    scenarioArrangementImpl();
}

void scenarioArrangementUndo()
{
    scenarioArrangementUndoImpl();
}

void scenarioArrangementCopy()
{
    scenarioArrangementCopyImpl();
}

void scenarioArrangementVectors()
{
    scenarioArrangementVectorsImpl();
}

void scenarioClipIdSkip()
{
    scenarioClipIdSkipImpl();
}

void scenarioClipIdsAfterOpen()
{
    scenarioClipIdsAfterOpenImpl();
    scenarioClipIdsHoleImpl();
    scenarioClipIdsHugeImpl();
}

void scenarioClipIdsWithoutOpen()
{
    scenarioClipIdsWithoutOpenImpl();
}

} // namespace ControlTest
} // namespace OpenTune
