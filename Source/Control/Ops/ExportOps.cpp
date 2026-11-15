// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExportOps.h"

#include "OpsJson.h"
#include "OpsSupport.h"
#include "SessionOps.h"
#include "WriteEnvelope.h"

#include "Control/ControlFileIdentity.h"
#include "Control/ControlJobs.h"
#include "Control/ControlPathPolicy.h"
#include "PluginProcessor.h"
#include "SourceStore.h"
#include "Standalone/StandaloneMediaOperations.h"
#include "Utils/AppLogger.h"
#include "Utils/AppPreferences.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

constexpr std::size_t kMaxPathCodePoints = 4096;
/** The last name that ifExists rename tries is "<stem> (999).wav". */
constexpr int kLastRenameNumber = 999;
/** The extension an export writes, compared without regard to case. */
constexpr std::string_view kWavExtension = ".wav";

using ExportKind = StandaloneMediaOperations::ExportKind;
using ExportTarget = StandaloneMediaOperations::ExportTarget;

using ExportPlan = StandaloneMediaOperations::ExportPlan;

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

/** "<stem> (n).wav" next to the target, <stem> being its file name without .wav. */
std::string numberedPath(const std::string& path, int number)
{
    return path.substr(0, path.size() - kWavExtension.size()) + " (" + std::to_string(number) + ").wav";
}

// ---- Loaded media ----------------------------------------------------------------------------------

/** The source file of every source OpenTune holds (SourceStore), active or retired, since an undo step
    can bring a retired one back: loaded media that an export never writes. */
std::vector<std::string> loadedMediaPaths(ControlOpContext& context)
{
    std::vector<std::string> paths;
    const auto* sources = context.processor().getSourceStore();
    if (sources == nullptr)
        return paths;
    auto ids = sources->getAllActiveSourceIds();
    const auto retired = sources->getRetiredSourceIds();
    ids.insert(ids.end(), retired.begin(), retired.end());
    for (const auto id : ids) {
        SourceStore::SourceSnapshot source;
        if (!sources->getSnapshot(id, source))
            continue;
        auto path = source.sourceFilePath.toStdString();
        if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end())
            paths.push_back(std::move(path));
    }
    return paths;
}

/** The error when the target is loaded media or when a required file identity cannot be determined;
    nullopt otherwise. */
std::optional<ControlJobError> loadedMediaError(const std::vector<std::string>& media, const std::string& target)
{
    // The identity of the target is read once. A loaded media file whose identity cannot be read for any
    // reason but "does not exist" (including an unavailable volume) rejects the export: the same
    // file may still be reachable through the target path, and overwrite would replace it.
    ControlSameFileProbe probe(target);
    for (const auto& path : media) {
        const auto same = probe.sameAs(path);
        if (!same)
            return ControlJobError{"IO_ERROR", "read_failed",
                                   "the identity of the target or of a loaded media file cannot be determined", false};
        if (*same)
            return ControlJobError{"INVALID_ARGUMENT", "target_is_loaded_media",
                                   "the target is a loaded source file or a media file of the project", false};
    }
    return std::nullopt;
}

/** The shared message-thread export busy state of the processor, released once. The admission was
    taken on the message thread in runExportStart; the guard only releases it. */
class ExportBusyGuard
{
public:
    ExportBusyGuard() = default;
    explicit ExportBusyGuard(OpenTuneAudioProcessor& processor) : processor_(&processor) {}
    ExportBusyGuard(ExportBusyGuard&& other) noexcept : processor_(std::exchange(other.processor_, nullptr)) {}
    ExportBusyGuard& operator=(ExportBusyGuard&&) = delete;
    ExportBusyGuard(const ExportBusyGuard&) = delete;
    ExportBusyGuard& operator=(const ExportBusyGuard&) = delete;
    ~ExportBusyGuard() { release(); }

    void release() noexcept
    {
        if (processor_ != nullptr)
            processor_->finishExport();
        processor_ = nullptr;
    }

private:
    OpenTuneAudioProcessor* processor_ = nullptr;
};

// ---- The file, written on a thread of its own ------------------------------------------------------

/** Writes the shared export plan to a file on a worker thread, then reads the resulting WAV header.
 * The destructor joins the thread; OpenTune's export cannot be interrupted, so that waits for the write.
 * This is the one known exception to bounded joins during shutdown; the editor's own export thread is joined the same
 * way. The worst case is the time that the render worker
 * needs to drain the process's whole render queue plus the time to write the file, and it depends on the
 * number and length of the pending renders and on the speed of the disk.
 */
