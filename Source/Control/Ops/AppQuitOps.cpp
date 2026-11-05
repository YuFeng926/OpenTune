// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppQuitOps.h"

#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Control/ControlJobs.h"
#include "Control/ControlService.h"
#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"
#include "Utils/AppLogger.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

/** The longest path the result carries (common.schema.json#/$defs/path). */
constexpr std::size_t kMaxPathCodePoints = 4096;

/**
 * The longest time, in milliseconds, in which the process ends after app.quit wrote its result (exitDeadlineMs of the
 * result). The steps behind it are the stop of the control service (open connections get at most 500 ms to flush and
 * the joins of their threads at most 2 s, ControlServer::stop()), the window's save of the plug-in state and the
 * normal shutdown of the application (the processor, the audio device, the model sessions). Measured on the Standalone
 * build with the control API on, from the request to the end of the process (the reply came within 30 ms): 0.50 to 0.58
 * s in six runs, with an empty project, with a dirty one and with an analysed clip while the models were loaded (the
 * connection itself closed after about 0.25 s). The value leaves about eighteen times that for a slow disk or a busy
 * machine, and stays below the 15 s that the specification's runners wait (vectors/README.md) and the 60 s that the
 * result schema allows.
 */
constexpr int kExitDeadlineMs = 10000;

/** What the job kinds of this instance mean for BUSY (PROTOCOL.md section 13.2): import, export, projectOpen and
    projectSave have a reason of their own, and so has projectCopy (project.saveCopy runs under the same
   project-operation flag as a save), every other kind (prefsApply, timeGridSeed, ...) is job_in_progress. */
const char* busyReasonOfJobKind(std::string_view kind)
{
    if (kind == "import")
        return "import_in_progress";
    if (kind == "export")
        return "export_in_progress";
    if (kind == "projectOpen" || kind == "projectSave" || kind == "projectCopy")
        return "project_operation";
    return "job_in_progress";
}

/** The reason of BUSY for the jobs that have not finished, or nullptr when there is none. With several jobs the reasons
    of section 13.2 come first, in the order that section lists them (import_in_progress, export_in_progress,
    project_operation), and job_in_progress last, whichever job started first: the answer does not depend on the
    order in which the jobs happened to start. */
const char* busyReasonOfJobs(const std::vector<std::string>& kinds)
{
    constexpr const char* kOrder[] = {"import_in_progress", "export_in_progress", "project_operation",
                                      "job_in_progress"};
    for (const char* reason : kOrder)
        for (const auto& kind : kinds)
            if (std::string_view(busyReasonOfJobKind(kind)) == reason)
                return reason;
    return nullptr;
}

struct QuitParams
{
    /** The end user agreed to lose the unsaved changes of the project. */
    bool discardUnsaved = false;
};

/** methods/app.quit.schema.json $defs.params, on the connection thread: the one optional member discardUnsaved. */
bool parseQuit(const JsonValue& params, QuitParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (!reader.onlyMembers({"discardUnsaved"})) {
        why = reader.why();
        return false;
    }
    if (const JsonValue* discard = reader.member("discardUnsaved")) {
        if (!discard->isBoolean()) {
            reader.fail("discardUnsaved must be a boolean");
            why = reader.why();
            return false;
        }
        out.discardUnsaved = discard->asBoolean();
    }
    return true;
}

/** The warnings member: IRREVERSIBLE with the project as its subject when changes are discarded, else empty. */
JsonValue quitWarnings(bool discarded)
{
    auto warnings = JsonValue::array();
    if (!discarded)
        return warnings;
    auto subject = JsonValue::object();
    subject.appendMember("singleton", JsonValue::string("project"));
    auto warning = JsonValue::object();
    warning.appendMember("code", JsonValue::string("IRREVERSIBLE"));
    warning.appendMember("subject", std::move(subject));
    warning.appendMember("data", JsonValue());
    warnings.append(std::move(warning));
    return warnings;
}

