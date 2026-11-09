// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TransportOps.h"

#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OpenTune {

namespace {

/** Positions closer than this are the same position: a position read twice during playback differs by the time
    between the reads, and the presented position is only as exact as the clock. */
constexpr double kSamePositionSec = 1e-6;
/** The tempo and time signature the schemas allow (OpenTune's setters accept more, and limit silently). */
constexpr double kMinBpm = 1.0;
constexpr double kMaxBpm = 999.0;
constexpr int kMaxTimeSignatureNumerator = 32;

JsonValue singletonRef(const char* name)
{
    auto ref = JsonValue::object();
    ref.appendMember("singleton", JsonValue::string(name));
    return ref;
}

/** {before, after, unit} of one changed member (PROTOCOL.md section 14.3). */
JsonValue fieldChange(JsonValue before, JsonValue after, const char* unit)
{
    auto field = JsonValue::object();
    field.appendMember("before", std::move(before));
    field.appendMember("after", std::move(after));
    field.appendMember("unit", unit != nullptr ? JsonValue::string(unit) : JsonValue());
    return field;
}

/** The member bpm of a tempo change: {before, after, unit bpm}, at the three decimals the tempo is reported with. */
JsonValue tempoBpmField(double before, double after)
{
    return fieldChange(controlJsonNumber(before, 3), controlJsonNumber(after, 3), "bpm");
}

/** Records the change of the singleton tempo with the members in fields (bpm, timeSignature): the one builder of
    the change that transport.setTempo and the view switches of ui.setWorkspace and ui.setSelection report. */
void addTempoChange(ControlWriteEnvelope& envelope, JsonValue fields)
{
    envelope.addChange("tempo", singletonRef("tempo"), JsonValue::string("Tempo"), std::move(fields));
}

JsonValue signatureJson(int numerator, int denominator)
{
    auto json = JsonValue::object();
    json.appendMember("numerator", JsonValue::integer(numerator));
    json.appendMember("denominator", JsonValue::integer(denominator));
    return json;
}

// ---- transport.control ----------------------------------------------------------------------------

enum class Action {
    Play,
    Pause,
    Stop,
    Seek,
    PlayFromLastStart,
    PauseAtLastStart
};

struct ControlParams
{
    Action action = Action::Play;
    double positionTimelineSec = 0.0;
};

bool parseControl(const JsonValue& params, ControlParams& out, std::string& why)
{
    ControlParamReader reader(params);
    const JsonValue* action = reader.member("action");
    std::optional<double> position;
    if (!reader.onlyMembers({"action", "positionTimelineSec"})
        || !reader.optionalNumber("positionTimelineSec", 0.0, std::numeric_limits<double>::max(), position)) {
        why = reader.why();
        return false;
    }
    struct Named
    {
        const char* name;
        Action action;
    };
    static const Named kActions[] = {{"play", Action::Play},
                                     {"pause", Action::Pause},
                                     {"stop", Action::Stop},
                                     {"seek", Action::Seek},
                                     {"playFromLastStart", Action::PlayFromLastStart},
                                     {"pauseAtLastStart", Action::PauseAtLastStart}};
    const auto* found = std::find_if(std::begin(kActions), std::end(kActions), [action](const Named& named) {
        return action != nullptr && action->isString() && action->text() == named.name;
    });
    if (found == std::end(kActions)) {
        why = "action must be play, pause, stop, seek, playFromLastStart or pauseAtLastStart";
        return false;
    }
    out.action = found->action;
    // positionTimelineSec is required with seek and not allowed with the other actions.
    if (out.action == Action::Seek) {
        if (!position) {
            why = "positionTimelineSec is required with action seek";
            return false;
        }
        out.positionTimelineSec = *position;
    } else if (position) {
        why = "positionTimelineSec is only allowed with action seek";
        return false;
    }
    return true;
}

/** The transport state transport.control compares before and after a call. */
struct PlayState
{
    bool playing = false;
    double positionSec = 0.0;
    double playStartSec = 0.0;
};

PlayState readPlayState(OpenTuneAudioProcessor& processor)
{
    PlayState state;
    state.playing = processor.isPlaying();
    state.positionSec = std::max(0.0, processor.getPosition());
    state.playStartSec = std::max(0.0, processor.getPlayStartPosition());
    return state;
}

Result<JsonValue> runControl(ControlOpContext& context, const ControlParams& params)
{
    if (auto busy = controlCheckBusy(context))
        return *busy;
    auto& processor = context.processor();
    ControlWriteEnvelope envelope(context);
    const auto device = controlAudioDeviceState(context);
    const auto before = readPlayState(processor);
    // Where the audio callback will put the position when it carries pauseAtLastStart out; the other actions report
    // the position the processor presents right after the call (nullopt: report the read at `after` below).
    std::optional<double> positionAfter;
    bool warnsNoDevice = false;

    // The processor's transport functions and recordControlCall(), as the editor's listener functions call them.
    using Call = OpenTuneAudioProcessor::DiagnosticControlCall;
    switch (params.action) {
        case Action::Play:
            // playRequested(). The transport bar's play button is the pause button while playing, so a play during
            // playback is not issued again (it would only move the play start position).
            if (!before.playing)
                processor.play();
            processor.recordControlCall(Call::Play);
            warnsNoDevice = true;
            break;
        case Action::Pause:
            // pauseRequested().
            processor.pause();
            processor.recordControlCall(Call::Pause);
            break;
        case Action::Stop:
            // stopRequested(): OpenTuneAudioProcessor::stop() freezes the presented position and asks the audio
            // callback to fade the running transport out and land it on 0 (processBlock's Stop case; stop() sets
            // pendingCompletionTime_ to 0). The landing happens on the audio thread when that fade-out completes
            // (kTransportRampDurationSeconds, 0.2 s on the audio thread), so a read right after the call still
            // returns the frozen presented position. The result reports that read, exactly as transport.get does
            // now; after the fade a read returns 0. There is no earlier value to wait for on this thread.
            processor.stop();
            processor.recordControlCall(Call::Stop);
            break;
        case Action::Seek:
            // playheadPositionChangeRequested(): also the play start position of the next play-from-start.
            processor.setPosition(params.positionTimelineSec);
            processor.recordControlCall(Call::Seek);
            break;
        case Action::PlayFromLastStart:
            // playFromStartToggleRequested(), the branch of a stopped transport. While playing, the button pauses
            // instead; the schema defines this action as "move to the last start and play", so a running transport is
            // moved and plays on.
            processor.setPosition(before.playStartSec);
            if (!before.playing)
                processor.play();
            processor.recordControlCall(Call::Play);
            warnsNoDevice = true;
            break;
        case Action::PauseAtLastStart:
            // playFromStartToggleRequested(), the branch of a running transport. A stopped transport has nothing to
            // pause and stays where it is: a no-op.
            if (before.playing) {
                processor.pauseAtPosition(before.playStartSec);
                if (device.running)
                    positionAfter = before.playStartSec;
            }
            processor.recordControlCall(Call::Pause);
            break;
    }
    if (warnsNoDevice && !device.running)
        envelope.addWarning("NO_AUDIO_DEVICE");

    if (auto* bridge = context.bridge())
        bridge->onTransportChanged();

    const auto after = readPlayState(processor);
    // play and pause keep the position; what the clock moved between the two reads is not a change.
    const bool keepsPosition = params.action == Action::Play || params.action == Action::Pause;
    const double afterPosition = positionAfter ? *positionAfter : after.positionSec;
    auto fields = JsonValue::object();
    if (before.playing != after.playing)
        fields.appendMember(
            "playing", fieldChange(JsonValue::boolean(before.playing), JsonValue::boolean(after.playing), nullptr));
    if (!keepsPosition && std::abs(afterPosition - before.positionSec) > kSamePositionSec)
        fields.appendMember("positionTimelineSec", fieldChange(controlJsonSeconds(before.positionSec),
                                                               controlJsonSeconds(afterPosition), "sec"));
    if (std::abs(after.playStartSec - before.playStartSec) > kSamePositionSec)
        fields.appendMember("playStartTimelineSec", fieldChange(controlJsonSeconds(before.playStartSec),
                                                                controlJsonSeconds(after.playStartSec), "sec"));
    if (fields.size() != 0) {
        envelope.addChange("transport", singletonRef("transport"), JsonValue::string("Transport"), std::move(fields));
        // View state (PROTOCOL.md section 15.2 rule 6): not saved with the project, no undo, nothing to revert.
        envelope.setNotUndoable("not_persisted_state");
    }
    auto result = envelope.committed(context);
    // The transport member reports the same position the change above reports: the read the call made, not a second
    // read that the audio thread may already have moved on from (the stop landing).
    result.appendMember("transport", controlTransportJson(context, afterPosition));
    return result;
}

// ---- transport.setTempo ---------------------------------------------------------------------------

struct TempoParams
{
    std::optional<double> bpm;
    std::optional<int> numerator;
    std::optional<int> denominator;
};

bool parseTempo(const JsonValue& params, TempoParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (!reader.onlyMembers({"bpm", "timeSignature"})) {
        why = reader.why();
        return false;
    }
    if (params.size() == 0) {
        why = "at least one of bpm and timeSignature is required";
        return false;
    }
    if (!reader.optionalNumber("bpm", kMinBpm, kMaxBpm, out.bpm)) {
        why = reader.why();
        return false;
    }
    if (const JsonValue* signature = reader.member("timeSignature")) {
        if (!signature->isObject()) {
            why = "timeSignature must be an object";
            return false;
        }
        ControlParamReader signatureReader(*signature);
        std::optional<std::int64_t> numerator;
        std::optional<std::int64_t> denominator;
        if (!signatureReader.onlyMembers({"numerator", "denominator"}) || signatureReader.member("numerator") == nullptr
            || signatureReader.member("denominator") == nullptr
            || !signatureReader.optionalInteger("numerator", 1, kMaxTimeSignatureNumerator, numerator)
            || !signatureReader.optionalInteger("denominator", 1, 32, denominator)) {
            why = signatureReader.why().empty() ? "timeSignature needs a numerator and a denominator"
                                                : signatureReader.why();
            return false;
        }
        // A power of two from 1 to 32: OpenTune's setter would ignore any other denominator without saying so.
        if ((*denominator & (*denominator - 1)) != 0) {
            why = "timeSignature.denominator must be a power of two";
            return false;
        }
        out.numerator = static_cast<int>(*numerator);
        out.denominator = static_cast<int>(*denominator);
    }
    return true;
}

JsonValue tempoJson(double bpm, int numerator, int denominator)
{
    auto tempo = JsonValue::object();
    tempo.appendMember("bpm", controlJsonNumber(bpm, 3));
    tempo.appendMember("timeSignature", signatureJson(numerator, denominator));
    return tempo;
}

/** True when transport.setTempo's schema accepts the time signature: a numerator from 1 to 32 and a denominator
    that is a power of two from 1 to 32 (parseTempo() checks the same). */
bool signatureCanBeSet(int numerator, int denominator)
{
    return numerator >= 1 && numerator <= kMaxTimeSignatureNumerator && denominator >= 1 && denominator <= 32
           && (denominator & (denominator - 1)) == 0;
}

Result<JsonValue> runSetTempo(ControlOpContext& context, const TempoParams& params)
{
    if (auto busy = controlCheckBusy(context))
        return *busy;
    auto& processor = context.processor();
    ControlWriteEnvelope envelope(context);
    const double bpmBefore = processor.getBpm();
    const int numeratorBefore = processor.getTimeSigNumerator();
    const int denominatorBefore = processor.getTimeSigDenominator();
    // The values equal to the current ones are not set again: the editor's fields set the processor only on an
    // edit, and the call is a no-op.
    const bool bpmChanges = params.bpm && controlBpmDiffers(*params.bpm, bpmBefore);
    const bool signatureChanges =
        params.numerator && (*params.numerator != numeratorBefore || *params.denominator != denominatorBefore);
    if (bpmChanges || signatureChanges) {
        // OpenTuneAudioProcessor::setBpm() and setTimeSignature(), the canonical setters bpmChanged() and
        // timeSignatureChanged() call; then the project is marked as changed, as they do.
        if (bpmChanges)
            processor.setBpm(*params.bpm);
        if (signatureChanges)
            processor.setTimeSignature(*params.numerator, *params.denominator);
        controlMarkDirty(context);
        if (auto* bridge = context.bridge())
            bridge->onTempoChanged();
    }
    const double bpmAfter = processor.getBpm();
    const int numeratorAfter = processor.getTimeSigNumerator();
    const int denominatorAfter = processor.getTimeSigDenominator();

    // One judgement of "the bpm changed" for the reported field and for the revert recipe, so that a field reported
    // as changed always has its value in revert.params (and the recipe is never an empty object).
    const bool bpmChanged = controlBpmDiffers(bpmAfter, bpmBefore);
    auto fields = JsonValue::object();
    if (bpmChanged)
        fields.appendMember("bpm", tempoBpmField(bpmBefore, bpmAfter));
    if (numeratorAfter != numeratorBefore || denominatorAfter != denominatorBefore)
        fields.appendMember("timeSignature", fieldChange(signatureJson(numeratorBefore, denominatorBefore),
                                                         signatureJson(numeratorAfter, denominatorAfter), nullptr));
    if (fields.size() != 0) {
        addTempoChange(envelope, std::move(fields));
        // No undo entry, as in the UI (ui_parity); the previous tempo and time signature restore the state. The
        // recipe carries what the call changed: a time signature only when it changed, and a time signature that
        // this method's schema cannot express (a denominator of 64, or a numerator above 32, which the project
        // may hold from a file or from OpenTune's own fields) leaves no recipe that could restore it.
        const bool signatureChanged = numeratorAfter != numeratorBefore || denominatorAfter != denominatorBefore;
        if (signatureChanged && !signatureCanBeSet(numeratorBefore, denominatorBefore)) {
            auto data = JsonValue::object();
            data.appendMember("reason", JsonValue::string("previous_time_signature_not_settable"));
            data.appendMember("timeSignature", signatureJson(numeratorBefore, denominatorBefore));
            envelope.addWarning("IRREVERSIBLE", JsonValue(), std::move(data));
            envelope.setNotUndoable("ui_parity");
        } else {
            auto revert = JsonValue::object();
            if (bpmChanged)
                revert.appendMember("bpm", controlJsonNumber(bpmBefore, 3));
            if (signatureChanged)
                revert.appendMember("timeSignature", signatureJson(numeratorBefore, denominatorBefore));
            envelope.setNotUndoable("ui_parity", controlRevertRecipe("transport.setTempo", std::move(revert)));
        }
    }
    auto result = envelope.committed(context);
    result.appendMember("tempo", tempoJson(bpmAfter, numeratorAfter, denominatorAfter));
    return result;
}

// ---- transport.setLoop ----------------------------------------------------------------------------

struct LoopParams
{
    bool enabled = false;
};

bool parseSetLoop(const JsonValue& params, LoopParams& out, std::string& why)
{
    ControlParamReader reader(params);
    const JsonValue* enabled = reader.member("enabled");
    if (!reader.onlyMembers({"enabled"}) || enabled == nullptr || !enabled->isBoolean()) {
        why = reader.why().empty() ? "enabled is required and must be a boolean" : reader.why();
        return false;
    }
    out.enabled = enabled->asBoolean();
    return true;
}

Result<JsonValue> runSetLoop(ControlOpContext& context, const LoopParams& params)
{
    if (auto busy = controlCheckBusy(context))
        return *busy;
    auto& processor = context.processor();
    ControlWriteEnvelope envelope(context);
    const bool before = processor.isLoopEnabled();
    if (before != params.enabled) {
        // OpenTuneAudioProcessor::setLoopEnabled(), the function loopToggled() calls. The flag is not saved with the
        // project, which the editor does not mark as changed either.
        processor.setLoopEnabled(params.enabled);
        if (auto* bridge = context.bridge())
            bridge->onTransportChanged();
        auto fields = JsonValue::object();
        fields.appendMember("loopEnabled",
                            fieldChange(JsonValue::boolean(before), JsonValue::boolean(params.enabled), nullptr));
        envelope.addChange("transport", singletonRef("transport"), JsonValue::string("Transport"), std::move(fields));
        auto revert = JsonValue::object();
        revert.appendMember("enabled", JsonValue::boolean(before));
        envelope.setNotUndoable("not_persisted_state", controlRevertRecipe("transport.setLoop", std::move(revert)));
        if (params.enabled)
            envelope.addWarning("LOOP_NO_EFFECT_STANDALONE");
        envelope.addWarning("NOT_PERSISTED", singletonRef("transport"));
    }
    auto result = envelope.committed(context);
    result.appendMember("transport", controlTransportJson(context));
    return result;
}

} // namespace

