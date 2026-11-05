// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scenarios of the time grid (content.shiftTiming, content.editTimeGrid) against the real processor without a window
// or an audio device, with a test bridge for the dirty flag, the busy state and the editor's notification. Contents
// get synthetic F0 (ControlContentFixtures.h); their time grids are set the way OpenTune's content owner applies one
// (StandaloneClipContent::applyTimeGrid). The timing analysis that seeding needs is OpenTune's own, or, where a
// scenario needs it at a known place or in a known state, a feature set given to the content the way the
// processor stores a finished analysis (OpenTuneAudioProcessor::setContentReferenceFeatures). Registered with the
// other processor scenarios in ControlProcessorTests.cpp.

#include "ControlContentFixtures.h"
#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneClipContent.h"
#include "Content/StandaloneContentRepository.h"
#include "Control/ControlJobs.h"
#include "Control/Edit/TimeGridOps.h"
#include "Control/Ops/OpsRegistry.h"
#include "Control/Ops/OpsSupport.h"
#include "Control/Ops/TimingOps.h"
#include "PluginProcessor.h"
#include "Utils/TimeGrid.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

namespace OpenTune {
namespace ControlTest {

namespace {

/** Bound of one vector's exchange while this thread pumps messages. */
constexpr int kVectorExchangeMs = 60000;
/** How long a scenario waits for a job to reach a state. */
constexpr int kJobStateTimeoutMs = 20000;
/** The wait limit of a seed job in the scenario that lets it run out. */
constexpr int kSeedTestWaitLimitMs = 600;

/** Sets the seed jobs' wait limit for its lifetime and restores the default (0) when it goes out of scope, also when
    a failed check ends the scope early. */
struct SeedWaitLimitGuard
{
    explicit SeedWaitLimitGuard(int milliseconds) { controlSetSeedWaitLimitForTesting(milliseconds); }
    ~SeedWaitLimitGuard() { controlSetSeedWaitLimitForTesting(0); }
    SeedWaitLimitGuard(const SeedWaitLimitGuard&) = delete;
    SeedWaitLimitGuard& operator=(const SeedWaitLimitGuard&) = delete;
};

/** Tolerance of hand-computed output times. */
constexpr double kOutputTolerance = 1e-9;
/** Tolerance of output times read back from results, which round seconds to 6 decimals. */
constexpr double kResultTolerance = 1e-6;
/** The description of a handle move, the text the time tool gives its drag: LOC_RAW("Drag time handle"), in the
    language of the window. */
std::string moveDescription()
{
    return Loc::get(testLanguageState()->language, "Drag time handle").toStdString();
}

JsonValue jsonOf(const std::string& text)
{
    auto parsed = parseJson(text);
    CONTROL_CHECK(parsed.ok);
    return parsed.value;
}

std::string jsonQuoted(const std::string& text)
{
    std::string out;
    appendJsonQuoted(out, text);
    return out;
}

std::string sec(double value)
{
    return toJsonString(controlJsonSeconds(value));
}

bool near(const JsonValue& value, double expected, double tolerance = kResultTolerance)
{
    return value.isNumber() && std::abs(value.asDouble() - expected) <= tolerance;
}

/** Registers a test bridge with the processor's service for as long as it lives. */
class ScopedTestBridge
{
public:
    ScopedTestBridge(ControlService& service, TestUiBridge& bridge) : service_(service)
    {
        service_.setUiBridge(&bridge);
    }
    ~ScopedTestBridge() { service_.setUiBridge(nullptr); }

private:
    ControlService& service_;
};

/** A clip of 6 s on track 0 from 2 s with two notes, and a time grid with handles at 1, 2 and 3 s of the content
    when gridded (output times equal content times). */
ClipSpec gridSpec(const std::string& name, bool gridded, int trackId = 0)
{
    ClipSpec spec;
    spec.name = name;
    spec.trackId = trackId;
    spec.startTimelineSec = 2.0;
    spec.durationSec = 6.0;
    spec.notes = {{0.5, 1.5, 64.0}, {2.0, 3.0, 69.0}};
    if (gridded)
        spec.gridHandlesContentSec = {1.0, 2.0, 3.0};
    return spec;
}

/** The handles of a content's time grid between its end points, from its current snapshot. */
std::vector<TimeHandle> innerOf(OpenTuneAudioProcessor& processor, const ContentKey& key)
{
    const auto snapshot = processor.getContentSnapshot(key);
    if (snapshot == nullptr || snapshot->timeGrid == nullptr)
        return {};
    return TimeGridOps::innerHandles(*snapshot->timeGrid);
}

bool sameHandles(const std::vector<TimeHandle>& a, const std::vector<TimeHandle>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].source_seconds != b[i].source_seconds
            || a[i].output_seconds != b[i].output_seconds || a[i].kind != b[i].kind)
            return false;
    return true;
}

/** The tokens of a content as project.get reports them right now (asOf.contents). */
JsonValue freshTokens(ApiSession& api, const std::string& contentId)
{
    const auto project = api.result("project.get");
    const auto& contents = memberOf(memberOf(project, "asOf"), "contents");
    for (std::size_t i = 0; i < contents.size(); ++i)
        if (memberOf(contents.at(i), "contentId").text() == contentId)
            return contents.at(i);
    return JsonValue();
}

/** The tokens of the content in a write result. */
JsonValue resultTokens(const JsonValue& result, const std::string& contentId)
{
    const auto& contents = memberOf(memberOf(result, "tokens"), "contents");
    for (std::size_t i = 0; i < contents.size(); ++i)
        if (memberOf(contents.at(i), "contentId").text() == contentId)
            return contents.at(i);
    return JsonValue();
}

std::size_t warningCount(const JsonValue& result, const std::string& code)
{
    std::size_t count = 0;
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        if (memberOf(warnings.at(i), "code").text() == code)
            ++count;
    return count;
}

bool isErrorOf(const JsonValue& response, const std::string& kind, const std::string& reason, bool retryable)
{
    const JsonValue* data = errorData(response);
    const bool ok = data != nullptr && memberOf(*data, "kind").text() == kind
                    && memberOf(*data, "reason").text() == reason && memberOf(*data, "retryable").isBoolean()
                    && memberOf(*data, "retryable").asBoolean() == retryable && memberOf(*data, "executed").isBoolean()
                    && !memberOf(*data, "executed").asBoolean();
    if (!ok)
        note("expected " + kind + "(" + reason + "), got " + toJsonString(response));
    return ok;
}

/** A timing analysis of the content given the way the processor stores a finished one: status, the producer this
    instance uses (standard AUTO: the processor of a test has no editor to hand it another mode), the content's audio
    revision as fingerprint, and onsets at the given source seconds. */
bool setTimingFeatures(OpenTuneAudioProcessor& processor, const ContentKey& key, ReferenceFeatureStatus status,
                       const std::vector<double>& onsetsSec)
{
    const auto snapshot = processor.getContentSnapshot(key);
    if (snapshot == nullptr)
        return false;
    ReferenceFeatureSet features;
    features.status = status;
    features.producer = ReferenceFeatureProducer::StandardAuto;
    features.inputFingerprint = static_cast<std::int64_t>(snapshot->audioRevision);
    features.analysisRevision = snapshot->referenceFeatures.analysisRevision + 1;
    features.sourceDurationSeconds = snapshot->timeGrid != nullptr ? snapshot->timeGrid->totalDurationSeconds() : 0.0;
    std::uint64_t nextId = 1;
    for (const double onset : onsetsSec) {
        ReferenceTimingAnchor anchor;
        anchor.anchorId = nextId++;
        anchor.sourceSeconds = onset;
        anchor.strength = 1.0f;
        anchor.kind = ReferenceTimingAnchorKind::Onset;
        anchor.confidence = 1.0f;
        features.timing.anchors.push_back(anchor);
    }
    return processor.setContentReferenceFeatures(key, features);
}

/** A started processor with a test bridge, a session and one clip. */
struct TimingFixture
{
    std::unique_ptr<OpenTuneAudioProcessor> processor;
    TestUiBridge bridge;
    std::unique_ptr<ScopedTestBridge> registered;
    ApiSession api;
    Clip clip;
    std::string contentId;
    std::string instanceShort;