/**
 * The step that ends the instance, run on the message thread after the result was written (ControlDispatcher): stops
 * the control service, which closes the connections, the listener and the discovery file, then closes the window the
 * way its close button does.
 *
 * The service is always stopped first, also when the window turns out not to close: the client was told `quitting:
 * true` and the dispatcher has answered SHUTTING_DOWN since the call returned, so a service that stayed up would only
 * keep a listener and a discovery file that point at an instance that no longer answers. Checking the bridge before
 * the stop would not help either: requestQuit() is what finds out whether the window closes, and it must come after
 * the stop (the interface is down before the window closes).
 *
 * When the editor is gone, or its window does not close, the application keeps running with the control API stopped.
 * The result cannot be taken back, so the step makes that state explainable instead: the log names it, and says that
 * the client was told `quitting: true`.
 */
void endInstance(const ControlOpEnvironment& environment)
{
    ControlOpContext context(environment);
    // ControlService::stop(): idempotent, so the normal shutdown that follows finds it done.
    context.processor().getControlService().stop();
    auto* bridge = context.bridge();
    const char* notClosed = nullptr;
    if (bridge == nullptr)
        notClosed = "the editor is gone";
    else if (!bridge->requestQuit())
        notClosed = "the editor did not close it";
    if (notClosed != nullptr)
        AppLogger::logNoThrow(
            juce::String("control API: app.quit: the client was told quitting: true, but the OpenTune "
                         "window was not closed (")
            + notClosed + "); the control API is stopped and OpenTune keeps running");
}

Result<JsonValue> runQuit(ControlOpContext& context, const QuitParams& params)
{
    auto* bridge = context.bridge();
    if (bridge == nullptr)
        return controlOpError("UNSUPPORTED", "no_editor_window",
                              "app.quit closes the OpenTune editor window, and no editor exists");
    if (!bridge->canQuit())
        return controlOpError("UNSUPPORTED", "capability_missing", "the editor does not close its window");

    // 2. BUSY: the checks of every write, then the jobs that are not finished.
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (auto* jobs = context.jobs())
        if (const char* reason = busyReasonOfJobs(jobs->activeKinds()))
            return controlOpError("BUSY", reason, "a job of the control API has not finished");

    // 3. UNSAVED_CHANGES: the flag that project.open checks (ControlUiBridge::projectInfo() reads
    // ProjectSession::isDirty() of the editor).
    const auto project = bridge->projectInfo();
    if (!project)
        return controlOpError("UNSUPPORTED", "capability_missing", "the editor does not report its project");
    const bool discarded = project->dirty;
    if (discarded && !params.discardUnsaved)
        return controlOpError("UNSAVED_CHANGES", "project_dirty",
                              "the project has unsaved changes, and discardUnsaved was not given");

    auto result = JsonValue::object();
    result.appendMember("quitting", JsonValue::boolean(true));
    result.appendMember("discardedUnsavedChanges", JsonValue::boolean(discarded));
    result.appendMember("projectPath", project->projectPath.empty()
                                           ? JsonValue()
                                           : controlJsonText(project->projectPath, kMaxPathCodePoints));
    result.appendMember("exitDeadlineMs", JsonValue::integer(kExitDeadlineMs));
    result.appendMember("warnings", quitWarnings(discarded));
    if (project->projectPath.empty()) {
        auto nullReasons = JsonValue::object();
        nullReasons.appendMember("projectPath", JsonValue::string("no_project_path"));
        result.appendMember("nullReasons", std::move(nullReasons));
    }

    // From here the call cannot fail: the dispatcher enters the stopping state when this function returns, writes the
    // result and then runs the exit step.
    context.endInstanceAfterReply([environment = context.environment()] { endInstance(environment); });
    return result;
}

} // namespace

void registerAppQuitOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<QuitParams>("app.quit", parseQuit, runQuit));
}

} // namespace OpenTune
