// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectOps.h"

#include "OpsSupport.h"
#include "WriteEnvelope.h"

#include "Control/ControlFileIdentity.h"
#include "Control/ControlJobs.h"
#include "Control/ControlPathPolicy.h"
#include "Control/ControlUiBridge.h"
#include "PluginProcessor.h"
#include "Utils/AppLogger.h"
#include "Utils/AppPreferences.h"
#include "Utils/ProjectSession.h"
#include "Utils/UndoManager.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

constexpr std::size_t kMaxPathCodePoints = 4096;
/** The most entries that the created and removed lists of an open report (the rest is counted in omittedCount). */
constexpr std::size_t kMaxListedEntries = 200;
/** The extension that saving and opening projects accept (PROTOCOL.md section 20.1), compared without regard to case.
 */
constexpr std::string_view kProjectExtension = ".otproj";
/** The state that a project file does not hold (warning NOT_SAVED_IN_PROJECT, data.items). The number of visible tracks
    is not in the list: the project file stores it. */
constexpr const char* kNotSavedItems[] = {"undoHistory", "preferences"};
constexpr const char* kJobKindSave = "projectSave";
constexpr const char* kJobKindCopy = "projectCopy";
constexpr const char* kJobKindOpen = "projectOpen";

juce::File fileOf(const std::string& utf8Path)
{
    return juce::File(juce::String::fromUTF8(utf8Path.c_str()));
}

std::string utf8PathOf(const juce::File& file)
{
    return file.getFullPathName().toStdString();
}

/** Whether the path's last segment ends in .otproj, compared without regard to case. */
bool hasProjectExtension(const std::string& path)
{
    if (path.size() < kProjectExtension.size())
        return false;
    const auto tail = path.substr(path.size() - kProjectExtension.size());
    for (std::size_t i = 0; i < tail.size(); ++i) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != kProjectExtension[i])
            return false;
    }
    return true;
}

// ---- Params ---------------------------------------------------------------------------------------

/** project.saveAs and project.open take a path and one member of consent. */
struct PathParams
{
    std::string path;
    /** confirm of project.saveAs, discardUnsaved of project.open. */
    bool consent = false;
};

bool readProjectPath(ControlParamReader& reader, std::string& out)
{
    const JsonValue* path = reader.member("path");
    if (path == nullptr)
        return reader.fail("path is required");
    if (!path->isString() || path->text().empty() || utf8CodePointCount(path->text()) > kMaxPathCodePoints)
        return reader.fail("path must be a string of 1 to 4096 characters");
    out = path->text();
    return true;
}

bool readConsent(ControlParamReader& reader, const char* name, bool& out)
{
    const JsonValue* value = reader.member(name);
    if (value == nullptr)
        return true;
    if (!value->isBoolean())
        return reader.fail(std::string(name) + " must be a boolean");
    out = value->asBoolean();
    return true;
}

/** methods/project.saveAs.schema.json $defs.params, on the connection thread. */
bool parseSaveAs(const JsonValue& params, PathParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (reader.onlyMembers({"path", "confirm"}) && readProjectPath(reader, out.path)
        && readConsent(reader, "confirm", out.consent))
        return true;
    why = reader.why();
    return false;
}

/** methods/project.saveCopy.schema.json $defs.params, on the connection thread: the path, and no other member. */
bool parseSaveCopy(const JsonValue& params, PathParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (reader.onlyMembers({"path"}) && readProjectPath(reader, out.path))
        return true;
    why = reader.why();
    return false;
}

/** methods/project.open.schema.json $defs.params, on the connection thread. */
bool parseOpen(const JsonValue& params, PathParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (reader.onlyMembers({"path", "discardUnsaved"}) && readProjectPath(reader, out.path)
        && readConsent(reader, "discardUnsaved", out.consent))
        return true;
    why = reader.why();
    return false;
}

// ---- Pieces shared by the methods ----------------------------------------------------------------

/** What the editor owns and a project job needs, all reached through the bridge. */
struct ProjectParts
{
    ControlUiBridge* bridge = nullptr;
    ProjectSession* session = nullptr;
    juce::ThreadPool* pool = nullptr;
};

/** The editor's parts, or the error of a build without an editor (the project session belongs to the editor).
    capability names the capability of the calling method, for the error's detail. */
Result<ProjectParts> projectParts(ControlOpContext& context, const char* capability = "project.io")
{
    ProjectParts parts;
    parts.bridge = context.bridge();
    if (parts.bridge != nullptr) {
        parts.session = parts.bridge->projectSession();
        parts.pool = parts.bridge->projectWorkerPool();
    }
    // PROTOCOL.md section 7 rule 6: without an editor window the method fails with no_editor_window.
    if (parts.bridge == nullptr)
        return controlOpError("UNSUPPORTED", "no_editor_window",
                              std::string(capability) + " acts through the editor window, and no editor exists");
    if (parts.session == nullptr || parts.pool == nullptr)
        return controlOpError("UNSUPPORTED", "capability_missing",
                              std::string(capability)
                                  + " needs the editor's project session, and the editor offers none");
    return parts;
}

JsonValue projectRef()
{
    auto ref = JsonValue::object();
    ref.appendMember("singleton", JsonValue::string("project"));
    return ref;
}

/** The fields member of a change of the project's file: {path: {before, after, unit: null}}. */
JsonValue pathFields(const std::string& before, const std::string& after)
{
    auto pair = JsonValue::object();
    pair.appendMember("before", before.empty() ? JsonValue() : controlJsonText(before, kMaxPathCodePoints));
    pair.appendMember("after", controlJsonText(after, kMaxPathCodePoints));
    pair.appendMember("unit", JsonValue());
    auto fields = JsonValue::object();
    fields.appendMember("path", std::move(pair));
    return fields;
}