    bool open(const ClipSpec& spec)
    {
        processor = startedProcessor();
        auto& service = processor->getControlService();
        if (!service.isRunning())
            return false;
        registered = std::make_unique<ScopedTestBridge>(service, bridge);
        if (!api.open(service))
            return false;
        instanceShort = memberOf(api.discovery(), "instanceId").text().substr(0, 8);
        const auto added = add(spec);
        if (!added)
            return false;
        clip = added->first;
        contentId = added->second;
        return true;
    }

    /** Adds another clip; its contentId as project.get reports it. */
    std::optional<std::pair<Clip, std::string>> add(const ClipSpec& spec)
    {
        auto added = fixtureAddClip(*processor, spec);
        // The render that importing the clip requested is over before the first timed request.
        CONTROL_CHECK(settleBackgroundWork(*processor));
        const auto handles = fixtureHandlesOf(api.result("project.get"), spec.trackId, spec.startTimelineSec);
        if (!CONTROL_CHECK(handles.has_value()))
            return std::nullopt;
        return std::make_pair(added, handles->contentId);
    }

    ControlService& service() { return processor->getControlService(); }
    std::string gridRevision(const std::string& id) { return memberOf(freshTokens(api, id), "gridRevision").text(); }
    std::string gridRevision() { return gridRevision(contentId); }

    std::string shiftParams(const std::string& range, double deltaSec, const std::string& extra = {})
    {
        return "{\"contentId\":" + jsonQuoted(contentId) + ",\"expectedGridRevision\":" + jsonQuoted(gridRevision())
               + ",\"range\":" + range + ",\"deltaSec\":" + sec(deltaSec) + extra + "}";
    }

    std::string movesParams(const std::vector<std::pair<std::string, double>>& moves, const std::string& extra = {})
    {
        std::string list;
        for (const auto& [handleId, outputSec] : moves)
            list += (list.empty() ? "" : ",") + std::string("{\"handleId\":") + jsonQuoted(handleId)
                    + ",\"outputSec\":" + sec(outputSec) + "}";
        return "{\"contentId\":" + jsonQuoted(contentId) + ",\"op\":\"moveHandles\",\"expectedGridRevision\":"
               + jsonQuoted(gridRevision()) + ",\"moves\":[" + list + "]" + extra + "}";
    }

    JsonValue jobOnceTerminal(const std::string& jobId)
    {
        return memberOf(api.result("job.get", jsonOf("{\"jobId\":" + jsonQuoted(jobId) + ",\"waitMs\":30000}")), "job");
    }

    /** Starts a seed of the content; its job id, or empty when the call did not hand the work to a job. */
    std::string startSeed(const std::string& id)
    {
        const auto start =
            api.result("content.editTimeGrid", jsonOf("{\"contentId\":" + jsonQuoted(id) + ",\"op\":\"seed\"}"));
        const auto& job = memberOf(start, "job");
        if (!CONTROL_CHECK(job.isObject() && memberOf(job, "kind").text() == "timeGridSeed"))
            return {};
        // The state "handed to a job": nothing changed yet (PROTOCOL.md section 14.2).
        CONTROL_CHECK(!memberOf(start, "applied").asBoolean() && !memberOf(start, "noOp").asBoolean()
                      && memberOf(start, "undo").isNull() && memberOf(start, "changes").size() == 0
                      && memberOf(start, "clamped").size() == 0);
        CONTROL_CHECK(toJsonString(memberOf(start, "revisionDiff"))
                      == R"({"arrangement":null,"trackState":null,"contents":[]})");
        return memberOf(job, "jobId").text();
    }

    /** The job from the registry, without a request. */
    JsonValue jobOf(const std::string& jobId)
    {
        JsonValue job;
        service().jobs()->get(jobId, job);
        return job;
    }