class ExportWriter
{
public:
    ExportWriter(StandaloneMediaOperations::ExportPlan plan, juce::File file, std::function<void()> wake)
        : plan_(std::move(plan))
        , file_(std::move(file))
        , wake_(std::move(wake))
    {
        thread_ = std::thread([this] {
            run();
            done_ = true;
            wake_();
        });
    }

    ~ExportWriter()
    {
        if (thread_.joinable())
            thread_.join();
    }

    ExportWriter(const ExportWriter&) = delete;
    ExportWriter& operator=(const ExportWriter&) = delete;

    bool done() const noexcept { return done_.load(); }
    const StandaloneMediaOperations::ExportPlan& plan() const noexcept { return plan_; }

    // Valid once done() is true.
    bool written = false;
    int channelCount = 0;
    double sampleRateHz = 0.0;
    std::int64_t frameCount = 0;
    std::string sampleFormat;

private:
    void run() noexcept
    {
        try {
            juce::String error;
            written = StandaloneMediaOperations::writeExportWav(plan_, file_, kErrorTextLanguage, error);
            if (written)
                readBack();
        } catch (...) {
            written = false;
        }
    }

    void readBack()
    {
        written = false;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file_));
        if (reader == nullptr)
            return;
        channelCount = static_cast<int>(reader->numChannels);
        sampleRateHz = reader->sampleRate;
        frameCount = reader->lengthInSamples;
        sampleFormat = (reader->usesFloatingPointData ? "float" : "int") + std::to_string(reader->bitsPerSample);
        written = channelCount > 0 && sampleRateHz > 0.0 && frameCount >= 0;
    }

    // The owner uses this language only for its write error text; API failures are encoded separately.
    static constexpr Language kErrorTextLanguage = Language::English;

    StandaloneMediaOperations::ExportPlan plan_;
    juce::File file_;
    std::function<void()> wake_;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

// ---- The job --------------------------------------------------------------------------------------

using Readiness = StandaloneMediaOperations::ExportReadiness;

ControlJobError readinessError(const Readiness& readiness)
{
    switch (readiness.failure) {
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
        case StandaloneMediaOperations::ExportFailure::None:
            break;
    }
    return {"INTERNAL", "", "export readiness failed without a reason", false};
}

/** API job coordination, file policy and result encoding; media selection, readiness and WAV output live in the owner. */
class ExportJob
{
public:
    ExportJob(ControlOpEnvironment environment, const ExportParams& params, const ExportTarget& target,
              ExportBusyGuard busyAdmission)
        : environment_(std::move(environment))
        , params_(params)
        , readiness_(params.allowDry)
        , target_(target)
        , busyAdmission_(std::move(busyAdmission))
    {}

    ~ExportJob()
    {
        writer_.reset();
        removeTemporary();
        busyAdmission_.release();
    }

    ExportJob(const ExportJob&) = delete;
    ExportJob& operator=(const ExportJob&) = delete;

    void step(ControlJobStep& step)
    {
        ControlOpContext context(environment_);
        if (writer_ == nullptr)
            stepWaiting(context, step);
        else
            stepWriting(context, step);
    }

private:
    void stepWaiting(ControlOpContext& context, ControlJobStep& step)
    {
        if (step.cancelRequested()) {
            finishCancelled(step);
            return;
        }
        const auto plan = StandaloneMediaOperations::buildExportPlan(context.processor(), target_);
        if (plan.targetGone) {
            fail(step, {"REVISION_CONFLICT", "changed_during_export", "the clip of the export was removed", false});
            return;
        }
        if (plan.contents.empty()) {
            fail(step, {"INVALID_ARGUMENT", "nothing_to_export", "the export holds no clip with audio", false});
            return;
        }
        int ready = 0;
        auto dry = JsonValue::array();
        for (const auto& key : plan.contents) {
            const auto readiness = readiness_.classify(context.processor(), key);
            if (readiness.state == Readiness::State::Fail) {
                fail(step, readinessError(readiness));
                return;
            }
            if (readiness.state == Readiness::State::Wait)
                continue;
            ++ready;
            if (readiness.state == Readiness::State::Dry) {
                auto entry = JsonValue::object();
                entry.appendMember("contentId", JsonValue::string(controlContentId(context, key)));
                entry.appendMember("reason", JsonValue::string(readiness.dryReason));
                dry.append(std::move(entry));
            }
        }
        const int total = static_cast<int>(plan.contents.size());
        if (ready == total) {
            const auto status = StandaloneMediaOperations::validateExportPlan(context.processor(), plan);
            if (status == StandaloneMediaOperations::ExportPlanStatus::TargetGone) {
                fail(step, {"REVISION_CONFLICT", "changed_during_export", "the clip of the export was removed", false});
                return;
            }
            if (status == StandaloneMediaOperations::ExportPlanStatus::ContentUnavailable) {
                Readiness unavailable;
                unavailable.state = Readiness::State::Fail;
                unavailable.failure = StandaloneMediaOperations::ExportFailure::ContentUnavailable;
                fail(step, readinessError(unavailable));
                return;
            }
            if (status == StandaloneMediaOperations::ExportPlanStatus::Changed) {
                setProgress(step, static_cast<double>(ready) / total, "waiting", ready, total);
                return;
            }
            startWriting(context, plan, std::move(dry), step.wakeCallback());
            if (writer_ != nullptr)
                setProgress(step, std::nullopt, "writing", std::nullopt, std::nullopt);
            else
                fail(step, {"IO_ERROR", "write_failed", "the target directory cannot be written", false});
            return;
        }
        setProgress(step, static_cast<double>(ready) / total, "waiting", ready, total);
    }

