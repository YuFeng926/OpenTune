// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PrefsOps.h"

#include "OpsSupport.h"
#include "PrefsModels.h"
#include "WriteEnvelope.h"

#include "Control/ControlJobs.h"
#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"
#include "Utils/AppPreferences.h"
#include "Utils/AudioEditingScheme.h"
#include "Utils/TuningConfig.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

constexpr const char* kJobKindPrefsApply = "prefsApply";

// ---- Params ---------------------------------------------------------------------------------------

/** Parses prefs.set params, retaining only the members that are present. */
struct PrefsRequest
{
    std::optional<float> tuningHz;
    /** editingScheme: true for openDyne (notes primary), false for openTune. */
    std::optional<bool> notesPrimary;
    std::optional<bool> hybridMode;
    std::optional<bool> experimentalFeatures;
    /** renderingPriority: true for cpuFirst, false for gpuFirst. */
    std::optional<bool> cpuFirst;
    /** f0Model: a name of processing.options.f0Model (checked when the call runs), and the type it names. */
    std::optional<std::string> f0Model;
    std::optional<F0ModelType> f0Type;
    std::optional<bool> accessibilityModeEnabled;
};

/** A4 as the wire carries it: three decimals, like controlJsonNumber(tuningHz, 3) in prefs.get and session.info. */
float roundedTuning(double hz)
{
    return static_cast<float>(std::round(hz * 1000.0) / 1000.0);
}

bool readBoolean(ControlParamReader& reader, const char* name, std::optional<bool>& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    if (!value->isBoolean())
        return reader.fail(std::string(name) + " must be a boolean");
    out = value->asBoolean();
    return true;
}

/** An optional text member of 1 to kControlOptionTextCodePoints code points. */
bool readOptionName(ControlParamReader& reader, const char* name, std::optional<std::string>& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    if (!value->isString() || value->text().empty() || utf8CodePointCount(value->text()) > kControlOptionTextCodePoints)
        return reader.fail(std::string(name) + " must be a string of 1 to "
                           + std::to_string(kControlOptionTextCodePoints) + " characters");
    out = value->text();
    return true;
}

/** An optional member that is one of two words; out is true for the second. */
bool readEnumPair(ControlParamReader& reader, const char* name, const char* first, const char* second,
                  std::optional<bool>& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    if (!value->isString() || (value->text() != first && value->text() != second))
        return reader.fail(std::string(name) + " must be " + first + " or " + second);
    out = value->text() == second;
    return true;
}

/** Checks the params of prefs.set on the connection thread: a closed object with at least one member. */
bool parsePrefsSet(const JsonValue& params, PrefsRequest& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<double> tuning;
    if (!reader.onlyMembers({"tuningHz", "editingScheme", "hybridMode", "experimentalFeatures",
                             "renderingPriority", "f0Model", "accessibilityModeEnabled"})
        || !reader.optionalNumber("tuningHz", static_cast<double>(TuningConfig::kMinTuningHz),
                                  static_cast<double>(TuningConfig::kMaxTuningHz), tuning)) {
        why = reader.why();
        return false;
    }
    if (const JsonValue* scheme = reader.member("editingScheme")) {
        if (!scheme->isString() || (scheme->text() != "openTune" && scheme->text() != "openDyne")) {
            why = "editingScheme must be openTune or openDyne";
            return false;
        }
        out.notesPrimary = scheme->text() == "openDyne";
    }
    if (!readBoolean(reader, "hybridMode", out.hybridMode)
        || !readBoolean(reader, "experimentalFeatures", out.experimentalFeatures)
        || !readEnumPair(reader, "renderingPriority", "gpuFirst", "cpuFirst", out.cpuFirst)
        || !readOptionName(reader, "f0Model", out.f0Model)
        || !readBoolean(reader, "accessibilityModeEnabled", out.accessibilityModeEnabled)) {
        why = reader.why();
        return false;
    }
    if (tuning)
        out.tuningHz = roundedTuning(*tuning);
    if (!out.tuningHz && !out.notesPrimary && !out.hybridMode
        && !out.experimentalFeatures && !out.cpuFirst && !out.f0Model && !out.accessibilityModeEnabled) {
        why = "params must have at least one member";
        return false;
    }
    return true;
}

// ---- Shared preference command input -----------------------------------------------------------------