/** The warning IRREVERSIBLE about the project, which the result states first (the envelope would add it last, and
    without a subject). */
void addProjectIrreversible(ControlWriteEnvelope& envelope)
{
    envelope.addWarning("IRREVERSIBLE", projectRef());
}

ControlJobError jobError(const char* kind, const char* reason, const char* detail)
{
    return ControlJobError{kind, reason, detail, false};
}

/** Why a start of a job failed, as the error of the method (PROTOCOL.md section 12). */
Error startRefusal(const std::string& refusal)
{
    return refusal == "job_limit" ? controlOpError("BUSY", "job_limit", "every retained job is still active")
                                  : controlOpError("SHUTTING_DOWN", "", "the control API is stopping");
}

// ---- project.save and project.saveAs: the job ----------------------------------------------------

/** What the worker of a save leaves for the job. The job and the worker share it; either may outlive the other. */
struct SaveShared
{
    std::atomic<bool> done{false};
    /** The job is gone (the control API stopped): the worker deletes its temporary file itself. */
    std::atomic<bool> abandoned{false};
    /** Written before done is set. */
    bool written = false;
    juce::File temporary;
};

/** What a save job needs to know of its request. */
struct SaveSpec
{
    /** project.saveAs: the target may be another file than the project's, and the project's file becomes it. */
    bool saveAs = false;
    /** The caller confirmed to replace another project file (project.saveAs confirm). */
    bool confirm = false;
    juce::File target;
    /** The dirty generation of the snapshot: the unsaved-changes flag is cleared only if it is still current. */
    std::uint64_t generation = 0;
};

/**
 * The job of project.save and project.saveAs. It follows OpenTuneAudioProcessorEditor::saveProject: the snapshot was
 * taken on the message thread when the request came (ProjectSession::prepareSave), a worker of the editor's pool wrote
 * the project and copied its media (ProjectSession::executeSaveToFile), and the message thread then makes the project
 * file the current one, clears the unsaved-changes flag if the project did not change meanwhile, pushes the file to the
 * recent projects and lets the editor update its menu and title. The one difference is where the project is written:
 * to a temporary file in the target directory, which the job moves into place with the final rename of PROTOCOL.md
 * section 20.4 once the identity of the target was checked again.
 */
class SaveJob
{
public:
    SaveJob(ControlOpEnvironment environment, SaveSpec spec, std::shared_ptr<SaveShared> shared)
        : environment_(std::move(environment))
        , spec_(std::move(spec))
        , shared_(std::move(shared))
    {}

    ~SaveJob()
    {
        if (finished_)
            return;
        // The registry stopped before the job ended: the worker deletes what it wrote, and the editor's flag of a
        // running project operation is released.
        shared_->abandoned = true;
        if (shared_->done.load())
            shared_->temporary.deleteFile();
        endOperation();
    }

    void step(ControlJobStep& step)
    {
        if (!shared_->done.load())
            return;
        ControlOpContext context(environment_);
        auto* bridge = context.bridge();
        auto* session = bridge != nullptr ? bridge->projectSession() : nullptr;
        if (session == nullptr) {
            fail(step, jobError("INTERNAL", "", "the editor's project session is gone"));
            return;
        }
        if (!shared_->written) {
            AppLogger::logNoThrow("control API: project save: OpenTune could not write the project file");
            fail(step, jobError("IO_ERROR", "write_failed", "OpenTune could not write the project file"));
            return;
        }
        const auto target = utf8PathOf(spec_.target);

        // PROTOCOL.md section 20.2 rule 4 and section 20.3 rule 5: whether the target is the project's file, another
        // file or none is decided again on the message thread right before the final rename.
        bool sameCurrent = !spec_.saveAs;
        bool targetExists = true;
        if (spec_.saveAs) {
            ControlFileId id;
            const auto status = controlFileIdentity(target, id);
            if (status != ControlFileIdStatus::Found && status != ControlFileIdStatus::Missing) {
                fail(step, jobError("IO_ERROR", "read_failed", "the identity of the target cannot be determined"));
                return;
            }
            targetExists = status == ControlFileIdStatus::Found;
            if (session->hasProjectPath()) {
                const auto same = controlSameFile(target, utf8PathOf(session->getCurrentProjectFile()));
                if (!same) {
                    fail(step,
                         jobError("IO_ERROR", "read_failed", "the identity of the project file cannot be determined"));
                    return;
                }
                sameCurrent = *same;
            }
        }
        // Another project file is replaced only with the caller's confirmation; without it the rename never replaces.
        const bool replace = sameCurrent || spec_.confirm;
        const bool overwroteOther = spec_.saveAs && targetExists && !sameCurrent;
        const auto from = utf8PathOf(shared_->temporary);
        switch (controlRenameFile(from, target, replace)) {
            case ControlRenameResult::DoneSourceRemains:
                // The target holds the data; only the temporary name is left over. Remove it again now.
                if (!shared_->temporary.deleteFile() && shared_->temporary.exists())
                    AppLogger::logNoThrow("control API: project save: the temporary file could not be removed");
                [[fallthrough]];
            case ControlRenameResult::Done:
                break;
            case ControlRenameResult::TargetExists:
                fail(step, jobError("INVALID_ARGUMENT", "confirm_required",
                                    "a project file appeared at the target and was not replaced"));
                return;
            case ControlRenameResult::Failed:
                AppLogger::logNoThrow("control API: project save: the project file could not be moved to the target");
                fail(step, jobError("IO_ERROR", "write_failed", "the project file could not be moved to the target"));
                return;
        }

        // The envelope starts immediately before the project takes its new file and loses its flag.
        ControlWriteEnvelope envelope(context);
        const auto previousPath =
            session->hasProjectPath() ? utf8PathOf(session->getCurrentProjectFile()) : std::string();
        // The editor's steps after a successful write (saveProject): the project's file is the target, the flag is
        // cleared unless the project changed while the job ran, the file is the newest recent project, and the menu
        // and the title follow.
        session->setCurrentProjectFile(spec_.target);
        if (session->getDirtyGeneration() == spec_.generation)
            session->clearDirty();
        session->pushRecentProject(spec_.target);
        bridge->onProjectSaved();
        if (spec_.saveAs && !sameCurrent)
            envelope.addChange("project", projectRef(), JsonValue::string("Project file"),
                               pathFields(previousPath, target));
        envelope.markEffective();
        envelope.setNotUndoable("no_upstream_undo");
        addProjectIrreversible(envelope);
        if (overwroteOther) {
            auto path = JsonValue::object();
            path.appendMember("path", controlJsonText(target, kMaxPathCodePoints));
            auto data = path;
            envelope.addWarning("OVERWROTE_OTHER_PROJECT", std::move(path), std::move(data));
        }
        auto items = JsonValue::array();
        for (const char* item : kNotSavedItems)
            items.append(JsonValue::string(item));
        auto data = JsonValue::object();
        data.appendMember("items", std::move(items));
        envelope.addWarning("NOT_SAVED_IN_PROJECT", JsonValue(), std::move(data));

        auto result = envelope.committed(context);
        result.appendMember("path", controlJsonText(target, kMaxPathCodePoints));
        result.appendMember("sizeBytes", JsonValue::integer(std::max<juce::int64>(0, spec_.target.getSize())));
        finished_ = true;
        endOperation();
        step.succeed(std::move(result));
    }

private:
    void fail(ControlJobStep& step, const ControlJobError& error)
    {
        shared_->temporary.deleteFile();
        finished_ = true;
        endOperation();
        step.fail(error);
    }