    /** In the tick that found every content ready and the plan current, records result metadata and
        starts the writer on a temporary file in the target directory. */
    void startWriting(ControlOpContext& context, const ExportPlan& plan, JsonValue dry, std::function<void()> wake)
    {
        std::vector<ControlContentVersions> versions;
        for (const auto& key : plan.contents) {
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
        dryContents_ = std::move(dry);
        processing_ = processingJson(context);
        startTimelineSec_ = target_.kind == ExportKind::Clip ? plan.clipStartSec : 0.0;
        const auto target = fileOf(params_.path);
        const auto directory = target.getParentDirectory();
        if (!directory.isDirectory())
            return;
        // A name of its own in the target directory, so that the final rename never moves data between
        // volumes; OpenTune's export writes .wav files only.
        temporary_ =
            directory.getNonexistentChildFile("." + target.getFileNameWithoutExtension() + ".export", ".wav", false);
        writer_ = std::make_unique<ExportWriter>(plan, temporary_, std::move(wake));
    }

    void stepWriting(ControlOpContext& context, ControlJobStep& step)
    {
        // OpenTune's export cannot be interrupted: a cancellation waits for the write and then deletes it.
        if (!writer_->done())
            return;
        const bool written = writer_->written;
        const int channelCount = writer_->channelCount;
        const double sampleRateHz = writer_->sampleRateHz;
        const std::int64_t frameCount = writer_->frameCount;
        const std::string sampleFormat = writer_->sampleFormat;
        if (step.cancelRequested()) {
            finishCancelled(step);
            return;
        }
        if (!written) {
            AppLogger::logNoThrow("control API: export.start: OpenTune could not write the export file");
            fail(step, {"IO_ERROR", "write_failed", "OpenTune could not write the export file", false});
            return;
        }
        // Validate the exact plan passed to the worker against current product state.
        if (StandaloneMediaOperations::validateExportPlan(context.processor(), writer_->plan())
            != StandaloneMediaOperations::ExportPlanStatus::Current) {
            fail(step, {"REVISION_CONFLICT", "changed_during_export",
                        "a content or clip of the export changed while the file was written", false});
            return;
        }
        // Cancellable until the final rename.
        if (!step.endCancellability()) {
            finishCancelled(step);
            return;
        }
        std::string finalPath;
        if (auto error = moveIntoPlace(context, finalPath)) {
            fail(step, *error);
            return;
        }
        temporary_ = juce::File();
        auto result = JsonValue::object();
        result.appendMember("path", JsonValue::string(finalPath));
        result.appendMember("sampleRateHz", controlJsonNumber(sampleRateHz, 3));
        result.appendMember("channelCount", JsonValue::integer(channelCount));
        result.appendMember("sampleFormat", JsonValue::string(sampleFormat));
        result.appendMember("durationSec", controlJsonSeconds(static_cast<double>(frameCount) / sampleRateHz));
        result.appendMember("startTimelineSec", controlJsonSeconds(startTimelineSec_));
        result.appendMember("contents", contents_);
        result.appendMember("dryContents", dryContents_);
        result.appendMember("processing", processing_);
        writer_.reset();
        busyAdmission_.release();
        step.succeed(std::move(result));
    }

    /** The names that ifExists allows, each checked against the loaded
        media right before its rename. nullopt on success, with finalPath the name written. */
    std::optional<ControlJobError> moveIntoPlace(ControlOpContext& context, std::string& finalPath)
    {
        const auto media = loadedMediaPaths(context);
        const auto from = temporary_.getFullPathName().toStdString();
        const int lastNumber = params_.ifExists == IfExists::Rename ? kLastRenameNumber : 1;
        for (int number = 1; number <= lastNumber; ++number) {
            const auto candidate = number == 1 ? params_.path : numberedPath(params_.path, number);
            if (auto error = loadedMediaError(media, candidate))
                return error;
            switch (controlRenameFile(from, candidate, params_.ifExists == IfExists::Overwrite)) {
                case ControlRenameResult::DoneSourceRemains:
                    // The target holds the data, so the export succeeded; only the temporary name is left
                    // over. Remove it again now, and say so in the log if that fails too (no path).
                    if (!temporary_.deleteFile() && temporary_.exists())
                        AppLogger::logNoThrow("control API: export.start: the temporary file could not be removed");
                    [[fallthrough]];
                case ControlRenameResult::Done:
                    finalPath = candidate;
                    return std::nullopt;
                case ControlRenameResult::TargetExists:
                    break; // rename: the next name; error: the loop ends
                case ControlRenameResult::Failed:
                    return ControlJobError{"IO_ERROR", "write_failed", "the file could not be moved to the target",
                                           false};
            }
        }
        return ControlJobError{"ALREADY_EXISTS", "target_exists", "a file exists at the target", false};
    }

    static JsonValue processingJson(ControlOpContext& context)
    {
        // The processing preferences and the inference backend that the render was made with, in the form
        // session.info reports them (controlProcessingJson).
        const auto preferences = context.processor().getAppPreferences().getState().shared;
        const auto runtime = controlProcessingRuntime();
        const auto full = controlProcessingJson(preferences, &runtime);
        auto json = JsonValue::object();
        for (const char* name : {"vocoderWeight", "hybridMode", "backend", "tuningHz"})
            if (const JsonValue* value = full.find(name))
                json.appendMember(name, *value);
        if (const JsonValue* reasons = full.find("nullReasons"))
            if (const JsonValue* weight = reasons->find("vocoderWeight")) {
                auto nullReasons = JsonValue::object();
                nullReasons.appendMember("vocoderWeight", *weight);
                json.appendMember("nullReasons", std::move(nullReasons));
            }
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

    void removeTemporary()
    {
        if (temporary_ != juce::File())
            temporary_.deleteFile();
        temporary_ = juce::File();
    }

    void fail(ControlJobStep& step, const ControlJobError& error)
    {
        writer_.reset();
        removeTemporary();
        busyAdmission_.release();
        if (error.kind != "INVALID_ARGUMENT" || !error.reason.empty())
            AppLogger::logNoThrow("control API: export.start: the export failed with " + juce::String(error.kind) + "("
                                  + juce::String(error.reason) + ")");
        step.fail(error);
    }

    void finishCancelled(ControlJobStep& step)
    {
        writer_.reset();
        removeTemporary();
        busyAdmission_.release();
        step.cancel(JsonValue(), false);
    }

    ControlOpEnvironment environment_;
    ExportParams params_;
    StandaloneMediaOperations::ExportReadinessTracker readiness_;
    ExportTarget target_;
    ExportBusyGuard busyAdmission_;
    std::unique_ptr<ExportWriter> writer_;
    juce::File temporary_;
    JsonValue contents_ = JsonValue::array();
    JsonValue dryContents_ = JsonValue::array();
    JsonValue processing_;
    double startTimelineSec_ = 0.0;
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
    if (StandaloneMediaOperations::buildExportPlan(context.processor(), target).contents.empty())
        return controlOpError("INVALID_ARGUMENT", "nothing_to_export", "the export holds no clip with audio");
    // Check loaded media by file identity; the job checks again right before its final rename.
    if (const auto error = loadedMediaError(loadedMediaPaths(context), params.path))
        return controlOpError(error->kind, error->reason, error->detail);
    if (!fileOf(params.path).getParentDirectory().isDirectory())
        return controlOpError("IO_ERROR", "write_failed", "the directory of the target does not exist");
    // With ifExists error, an existing target fails at once and nothing is rendered.
    if (params.ifExists == IfExists::Error && fileOf(params.path).exists())
        return controlOpError("ALREADY_EXISTS", "target_exists", "a file exists at the target");

    // The shared message-thread busy state that the editor's export keeps too, taken here on the
    // message thread after every check, held until the job ends.
    if (!processor.tryBeginExport())
        return controlOpError("BUSY", "export_in_progress", "an export is running");
    ExportBusyGuard busyAdmission(processor);
    auto job = std::make_shared<ExportJob>(context.environment(), params, target, std::move(busyAdmission));
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