ProcessingPreferencePatch preferencePatch(const PrefsRequest& request)
{
    ProcessingPreferencePatch patch;
    patch.tuningHz = request.tuningHz;
    if (request.notesPrimary)
        patch.audioEditingScheme = *request.notesPrimary ? AudioEditingScheme::Scheme::NotesPrimary
                                                         : AudioEditingScheme::Scheme::CorrectedF0Primary;
    patch.lightPitchCorrectionEnabled = request.hybridMode;
    patch.experimentalFeaturesEnabled = request.experimentalFeatures;
    if (request.cpuFirst)
        patch.renderingPriority = *request.cpuFirst ? RenderingPriority::CpuFirst : RenderingPriority::GpuFirst;
    patch.f0ModelType = request.f0Type;
    patch.accessibilityModeEnabled = request.accessibilityModeEnabled;
    return patch;
}

/** The type a pitch model name stands for among the pitch models that OpenTune lists. */
std::optional<F0ModelType> f0TypeOf(const std::string& name)
{
    for (const auto& model : controlF0Models())
        if (model.name == name)
            return model.type;
    return std::nullopt;
}

ProcessingPreferenceApplyResult applyPreferencePatch(
    ControlOpContext& context, const PrefsRequest& request,
    std::function<void(ProcessingPreferenceRuntimeCompletion)> onRuntimeCompletion = {})
{
    auto* bridge = context.bridge();
    return applyProcessingPreferencePatch(
        context.processor().getAppPreferences(), preferencePatch(request),
        context.processor().getProcessingPreferenceRuntimeOperations(),
        [bridge] {
            if (bridge != nullptr)
                bridge->onPreferencesChanged();
        },
        std::move(onRuntimeCompletion));
}

std::optional<Error> applyError(const ProcessingPreferenceApplyResult& result)
{
    switch (result.status) {
        case ProcessingPreferenceApplyResult::Status::F0ModelRejected:
            return controlOpError("MODEL_UNAVAILABLE", "model_missing", "OpenTune could not switch the pitch model");
        case ProcessingPreferenceApplyResult::Status::RuntimeCommandRejected:
            return controlOpError("MODEL_UNAVAILABLE", "backend_change_rejected",
                                  "OpenTune could not queue the processing configuration change");
        case ProcessingPreferenceApplyResult::Status::F0RestoreRejected:
            return controlOpErrorAfterChange(
                "MODEL_UNAVAILABLE", "backend_change_rejected",
                "OpenTune could not queue the processing configuration change; the pitch model remains changed because its "
                "previous selection could not be restored");
        case ProcessingPreferenceApplyResult::Status::Applied:
        case ProcessingPreferenceApplyResult::Status::NoChange:
            break;
    }
    return std::nullopt;
}

// ---- The write envelope of a change -------------------------------------------------------------------

JsonValue preferenceRef(const char* name)
{
    auto ref = JsonValue::object();
    ref.appendMember("preference", JsonValue::string(name));
    return ref;
}

/** One change of entity preference: {entity, ref {preference}, label, fields {name: {before, after, unit}}}. */
void addPreferenceChange(ControlWriteEnvelope& envelope, const char* name, const char* label, JsonValue before,
                         JsonValue after, JsonValue unit)
{
    auto pair = JsonValue::object();
    pair.appendMember("before", std::move(before));
    pair.appendMember("after", std::move(after));
    pair.appendMember("unit", std::move(unit));
    auto fields = JsonValue::object();
    fields.appendMember(name, std::move(pair));
    envelope.addChange("preference", preferenceRef(name), JsonValue::string(label), std::move(fields));
}