    void endOperation()
    {
        if (operationEnded_)
            return;
        operationEnded_ = true;
        if (auto* bridge = environment_.bridge ? environment_.bridge() : nullptr)
            bridge->setProjectOperationRunning(false);
    }

    ControlOpEnvironment environment_;
    SaveSpec spec_;
    std::shared_ptr<SaveShared> shared_;
    bool finished_ = false;
    bool operationEnded_ = false;
};

/** Starts the save job of a request that passed its checks: the snapshot now, the write on the editor's worker. */
Result<JsonValue> startSave(ControlOpContext& context, const ProjectParts& parts, const SaveSpec& requested)
{
    auto* jobs = context.jobs();
    if (jobs == nullptr)
        return controlOpError("INTERNAL", "", "the jobs registry is not available");
    SaveSpec spec = requested;
    // ProjectSession::prepareSave(): captures the snapshot of the target on the message thread, as the editor's
    // saveProject does before it hands the write to its worker. The dirty generation belongs to this snapshot.
    auto task = std::make_shared<ProjectSession::SaveTask>(parts.session->prepareSave(spec.target));
    spec.generation = parts.session->getDirtyGeneration();
    auto shared = std::make_shared<SaveShared>();
    // The write goes to a name of its own in the target directory, so that the final rename never moves data between
    // volumes; the media directory stays next to the target (prepareSave derived it from the target).
    shared->temporary = spec.target.getParentDirectory().getNonexistentChildFile(
        "." + spec.target.getFileNameWithoutExtension() + ".save", ".tmp", false);
    task->targetFile = shared->temporary;

    auto job = std::make_shared<SaveJob>(context.environment(), spec, shared);
    const auto started = jobs->start(kJobKindSave, false, [job](ControlJobStep& step) { job->step(step); });
    if (started.jobId.empty())
        return startRefusal(started.refusal);
    parts.bridge->setProjectOperationRunning(true);
    // ProjectSession::executeSaveToFile(): the editor's own file I/O (media copy and project write), a static function
    // that touches no state of the session.
    parts.pool->addJob([shared, task] {
        shared->written = ProjectSession::executeSaveToFile(*task).ok();
        shared->done = true;
        if (shared->abandoned.load())
            shared->temporary.deleteFile();
    });
    ControlWriteEnvelope envelope(context);
    return envelope.handedToJob(context, jobs->jobRef(started.jobId));
}

// ---- project.save ------------------------------------------------------------------------------

Result<JsonValue> runSave(ControlOpContext& context, const ControlNoParams&)
{
    const auto parts = projectParts(context);
    if (!parts.ok())
        return parts.error();
    if (auto busy = controlCheckBusy(context))
        return *busy;
    if (!parts.value().session->hasProjectPath())
        return controlOpError("INVALID_ARGUMENT", "no_project_path", "the project was never saved and has no file");
    SaveSpec spec;
    spec.target = parts.value().session->getCurrentProjectFile();
    return startSave(context, parts.value(), spec);
}

// ---- project.saveAs ----------------------------------------------------------------------------

