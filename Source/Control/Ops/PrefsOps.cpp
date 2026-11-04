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
#include <atomic>
#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

constexpr const char* kJobKindPrefsApply = "prefsApply";
/** How long a prefsApply job waits for the editor to apply the editing scheme. The editor applies the preferences
    synchronously when the bridge hands them over and again on its heartbeat (30 Hz when idle), so the wait only
    matters for an editor that has stopped running its heartbeat. */
constexpr std::uint32_t kDefaultApplyTimeoutMs = 10000;
/** How long a prefsApply job waits for the vocoder to be ready again after a change of the vocoder weight or the
    rendering priority. Longer than kDefaultApplyTimeoutMs: the runtime destroys the vocoder session and loads the model
    again, which takes seconds on a slow disk or a cold GPU. The specification names no number (an implementation
    defined limit). */
constexpr std::uint32_t kDefaultVocoderWaitMs = 60000;

std::atomic<std::uint32_t>& applyTimeoutOverrideMs()
{
    static std::atomic<std::uint32_t> overrideMs{0};
    return overrideMs;
}

std::atomic<std::uint32_t>& vocoderWaitOverrideMs()
{
    static std::atomic<std::uint32_t> overrideMs{0};
    return overrideMs;
}

std::uint32_t applyTimeoutMs()
{
    const auto overrideMs = applyTimeoutOverrideMs().load();
    return overrideMs != 0 ? overrideMs : kDefaultApplyTimeoutMs;
}

std::uint32_t vocoderWaitMs()
{
    const auto overrideMs = vocoderWaitOverrideMs().load();
    return overrideMs != 0 ? overrideMs : kDefaultVocoderWaitMs;
}

// ---- Params ---------------------------------------------------------------------------------------

/** methods/prefs.set.schema.json $defs.params: the members that are present. */
struct PrefsRequest
{
    std::optional<float> tuningHz;
    /** editingScheme: true for openDyne (notes primary), false for openTune. */
    std::optional<bool> notesPrimary;
    std::optional<bool> hybridMode;
    std::optional<bool> experimentalFeatures;
    /** vocoderWeight: a file name of processing.options.vocoderWeight (checked when the call runs). */
    std::optional<std::string> vocoderWeight;
    /** renderingPriority: true for cpuFirst, false for gpuFirst. */
    std::optional<bool> cpuFirst;
    /** f0Model: a name of processing.options.f0Model (checked when the call runs), and the type it names. */
    std::optional<std::string> f0Model;
    std::optional<F0ModelType> f0Type;
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
    if (!reader.onlyMembers({"tuningHz", "editingScheme", "hybridMode", "experimentalFeatures", "vocoderWeight",
                             "renderingPriority", "f0Model"})
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
        || !readOptionName(reader, "vocoderWeight", out.vocoderWeight)
        || !readEnumPair(reader, "renderingPriority", "gpuFirst", "cpuFirst", out.cpuFirst)
        || !readOptionName(reader, "f0Model", out.f0Model)) {
        why = reader.why();
        return false;
    }
    if (tuning)
        out.tuningHz = roundedTuning(*tuning);
    if (!out.tuningHz && !out.notesPrimary && !out.hybridMode && !out.experimentalFeatures && !out.vocoderWeight
        && !out.cpuFirst && !out.f0Model) {
        why = "params must have at least one member";
        return false;
    }
    return true;
}

// ---- The preferences, and what a request changes -----------------------------------------------------

/** The preferences prefs.set changes, as prefs.get reports them. */
struct PrefsState
{
    float tuningHz = TuningConfig::kDefaultTuningHz;
    bool notesPrimary = false;
    bool hybridMode = true;
    bool experimentalFeatures = false;
    std::string vocoderWeight = kDefaultVocoderWeight;
    bool cpuFirst = false;
    std::string f0Model = "fcpe";
};

