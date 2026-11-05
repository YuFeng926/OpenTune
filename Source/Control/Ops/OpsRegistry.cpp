// SPDX-License-Identifier: AGPL-3.0-or-later
#include "OpsRegistry.h"

#include "AppQuitOps.h"
#include "AppReadOps.h"
#include "ArrangementOps.h"
#include "AudioDeviceOps.h"
#include "ContentDetailsOps.h"
#include "ContentEditOps.h"
#include "ContentReadOps.h"
#include "ContentTuneOps.h"
#include "ExportOps.h"
#include "ImportOps.h"
#include "JobOps.h"
#include "PrefsOps.h"
#include "ProjectOps.h"
#include "ProjectReadOps.h"
#include "SelectionOps.h"
#include "SessionOps.h"
#include "TimingOps.h"
#include "TrackOps.h"
#include "TransportOps.h"
#include "UiOps.h"
#include "UndoOps.h"
#include "WorkspaceOps.h"

#include "Control/ContentHandles.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <mutex>

namespace OpenTune {

namespace {

/** Context of an error that changed nothing, and of one after a change (executed true). Same length. */
constexpr std::string_view kErrorContextPrefix = "otcp:";
constexpr std::string_view kErrorAfterChangePrefix = "otcp!";

std::atomic<bool>& reportIncompleteCapabilities()
{
    static std::atomic<bool> report{false};
    return report;
}

/** The capabilities controlSetWithheldCapabilitiesForTesting() leaves out, under their mutex. */
struct WithheldCapabilities
{
    std::mutex mutex;
    std::vector<std::string> names;
};

WithheldCapabilities& withheldCapabilities()
{
    static WithheldCapabilities withheld;
    return withheld;
}

/** A capability without methods of its own, which an instance reports together with the capability it extends
    (PROTOCOL.md section 22): ui.onboardingClose says that ui.dismiss can close an onboarding overlay that is already
    showing, which UiOps does wherever it can reach the editor. */
struct FeatureCapability
{
    const char* capability;
    const char* extends;
};

constexpr FeatureCapability kFeatureCapabilities[] = {
    {"ui.onboardingClose", "ui.bridge"},
};

Error makeOpError(std::string_view prefix, std::string_view kind, std::string_view reason, std::string detail)
{
    const ErrorCode code = kind == "INVALID_ARGUMENT" || kind == "NOT_FOUND" || kind == "REVISION_CONFLICT"
                               ? ErrorCode::InvalidParameter
                           : kind == "NOT_READY" ? ErrorCode::NotInitialized
                                                 : ErrorCode::UnknownError;
    std::string context(prefix);
    context.append(kind).append(":").append(reason);
    return Error(code, std::move(detail), std::move(context));
}

} // namespace

void registerControlMethods(ControlMethodTable& table)
{
    // One line per group. The order matters: session.hello and session.info list the capabilities in the order
    // of the first method registered for each (session.v1 first; a feature capability such as ui.onboardingClose
    // follows the capability it extends). A method that is not registered is answered with
    // UNSUPPORTED(capability_missing).
    registerSessionOps(table);        // session.info (ControlServer answers session.hello itself)
    registerProjectReadOps(table);    // project.get, transport.get
    registerContentReadOps(table);    // content.getNotes, content.getPitchCurve, content.segmentNotes
    registerContentDetailsOps(table); // content.getDetails
    registerAppReadOps(table);        // prefs.get, app.getLog
    registerJobOps(table);            // job.get, job.cancel
    registerImportOps(table);         // import.start
    registerExportOps(table);         // export.start
    registerUiOps(table);             // ui.getState, ui.dismiss
    registerSelectionOps(table);      // ui.setSelection
    registerWorkspaceOps(table);      // ui.setWorkspace
    registerContentTuneOps(table);    // content.setKey, content.autoTune
    registerContentEditOps(table);    // content.editNotes, content.setPitchShift
    registerUndoOps(table);           // edit.getHistory, edit.undo, edit.redo
    registerTimingOps(table);         // content.shiftTiming, content.editTimeGrid
    registerTransportOps(table);      // transport.control, transport.setTempo, transport.setLoop
    registerArrangementOps(table);    // arrangement.edit, arrangement.copy
    registerTrackOps(table);          // track.set, track.add, track.duplicate, track.delete
    registerProjectOps(table);     // project.save, project.saveAs, project.open, project.clearRecent, project.saveCopy
    registerPrefsOps(table);       // prefs.set
    registerAudioDeviceOps(table); // audio.get, audio.set
    registerAppQuitOps(table);     // app.quit
}

ControlOpContext::ControlOpContext(OpenTuneAudioProcessor& processor, ControlUiBridge* bridge,
                                   const ControlInstanceFacts& facts, ControlThreadProbe& probe, ControlJobs* jobs,
                                   std::function<ControlUiBridge*()> bridgeAccessor,
                                   std::shared_ptr<ControlHistory> history)
    : processor_(processor)
    , bridge_(bridge)
    , facts_(facts)
    , probe_(probe)
    , jobs_(jobs)
    , bridgeAccessor_(std::move(bridgeAccessor))
    , history_(std::move(history))
    , instanceShort_(controlInstanceShort(facts.instanceId))
{}

ControlOpContext::ControlOpContext(const ControlOpEnvironment& environment)
    : ControlOpContext(*environment.processor, environment.bridge ? environment.bridge() : nullptr, environment.facts,
                       *environment.probe, environment.jobs, environment.bridge, environment.history)
{}

std::shared_ptr<ControlHistory> ControlOpContext::history()
{
    requireMessageThread();
    return history_;
}

ControlJobs* ControlOpContext::jobs()
{
    requireMessageThread();
    return jobs_;
}

ControlOpEnvironment ControlOpContext::environment()
{
    requireMessageThread();
    ControlOpEnvironment environment;
    environment.processor = &processor_;
    environment.bridge = bridgeAccessor_;
    if (!environment.bridge) {
        // Without an accessor the bridge of this request stays the bridge of every later step.
        auto* bridge = bridge_;
        environment.bridge = [bridge] { return bridge; };
    }
    environment.facts = facts_;
    environment.probe = &probe_;
    environment.jobs = jobs_;
    environment.history = history_;
    return environment;
}

void ControlOpContext::endInstanceAfterReply(std::function<void()> action)
{
    requireMessageThread();
    endInstanceAction_ = std::move(action);
}

void ControlOpContext::requireMessageThread()
{
    // Always on, also in release builds: content is read and changed only on the message thread.
    if (juce::MessageManager::existsAndIsCurrentThread()) {
        ++probe_.messageThreadAccesses;
        return;
    }
    ++probe_.otherThreadAccesses;
    jassertfalse;
    throw ControlThreadViolation();
}

OpenTuneAudioProcessor& ControlOpContext::processor()
{
    requireMessageThread();
    return processor_;
}

ControlUiBridge* ControlOpContext::bridge()
{
    requireMessageThread();
    return bridge_;
}

Error controlOpError(std::string_view kind, std::string_view reason, std::string detail)
{
    return makeOpError(kErrorContextPrefix, kind, reason, std::move(detail));
}

Error controlOpErrorAfterChange(std::string_view kind, std::string_view reason, std::string detail)
{
    return makeOpError(kErrorAfterChangePrefix, kind, reason, std::move(detail));
}

ControlErrorReply controlMapError(const Error& error)
{
    ControlErrorReply reply;
    const bool unchanged = error.context.compare(0, kErrorContextPrefix.size(), kErrorContextPrefix) == 0;
    const bool afterChange = error.context.compare(0, kErrorAfterChangePrefix.size(), kErrorAfterChangePrefix) == 0;
    if (unchanged || afterChange) {
        const std::string rest = error.context.substr(kErrorContextPrefix.size());
        const auto colon = rest.find(':');
        reply.kind = rest.substr(0, colon);
        reply.reason = colon == std::string::npos ? std::string() : rest.substr(colon + 1);
        reply.detail = error.message;
        reply.executed = afterChange;
        return reply;
    }
    switch (error.code) {
        case ErrorCode::InvalidParameter:
            reply.kind = "INVALID_ARGUMENT";
            reply.reason = "invalid_params";
            break;
        case ErrorCode::NotInitialized:
            reply.kind = "NOT_READY";
            break;
        case ErrorCode::ModelNotFound:
            reply.kind = "MODEL_UNAVAILABLE";
            reply.reason = "model_missing";
            break;
        case ErrorCode::ModelLoadFailed:
        case ErrorCode::SessionCreationFailed:
            reply.kind = "MODEL_UNAVAILABLE";
            reply.reason = "backend_unavailable";
            break;
        case ErrorCode::OperationCancelled:
            reply.kind = "CANCELLED";
            break;
        default:
            reply.kind = "INTERNAL";
            break;
    }
    // The fixed text of the code: an OpenTune message may contain paths or names from the project.
    reply.detail = errorCodeMessage(error.code);
    return reply;
}

void ControlMethodTable::add(ControlMethod method)
{
    jassert(controlMethodCapability(method.name) != nullptr && find(method.name) == nullptr);
    // A method runs either on the message thread (run only) or off it with exactly one of runOffMainThread
    // (the instance facts: app.getLog) and runWithoutMessageThread (the jobs registry: job.get, job.cancel),
    // without run; writes never run off it.
    const bool withFacts = static_cast<bool>(method.runOffMainThread);
    const bool withJobs = static_cast<bool>(method.runWithoutMessageThread);
    jassert(method.mainThread ? (method.run && !withFacts && !withJobs)
                              : (withFacts != withJobs && !method.run && !method.write));
    juce::ignoreUnused(withFacts, withJobs);
    methods_.push_back(std::move(method));
}

const ControlMethod* ControlMethodTable::find(std::string_view name) const
{
    for (const auto& method : methods_)
        if (method.name == name)
            return &method;
    return nullptr;
}

std::vector<std::string> ControlMethodTable::capabilities() const
{
    std::vector<std::string> out{"session.v1"};
    const bool reportIncomplete = reportIncompleteCapabilities().load();
    for (const auto& method : methods_) {
        const char* capability = controlMethodCapability(method.name);
        if (capability != nullptr && std::find(out.begin(), out.end(), capability) == out.end()
            && (reportIncomplete || !controlCapabilityIsIncomplete(capability)))
            out.emplace_back(capability);
    }
    for (const auto& feature : kFeatureCapabilities) {
        const auto extended = std::find(out.begin(), out.end(), feature.extends);
        if (extended != out.end())
            out.insert(extended + 1, feature.capability);
    }
    auto& withheld = withheldCapabilities();
    const std::lock_guard<std::mutex> lock(withheld.mutex);
    for (const auto& name : withheld.names)
        out.erase(std::remove(out.begin(), out.end(), name), out.end());
    return out;
}

bool controlCapabilityIsIncomplete(std::string_view capability)
{
    // Every capability of which this build registers a method has all its methods registered. A capability
    // whose methods are registered in several steps names itself here until the last step.
    juce::ignoreUnused(capability);
    return false;
}

void controlSetReportIncompleteCapabilitiesForTesting(bool report)
{
    reportIncompleteCapabilities() = report;
}

void controlSetWithheldCapabilitiesForTesting(std::vector<std::string> capabilities)
{
    auto& withheld = withheldCapabilities();
    const std::lock_guard<std::mutex> lock(withheld.mutex);
    withheld.names = std::move(capabilities);
}

} // namespace OpenTune
