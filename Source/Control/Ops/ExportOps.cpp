// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExportOps.h"

#include "OpsJson.h"
#include "OpsSupport.h"
#include "SessionOps.h"
#include "WriteEnvelope.h"

#include "Control/ControlJobs.h"
#include "Control/ControlPathPolicy.h"
#include "PluginProcessor.h"
#include "Standalone/StandaloneMediaOperations.h"
#include "Utils/AppLogger.h"
#include "Utils/AppPreferences.h"

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

constexpr std::size_t kMaxPathCodePoints = 4096;
/** The extension an export writes, compared without regard to case. */
constexpr std::string_view kWavExtension = ".wav";

using ExportKind = StandaloneMediaOperations::ExportKind;
using ExportTarget = StandaloneMediaOperations::ExportTarget;
using ExportFailure = StandaloneMediaOperations::ExportFailure;
using ExportOperation = StandaloneMediaOperations::ExportOperation;
using ExportOperationProgress = StandaloneMediaOperations::ExportOperationProgress;
using ExportOperationState = StandaloneMediaOperations::ExportOperationState;

enum class IfExists {
    Error,
    Rename,
    Overwrite
};

const char* kindName(ExportKind kind)
{
    switch (kind) {
        case ExportKind::Clip:
            return "clip";
        case ExportKind::Track:
            return "track";
        case ExportKind::Master:
            return "master";
    }
    return "master";
}

const char* ifExistsName(IfExists ifExists)
{
    switch (ifExists) {
        case IfExists::Error:
            return "error";
        case IfExists::Rename:
            return "rename";
        case IfExists::Overwrite:
            return "overwrite";
    }
    return "error";
}

juce::File fileOf(const std::string& utf8Path)
{
    return juce::File(juce::String::fromUTF8(utf8Path.c_str()));
}

StandaloneMediaOperations::ExportDestinationPolicy destinationPolicy(IfExists ifExists)
{
    switch (ifExists) {
        case IfExists::Error:
            return StandaloneMediaOperations::ExportDestinationPolicy::FailIfExists;
        case IfExists::Rename:
            return StandaloneMediaOperations::ExportDestinationPolicy::Rename;
        case IfExists::Overwrite:
            return StandaloneMediaOperations::ExportDestinationPolicy::Overwrite;
    }
    return StandaloneMediaOperations::ExportDestinationPolicy::FailIfExists;
}

// ---- Params ---------------------------------------------------------------------------------------

struct ExportParams
{
    ExportKind kind = ExportKind::Master;
    std::optional<ControlHandle> placement;
    /** The placementId as the request wrote it, for the digest. */
    std::string placementText;
    std::optional<int> trackId;
    std::string path;
    IfExists ifExists = IfExists::Error;
    bool allowDry = false;

    /** The params after defaults, as one string: equal strings mean equal normalized params. */
    std::string digest() const
    {
        auto json = JsonValue::object();
        json.appendMember("kind", JsonValue::string(kindName(kind)));
        json.appendMember("placementId", placement ? JsonValue::string(placementText) : JsonValue());
        json.appendMember("trackId", trackId ? JsonValue::integer(*trackId) : JsonValue());
        json.appendMember("path", JsonValue::string(path));
        json.appendMember("ifExists", JsonValue::string(ifExistsName(ifExists)));
        json.appendMember("allowDry", JsonValue::boolean(allowDry));
        return toJsonString(json);
    }
};

bool readKind(ControlParamReader& reader, ExportKind& out)
{
    const JsonValue* kind = reader.member("kind");
    if (kind == nullptr)
        return reader.fail("kind is required");
    for (const auto candidate : {ExportKind::Clip, ExportKind::Track, ExportKind::Master})
        if (kind->isString() && kind->text() == kindName(candidate)) {
            out = candidate;
            return true;
        }
    return reader.fail("kind must be clip, track or master");
}

/** target: absent for master, {placementId} for clip, {trackId} for track (closed, exactly one member). */
bool readTarget(ControlParamReader& reader, ExportParams& out)
{
    const JsonValue* target = reader.member("target");
    if (out.kind == ExportKind::Master)
        return target == nullptr || reader.fail("a master export has no target");
    if (target == nullptr)
        return reader.fail("target is required for a clip or track export");
    if (!target->isObject() || target->size() != 1)
        return reader.fail("target must be an object with exactly one of placementId and trackId");
    ControlParamReader inner(*target);
    if (out.kind == ExportKind::Clip) {
        std::optional<ControlHandle> handle;
        if (!inner.optionalHandle("placementId", ControlHandleKind::Placement, handle) || !handle)
            return reader.fail(inner.why().empty() ? "the target of a clip export is {placementId}" : inner.why());
        out.placement = handle;
        out.placementText = target->find("placementId")->text();
        return true;
    }
    std::optional<std::int64_t> trackId;
    if (!inner.optionalInteger("trackId", 0, std::numeric_limits<int>::max(), trackId) || !trackId)
        return reader.fail(inner.why().empty() ? "the target of a track export is {trackId}" : inner.why());
    out.trackId = static_cast<int>(*trackId);
    return true;
}

