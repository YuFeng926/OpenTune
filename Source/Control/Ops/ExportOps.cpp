// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExportOps.h"

#include "OpsJson.h"
#include "OpsSupport.h"
#include "SessionOps.h"
#include "WriteEnvelope.h"

#include "Content/EditableContentSnapshot.h"
#include "Control/ControlFileIdentity.h"
#include "Control/ControlJobs.h"
#include "Control/ControlPathPolicy.h"
#include "Inference/RenderCache.h"
#include "PluginProcessor.h"
#include "Render/ContentRenderService.h"
#include "Render/PlaybackReadSource.h"
#include "Runtime/ProcessRenderRuntime.h"
#include "SourceStore.h"
#include "Utils/AppLogger.h"
#include "Utils/AppPreferences.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

constexpr std::size_t kMaxPathCodePoints = 4096;
/** PROTOCOL.md section 19 rule 7: the longest wait for analysis and rendering. */
constexpr int kWaitLimitMs = 120000;
/** How long the render worker must be seen idle, with the render settled and no current time-stretch output, before
    the job asks for the time-stretch rebuild of a content again (the rebuild it asked for was dropped). */
constexpr int kStretchRetryIdleMs = 500;
/** Samples per slice of the probe of a published render for rendered audio (publishedRenderHasAudio). */
constexpr int kPublishedAudioProbeSamples = 1 << 16;
/** Section 19 rule 3: the last name that ifExists rename tries is "<stem> (999).wav". */
constexpr int kLastRenameNumber = 999;
/** The extension an export writes (section 20.1), compared without regard to case. */
constexpr std::string_view kWavExtension = ".wav";

int& waitLimitOverrideMs()
{
    static int milliseconds = 0;
    return milliseconds;
}

/**
 * Whether OpenTune's render runtime holds a deferred render retry of the processor's render service: a render
 * block that could not be handed to the render worker (the vocoder is being reconfigured or not loaded) and is
 * kept until it can. ProcessRenderRuntime::hasDeferredRetriesForService, the test that
 * ContentRenderService::drainRenderWorkerForExport makes before it lets an export read the render: while one
 * exists, that drain gives up and the export functions report "Render is not ready for export". The runtime
 * resumes such retries on its own on the message thread (postDeferredRetryEvent), so the job only waits for
 * them; it never flushes them itself. Message thread.
 */
bool renderRetriesDeferred(OpenTuneAudioProcessor& processor)
{
    const auto* service = processor.getContentRenderService();
    return service != nullptr && ProcessRenderRuntime::getInstance().hasDeferredRetriesForService(service);
}

enum class ExportKind {
    Clip,
    Track,
    Master
};

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

    /** The params after defaults, as one string: equal strings mean equal normalized params (section 19
        rule 5). */
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

/** methods/export.start.schema.json $defs.params, on the connection thread. The path gets the checks of
    PROTOCOL.md section 20.1 on the message thread, as method-specific validation with its own reasons. */
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

/** "<stem> (n).wav" next to the target, <stem> being its file name without .wav (section 19 rule 3). */
std::string numberedPath(const std::string& path, int number)
{
    return path.substr(0, path.size() - kWavExtension.size()) + " (" + std::to_string(number) + ").wav";
}

// ---- What the file is made of --------------------------------------------------------------------

/** What export.start resolved: the kind, the track and, for a clip, its placement id. */
struct ExportTarget
{
    ExportKind kind = ExportKind::Master;
    int trackId = 0;
    std::uint64_t placementId = 0;
};

/**
 * The clips and contents that OpenTune's export of the target mixes, now. It mirrors the selection of
 * OpenTuneAudioProcessor::exportPlacementAudio, exportTrackAudio and exportMasterMixAudio (Source/
 * PluginProcessor.cpp): the clips whose content has a playback source with audio; for the master mix the
 * tracks of the playback snapshot, only the soloed ones while a track is soloed, else those not muted.
 */
struct ExportPlan
{
    /** The clip of a clip export no longer exists. */
    bool targetGone = false;
    /** Where the clip of a clip export is now. */
    int clipTrackId = 0;
    double clipStartSec = 0.0;
    /** Each content of the export once, in the order of the clips. */
    std::vector<ContentKey> contents;
    /** A token of everything besides the contents that the file is made of: each clip's track, position,
        length, source window, gain and fades, and the volume of its track. */
    std::string mixToken;
};

bool hasPlaybackAudio(OpenTuneAudioProcessor& processor, const ContentKey& key)
{
    // ContentRenderService::getPlaybackReadSource(): what OpenTune's export reads; a clip without one is
    // left out of the file.
    const auto* service = processor.getContentRenderService();
    PlaybackReadSource source;
    return service != nullptr && service->getPlaybackReadSource(key, source) && source.hasAudio();
}