    bool waitJobState(const std::string& jobId, const std::string& state)
    {
        return pumpMessagesUntil([&] { return memberOf(jobOf(jobId), "state").text() == state; }, kJobStateTimeoutMs);
    }
};

// ---- Hand-computed handle moves -------------------------------------------------------------------------

/** One write of the table: its params and the output times it must give the three handles at 1, 2 and 3 s. */
struct GridCase
{
    const char* name;
    /** content.shiftTiming with range and deltaSec, else content.editTimeGrid moveHandles with moves. */
    bool shift = true;
    std::string range;
    double deltaSec = 0.0;
    /** Index of the handle (0..2) and its requested output time. */
    std::vector<std::pair<int, double>> moves;
    /** Output times of the three handles after the write. */
    std::array<double, 3> expected{};
    /** Index of the handle and the output time the request asked for, for each limited move. */
    std::vector<std::pair<int, double>> clamped;
};

/**
 * The cases, computed by hand from the time tool's rules (PianoRollToolHandler::handleTimeToolMouseDrag): a moved
 * handle stays 30 ms (TimeGridSnapshot::kMinOutputSpacingSeconds) from its neighbours; a section moves by the
 * distance its leading handle may move; several moves are limited by their neighbours' targets, then in order.
 * The grid has ClipStart at 0, the handles at 1, 2 and 3 s and ClipEnd at 6 s, output equal to content.
 */
std::vector<GridCase> gridCases()
{
    const std::string middle = R"({"startOutputSec":1.5,"endOutputSec":2.5})";
    const std::string firstTwo = R"({"startOutputSec":0.5,"endOutputSec":2.5})";
    std::vector<GridCase> cases;
    cases.push_back({"shift one handle 30 ms later", true, middle, 0.03, {}, {1.0, 2.03, 3.0}, {}});
    cases.push_back({"shift one handle past its right neighbour", true, middle, 1.5, {}, {1.0, 2.97, 3.0}, {{1, 3.5}}});
    cases.push_back({"shift one handle past its left neighbour", true, middle, -1.5, {}, {1.0, 1.03, 3.0}, {{1, 0.5}}});
    cases.push_back({"shift a section of two handles", true, firstTwo, 0.5, {}, {1.5, 2.5, 3.0}, {}});
    // The range is a closed interval (start <= position <= end, as the time tool selects the handles): a handle exactly
    // at the start and a handle exactly at the end both move. A half-open range would leave one of them where it was.
    cases.push_back({"shift a section whose range starts on a handle",
                     true,
                     R"({"startOutputSec":1.0,"endOutputSec":2.5})",
                     0.5,
                     {},
                     {1.5, 2.5, 3.0},
                     {}});
    cases.push_back({"shift a section whose range ends on a handle",
                     true,
                     R"({"startOutputSec":0.5,"endOutputSec":2.0})",
                     0.5,
                     {},
                     {1.5, 2.5, 3.0},
                     {}});
    cases.push_back({"shift a section whose range starts and ends on handles",
                     true,
                     R"({"startOutputSec":1.0,"endOutputSec":2.0})",
                     0.5,
                     {},
                     {1.5, 2.5, 3.0},
                     {}});
    cases.push_back({"shift a section until its leading handle meets the neighbour",
                     true,
                     firstTwo,
                     2.0,
                     {},
                     {1.97, 2.97, 3.0},
                     {{0, 3.0}, {1, 4.0}}});
    cases.push_back({"shift a section earlier until it meets ClipStart",
                     true,
                     firstTwo,
                     -2.0,
                     {},
                     {0.03, 1.03, 3.0},
                     {{0, -1.0}, {1, 0.0}}});
    cases.push_back({"shift a content range",
                     true,
                     R"({"startContentSec":1.5,"endContentSec":2.5})",
                     0.2,
                     {},
                     {1.0, 2.2, 3.0},
                     {}});
    cases.push_back(
        {"shift every handle", true, R"({"startOutputSec":0,"endOutputSec":6})", 0.5, {}, {1.5, 2.5, 3.5}, {}});
    cases.push_back({"move one handle", false, {}, 0.0, {{0, 1.1}}, {1.1, 2.0, 3.0}, {}});
    cases.push_back({"move one handle past its neighbour", false, {}, 0.0, {{0, 2.5}}, {1.97, 2.0, 3.0}, {{0, 2.5}}});
    cases.push_back({"move one handle past ClipEnd", false, {}, 0.0, {{2, 7.0}}, {1.0, 2.0, 5.97}, {{2, 7.0}}});
    cases.push_back({"move two handles the same way", false, {}, 0.0, {{0, 2.2}, {1, 2.6}}, {2.2, 2.6, 3.0}, {}});
    cases.push_back({"move two handles across each other",
                     false,
                     {},
                     0.0,
                     {{0, 2.5}, {1, 1.5}},
                     {1.47, 2.53, 3.0},
                     {{0, 2.5}, {1, 1.5}}});
    return cases;
}

/** Runs one case of the table on the fixture's grid and undoes it: the result describes exactly the hand-computed
    change, the tokens equal a fresh read, and the undo step gives back the grid before. */
void runGridCase(TimingFixture& f, const GridCase& gridCase)
{
    note(std::string("case: ") + gridCase.name);
    const auto before = innerOf(*f.processor, f.clip.key);
    if (!CONTROL_CHECK(before.size() == 3))
        return;
    const auto tokensBefore = freshTokens(f.api, f.contentId);
    const int notified = f.bridge.timeGridNotifications;
    const int undoCursor = f.processor->getUndoManager().cursor();
    std::vector<std::pair<std::string, double>> moves;
    for (const auto& [index, outputSec] : gridCase.moves)
        moves.emplace_back(f.clip.handleIds[static_cast<std::size_t>(index)], outputSec);
    const auto result =
        gridCase.shift ? f.api.result("content.shiftTiming", jsonOf(f.shiftParams(gridCase.range, gridCase.deltaSec)))
                       : f.api.result("content.editTimeGrid", jsonOf(f.movesParams(moves)));

    // The grid: the hand-computed output times, the content times and ids unchanged.
    const auto after = innerOf(*f.processor, f.clip.key);
    if (!CONTROL_CHECK(after.size() == 3))
        return;
    for (std::size_t i = 0; i < 3; ++i) {
        if (!CONTROL_CHECK(std::abs(after[i].output_seconds - gridCase.expected[i]) <= kOutputTolerance))
            note("handle " + std::to_string(i) + " at " + std::to_string(after[i].output_seconds));
        CONTROL_CHECK(after[i].id == before[i].id && after[i].source_seconds == before[i].source_seconds);
    }
    // changes: one entry per moved handle, with its output times.
    std::size_t movedCount = 0;
    for (std::size_t i = 0; i < 3; ++i)
        if (after[i].output_seconds != before[i].output_seconds)
            ++movedCount;
    const auto& changes = memberOf(result, "changes");
    CONTROL_CHECK(changes.size() == movedCount);
    for (std::size_t c = 0; c < changes.size(); ++c) {
        const auto& change = changes.at(c);
        const auto handleId = memberOf(memberOf(change, "ref"), "handleId").text();
        const auto at = std::find(f.clip.handleIds.begin(), f.clip.handleIds.end(), handleId);
        if (!CONTROL_CHECK(at != f.clip.handleIds.end() && memberOf(change, "entity").text() == "timeHandle"))
            continue;
        const auto i = static_cast<std::size_t>(at - f.clip.handleIds.begin());
        const auto& field = memberOf(memberOf(change, "fields"), "outputSec");
        CONTROL_CHECK(near(memberOf(field, "before"), before[i].output_seconds)
                      && near(memberOf(field, "after"), gridCase.expected[i])
                      && memberOf(field, "unit").text() == "sec");
        CONTROL_CHECK(memberOf(memberOf(change, "ref"), "contentId").text() == f.contentId);
    }
    // clamped and CLAMPED: one per limited move, with the requested and the applied time.
    const auto& clamped = memberOf(result, "clamped");
    CONTROL_CHECK(clamped.size() == gridCase.clamped.size()
                  && warningCount(result, "CLAMPED") == gridCase.clamped.size());
    for (std::size_t c = 0; c < clamped.size() && c < gridCase.clamped.size(); ++c) {
        const auto index = static_cast<std::size_t>(gridCase.clamped[c].first);
        CONTROL_CHECK(memberOf(clamped.at(c), "handleId").text() == f.clip.handleIds[index]);
        CONTROL_CHECK(near(memberOf(clamped.at(c), "requestedOutputSec"), gridCase.clamped[c].second));
        CONTROL_CHECK(near(memberOf(clamped.at(c), "appliedOutputSec"), gridCase.expected[index]));
    }
    // One undo step of the time tool's kind; only the grid aspect changes; rendering queued; dirty; the editor told.
    CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean());
    const auto& undo = memberOf(result, "undo");
    CONTROL_CHECK(memberOf(undo, "undoable").asBoolean() && memberOf(undo, "steps").asDouble() == 1
                  && memberOf(undo, "description").text() == moveDescription()
                  && toJsonString(memberOf(undo, "innerActions")) == R"(["TimeGridEditAction"])");
    CONTROL_CHECK(f.processor->getUndoManager().cursor() == undoCursor + 1);
    const auto& diff = memberOf(memberOf(result, "revisionDiff"), "contents");
    CONTROL_CHECK(diff.size() == 1 && memberOf(memberOf(diff.at(0), "changed"), "grid").isObject()
                  && memberOf(diff.at(0), "changed").size() == 1);
    CONTROL_CHECK(toJsonString(memberOf(memberOf(result, "sideEffects"), "reRenderQueued"))
                  == "{\"contentIds\":[" + jsonQuoted(f.contentId) + "],\"revert\":null}");
    CONTROL_CHECK(memberOf(memberOf(result, "dirty"), "after").asBoolean());
    CONTROL_CHECK(f.bridge.timeGridNotifications == notified + 1);
    // Tokens read back after the commit equal a fresh read; the content's revision did not change.
    const auto tokens = resultTokens(result, f.contentId);
    CONTROL_CHECK(VectorMatcher::equal(tokens, freshTokens(f.api, f.contentId)));
    CONTROL_CHECK(memberOf(tokens, "revision").text() == memberOf(tokensBefore, "revision").text()
                  && memberOf(tokens, "gridRevision").text() != memberOf(tokensBefore, "gridRevision").text());
    // content.getDetails shows the new grid.
    const auto details = f.api.result("content.getDetails", jsonOf("{\"contentId\":" + jsonQuoted(f.contentId) + "}"));
    const auto& handles = memberOf(memberOf(details, "timeGrid"), "handles");
    if (CONTROL_CHECK(handles.size() == 3))
        for (std::size_t i = 0; i < 3; ++i)
            CONTROL_CHECK(near(memberOf(handles.at(i), "outputSec"), gridCase.expected[i]));
    // The "after" of each change is, byte for byte, the outputSec that content.getDetails reads back for the handle.
    for (std::size_t c = 0; c < changes.size(); ++c) {
        const auto handleId = memberOf(memberOf(changes.at(c), "ref"), "handleId").text();
        const auto written = toJsonString(memberOf(memberOf(memberOf(changes.at(c), "fields"), "outputSec"), "after"));
        for (std::size_t i = 0; i < handles.size(); ++i)
            if (memberOf(handles.at(i), "handleId").text() == handleId)
                if (!CONTROL_CHECK(toJsonString(memberOf(handles.at(i), "outputSec")) == written))
                    note("handle " + handleId + " written " + written + ", read back "
                         + toJsonString(memberOf(handles.at(i), "outputSec")));
    }