bool readPath(ControlParamReader& reader, std::string& out)
{
    const JsonValue* path = reader.member("path");
    if (path == nullptr)
        return reader.fail("path is required");
    if (!path->isString() || path->text().empty() || utf8CodePointCount(path->text()) > kMaxPathCodePoints)
        return reader.fail("path must be a string of 1 to 4096 characters");
    out = path->text();
    return true;
}

bool readIfExists(ControlParamReader& reader, IfExists& out)
{
    const JsonValue* ifExists = reader.member("ifExists");
    if (ifExists == nullptr)
        return true;
    for (const auto candidate : {IfExists::Error, IfExists::Rename, IfExists::Overwrite})
        if (ifExists->isString() && ifExists->text() == ifExistsName(candidate)) {
            out = candidate;
            return true;
        }
    return reader.fail("ifExists must be error, rename or overwrite");
}

bool readAllowDry(ControlParamReader& reader, bool& out)
{
    const JsonValue* value = reader.member("allowDry");
    if (value == nullptr)
        return true;
    if (!value->isBoolean())
        return reader.fail("allowDry must be a boolean");
    out = value->asBoolean();
    return true;
}

/** Parses export.start params on the connection thread. The path is checked on the message thread with
    method-specific validation and errors. */
bool parseExport(const JsonValue& params, ExportParams& out, std::string& why)
{
    ControlParamReader reader(params);
    const bool ok = reader.onlyMembers({"kind", "target", "path", "ifExists", "allowDry"}) && readKind(reader, out.kind)
                    && readTarget(reader, out) && readPath(reader, out.path) && readIfExists(reader, out.ifExists)
                    && readAllowDry(reader, out.allowDry);
    if (!ok) {
        why = reader.why();
        return false;
    }
    return true;
}

/** Whether the path's last segment ends in .wav, compared without regard to case. */
bool hasWavExtension(const std::string& path)
{
    if (path.size() < kWavExtension.size())
        return false;
    const auto tail = path.substr(path.size() - kWavExtension.size());
    for (std::size_t i = 0; i < tail.size(); ++i) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != kWavExtension[i])
            return false;
    }
    return true;
}

// ---- The job --------------------------------------------------------------------------------------

ControlJobError exportFailureError(ExportFailure failure, bool atStart = false)
{
    switch (failure) {
        case StandaloneMediaOperations::ExportFailure::AnalysisFailed:
            return {"ANALYSIS_FAILED", "f0_failed",
                    "the pitch analysis of a content of the export failed; allowDry exports it uncorrected", false};
        case StandaloneMediaOperations::ExportFailure::ContentUnavailable:
            return {"NOT_FOUND", "content_unavailable", "a content of the export is no longer available", false};
        case StandaloneMediaOperations::ExportFailure::RenderChunkFailed:
            return {"RENDER_FAILED", "render_chunk_failed", "a render block of a content of the export failed twice",
                    false};
        case StandaloneMediaOperations::ExportFailure::RenderUnavailable:
            return {"NOT_READY", "render_unavailable", "the render of a content of the export is unavailable", false};
        case StandaloneMediaOperations::ExportFailure::StretchFailed:
            return {"RENDER_FAILED", "stretch_failed", "the time-stretch rebuild of a content of the export failed",
                    false};
        case StandaloneMediaOperations::ExportFailure::DryStretched:
            return {"NOT_READY", "export_dry_stretched",
                    "a content of the export would be exported uncorrected, but its time grid is stretched and "
                    "OpenTune has no render to build stretched audio from",
                    false};
        case StandaloneMediaOperations::ExportFailure::Busy:
            return {"BUSY", "export_in_progress", "an export is running", false};
        case StandaloneMediaOperations::ExportFailure::TargetGone:
            return {"REVISION_CONFLICT", "changed_during_export", "the clip of the export was removed", false};
        case StandaloneMediaOperations::ExportFailure::NothingToExport:
            return {"INVALID_ARGUMENT", "nothing_to_export", "the export holds no clip with audio", false};
        case StandaloneMediaOperations::ExportFailure::Changed:
            return {"REVISION_CONFLICT", "changed_during_export",
                    "a content or clip of the export changed while the file was written", false};
        case StandaloneMediaOperations::ExportFailure::LoadedMedia:
            return {"INVALID_ARGUMENT", "target_is_loaded_media",
                    "the target is a loaded source file or a media file of the project", false};
        case StandaloneMediaOperations::ExportFailure::FileIdentity:
            return {"IO_ERROR", "read_failed",
                    "the identity of the target or of a loaded media file cannot be determined", false};
        case StandaloneMediaOperations::ExportFailure::DestinationUnavailable:
            return {"IO_ERROR", "write_failed",
                    atStart ? "the directory of the target does not exist" : "the target directory cannot be written",
                    false};
        case StandaloneMediaOperations::ExportFailure::TargetExists:
            return {"ALREADY_EXISTS", "target_exists", "a file exists at the target", false};
        case StandaloneMediaOperations::ExportFailure::WriteFailed:
            return {"IO_ERROR", "write_failed", "OpenTune could not write the export file", false};
        case StandaloneMediaOperations::ExportFailure::None:
            break;
    }
    return {"INTERNAL", "", "export failed without a reason", false};
}