Result<JsonValue> runSaveAs(ControlOpContext& context, const PathParams& params)
{
    const auto parts = projectParts(context);
    if (!parts.ok())
        return parts.error();
    if (auto busy = controlCheckBusy(context))
        return *busy;
    // Section 20.1: the raw text, before any file-system access.
    const auto reason = controlCheckAbsolutePath(params.path, controlNativePathStyle());
    if (!reason.empty())
        return controlOpError("INVALID_ARGUMENT", reason, "the path is not an absolute path in the accepted form");
    if (!hasProjectExtension(params.path))
        return controlOpError("INVALID_ARGUMENT", "path_bad_extension", "a project file ends in .otproj");

    // Section 20.2 rule 4 and section 20.3: the target is the project's file, another existing file or no file,
    // decided by file identity; the job decides again right before the rename.
    ControlFileId id;
    const auto status = controlFileIdentity(params.path, id);
    if (status != ControlFileIdStatus::Found && status != ControlFileIdStatus::Missing)
        return controlOpError("IO_ERROR", "read_failed", "the identity of the target cannot be determined");
    const auto target = fileOf(params.path);
    if (status == ControlFileIdStatus::Found) {
        if (target.isDirectory())
            return controlOpError("IO_ERROR", "write_failed", "the target is a directory");
        bool sameCurrent = false;
        if (parts.value().session->hasProjectPath()) {
            const auto same = controlSameFile(params.path, utf8PathOf(parts.value().session->getCurrentProjectFile()));
            if (!same)
                return controlOpError("IO_ERROR", "read_failed",
                                      "the identity of the project file cannot be determined");
            sameCurrent = *same;
        }
        if (!sameCurrent && !params.consent)
            return controlOpError("INVALID_ARGUMENT", "confirm_required",
                                  "the target is another existing file, and replacing it needs confirm true");
    }
    if (!target.getParentDirectory().isDirectory())
        return controlOpError("IO_ERROR", "write_failed", "the directory of the target does not exist");

    SaveSpec spec;
    spec.saveAs = true;
    spec.confirm = params.consent;
    spec.target = target;
    return startSave(context, parts.value(), spec);
}

// ---- project.saveCopy: the job -------------------------------------------------------------------

/**
 * The job of project.saveCopy. It runs the first half of OpenTuneAudioProcessorEditor::saveProject and stops where the
 * editor takes the written file as the project's own: the snapshot was taken on the message thread when the request
 * came (ProjectSession::prepareSave), a worker of the editor's pool wrote the project and copied its media
 * (ProjectSession::executeSaveToFile) into a temporary file in the target directory, and the job moves that file into
 * place with the final rename of PROTOCOL.md section 20.4, which never replaces. It does not call
 * setCurrentProjectFile, clearDirty or pushRecentProject, and it does not ask the editor to update its menu or title:
 * the project keeps its file, its unsaved-changes flag, its recent projects and its undo history, and the result
 * reports the file and the flag so that a client can see it. The worker's state is that of SaveJob (SaveShared).
 */
class CopyJob
{
public:
    CopyJob(ControlOpEnvironment environment, juce::File target, std::shared_ptr<SaveShared> shared)
        : environment_(std::move(environment))
        , target_(std::move(target))
        , shared_(std::move(shared))
    {}

    ~CopyJob()
    {
        if (finished_)
            return;
        // The registry stopped before the job ended: the worker deletes what it wrote, and the editor's flag of a
        // running project operation is released.
        shared_->abandoned = true;
        if (shared_->done.load())
            shared_->temporary.deleteFile();
        endOperation();
    }

    void step(ControlJobStep& step)
    {
        if (!shared_->done.load())
            return;
        ControlOpContext context(environment_);
        auto* bridge = context.bridge();
        auto* session = bridge != nullptr ? bridge->projectSession() : nullptr;
        if (session == nullptr) {
            fail(step, jobError("INTERNAL", "", "the editor's project session is gone"));
            return;
        }
        if (!shared_->written) {
            AppLogger::logNoThrow("control API: project copy: OpenTune could not write the project file");
            fail(step, jobError("IO_ERROR", "write_failed",
                                "OpenTune could not copy the project's media or write the project file"));
            return;
        }
        const auto target = utf8PathOf(target_);

        // The copy never replaces a file. Whether the target names the project's own file is decided again right
        // before the rename (PROTOCOL.md section 20.3 rule 5): a project file that was deleted meanwhile would
        // otherwise be created again by the rename, which is a save and not a copy.
        if (session->hasProjectPath()) {
            const auto same = controlSameFile(target, utf8PathOf(session->getCurrentProjectFile()));
            if (!same) {
                fail(step,
                     jobError("IO_ERROR", "read_failed", "the identity of the project file cannot be determined"));
                return;
            }
            if (*same) {
                fail(step, jobError("ALREADY_EXISTS", "target_exists", "the target is the project's own file"));
                return;
            }
        }
        const auto from = utf8PathOf(shared_->temporary);
        switch (controlRenameFile(from, target, false)) {
            case ControlRenameResult::DoneSourceRemains:
                // The target holds the data; only the temporary name is left over. Remove it again now.
                if (!shared_->temporary.deleteFile() && shared_->temporary.exists())
                    AppLogger::logNoThrow("control API: project copy: the temporary file could not be removed");
                [[fallthrough]];
            case ControlRenameResult::Done:
                break;
            case ControlRenameResult::TargetExists:
                fail(step,
                     jobError("ALREADY_EXISTS", "target_exists", "a file appeared at the target and was not replaced"));
                return;
            case ControlRenameResult::Failed:
                AppLogger::logNoThrow("control API: project copy: the project file could not be moved to the target");
                fail(step, jobError("IO_ERROR", "write_failed", "the project file could not be moved to the target"));
                return;
        }

        // The result is no write envelope: the project did not change. It names the project's file and the flag as
        // they are now, which this job did not touch.
        auto result = JsonValue::object();
        result.appendMember("path", controlJsonText(target, kMaxPathCodePoints));
        result.appendMember("sizeBytes", JsonValue::integer(std::max<juce::int64>(0, target_.getSize())));
        const bool hasProjectPath = session->hasProjectPath();
        result.appendMember("projectPath",
                            hasProjectPath
                                ? controlJsonText(utf8PathOf(session->getCurrentProjectFile()), kMaxPathCodePoints)
                                : JsonValue());
        result.appendMember("projectDirty", JsonValue::boolean(session->isDirty()));
        auto items = JsonValue::array();
        for (const char* item : kNotSavedItems)
            items.append(JsonValue::string(item));
        auto data = JsonValue::object();
        data.appendMember("items", std::move(items));
        auto warning = JsonValue::object();
        warning.appendMember("code", JsonValue::string("NOT_SAVED_IN_PROJECT"));
        warning.appendMember("subject", JsonValue());
        warning.appendMember("data", std::move(data));
        auto warnings = JsonValue::array();
        warnings.append(std::move(warning));
        result.appendMember("warnings", std::move(warnings));
        if (!hasProjectPath) {
            auto nullReasons = JsonValue::object();
            nullReasons.appendMember("projectPath", JsonValue::string("no_project_path"));
            result.appendMember("nullReasons", std::move(nullReasons));
        }
        finished_ = true;
        endOperation();
        step.succeed(std::move(result));
    }

private:
    void fail(ControlJobStep& step, const ControlJobError& error)
    {
        shared_->temporary.deleteFile();
        finished_ = true;
        endOperation();
        step.fail(error);
    }