bool controlBpmDiffers(double a, double b)
{
    return std::abs(a - b) > 1e-9;
}

void controlReportTempoRounded(ControlOpContext& context, ControlWriteEnvelope& envelope, double fromBpm, double toBpm)
{
    auto fields = JsonValue::object();
    fields.appendMember("bpm", tempoBpmField(fromBpm, toBpm));
    addTempoChange(envelope, std::move(fields));

    auto effect = JsonValue::object();
    effect.appendMember("fromBpm", controlJsonNumber(fromBpm, 3));
    effect.appendMember("toBpm", controlJsonNumber(toBpm, 3));
    // transport.setTempo back to the tempo from before; the instance that does not report transport.v1 has no such
    // method, so there is nothing that could restore it (PROTOCOL.md section 15.2 rule 2).
    JsonValue revert;
    if (controlReportsCapability(context, "transport.v1")) {
        auto params = JsonValue::object();
        params.appendMember("bpm", controlJsonNumber(fromBpm, 3));
        revert = controlRevertRecipe("transport.setTempo", std::move(params));
    }
    effect.appendMember("revert", std::move(revert));
    envelope.addSideEffect("tempoRounded", std::move(effect));
}

Error controlViewSwitchError(ControlOpContext& context, double bpmBefore, std::string_view kind,
                             std::string_view reason, std::string detail)
{
    const double bpmNow = context.processor().getBpm();
    if (!controlBpmDiffers(bpmBefore, bpmNow))
        return controlOpError(kind, reason, std::move(detail));
    // The tempo as the results report it (three decimals), so that the text agrees with the tempoRounded side effect of
    // a call that went through.
    detail += "; the tempo changed from " + toJsonString(controlJsonNumber(bpmBefore, 3)) + " to "
              + toJsonString(controlJsonNumber(bpmNow, 3)) + " bpm";
    return controlOpErrorAfterChange(kind, reason, std::move(detail));
}

void registerTransportOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<ControlParams>("transport.control", parseControl, runControl));
    table.add(controlWriteMethod<TempoParams>("transport.setTempo", parseTempo, runSetTempo));
    table.add(controlWriteMethod<LoopParams>("transport.setLoop", parseSetLoop, runSetLoop));
}

} // namespace OpenTune