/** API job progress and result encoding; the owner handles the complete export lifecycle. */
class ExportJob
{
public:
    ExportJob(ControlOpEnvironment environment, OpenTuneAudioProcessor& processor, const ExportParams& params,
              const ExportTarget& target)
        : environment_(std::move(environment))
        , requestedPath_(params.path)
        , operation_(processor, target, fileOf(params.path), destinationPolicy(params.ifExists), params.allowDry,
                     Language::English)
    {}

    ExportJob(const ExportJob&) = delete;
    ExportJob& operator=(const ExportJob&) = delete;

    ExportFailure start() { return operation_.start(); }

    void step(ControlJobStep& step)
    {
        ControlOpContext context(environment_);
        operation_.setWakeCallback(step.wakeCallback());
        const auto progress = operation_.advance(step.cancelRequested(), [&step] { return step.endCancellability(); });
        switch (progress.state) {
            case ExportOperationState::Waiting:
                setProgress(step, static_cast<double>(progress.readyContents) / progress.totalContents,
                            "waiting", progress.readyContents, progress.totalContents);
                return;
            case ExportOperationState::Writing:
                if (!resultMetadataCaptured_)
                    captureResultMetadata(context, progress);
                setProgress(step, std::nullopt, "writing", std::nullopt, std::nullopt);
                return;
            case ExportOperationState::Succeeded:
                succeed(step);
                return;
            case ExportOperationState::Failed:
                fail(step, exportFailureError(progress.failure));
                return;
            case ExportOperationState::Cancelled:
                step.cancel(JsonValue(), false);
                return;
            case ExportOperationState::NotStarted:
                fail(step, {"INTERNAL", "", "export operation was not started", false});
                return;
        }
    }

private:
    void captureResultMetadata(ControlOpContext& context, const ExportOperationProgress& progress)
    {
        std::vector<ControlContentVersions> versions;
        for (const auto& key : operation_.plan().contents) {
            const auto snapshot = controlLiveSnapshot(context, key);
            if (snapshot != nullptr)
                versions.push_back(controlContentVersions(context, controlContentId(context, key), *snapshot));
        }
        contents_ = JsonValue::array();
        for (const auto& version : versions) {
            auto entry = JsonValue::object();
            entry.appendMember("contentId", JsonValue::string(version.contentId));
            entry.appendMember("revision", JsonValue::string(version.revision()));
            contents_.append(std::move(entry));
        }
        for (const auto& dry : progress.dryContents) {
            auto entry = JsonValue::object();
            entry.appendMember("contentId", JsonValue::string(controlContentId(context, dry.key)));
            entry.appendMember("reason", JsonValue::string(dry.reason));
            dryContents_.append(std::move(entry));
        }
        processing_ = processingJson(context);
        startTimelineSec_ = operation_.plan().target.kind == ExportKind::Clip ? operation_.plan().clipStartSec : 0.0;
        resultMetadataCaptured_ = true;
    }

    void succeed(ControlJobStep& step)
    {
        const auto& info = operation_.wavInfo();
        auto outputPath = operation_.destination().getFullPathName().toStdString();
        if (operation_.destination() == fileOf(requestedPath_))
            outputPath = requestedPath_;
        auto result = JsonValue::object();
        result.appendMember("path", JsonValue::string(outputPath));
        result.appendMember("sampleRateHz", controlJsonNumber(info.sampleRateHz, 3));
        result.appendMember("channelCount", JsonValue::integer(info.channelCount));
        result.appendMember("sampleFormat", JsonValue::string(info.sampleFormat));
        result.appendMember("durationSec", controlJsonSeconds(static_cast<double>(info.frameCount) / info.sampleRateHz));
        result.appendMember("startTimelineSec", controlJsonSeconds(startTimelineSec_));
        result.appendMember("contents", contents_);
        result.appendMember("dryContents", dryContents_);
        result.appendMember("processing", processing_);
        step.succeed(std::move(result));
    }