    // Undo gives back the grid before, exactly; its tokens equal a fresh read again.
    const auto undone =
        f.api.result("edit.undo", jsonOf("{\"expectedUndoId\":" + jsonQuoted(memberOf(undo, "undoId").text()) + "}"));
    CONTROL_CHECK(memberOf(undone, "applied").asBoolean() && !memberOf(undone, "noOp").asBoolean());
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), before));
    CONTROL_CHECK(VectorMatcher::equal(resultTokens(undone, f.contentId), freshTokens(f.api, f.contentId)));
    CONTROL_CHECK(memberOf(freshTokens(f.api, f.contentId), "revision").text()
                  == memberOf(tokensBefore, "revision").text());
}

/** Requested times on a rounding boundary of six decimals (the API rounds what it writes to 6 decimals): the "after" of
    each change equals, byte for byte, the outputSec that content.getDetails reads back. */
void checkGridRoundingMatchesReadback(TimingFixture& f)
{
    const auto before = innerOf(*f.processor, f.clip.key);
    const auto handleId = f.clip.handleIds[0];
    for (const char* text : {"1.1234565", "1.1234575", "0.9999995", "1.5000005", "1.2499995", "1.3333335"}) {
        const auto result = f.api.result(
            "content.editTimeGrid",
            jsonOf("{\"contentId\":" + jsonQuoted(f.contentId)
                   + ",\"op\":\"moveHandles\",\"expectedGridRevision\":" + jsonQuoted(f.gridRevision())
                   + ",\"moves\":[{\"handleId\":" + jsonQuoted(handleId) + ",\"outputSec\":" + text + "}]}"));
        const auto& changes = memberOf(result, "changes");
        if (!CONTROL_CHECK(changes.size() == 1))
            continue;
        const auto written = toJsonString(memberOf(memberOf(memberOf(changes.at(0), "fields"), "outputSec"), "after"));
        const auto details =
            f.api.result("content.getDetails", jsonOf("{\"contentId\":" + jsonQuoted(f.contentId) + "}"));
        const auto& handles = memberOf(memberOf(details, "timeGrid"), "handles");
        const auto read = handles.size() != 0 ? toJsonString(memberOf(handles.at(0), "outputSec")) : std::string();
        if (!CONTROL_CHECK(written == read))
            note(std::string("moveHandles to ") + text + ": written " + written + ", read back " + read);
        f.api.result("edit.undo", jsonOf("{\"expectedUndoId\":"
                                         + jsonQuoted(memberOf(memberOf(result, "undo"), "undoId").text()) + "}"));
    }
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), before));
}