    void endOperation()
    {
        if (operationEnded_)
            return;
        operationEnded_ = true;
        if (auto* bridge = environment_.bridge ? environment_.bridge() : nullptr)
            bridge->setProjectOperationRunning(false);
    }

    ControlOpEnvironment environment_;
    juce::File target_;
    std::shared_ptr<SaveShared> shared_;
    bool finished_ = false;
    bool operationEnded_ = false;
};

/** Starts the copy job of a request that passed its checks: the snapshot now, the write on the editor's worker. */
Result<JsonValue> startCopy(ControlOpContext& context, const ProjectParts& parts, const juce::File& target)
{
    auto* jobs = context.jobs();
    if (jobs == nullptr)
        return controlOpError("INTERNAL", "", "the jobs registry is not available");
    // ProjectSession::prepareSave(): captures the snapshot of the target on the message thread, as the editor's
    // saveProject does before it hands the write to its worker. The dirty generation of the snapshot, which the
    // editor keeps to clear the flag after its save, is not needed: a copy never clears the flag.
    auto task = std::make_shared<ProjectSession::SaveTask>(parts.session->prepareSave(target));
    auto shared = std::make_shared<SaveShared>();
    // A name of its own in the target directory, so that the final rename never moves data between volumes; the media
    // directory stays next to the target (prepareSave derived it from the target).
    shared->temporary = target.getParentDirectory().getNonexistentChildFile(
        "." + target.getFileNameWithoutExtension() + ".copy", ".tmp", false);
    task->targetFile = shared->temporary;

    auto job = std::make_shared<CopyJob>(context.environment(), target, shared);
    const auto started = jobs->start(kJobKindCopy, false, [job](ControlJobStep& step) { job->step(step); });
    if (started.jobId.empty())
        return startRefusal(started.refusal);
    parts.bridge->setProjectOperationRunning(true);
    // ProjectSession::executeSaveToFile(): the editor's own file I/O (media copy and project write), a static function
    // that touches no state of the session.
    parts.pool->addJob([shared, task] {
        shared->written = ProjectSession::executeSaveToFile(*task).ok();
        shared->done = true;
        if (shared->abandoned.load())
            shared->temporary.deleteFile();
    });
    ControlWriteEnvelope envelope(context);
    return envelope.handedToJob(context, jobs->jobRef(started.jobId));
}

// ---- project.saveCopy ----------------------------------------------------------------------------

Result<JsonValue> runSaveCopy(ControlOpContext& context, const PathParams& params)
{
    const auto parts = projectParts(context, "project.copy");
    if (!parts.ok())
        return parts.error();
    if (auto busy = controlCheckBusy(context))
        return *busy;
    // Section 20.1: the raw text, before any file-system access.
    const auto reason = controlCheckAbsolutePath(params.path, controlNativePathStyle());
    if (!reason.empty())
        return controlOpError("INVALID_ARGUMENT", reason, "the path is not an absolute path in the accepted form");
    if (!hasProjectExtension(params.path))
        return controlOpError("INVALID_ARGUMENT", "path_bad_extension", "a project file ends in .otproj");

    // The copy never replaces anything: a file or directory at the target, the project's own file included, ends the
    // call, and the job's rename (which never replaces) and its second identity check decide again.
    ControlFileId id;
    const auto status = controlFileIdentity(params.path, id);
    if (status != ControlFileIdStatus::Found && status != ControlFileIdStatus::Missing)
        return controlOpError("IO_ERROR", "read_failed", "the identity of the target cannot be determined");
    if (status == ControlFileIdStatus::Found)
        return controlOpError("ALREADY_EXISTS", "target_exists", "a file exists at the target");
    if (parts.value().session->hasProjectPath()) {
        // The project's file may be missing on disk (deleted since); a path that names it is still not a copy.
        const auto same = controlSameFile(params.path, utf8PathOf(parts.value().session->getCurrentProjectFile()));
        if (!same)
            return controlOpError("IO_ERROR", "read_failed", "the identity of the project file cannot be determined");
        if (*same)
            return controlOpError("ALREADY_EXISTS", "target_exists", "the target is the project's own file");
    }
    const auto target = fileOf(params.path);
    if (!target.getParentDirectory().isDirectory())
        return controlOpError("IO_ERROR", "write_failed", "the directory of the target does not exist");
    return startCopy(context, parts.value(), target);
}

// ---- project.open: the job -----------------------------------------------------------------------

/** What the worker of an open leaves for the job. */
struct OpenShared
{
    std::atomic<bool> done{false};
    /** Written before done is set; unused when the job ended first (cancelled, or the control API stopped). */
    std::optional<Result<ProjectSession::PreparedOpen>> prepared;
};

/** One entry of the created or removed list of an open: a placement or a content. */
struct ListedEntity
{
    bool placement = false;
    std::uint64_t placementId = 0;
    std::string handle;
    std::string label;
};