PrefsState readState(OpenTuneAudioProcessor& processor)
{
    // AppPreferences::getState(): a copy taken under the preferences' own mutex; TuningConfig::currentTuningHz():
    // the A4 frequency that analysis and rendering use (the same sources as prefs.get).
    const auto shared = processor.getAppPreferences().getState().shared;
    PrefsState state;
    state.tuningHz = roundedTuning(TuningConfig::currentTuningHz());
    state.notesPrimary = AudioEditingScheme::usesNotesPrimaryScheme(shared.audioEditingScheme);
    state.hybridMode = shared.lightPitchCorrectionEnabled;
    state.experimentalFeatures = shared.experimentalFeaturesEnabled;
    state.vocoderWeight = shared.vocoderModelWeight;
    state.cpuFirst = shared.renderingPriority == RenderingPriority::CpuFirst;
    state.f0Model = controlF0ModelName(shared.f0ModelType);
    return state;
}

/** Which preferences a request changes: the members that differ from the state. */
struct PrefsDelta
{
    bool tuning = false;
    bool scheme = false;
    bool hybrid = false;
    bool experimental = false;
    bool vocoder = false;
    bool priority = false;
    bool f0 = false;

    bool any() const noexcept { return tuning || scheme || hybrid || experimental || vocoder || priority || f0; }
    /** The editor applies the scheme asynchronously, and the runtime replaces the vocoder asynchronously after a new
        weight or a new rendering priority: a call that changes one of them is a job. */
    bool needsJob() const noexcept { return scheme || vocoder || priority; }
};

PrefsDelta computeDelta(const PrefsState& state, const PrefsRequest& request)
{
    PrefsDelta delta;
    delta.tuning = request.tuningHz && *request.tuningHz != state.tuningHz;
    delta.scheme = request.notesPrimary && *request.notesPrimary != state.notesPrimary;
    delta.hybrid = request.hybridMode && *request.hybridMode != state.hybridMode;
    delta.experimental = request.experimentalFeatures && *request.experimentalFeatures != state.experimentalFeatures;
    delta.vocoder = request.vocoderWeight && *request.vocoderWeight != state.vocoderWeight;
    delta.priority = request.cpuFirst && *request.cpuFirst != state.cpuFirst;
    delta.f0 = request.f0Model && *request.f0Model != state.f0Model;
    return delta;
}

/** The state that a request makes of the state before it: the preferences of the delta set to the request's values. */
PrefsState stateAfter(const PrefsState& before, const PrefsRequest& request, const PrefsDelta& delta)
{
    auto after = before;
    if (delta.tuning)
        after.tuningHz = *request.tuningHz;
    if (delta.scheme)
        after.notesPrimary = *request.notesPrimary;
    if (delta.hybrid)
        after.hybridMode = *request.hybridMode;
    if (delta.experimental)
        after.experimentalFeatures = *request.experimentalFeatures;
    if (delta.vocoder)
        after.vocoderWeight = *request.vocoderWeight;
    if (delta.priority)
        after.cpuFirst = *request.cpuFirst;
    if (delta.f0)
        after.f0Model = *request.f0Model;
    return after;
}

/** The type a pitch model name stands for among the pitch models that OpenTune lists. */
std::optional<F0ModelType> f0TypeOf(const std::string& name)
{
    for (const auto& model : controlF0Models())
        if (model.name == name)
            return model.type;
    return std::nullopt;
}

/**
 * Makes the changes of a request, as OpenTune's preferences window does for each control (SharedEditingPage's tuning
 * slider and scheme selector, SharedAudioPage's experimental toggle, hybrid toggle with its processor callback in
 * showPreferencesDialog, rendering priority, vocoder weight and pitch model selectors with their processor callbacks),
 * and then lets the editor apply the preferences. The pitch model comes first, as the
 * window asks the processor before it stores the model: when the processor refuses, nothing changes and the error is
 * returned. Message thread.
 */