void scenarioTimingImpl()
{
    TimingFixture f;
    if (!f.open(gridSpec("timing", true)))
        return;
    if (!CONTROL_CHECK(f.clip.handleIds.size() == 3))
        return;
    for (const auto& gridCase : gridCases())
        runGridCase(f, gridCase);
    checkGridRoundingMatchesReadback(f);

    // OpenTune's own undo and redo (the UI's Ctrl+Z) take the entry back and forth as well.
    const auto before = innerOf(*f.processor, f.clip.key);
    f.api.result("content.shiftTiming", jsonOf(f.shiftParams(R"({"startOutputSec":1.5,"endOutputSec":2.5})", 0.25)));
    const auto shifted = innerOf(*f.processor, f.clip.key);
    CONTROL_CHECK(std::abs(shifted[1].output_seconds - 2.25) <= kOutputTolerance);
    CONTROL_CHECK(f.processor->getUndoManager().undo() != nullptr);
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), before));
    CONTROL_CHECK(f.processor->getUndoManager().redo() != nullptr);
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), shifted));
    CONTROL_CHECK(f.processor->getUndoManager().undo() != nullptr);

    // A stale grid revision: REVISION_CONFLICT(grid_changed), nothing moves.
    const auto staleParams = f.shiftParams(R"({"startOutputSec":1.5,"endOutputSec":2.5})", 0.03);
    f.api.result("content.shiftTiming", jsonOf(staleParams));
    const auto afterFirst = innerOf(*f.processor, f.clip.key);
    CONTROL_CHECK(isErrorOf(f.api.request("content.shiftTiming", jsonOf(staleParams)), "REVISION_CONFLICT",
                            "grid_changed", false));
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), afterFirst));
    const std::string staleMove = "{\"contentId\":" + jsonQuoted(f.contentId)
                                  + ",\"op\":\"moveHandles\",\"expectedGridRevision\":\"g0\",\"moves\":[{\"handleId\":"
                                  + jsonQuoted(f.clip.handleIds[0]) + ",\"outputSec\":1.2}]}";
    CONTROL_CHECK(isErrorOf(f.api.request("content.editTimeGrid", jsonOf(staleMove)), "REVISION_CONFLICT",
                            "grid_changed", false));
    CONTROL_CHECK(f.processor->getUndoManager().undo() != nullptr);

    // The handle checks: each handle once, inner handles of this grid only (not the end points, not another
    // spelling of an id).
    const auto grid = innerOf(*f.processor, f.clip.key);
    const int undoSize = f.processor->getUndoManager().size();
    CONTROL_CHECK(isErrorOf(f.api.request("content.editTimeGrid", jsonOf(f.movesParams({{f.clip.handleIds[0], 1.1},
                                                                                        {f.clip.handleIds[0], 1.2}}))),
                            "INVALID_ARGUMENT", "duplicate_handle", false));
    const auto endpointId = std::to_string(f.processor->getContentSnapshot(f.clip.key)->timeGrid->handles().front().id);
    for (const std::string handleId : {std::string("no-such-handle"), endpointId, "0" + f.clip.handleIds[0]})
        CONTROL_CHECK(isErrorOf(
            f.api.request("content.editTimeGrid", jsonOf(f.movesParams({{f.clip.handleIds[1], 2.1}, {handleId, 1.0}}))),
            "NOT_FOUND", "unknown_handle", false));
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), grid));
    CONTROL_CHECK(f.processor->getUndoManager().size() == undoSize);

    // No-ops: no handle in the range, a distance of 0, a move to where the handle is. No undo entry, no change, the
    // editor is not told.
    const int notified = f.bridge.timeGridNotifications;
    for (const auto& params : {f.shiftParams(R"({"startOutputSec":4,"endOutputSec":5})", 0.5),
                               f.shiftParams(R"({"startOutputSec":1.5,"endOutputSec":2.5})", 0.0),
                               f.shiftParams(R"({"startOutputSec":2.5,"endOutputSec":1.5})", 0.5)}) {
        const auto result = f.api.result("content.shiftTiming", jsonOf(params));
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && memberOf(result, "noOp").asBoolean()
                      && memberOf(result, "clamped").size() == 0
                      && toJsonString(memberOf(result, "undo"))
                             == R"({"undoable":false,"reason":"no_op","revert":null})");
    }
    const auto unmoved = f.api.result("content.editTimeGrid", jsonOf(f.movesParams({{f.clip.handleIds[0], 1.0}})));
    CONTROL_CHECK(memberOf(unmoved, "noOp").asBoolean() && memberOf(unmoved, "clamped").size() == 0);
    CONTROL_CHECK(f.processor->getUndoManager().size() == undoSize && f.bridge.timeGridNotifications == notified);

    // dryRun: the prediction of the commit, and nothing changes.
    const auto tokens = freshTokens(f.api, f.contentId);
    const bool dirty = f.bridge.project.dirty;
    const auto predicted =
        f.api.result("content.shiftTiming",
                     jsonOf(f.shiftParams(R"({"startOutputSec":1.5,"endOutputSec":2.5})", 1.5, R"(,"dryRun":true)")));
    CONTROL_CHECK(memberOf(predicted, "dryRun").asBoolean() && !memberOf(predicted, "applied").asBoolean()
                  && memberOf(predicted, "undo").isNull());
    CONTROL_CHECK(memberOf(predicted, "changes").size() == 1 && memberOf(predicted, "clamped").size() == 1
                  && warningCount(predicted, "CLAMPED") == 1);
    CONTROL_CHECK(toJsonString(memberOf(memberOf(predicted, "sideEffects"), "reRenderQueued"))
                  == "{\"contentIds\":[" + jsonQuoted(f.contentId) + "],\"revert\":null}");
    CONTROL_CHECK(toJsonString(memberOf(predicted, "revisionDiff"))
                  == R"({"arrangement":null,"trackState":null,"contents":[]})");
    CONTROL_CHECK(VectorMatcher::equal(tokens, freshTokens(f.api, f.contentId)));
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), grid));
    CONTROL_CHECK(memberOf(memberOf(predicted, "dirty"), "after").asBoolean() == dirty);
    const auto predictedMove =
        f.api.result("content.editTimeGrid", jsonOf(f.movesParams({{f.clip.handleIds[0], 1.2}}, R"(,"dryRun":true)")));
    CONTROL_CHECK(memberOf(predictedMove, "dryRun").asBoolean() && memberOf(predictedMove, "changes").size() == 1);
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), grid));
    CONTROL_CHECK(f.processor->getUndoManager().size() == undoSize && f.bridge.timeGridNotifications == notified);

    // A handle without room between its neighbours (a grid whose spacing the time tool made smaller than 30 ms, as
    // with Alt held): the time tool keeps the grid as it was; the move is reported as limited to where it is.
    {
        const auto previous = f.processor->getContentSnapshot(f.clip.key)->timeGrid;
        std::vector<TimeHandle> tight = previous->handles();
        tight[1].output_seconds = 1.0;
        tight[2].output_seconds = 1.02;
        tight[3].output_seconds = 1.04;
        auto* owner = f.processor->getStandaloneContentRepository()->findClip(f.clip.key);
        const auto tightGrid = TimeGridSnapshot::makeFromHandles(tight);
        if (CONTROL_CHECK(owner != nullptr && tightGrid != nullptr)) {
            owner->applyTimeGrid(tightGrid);
            const auto noRoom =
                f.api.result("content.editTimeGrid", jsonOf(f.movesParams({{f.clip.handleIds[1], 1.5}})));
            CONTROL_CHECK(memberOf(noRoom, "noOp").asBoolean() && memberOf(noRoom, "clamped").size() == 1
                          && near(memberOf(memberOf(noRoom, "clamped").at(0), "appliedOutputSec"), 1.02)
                          && warningCount(noRoom, "CLAMPED") == 1);
            CONTROL_CHECK(f.processor->getContentSnapshot(f.clip.key)->timeGrid == tightGrid);
            owner->applyTimeGrid(previous);
        }
    }

    // A grid without handles: NOT_READY(grid_not_seeded), which waiting does not change.
    const auto plain = f.add(gridSpec("plain", false, 1));
    if (CONTROL_CHECK(plain.has_value())) {
        const auto& plainId = plain->second;
        CONTROL_CHECK(
            isErrorOf(f.api.request("content.shiftTiming",
                                    jsonOf("{\"contentId\":" + jsonQuoted(plainId)
                                           + ",\"expectedGridRevision\":" + jsonQuoted(f.gridRevision(plainId))
                                           + R"(,"range":{"startOutputSec":1,"endOutputSec":2},"deltaSec":0.03})")),
                      "NOT_READY", "grid_not_seeded", false));
        CONTROL_CHECK(
            isErrorOf(f.api.request("content.editTimeGrid", jsonOf("{\"contentId\":" + jsonQuoted(plainId)
                                                                   + ",\"op\":\"moveHandles\",\"expectedGridRevision\":"
                                                                   + jsonQuoted(f.gridRevision(plainId))
                                                                   + R"(,"moves":[{"handleId":"2","outputSec":1}]})")),
                      "NOT_READY", "grid_not_seeded", false));
    }

    // The readiness gate comes before the tokens: an analysis that runs gives NOT_READY(f0_not_ready).
    auto runningSpec = gridSpec("running", true, 2);
    runningSpec.analysis = Analysis::Running;
    const auto running = f.add(runningSpec);
    if (CONTROL_CHECK(running.has_value()))
        CONTROL_CHECK(isErrorOf(
            f.api.request(
                "content.shiftTiming",
                jsonOf(
                    "{\"contentId\":" + jsonQuoted(running->second)
                    + R"(,"expectedGridRevision":"g0","range":{"startOutputSec":1,"endOutputSec":2},"deltaSec":0.03})")),
            "NOT_READY", "f0_not_ready", true));

    // Busy: the user's gesture comes first; nothing moves.
    f.bridge.busy.pointerDown = true;
    CONTROL_CHECK(isErrorOf(f.api.request("content.editTimeGrid", jsonOf(f.movesParams({{f.clip.handleIds[0], 1.2}}))),
                            "BUSY", "pointer_down", true));
    f.bridge.busy.pointerDown = false;
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), grid));

    // A handle of another instance: NOT_FOUND(stale_handle).
    const auto epoch = std::to_string(f.processor->getStandaloneContentRepository()->getContentEpoch());
    const auto stale = "c:" + fixtureOtherInstanceShort(f.instanceShort) + ":" + epoch + ":1";
    CONTROL_CHECK(isErrorOf(
        f.api.request(
            "content.shiftTiming",
            jsonOf("{\"contentId\":" + jsonQuoted(stale)
                   + R"(,"expectedGridRevision":"g0","range":{"startOutputSec":1,"endOutputSec":2},"deltaSec":0.03})")),
        "NOT_FOUND", "stale_handle", false));

    // Params outside the schemas: INVALID_ARGUMENT(invalid_params).
    const std::string content = "{\"contentId\":" + jsonQuoted(f.contentId);
    const std::string range = R"(,"range":{"startOutputSec":1,"endOutputSec":2})";
    for (const std::string params :
         {content + R"(,"expectedGridRevision":"g1")" + range + R"(,"deltaSec":10.5})",
          content + R"(,"expectedGridRevision":"g1","range":{"startTimelineSec":1,"endTimelineSec":2},"deltaSec":0.1})",
          content + R"(,"expectedGridRevision":"g1","range":{"startOutputSec":1},"deltaSec":0.1})",
          content + R"(,"expectedGridRevision":"g1","range":{"startOutputSec":-1,"endOutputSec":2},"deltaSec":0.1})",
          content + range + R"(,"deltaSec":0.1})", content + R"(,"expectedGridRevision":"g1")" + range + "}",
          content + R"(,"expectedGridRevision":"g1")" + range + R"(,"deltaSec":0.1,"dryRun":1})",
          content + R"(,"expectedGridRevision":"g1")" + range + R"(,"deltaSec":0.1,"other":1})"})
        CONTROL_CHECK(f.api.fails("content.shiftTiming", jsonOf(params), "INVALID_ARGUMENT", "invalid_params"));
    std::string tooMany;
    for (int i = 0; i < 65; ++i)
        tooMany += (i == 0 ? "" : ",") + std::string(R"({"handleId":"2","outputSec":1})");
    for (const std::string params :
         {content + R"(,"op":"reset"})", content + R"(,"op":"seed","expectedGridRevision":"g1"})",
          content + R"(,"op":"seed","dryRun":true})", content + R"(,"op":"moveHandles","moves":[]})",
          content + R"(,"op":"moveHandles","expectedGridRevision":"g1","moves":[]})",
          content + R"(,"op":"moveHandles","expectedGridRevision":"g1","moves":[)" + tooMany + "]}",
          content + R"(,"op":"moveHandles","expectedGridRevision":"g1","moves":[{"handleId":"2","outputSec":-1}]})",
          content + R"(,"op":"moveHandles","expectedGridRevision":"g1","moves":[{"handleId":"","outputSec":1}]})",
          content + R"(,"op":"moveHandles","expectedGridRevision":"g1","moves":[{"handleId":2,"outputSec":1}]})",
          content + R"(,"op":"moveHandles","expectedGridRevision":"g1","moves":[{"handleId":"2"}]})",
          std::string(R"({"op":"seed"})")})
        CONTROL_CHECK(f.api.fails("content.editTimeGrid", jsonOf(params), "INVALID_ARGUMENT", "invalid_params"));
}