void addClip(ExportPlan& plan, ControlTokenHasher& hasher, int trackId, std::uint64_t placementId,
             const ContentKey& key, double startSec, double durationSec, double clipInSec, float gain, double fadeInSec,
             double fadeOutSec, float trackVolume)
{
    if (std::find(plan.contents.begin(), plan.contents.end(), key) == plan.contents.end())
        plan.contents.push_back(key);
    hasher.add(trackId)
        .add(placementId)
        .add(static_cast<int>(key.domainKind))
        .add(key.objectId)
        .add(key.sourceWindowDiscriminator)
        .add(startSec)
        .add(durationSec)
        .add(clipInSec)
        .add(static_cast<double>(gain))
        .add(fadeInSec)
        .add(fadeOutSec)
        .add(static_cast<double>(trackVolume));
}

ExportPlan buildPlan(ControlOpContext& context, const ExportTarget& target)
{
    ExportPlan plan;
    auto& processor = context.processor();
    const auto* arrangement = processor.getStandaloneArrangement();
    ControlTokenHasher hasher('x');
    hasher.add(static_cast<int>(target.kind));
    if (arrangement == nullptr) {
        plan.targetGone = target.kind == ExportKind::Clip;
        plan.mixToken = hasher.token();
        return plan;
    }
    switch (target.kind) {
        case ExportKind::Clip: {
            // The clip by its id, on whichever track it is now.
            StandaloneArrangement::Placement placement;
            int trackId = -1;
            for (int t = 0; t < arrangement->getNumTracks() && trackId < 0; ++t)
                if (arrangement->getPlacementById(t, target.placementId, placement) && !placement.isRetired)
                    trackId = t;
            if (trackId < 0) {
                plan.targetGone = true;
                break;
            }
            plan.clipTrackId = trackId;
            plan.clipStartSec = placement.timelineStartSeconds;
            if (hasPlaybackAudio(processor, placement.contentKey))
                addClip(plan, hasher, trackId, placement.placementId, placement.contentKey,
                        placement.timelineStartSeconds, placement.durationSeconds, placement.clipInSeconds,
                        placement.gain, placement.fadeInDuration, placement.fadeOutDuration,
                        arrangement->getTrackVolume(trackId));
            break;
        }
        case ExportKind::Track: {
            const float volume = arrangement->getTrackVolume(target.trackId);
            const int count = arrangement->getNumPlacements(target.trackId);
            for (int index = 0; index < count; ++index) {
                StandaloneArrangement::Placement placement;
                if (!arrangement->getPlacementByIndex(target.trackId, index, placement)
                    || !hasPlaybackAudio(processor, placement.contentKey))
                    continue;
                addClip(plan, hasher, target.trackId, placement.placementId, placement.contentKey,
                        placement.timelineStartSeconds, placement.durationSeconds, placement.clipInSeconds,
                        placement.gain, placement.fadeInDuration, placement.fadeOutDuration, volume);
            }
            break;
        }
        case ExportKind::Master: {
            const auto snapshot = arrangement->loadPlaybackSnapshot();
            if (snapshot == nullptr)
                break;
            for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId) {
                const auto& track = snapshot->tracks[static_cast<std::size_t>(trackId)];
                if (snapshot->anySoloed ? !track.isSolo : track.isMuted)
                    continue;
                for (const auto& placement : track.placements)
                    if (hasPlaybackAudio(processor, placement.contentKey))
                        addClip(plan, hasher, trackId, 0, placement.contentKey, placement.timelineStartSeconds,
                                placement.durationSeconds, placement.clipInSeconds, placement.gain,
                                placement.fadeInDuration, placement.fadeOutDuration, track.volume);
            }
            break;
        }
    }
    plan.mixToken = hasher.token();
    return plan;
}

/**
 * The token of the export's inputs (section 19 rule 8): the mix token of the plan and, per content, the
 * versions that its render is made of (the revision token of notes, pitch and content, the time grid and
 * the pitch shift). versions receives the versions of the contents, in the plan's order.
 */
std::string inputsToken(ControlOpContext& context, const ExportPlan& plan,
                        std::vector<ControlContentVersions>* versions = nullptr)
{
    ControlTokenHasher hasher('e');
    hasher.add(std::string_view(plan.mixToken));
    for (const auto& key : plan.contents) {
        const auto snapshot = controlLiveSnapshot(context, key);
        if (snapshot == nullptr) {
            hasher.add(std::string_view("gone"));
            continue;
        }
        auto current = controlContentVersions(context, controlContentId(context, key), *snapshot);
        hasher.add(std::string_view(current.revision()))
            .add(std::string_view(current.grid))
            .add(std::string_view(current.pitchShift));
        if (versions != nullptr)
            versions->push_back(std::move(current));
    }
    return hasher.token();
}

// ---- Loaded media ----------------------------------------------------------------------------------

/** The source file of every source OpenTune holds (SourceStore), active or retired, since an undo step
    can bring a retired one back: the loaded media that an export never writes (section 19 rule 2). */
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

/** Section 19 rule 2 by file identity (section 20.3): the error when the target is loaded media or when an
    identity that the check needs cannot be determined; nullopt otherwise. */