std::optional<Error> applyDelta(ControlOpContext& context, const PrefsRequest& request, const PrefsDelta& delta)
{
    auto& processor = context.processor();
    // AppPreferences setters: each stores the value, persists the preferences file and, for the tuning, updates the
    // process-wide A4 frequency.
    auto& preferences = processor.getAppPreferences();
    if (delta.f0) {
        // f0ModelSelector_.onChange: the processor switches the model first (setF0ModelType); a refusal leaves the
        // window's preference as it is, and only an accepted model is stored.
        if (!request.f0Type || !controlSetF0Model(processor, *request.f0Type))
            return controlOpError("MODEL_UNAVAILABLE", "model_missing", "OpenTune could not switch the pitch model");
        preferences.setF0ModelType(*request.f0Type);
    }
    if (delta.tuning) {
        TuningConfig::TuningSettings tuning;
        tuning.tuningHz = *request.tuningHz;
        preferences.setTuning(tuning);
    }
    if (delta.scheme)
        preferences.setAudioEditingScheme(*request.notesPrimary ? AudioEditingScheme::Scheme::NotesPrimary
                                                                : AudioEditingScheme::Scheme::CorrectedF0Primary);
    if (delta.hybrid) {
        preferences.setLightPitchCorrectionEnabled(*request.hybridMode);
        // The window's hybrid toggle calls this next: every content renders again with the new mode.
        processor.invalidateAllContentCaches();
    }
    if (delta.experimental)
        preferences.setExperimentalFeaturesEnabled(*request.experimentalFeatures);
    if (delta.vocoder) {
        // vocoderWeightSelector_.onChange: the preference first, then the processor's asynchronous switch (the
        // runtime's control worker destroys the vocoder session and loads the weight).
        preferences.setVocoderModelWeight(*request.vocoderWeight);
        controlSetVocoderWeight(processor, *request.vocoderWeight);
    }
    if (delta.priority) {
        // renderingPrioritySelector_.onChange: the preference first, then the processor's asynchronous reset of the
        // inference backend (cpuFirst forces the CPU).
        preferences.setRenderingPriority(*request.cpuFirst ? RenderingPriority::CpuFirst : RenderingPriority::GpuFirst);
        controlResetInferenceBackend(processor, *request.cpuFirst);
    }
    if (delta.any())
        if (auto* bridge = context.bridge())
            bridge->onPreferencesChanged();
    return std::nullopt;
}

/**
 * Puts the preferences a delta changed back to the state before it; the editor is told only when asked to. With a
 * context the runtime is asked for the previous vocoder weight, rendering priority and pitch model as well, so that
 * the stored preferences and the runtime agree again; without one (the control API is stopping) only the stored
 * preferences change.
 */