/** The placements, then the contents that exist now, with their handles of the current epoch. */
std::vector<ListedEntity> listWorld(ControlOpContext& context)
{
    std::vector<ListedEntity> out;
    for (const auto& placement : controlLivePlacements(context)) {
        ListedEntity entity;
        entity.placement = true;
        entity.placementId = placement.placement.placementId;
        entity.handle = placement.placementId;
        entity.label = "Track " + std::to_string(placement.trackId + 1) + " clip";
        out.push_back(std::move(entity));
    }
    for (const auto& key : controlLiveContents(context)) {
        ListedEntity entity;
        entity.handle = controlContentId(context, key);
        entity.label = "Content";
        out.push_back(std::move(entity));
    }
    return out;
}

/** A media file the project references that could not be loaded. */
struct SkippedMedia
{
    std::string path;
    /** missing (no file at the path) or unreadable. */
    std::string reason;
};

/** What an open needs of the project file to report what it skipped, taken before the commit consumes it. */
struct OpenedFileFacts
{
    std::vector<SkippedMedia> skippedMedia;
    int placementCount = 0;
};

OpenedFileFacts factsOf(const ProjectSession::PreparedOpen& prepared)
{
    OpenedFileFacts facts;
    std::set<std::uint64_t> loaded;
    for (const auto& source : prepared.sources)
        loaded.insert(source.sourceId);
    const auto directory = prepared.projectFile.getParentDirectory();
    for (const auto& source : prepared.snapshot.sources) {
        if (loaded.count(source.sourceId) != 0)
            continue;
        // ProjectSession::prepareOpen() looks for the copy next to the project first, then for the original.
        juce::File inProject;
        if (source.relativeMediaPath.isNotEmpty())
            inProject = directory.getChildFile(source.relativeMediaPath);
        const juce::File original =
            source.originalImportPath.isNotEmpty() ? juce::File(source.originalImportPath) : juce::File();
        SkippedMedia skipped;
        skipped.reason = inProject.existsAsFile() || original.existsAsFile() ? "unreadable" : "missing";
        skipped.path = utf8PathOf(inProject != juce::File() ? inProject : original);
        facts.skippedMedia.push_back(std::move(skipped));
    }
    for (const auto& track : prepared.snapshot.tracks)
        facts.placementCount += static_cast<int>(track.placements.size());
    return facts;
}

/**
 * The job of project.open. It follows OpenTuneAudioProcessorEditor::startOpenProject: a worker of the editor's pool
 * reads the file and prepares its media (ProjectSession::prepareOpen), and the message thread then replaces the project
 * in one step (ProjectSession::commitPreparedOpen) and lets the editor refresh from it. Until the file is prepared the
 * job can be cancelled and the open project is untouched. The editor shows a dialog when an open fails; the job reports
 * the failure instead.
 */
class OpenJob
{
public:
    OpenJob(ControlOpEnvironment environment, bool discardUnsaved, std::shared_ptr<OpenShared> shared)
        : environment_(std::move(environment))
        , discardUnsaved_(discardUnsaved)
        , shared_(std::move(shared))
    {}

    ~OpenJob()
    {
        if (!finished_)
            endOperation();
    }