    static JsonValue processingJson(ControlOpContext& context)
    {
        // The processing preferences and the inference backend that the render was made with, in the form
        // session.info reports them (controlProcessingJson).
        const auto preferences = context.processor().getAppPreferences().getState().shared;
        const auto runtime = controlProcessingRuntime();
        const auto full = controlProcessingJson(preferences, &runtime);
        auto json = JsonValue::object();
        for (const char* name : {"hybridMode", "backend", "tuningHz"})
            if (const JsonValue* value = full.find(name))
                json.appendMember(name, *value);
        return json;
    }

    void setProgress(ControlJobStep& step, std::optional<double> fraction, const char* phase,
                     std::optional<int> itemsDone, std::optional<int> itemsTotal)
    {
        if (lastPhase_ == phase && fraction == lastFraction_)
            return;
        lastPhase_ = phase;
        lastFraction_ = fraction;
        step.setProgress(fraction, phase, itemsDone, itemsTotal);
    }

    void fail(ControlJobStep& step, const ControlJobError& error)
    {
        if (error.kind != "INVALID_ARGUMENT" || !error.reason.empty())
            AppLogger::logNoThrow("control API: export.start: the export failed with " + juce::String(error.kind) + "("
                                  + juce::String(error.reason) + ")");
        step.fail(error);
    }

    ControlOpEnvironment environment_;
    std::string requestedPath_;
    ExportOperation operation_;
    JsonValue contents_ = JsonValue::array();
    JsonValue dryContents_ = JsonValue::array();
    JsonValue processing_;
    double startTimelineSec_ = 0.0;
    bool resultMetadataCaptured_ = false;
    std::string lastPhase_;
    std::optional<double> lastFraction_;
};

// ---- export.start ---------------------------------------------------------------------------------

Result<JsonValue> runExportStart(ControlOpContext& context, const ExportParams& params)
{
    auto* jobs = context.jobs();
    if (jobs == nullptr)
        return controlOpError("INTERNAL", "", "the jobs registry is not available");
    const auto digest = params.digest();

    // The same normalized params as an export job that still runs return that job,
    // before any check of state (that job holds the export admission).
    if (const auto existing = jobs->findActiveByDigest("export", digest); !existing.empty()) {
        ControlWriteEnvelope envelope(context);
        return envelope.handedToJob(context, jobs->jobRef(existing));
    }

    // Handles, then busy state (including an active export), then the method's own checks.
    auto& processor = context.processor();
    const auto* arrangement = processor.getStandaloneArrangement();
    if (arrangement == nullptr)
        return controlOpError("INTERNAL", "", "the arrangement is not available");
    ExportTarget target;
    target.kind = params.kind;
    if (params.kind == ExportKind::Clip) {
        const auto placement = controlResolvePlacement(context, *params.placement);
        if (!placement.ok())
            return placement.error();
        target.trackId = placement.value().trackId;
        target.placementId = placement.value().placement.placementId;
    } else if (params.kind == ExportKind::Track) {
        if (*params.trackId >= arrangement->getNumTracks())
            return controlOpError("NOT_FOUND", "unknown_handle", "trackId names no track slot");
        target.trackId = *params.trackId;
    }
    if (auto busy = controlCheckBusy(context))
        return *busy;
    // Check the raw path text before any file-system access.
    const auto reason = controlCheckAbsolutePath(params.path, controlNativePathStyle());
    if (!reason.empty())
        return controlOpError("INVALID_ARGUMENT", reason, "the path is not an absolute path in the accepted form");
    if (!hasWavExtension(params.path))
        return controlOpError("INVALID_ARGUMENT", "path_bad_extension", "an export writes only .wav files");
    auto job = std::make_shared<ExportJob>(context.environment(), processor, params, target);
    const auto startFailure = job->start();
    if (startFailure != ExportFailure::None) {
        const auto error = exportFailureError(startFailure, true);
        return controlOpError(error.kind, error.reason, error.detail);
    }
    const auto started =
        jobs->start("export", true, [job](ControlJobStep& step) { job->step(step); }, std::string(), digest);
    if (started.jobId.empty())
        return started.refusal == "job_limit"
                   ? controlOpError("BUSY", "job_limit", "every retained job is still active")
                   : controlOpError("SHUTTING_DOWN", "", "the control API is stopping");
    ControlWriteEnvelope envelope(context);
    return envelope.handedToJob(context, jobs->jobRef(started.jobId));
}

} // namespace

void registerExportOps(ControlMethodTable& table)
{
    table.add(controlWriteMethod<ExportParams>("export.start", parseExport, runExportStart));
}

} // namespace OpenTune