std::optional<ControlJobError> loadedMediaError(const std::vector<std::string>& media, const std::string& target)
{
    // The identity of the target is read once. A loaded media file whose identity cannot be read for any
    // reason but "does not exist" (including an unavailable volume) rejects the export (rule 4): the same
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

/** The lock that the editor's export and export.start share, released once. */
class ExportLock
{
public:
    ExportLock() = default;
    explicit ExportLock(OpenTuneAudioProcessor& processor) : processor_(&processor) {}
    ExportLock(ExportLock&& other) noexcept : processor_(std::exchange(other.processor_, nullptr)) {}
    ExportLock& operator=(ExportLock&&) = delete;
    ExportLock(const ExportLock&) = delete;
    ExportLock& operator=(const ExportLock&) = delete;
    ~ExportLock() { release(); }

    void release() noexcept
    {
        if (processor_ != nullptr)
            processor_->releaseExportLock();
        processor_ = nullptr;
    }

private:
    OpenTuneAudioProcessor* processor_ = nullptr;
};

// ---- The file, written on a thread of its own ------------------------------------------------------

/**
 * Has OpenTune write the export into a file on a thread of its own, as the editor's export does on its
 * export thread (exportAudioRequested in Source/Standalone/PluginEditor.cpp): exportPlacementAudio,
 * exportTrackAudio or exportMasterMixAudio of OpenTuneAudioProcessor, which wait for the render worker
 * (drainRenderWorker) and write 44.1 kHz 32-bit float WAV. Then it reads the header of the file back.
 * The destructor joins the thread; OpenTune's export cannot be interrupted, so that waits for the write.
 * This is the one known exception to the bounded joins of the shutdown contract (Source/Control/README.md):
 * the editor's own export thread is joined the same way. The worst case is the time that the render worker
 * needs to drain the process's whole render queue plus the time to write the file, and it depends on the
 * number and length of the pending renders and on the speed of the disk.
 */
class ExportWriter
{
public:
    ExportWriter(OpenTuneAudioProcessor& processor, ExportKind kind, int trackId, std::uint64_t placementId,
                 juce::File file)
        : kind_(kind)
        , trackId_(trackId)
        , placementId_(placementId)
        , file_(std::move(file))
    {
        thread_ = std::thread([this, &processor] {
            run(processor);
            done_ = true;
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

    // Valid once done() is true.
    bool written = false;
    /** The export function refused because the render was not ready (a deferred render retry stayed, see
        renderRetriesDeferred): nothing was written, and a later attempt may succeed. */
    bool renderNotReady = false;
    int channelCount = 0;
    double sampleRateHz = 0.0;
    std::int64_t frameCount = 0;
    std::string sampleFormat;

private:
    void run(OpenTuneAudioProcessor& processor) noexcept
    {
        try {
            bool ok = false;
            bool called = true;
            switch (kind_) {
                case ExportKind::Clip: {
                    // exportPlacementAudio() takes the clip's index on its track, which
                    // StandaloneArrangement::findPlacementIndexById() gives for its id right before.
                    const auto* arrangement = processor.getStandaloneArrangement();
                    const int index =
                        arrangement != nullptr ? arrangement->findPlacementIndexById(trackId_, placementId_) : -1;
                    called = index >= 0;
                    ok = called && processor.exportPlacementAudio(trackId_, index, file_, kErrorTextLanguage);
                    break;
                }
                case ExportKind::Track:
                    ok = processor.exportTrackAudio(trackId_, file_, kErrorTextLanguage);
                    break;
                case ExportKind::Master:
                    ok = processor.exportMasterMixAudio(file_, kErrorTextLanguage);
                    break;
            }
            if (ok)
                readBack();
            else if (called)
                // The one failure of the export functions that the job reports differently: their only text for it
                // (OpenTuneAudioProcessor::getLastExportError, cleared at the start of every export) is the key itself
                // in every language, so it is compared in the language that was passed.
                renderNotReady = processor.getLastExportError() == Loc::get(kErrorTextLanguage, kRenderNotReadyText);
        } catch (...) {
            written = false;
        }
    }

    void readBack()
    {
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

    // The export functions take the language of the error text they leave in getLastExportError(). The
    // editor passes the language of its window; this writer never reads that text (a failed export is
    // reported with a fixed detail), and a headless process has no window whose language could be asked
    // (LocalizationManager::resolveLanguage() needs a bound language), so a fixed language is passed.
    static constexpr Language kErrorTextLanguage = Language::English;
    /** What OpenTuneAudioProcessor::exportPlacementAudio / exportTrackAudio / exportMasterMixAudio leave in
        getLastExportError() when ContentRenderService::drainRenderWorkerForExport() fails. */
    static constexpr const char* kRenderNotReadyText = "Render is not ready for export";

    ExportKind kind_;
    int trackId_;
    std::uint64_t placementId_;
    juce::File file_;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

// ---- The job --------------------------------------------------------------------------------------

/** How one content of the export stands (section 19 rule 6). */
struct Readiness
{
    enum class State {
        Wait,
        Ready,
        Dry,
        Fail
    };
    State state = State::Wait;
    /** dryContents[].reason: unvoiced, nothingToRender or analysisFailed. */
    const char* dryReason = "";
    ControlJobError error;
};

/**
 * The work of one export job, on the message thread (section 19).
 *
 * - While it waits, each tick builds the plan again (the user may change the project meanwhile) and
 *   decides for every content: analysis not finished: wait; no voiced audio or nothing to render: export it
 *   uncorrected and list it in dryContents; analysis failed otherwise: ANALYSIS_FAILED(f0_failed), or dry
 *   with allowDry; render work pending: wait (a content with nothing to render does not wait for blocks out
 *   of the render worker's reach, pendingBlocksStuck); then a render block failed: request the render once
 *   more (OpenTuneAudioProcessor::requestFullContentRender, whose reconcile retries a failed block of the
 *   same revision), then RENDER_FAILED(render_chunk_failed); else ready once the render cache is settled at
 *   its current revision (RenderCache::isCanonicalSettled). A content without
 *   render blocks is dry at once when it has nothing to render (no correction, identity pitch shift, no
 *   active note EQ) and an identity time grid; otherwise it gets one render request. A content whose time
 *   grid is not the identity is exported from its time-stretch output, so it is ready (or exported dry) only
 *   once that output was rebuilt at its current revisions (stretchState); a content to export dry without a
 *   render to stretch fails at once. Even when every content is ready the job keeps waiting while the render
 *   runtime holds a deferred render retry of the render service (renderRetriesDeferred): the export functions
 *   of OpenTune refuse to read the render then ("Render is not ready for export"). If they refuse anyway
 *   (a retry deferred after the check), the job ends the same way.
 *   NOT_READY(export_wait_timeout) after the wait limit, or when the writer found the render not ready; no
 *   file is written then.
 * - In the tick that finds every content ready it records the inputs and the revisions and starts the
 *   writer on a temporary file in the target directory.
 * - Once the file is written: the inputs are compared with the recorded ones (REVISION_CONFLICT
 *   (changed_during_export) when they changed), cancellability ends, loaded media are checked again for
 *   the name used, and the final rename moves the file into place (section 20.4).
 * - Every failure and cancellation deletes the temporary file and leaves the target untouched (executed
 *   false). The export lock is released when the job ends or its work is destroyed.
 */
class ExportJob
{
public:
    ExportJob(ControlOpEnvironment environment, const ExportParams& params, const ExportTarget& target, ExportLock lock)
        : environment_(std::move(environment))
        , params_(params)
        , target_(target)
        , lock_(std::move(lock))
        , started_(std::chrono::steady_clock::now())
        , waitLimit_(std::chrono::milliseconds(waitLimitOverrideMs() > 0 ? waitLimitOverrideMs() : kWaitLimitMs))
    {}

    ~ExportJob()
    {
        writer_.reset();
        removeTemporary();
        lock_.release();
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
    struct ContentProgress
    {
        bool renderRequested = false;
        bool renderRetried = false;
        /** The content and time-grid revisions of the job's requests for the time-stretch rebuild, how many it made
            for them, and since when the render worker has been seen idle without a current output after the last. */
        std::optional<std::pair<std::uint64_t, std::uint64_t>> stretchRequestedFor;
        int stretchRequests = 0;
        std::optional<std::chrono::steady_clock::time_point> stretchIdleSince;
        /** Since when the pending render blocks of the content have been seen out of reach of the render worker
            (pendingBlocksStuck), and whether the log said so. */
        std::optional<std::chrono::steady_clock::time_point> stuckSince;
        /** Whether rendered audio of the content was found while its blocks have been out of reach (once per such
            time; publishedRenderHasAudio). */
        std::optional<bool> stuckRenderedAudio;
        bool stuckLogged = false;
    };

    /** Where the time-stretch output of a content stands (stretchState). */
    enum class Stretch {
        /** Identity grid, or the output was built at the current revisions. */
        Current,
        /** The output is not current yet; the job waits. */
        Pending,
        /** The content has no render to stretch and none will come. */
        NoRender,
        /** The job asked for the rebuild kControlExportMaxStretchRequests times and the output is still not current. */
        GaveUp
    };

    Readiness classify(ControlOpContext& context, const ContentKey& key)
    {
        auto readiness = classifyRender(context, key);
        if (readiness.state != Readiness::State::Ready && readiness.state != Readiness::State::Dry)
            return readiness;
        switch (stretchState(context, key)) {
            case Stretch::Current:
                return readiness;
            case Stretch::Pending:
                return Readiness{};
            case Stretch::NoRender:
                return unstretchable();
            case Stretch::GaveUp: {
                Readiness failed;
                failed.state = Readiness::State::Fail;
                failed.error = {"NOT_READY", "export_stretch_dropped",
                                "the time-stretch rebuild of a content of the export was asked for "
                                    + std::to_string(kControlExportMaxStretchRequests) + " times and did not complete",
                                false};
                return failed;
            }
        }
        return readiness;
    }

    /**
     * A content that section 19 rule 6 exports uncorrected (dry), whose time grid is not the identity and that has no
     * render to stretch (classify, Stretch::NoRender): OpenTune builds the stretched audio only from a settled render
     * (ContentRenderService::enqueueStage2RebuildWhenCanonicalSettled), so it has no uncorrected audio of the content
     * for the file, and waiting does not change that. The job ends at once instead of at the wait limit, whatever made
     * the content dry (unvoiced, nothing to render, or a failed analysis with allowDry), with
     * NOT_READY(export_dry_stretched): not retryable, it succeeds again once the grid is the identity.
     */
    static Readiness unstretchable()
    {
        Readiness failed;
        failed.state = Readiness::State::Fail;
        failed.error = {"NOT_READY", "export_dry_stretched",
                        "a content of the export would be exported uncorrected, but its time grid is stretched and "
                        "OpenTune has no render to build stretched audio from",
                        false};
        return failed;
    }

    /**
     * Section 19 rule 6 for a content whose time grid is not the identity: OpenTune plays and exports such a
     * content from its time-stretch output (the Stage2 rebuild, Stage2TimeStretchRebuilder), not from its render,
     * and its export reads that output only when it was built at the content's current content and time-grid
     * revisions (readExportPlaybackAudio in Utils/PlaybackAudioReader.h); otherwise the content would be silent or
     * old in the file. The content is ready only once that output is current, which the export's own read
     * (TimeStretchCache::sliceCanonicalForOutputRange) tells.
     *
     * The revisions come from the live snapshot (controlLiveSnapshot), while the export reads with the snapshot of
     * the published playback source (request.source.contentSnapshot). They agree here: setContentTimeGrid()
     * republishes the playback source before it returns, on the message thread this runs on, and the rebuild stores
     * its output only while the published source carries the revisions it was built for
     * (Stage2TimeStretchRebuilder::rebuild checks it before and after), so an output that is current for the live
     * revisions is current for the published source.
     *
     * The rebuild needs a settled render (enqueueStage2RebuildWhenCanonicalSettled). A content without one that the
     * job would export dry and that has no render work pending will never get one: NoRender. OpenTune queues the
     * rebuild itself when the grid changes while the render is settled (OpenTuneAudioProcessor::setContentTimeGrid)
     * and when a render block settles. When the render is settled, no output is current and the render worker has
     * nothing queued or running, the job queues the rebuild as setContentTimeGrid does (with the published playback
     * source). The worker may drop a rebuild (Stage2TimeStretchRebuilder::rebuild gives up when the published source
     * changed meanwhile, without a new revision), so when the worker has been seen idle for kStretchRetryIdleMs with
     * still no current output, the job asks again, up to kControlExportMaxStretchRequests times for the same
     * revisions: GaveUp after that. New revisions start the count again.
     */
    Stretch stretchState(ControlOpContext& context, const ContentKey& key)
    {
        const auto snapshot = controlLiveSnapshot(context, key);
        if (snapshot == nullptr || snapshot->timeGrid == nullptr || snapshot->timeGrid->isIdentity())
            return Stretch::Current;
        auto* service = context.processor().getContentRenderService();
        if (service == nullptr)
            return Stretch::Current;
        juce::AudioBuffer<float> probe(1, 1);
        if (service->getTimeStretchCache().sliceCanonicalForOutputRange(key, snapshot->contentRevision,
                                                                        snapshot->timeGridRevision, 0, probe, 0, 1)
            > 0)
            return Stretch::Current;
        auto& progress = contentProgress_[key];
        const std::pair<std::uint64_t, std::uint64_t> revisions{snapshot->contentRevision, snapshot->timeGridRevision};
        if (progress.stretchRequestedFor != revisions) {
            progress.stretchRequestedFor.reset();
            progress.stretchRequests = 0;
            progress.stretchIdleSince.reset();
        }
        const auto cache = service->getRenderCache(key);
        if (cache == nullptr || !cache->isCanonicalSettled()) {
            // classifyRender returns Ready only for a settled render, so this is a content to export dry.
            const bool renderPending = cache != nullptr && cache->getChunkStats().hasActiveWork();
            return renderPending ? Stretch::Pending : Stretch::NoRender;
        }
        if (service->renderQueueDepth() != 0 || service->renderInFlight() != 0
            || renderRetriesDeferred(context.processor())) {
            progress.stretchIdleSince.reset();
            return Stretch::Pending;
        }
        const auto now = std::chrono::steady_clock::now();
        if (progress.stretchRequests > 0) {
            // The worker finished the rebuild the job asked for, and the output is still not current.
            if (!progress.stretchIdleSince)
                progress.stretchIdleSince = now;
            if (now - *progress.stretchIdleSince < std::chrono::milliseconds(kStretchRetryIdleMs))
                return Stretch::Pending;
            if (progress.stretchRequests >= kControlExportMaxStretchRequests)
                return Stretch::GaveUp;
        }
        PlaybackReadSource source;
        if (service->getPlaybackReadSource(key, source)
            && service->enqueueStage2RebuildWhenCanonicalSettled(key, snapshot, source.audioBuffer,
                                                                 source.audioSampleRate)) {
            progress.stretchRequestedFor = revisions;
            ++progress.stretchRequests;
            progress.stretchIdleSince.reset();
        }
        return Stretch::Pending;
    }

    /**
     * Section 19 rule 6, "analysis found no voiced audio": the analysis has F0 frames and at least one of them is
     * above 0 Hz. An analysis that ended Ready without any (what a silent file gives) has none. The same test as the
     * readiness gate of the other writes (controlCheckContentReady, NOT_READY(f0_empty)).
     */
    static bool hasVoicedFrame(const EditableContentSnapshot& snapshot)
    {
        if (!snapshot.hasUsableOriginalF0())
            return false;
        const auto& f0 = snapshot.pitchCurve->getOriginalF0();
        return std::any_of(f0.begin(), f0.end(), [](float hz) { return hz > 0.0f; });
    }

    /**
     * Whether the current version of a content has anything to render. OpenTune's render marks a block blank, and
     * plays the content's own audio for it, when the global pitch shift is the identity, no correction segment covers
     * the block and no note with an active EQ intersects it (ProcessRenderRuntime::processChunkRenderJob). Without a
     * correction segment, with the identity pitch shift and no active note EQ, every block of the content is blank.
     */
    static bool needsRender(const EditableContentSnapshot& snapshot)
    {
        if (!snapshot.pitchShiftSettings.isIdentity())
            return true;
        if (snapshot.pitchCurve != nullptr && snapshot.pitchCurve->hasCorrectionLayer())
            return true;
        return std::any_of(snapshot.notes.begin(), snapshot.notes.end(),
                           [](const Note& note) { return note.eq.has_value() && note.eq->active; });
    }

    /**
     * Whether the pending render blocks of a content are out of reach of OpenTune's render worker: blocks are pending,
     * none is running and none is in the cache's set of blocks to claim (RenderCache::getPendingJobs, the set that
     * RenderCache::claimPendingJob takes from), seen so for kControlExportStuckBlocksMs. Upstream
     * RenderCache::reconcileFullPlanAndRequest leaves blocks so when a render is asked for again at a revision for
     * which they are still pending: it keeps them pending but out of that set, and the worker's queue drops them
     * (RenderWorker::syncStage1QueueLocked); only a new revision of the content queues them again. The set is read
     * before the counts, so a block the worker claims meanwhile is never taken for one out of reach.
     */
    bool pendingBlocksStuck(ContentProgress& progress, const RenderCache& cache)
    {
        const bool noneToClaim = cache.getPendingJobs().empty();
        const auto stats = cache.getChunkStats();
        if (!noneToClaim || stats.pending == 0 || stats.running != 0) {
            progress.stuckSince.reset();
            progress.stuckRenderedAudio.reset();
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!progress.stuckSince)
            progress.stuckSince = now;
        return now - *progress.stuckSince >= std::chrono::milliseconds(kControlExportStuckBlocksMs);
    }

    /**
     * Whether the published render of a content holds rendered audio: the audio that the export lays over the
     * content's own when its time grid is the identity (readExportPlaybackAudio, through
     * RenderCache::overlayCanonicalAudio). The blocks in the cache do not tell that on their own: a failed block, or
     * a block whose span changed, no longer holds its audio, while the published render, which changes only once every
     * block has settled, still does. Probes the whole content in slices with a value no render produces (NaN); without
     * the playback source of the content it answers true, so that the content keeps waiting.
     */
    static bool publishedRenderHasAudio(const ContentRenderService& service, const ContentKey& key,
                                        const RenderCache& cache)
    {
        PlaybackReadSource source;
        if (!service.getPlaybackReadSource(key, source) || source.audioBuffer == nullptr)
            return true;
        const std::int64_t sampleCount = source.audioBuffer->getNumSamples();
        juce::AudioBuffer<float> probe(1, kPublishedAudioProbeSamples);
        for (std::int64_t start = 0; start < sampleCount; start += kPublishedAudioProbeSamples) {
            const int count =
                static_cast<int>(std::min<std::int64_t>(kPublishedAudioProbeSamples, sampleCount - start));
            juce::FloatVectorOperations::fill(probe.getWritePointer(0), std::numeric_limits<float>::quiet_NaN(), count);
            cache.overlayCanonicalAudio(probe, 0, count, start);
            const float* samples = probe.getReadPointer(0);
            if (std::any_of(samples, samples + count, [](float value) { return !std::isnan(value); }))
                return true;
        }
        return false;
    }

    Readiness classifyRender(ControlOpContext& context, const ContentKey& key)
    {
        auto& processor = context.processor();
        Readiness readiness;
        const auto snapshot = controlLiveSnapshot(context, key);
        if (snapshot == nullptr)
            return readiness;
        switch (snapshot->originalF0State) {
            case OriginalF0State::NotRequested:
            case OriginalF0State::Extracting:
                return readiness;
            case OriginalF0State::Failed: {
                // The failure message OpenTune keeps with the content: f0_empty for an analysis that found no
                // voiced frame. A failure without a message counts as a failure.
                if (std::string_view(controlF0FailureCode(*snapshot)) == "f0_empty") {
                    readiness.state = Readiness::State::Dry;
                    readiness.dryReason = "unvoiced";
                } else if (params_.allowDry) {
                    readiness.state = Readiness::State::Dry;
                    readiness.dryReason = "analysisFailed";
                } else {
                    readiness.state = Readiness::State::Fail;
                    readiness.error = {"ANALYSIS_FAILED", "f0_failed",
                                       "the pitch analysis of a content of the export failed; allowDry exports it "
                                       "uncorrected",
                                       false};
                }
                return readiness;
            }
            case OriginalF0State::Ready:
                break;
        }
        // "Analysis found no voiced audio": decided before anything about the render, whose blocks would all end
        // blank for such a content and list it as nothingToRender.
        if (!hasVoicedFrame(*snapshot)) {
            readiness.state = Readiness::State::Dry;
            readiness.dryReason = "unvoiced";
            return readiness;
        }
        auto& progress = contentProgress_[key];
        const auto* service = processor.getContentRenderService();
        const auto cache = service != nullptr ? service->getRenderCache(key) : nullptr;
        const auto stats = cache != nullptr ? cache->getChunkStats() : RenderCache::ChunkStats{};
        // A content with nothing to render whose export reads its own audio: every block of its render would end
        // blank, so the job does not ask for that render or wait for it.
        const bool identityGrid = snapshot->timeGrid == nullptr || snapshot->timeGrid->isIdentity();
        const bool dryWithoutRender = !needsRender(*snapshot) && identityGrid;
        if (stats.total() == 0) {
            // No render block: no render was requested for the content (or its render cache was cleared), so the
            // export reads the content's own audio; such a content with nothing to render is dry at once. Otherwise
            // the job asks for the render once (a stretched grid needs a settled render for its time-stretch output,
            // stretchState); a content that still has no block after that has nothing to render either.
            if (!progress.renderRequested && !dryWithoutRender) {
                progress.renderRequested = true;
                processor.requestFullContentRender(key);
                return readiness;
            }
            readiness.state = Readiness::State::Dry;
            readiness.dryReason = "nothingToRender";
            return readiness;
        }
        // Render work pending or running: wait, also before asking again for a block that failed. A request at a
        // revision for which blocks are still pending leaves them pending without queueing them (upstream
        // RenderCache::reconcileFullPlanAndRequest; pendingBlocksStuck), so asking while they are pending would
        // keep them from ever being rendered. Pending blocks that will not be rendered do not hold back a content
        // with nothing to render, as long as no failed block waits and the export would read the content's own
        // audio: neither a block nor the published render holds rendered audio of an earlier version
        // (publishedRenderHasAudio, probed once while the blocks stay out of reach; the published render cannot
        // change before they settle). Any other content waits for them until the wait limit.
        if (stats.hasActiveWork()) {
            if (pendingBlocksStuck(progress, *cache)) {
                if (dryWithoutRender && stats.failed == 0) {
                    if (!progress.stuckRenderedAudio)
                        progress.stuckRenderedAudio = cache->getStateSnapshot().hasPublishedAudio
                                                      || publishedRenderHasAudio(*service, key, *cache);
                    if (!*progress.stuckRenderedAudio) {
                        readiness.state = Readiness::State::Dry;
                        readiness.dryReason = "nothingToRender";
                        return readiness;
                    }
                }
                if (!progress.stuckLogged) {
                    progress.stuckLogged = true;
                    AppLogger::logNoThrow("control API: export.start: render blocks of a content of the export are "
                                          "pending but not queued; they are rendered only after the content changes");
                }
            }
            return readiness;
        }
        progress.stuckSince.reset();
        progress.stuckRenderedAudio.reset();
        if (stats.failed > 0) {
            if (progress.renderRetried) {
                readiness.state = Readiness::State::Fail;
                readiness.error = {"RENDER_FAILED", "render_chunk_failed",
                                   "a render block of a content of the export failed twice", false};
                return readiness;
            }
            progress.renderRetried = true;
            progress.renderRequested = true;
            processor.requestFullContentRender(key);
            return readiness;
        }
        if (stats.blank == stats.total()) {
            readiness.state = Readiness::State::Dry;
            readiness.dryReason = "nothingToRender";
            return readiness;
        }
        if (cache->isCanonicalSettled()) {
            readiness.state = Readiness::State::Ready;
            return readiness;
        }
        // Blocks that are neither pending nor published at their revision: ask for the render once.
        if (!progress.renderRequested) {
            progress.renderRequested = true;
            processor.requestFullContentRender(key);
        }
        return readiness;
    }

    void stepWaiting(ControlOpContext& context, ControlJobStep& step)
    {
        if (step.cancelRequested()) {
            finishCancelled(step);
            return;
        }
        const auto plan = buildPlan(context, target_);
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
            const auto readiness = classify(context, key);
            if (readiness.state == Readiness::State::Fail) {
                fail(step, readiness.error);
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
        // Every content is ready, but the export functions refuse to read the render while a render retry of the
        // service is deferred (renderRetriesDeferred): the file would not be written. Keep waiting, up to the
        // wait limit, for the runtime to resume it.
        if (ready == total && !renderRetriesDeferred(context.processor())) {
            startWriting(context, plan, std::move(dry));
            if (writer_ != nullptr)
                setProgress(step, std::nullopt, "writing", std::nullopt, std::nullopt);
            else
                fail(step, {"IO_ERROR", "write_failed", "the target directory cannot be written", false});
            return;
        }
        if (std::chrono::steady_clock::now() - started_ >= waitLimit_) {
            fail(step, {"NOT_READY", "export_wait_timeout",
                        "analysis and rendering of the export did not settle within the wait limit", false});
            return;
        }
        setProgress(step, static_cast<double>(ready) / total, "waiting", ready, total);
    }

    /** Section 19 rule 8, in the tick that found every content ready: records the inputs and revisions and
        starts the writer on a temporary file in the target directory. */
    void startWriting(ControlOpContext& context, const ExportPlan& plan, JsonValue dry)
    {
        std::vector<ControlContentVersions> versions;
        recordedInputs_ = inputsToken(context, plan, &versions);
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
        auto& processor = context.processor();
        writer_ = std::make_unique<ExportWriter>(processor, target_.kind,
                                                 target_.kind == ExportKind::Clip ? plan.clipTrackId : target_.trackId,
                                                 target_.placementId, temporary_);
    }

    void stepWriting(ControlOpContext& context, ControlJobStep& step)
    {
        // OpenTune's export cannot be interrupted: a cancellation waits for the write and then deletes it.
        if (!writer_->done())
            return;
        const bool written = writer_->written;
        const bool renderNotReady = writer_->renderNotReady;
        const int channelCount = writer_->channelCount;
        const double sampleRateHz = writer_->sampleRateHz;
        const std::int64_t frameCount = writer_->frameCount;
        const std::string sampleFormat = writer_->sampleFormat;
        if (step.cancelRequested()) {
            finishCancelled(step);
            return;
        }
        if (!written && renderNotReady) {
            // A deferred render retry appeared after the job found every content ready, and the export function
            // could not get the render worker to settle (drainRenderWorkerForExport). Not a write failure: nothing
            // was written, and the export succeeds once the render is ready.
            fail(step, {"NOT_READY", "export_wait_timeout",
                        "the render of the export was not ready when OpenTune wrote the file", false});
            return;
        }
        if (!written) {
            AppLogger::logNoThrow("control API: export.start: OpenTune could not write the export file");
            fail(step, {"IO_ERROR", "write_failed", "OpenTune could not write the export file", false});
            return;
        }
        // Section 19 rule 8: what the file is made of must not have changed since it was recorded.
        if (inputsToken(context, buildPlan(context, target_)) != recordedInputs_) {
            fail(step, {"REVISION_CONFLICT", "changed_during_export",
                        "a content or clip of the export changed while the file was written", false});
            return;
        }
        // Section 12.3: cancellable until the final rename.
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
        lock_.release();
        step.succeed(std::move(result));
    }

    /** Sections 19 rules 2 and 3 and 20.4: the names that ifExists allows, each checked against the loaded
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
        lock_.release();
        if (error.kind != "INVALID_ARGUMENT" || !error.reason.empty())
            AppLogger::logNoThrow("control API: export.start: the export failed with " + juce::String(error.kind) + "("
                                  + juce::String(error.reason) + ")");
        step.fail(error);
    }

    void finishCancelled(ControlJobStep& step)
    {
        writer_.reset();
        removeTemporary();
        lock_.release();
        step.cancel(JsonValue(), false);
    }

    ControlOpEnvironment environment_;
    ExportParams params_;
    ExportTarget target_;
    ExportLock lock_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::milliseconds waitLimit_;
    std::map<ContentKey, ContentProgress> contentProgress_;
    std::unique_ptr<ExportWriter> writer_;
    juce::File temporary_;
    std::string recordedInputs_;
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

    // Section 19 rule 5: the same normalized params as an export job that still runs return that job,
    // before any check of state (that job holds the export lock).
    if (const auto existing = jobs->findActiveByDigest("export", digest); !existing.empty()) {
        ControlWriteEnvelope envelope(context);
        return envelope.handedToJob(context, jobs->jobRef(existing));
    }

    // Handles, then busy state (with the export lock), then the method's own checks (section 13.1 rule 4).
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
    // Section 20.1: the raw text, before any file-system access.
    const auto reason = controlCheckAbsolutePath(params.path, controlNativePathStyle());
    if (!reason.empty())
        return controlOpError("INVALID_ARGUMENT", reason, "the path is not an absolute path in the accepted form");
    if (!hasWavExtension(params.path))
        return controlOpError("INVALID_ARGUMENT", "path_bad_extension", "an export writes only .wav files");
    if (buildPlan(context, target).contents.empty())
        return controlOpError("INVALID_ARGUMENT", "nothing_to_export", "the export holds no clip with audio");
    // Section 19 rule 2 by file identity; the job checks again right before its final rename.
    if (const auto error = loadedMediaError(loadedMediaPaths(context), params.path))
        return controlOpError(error->kind, error->reason, error->detail);
    if (!fileOf(params.path).getParentDirectory().isDirectory())
        return controlOpError("IO_ERROR", "write_failed", "the directory of the target does not exist");
    // Section 19 rule 3: with ifExists error an existing target fails at once and nothing is rendered.
    if (params.ifExists == IfExists::Error && fileOf(params.path).exists())
        return controlOpError("ALREADY_EXISTS", "target_exists", "a file exists at the target");

    // The lock that the editor's export shares, held until the job ends (section 19 rule 4).
    if (!processor.tryAcquireExportLock())
        return controlOpError("BUSY", "export_in_progress", "an export is running");
    ExportLock lock(processor);
    auto job = std::make_shared<ExportJob>(context.environment(), params, target, std::move(lock));
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

void controlSetExportWaitLimitForTesting(int milliseconds)
{
    waitLimitOverrideMs() = milliseconds > 0 ? milliseconds : 0;
}

} // namespace OpenTune