// ---- Seeding -------------------------------------------------------------------------------------------

/** The ids and content times of a grid's inner handles. */
std::vector<double> contentTimesOf(const std::vector<TimeHandle>& handles)
{
    std::vector<double> times;
    for (const auto& handle : handles)
        times.push_back(handle.source_seconds);
    return times;
}

void scenarioTimeGridSeedImpl()
{
    TimingFixture f;
    if (!f.open(gridSpec("seed", false)))
        return;

    // A timing analysis with onsets at 1, 2, 2.1, 4 and 5.9 s of the 6 s content: OpenTune places a handle at each
    // onset that is 150 ms from the one before and from ClipEnd (TimeGridSnapshot::kMinSourceSpacingSeconds), with
    // output equal to content: at 1, 2 and 4 s.
    CONTROL_CHECK(
        setTimingFeatures(*f.processor, f.clip.key, ReferenceFeatureStatus::Ready, {1.0, 2.0, 2.1, 4.0, 5.9}));
    const auto tokensBefore = freshTokens(f.api, f.contentId);
    const int undoSize = f.processor->getUndoManager().size();
    const auto seedJob = f.startSeed(f.contentId);
    const auto seeded = f.jobOnceTerminal(seedJob);
    CONTROL_CHECK(memberOf(seeded, "state").text() == "succeeded" && memberOf(seeded, "error").isNull());
    const auto& result = memberOf(seeded, "result");
    const auto handles = innerOf(*f.processor, f.clip.key);
    if (CONTROL_CHECK(handles.size() == 3)) {
        CONTROL_CHECK(contentTimesOf(handles) == std::vector<double>({1.0, 2.0, 4.0}));
        for (const auto& handle : handles)
            CONTROL_CHECK(handle.output_seconds == handle.source_seconds);
    }
    // The envelope: created handles, only the grid aspect, no undo entry and no revert (IRREVERSIBLE), dirty, the
    // editor told, nothing rendered again (an identity grid sounds the same).
    CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean());
    CONTROL_CHECK(memberOf(result, "created").size() == 3 && memberOf(result, "changes").size() == 0);
    if (memberOf(result, "created").size() == 3)
        CONTROL_CHECK(memberOf(memberOf(memberOf(result, "created").at(0), "ref"), "handleId").text()
                      == std::to_string(handles[0].id));
    CONTROL_CHECK(toJsonString(memberOf(result, "undo")) == R"({"undoable":false,"reason":"ui_parity","revert":null})");
    CONTROL_CHECK(toJsonString(memberOf(result, "warnings"))
                  == "[{\"code\":\"IRREVERSIBLE\",\"subject\":{\"contentId\":" + jsonQuoted(f.contentId)
                         + "},\"data\":null}]");
    CONTROL_CHECK(memberOf(result, "sideEffects").size() == 0 && memberOf(result, "clamped").size() == 0);
    const auto& diff = memberOf(memberOf(result, "revisionDiff"), "contents");
    CONTROL_CHECK(diff.size() == 1 && memberOf(diff.at(0), "changed").size() == 1
                  && memberOf(memberOf(diff.at(0), "changed"), "grid").isObject());
    CONTROL_CHECK(memberOf(memberOf(result, "dirty"), "after").asBoolean() && f.bridge.timeGridNotifications == 1);
    CONTROL_CHECK(f.processor->getUndoManager().size() == undoSize);
    const auto tokens = resultTokens(result, f.contentId);
    CONTROL_CHECK(VectorMatcher::equal(tokens, freshTokens(f.api, f.contentId)));
    CONTROL_CHECK(memberOf(tokens, "revision").text() == memberOf(tokensBefore, "revision").text());

    // A grid with handles is not seeded again.
    const auto again = f.jobOnceTerminal(f.startSeed(f.contentId));
    CONTROL_CHECK(memberOf(again, "state").text() == "succeeded"
                  && memberOf(memberOf(again, "result"), "noOp").asBoolean());
    CONTROL_CHECK(sameHandles(innerOf(*f.processor, f.clip.key), handles));

    // OpenTune's own timing analysis: the job asks for it and waits. The handles lie where OpenTune puts them when
    // the time tool is picked (ensureTimeToolAnchorSeed, as the UI calls it) for a content with the same analysis.
    // The UI's side gets the analysis the job waited for, as a finished analysis is stored, so it has none of its own
    // to wait for.
    const auto analysed = f.add(gridSpec("analysed", false, 1));
    const auto uiSide = f.add(gridSpec("ui", false, 2));
    if (CONTROL_CHECK(analysed.has_value() && uiSide.has_value())) {
        const auto job = f.startSeed(analysed->second);
        const auto finished = f.jobOnceTerminal(job);
        CONTROL_CHECK(memberOf(finished, "state").text() == "succeeded");
        const auto apiHandles = innerOf(*f.processor, analysed->first.key);
        CONTROL_CHECK(!apiHandles.empty());
        auto features = f.processor->getContentSnapshot(analysed->first.key)->referenceFeatures;
        features.inputFingerprint =
            static_cast<std::int64_t>(f.processor->getContentSnapshot(uiSide->first.key)->audioRevision);
        CONTROL_CHECK(features.isReady() && f.processor->setContentReferenceFeatures(uiSide->first.key, features));
        CONTROL_CHECK(f.processor->ensureTimeToolAnchorSeed(uiSide->first.key));
        CONTROL_CHECK(contentTimesOf(apiHandles) == contentTimesOf(innerOf(*f.processor, uiSide->first.key)));
    }

    // OpenTune's reference analysis service takes the analysis of every content in turn, so the seed of a later
    // content of the same session finishes as the first ones did. The cancellation of a job that waits for an
    // analysis is covered by the failures below, which hold the analysis as running.
    const auto second = f.add(gridSpec("second", false, 3));
    if (CONTROL_CHECK(second.has_value())) {
        CONTROL_CHECK(memberOf(f.jobOnceTerminal(f.startSeed(second->second)), "state").text() == "succeeded");
        CONTROL_CHECK(!innerOf(*f.processor, second->first.key).empty());
    }
}

/** The failures and the cancellation of seed jobs. Each content's timing analysis is held as running (status
    Extracting for this instance's producer and the content's audio), so the job waits until the scenario acts. */
