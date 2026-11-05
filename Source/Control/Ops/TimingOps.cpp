// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimingOps.h"

#include "EditSupport.h"
#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Content/EditableContentSnapshot.h"
#include "Control/ControlJobs.h"
#include "Control/ControlUiBridge.h"
#include "Control/Edit/ControlOriginAction.h"
#include "Control/Edit/TimeGridOps.h"
#include "PluginProcessor.h"

#include <chrono>
#include <cmath>

namespace OpenTune {

namespace {

/** How long a seed job waits for OpenTune's timing analysis of the content before it gives up. */
constexpr int kSeedWaitLimitMs = 120000;

/** The wait limit that the tests set (controlSetSeedWaitLimitForTesting); 0 for kSeedWaitLimitMs. */
int& seedWaitLimitOverrideMs()
{
    static int milliseconds = 0;
    return milliseconds;
}
/** moveHandles takes at most this many moves (methods/content.editTimeGrid.schema.json). */
constexpr std::size_t kMaxMoves = 64;
/** content.shiftTiming moves by at most this many seconds either way. */
constexpr double kMaxShiftSec = 10.0;
/** A handleId has 1 to 64 characters (common.schema.json#/$defs/handleId). */
constexpr std::size_t kMaxHandleIdLength = 64;

bool failWith(ControlParamReader& reader, std::string& why)
{
    why = reader.why();
    return false;
}

bool reject(ControlParamReader& reader, std::string& why, std::string message)
{
    reader.fail(std::move(message));
    return failWith(reader, why);
}

bool readDryRun(ControlParamReader& reader, bool& out)
{
    const JsonValue* dryRun = reader.member("dryRun");
    if (dryRun == nullptr)
        return true;
    if (!dryRun->isBoolean())
        return reader.fail("dryRun must be a boolean");
    out = dryRun->asBoolean();
    return true;
}

bool nonNegativeSeconds(const JsonValue* value)
{
    return value != nullptr && value->isNumber() && std::isfinite(value->asDouble()) && value->asDouble() >= 0.0;
}

JsonValue contentRef(const std::string& contentId)
{
    auto ref = JsonValue::object();
    ref.appendMember("contentId", JsonValue::string(contentId));
    return ref;
}

JsonValue handleRef(const std::string& contentId, std::uint64_t handleId)
{
    auto ref = contentRef(contentId);
    ref.appendMember("handleId", JsonValue::string(TimeGridOps::handleIdText(handleId)));
    return ref;
}

/** The display label of a handle: its position among the inner handles and its fixed point in the source audio,
    for example "#2 1.25s" (display only, like the labels of notes). */
JsonValue handleLabel(std::size_t index, double contentSec)
{
    const auto hundredths = static_cast<long long>(std::llround(std::max(0.0, contentSec) * 100.0));
    std::string fraction = std::to_string(hundredths % 100);
    if (fraction.size() < 2)
        fraction.insert(0, 1, '0');
    return JsonValue::string("#" + std::to_string(index) + " " + std::to_string(hundredths / 100) + "." + fraction
                             + "s");
}

JsonValue secondsField(double before, double after)
{
    auto field = JsonValue::object();
    field.appendMember("before", controlJsonSeconds(before));
    field.appendMember("after", controlJsonSeconds(after));
    field.appendMember("unit", JsonValue::string("sec"));
    return field;
}

/** REVISION_CONFLICT(grid_changed) when the grid is not the one the client read (PROTOCOL.md section 10). */
std::optional<Error> checkGridRevision(const ControlContentVersions& versions, const std::string& expected)
{
    if (versions.grid != expected)
        return controlOpError("REVISION_CONFLICT", "grid_changed",
                              "the content's time grid differs from expectedGridRevision; read it again");
    return std::nullopt;
}

/** The checks of a handle edit before its own validation, in the order of PROTOCOL.md section 13.1 rule 4: the
    handle, the busy state, the readiness gate, expectedGridRevision, then that the grid has handles. */
Result<ControlContentRef> checkGridEdit(ControlOpContext& context, const ControlHandle& contentId,
                                        const std::string& expectedGridRevision)
{
    auto content = controlResolveContent(context, contentId);
    if (!content.ok())
        return content.error();
    const auto& ref = content.value();
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (auto notReady = controlCheckContentReady(context, ref))
        return *notReady;
    if (auto conflict =
            checkGridRevision(controlContentVersions(context, ref.contentId, *ref.snapshot), expectedGridRevision))
        return *conflict;
    if (!TimeGridOps::isSeeded(ref.snapshot->timeGrid.get()))
        return controlOpError("NOT_READY", "grid_not_seeded",
                              "the content's time grid has no handles; seed it with content.editTimeGrid op seed");
    return ref;
}

/**
 * The result of a handle edit, for both methods: the changed handles, clamped and CLAMPED for each limited move,
 * and for a commit the undo entry (a TimeGridEditAction in a ControlOriginAction, as the piano roll pushes it), the
 * dirty flag, reRenderQueued and the editor's notification. A plan without a grid after it is a no-op, as
 * PianoRollToolHandler::handleTimeToolMouseUp commits a drag only when it changed the grid, once, with the drag's
 * description.
 */
Result<JsonValue> applyPlan(ControlOpContext& context, const ControlContentRef& ref, const TimeGridOps::Plan& plan,
                            bool dryRun)
{
    auto history = context.history();
    if (history == nullptr)
        return controlOpError("INTERNAL", "", "the undo history of the control API is not available");
    auto& processor = context.processor();
    const auto beforeWorld = controlCaptureWorldVersions(context);
    ControlWriteEnvelope envelope(context);
    envelope.touchContent(ref.contentId);
    auto clamped = JsonValue::array();
    for (const auto& handle : plan.handles) {
        if (plan.after != nullptr && handle.moved()) {
            auto fields = JsonValue::object();
            fields.appendMember("outputSec", secondsField(handle.beforeOutputSec, handle.afterOutputSec));
            envelope.addChange("timeHandle", handleRef(ref.contentId, handle.handleId),
                               handleLabel(handle.index, handle.contentSec), std::move(fields));
        }
        if (!handle.clamped())
            continue;
        auto entry = JsonValue::object();
        entry.appendMember("handleId", JsonValue::string(TimeGridOps::handleIdText(handle.handleId)));
        entry.appendMember("requestedOutputSec", controlJsonSeconds(*handle.requestedOutputSec));
        entry.appendMember("appliedOutputSec", controlJsonSeconds(handle.afterOutputSec));
        clamped.append(std::move(entry));
        auto data = JsonValue::object();
        data.appendMember("field", JsonValue::string("outputSec"));
        data.appendMember("requested", controlJsonSeconds(*handle.requestedOutputSec));
        data.appendMember("applied", controlJsonSeconds(handle.afterOutputSec));
        envelope.addWarning("CLAMPED", handleRef(ref.contentId, handle.handleId), std::move(data));
    }

    JsonValue result;
    if (dryRun) {
        if (plan.after != nullptr) {
            // What a commit reports: setContentTimeGrid() advances the grid revision, an input of the render.
            auto effect = JsonValue::object();
            auto ids = JsonValue::array();
            ids.append(JsonValue::string(ref.contentId));
            effect.appendMember("contentIds", std::move(ids));
            effect.appendMember("revert", JsonValue());
            envelope.addSideEffect("reRenderQueued", std::move(effect));
        }
        result = envelope.predicted(context);
    } else if (plan.after != nullptr) {
        const auto description = TimeGridOps::moveDescription();
        auto inner = TimeGridOps::commit(processor, ref.key, plan, description);
        if (!inner)
            return controlOpError("INTERNAL", "", "OpenTune did not accept the time grid");
        std::vector<ControlOriginAction::Inner> actions;
        actions.push_back(std::move(*inner));
        const auto undoId = history->nextUndoId(context.instanceShort());
        auto action = std::make_unique<ControlOriginAction>(history, undoId, description, std::move(actions));
        const auto names = action->innerActionNames();
        // UndoManager::addAction(), as the piano roll's commitTimeGrid pushes its entry; it discards the redo history.
        history->record(processor.getUndoManager(), std::move(action));
        envelope.setUndoable(undoId, description.toStdString(), names);
        // The time grid is saved with the project; the UI does not mark the project after a time-grid edit.
        controlMarkDirty(context);
        if (auto* bridge = context.bridge())
            bridge->onTimeGridChanged();
        controlAddReRenderQueued(envelope, beforeWorld, controlCaptureWorldVersions(context));
        result = envelope.committed(context);
    } else {
        result = envelope.committed(context);
    }
    result.appendMember("clamped", std::move(clamped));
    return result;
}

// ---- content.shiftTiming ------------------------------------------------------------------------------

struct ShiftParams
{
    ControlHandle contentId;
    std::string expectedGridRevision;
    /** The range's clock: content seconds (mapped through the grid) or output seconds. */
    bool contentClock = false;
    double start = 0.0;
    double end = 0.0;
    double deltaSec = 0.0;
    bool dryRun = false;
};

/** range: {startContentSec, endContentSec} or {startOutputSec, endOutputSec}, closed, non-negative seconds. */
bool readShiftRange(ControlParamReader& reader, ShiftParams& out)
{
    const JsonValue* range = reader.member("range");
    if (range == nullptr || !range->isObject() || range->size() != 2)
        return reader.fail("range must be {startContentSec, endContentSec} or {startOutputSec, endOutputSec}");
    for (const bool content : {true, false}) {
        const JsonValue* start = range->find(content ? "startContentSec" : "startOutputSec");
        const JsonValue* end = range->find(content ? "endContentSec" : "endOutputSec");
        if (start == nullptr && end == nullptr)
            continue;
        if (!nonNegativeSeconds(start) || !nonNegativeSeconds(end))
            break;
        out.contentClock = content;
        out.start = start->asDouble();
        out.end = end->asDouble();
        return true;
    }
    return reader.fail("range must be {startContentSec, endContentSec} or {startOutputSec, endOutputSec} of "
                       "non-negative seconds");
}

/** methods/content.shiftTiming.schema.json $defs.params. */
bool parseShift(const JsonValue& params, ShiftParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<ControlHandle> contentId;
    std::optional<std::string> expected;
    std::optional<double> delta;
    if (!reader.onlyMembers({"contentId", "expectedGridRevision", "range", "deltaSec", "dryRun"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, contentId)
        || !controlReadRevisionToken(reader, "expectedGridRevision", expected)
        || !reader.optionalNumber("deltaSec", -kMaxShiftSec, kMaxShiftSec, delta) || !readDryRun(reader, out.dryRun))
        return failWith(reader, why);
    if (!contentId || !expected || !delta)
        return reject(reader, why, "contentId, expectedGridRevision, range and deltaSec are required");
    if (!readShiftRange(reader, out))
        return failWith(reader, why);
    out.contentId = *contentId;
    out.expectedGridRevision = *expected;
    out.deltaSec = *delta;
    return true;
}

Result<JsonValue> runShift(ControlOpContext& context, const ShiftParams& params)
{
    auto content = checkGridEdit(context, params.contentId, params.expectedGridRevision);
    if (!content.ok())
        return content.error();
    const auto& ref = content.value();
    const auto& grid = ref.snapshot->timeGrid;
    // A content range maps through the grid the client read (expectedGridRevision matched): tauForward() is how
    // OpenTune maps source seconds to output seconds.
    const double start = params.contentClock ? grid->tauForward(params.start) : params.start;
    const double end = params.contentClock ? grid->tauForward(params.end) : params.end;
    return applyPlan(context, ref, TimeGridOps::planShift(grid, start, end, params.deltaSec), params.dryRun);
}

// ---- content.editTimeGrid -----------------------------------------------------------------------------

struct EditGridParams
{
    ControlHandle contentId;
    bool seed = false;
    std::string expectedGridRevision;
    std::vector<std::pair<std::string, double>> moves;
    bool dryRun = false;
};

bool readMoves(ControlParamReader& reader, EditGridParams& out)
{
    const JsonValue* moves = reader.member("moves");
    if (moves == nullptr || !moves->isArray() || moves->size() < 1 || moves->size() > kMaxMoves)
        return reader.fail("moves must be an array of 1 to 64 moves");
    for (std::size_t i = 0; i < moves->size(); ++i) {
        const auto& move = moves->at(i);
        const JsonValue* handleId = move.isObject() ? move.find("handleId") : nullptr;
        const JsonValue* outputSec = move.isObject() ? move.find("outputSec") : nullptr;
        if (handleId == nullptr || outputSec == nullptr || move.size() != 2 || !handleId->isString()
            || handleId->text().empty() || handleId->text().size() > kMaxHandleIdLength
            || !nonNegativeSeconds(outputSec))
            return reader.fail("every move must be {handleId, outputSec} with non-negative seconds");
        out.moves.emplace_back(handleId->text(), outputSec->asDouble());
    }
    return true;
}

/** methods/content.editTimeGrid.schema.json $defs.params: op seed or op moveHandles, each closed. */
bool parseEditGrid(const JsonValue& params, EditGridParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<ControlHandle> contentId;
    const JsonValue* op = reader.member("op");
    if (op == nullptr || !op->isString() || (op->text() != "seed" && op->text() != "moveHandles"))
        return reject(reader, why, "op must be seed or moveHandles");
    out.seed = op->text() == "seed";
    if (out.seed) {
        if (!reader.onlyMembers({"contentId", "op"})
            || !reader.optionalHandle("contentId", ControlHandleKind::Content, contentId))
            return failWith(reader, why);
        if (!contentId)
            return reject(reader, why, "contentId is required");
        out.contentId = *contentId;
        return true;
    }
    std::optional<std::string> expected;
    if (!reader.onlyMembers({"contentId", "op", "expectedGridRevision", "moves", "dryRun"})
        || !reader.optionalHandle("contentId", ControlHandleKind::Content, contentId)
        || !controlReadRevisionToken(reader, "expectedGridRevision", expected) || !readDryRun(reader, out.dryRun))
        return failWith(reader, why);
    if (!contentId || !expected)
        return reject(reader, why, "op moveHandles needs contentId, expectedGridRevision and moves");
    if (!readMoves(reader, out))
        return failWith(reader, why);
    out.contentId = *contentId;
    out.expectedGridRevision = *expected;
    return true;
}

Result<JsonValue> runMoveHandles(ControlOpContext& context, const EditGridParams& params)
{
    auto content = checkGridEdit(context, params.contentId, params.expectedGridRevision);
    if (!content.ok())
        return content.error();
    const auto& ref = content.value();
    const auto& grid = ref.snapshot->timeGrid;
    // Each handle once (nothing moves otherwise), then every handle must be an inner handle of the grid.
    for (std::size_t i = 0; i < params.moves.size(); ++i)
        for (std::size_t j = i + 1; j < params.moves.size(); ++j)
            if (params.moves[i].first == params.moves[j].first)
                return controlOpError("INVALID_ARGUMENT", "duplicate_handle", "moves names a handle more than once");
    std::vector<TimeGridOps::Move> moves;
    for (const auto& [handleId, outputSec] : params.moves) {
        const auto id = TimeGridOps::findInnerHandle(*grid, handleId);
        if (!id)
            return controlOpError("NOT_FOUND", "unknown_handle",
                                  "a handleId names no handle of the content's time grid");
        moves.push_back({*id, outputSec});
    }
    return applyPlan(context, ref, TimeGridOps::planMoves(grid, moves), params.dryRun);
}

ControlJobError jobErrorOf(const Error& error)
{
    // The checks of the job's steps return errors that changed nothing (controlOpError).
    const auto reply = controlMapError(error);
    return {reply.kind, reply.reason, reply.detail, reply.executed.value_or(false)};
}

/**
 * The work of a timeGridSeed job, on the message thread, one step per tick:
 *
 * - every step checks that the content still exists (NOT_FOUND(unknown_handle)) and that its pitch analysis is
 *   still ready (the readiness gate); a grid that has handles by now is not seeded again (a no-op);
 * - it asks for OpenTune's timing analysis of the content (preheatReferenceFeatures(), which does
 *   nothing while that analysis runs or is ready) and waits for it; an analysis that failed or found no onset
 *   ends the job with ANALYSIS_FAILED(timing_analysis_failed), as the time tool then places no handle;
 * - once the analysis is ready it checks the busy state and seeds with ensureTimeToolAnchorSeed(), which places
 *   the handles at once when the analysis is ready, then marks the project as changed and tells the editor.
 *
 * Cancellation is possible until the seeding step; nothing is written then. The job never registers the content
 * for the processor's own deferred seeding (what ensureTimeToolAnchorSeed() does when the analysis is not ready),
 * so no handle appears after the job ended.
 */
class SeedJob
{
public:
    SeedJob(ControlOpEnvironment environment, ContentKey key, std::string contentId)
        : environment_(std::move(environment))
        , key_(key)
        , contentId_(std::move(contentId))
        , started_(std::chrono::steady_clock::now())
    {}

    void step(ControlJobStep& step)
    {
        ControlOpContext context(environment_);
        if (step.cancelRequested()) {
            step.cancel();
            return;
        }
        auto& processor = context.processor();
        ControlContentRef ref;
        ref.key = key_;
        ref.contentId = contentId_;
        ref.snapshot = controlLiveSnapshot(context, key_);
        if (ref.snapshot == nullptr) {
            step.fail(
                {"NOT_FOUND", "unknown_handle", "the content was removed before its time grid was seeded", false});
            return;
        }
        if (const auto notReady = controlCheckContentReady(context, ref)) {
            step.fail(jobErrorOf(*notReady));
            return;
        }
        if (firstStep_) {
            // The checks the call made, made again when the job runs (methods/content.editTimeGrid.schema.json).
            firstStep_ = false;
            if (const auto busy = controlCheckBusy(context)) {
                step.fail(jobErrorOf(*busy));
                return;
            }
        }
        if (TimeGridOps::isSeeded(ref.snapshot->timeGrid.get())) {
            // OpenTune seeds a grid only while it has no handles; this one has some by now.
            ControlWriteEnvelope envelope(context);
            envelope.touchContent(contentId_);
            finish(step, envelope.committed(context));
            return;
        }
        // OpenTuneAudioProcessor::preheatReferenceFeatures(): the timing analysis that
        // ensureTimeToolAnchorSeed() needs, requested once and reported while it runs.
        switch (processor.preheatReferenceFeatures(key_)) {
            case OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::AlreadyReady:
                break;
            case OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::Queued:
            case OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::WaitingForSource:
                wait(step);
                return;
            case OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::InvalidContent:
            case OpenTuneAudioProcessor::ReferenceAnalysisPreheatStatus::AnalysisFailed:
                step.fail({"ANALYSIS_FAILED", "timing_analysis_failed",
                           "OpenTune's timing analysis of the content failed; no handle was placed", false});
                return;
        }
        if (!ref.snapshot->referenceFeatures.hasTimingAnchors()) {
            step.fail({"ANALYSIS_FAILED", "timing_analysis_failed",
                       "OpenTune's timing analysis found no onset to place a handle at", false});
            return;
        }
        seed(context, step, ref);
    }

private:
    void wait(ControlJobStep& step)
    {
        const int limitMs = seedWaitLimitOverrideMs() > 0 ? seedWaitLimitOverrideMs() : kSeedWaitLimitMs;
        if (std::chrono::steady_clock::now() - started_ >= std::chrono::milliseconds(limitMs)) {
            step.fail({"NOT_READY", "timing_analysis_timeout",
                       "OpenTune's timing analysis of the content did not finish within the wait limit", false});
            return;
        }
        step.setProgress(std::nullopt, "analysing", std::nullopt, std::nullopt);
    }

    void seed(ControlOpContext& context, ControlJobStep& step, const ControlContentRef& ref)
    {
        if (const auto busy = controlCheckBusy(context)) {
            step.fail(jobErrorOf(*busy));
            return;
        }
        auto& processor = context.processor();
        auto history = context.history();
        const auto gridBefore = ref.snapshot->timeGridRevision;
        ControlWriteEnvelope envelope(context);
        envelope.touchContent(contentId_);
        envelope.limitDiffToTouchedContents();
        // OpenTuneAudioProcessor::ensureTimeToolAnchorSeed(): what PianoRollComponent::setCurrentTool() calls when
        // the time tool is picked. With the analysis ready it places the handles at once (setContentTimeGrid).
        const bool seeded = processor.ensureTimeToolAnchorSeed(key_);
        const auto after = controlLiveSnapshot(context, key_);
        if (!seeded || after == nullptr || !TimeGridOps::isSeeded(after->timeGrid.get())) {
            const bool changed = after != nullptr && after->timeGridRevision != gridBefore;
            step.fail({"ANALYSIS_FAILED", "timing_analysis_failed",
                       "OpenTune placed no handle in the content's time grid", changed});
            return;
        }
        // A change without an undo entry (PROTOCOL.md section 15.3 rule 9): earlier entries of the API no longer put
        // the grid back, and the redo history is cleared.
        if (history != nullptr)
            history->onChangeWithoutEntry(processor.getUndoManager(), key_, ControlContentAspect::TimeGrid);
        // The time grid is saved with the project; the UI does not mark the project after seeding.
        controlMarkDirty(context);
        if (auto* bridge = context.bridge())
            bridge->onTimeGridChanged();
        const auto handles = TimeGridOps::innerHandles(*after->timeGrid);
        for (std::size_t i = 0; i < handles.size(); ++i)
            envelope.addCreated("timeHandle", handleRef(contentId_, handles[i].id),
                                handleLabel(i, handles[i].source_seconds));
        // As in the UI there is no undo entry, and no request restores a grid without handles.
        envelope.addWarning("IRREVERSIBLE", contentRef(contentId_));
        envelope.setNotUndoable("ui_parity");
        finish(step, envelope.committed(context));
    }

    static void finish(ControlJobStep& step, JsonValue result)
    {
        result.appendMember("clamped", JsonValue::array());
        step.succeed(std::move(result));
    }

    ControlOpEnvironment environment_;
    ContentKey key_;
    std::string contentId_;
    std::chrono::steady_clock::time_point started_;
    bool firstStep_ = true;
};

Result<JsonValue> runSeed(ControlOpContext& context, const EditGridParams& params)
{
    auto* jobs = context.jobs();
    if (jobs == nullptr)
        return controlOpError("INTERNAL", "", "the jobs registry is not available");
    auto content = controlResolveContent(context, params.contentId);
    if (!content.ok())
        return content.error();
    const auto& ref = content.value();
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (auto notReady = controlCheckContentReady(context, ref))
        return *notReady;
    ControlWriteEnvelope envelope(context);
    envelope.touchContent(ref.contentId);
    auto job = std::make_shared<SeedJob>(context.environment(), ref.key, ref.contentId);
    const auto started = jobs->start("timeGridSeed", true, [job](ControlJobStep& step) { job->step(step); });
    if (started.jobId.empty())
        return started.refusal == "job_limit"
                   ? controlOpError("BUSY", "job_limit", "every retained job is still active")
                   : controlOpError("SHUTTING_DOWN", "", "the control API is stopping");
    auto result = envelope.handedToJob(context, jobs->jobRef(started.jobId));
    result.appendMember("clamped", JsonValue::array());
    return result;
}

Result<JsonValue> runEditGrid(ControlOpContext& context, const EditGridParams& params)
{
    return params.seed ? runSeed(context, params) : runMoveHandles(context, params);
}

} // namespace

void registerTimingOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<ShiftParams>("content.shiftTiming", parseShift, runShift));
    table.add(controlWriteMethod<EditGridParams>("content.editTimeGrid", parseEditGrid, runEditGrid));
}

void controlSetSeedWaitLimitForTesting(int milliseconds)
{
    seedWaitLimitOverrideMs() = milliseconds > 0 ? milliseconds : 0;
}

} // namespace OpenTune