    void step(ControlJobStep& step)
    {
        if (step.cancelRequested()) {
            // The open project has not been touched yet; the worker's result is not used.
            finished_ = true;
            endOperation();
            step.cancel();
            return;
        }
        if (!shared_->done.load())
            return;
        ControlOpContext context(environment_);
        auto* bridge = context.bridge();
        auto* session = bridge != nullptr ? bridge->projectSession() : nullptr;
        if (session == nullptr) {
            fail(step, jobError("INTERNAL", "", "the editor's project session is gone"));
            return;
        }
        if (!shared_->prepared->ok()) {
            AppLogger::logNoThrow("control API: project open: OpenTune could not read the project file");
            fail(step, jobError("IO_ERROR", "read_failed", "OpenTune could not read the file as a project"));
            return;
        }
        // Section 12.3: cancellable until the file was read and prepared; from here the project is replaced.
        if (!step.endCancellability()) {
            finished_ = true;
            endOperation();
            step.cancel();
            return;
        }
        // Changes made while the file was being read count as unsaved changes the caller never agreed to lose.
        const bool wasDirty = session->isDirty();
        if (wasDirty && !discardUnsaved_) {
            fail(step, jobError("UNSAVED_CHANGES", "project_dirty",
                                "the project changed while the file was read, and discardUnsaved was not given"));
            return;
        }
        commit(context, step, *bridge, *session, wasDirty);
    }

private:
    void commit(ControlOpContext& context, ControlJobStep& step, ControlUiBridge& bridge, ProjectSession& session,
                bool wasDirty)
    {
        // The envelope starts immediately before the project is replaced.
        ControlWriteEnvelope envelope(context);
        auto& processor = context.processor();
        const bool hadPath = session.hasProjectPath();
        const auto previousPath = hadPath ? utf8PathOf(session.getCurrentProjectFile()) : std::string();
        const auto& undoManager = processor.getUndoManager();
        const int undoCount = undoManager.cursor();
        const int redoCount = undoManager.size() - undoManager.cursor();
        // The tracks shown now, to tell what the open adds to the view.
        const auto* shownArrangement = processor.getStandaloneArrangement();
        const int shownBefore = shownArrangement != nullptr ? shownArrangement->getVisibleTrackCount() : 0;
        // What is removed: every clip and content of the project, with the handles of the epoch that ends now.
        const auto removed = listWorld(context);

        auto prepared = std::move(*shared_->prepared).value();
        const auto facts = factsOf(prepared);
        const auto openedFile = utf8PathOf(prepared.projectFile);
        // ProjectSession::commitPreparedOpen(): the editor's own replacement of the project, in one message-thread
        // step, as startOpenProject does it. It clears the undo history, the contents (a new content epoch), the
        // arrangement and the render state, restores the project, makes the file the project's file and clears the
        // flag.
        const auto committed = session.commitPreparedOpen(std::move(prepared));
        if (!committed.ok()) {
            AppLogger::logNoThrow("control API: project open: OpenTune could not commit the project");
            fail(step, jobError("INTERNAL", "", "OpenTune could not commit the project"));
            return;
        }

        // The track count is project state: commitPreparedOpen() restored it into the arrangement (the saved count,
        // or more when clips, mix settings or the selected track need more: resolveLoadedVisibleTrackCount()), and
        // the editor then takes it from there (refreshAllUIFromProject() with applyingLoadedProjectUi_ set, so that
        // the change of the panel is not marked). Nothing shows tracks before that: revealing the tracks of the clips
        // through the track panel would write that smaller count back to the arrangement and mark the project
        // (OpenTuneAudioProcessorEditor::visibleTrackCountChanged()). The tracks that the open adds to the view are
        // the ones between the count before and the count after.
        const auto created = listWorld(context);
        std::vector<int> revealed;
        if (const auto* arrangement = processor.getStandaloneArrangement())
            for (int track = shownBefore; track < arrangement->getVisibleTrackCount(); ++track)
                revealed.push_back(track);
        bridge.onProjectReloaded();

        envelope.addChange("project", projectRef(), JsonValue::string("Project file"),
                           pathFields(previousPath, openedFile));
        reportEntities(envelope, removed, false);
        const auto omittedCreated = reportEntities(envelope, created, true);
        const auto omittedRemoved = removed.size() > kMaxListedEntries ? removed.size() - kMaxListedEntries : 0;
        envelope.limitDiffToTouchedContents();
        if (!revealed.empty()) {
            // The tracks the opened project adds to the view: no revert recipe of its own and no IRREVERSIBLE for it
            // (section 15.2 rule 6); the open as a whole carries its revert recipe or IRREVERSIBLE below.
            auto trackIds = JsonValue::array();
            for (const int track : revealed)
                trackIds.append(JsonValue::integer(track));
            auto effect = JsonValue::object();
            effect.appendMember("trackIds", std::move(trackIds));
            effect.appendMember("revert", JsonValue());
            envelope.addSideEffect("tracksRevealed", std::move(effect));
        }

        // The revert recipe opens the previous file again; it carries no consent member, so after any later change the
        // dirty check keeps it from overwriting work. Nothing restores a project that had no file or whose changes
        // were discarded.
        const bool discarded = wasDirty && discardUnsaved_;
        JsonValue revert;
        if (hadPath && !wasDirty && controlReportsCapability(context, "project.io")) {
            auto params = JsonValue::object();
            params.appendMember("path", controlJsonText(previousPath, kMaxPathCodePoints));
            revert = controlRevertRecipe("project.open", std::move(params));
        }
        const bool revertible = !revert.isNull();
        envelope.setNotUndoable("no_upstream_undo", std::move(revert));
        if (!revertible)
            addProjectIrreversible(envelope);
        auto missing = JsonValue::array();
        for (const auto& media : facts.skippedMedia)
            if (media.reason == "missing")
                missing.append(controlJsonText(media.path, kMaxPathCodePoints));
        if (missing.size() != 0) {
            auto data = JsonValue::object();
            data.appendMember("paths", std::move(missing));
            envelope.addWarning("MEDIA_MISSING", JsonValue(), std::move(data));
        }

        int placementCount = 0;
        int contentCount = 0;
        for (const auto& entity : created)
            (entity.placement ? placementCount : contentCount) += 1;
        bool derived = false;

        auto result = envelope.committed(context);
        result.appendMember("path", controlJsonText(openedFile, kMaxPathCodePoints));
        auto previous = JsonValue::object();
        previous.appendMember("path", hadPath ? controlJsonText(previousPath, kMaxPathCodePoints) : JsonValue());
        previous.appendMember("dirty", JsonValue::boolean(wasDirty));
        previous.appendMember("discarded", JsonValue::boolean(discarded));
        result.appendMember("previous", std::move(previous));
        auto restored = JsonValue::object();
        restored.appendMember("placementCount", JsonValue::integer(placementCount));
        restored.appendMember("contentCount", JsonValue::integer(contentCount));
        restored.appendMember("visibleTrackCount", JsonValue::integer(controlVisibleTrackCount(context, derived)));
        result.appendMember("restored", std::move(restored));
        auto skipped = JsonValue::object();
        auto media = JsonValue::array();
        for (const auto& entry : facts.skippedMedia) {
            auto item = JsonValue::object();
            item.appendMember("path", controlJsonText(entry.path, kMaxPathCodePoints));
            item.appendMember("reason", JsonValue::string(entry.reason));
            media.append(std::move(item));
        }
        skipped.appendMember("media", std::move(media));
        skipped.appendMember("placementCount", JsonValue::integer(std::max(0, facts.placementCount - placementCount)));
        result.appendMember("skipped", std::move(skipped));
        auto cleared = JsonValue::object();
        cleared.appendMember("undoCount", JsonValue::integer(undoCount));
        cleared.appendMember("redoCount", JsonValue::integer(redoCount));
        result.appendMember("historyCleared", std::move(cleared));
        auto omitted = JsonValue::object();
        omitted.appendMember("created", JsonValue::integer(static_cast<std::int64_t>(omittedCreated)));
        omitted.appendMember("removed", JsonValue::integer(static_cast<std::int64_t>(omittedRemoved)));
        result.appendMember("omittedCount", std::move(omitted));
        finished_ = true;
        endOperation();
        step.succeed(std::move(result));
    }