void scenarioTimeGridSeedFailuresImpl()
{
    TimingFixture f;
    if (!f.open(gridSpec("cancelled", false)))
        return;
    const auto waiting = [&f](const Clip& clip, const std::string& id) {
        CONTROL_CHECK(setTimingFeatures(*f.processor, clip.key, ReferenceFeatureStatus::Extracting, {}));
        const auto jobId = f.startSeed(id);
        CONTROL_CHECK(f.waitJobState(jobId, "running"));
        pumpMessagesFor(kQuietPumpMs);
        CONTROL_CHECK(memberOf(f.jobOf(jobId), "state").text() == "running");
        CONTROL_CHECK(memberOf(memberOf(f.jobOf(jobId), "progress"), "phase").text() == "analysing");
        return jobId;
    };
    const auto failedWith = [&f](const std::string& jobId, const std::string& kind, const std::string& reason,
                                 std::optional<bool> retryable = std::nullopt) {
        const auto job = f.jobOnceTerminal(jobId);
        const auto& data = memberOf(memberOf(job, "error"), "data");
        const bool ok = memberOf(job, "state").text() == "failed" && memberOf(data, "kind").text() == kind
                        && memberOf(data, "reason").text() == reason && memberOf(data, "executed").isBoolean()
                        && (!retryable || memberOf(data, "retryable").asBoolean() == *retryable)
                        && !memberOf(data, "executed").asBoolean();
        if (!ok)
            note("expected a job failed with " + kind + "(" + reason + "), got " + toJsonString(job));
        return ok;
    };

    // Cancelled while it waits: no handle, and none appears once the analysis finishes later (the job never asked
    // the processor to seed by itself).
    const auto cancelledJob = waiting(f.clip, f.contentId);
    const auto cancel = f.api.result("job.cancel", jsonOf("{\"jobId\":" + jsonQuoted(cancelledJob) + "}"));
    CONTROL_CHECK(memberOf(cancel, "cancelRequested").asBoolean());
    const auto cancelled = f.jobOnceTerminal(cancelledJob);
    CONTROL_CHECK(memberOf(cancelled, "state").text() == "cancelled");
    CONTROL_CHECK(setTimingFeatures(*f.processor, f.clip.key, ReferenceFeatureStatus::Ready, {1.0, 2.0}));
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(innerOf(*f.processor, f.clip.key).empty() && f.bridge.timeGridNotifications == 0);

    // A dialog opened while the job waited: BUSY(modal_open) when it would write; no handle.
    const auto busyClip = f.add(gridSpec("busy", false, 1));
    if (CONTROL_CHECK(busyClip.has_value())) {
        const auto jobId = waiting(busyClip->first, busyClip->second);
        f.bridge.busy.modalOpen = true;
        CONTROL_CHECK(setTimingFeatures(*f.processor, busyClip->first.key, ReferenceFeatureStatus::Ready, {1.0, 2.0}));
        CONTROL_CHECK(failedWith(jobId, "BUSY", "modal_open"));
        f.bridge.busy.modalOpen = false;
        CONTROL_CHECK(innerOf(*f.processor, busyClip->first.key).empty());
    }

    // The pitch analysis runs again: NOT_READY(f0_not_ready).
    const auto reanalysed = f.add(gridSpec("reanalysed", false, 2));
    if (CONTROL_CHECK(reanalysed.has_value())) {
        const auto jobId = waiting(reanalysed->first, reanalysed->second);
        CONTROL_CHECK(f.processor->setContentOriginalF0State(reanalysed->first.key, OriginalF0State::Extracting));
        CONTROL_CHECK(failedWith(jobId, "NOT_READY", "f0_not_ready"));
        CONTROL_CHECK(innerOf(*f.processor, reanalysed->first.key).empty());
    }

    // The content was removed: NOT_FOUND(unknown_handle).
    const auto removed = f.add(gridSpec("removed", false, 3));
    if (CONTROL_CHECK(removed.has_value())) {
        const auto jobId = waiting(removed->first, removed->second);
        CONTROL_CHECK(f.processor->getStandaloneContentRepository()->retireClip(removed->first.key));
        CONTROL_CHECK(failedWith(jobId, "NOT_FOUND", "unknown_handle"));
    }

    // The timing analysis failed, or found no onset: OpenTune places no handle
    // (ANALYSIS_FAILED(timing_analysis_failed), not retryable).
    for (const auto status : {ReferenceFeatureStatus::Failed, ReferenceFeatureStatus::Ready}) {
        const bool failed = status == ReferenceFeatureStatus::Failed;
        const auto clip = f.add(gridSpec(failed ? "noAnalysis" : "noOnset", false, failed ? 4 : 5));
        if (!CONTROL_CHECK(clip.has_value()))
            continue;
        CONTROL_CHECK(setTimingFeatures(*f.processor, clip->first.key, status, {}));
        CONTROL_CHECK(failedWith(f.startSeed(clip->second), "ANALYSIS_FAILED", "timing_analysis_failed", false));
        CONTROL_CHECK(innerOf(*f.processor, clip->first.key).empty());
    }

    // The analysis does not finish within the job's wait limit: NOT_READY(timing_analysis_timeout), retryable. Every
    // wait step re-reads the limit, so jobs already waiting see the change; the guard restores it on every exit.
    {
        const auto slow = f.add(gridSpec("slow", false, 7));
        if (CONTROL_CHECK(slow.has_value())) {
            const SeedWaitLimitGuard limit(kSeedTestWaitLimitMs);
            CONTROL_CHECK(setTimingFeatures(*f.processor, slow->first.key, ReferenceFeatureStatus::Extracting, {}));
            const auto jobId = f.startSeed(slow->second);
            CONTROL_CHECK(failedWith(jobId, "NOT_READY", "timing_analysis_timeout", true));
            CONTROL_CHECK(innerOf(*f.processor, slow->first.key).empty());
        }
    }

    // The checks of the call itself start no job: the readiness gate and the busy state.
    const auto retained = f.service().jobs()->retainedCount();
    auto runningSpec = gridSpec("running", false, 6);
    runningSpec.analysis = Analysis::Running;
    const auto running = f.add(runningSpec);
    if (CONTROL_CHECK(running.has_value()))
        CONTROL_CHECK(
            isErrorOf(f.api.request("content.editTimeGrid",
                                    jsonOf("{\"contentId\":" + jsonQuoted(running->second) + R"(,"op":"seed"})")),
                      "NOT_READY", "f0_not_ready", true));
    f.bridge.busy.modalOpen = true;
    CONTROL_CHECK(isErrorOf(
        f.api.request("content.editTimeGrid", jsonOf("{\"contentId\":" + jsonQuoted(f.contentId) + R"(,"op":"seed"})")),
        "BUSY", "modal_open", true));
    f.bridge.busy.modalOpen = false;
    CONTROL_CHECK(f.service().jobs()->retainedCount() == retained);
}

// ---- The specification's vectors -------------------------------------------------------------------------

/** The vectors of the specification that run against the processor and must pass, by id. */
const char* const kMustRunTimingVectors[] = {
    "content/edit-time-grid-duplicate-handle",
    "content/edit-time-grid-grid-changed",
    "content/edit-time-grid-job-failures",
    "content/edit-time-grid-move-handles",
    "content/edit-time-grid-not-ready",
    "content/edit-time-grid-seed",
    "content/edit-time-grid-unknown-handle",
    "content/shift-timing-clamped",
    "content/shift-timing-grid-changed",
    "content/shift-timing-grid-not-seeded",
    "content/shift-timing-ok",
    "content/shift-timing-stale-handle",
};

const std::set<std::string> kTimingVectorMethods = {
    "content.shiftTiming", "content.editTimeGrid", "job.get", "job.cancel", "project.get", "content.getDetails"};
const std::set<std::string> kTimingVectorCapabilities = {"session.v1", "read.v1", "jobs.v1", "timing.v1"};

/** Why this runner does not run a vector, or empty when it does. */
std::string timingVectorSkipReason(const JsonValue& vector)
{
    if (memberOf(vector, "capability").text() != "timing.v1")
        return "covers capability " + memberOf(vector, "capability").text() + ", which another runner covers";
    const auto& extra = memberOf(vector, "extraCapabilities");
    for (std::size_t i = 0; i < extra.size(); ++i)
        if (kTimingVectorCapabilities.count(extra.at(i).text()) == 0)
            return "needs capability " + extra.at(i).text() + ", which this runner does not cover";
    if (memberOf(vector, "session").text() != "authenticated")
        return "unauthenticated vectors test the listener (ControlUnitTests)";
    std::vector<const JsonValue*> requests;
    if (const JsonValue* request = vector.find("request"))
        requests.push_back(request);
    if (const JsonValue* steps = vector.find("steps"))
        for (std::size_t i = 0; i < steps->size(); ++i) {
            const auto& step = steps->at(i);
            if ((step.find("send") == nullptr && step.find("expect") == nullptr) || step.find("connection") != nullptr)
                return "uses steps other than send and expect on the main connection";
            if (const JsonValue* send = step.find("send"))
                requests.push_back(send);
        }
    for (const auto* request : requests)
        if (kTimingVectorMethods.count(memberOf(*request, "method").text()) == 0)
            return "uses " + memberOf(*request, "method").text() + ", which this runner does not cover";
    const auto& given = memberOf(vector, "given");
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& name = given.keyAt(i);
        if (name == "dirty" || name == "clips")
            continue;
        if (name == "jobs") {
            for (std::size_t j = 0; j < given.at(i).size(); ++j)
                if (memberOf(given.at(i).at(j), "state").text() != "failed")
                    return "a job that has not failed is not established by this runner";
            continue;
        }
        return "precondition given." + name + " is not established by this runner";
    }
    return {};
}