/** Records preference changes from the persisted state and runtime completion actually observed. */
void recordDelta(ControlOpContext& context, ControlWriteEnvelope& envelope,
                 const ProcessingPreferenceState& before, const ProcessingPreferenceState& after,
                 const ProcessingPreferenceDelta& delta,
                 const ProcessingPreferenceRuntimeCompletion& runtimeCompletion = {})
{
    if (!delta.any() && !runtimeCompletion.backendChanged)
        return;

    auto names = JsonValue::array();
    auto revert = JsonValue::object();
    if (delta.tuning) {
        addPreferenceChange(envelope, "tuningHz", "Tuning (A4)", controlJsonNumber(before.tuningHz, 3),
                            controlJsonNumber(after.tuningHz, 3), JsonValue::string("hz"));
        names.append(JsonValue::string("tuningHz"));
        revert.appendMember("tuningHz", controlJsonNumber(before.tuningHz, 3));
    }
    if (delta.scheme) {
        const auto name = [](AudioEditingScheme::Scheme scheme) {
            return scheme == AudioEditingScheme::Scheme::NotesPrimary ? "openDyne" : "openTune";
        };
        addPreferenceChange(envelope, "editingScheme", "Editing scheme", JsonValue::string(name(before.audioEditingScheme)),
                            JsonValue::string(name(after.audioEditingScheme)), JsonValue());
        names.append(JsonValue::string("editingScheme"));
        revert.appendMember("editingScheme", JsonValue::string(name(before.audioEditingScheme)));
    }
    if (delta.hybrid) {
        addPreferenceChange(envelope, "hybridMode", "Hybrid mode",
                            JsonValue::boolean(before.lightPitchCorrectionEnabled),
                            JsonValue::boolean(after.lightPitchCorrectionEnabled), JsonValue());
        names.append(JsonValue::string("hybridMode"));
        revert.appendMember("hybridMode", JsonValue::boolean(before.lightPitchCorrectionEnabled));
    }
    if (delta.experimental) {
        addPreferenceChange(envelope, "experimentalFeatures", "Experimental features",
                            JsonValue::boolean(before.experimentalFeaturesEnabled),
                            JsonValue::boolean(after.experimentalFeaturesEnabled), JsonValue());
        names.append(JsonValue::string("experimentalFeatures"));
        revert.appendMember("experimentalFeatures", JsonValue::boolean(before.experimentalFeaturesEnabled));
    }
    if (delta.priority) {
        const auto name = [](RenderingPriority priority) {
            return priority == RenderingPriority::CpuFirst ? "cpuFirst" : "gpuFirst";
        };
        addPreferenceChange(envelope, "renderingPriority", "Rendering priority",
                            JsonValue::string(name(before.renderingPriority)),
                            JsonValue::string(name(after.renderingPriority)), JsonValue());
        names.append(JsonValue::string("renderingPriority"));
        revert.appendMember("renderingPriority", JsonValue::string(name(before.renderingPriority)));
    }
    if (delta.f0) {
        const auto previous = controlF0ModelName(before.f0ModelType);
        addPreferenceChange(envelope, "f0Model", "Pitch model", JsonValue::string(previous),
                            JsonValue::string(controlF0ModelName(after.f0ModelType)), JsonValue());
        names.append(JsonValue::string("f0Model"));
        revert.appendMember("f0Model", JsonValue::string(previous));
    }
    if (delta.accessibilityMode) {
        addPreferenceChange(envelope, "accessibilityModeEnabled", "Accessibility mode",
                            JsonValue::boolean(before.accessibilityModeEnabled),
                            JsonValue::boolean(after.accessibilityModeEnabled), JsonValue());
        names.append(JsonValue::string("accessibilityModeEnabled"));
        revert.appendMember("accessibilityModeEnabled", JsonValue::boolean(before.accessibilityModeEnabled));
    }

    if (delta.any()) {
        auto data = JsonValue::object();
        data.appendMember("preferences", std::move(names));
        envelope.addWarning("GLOBAL_PREFERENCE", JsonValue(), std::move(data));
    }
    if (delta.tuning)
        envelope.addWarning("NOT_RETROACTIVE", preferenceRef("tuningHz"));
    if (delta.priority && after.renderingPriority == RenderingPriority::GpuFirst
        && std::string(controlInferenceBackendName()) == "cpu") {
        auto fallback = JsonValue::object();
        fallback.appendMember("requested", JsonValue::string("gpuFirst"));
        fallback.appendMember("actual", JsonValue::string("cpu"));
        envelope.addWarning("BACKEND_FALLBACK", JsonValue(), std::move(fallback));
    }
    if (delta.hybrid || runtimeCompletion.backendChanged) {
        auto ids = JsonValue::array();
        for (const auto& key : controlLiveContents(context))
            ids.append(JsonValue::string(controlContentId(context, key)));
        if (ids.size() != 0) {
            auto effect = JsonValue::object();
            effect.appendMember("contentIds", std::move(ids));
            effect.appendMember("revert", JsonValue());
            envelope.addSideEffect("reRenderQueued", std::move(effect));
        }
    }
    if (revert.size() == 0)
        envelope.setNotUndoable("ui_parity");
    else
        envelope.setNotUndoable("ui_parity", controlRevertRecipe("prefs.set", std::move(revert)));
}