void restoreState(ControlOpContext* context, OpenTuneAudioProcessor& processor, const PrefsState& before,
                  const PrefsDelta& delta)
{
    auto& preferences = processor.getAppPreferences();
    if (delta.f0)
        if (const auto type = f0TypeOf(before.f0Model)) {
            if (context != nullptr)
                controlSetF0Model(processor, *type);
            preferences.setF0ModelType(*type);
        }
    if (delta.tuning) {
        TuningConfig::TuningSettings tuning;
        tuning.tuningHz = before.tuningHz;
        preferences.setTuning(tuning);
    }
    if (delta.scheme)
        preferences.setAudioEditingScheme(before.notesPrimary ? AudioEditingScheme::Scheme::NotesPrimary
                                                              : AudioEditingScheme::Scheme::CorrectedF0Primary);
    if (delta.hybrid) {
        preferences.setLightPitchCorrectionEnabled(before.hybridMode);
        if (context != nullptr)
            processor.invalidateAllContentCaches();
    }
    if (delta.experimental)
        preferences.setExperimentalFeaturesEnabled(before.experimentalFeatures);
    if (delta.vocoder) {
        preferences.setVocoderModelWeight(before.vocoderWeight);
        if (context != nullptr)
            controlSetVocoderWeight(processor, before.vocoderWeight);
    }
    if (delta.priority) {
        preferences.setRenderingPriority(before.cpuFirst ? RenderingPriority::CpuFirst : RenderingPriority::GpuFirst);
        if (context != nullptr)
            controlResetInferenceBackend(processor, before.cpuFirst);
    }
    if (context != nullptr && delta.any())
        if (auto* bridge = context->bridge())
            bridge->onPreferencesChanged();
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

/** A stored vocoder weight as the wire carries it, or nullopt when it cannot be sent back (empty, not UTF-8 or longer
    than the limit of processing.options). */
std::optional<JsonValue> wireWeight(const std::string& weight)
{
    if (weight.empty() || !isValidUtf8(weight) || utf8CodePointCount(weight) > kControlOptionTextCodePoints)
        return std::nullopt;
    return JsonValue::string(weight);
}

/**
 * Records what a delta changed in the envelope: the changes, the warning GLOBAL_PREFERENCE (NOT_RETROACTIVE for the
 * tuning, BACKEND_FALLBACK for a change to gpuFirst that ends on the CPU backend), reRenderQueued for the hybrid
 * switch and for every change of the vocoder weight, and the undo descriptor
 * (no undo entry, as in OpenTune; the revert recipe is the same call with the previous values, or null when none of
 * them can be sent). The members come in the order of the schema: the original four, then vocoderWeight,
 * renderingPriority and f0Model. Nothing is recorded for a delta that changes nothing.
 */
void recordDelta(ControlOpContext& context, ControlWriteEnvelope& envelope, const PrefsState& before,
                 const PrefsRequest& request, const PrefsDelta& delta)
{
    if (!delta.any())
        return;
    auto names = JsonValue::array();
    auto revert = JsonValue::object();
    if (delta.tuning) {
        addPreferenceChange(envelope, "tuningHz", "Tuning (A4)", controlJsonNumber(before.tuningHz, 3),
                            controlJsonNumber(*request.tuningHz, 3), JsonValue::string("hz"));
        names.append(JsonValue::string("tuningHz"));
        revert.appendMember("tuningHz", controlJsonNumber(before.tuningHz, 3));
    }
    if (delta.scheme) {
        addPreferenceChange(envelope, "editingScheme", "Editing scheme",
                            JsonValue::string(before.notesPrimary ? "openDyne" : "openTune"),
                            JsonValue::string(*request.notesPrimary ? "openDyne" : "openTune"), JsonValue());
        names.append(JsonValue::string("editingScheme"));
        revert.appendMember("editingScheme", JsonValue::string(before.notesPrimary ? "openDyne" : "openTune"));
    }
    if (delta.hybrid) {
        addPreferenceChange(envelope, "hybridMode", "Hybrid mode", JsonValue::boolean(before.hybridMode),
                            JsonValue::boolean(*request.hybridMode), JsonValue());
        names.append(JsonValue::string("hybridMode"));
        revert.appendMember("hybridMode", JsonValue::boolean(before.hybridMode));
    }
    if (delta.experimental) {
        addPreferenceChange(envelope, "experimentalFeatures", "Experimental features",
                            JsonValue::boolean(before.experimentalFeatures),
                            JsonValue::boolean(*request.experimentalFeatures), JsonValue());
        names.append(JsonValue::string("experimentalFeatures"));
        revert.appendMember("experimentalFeatures", JsonValue::boolean(before.experimentalFeatures));
    }
    if (delta.vocoder) {
        const auto previous = wireWeight(before.vocoderWeight);
        addPreferenceChange(envelope, "vocoderWeight", "Vocoder weight", previous ? *previous : JsonValue(),
                            JsonValue::string(*request.vocoderWeight), JsonValue());
        names.append(JsonValue::string("vocoderWeight"));
        // A stored weight that the wire cannot carry has no value to revert to.
        if (previous)
            revert.appendMember("vocoderWeight", *previous);
    }
    if (delta.priority) {
        addPreferenceChange(envelope, "renderingPriority", "Rendering priority",
                            JsonValue::string(before.cpuFirst ? "cpuFirst" : "gpuFirst"),
                            JsonValue::string(*request.cpuFirst ? "cpuFirst" : "gpuFirst"), JsonValue());
        names.append(JsonValue::string("renderingPriority"));
        revert.appendMember("renderingPriority", JsonValue::string(before.cpuFirst ? "cpuFirst" : "gpuFirst"));
    }
    if (delta.f0) {
        addPreferenceChange(envelope, "f0Model", "Pitch model", JsonValue::string(before.f0Model),
                            JsonValue::string(*request.f0Model), JsonValue());
        names.append(JsonValue::string("f0Model"));
        revert.appendMember("f0Model", JsonValue::string(before.f0Model));
    }
    auto data = JsonValue::object();
    data.appendMember("preferences", std::move(names));
    envelope.addWarning("GLOBAL_PREFERENCE", JsonValue(), std::move(data));
    if (delta.tuning)
        envelope.addWarning("NOT_RETROACTIVE", preferenceRef("tuningHz"));
    if (delta.priority && !*request.cpuFirst) {
        // BACKEND_FALLBACK (PROTOCOL.md section 12.7 rule 7): this call changed the priority to gpuFirst, and the
        // backend that OpenTune reports now, when the job ends, is the CPU, so the GPU is not in use. The backend is
        // read once, here, and the job does not wait for it: a job that did not wait for the vocoder (it was not
        // ready before the change) can still read the backend from before the change, which the rule allows. A change
        // to cpuFirst, or no change of the priority, never carries the warning.
        const std::string actual = controlInferenceBackendName();
        if (actual == "cpu") {
            auto fallback = JsonValue::object();
            fallback.appendMember("requested", JsonValue::string("gpuFirst"));
            fallback.appendMember("actual", JsonValue::string(actual));
            envelope.addWarning("BACKEND_FALLBACK", JsonValue(), std::move(fallback));
        }
    }
    if (delta.hybrid || delta.vocoder) {
        // The processor queued a new render of every content: invalidateAllContentCaches() for the hybrid switch; for
        // the vocoder weight the completion of setVocoderModelWeight() clears every render cache and requests a full
        // render of every clip once the new weight has loaded. That holds for every change of the weight, whether or
        // not the job waited for the vocoder: a job that did not wait (the vocoder was not ready before the change)
        // ends before the render starts, and a weight that cannot be loaded renders nothing (session.info keeps
        // reporting vocoderReady false). The rendering priority clears no cache.
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
    // A stored vocoder weight that the wire cannot carry is left out of the recipe; when nothing else is left (only
    // the vocoder weight changed), no request can restore the state, and revert is null (the envelope then adds the
    // warning IRREVERSIBLE), never a prefs.set with empty params.
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
    // ControlUiBridge::viewState(): the placement the piano roll shows; the editor generates the notes of the content
    // it edits when the scheme becomes openDyne (PianoRollComponent::ensureOpenDyneNotesIfNeeded).
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

/** Records the notes the editor generated for the watched clip: the side effect, the two warnings, the project marked
    as changed (the scheme change has no undo entry, and nothing but deleting the clip takes the notes away). */
void recordGeneratedNotes(ControlOpContext& context, ControlWriteEnvelope& envelope, const NotesWatch& watch)
{
    if (!notesWereGenerated(context, watch))
        return;
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
    controlMarkDirty(context);
}

// ---- The job of a change that OpenTune applies asynchronously -------------------------------------------

/**
 * The job of a prefs.set that changes the editing scheme, the vocoder weight or the rendering priority. Its first step
 * makes every change of the request (so the call that started it changed nothing), the following steps wait, and the
 * last step ends the job with the write envelope. What the job waits for:
 *
 * - The editing scheme: until the editor reports the scheme as applied. If it does not within applyTimeoutMs(), the job
 *   takes its changes back (withdraw()) and fails with INTERNAL.
 * - The vocoder weight and the rendering priority, when the vocoder was ready before the first step: until the vocoder
 *   is ready again, is not reconfiguring and the runtime's control worker has nothing left to do. The flag
 *   "reconfiguring" cannot be the only sign: OpenTuneAudioProcessor::setVocoderModelWeight() and
 * resetInferenceBackend() only post a command to the process-wide ProcessRenderRuntime and return, and the control
 * worker thread sets the flag (and takes the vocoder away) when it starts the command, so the vocoder stays "ready, not
 * reconfiguring" until then. The command stays in the worker's queue or is the worker's active transaction until its
 * completion callback ran on the message thread, so an idle worker proves that the command was executed, whatever it
 * did (a weight the runtime already holds is answered without any flag). If the vocoder is not back within
 * vocoderWaitMs(), the job takes its changes back and fails with MODEL_UNAVAILABLE(vocoder_not_ready), which can
 * succeed when asked again. If the worker is idle, no reconfiguration is running and the vocoder is not there, the
 * command ran and could not load a vocoder (a damaged or incompatible weight file; the runtime keeps asking for the
 * same weight, so the vocoder never recovers by itself): the job takes its changes back at once and fails with
 * MODEL_UNAVAILABLE(vocoder_load_failed), which is not retryable. A vocoder that was not ready before the change is
 * not waited for: the job ends as soon as the changes are made.
 *
 * When a job takes its changes back it hands the previous vocoder weight and rendering priority back to the runtime
 * (restoreState()), which loads them again: session.info reports the vocoder as reconfiguring while it does, and
 * vocoderReady afterwards says whether it could. The one change that cannot be taken back is the notes that the switch
 * to openDyne made the editor generate for the clip open in the piano roll (no undo entry): they stay, the project
 * stays marked as changed, the error's detail says so and executed is true.
 *
 * A job that the registry destroys before it ended (the control API is stopping) also takes its changes back.
 */
class PrefsApplyJob
{
public:
    PrefsApplyJob(ControlOpEnvironment environment, PrefsRequest request)
        : environment_(std::move(environment))
        , request_(std::move(request))
    {}

    ~PrefsApplyJob()
    {
        // Destroyed on the message thread before the processor goes away (ControlJobs::stop()).
        if (started_ && !finished_ && environment_.processor != nullptr)
            withdraw(nullptr, *environment_.processor);
        leavePending();
    }

    void step(ControlJobStep& jobStep)
    {
        ControlOpContext context(environment_);
        if (!started_) {
            // The call checked the busy state when it started the job, but the job's first step runs later: a modal
            // dialog or an export may have begun meanwhile. Nothing was changed yet, so the job just fails.
            if (const auto busy = controlCheckBusy(context)) {
                const auto reply = controlMapError(*busy);
                jobStep.fail(ControlJobError{reply.kind, reply.reason, reply.detail, false});
                finished_ = true;
                return;
            }
            if (const auto refused = begin(context)) {
                // The processor could not switch the pitch model, which is applied first: nothing was changed.
                const auto reply = controlMapError(*refused);
                jobStep.fail(ControlJobError{reply.kind, reply.reason, reply.detail, false});
                finished_ = true;
                return;
            }
        }
        auto* bridge = context.bridge();
        bool applied = true;
        if (delta_.scheme && bridge != nullptr)
            if (const auto reported = bridge->editingSchemeApplied(*request_.notesPrimary))
                applied = *reported;
        bool vocoderBack = true;
        if (waitsForVocoder_) {
            const auto vocoder = controlVocoderState();
            vocoderBack = vocoder.ready && !vocoder.reconfiguring && vocoder.controlIdle;
            // The command was posted before the first state read, so an idle worker means it ran to its completion.
            // The vocoder was ready before it: if it is gone now and nothing is reconfiguring, the load failed, and
            // waiting for the limit would only delay the same failure.
            if (vocoder.controlIdle && !vocoder.reconfiguring && !vocoder.ready) {
                abandon(context, jobStep, "MODEL_UNAVAILABLE", "vocoder_load_failed",
                        "the vocoder could not be loaded with the new vocoder weight or rendering priority");
                return;
            }
        }
        if (applied && vocoderBack) {
            finish(context, jobStep);
            return;
        }
        const auto waitedMs = juce::Time::getMillisecondCounter() - startedAtMs_;
        if (!applied && waitedMs >= applyTimeoutMs()) {
            abandon(context, jobStep, "INTERNAL", "", "the editor did not apply the editing scheme in time");
        } else if (!vocoderBack && waitedMs >= vocoderWaitMs()) {
            abandon(context, jobStep, "MODEL_UNAVAILABLE", "vocoder_not_ready",
                    "the vocoder was not ready again in time after the vocoder weight or the rendering priority "
                    "changed");
        }
    }

private:
    /** The jobs that made their changes and have not ended, in the order they made them. Message thread only. A job
        that takes its changes back looks for the later ones among them (settle()). */
    static std::vector<PrefsApplyJob*>& pendingJobs()
    {
        static std::vector<PrefsApplyJob*> jobs;
        return jobs;
    }

    void leavePending()
    {
        auto& jobs = pendingJobs();
        jobs.erase(std::remove(jobs.begin(), jobs.end(), this), jobs.end());
    }

    /** The envelope starts immediately before the change; the change is made and handed to the editor. Returns the
       error of the pitch model that the processor refused, in which case nothing was changed. */
    std::optional<Error> begin(ControlOpContext& context)
    {
        before_ = readState(context.processor());
        delta_ = computeDelta(before_, request_);
        written_ = stateAfter(before_, request_, delta_);
        restoreTo_ = before_;
        envelope_.emplace(context);
        watch_ = delta_.scheme && *request_.notesPrimary ? captureNotesWatch(context) : NotesWatch{};
        // Whether the vocoder is there to be waited for is decided before the commands are posted.
        if (delta_.vocoder || delta_.priority) {
            const auto vocoder = controlVocoderState();
            waitsForVocoder_ = vocoder.ready && !vocoder.reconfiguring;
        }
        if (auto refused = applyDelta(context, request_, delta_))
            return refused;
        startedAtMs_ = juce::Time::getMillisecondCounter();
        started_ = true;
        pendingJobs().push_back(this);
        return std::nullopt;
    }

    void finish(ControlOpContext& context, ControlJobStep& jobStep)
    {
        recordDelta(context, *envelope_, before_, request_, delta_);
        recordGeneratedNotes(context, *envelope_, watch_);
        // The job runs while the user may change other contents.
        envelope_->limitDiffToTouchedContents();
        auto result = envelope_->committed(context);
        finished_ = true;
        leavePending();
        jobStep.succeed(std::move(result));
    }

    /** What the job waits for did not happen in time (the editor applying the scheme, the vocoder being ready again):
        the job takes its changes back, so that no half-applied set of preferences stays, and fails with the error of
        the wait that ran out. A preference that another write changed meanwhile keeps that write's value; the detail
        names it, and executed is true because the job's change was not rolled back completely (PROTOCOL.md section
        17.4 rule 3). The same holds for the notes that the editor generated for the clip open in the piano roll when it
        applied openDyne: they have no undo entry and stay, the project stays marked as changed, and the detail names
        the content (a handle, never a file name). */
    void abandon(ControlOpContext& context, ControlJobStep& jobStep, const char* kind, const char* reason,
                 const char* what)
    {
        // Looked at before the changes are taken back: taking the scheme back does not remove the notes.
        const bool notesKept = notesWereGenerated(context, watch_);
        const auto kept = withdraw(&context, context.processor());
        finished_ = true;
        leavePending();
        std::string detail = std::string(what) + "; the previous preferences were restored";
        if (!kept.empty()) {
            detail += " except those another write changed while the job waited, which keep that write's value:";
            for (const auto& name : kept)
                detail += " " + name;
        }
        if (notesKept) {
            // controlMarkDirty(): the project has unsaved changes now, as it has after the job that ended normally.
            controlMarkDirty(context);
            detail += std::string(kept.empty() ? "; " : ", and ")
                      + "the editor had generated notes for the clip open in the piano roll (content "
                      + watch_.contentId + ") when it applied the editing scheme openDyne, and they were kept";
        }
        jobStep.fail(ControlJobError{kind, reason, detail, !kept.empty() || notesKept});
    }

    /**
     * Takes the changes of this job back, preference by preference (settle()), and tells the editor when context is
     * given. Message thread. Returns the names of the preferences that keep the value of another write.
     */
    std::vector<std::string> withdraw(ControlOpContext* context, OpenTuneAudioProcessor& processor)
    {
        const auto current = readState(processor);
        PrefsDelta restore;
        std::vector<std::string> kept;
        settle(&PrefsDelta::tuning, &PrefsState::tuningHz, "tuningHz", current, restore, kept);
        settle(&PrefsDelta::scheme, &PrefsState::notesPrimary, "editingScheme", current, restore, kept);
        settle(&PrefsDelta::hybrid, &PrefsState::hybridMode, "hybridMode", current, restore, kept);
        settle(&PrefsDelta::experimental, &PrefsState::experimentalFeatures, "experimentalFeatures", current, restore,
               kept);
        settle(&PrefsDelta::vocoder, &PrefsState::vocoderWeight, "vocoderWeight", current, restore, kept);
        settle(&PrefsDelta::priority, &PrefsState::cpuFirst, "renderingPriority", current, restore, kept);
        settle(&PrefsDelta::f0, &PrefsState::f0Model, "f0Model", current, restore, kept);
        restoreState(context, processor, restoreTo_, restore);
        return kept;
    }

    /**
     * How one preference this job changed is taken back. A later pending job of the same processor that read the
     * value this job wrote as its previous value gets this job's previous value instead, so that its own restore never
     * writes this job's value back (its success keeps its own value). Then: a preference that holds the previous value
     * already needs nothing; one that still holds the value this job wrote is marked in restore; one that such a later
     * job changed from that value is left to it; any other was changed by another write while the job waited (a
     * prefs.set, the preferences window), keeps that value and is added to kept.
     */
    template <typename T>
    void settle(bool PrefsDelta::*item, T PrefsState::*value, const char* name, const PrefsState& current,
                PrefsDelta& restore, std::vector<std::string>& kept)
    {
        if (!(delta_.*item))
            return;
        bool handedOver = false;
        auto& jobs = pendingJobs();
        const auto self = std::find(jobs.begin(), jobs.end(), this);
        if (self != jobs.end())
            for (auto later = std::next(self); later != jobs.end(); ++later) {
                auto& job = **later;
                if (job.environment_.processor == environment_.processor && job.delta_.*item
                    && job.restoreTo_.*value == written_.*value) {
                    job.restoreTo_.*value = restoreTo_.*value;
                    handedOver = true;
                }
            }
        if (current.*value == restoreTo_.*value)
            return;
        if (current.*value == written_.*value)
            restore.*item = true;
        else if (!handedOver)
            kept.emplace_back(name);
    }

    ControlOpEnvironment environment_;
    PrefsRequest request_;
    PrefsState before_;
    PrefsDelta delta_;
    /** The preferences this job wrote, and what taking them back writes: before_, except where an earlier job handed
        over its previous value (settle()). */
    PrefsState written_;
    PrefsState restoreTo_;
    NotesWatch watch_;
    std::optional<ControlWriteEnvelope> envelope_;
    std::uint32_t startedAtMs_ = 0;
    /** The vocoder was ready before the job changed the vocoder weight or the rendering priority: the job waits for it
        to be ready again. */
    bool waitsForVocoder_ = false;
    bool started_ = false;
    bool finished_ = false;
};

// ---- prefs.set -----------------------------------------------------------------------------------------

/** Why a start of a job failed, as the error of the method (PROTOCOL.md section 12). */
Error startRefusal(const std::string& refusal)
{
    return refusal == "job_limit" ? controlOpError("BUSY", "job_limit", "every retained job is still active")
                                  : controlOpError("SHUTTING_DOWN", "", "the control API is stopping");
}

/**
 * Checks the vocoder weight and the pitch model of a request against what the preferences window offers
 * (processing.options of prefs.get) and resolves the pitch model's type. A value that is not on offer gives
 * INVALID_ARGUMENT(option_unavailable) with the name of the member, and nothing changes. A value that is equal to the
 * current one is checked as well. Reads the disk on every call that names a vocoder weight or a pitch model (the lists
 * are scanned from the models directory, as controlPrefsOptionsJson() does for prefs.get); the directory is small.
 * Message thread.
 */
std::optional<Error> checkOptions(PrefsRequest& request)
{
    if (request.vocoderWeight) {
        const auto weights = controlVocoderWeightOptions();
        if (std::find(weights.begin(), weights.end(), *request.vocoderWeight) == weights.end())
            return controlOpError("INVALID_ARGUMENT", "option_unavailable",
                                  "vocoderWeight is not one of processing.options.vocoderWeight");
    }
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
    const auto before = readState(context.processor());
    const auto delta = computeDelta(before, request);
    if (delta.needsJob()) {
        // OpenTune applies a new editing scheme to its editor asynchronously, and replaces the vocoder asynchronously
        // after a new weight or rendering priority: a job makes all the changes, and the call itself changes nothing
        // (PROTOCOL.md section 14.2 rule 3).
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
    if (auto refused = applyDelta(context, request, delta))
        return *refused;
    recordDelta(context, envelope, before, request, delta);
    return envelope.committed(context);
}

} // namespace

void registerPrefsOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<PrefsRequest>("prefs.set", parsePrefsSet, runPrefsSet));
}

void controlSetPrefsApplyTimeoutForTesting(std::uint32_t milliseconds)
{
    applyTimeoutOverrideMs() = milliseconds;
}

void controlSetPrefsVocoderWaitForTesting(std::uint32_t milliseconds)
{
    vocoderWaitOverrideMs() = milliseconds;
}

} // namespace OpenTune