std::string runTimingVector(const JsonValue& vector)
{
    TimingFixture f;
    f.processor = startedProcessor();
    auto& service = f.processor->getControlService();
    if (!service.isRunning())
        return "the control API did not start";
    f.registered = std::make_unique<ScopedTestBridge>(service, f.bridge);
    if (!f.api.open(service))
        return "session.hello failed";
    f.instanceShort = memberOf(f.api.discovery(), "instanceId").text().substr(0, 8);
    const auto& given = memberOf(vector, "given");
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
        const auto analysis = memberOf(item, "analysis").text();
        spec.analysis = analysis == "ready"     ? Analysis::Ready
                        : analysis == "running" ? Analysis::Running
                                                : Analysis::Failed;
        const auto& notes = memberOf(item, "notes");
        for (std::size_t n = 0; n < notes.size(); ++n)
            spec.notes.push_back({memberOf(notes.at(n), "startContentSec").asDouble(),
                                  memberOf(notes.at(n), "endContentSec").asDouble(),
                                  memberOf(notes.at(n), "targetMidi").asDouble()});
        const auto& grid = memberOf(item, "timeGrid");
        for (std::size_t h = 0; h < grid.size(); ++h)
            spec.gridHandlesContentSec.push_back(memberOf(grid.at(h), "contentSec").asDouble());
        const auto clip = fixtureAddClip(*f.processor, spec);
        if (clip.handleIds.size() != grid.size())
            return "the time grid of " + spec.name + " could not be established";
        for (std::size_t h = 0; h < grid.size(); ++h)
            matcher.bindings[memberOf(grid.at(h), "bind").text()] = JsonValue::string(clip.handleIds[h]);
        specs.emplace_back(spec.name, spec);
    }
    CONTROL_CHECK(settleBackgroundWork(*f.processor));
    if (!establishFailedJobs(*service.jobs(), memberOf(given, "jobs"), matcher))
        return "given.jobs could not be established";
    f.bridge.project.dirty = memberOf(given, "dirty").asBoolean();

    const auto& discovery = f.api.discovery();
    for (std::size_t i = 0; i < discovery.size(); ++i)
        matcher.bindings["discovery." + discovery.keyAt(i)] = discovery.at(i);
    const auto epoch = std::to_string(f.processor->getStandaloneContentRepository()->getContentEpoch());
    matcher.bindings["runner.unknownContentId"] =
        JsonValue::string("c:" + f.instanceShort + ":" + epoch + ":4294967295");
    matcher.bindings["runner.staleContentId"] =
        JsonValue::string("c:" + fixtureOtherInstanceShort(f.instanceShort) + ":" + epoch + ":1");
    if (!specs.empty()) {
        const auto project = f.api.result("project.get");
        for (const auto& [bind, spec] : specs) {
            const auto handles = fixtureHandlesOf(project, spec.trackId, spec.startTimelineSec);
            if (!handles)
                return "the clip " + bind + " is not in project.get";
            matcher.bindings[bind + ".contentId"] = JsonValue::string(handles->contentId);
            matcher.bindings[bind + ".placementId"] = JsonValue::string(handles->placementId);
            for (const char* token : {"revision", "gridRevision", "keyRevision"})
                matcher.bindings[bind + "." + token] = memberOf(handles->tokens, token);
        }
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

// ---- Scenario: the aspects of the tokens, for the time grid ----------------------------------------------

/** A time-grid edit moves gridRevision and not revision, so a note edit that addresses notes by index with the
    revision read before the grid edit still works (a write depends only on the aspects it declares). */
void scenarioTimingTokenAspectsImpl()
{
    TimingFixture f;
    if (!f.open(gridSpec("tokens", true)))
        return;
    if (!CONTROL_CHECK(f.clip.handleIds.size() == 3))
        return;
    const auto revisionOf = [&f] { return memberOf(freshTokens(f.api, f.contentId), "revision").text(); };
    const auto editNotesBy = [&f](const std::string& revision, int index) {
        return f.api.result("content.editNotes",
                            jsonOf("{\"contentId\":" + jsonQuoted(f.contentId) + ",\"expectedRevision\":"
                                   + jsonQuoted(revision) + R"(,"ops":[{"op":"shiftCents","select":{"indices":[)"
                                   + std::to_string(index) + R"(]},"deltaCents":50}]})"));
    };

    // content.shiftTiming: the grid revision changes, the revision does not.
    const auto revision = revisionOf();
    const auto grid = f.gridRevision();
    const auto shifted = f.api.result("content.shiftTiming",
                                      jsonOf(f.shiftParams(R"({"startOutputSec":1.5,"endOutputSec":2.5})", 0.25)));
    CONTROL_CHECK(memberOf(shifted, "applied").asBoolean() && !memberOf(shifted, "noOp").asBoolean());
    CONTROL_CHECK(revisionOf() == revision && f.gridRevision() != grid);
    const auto first = editNotesBy(revision, 0);
    CONTROL_CHECK(memberOf(first, "applied").asBoolean() && !memberOf(first, "noOp").asBoolean());
    CONTROL_CHECK(revisionOf() != revision);

    // content.editTimeGrid moveHandles: the same.
    const auto revisionAfterNotes = revisionOf();
    const auto gridAfterShift = f.gridRevision();
    const auto moved = f.api.result("content.editTimeGrid", jsonOf(f.movesParams({{f.clip.handleIds[0], 1.1}})));
    CONTROL_CHECK(memberOf(moved, "applied").asBoolean() && !memberOf(moved, "noOp").asBoolean());
    CONTROL_CHECK(revisionOf() == revisionAfterNotes && f.gridRevision() != gridAfterShift);
    // And the other way round: a note edit leaves the grid revision as it is.
    const auto gridBeforeNotes = f.gridRevision();
    const auto second = editNotesBy(revisionAfterNotes, 1);
    CONTROL_CHECK(memberOf(second, "applied").asBoolean() && !memberOf(second, "noOp").asBoolean());
    CONTROL_CHECK(f.gridRevision() == gridBeforeNotes);
}

} // namespace

void scenarioTimingTokenAspects()
{
    scenarioTimingTokenAspectsImpl();
}

void scenarioTiming()
{
    scenarioTimingImpl();
}

void scenarioTimeGridSeed()
{
    scenarioTimeGridSeedImpl();
    scenarioTimeGridSeedFailuresImpl();
}

void scenarioTimingVectors()
{
    const auto root = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root / "content")) {
        const auto name = entry.path().filename().u8string();
        if (entry.path().extension() == ".json"
            && (name.rfind("shift-timing-", 0) == 0 || name.rfind("edit-time-grid-", 0) == 0))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::set<std::string> passedIds;
    int skipped = 0;
    VectorReport report("ControlProcessorTests.TimingVectors");
    for (const auto& file : files) {
        const auto parsed = parseJson(readFile(file));
        if (!CONTROL_CHECK(parsed.ok))
            continue;
        const auto id = memberOf(parsed.value, "id").text();
        if (const auto reason = timingVectorSkipReason(parsed.value); !reason.empty()) {
            ++skipped;
            report.skip(id, reason);
            note("SKIP " + id + ": " + reason);
            continue;
        }
        const auto started = Clock::now();
        const auto failure = runTimingVector(parsed.value);
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty()))
            passedIds.insert(id);
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    for (const char* id : kMustRunTimingVectors)
        CONTROL_CHECK(passedIds.count(id) == 1);
    note(std::to_string(passedIds.size()) + " vectors passed, " + std::to_string(skipped) + " skipped");
}

} // namespace ControlTest
} // namespace OpenTune