// ---- Notes that a change of the editing scheme makes the editor generate --------------------------------

/** The clip open in the piano roll before the scheme changes, with whether its notes were ever generated. */
struct NotesWatch
{
    bool valid = false;
    ContentKey key;
    std::string contentId;
    bool topologyInitialized = false;
};

NotesWatch captureNotesWatch(ControlOpContext& context)
{
    NotesWatch watch;
    auto* bridge = context.bridge();
    if (bridge == nullptr)
        return watch;
    // The Standalone editor applies the shared initialization command to its active clip on a scheme change.
    const auto view = bridge->viewState();
    if (!view || view->pianoRollPlacementId == 0)
        return watch;
    ControlHandle handle;
    handle.kind = ControlHandleKind::Placement;
    handle.instanceShort = context.instanceShort();
    handle.epoch = controlContentEpoch(context);
    handle.object = view->pianoRollPlacementId;
    const auto placement = controlResolvePlacement(context, handle);
    if (!placement.ok())
        return watch;
    const auto snapshot = controlLiveSnapshot(context, placement.value().placement.contentKey);
    if (snapshot == nullptr)
        return watch;
    watch.valid = true;
    watch.key = placement.value().placement.contentKey;
    watch.contentId = controlContentId(context, watch.key);
    watch.topologyInitialized = snapshot->noteTopologyInitialized;
    return watch;
}

/** Whether the editor generated the notes of the watched clip since the watch was taken (they were not generated
    before, and the content still exists). */
bool notesWereGenerated(ControlOpContext& context, const NotesWatch& watch)
{
    if (!watch.valid || watch.topologyInitialized)
        return false;
    const auto snapshot = controlLiveSnapshot(context, watch.key);
    return snapshot != nullptr && snapshot->noteTopologyInitialized;
}

/** Reports the generated notes and their irreversible history policy. */
bool recordGeneratedNotes(ControlOpContext& context, ControlWriteEnvelope& envelope, const NotesWatch& watch)
{
    if (!notesWereGenerated(context, watch))
        return false;
    const auto snapshot = controlLiveSnapshot(context, watch.key);
    auto effect = JsonValue::object();
    effect.appendMember("contentId", JsonValue::string(watch.contentId));
    effect.appendMember("noteCount", JsonValue::integer(static_cast<std::int64_t>(snapshot->notes.size())));
    effect.appendMember("revert", JsonValue());
    envelope.addSideEffect("notesGenerated", std::move(effect));
    for (const char* code : {"SCHEME_GENERATED_NOTES", "IRREVERSIBLE"}) {
        auto subject = JsonValue::object();
        subject.appendMember("contentId", JsonValue::string(watch.contentId));
        envelope.addWarning(code, std::move(subject));
    }
    envelope.touchContent(watch.contentId);
    return true;
}

// ---- The job of an asynchronous preference change ------------------------------------------------------

class PrefsApplyJob
{
public:
    PrefsApplyJob(ControlOpEnvironment environment, PrefsRequest request)
        : environment_(std::move(environment))
        , request_(std::move(request))
    {}

    void step(ControlJobStep& jobStep)
    {
        ControlOpContext context(environment_);
        if (!started_) {
            if (const auto busy = controlCheckBusy(context)) {
                const auto reply = controlMapError(*busy);
                jobStep.fail(ControlJobError{reply.kind, reply.reason, reply.detail, false});
                return;
            }
            envelope_.emplace(context);
            const auto initial = readProcessingPreferenceState(context.processor().getAppPreferences());
            const auto requested = computeProcessingPreferenceDelta(initial, preferencePatch(request_));
            watch_ = requested.scheme && request_.notesPrimary.value_or(false)
                ? captureNotesWatch(context) : NotesWatch{};

            auto completion = std::make_shared<RuntimeCompletionState>();
            auto wake = jobStep.wakeCallback();
            auto applied = applyPreferencePatch(
                context, request_, [completion, wake](ProcessingPreferenceRuntimeCompletion value) {
                    completion->value = std::move(value);
                    completion->ready = true;
                    wake();
                });
            if (const auto refused = applyError(applied)) {
                const auto reply = controlMapError(*refused);
                jobStep.fail(ControlJobError{reply.kind, reply.reason, reply.detail, reply.executed.value_or(false)});
                return;
            }
            before_ = applied.before;
            delta_ = applied.delta;
            after_ = readProcessingPreferenceState(context.processor().getAppPreferences());
            runtimeCompletion_ = std::move(completion);
            started_ = true;
        }
        if (delta_.needsRuntimeCompletion() && !runtimeCompletion_->ready)
            return;
        finish(context, jobStep);
    }

private:
    struct RuntimeCompletionState
    {
        bool ready = false;
        ProcessingPreferenceRuntimeCompletion value;
    };