    /** Lists at most kMaxListedEntries entities in the created or removed list of the result, and touches the contents
        among them (the revisionDiff lists only those). Returns how many were left out. */
    static std::size_t reportEntities(ControlWriteEnvelope& envelope, const std::vector<ListedEntity>& entities,
                                      bool created)
    {
        const auto listed = std::min(entities.size(), kMaxListedEntries);
        for (std::size_t i = 0; i < listed; ++i) {
            const auto& entity = entities[i];
            auto ref = JsonValue::object();
            ref.appendMember(entity.placement ? "placementId" : "contentId", JsonValue::string(entity.handle));
            auto label = JsonValue::string(entity.label);
            if (created)
                envelope.addCreated(entity.placement ? "placement" : "content", std::move(ref), std::move(label));
            else
                envelope.addRemoved(entity.placement ? "placement" : "content", std::move(ref), std::move(label));
            if (!entity.placement)
                envelope.touchContent(entity.handle);
            else if (created)
                envelope.touchPlacement(entity.placementId);
        }
        return entities.size() - listed;
    }

    void fail(ControlJobStep& step, const ControlJobError& error)
    {
        finished_ = true;
        endOperation();
        step.fail(error);
    }

    void endOperation()
    {
        if (operationEnded_)
            return;
        operationEnded_ = true;
        if (auto* bridge = environment_.bridge ? environment_.bridge() : nullptr)
            bridge->setProjectOperationRunning(false);
    }

    ControlOpEnvironment environment_;
    bool discardUnsaved_;
    std::shared_ptr<OpenShared> shared_;
    bool finished_ = false;
    bool operationEnded_ = false;
};

// ---- project.open ------------------------------------------------------------------------------

Result<JsonValue> runOpen(ControlOpContext& context, const PathParams& params)
{
    const auto parts = projectParts(context);
    if (!parts.ok())
        return parts.error();
    auto* jobs = context.jobs();
    if (jobs == nullptr)
        return controlOpError("INTERNAL", "", "the jobs registry is not available");
    if (auto busy = controlCheckBusy(context))
        return *busy;
    // Section 20.1: the raw text, before any file-system access.
    const auto reason = controlCheckAbsolutePath(params.path, controlNativePathStyle());
    if (!reason.empty())
        return controlOpError("INVALID_ARGUMENT", reason, "the path is not an absolute path in the accepted form");
    if (!hasProjectExtension(params.path))
        return controlOpError("INVALID_ARGUMENT", "path_bad_extension", "a project file ends in .otproj");
    const auto file = fileOf(params.path);
    if (!file.existsAsFile())
        return controlOpError("NOT_FOUND", "file_not_found", "no file exists at the path");
    // Section 20.2 rule 3: opening never saves and never discards changes the caller did not give up.
    if (parts.value().session->isDirty() && !params.consent)
        return controlOpError("UNSAVED_CHANGES", "project_dirty",
                              "the project has unsaved changes, and discardUnsaved was not given");

    auto shared = std::make_shared<OpenShared>();
    auto job = std::make_shared<OpenJob>(context.environment(), params.consent, shared);
    const auto started = jobs->start(kJobKindOpen, true, [job](ControlJobStep& step) { job->step(step); });
    if (started.jobId.empty())
        return startRefusal(started.refusal);
    parts.value().bridge->setProjectOperationRunning(true);
    // ProjectSession::prepareOpen(): reads the project file and the media it references on the editor's worker, as
    // startOpenProject does; the session pointer stays valid while the editor, which owns the pool, exists. The
    // language of its error texts is resolved here, on the message thread, as startOpenProject resolves it.
    auto* session = parts.value().session;
    const auto openLanguage = LocalizationManager::getInstance().resolveLanguage();
    parts.value().pool->addJob([shared, session, file, openLanguage] {
        shared->prepared.emplace(session->prepareOpen(file, openLanguage));
        shared->done = true;
    });
    ControlWriteEnvelope envelope(context);
    return envelope.handedToJob(context, jobs->jobRef(started.jobId));
}

// ---- project.clearRecent -------------------------------------------------------------------------

Result<JsonValue> runClearRecent(ControlOpContext& context, const ControlNoParams&)
{
    if (auto busy = controlCheckBusy(context))
        return *busy;
    // AppPreferences::getRecentProjects() and clearRecentProjects(): the list the editor's recent-projects menu is
    // made from and the call behind its command to clear it (ProjectSession::clearRecentProjects), on the same
    // preferences, so that the list is cleared also without an editor.
    auto& preferences = context.processor().getAppPreferences();
    const auto before = preferences.getRecentProjects();
    ControlWriteEnvelope envelope(context);
    if (!before.empty()) {
        preferences.clearRecentProjects();
        if (auto* bridge = context.bridge())
            bridge->onRecentProjectsChanged();
        auto paths = JsonValue::array();
        for (const auto& path : before)
            paths.append(controlJsonText(path.toStdString(), kMaxPathCodePoints));
        auto pair = JsonValue::object();
        pair.appendMember("before", std::move(paths));
        pair.appendMember("after", JsonValue::array());
        pair.appendMember("unit", JsonValue());
        auto fields = JsonValue::object();
        fields.appendMember("recentProjects", std::move(pair));
        auto ref = JsonValue::object();
        ref.appendMember("preference", JsonValue::string("recentProjects"));
        envelope.addChange("preference", ref, JsonValue::string("Recent projects"), std::move(fields));
        envelope.setNotUndoable("no_upstream_undo");
        envelope.addWarning("IRREVERSIBLE", std::move(ref));
    }
    return envelope.committed(context);
}

} // namespace

void registerProjectOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<ControlNoParams>("project.save", controlParseNoParams, runSave));
    table.add(controlWriteMethod<PathParams>("project.saveAs", parseSaveAs, runSaveAs));
    table.add(controlWriteMethod<PathParams>("project.open", parseOpen, runOpen));
    table.add(controlWriteMethod<ControlNoParams>("project.clearRecent", controlParseNoParams, runClearRecent));
    table.add(controlWriteMethod<PathParams>("project.saveCopy", parseSaveCopy, runSaveCopy));
}

} // namespace OpenTune