    void finish(ControlOpContext& context, ControlJobStep& jobStep)
    {
        const auto actualDelta = diffProcessingPreferenceStates(before_, after_);
        const auto completion = runtimeCompletion_->value;
        recordDelta(context, *envelope_, before_, after_, actualDelta, completion);
        const bool notesGenerated = recordGeneratedNotes(context, *envelope_, watch_);
        envelope_->limitDiffToTouchedContents();
        auto result = envelope_->committed(context);
        if (completion.failed) {
            jobStep.fail(ControlJobError{
                "MODEL_UNAVAILABLE", "vocoder_load_failed",
                "the runtime could not apply the requested processing preference",
                actualDelta.any() || completion.backendChanged || notesGenerated
            }, std::move(result));
        } else {
            jobStep.succeed(std::move(result));
        }
    }

    ControlOpEnvironment environment_;
    PrefsRequest request_;
    ProcessingPreferenceState before_;
    ProcessingPreferenceState after_;
    ProcessingPreferenceDelta delta_;
    NotesWatch watch_;
    std::optional<ControlWriteEnvelope> envelope_;
    std::shared_ptr<RuntimeCompletionState> runtimeCompletion_;
    bool started_ = false;
};

// ---- prefs.set -----------------------------------------------------------------------------------------

/** Error returned when starting a job is refused. */
Error startRefusal(const std::string& refusal)
{
    return refusal == "job_limit" ? controlOpError("BUSY", "job_limit", "every retained job is still active")
                                  : controlOpError("SHUTTING_DOWN", "", "the control API is stopping");
}

/**
 * Checks the pitch model against processing.options of prefs.get and resolves its type. Reads the disk on every call
 * that names a pitch model; its list is scanned from the models directory, as controlPrefsOptionsJson() does.
 * Message thread.
 */
std::optional<Error> checkOptions(PrefsRequest& request)
{
    if (request.f0Model) {
        const auto names = controlF0ModelOptions();
        const auto type = f0TypeOf(*request.f0Model);
        if (!type || std::find(names.begin(), names.end(), *request.f0Model) == names.end())
            return controlOpError("INVALID_ARGUMENT", "option_unavailable",
                                  "f0Model is not one of processing.options.f0Model");
        request.f0Type = type;
    }
    return std::nullopt;
}

Result<JsonValue> runPrefsSet(ControlOpContext& context, const PrefsRequest& parsed)
{
    if (auto busy = controlCheckBusy(context))
        return *busy;
    auto request = parsed;
    if (auto unavailable = checkOptions(request))
        return *unavailable;
    const auto before = readProcessingPreferenceState(context.processor().getAppPreferences());
    const auto delta = computeProcessingPreferenceDelta(before, preferencePatch(request));
    if (delta.needsJob()) {
        auto* jobs = context.jobs();
        if (jobs == nullptr)
            return controlOpError("INTERNAL", "", "the jobs registry is not available");
        auto job = std::make_shared<PrefsApplyJob>(context.environment(), request);
        const auto started = jobs->start(kJobKindPrefsApply, false, [job](ControlJobStep& step) { job->step(step); });
        if (started.jobId.empty())
            return startRefusal(started.refusal);
        ControlWriteEnvelope envelope(context);
        return envelope.handedToJob(context, jobs->jobRef(started.jobId));
    }
    ControlWriteEnvelope envelope(context);
    const auto applied = applyPreferencePatch(context, request);
    if (const auto refused = applyError(applied))
        return *refused;
    const auto after = readProcessingPreferenceState(context.processor().getAppPreferences());
    recordDelta(context, envelope, before, after, diffProcessingPreferenceStates(before, after));
    return envelope.committed(context);
}

} // namespace

void registerPrefsOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<PrefsRequest>("prefs.set", parsePrefsSet, runPrefsSet));
}

} // namespace OpenTune
