// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scenarios of export.start and the export lock, against the real processor without a window or an audio
// device. Contents get a synthetic pitch analysis committed the way OpenTune commits a finished analysis.
// Without a vocoder model OpenTune cannot render, so the scenarios drive the render cache of a content
// themselves while OpenTune's render worker is paused: they publish its blocks as a finished render would,
// or fail them as a failing vocoder would (RenderCache::claimPendingJob, completeChunkRenderWithAudio,
// completeChunkRenderFailure). Registered with the other processor scenarios in ControlProcessorTests.cpp.

#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneClipContent.h"
#include "Content/StandaloneContentRepository.h"
#include "Control/ControlDispatcher.h"
#include "Control/ControlFileIdentity.h"
#include "Control/ControlJobs.h"
#include "Control/ControlPathPolicy.h"
#include "Control/Ops/ExportOps.h"
#include "Control/Ops/ImportOps.h"
#include "Inference/RenderCache.h"
#include "Render/ContentRenderService.h"
#include "Render/PlaybackReadSource.h"
#include "Runtime/ProcessRenderRuntime.h"
#include "Utils/AppPreferences.h"
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "Utils/PitchUtils.h"
#include "Utils/TimeGrid.h"
#include "Utils/TuningConfig.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "ControlWinAcl.h" // after the JUCE headers: needs windows.h

namespace OpenTune {

/** The friend of ProcessRenderRuntime that lets a test put a deferred render retry into the runtime without a
    vocoder, as OpenTune's own RenderRuntimeDeferredRetryTests does (the same member names are used there). */
struct ProcessRenderRuntimeTestAccess
{
    static void setReconfiguring(ProcessRenderRuntime& runtime, bool value)
    {
        std::lock_guard<std::mutex> lock(runtime.vocoderMutex_);
        runtime.vocoderReconfiguring_ = value;
    }

    static void defer(ProcessRenderRuntime& runtime, std::shared_ptr<ContentRenderService> service, RenderJob job,
                      ProcessRenderRuntime::CompletionContext completion)
    {
        runtime.deferOrRequeue(std::move(service), std::move(job), std::move(completion), "test retry",
                               ProcessRenderRuntime::kDeferredSnapshotBudgetBytes);
    }
};

namespace ControlTest {

namespace {

/** Sample rate of the test audio (OpenTune stores audio at 44.1 kHz). */
constexpr double kAudioSampleRate = 44100.0;
/** Geometry of the synthetic pitch analysis, the one the F0 extractor produces: 16 kHz, hop 160. */
constexpr double kF0SampleRate = 16000.0;
constexpr int kF0HopSize = 160;
/** The pitch of the test tone (A3) and its unvoiced tail. */
constexpr float kToneHz = 220.0f;
constexpr double kUnvoicedTailSec = 0.3;
constexpr double kSecTolerance = 0.01;
/** Bound of a client exchange that includes an export job. */
constexpr int kJobExchangeMs = 60000;
/** The wait limit of the export jobs of the NOT_READY case. */
constexpr int kShortWaitLimitMs = 300;
/** The wait limit of the export of contents with render blocks pending: twice kControlExportStuckBlocksMs (the time
    the job sees blocks no worker will claim before it takes them for stuck ones), so that the wait goes past the
    moment of that judgement and still ends at the limit when the blocks are claimable. */
constexpr int kPendingBlocksWaitLimitMs = 2 * kControlExportStuckBlocksMs;
/** The wait limit of exports whose contents are ready within a few ticks of the job, or once the scenario published
    their render: one that waits for a render that never comes fails with NOT_READY at this limit instead of at 120 s.
    Shorter than kReadTimeoutMs, so that the job.get that waits for the job is answered either way. */
constexpr int kPromptWaitLimitMs = 4000;
static_assert(kPromptWaitLimitMs > kControlExportStuckBlocksMs,
              "the was-corrected case waits with kPromptWaitLimitMs for the job to take the pending blocks for stuck "
              "ones");
/** How long a scenario waits for a job it drives to reach a state. */
constexpr int kDriveTimeoutMs = 20000;
/** Level of the synthetic render, which differs from the tone so that a file shows which one it holds. */
constexpr float kRenderLevel = 0.05f;

enum class Analysis {
    Ready,
    Running,
    Failed,
    Unvoiced,
    /** Ready, but the analysis found no voiced frame: its F0 is zero throughout, as for a silent file. */
    ReadySilent
};

struct TestClip
{
    ContentKey key;
    std::uint64_t placementId = 0;
    int trackId = 0;
    double startSec = 0.0;
    double durationSec = 0.0;
    std::string sourcePath;
};

juce::File juceFileOf(const std::string& utf8Path)
{
    return juce::File(juce::String::fromUTF8(utf8Path.c_str()));
}

/** A new, empty directory in the test's data directory. */
std::filesystem::path exportDirectory(const std::string& name)
{
    const auto directory = isolatedDataDirectory() / "export" / std::filesystem::u8path(name);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

/** The names of the files in a directory, sorted. */
std::vector<std::string> filesIn(const std::filesystem::path& directory)
{
    std::vector<std::string> names;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ignored))
        names.push_back(entry.path().filename().u8string());
    std::sort(names.begin(), names.end());
    return names;
}

/** A mono 16-bit WAV file of a tone (or of silence); returns its absolute path (UTF-8). */
std::string writeWav(const std::filesystem::path& directory, const std::string& name, double durationSec,
                     bool voiced = true)
{
    const auto samples = static_cast<int>(std::llround(durationSec * kAudioSampleRate));
    juce::AudioBuffer<float> audio(1, samples);
    audio.clear();
    if (voiced)
        for (int i = 0; i < samples; ++i)
            audio.setSample(0, i,
                            0.25f
                                * static_cast<float>(
                                    std::sin(2.0 * juce::MathConstants<double>::pi * kToneHz * i / kAudioSampleRate)));
    const auto path = (directory / std::filesystem::u8path(name + ".wav")).u8string();
    const auto file = juceFileOf(path);
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream().release());
    juce::WavAudioFormat wav;
    const auto options =
        juce::AudioFormatWriterOptions{}.withSampleRate(kAudioSampleRate).withNumChannels(1).withBitsPerSample(16);
    auto writer = stream != nullptr ? wav.createWriterFor(stream, options) : nullptr;
    CONTROL_CHECK(writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, samples));
    return path;
}

/** The header of a WAV file. */
struct WavInfo
{
    int channels = 0;
    double sampleRateHz = 0.0;
    std::int64_t frames = 0;
    bool floatingPoint = false;
    int bits = 0;
};

std::optional<WavInfo> wavInfo(const std::string& path)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juceFileOf(path)));
    if (reader == nullptr)
        return std::nullopt;
    WavInfo info;
    info.channels = static_cast<int>(reader->numChannels);
    info.sampleRateHz = reader->sampleRate;
    info.frames = reader->lengthInSamples;
    info.floatingPoint = reader->usesFloatingPointData;
    info.bits = static_cast<int>(reader->bitsPerSample);
    return info;
}

/** F0 of a test tone: A3, an unvoiced tail; with silent no frame is voiced. */
std::shared_ptr<PitchCurve> syntheticCurve(double durationSec, bool silent = false)
{
    const auto frames = static_cast<std::size_t>(std::llround(durationSec * kF0SampleRate / kF0HopSize));
    const auto tail = static_cast<std::size_t>(std::llround(kUnvoicedTailSec * kF0SampleRate / kF0HopSize));
    std::vector<float> f0(frames, silent ? 0.0f : kToneHz);
    for (auto frame = frames > tail ? frames - tail : 0; frame < frames; ++frame)
        f0[frame] = 0.0f;
    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(kF0HopSize);
    curve->setSampleRate(kF0SampleRate);
    curve->setOriginalF0(f0);
    return curve;
}

/** The synthetic curve of a test tone with a correction (a semitone up) over its first tenth of a second, so that the
    content has something to render: OpenTune's render marks a block without correction blank, and the export lists
    a content without any as nothingToRender without a render. */
std::shared_ptr<PitchCurve> correctedCurve(double durationSec)
{
    auto curve = syntheticCurve(durationSec);
    const int frames = static_cast<int>(curve->size());
    const int end = std::min(frames, static_cast<int>(0.1 * kF0SampleRate / kF0HopSize));
    if (end > 0)
        curve->replaceCorrectionSegments({PitchCorrectionSegment(
            0, end, std::vector<float>(static_cast<std::size_t>(end), kToneHz * std::pow(2.0f, 1.0f / 12.0f)),
            PitchCorrectionSegment::Source::HandDraw)});
    return curve;
}

/**
 * Imports a clip of a tone (silence for Unvoiced) the way the editor imports (prepareImport, then
 * commitPreparedImportAsPlacement) and gives its content the analysis state: Ready with a synthetic curve
 * (StandaloneClipContent::applyOriginalF0, as the processor's completion of a finished analysis), Running,
 * or Failed with the message OpenTune stores (the one for an analysis without voiced frames reads f0_empty).
 */
TestClip addClip(OpenTuneAudioProcessor& processor, const std::filesystem::path& directory, const std::string& name,
                 int trackId, double startSec, double durationSec, Analysis analysis)
{
    TestClip clip;
    clip.trackId = trackId;
    clip.startSec = startSec;
    clip.durationSec = durationSec;
    clip.sourcePath =
        writeWav(directory, name, durationSec, analysis != Analysis::Unvoiced && analysis != Analysis::ReadySilent);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juceFileOf(clip.sourcePath)));
    if (!CONTROL_CHECK(reader != nullptr))
        return clip;
    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
    OpenTuneAudioProcessor::PreparedImport prepared;
    if (!CONTROL_CHECK(processor.prepareImport(std::move(audio), reader->sampleRate, juce::String(name),
                                               juce::String::fromUTF8(clip.sourcePath.c_str()), prepared)))
        return clip;
    OpenTuneAudioProcessor::ImportPlacement placement;
    placement.trackId = trackId;
    placement.timelineStartSeconds = startSec;
    const auto committed = processor.commitPreparedImportAsPlacement(std::move(prepared), placement);
    if (!CONTROL_CHECK(committed.isValid()))
        return clip;
    clip.key = committed.contentKey;
    clip.placementId = committed.placementId;
    switch (analysis) {
        case Analysis::Ready:
        case Analysis::ReadySilent: {
            auto* owner = processor.getStandaloneContentRepository()->findClip(clip.key);
            if (CONTROL_CHECK(owner != nullptr))
                owner->applyOriginalF0(syntheticCurve(durationSec, analysis == Analysis::ReadySilent));
            CONTROL_CHECK(processor.setContentOriginalF0State(clip.key, OriginalF0State::Ready));
            break;
        }
        case Analysis::Running:
            CONTROL_CHECK(processor.setContentOriginalF0State(clip.key, OriginalF0State::Extracting));
            break;
        case Analysis::Failed:
        case Analysis::Unvoiced:
            CONTROL_CHECK(failAnalysis(processor, clip.key, analysis == Analysis::Unvoiced));
            break;
    }
    return clip;
}

/** Notes with OpenTune's default parameters (startSec, endSec, targetMidi), as the piano roll stores them. */
bool setNotes(OpenTuneAudioProcessor& processor, const ContentKey& key, const std::vector<std::array<double, 3>>& specs)
{
    std::vector<Note> notes;
    for (const auto& spec : specs) {
        Note note;
        note.startTime = spec[0];
        note.endTime = spec[1];
        note.pitch = PitchUtils::midiToFreq(static_cast<float>(spec[2]), TuningConfig::currentTuningHz());
        note.originalPitch = kToneHz;
        note.isVoiced = true;
        notes.push_back(note);
    }
    return processor.replaceContentNotesForFullMutation(key, std::move(notes));
}

/** Completes every pending render block of a cache as a finished render would (with a quiet constant), or
    as a failing vocoder would. OpenTune's render worker must be paused. Returns how many it completed. */
int completePendingRender(RenderCache& cache, bool succeed, float level = kRenderLevel)
{
    int completed = 0;
    for (const auto& job : cache.getPendingJobs()) {
        if (!cache.claimPendingJob(job))
            continue;
        if (succeed) {
            std::vector<float> audio(static_cast<std::size_t>(job.endSampleExclusive - job.startSample), level);
            if (cache.completeChunkRenderWithAudio(job.startSample, job.endSampleExclusive, std::move(audio),
                                                   job.targetRevision)
                == RenderCache::ChunkRenderResult::Published)
                ++completed;
        } else if (cache.completeChunkRenderFailure(job.startSample, job.targetRevision)) {
            ++completed;
        }
    }
    return completed;
}

/**
 * Makes the render of a content current without a vocoder model: pauses OpenTune's render worker,
 * requests the render (OpenTuneAudioProcessor::requestFullContentRender), publishes every block, and
 * resumes the worker, which skips the queued blocks it can no longer claim.
 */
bool settleRender(OpenTuneAudioProcessor& processor, const ContentKey& key, float level = kRenderLevel)
{
    auto* service = processor.getContentRenderService();
    if (service == nullptr)
        return false;
    service->pauseRenderWorker();
    processor.requestFullContentRender(key);
    const auto cache = service->getRenderCache(key);
    const bool published = cache != nullptr && completePendingRender(*cache, true, level) > 0;
    service->resumeRenderWorker();
    return published && cache->isCanonicalSettled();
}

/** Registers a bridge with the service for the life of this object. */
class ScopedBridge
{
public:
    ScopedBridge(ControlService& service, ControlUiBridge& bridge) : service_(service)
    {
        service_.setUiBridge(&bridge);
    }
    ~ScopedBridge() { service_.setUiBridge(nullptr); }

    ScopedBridge(const ScopedBridge&) = delete;
    ScopedBridge& operator=(const ScopedBridge&) = delete;

private:
    ControlService& service_;
};

/** Restores the export wait limit when it goes out of scope. */
struct ScopedWaitLimit
{
    explicit ScopedWaitLimit(int milliseconds) { controlSetExportWaitLimitForTesting(milliseconds); }
    ~ScopedWaitLimit() { controlSetExportWaitLimitForTesting(0); }
    ScopedWaitLimit(const ScopedWaitLimit&) = delete;
    ScopedWaitLimit& operator=(const ScopedWaitLimit&) = delete;
};

JsonValue json(const std::string& text)
{
    auto parsed = parseJson(text);
    CONTROL_CHECK(parsed.ok);
    return parsed.value;
}

std::string quoted(const std::string& text)
{
    std::string out;
    appendJsonQuoted(out, text);
    return out;
}

bool near(const JsonValue& value, double expected, double tolerance = kSecTolerance)
{
    return value.isNumber() && std::abs(value.asDouble() - expected) <= tolerance;
}

/** The handles of the instance for a placement or a content. */
struct Handles
{
    std::string instanceShort;
    std::uint64_t epoch = 0;

    std::string placement(std::uint64_t id) const
    {
        return "p:" + instanceShort + ":" + std::to_string(epoch) + ":" + std::to_string(id);
    }
    std::string content(const ContentKey& key) const
    {
        return "c:" + instanceShort + ":" + std::to_string(epoch) + ":" + std::to_string(key.objectId);
    }
};

Handles handlesOf(ApiSession& api, OpenTuneAudioProcessor& processor)
{
    Handles handles;
    handles.instanceShort = memberOf(api.discovery(), "instanceId").text().substr(0, 8);
    handles.epoch = processor.getStandaloneContentRepository()->getContentEpoch();
    return handles;
}

/** The revision token of a content as project.get reports it (asOf.contents). */
std::string revisionOf(ApiSession& api, const std::string& contentId)
{
    const auto project = api.result("project.get");
    const auto& tokens = memberOf(memberOf(project, "asOf"), "contents");
    for (std::size_t i = 0; i < tokens.size(); ++i)
        if (memberOf(tokens.at(i), "contentId").text() == contentId)
            return memberOf(tokens.at(i), "revision").text();
    return {};
}

std::string clipParams(const std::string& placementId, const std::string& path, const std::string& extra = {})
{
    return R"({"kind":"clip","target":{"placementId":)" + quoted(placementId) + R"(},"path":)" + quoted(path) + extra
           + "}";
}

/** Starts an export; returns its job id, or empty when the call did not hand the work to a job. */
std::string startExport(ApiSession& api, const std::string& params)
{
    const auto start = api.result("export.start", json(params));
    const auto& job = memberOf(start, "job");
    if (!CONTROL_CHECK(job.isObject() && memberOf(job, "kind").text() == "export"))
        return {};
    // The call changes no project state (methods/export.start.schema.json).
    CONTROL_CHECK(!memberOf(start, "applied").asBoolean() && !memberOf(start, "noOp").asBoolean()
                  && memberOf(start, "undo").isNull() && memberOf(start, "changes").size() == 0
                  && memberOf(start, "warnings").size() == 0);
    CONTROL_CHECK(toJsonString(memberOf(start, "revisionDiff"))
                  == R"({"arrangement":null,"trackState":null,"contents":[]})");
    return memberOf(job, "jobId").text();
}

/** The job once terminal (job.get with the longest hold). */
JsonValue waitJob(ApiSession& api, const std::string& jobId)
{
    return memberOf(api.result("job.get", json(R"({"jobId":)" + quoted(jobId) + R"(,"waitMs":30000})")), "job");
}

/** Starts an export and returns its job once terminal. */
JsonValue exportAndWait(ApiSession& api, const std::string& params)
{
    const auto jobId = startExport(api, params);
    return jobId.empty() ? JsonValue() : waitJob(api, jobId);
}

/** The job straight from the registry (any thread), without a request. */
JsonValue jobOf(ControlService& service, const std::string& jobId)
{
    JsonValue job;
    service.jobs()->get(jobId, job);
    return job;
}

bool isTerminal(const JsonValue& job)
{
    const auto state = memberOf(job, "state").text();
    return state == "succeeded" || state == "failed" || state == "cancelled";
}

/** True when the job runs and waits for its contents. A job's first step comes from a timer, and how late that timer
    fires on a loaded machine has no short bound, so tests pump until this holds instead of assuming a time. */
bool isWaiting(const JsonValue& job)
{
    return memberOf(job, "state").text() == "running"
           && memberOf(memberOf(job, "progress"), "phase").text() == "waiting";
}

/** True when the job failed with that error kind and reason, with executed false (the target untouched). */
bool failedWith(const JsonValue& job, const std::string& kind, const std::string& reason)
{
    const auto& data = memberOf(memberOf(job, "error"), "data");
    const bool ok = memberOf(job, "state").text() == "failed" && memberOf(data, "kind").text() == kind
                    && (reason.empty() ? data.find("reason") == nullptr : memberOf(data, "reason").text() == reason)
                    && memberOf(data, "executed").isBoolean() && !memberOf(data, "executed").asBoolean();
    if (!ok)
        note("expected a job failed with " + kind + "(" + reason + "), got " + toJsonString(job));
    return ok;
}

const JsonValue& resultOf(const JsonValue& job)
{
    return memberOf(job, "result");
}

/** The result members that every export reports, checked against the expected format. */
void checkFormat(const JsonValue& result, int channels, double durationSec, double startTimelineSec)
{
    CONTROL_CHECK(near(memberOf(result, "sampleRateHz"), kAudioSampleRate, 0.0));
    CONTROL_CHECK(memberOf(result, "channelCount").asDouble() == channels);
    CONTROL_CHECK(memberOf(result, "sampleFormat").text() == "float32");
    CONTROL_CHECK(near(memberOf(result, "durationSec"), durationSec));
    CONTROL_CHECK(near(memberOf(result, "startTimelineSec"), startTimelineSec, 1e-6));
    const auto& processing = memberOf(result, "processing");
    CONTROL_CHECK(processing.find("vocoderWeight") != nullptr && memberOf(processing, "hybridMode").isBoolean()
                  && processing.find("backend") != nullptr && memberOf(processing, "tuningHz").isNumber());
    const auto info = wavInfo(memberOf(result, "path").text());
    if (CONTROL_CHECK(info.has_value())) {
        CONTROL_CHECK(info->channels == channels && info->sampleRateHz == kAudioSampleRate);
        CONTROL_CHECK(info->floatingPoint && info->bits == 32);
        CONTROL_CHECK(std::abs(static_cast<double>(info->frames) / kAudioSampleRate - durationSec) <= kSecTolerance);
    }
}

std::string dryJson(const std::string& contentId, const char* reason)
{
    return R"([{"contentId":)" + quoted(contentId) + R"(,"reason":")" + reason + R"("}])";
}

} // namespace

// ---- export.start: results, checks and the export lock ------------------------------------------

void scenarioExport()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    bridge.visibleTracks = 4;
    ScopedBridge registered(service, bridge);
    ApiSession api;
    if (!api.open(service))
        return;
    auto* arrangement = processor->getStandaloneArrangement();
    const auto media = exportDirectory("media");
    const auto out = exportDirectory("out");
    const auto outPath = [&out](const std::string& name) { return (out / name).u8string(); };

    // A clip with a current render, an unvoiced clip after it on track 0, an unvoiced clip on a muted
    // track 1 and one on track 2.
    const auto wet = addClip(*processor, media, "wet", 0, 1.0, 2.0, Analysis::Ready);
    CONTROL_CHECK(settleRender(*processor, wet.key));
    const auto dry = addClip(*processor, media, "dry", 0, 4.0, 1.5, Analysis::Unvoiced);
    const auto muted = addClip(*processor, media, "muted", 1, 0.0, 1.0, Analysis::Unvoiced);
    const auto other = addClip(*processor, media, "other", 2, 0.5, 1.0, Analysis::Unvoiced);
    CONTROL_CHECK(arrangement->setTrackMuted(1, true));
    const auto handles = handlesOf(api, *processor);
    const auto wetId = handles.content(wet.key);
    const auto dryId = handles.content(dry.key);

    // 1. A clip with a current render: no dry content; the file holds the clip from its start on the
    //    timeline, mono, and the revision the job recorded.
    {
        const auto job = exportAndWait(api, clipParams(handles.placement(wet.placementId), outPath("wet.wav")));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded" && memberOf(job, "error").isNull()
                      && !memberOf(job, "cancellable").asBoolean());
        const auto& result = resultOf(job);
        CONTROL_CHECK(memberOf(result, "path").text() == outPath("wet.wav"));
        checkFormat(result, 1, 2.0, 1.0);
        CONTROL_CHECK(toJsonString(memberOf(result, "dryContents")) == "[]");
        CONTROL_CHECK(toJsonString(memberOf(result, "contents"))
                      == R"([{"contentId":)" + quoted(wetId) + R"(,"revision":)" + quoted(revisionOf(api, wetId))
                             + "}]");
        // Nothing but the file: the temporary file was moved into place.
        CONTROL_CHECK(filesIn(out) == std::vector<std::string>{"wet.wav"});
        CONTROL_CHECK(!processor->isExportLockHeld());
    }
    // 2. An unvoiced clip is exported uncorrected and listed.
    {
        const auto job = exportAndWait(api, clipParams(handles.placement(dry.placementId), outPath("dry.wav")));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        checkFormat(resultOf(job), 1, 1.5, 4.0);
        CONTROL_CHECK(toJsonString(memberOf(resultOf(job), "dryContents")) == dryJson(dryId, "unvoiced"));
    }
    // 3. A track from timeline 0 to the end of its last clip.
    {
        const auto job = exportAndWait(api, R"({"kind":"track","target":{"trackId":0},"path":)"
                                                + quoted(outPath("track.wav")) + "}");
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = resultOf(job);
        checkFormat(result, 1, 5.5, 0.0);
        CONTROL_CHECK(memberOf(result, "contents").size() == 2
                      && memberOf(memberOf(result, "contents").at(0), "contentId").text() == wetId
                      && memberOf(memberOf(result, "contents").at(1), "contentId").text() == dryId);
        CONTROL_CHECK(toJsonString(memberOf(result, "dryContents")) == dryJson(dryId, "unvoiced"));
    }
    // 4. The master mix in stereo, without the muted track.
    {
        const auto job = exportAndWait(api, R"({"kind":"master","path":)" + quoted(outPath("master.wav")) + "}");
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = resultOf(job);
        checkFormat(result, 2, 5.5, 0.0);
        std::set<std::string> contents;
        for (std::size_t i = 0; i < memberOf(result, "contents").size(); ++i)
            contents.insert(memberOf(memberOf(result, "contents").at(i), "contentId").text());
        CONTROL_CHECK((contents == std::set<std::string>{wetId, dryId, handles.content(other.key)}));
        CONTROL_CHECK(contents.count(handles.content(muted.key)) == 0);
        CONTROL_CHECK(memberOf(result, "dryContents").size() == 2);
    }

    // 5. ifExists: error answers at once and starts no job; rename takes the next free name; overwrite
    //    replaces the file.
    {
        const auto retained = service.jobs()->retainedCount();
        CONTROL_CHECK(api.fails("export.start",
                                json(clipParams(handles.placement(wet.placementId), outPath("wet.wav"))),
                                "ALREADY_EXISTS", "target_exists"));
        CONTROL_CHECK(service.jobs()->retainedCount() == retained);
        for (const char* expected : {"wet (2).wav", "wet (3).wav"}) {
            const auto job = exportAndWait(
                api, clipParams(handles.placement(wet.placementId), outPath("wet.wav"), R"(,"ifExists":"rename")"));
            CONTROL_CHECK(memberOf(job, "state").text() == "succeeded"
                          && memberOf(resultOf(job), "path").text() == outPath(expected));
        }
        CONTROL_CHECK(juceFileOf(outPath("note.wav")).replaceWithText("not audio"));
        const auto job = exportAndWait(
            api, clipParams(handles.placement(dry.placementId), outPath("note.wav"), R"(,"ifExists":"overwrite")"));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        CONTROL_CHECK(wavInfo(outPath("note.wav")).has_value());
        CONTROL_CHECK((filesIn(out)
                       == std::vector<std::string>{"dry.wav", "master.wav", "note.wav", "track.wav", "wet (2).wav",
                                                   "wet (3).wav", "wet.wav"}));
    }

    // 6. Checks before the job: nothing starts, nothing on the file system is touched.
    {
        const auto retained = service.jobs()->retainedCount();
        const auto wetPlacement = handles.placement(wet.placementId);
        CONTROL_CHECK(api.fails("export.start", json(clipParams(wetPlacement, outPath("x.mp3"))), "INVALID_ARGUMENT",
                                "path_bad_extension"));
        CONTROL_CHECK(!juceFileOf(outPath("x.mp3")).exists());
        CONTROL_CHECK(api.fails("export.start", json(clipParams(wetPlacement, "relative.wav")), "INVALID_ARGUMENT",
                                "path_not_absolute"));
        if (controlNativePathStyle() == ControlPathStyle::Windows) {
            CONTROL_CHECK(api.fails("export.start", json(clipParams(wetPlacement, R"(\\server\share\x.wav)")),
                                    "INVALID_ARGUMENT", "path_unc"));
            CONTROL_CHECK(api.fails("export.start", json(clipParams(wetPlacement, outPath("NUL.wav"))),
                                    "INVALID_ARGUMENT", "path_reserved_name"));
        }
        // Loaded media by file identity, whatever ifExists says: the source itself, the same file spelled
        // otherwise, and a hard link to it.
        const auto sourceSize = juceFileOf(wet.sourcePath).getSize();
        CONTROL_CHECK(api.fails("export.start",
                                json(clipParams(wetPlacement, wet.sourcePath, R"(,"ifExists":"overwrite")")),
                                "INVALID_ARGUMENT", "target_is_loaded_media"));
        if (controlNativePathStyle() == ControlPathStyle::Windows) {
            const auto upper = (media / "WET.WAV").u8string();
            CONTROL_CHECK(api.fails("export.start", json(clipParams(wetPlacement, upper, R"(,"ifExists":"overwrite")")),
                                    "INVALID_ARGUMENT", "target_is_loaded_media"));
        }
        const auto link = (out / "link.wav").u8string();
        std::error_code linkError;
        std::filesystem::create_hard_link(std::filesystem::u8path(wet.sourcePath), std::filesystem::u8path(link),
                                          linkError);
        if (CONTROL_CHECK(!linkError))
            CONTROL_CHECK(api.fails("export.start", json(clipParams(wetPlacement, link, R"(,"ifExists":"rename")")),
                                    "INVALID_ARGUMENT", "target_is_loaded_media"));
        CONTROL_CHECK(juceFileOf(wet.sourcePath).getSize() == sourceSize);
        CONTROL_CHECK(api.fails("export.start", json(clipParams(wetPlacement, (out / "missing" / "x.wav").u8string())),
                                "IO_ERROR", "write_failed"));
        // Handles and params.
        CONTROL_CHECK(api.fails("export.start", json(clipParams(handles.content(wet.key), outPath("y.wav"))),
                                "INVALID_ARGUMENT", "invalid_params"));
        CONTROL_CHECK(api.fails(
            "export.start", json(R"({"kind":"master","target":{"trackId":0},"path":)" + quoted(outPath("y.wav")) + "}"),
            "INVALID_ARGUMENT", "invalid_params"));
        CONTROL_CHECK(api.fails("export.start", json(R"({"kind":"clip","path":)" + quoted(outPath("y.wav")) + "}"),
                                "INVALID_ARGUMENT", "invalid_params"));
        CONTROL_CHECK(api.fails("export.start",
                                json(R"({"kind":"master","path":)" + quoted(outPath("y.wav")) + R"(,"format":"wav"})"),
                                "INVALID_ARGUMENT", "invalid_params"));
        CONTROL_CHECK(api.fails("export.start",
                                json(clipParams(handles.placement(wet.placementId + 1000), outPath("y.wav"))),
                                "NOT_FOUND", "unknown_handle"));
        CONTROL_CHECK(
            api.fails("export.start",
                      json(R"({"kind":"track","target":{"trackId":)" + std::to_string(arrangement->getNumTracks())
                           + R"(},"path":)" + quoted(outPath("y.wav")) + "}"),
                      "NOT_FOUND", "unknown_handle"));
        // A track without clips: OpenTune exports nothing for it (INVALID_ARGUMENT(nothing_to_export), not retryable).
        {
            const auto response = api.request("export.start", json(R"({"kind":"track","target":{"trackId":5},"path":)"
                                                                   + quoted(outPath("y.wav")) + "}"));
            const auto* data = errorData(response);
            CONTROL_CHECK(data != nullptr && memberOf(*data, "kind").text() == "INVALID_ARGUMENT"
                          && memberOf(*data, "reason").text() == "nothing_to_export"
                          && !memberOf(*data, "retryable").asBoolean());
        }
        // A master export in which no clip is heard: every track muted, or a soloed track without clips.
        {
            const auto masterParams = json(R"({"kind":"master","path":)" + quoted(outPath("y.wav")) + "}");
            std::vector<bool> wasMuted;
            for (int t = 0; t < arrangement->getNumTracks(); ++t) {
                wasMuted.push_back(arrangement->isTrackMuted(t));
                CONTROL_CHECK(arrangement->setTrackMuted(t, true));
            }
            CONTROL_CHECK(api.fails("export.start", masterParams, "INVALID_ARGUMENT", "nothing_to_export"));
            for (int t = 0; t < arrangement->getNumTracks(); ++t)
                CONTROL_CHECK(arrangement->setTrackMuted(t, wasMuted[static_cast<std::size_t>(t)]));
            // Solo hides every other track: a soloed track without clips leaves nothing to hear.
            CONTROL_CHECK(arrangement->setTrackSolo(5, true));
            CONTROL_CHECK(api.fails("export.start", masterParams, "INVALID_ARGUMENT", "nothing_to_export"));
            CONTROL_CHECK(arrangement->setTrackSolo(5, false));
        }
        CONTROL_CHECK(service.jobs()->retainedCount() == retained);
        CONTROL_CHECK(!juceFileOf(outPath("y.wav")).exists());
    }

    // 7. The name that rename uses is checked against the loaded media right before the final rename: a
    //    clip's source file named "<stem> (2).wav" is never written; the job fails and nothing changes.
    {
        const auto loop = addClip(*processor, media, "loop (2)", 3, 0.0, 1.0, Analysis::Unvoiced);
        const auto loopSize = juceFileOf(loop.sourcePath).getSize();
        CONTROL_CHECK(juceFileOf((media / "loop.wav").u8string()).replaceWithText("taken"));
        const auto job = exportAndWait(api, clipParams(handles.placement(dry.placementId),
                                                       (media / "loop.wav").u8string(), R"(,"ifExists":"rename")"));
        CONTROL_CHECK(failedWith(job, "INVALID_ARGUMENT", "target_is_loaded_media"));
        CONTROL_CHECK(juceFileOf(loop.sourcePath).getSize() == loopSize
                      && juceFileOf((media / "loop.wav").u8string()).loadFileAsString() == "taken");
        CONTROL_CHECK(!processor->isExportLockHeld());
    }

    // 8. The export lock that the UI shares: held by the editor's export, export.start and every other write
    //    are refused; reads and job.get are not.
    {
        CONTROL_CHECK(processor->tryAcquireExportLock());
        CONTROL_CHECK(api.fails("export.start", json(R"({"kind":"master","path":)" + quoted(outPath("busy.wav")) + "}"),
                                "BUSY", "export_in_progress"));
        CONTROL_CHECK(api.fails("import.start",
                                json(R"({"items":[{"path":)" + quoted(wet.sourcePath) + R"(}],"layout":"single"})"),
                                "BUSY", "export_in_progress"));
        CONTROL_CHECK(api.result("project.get").isObject());
        processor->releaseExportLock();
        // The editor's own flag and its other busy conditions.
        bridge.busy.exportRunning = true;
        CONTROL_CHECK(api.fails("export.start", json(R"({"kind":"master","path":)" + quoted(outPath("busy.wav")) + "}"),
                                "BUSY", "export_in_progress"));
        bridge.busy = {};
        bridge.busy.pointerDown = true;
        CONTROL_CHECK(api.fails("export.start", json(R"({"kind":"master","path":)" + quoted(outPath("busy.wav")) + "}"),
                                "BUSY", "pointer_down"));
        bridge.busy = {};
    }

    // 9. Two exports: while the first waits for an analysis, a second one is refused, the same params
    //    return the same job, and other writes are refused; after its cancellation nothing is busy.
    {
        const auto pending = addClip(*processor, media, "pending", 4, 0.0, 1.0, Analysis::Running);
        const auto params = clipParams(handles.placement(pending.placementId), outPath("pending.wav"));
        const auto jobId = startExport(api, params);
        CONTROL_CHECK(processor->isExportLockHeld());
        CONTROL_CHECK(pumpMessagesUntil(
            [&] { return memberOf(memberOf(jobOf(service, jobId), "progress"), "phase").text() == "waiting"; },
            kDriveTimeoutMs));
        CONTROL_CHECK(api.fails("export.start",
                                json(R"({"kind":"master","path":)" + quoted(outPath("second.wav")) + "}"), "BUSY",
                                "export_in_progress"));
        CONTROL_CHECK(memberOf(memberOf(api.result("export.start", json(params)), "job"), "jobId").text() == jobId);
        CONTROL_CHECK(api.fails("import.start",
                                json(R"({"items":[{"path":)" + quoted(wet.sourcePath) + R"(}],"layout":"single"})"),
                                "BUSY", "export_in_progress"));
        const auto running = memberOf(api.result("job.get", json(R"({"jobId":)" + quoted(jobId) + "}")), "job");
        CONTROL_CHECK(memberOf(running, "state").text() == "running" && memberOf(running, "cancellable").asBoolean());
        CONTROL_CHECK(memberOf(memberOf(running, "progress"), "phase").text() == "waiting");
        CONTROL_CHECK(memberOf(api.result("job.cancel", json(R"({"jobId":)" + quoted(jobId) + "}")), "cancelRequested")
                          .asBoolean());
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(memberOf(job, "state").text() == "cancelled");
        const auto& data = memberOf(memberOf(job, "error"), "data");
        CONTROL_CHECK(memberOf(data, "kind").text() == "CANCELLED" && !memberOf(data, "executed").asBoolean());
        CONTROL_CHECK(!juceFileOf(outPath("pending.wav")).exists() && !processor->isExportLockHeld());
        const auto again = exportAndWait(api, clipParams(handles.placement(dry.placementId), outPath("after.wav")));
        CONTROL_CHECK(memberOf(again, "state").text() == "succeeded");
    }
    const auto dispatcher = service.dispatcher();
    CONTROL_CHECK(dispatcher != nullptr && dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

// ---- The failures of an export job ----------------------------------------------------------------

void scenarioExportFailures()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    ApiSession api;
    if (!api.open(service))
        return;
    auto* renderService = processor->getContentRenderService();
    if (!CONTROL_CHECK(renderService != nullptr))
        return;
    const auto media = exportDirectory("failure-media");
    const auto out = exportDirectory("failure-out");
    const auto outPath = [&out](const std::string& name) { return (out / name).u8string(); };
    const auto failed = addClip(*processor, media, "failed", 0, 0.0, 1.0, Analysis::Failed);
    const auto handles = handlesOf(api, *processor);

    // ANALYSIS_FAILED without allowDry; with it the content is exported uncorrected.
    {
        const auto job = exportAndWait(api, clipParams(handles.placement(failed.placementId), outPath("failed.wav")));
        CONTROL_CHECK(failedWith(job, "ANALYSIS_FAILED", "f0_failed"));
        CONTROL_CHECK(memberOf(job, "result").isNull() && !juceFileOf(outPath("failed.wav")).exists());
        const auto dry = exportAndWait(
            api, clipParams(handles.placement(failed.placementId), outPath("failed.wav"), R"(,"allowDry":true)"));
        CONTROL_CHECK(memberOf(dry, "state").text() == "succeeded");
        CONTROL_CHECK(toJsonString(memberOf(resultOf(dry), "dryContents"))
                      == dryJson(handles.content(failed.key), "analysisFailed"));
    }

    // RENDER_FAILED: every render block fails, also after the job asked for the render once more. The content has a
    // correction, so it has something to render, and no render block yet: the job asks for its render.
    {
        const auto clip = addClip(*processor, media, "unrendered", 1, 0.0, 1.0, Analysis::Ready);
        if (auto* owner = processor->getStandaloneContentRepository()->findClip(clip.key);
            CONTROL_CHECK(owner != nullptr))
            owner->applyOriginalF0(correctedCurve(clip.durationSec));
        renderService->pauseRenderWorker();
        const auto jobId = startExport(api, clipParams(handles.placement(clip.placementId), outPath("render.wav")));
        int failedBlocks = 0;
        CONTROL_CHECK(pumpMessagesUntil(
            [&] {
                if (const auto cache = renderService->getRenderCache(clip.key))
                    failedBlocks += completePendingRender(*cache, false);
                return isTerminal(jobOf(service, jobId));
            },
            kDriveTimeoutMs));
        renderService->resumeRenderWorker();
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(failedWith(job, "RENDER_FAILED", "render_chunk_failed"));
        CONTROL_CHECK(!memberOf(memberOf(memberOf(job, "error"), "data"), "retryable").asBoolean());
        // The first request and the one retry, each with at least one block.
        CONTROL_CHECK(failedBlocks >= 2);
        CONTROL_CHECK(filesIn(out) == std::vector<std::string>{"failed.wav"} && !processor->isExportLockHeld());
    }

    // A block fails while the other blocks of the content are still pending: the job asks for the failed block again
    // only once they are rendered. Asking while they are pending, at the same revision, would leave them pending but
    // unqueued (upstream RenderCache::reconcileFullPlanAndRequest), never rendered, and the export would wait until
    // its limit. Here every block renders and the export succeeds with the render.
    {
        const auto clip = addClip(*processor, media, "retried", 6, 0.0, 20.0, Analysis::Ready);
        if (auto* owner = processor->getStandaloneContentRepository()->findClip(clip.key);
            CONTROL_CHECK(owner != nullptr))
            owner->applyOriginalF0(correctedCurve(clip.durationSec));
        renderService->pauseRenderWorker();
        processor->requestFullContentRender(clip.key);
        const auto cache = renderService->getRenderCache(clip.key);
        const auto pending = cache != nullptr ? cache->getPendingJobs() : std::vector<RenderCache::PendingJob>{};
        // A clip of 20 s has blocks of at most 8 s.
        if (CONTROL_CHECK(pending.size() >= 3) && CONTROL_CHECK(cache->claimPendingJob(pending.front())))
            CONTROL_CHECK(
                cache->completeChunkRenderFailure(pending.front().startSample, pending.front().targetRevision));
        const ScopedWaitLimit limit(kPromptWaitLimitMs);
        const auto path = outPath("retried.wav");
        const auto jobId = startExport(api, clipParams(handles.placement(clip.placementId), path));
        // The job has looked at the blocks (one failed, the others pending) before any of them is rendered.
        CONTROL_CHECK(pumpMessagesUntil(
            [&] { return memberOf(memberOf(jobOf(service, jobId), "progress"), "phase").text() == "waiting"; },
            kDriveTimeoutMs));
        bool resumed = false;
        CONTROL_CHECK(pumpMessagesUntil(
            [&] {
                // The render worker stays held back until the job writes, which waits for the worker
                // (drainRenderWorker); meanwhile the scenario renders every block that is queued.
                completePendingRender(*cache, true);
                if (!resumed && memberOf(memberOf(jobOf(service, jobId), "progress"), "phase").text() == "writing") {
                    renderService->resumeRenderWorker();
                    resumed = true;
                }
                return isTerminal(jobOf(service, jobId));
            },
            kDriveTimeoutMs));
        if (!resumed)
            renderService->resumeRenderWorker();
        const auto job = waitJob(api, jobId);
        note("retried: " + toJsonString(memberOf(job, "state")) + ", blocks "
             + std::to_string(cache->getChunkStats().idle) + " rendered, "
             + std::to_string(cache->getChunkStats().pending) + " pending");
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        CONTROL_CHECK(toJsonString(memberOf(resultOf(job), "dryContents")) == "[]");
        CONTROL_CHECK(cache->isCanonicalSettled() && !processor->isExportLockHeld());
        std::error_code ignored;
        std::filesystem::remove(std::filesystem::u8path(path), ignored);
    }

    // REVISION_CONFLICT: the clip's notes change while OpenTune writes the file. The writer waits for the
    // render worker (drainRenderWorker), which stays paused with the render of another clip queued until
    // the notes have changed.
    {
        const auto clip = addClip(*processor, media, "edited", 2, 0.0, 1.0, Analysis::Ready);
        CONTROL_CHECK(settleRender(*processor, clip.key));
        const auto queued = addClip(*processor, media, "queued", 3, 0.0, 1.0, Analysis::Ready);
        renderService->pauseRenderWorker();
        processor->requestFullContentRender(queued.key);
        CONTROL_CHECK(juceFileOf(outPath("edited.wav")).replaceWithText("kept"));
        const auto jobId = startExport(
            api, clipParams(handles.placement(clip.placementId), outPath("edited.wav"), R"(,"ifExists":"overwrite")"));
        CONTROL_CHECK(pumpMessagesUntil(
            [&] { return memberOf(memberOf(jobOf(service, jobId), "progress"), "phase").text() == "writing"; },
            kDriveTimeoutMs));
        CONTROL_CHECK(setNotes(*processor, clip.key, {{0.1, 0.5, 60.0}}));
        renderService->resumeRenderWorker();
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(failedWith(job, "REVISION_CONFLICT", "changed_during_export"));
        CONTROL_CHECK(juceFileOf(outPath("edited.wav")).loadFileAsString() == "kept");
        CONTROL_CHECK((filesIn(out) == std::vector<std::string>{"edited.wav", "failed.wav"}));
        CONTROL_CHECK(!processor->isExportLockHeld());
    }

    // NOT_READY: the analysis does not finish within the wait limit.
    {
        const ScopedWaitLimit limit(kShortWaitLimitMs);
        const auto clip = addClip(*processor, media, "slow", 4, 0.0, 1.0, Analysis::Running);
        const auto job = exportAndWait(api, clipParams(handles.placement(clip.placementId), outPath("slow.wav")));
        CONTROL_CHECK(failedWith(job, "NOT_READY", "export_wait_timeout"));
        CONTROL_CHECK(memberOf(memberOf(memberOf(job, "error"), "data"), "retryable").asBoolean());
        CONTROL_CHECK(!juceFileOf(outPath("slow.wav")).exists() && !processor->isExportLockHeld());
    }

    // The clip of a clip export is deleted while the job waits.
    {
        const auto clip = addClip(*processor, media, "removed", 5, 0.0, 1.0, Analysis::Running);
        const auto jobId = startExport(api, clipParams(handles.placement(clip.placementId), outPath("removed.wav")));
        CONTROL_CHECK(processor->getStandaloneArrangement()->deletePlacementById(5, clip.placementId));
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(failedWith(job, "REVISION_CONFLICT", "changed_during_export"));
        CONTROL_CHECK(!juceFileOf(outPath("removed.wav")).exists() && !processor->isExportLockHeld());
    }

    // Every clip of a track export is deleted while the job waits: nothing is left to export
    // (INVALID_ARGUMENT(nothing_to_export), not retryable).
    {
        const auto clip = addClip(*processor, media, "vanishing", 11, 0.0, 1.0, Analysis::Running);
        const auto jobId = startExport(api, R"({"kind":"track","target":{"trackId":11},"path":)"
                                                + quoted(outPath("vanishing.wav")) + "}");
        CONTROL_CHECK(processor->getStandaloneArrangement()->deletePlacementById(11, clip.placementId));
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(failedWith(job, "INVALID_ARGUMENT", "nothing_to_export"));
        CONTROL_CHECK(!memberOf(memberOf(memberOf(job, "error"), "data"), "retryable").asBoolean());
        CONTROL_CHECK(!juceFileOf(outPath("vanishing.wav")).exists() && !processor->isExportLockHeld());
    }

    // File identity (PROTOCOL.md section 20.3) and the final rename (section 20.4) on their own.
    {
        const auto a = writeWav(out, "identity-a", 0.1);
        const auto missing = (out / "identity-missing.wav").u8string();
        ControlFileId id;
        CONTROL_CHECK(controlFileIdentity(a, id) == ControlFileIdStatus::Found);
        CONTROL_CHECK(controlFileIdentity(missing, id) == ControlFileIdStatus::Missing);
        CONTROL_CHECK(controlFileIdentity((out / "no-directory" / "x.wav").u8string(), id)
                      == ControlFileIdStatus::Missing);
        CONTROL_CHECK(controlSameFile(a, a) == std::optional<bool>(true));
        CONTROL_CHECK(controlSameFile(a, missing) == std::optional<bool>(false));
        CONTROL_CHECK(controlSameFile(missing, missing) == std::optional<bool>(true));
        const auto b = writeWav(out, "identity-b", 0.1);
        CONTROL_CHECK(controlSameFile(a, b) == std::optional<bool>(false));
        // A rename that never replaces leaves an existing target alone.
        CONTROL_CHECK(controlRenameFile(a, b, false) == ControlRenameResult::TargetExists);
        CONTROL_CHECK(juceFileOf(a).existsAsFile() && juceFileOf(b).existsAsFile());
        CONTROL_CHECK(controlRenameFile(a, missing, false) == ControlRenameResult::Done);
        CONTROL_CHECK(!juceFileOf(a).exists() && juceFileOf(missing).existsAsFile());
        CONTROL_CHECK(controlRenameFile(missing, b, true) == ControlRenameResult::Done);
        CONTROL_CHECK(!juceFileOf(missing).exists() && juceFileOf(b).existsAsFile());

        // One probe compares the target with many paths and reads the target's identity once.
        {
            ControlSameFileProbe probe(b);
            CONTROL_CHECK(probe.sameAs(b) == std::optional<bool>(true));
            CONTROL_CHECK(probe.sameAs(missing) == std::optional<bool>(false));
            CONTROL_CHECK(probe.sameAs(a) == std::optional<bool>(false));
            ControlSameFileProbe missingProbe(missing);
            CONTROL_CHECK(missingProbe.sameAs(missing) == std::optional<bool>(true));
            CONTROL_CHECK(missingProbe.sameAs((out / "identity-other.wav").u8string()) == std::optional<bool>(false));
        }

        // The error codes of a volume that is not there are classified as "unavailable" (for the error detail);
        // like "failed" they make an identity undeterminable, so an export is refused (section 20.3 rule 4).
        // The test fixes this by status only: whether a classification is acted on is in loadedMediaError.
#if defined(_WIN32)
        CONTROL_CHECK(controlClassifyOsError(2) == ControlFileIdStatus::Missing);      // ERROR_FILE_NOT_FOUND
        CONTROL_CHECK(controlClassifyOsError(3) == ControlFileIdStatus::Missing);      // ERROR_PATH_NOT_FOUND
        CONTROL_CHECK(controlClassifyOsError(21) == ControlFileIdStatus::Unavailable); // ERROR_NOT_READY
        CONTROL_CHECK(controlClassifyOsError(53) == ControlFileIdStatus::Unavailable); // ERROR_BAD_NETPATH
        CONTROL_CHECK(controlClassifyOsError(67) == ControlFileIdStatus::Unavailable); // ERROR_BAD_NET_NAME
        CONTROL_CHECK(controlClassifyOsError(15) == ControlFileIdStatus::Unavailable); // ERROR_INVALID_DRIVE
        CONTROL_CHECK(controlClassifyOsError(5) == ControlFileIdStatus::Failed);       // ERROR_ACCESS_DENIED
#else
        CONTROL_CHECK(controlClassifyOsError(ENOENT) == ControlFileIdStatus::Missing);
        CONTROL_CHECK(controlClassifyOsError(ENODEV) == ControlFileIdStatus::Unavailable);
        CONTROL_CHECK(controlClassifyOsError(ESTALE) == ControlFileIdStatus::Unavailable);
        CONTROL_CHECK(controlClassifyOsError(EACCES) == ControlFileIdStatus::Failed);
#endif

        // After the link of a rename that never replaces, a source that cannot be unlinked is reported, not
        // hidden; one that is gone already is not a problem.
        CONTROL_CHECK(controlRenameAfterLink(0, 0) == ControlRenameResult::Done);
        CONTROL_CHECK(controlRenameAfterLink(-1, ENOENT) == ControlRenameResult::Done);
        CONTROL_CHECK(controlRenameAfterLink(-1, 13) == ControlRenameResult::DoneSourceRemains);
    }
}

namespace {

/**
 * A deferred render retry in the process's render runtime for the render service of a processor: one running render
 * block of a content that is not part of any export, which the runtime was told to retry later because the vocoder
 * is being reconfigured (ProcessRenderRuntime::deferOrRequeue). While it exists,
 * ProcessRenderRuntime::hasDeferredRetriesForService is true for the service and nothing resumes it. release()
 * removes it (pruneDeferredJobsForService) and ends the reconfiguration; the destructor does the same.
 */
class DeferredRenderRetry
{
public:
    explicit DeferredRenderRetry(ContentRenderService& service) : service_(service)
    {
        auto& runtime = ProcessRenderRuntime::getInstance();
        const ContentKey key{DomainKind::StandaloneClip, 9000u, 0};
        auto cache = service.getOrCreateRenderCache(key);
        cache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
        RenderCache::RunningChunkSnapshot running;
        const auto pending = cache->getPendingJobs();
        if (pending.size() != 1 || !cache->claimPendingJob(pending.front()) || !cache->getRunningChunk(0, running))
            return;

        auto snapshot = std::make_shared<EditableContentSnapshot>();
        snapshot->contentRevision = 1;
        snapshot->audioRevision = 7;
        snapshot->timeGrid = TimeGridSnapshot::makeIdentity(1.0);
        snapshot->pitchCurve =
            std::make_shared<const PitchCurveSnapshot>(std::vector<float>(512, 220.0f), std::vector<float>(512, 1.0f),
                                                       std::vector<PitchCorrectionSegment>{}, 512, 16000.0);
        auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 44100);
        audio->clear();
        service.publishPlaybackSource(
            key, makePlaybackReadSource(key, snapshot, audio, 44100.0, cache, service.getTimeStretchCache()));

        RenderJob job;
        job.kind = RenderJob::Kind::Stage1Render;
        job.contentKey = key;
        job.contentSnapshot = snapshot;
        job.renderCache = cache;
        job.audioBuffer = audio;
        job.audioSampleRate = 44100.0;
        job.startSample = running.startSample;
        job.endSampleExclusive = running.endSampleExclusive;
        job.queuedChunkStartSample = running.startSample;
        job.contentRevision = snapshot->contentRevision;
        job.targetRevision = running.runningRevision;
        ProcessRenderRuntime::CompletionContext completion;
        completion.gate = std::make_shared<ProcessRenderRuntime::CompletionGate>();
        completion.contentRevision = job.contentRevision;
        completion.renderCache = cache;

        wasReconfiguring_ = runtime.isVocoderReconfiguring();
        ProcessRenderRuntimeTestAccess::setReconfiguring(runtime, true);
        reconfiguringSet_ = true;
        // The runtime keeps only a weak reference to the service; a non-owning handle is enough for the test, which
        // looks the retry up by the service's identity, as hasDeferredRetriesForService does.
        ProcessRenderRuntimeTestAccess::defer(runtime,
                                              std::shared_ptr<ContentRenderService>(std::shared_ptr<void>(), &service),
                                              std::move(job), std::move(completion));
    }

    ~DeferredRenderRetry() { release(); }

    DeferredRenderRetry(const DeferredRenderRetry&) = delete;
    DeferredRenderRetry& operator=(const DeferredRenderRetry&) = delete;

    bool active() const { return ProcessRenderRuntime::getInstance().hasDeferredRetriesForService(&service_); }

    void release()
    {
        auto& runtime = ProcessRenderRuntime::getInstance();
        runtime.pruneDeferredJobsForService(&service_);
        if (reconfiguringSet_) {
            ProcessRenderRuntimeTestAccess::setReconfiguring(runtime, wasReconfiguring_);
            reconfiguringSet_ = false;
        }
    }

private:
    ContentRenderService& service_;
    bool wasReconfiguring_ = false;
    bool reconfiguringSet_ = false;
};

juce::MemoryBlock bytesOf(const std::string& path)
{
    juce::MemoryBlock bytes;
    juceFileOf(path).loadFileAsData(bytes);
    return bytes;
}

} // namespace

void scenarioExportDeferredRetry()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    ApiSession api;
    if (!api.open(service))
        return;
    auto* renderService = processor->getContentRenderService();
    if (!CONTROL_CHECK(renderService != nullptr))
        return;
    const auto media = exportDirectory("deferred-media");
    const auto out = exportDirectory("deferred-out");
    const auto outPath = [&out](const std::string& name) { return (out / name).u8string(); };
    const auto clip = addClip(*processor, media, "settled", 0, 0.0, 1.0, Analysis::Ready);
    CONTROL_CHECK(settleRender(*processor, clip.key));
    const auto handles = handlesOf(api, *processor);
    const auto placement = handles.placement(clip.placementId);
    const auto overwrite = R"(,"ifExists":"overwrite")";

    // A file that the export would replace: another tone, so that a write would be seen in its bytes.
    const auto target = writeWav(out, "target", 0.2);
    const auto original = bytesOf(target);
    CONTROL_CHECK(original.getSize() > 0);

    DeferredRenderRetry retry(*renderService);
    if (!CONTROL_CHECK(retry.active()))
        return;

    // OpenTune's export functions refuse to read the render while a retry is deferred, and say so in their error
    // text, which is what the job's writer compares (getLastExportError, the same text for every kind of export).
    {
        const auto direct = juceFileOf(outPath("direct.wav"));
        CONTROL_CHECK(!processor->exportMasterMixAudio(direct, Language::English));
        CONTROL_CHECK(processor->getLastExportError() == "Render is not ready for export");
        CONTROL_CHECK(!direct.exists());
    }

    // 1. Every content is ready, but the retry stays: the job waits for it and ends at the wait limit with
    //    NOT_READY(export_wait_timeout), retryable, without a file and with the target as it was.
    {
        const ScopedWaitLimit limit(kShortWaitLimitMs * 3);
        const auto jobId = startExport(api, clipParams(placement, target, overwrite));
        CONTROL_CHECK(pumpMessagesUntil([&] { return isWaiting(jobOf(service, jobId)); }, kDriveTimeoutMs));
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(failedWith(job, "NOT_READY", "export_wait_timeout"));
        CONTROL_CHECK(memberOf(memberOf(memberOf(job, "error"), "data"), "retryable").asBoolean());
        CONTROL_CHECK(memberOf(job, "result").isNull());
        CONTROL_CHECK(bytesOf(target) == original);
        CONTROL_CHECK(filesIn(out) == std::vector<std::string>{"target.wav"});
        CONTROL_CHECK(!processor->isExportLockHeld());
    }

    // 2. The retry goes away while the job waits (the runtime resumed it): the job writes the file.
    {
        const ScopedWaitLimit limit(kDriveTimeoutMs);
        const auto jobId = startExport(api, clipParams(placement, target, overwrite));
        CONTROL_CHECK(pumpMessagesUntil([&] { return isWaiting(jobOf(service, jobId)); }, kDriveTimeoutMs));
        pumpMessagesFor(kQuietPumpMs);
        CONTROL_CHECK(isWaiting(jobOf(service, jobId)));
        CONTROL_CHECK(bytesOf(target) == original);
        retry.release();
        CONTROL_CHECK(!retry.active());
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        checkFormat(resultOf(job), 1, 1.0, 0.0);
        CONTROL_CHECK(!(bytesOf(target) == original));
        CONTROL_CHECK(filesIn(out) == std::vector<std::string>{"target.wav"});
        CONTROL_CHECK(!processor->isExportLockHeld());
    }
}

// ---- The specification's vectors -------------------------------------------------------------------

namespace {

const std::set<std::string> kExportVectorMethods = {"export.start", "job.get", "job.cancel"};
const std::set<std::string> kExportVectorCapabilities = {"session.v1", "read.v1", "jobs.v1", "import.v1", "export.v1"};

std::string exportVectorSkipReason(const JsonValue& vector)
{
    if (kExportVectorCapabilities.count(memberOf(vector, "capability").text()) == 0)
        return "needs capability " + memberOf(vector, "capability").text() + ", which this runner does not cover";
    const auto& extra = memberOf(vector, "extraCapabilities");
    for (std::size_t i = 0; i < extra.size(); ++i)
        if (kExportVectorCapabilities.count(extra.at(i).text()) == 0)
            return "needs capability " + extra.at(i).text() + ", which this runner does not cover";
    if (memberOf(vector, "session").text() != "authenticated")
        return "unauthenticated vectors test the listener (ControlUnitTests)";
    std::vector<std::string> methods;
    if (const JsonValue* request = vector.find("request"))
        methods.push_back(memberOf(*request, "method").text());
    if (const JsonValue* steps = vector.find("steps"))
        for (std::size_t i = 0; i < steps->size(); ++i) {
            const auto& step = steps->at(i);
            if ((step.find("send") == nullptr && step.find("expect") == nullptr) || step.find("connection") != nullptr)
                return "uses steps other than send and expect on the main connection";
            if (const JsonValue* send = step.find("send"))
                methods.push_back(memberOf(*send, "method").text());
        }
    for (const auto& method : methods)
        if (kExportVectorMethods.count(method) == 0)
            return "uses " + method + ", which this runner does not cover";
    const auto& given = memberOf(vector, "given");
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& name = given.keyAt(i);
        if (name == "dirty")
            continue;
        if (name == "files") {
            for (std::size_t f = 0; f < given.at(i).size(); ++f) {
                const auto kind = memberOf(given.at(i).at(f), "kind").text();
                if (kind != "absent" && kind != "existing" && kind != "audio" && kind != "noAccess")
                    return "file kind " + kind + " is not established by this runner";
            }
            continue;
        }
        if (name == "jobs") {
            for (std::size_t j = 0; j < given.at(i).size(); ++j)
                if (memberOf(given.at(i).at(j), "state").text() == "queued")
                    return "a queued job is not established by this runner";
            continue;
        }
        if (name == "clips") {
            for (std::size_t c = 0; c < given.at(i).size(); ++c) {
                const auto& clip = given.at(i).at(c);
                for (std::size_t m = 0; m < clip.size(); ++m) {
                    const auto& member = clip.keyAt(m);
                    if (member != "bind" && member != "trackId" && member != "startTimelineSec"
                        && member != "durationSec" && member != "analysis" && member != "notes")
                        return "clips with " + member + " are not established by this runner";
                }
            }
            continue;
        }
        return "precondition given." + name + " is not established by this runner";
    }
    return {};
}

Analysis analysisOf(const std::string& name)
{
    if (name == "running")
        return Analysis::Running;
    if (name == "failed")
        return Analysis::Failed;
    if (name == "unvoiced")
        return Analysis::Unvoiced;
    return Analysis::Ready;
}

/** A job of the fixture: runs until released or cancelled. */
struct FixtureJob
{
    std::atomic<bool> release{false};
};

/** Establishes given.jobs: each job in its state, its id bound to its name. A running export job holds the
    export lock, as the vectors' README requires; lockHeld tells the caller to release it. */
bool establishJobs(OpenTuneAudioProcessor& processor, ControlJobs& jobs, const JsonValue& given, VectorMatcher& matcher,
                   std::vector<std::shared_ptr<FixtureJob>>& fixtures, bool& lockHeld)
{
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& spec = given.at(i);
        const auto state = memberOf(spec, "state").text();
        const auto& error = memberOf(spec, "error");
        auto fixture = std::make_shared<FixtureJob>();
        ControlJobWork work = [fixture](ControlJobStep& step) {
            if (step.cancelRequested())
                step.cancel();
            else if (fixture->release.load())
                step.succeed(JsonValue::object());
        };
        if (state == "succeeded") {
            work = [](ControlJobStep& step) { step.succeed(JsonValue::object()); };
        } else if (state == "failed") {
            ControlJobError failure{error.isObject() ? memberOf(error, "kind").text() : "INTERNAL",
                                    error.isObject() ? memberOf(error, "reason").text() : "",
                                    "a failed job of the fixture", false};
            work = [failure](ControlJobStep& step) { step.fail(failure); };
        } else if (state == "cancelled") {
            work = [](ControlJobStep& step) { step.cancel(); };
        }
        const auto kind = memberOf(spec, "kind").text();
        const auto started = jobs.start(kind, memberOf(spec, "cancellable").asBoolean() || state != "running", work);
        if (started.jobId.empty())
            return false;
        matcher.bindings[memberOf(spec, "bind").text()] = JsonValue::string(started.jobId);
        const std::string jobId = started.jobId;
        if (!pumpMessagesUntil(
                [&jobs, jobId, state] {
                    JsonValue job;
                    return jobs.get(jobId, job) == ControlJobs::Lookup::Found && memberOf(job, "state").text() == state;
                },
                kReadTimeoutMs))
            return false;
        if (kind == "export" && state == "running" && !lockHeld)
            lockHeld = processor.tryAcquireExportLock();
        fixtures.push_back(std::move(fixture));
    }
    return true;
}

/** Runs one vector of export.start against a fresh processor with a test bridge. Empty on success. */
std::string runExportVector(const JsonValue& vector, const std::string& slug)
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return "the control API did not start";
    const auto& given = memberOf(vector, "given");
    TestUiBridge bridge;
    bridge.project.dirty = memberOf(given, "dirty").isBoolean() && memberOf(given, "dirty").asBoolean();
    ScopedBridge registered(service, bridge);
    const auto directory = exportDirectory("vectors-" + slug);
    VectorMatcher matcher;

    const auto& files = memberOf(given, "files");
    NoAccessFile noAccess; // restores the rights when the run ends
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files.at(i);
        const auto bind = memberOf(file, "bind").text();
        const auto kind = memberOf(file, "kind").text();
        const auto extension = file.find("extension") != nullptr ? memberOf(file, "extension").text() : ".wav";
        std::string path;
        if (kind == "audio") {
            path = writeWav(directory, bind, memberOf(file, "durationSec").asDouble());
        } else if (kind == "noAccess") {
            if (noAccess.isLocked())
                return "the vector has a second noAccess file, which this runner cannot lock";
            path = noAccess.create(directory, bind + extension);
            if (path.empty())
                return "the file " + bind + " could not be made unreadable for this user";
        } else {
            path = (directory / (bind + extension)).u8string();
            if (kind == "existing") {
                if (!juceFileOf(path).replaceWithText("an existing file"))
                    return "the file " + bind + " could not be written";
                matcher.bindings[bind + ".next"] =
                    JsonValue::string((directory / (bind + " (2)" + extension)).u8string());
            }
        }
        matcher.bindings[bind] = JsonValue::string(path);
    }
    const auto& clips = memberOf(given, "clips");
    std::vector<std::pair<std::string, TestClip>> created;
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto& spec = clips.at(i);
        const auto bind = memberOf(spec, "bind").text();
        const auto analysis = analysisOf(memberOf(spec, "analysis").text());
        auto clip =
            addClip(*processor, directory, "clip-" + bind, static_cast<int>(memberOf(spec, "trackId").asDouble()),
                    memberOf(spec, "startTimelineSec").asDouble(), memberOf(spec, "durationSec").asDouble(), analysis);
        if (!clip.key.isValid())
            return "the clip " + bind + " could not be added";
        std::vector<std::array<double, 3>> notes;
        const auto& noteSpecs = memberOf(spec, "notes");
        for (std::size_t n = 0; n < noteSpecs.size(); ++n)
            notes.push_back({memberOf(noteSpecs.at(n), "startContentSec").asDouble(),
                             memberOf(noteSpecs.at(n), "endContentSec").asDouble(),
                             memberOf(noteSpecs.at(n), "targetMidi").asDouble()});
        if (!notes.empty() && !setNotes(*processor, clip.key, notes))
            return "the notes of " + bind + " could not be set";
        // An analysed clip has a current render, as in OpenTune once rendering finished.
        if (analysis == Analysis::Ready && !settleRender(*processor, clip.key))
            return "the render of " + bind + " could not be made current";
        created.emplace_back(bind, clip);
    }
    std::vector<std::shared_ptr<FixtureJob>> fixtures;
    bool lockHeld = false;
    if (!establishJobs(*processor, *service.jobs(), memberOf(given, "jobs"), matcher, fixtures, lockHeld))
        return "given.jobs could not be established";

    ApiSession api;
    if (!api.open(service))
        return "session.hello failed";
    const auto& discovery = api.discovery();
    for (std::size_t i = 0; i < discovery.size(); ++i)
        matcher.bindings["discovery." + discovery.keyAt(i)] = discovery.at(i);
    const auto handles = handlesOf(api, *processor);
    matcher.bindings["runner.unknownPlacementId"] = JsonValue::string(handles.placement(4294967295ULL));
    matcher.bindings["runner.unknownContentId"] =
        JsonValue::string("c:" + handles.instanceShort + ":" + std::to_string(handles.epoch) + ":4294967295");
    const int lastTrack = processor->getStandaloneArrangement()->getNumTracks() - 1;
    matcher.bindings["runner.lastTrackId"] = JsonValue::integer(lastTrack);
    matcher.bindings["runner.unknownTrackId"] = JsonValue::integer(lastTrack + 1);
    if (!created.empty()) {
        const auto project = api.result("project.get");
        const auto& placements = memberOf(project, "placements");
        const auto& tokens = memberOf(memberOf(project, "asOf"), "contents");
        for (const auto& [bind, clip] : created) {
            const auto contentId = handles.content(clip.key);
            matcher.bindings[bind + ".contentId"] = JsonValue::string(contentId);
            matcher.bindings[bind + ".placementId"] = JsonValue::string(handles.placement(clip.placementId));
            matcher.bindings[bind + ".sourcePath"] = JsonValue::string(clip.sourcePath);
            for (std::size_t p = 0; p < placements.size(); ++p)
                if (memberOf(placements.at(p), "placementId").text() == handles.placement(clip.placementId))
                    matcher.bindings[bind + ".geometryHash"] = memberOf(placements.at(p), "geometryHash");
            for (std::size_t t = 0; t < tokens.size(); ++t)
                if (memberOf(tokens.at(t), "contentId").text() == contentId)
                    for (const char* token : {"revision", "gridRevision", "keyRevision"})
                        matcher.bindings[bind + "." + token] = memberOf(tokens.at(t), token);
        }
    }

    std::string failure;
    runWhilePumping(
        [&] {
            auto& connection = api.client();
            if (memberOf(vector, "type").text() == "request") {
                const auto request = matcher.substitute(memberOf(vector, "request"), failure);
                if (!failure.empty())
                    return;
                if (!connection.sendLine(toJsonString(request))) {
                    failure = "cannot send the request";
                    return;
                }
                failure = matcher.expectLine(connection, memberOf(vector, "response"));
                if (failure.empty() && memberOf(vector, "connectionAfter").text() == "open"
                    && !connection.staysQuiet(kOpenCheckMs))
                    failure = "the connection closed or sent more";
                return;
            }
            const auto& steps = memberOf(vector, "steps");
            for (std::size_t i = 0; i < steps.size() && failure.empty(); ++i) {
                const auto& step = steps.at(i);
                if (const JsonValue* send = step.find("send")) {
                    const auto value = matcher.substitute(*send, failure);
                    if (failure.empty() && !connection.sendLine(toJsonString(value)))
                        failure = "send failed";
                } else {
                    failure = matcher.expectLine(connection, memberOf(step, "expect"));
                }
                if (!failure.empty())
                    failure = "step " + std::to_string(i + 1) + ": " + failure;
            }
        },
        kJobExchangeMs);
    for (const auto& fixture : fixtures)
        fixture->release = true;
    if (lockHeld)
        processor->releaseExportLock();
    return failure;
}

/** The vectors of export/ that must run and pass headless; the rest are listed as skipped with the reason. */
const char* const kMustRunExportVectors[] = {
    "export/job-analysis-failed",
    "export/job-failures",
    "export/start-bad-extension",
    "export/start-busy-export",
    "export/start-clip",
    "export/start-dry-contents",
    "export/start-handle-kind",
    "export/start-identity-unreadable",
    "export/start-loaded-media",
    "export/start-master",
    "export/start-not-found",
    "export/start-nothing-to-export",
    "export/start-reports-revisions",
    "export/start-target-exists",
    "export/start-track",
};

} // namespace

void scenarioExportVectors()
{
    const auto root = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors" / "export";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.path().extension() == ".json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    int passed = 0;
    int skipped = 0;
    std::set<std::string> passedIds;
    VectorReport report("ControlProcessorTests.ExportVectors");
    for (const auto& file : files) {
        const auto parsed = parseJson(readFile(file));
        if (!CONTROL_CHECK(parsed.ok))
            continue;
        const auto& vector = parsed.value;
        const auto id = memberOf(vector, "id").text();
        if (const auto reason = exportVectorSkipReason(vector); !reason.empty()) {
            ++skipped;
            report.skip(id, reason);
            note("SKIP " + id + ": " + reason);
            continue;
        }
        const auto started = Clock::now();
        auto slug = id;
        std::replace(slug.begin(), slug.end(), '/', '-');
        const auto failure = runExportVector(vector, slug);
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty())) {
            ++passed;
            passedIds.insert(id);
        }
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    for (const char* id : kMustRunExportVectors)
        CONTROL_CHECK(passedIds.count(id) == 1);
    note(std::to_string(passed) + " vectors passed, " + std::to_string(skipped) + " skipped");
}

// ---- A clip whose time grid is not the identity ----------------------------------------------------

namespace {

/** The largest absolute sample of a WAV file; 0 for a file that cannot be read. */
float peakOf(const std::string& path)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juceFileOf(path)));
    if (reader == nullptr)
        return 0.0f;
    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
    float peak = 0.0f;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        peak = std::max(peak, audio.findMinMax(channel, 0, audio.getNumSamples()).getEnd());
    return peak;
}

/** A grid that plays the first second of a clip in 1.2 s and the rest faster: one handle at 1 s of the content,
    at 1.2 s of the output. */
std::shared_ptr<const TimeGridSnapshot> stretchedGrid(double durationSec)
{
    std::vector<TimeHandle> handles(3);
    handles[0].id = 1;
    handles[0].kind = HandleKind::ClipStart;
    handles[1].id = 2;
    handles[1].source_seconds = 1.0;
    handles[1].output_seconds = 1.2;
    handles[1].kind = HandleKind::UserAdded;
    handles[2].id = 3;
    handles[2].source_seconds = durationSec;
    handles[2].output_seconds = durationSec;
    handles[2].kind = HandleKind::ClipEnd;
    return TimeGridSnapshot::makeFromHandles(std::move(handles));
}

/** Whether the time-stretch output of a content was built at its current revisions, read the way the export reads
    it (TimeStretchCache::sliceCanonicalForOutputRange). */
bool stretchOutputCurrent(OpenTuneAudioProcessor& processor, const ContentKey& key)
{
    const auto snapshot = processor.getContentSnapshot(key);
    auto* service = processor.getContentRenderService();
    juce::AudioBuffer<float> probe(1, 1);
    return snapshot != nullptr && service != nullptr
           && service->getTimeStretchCache().sliceCanonicalForOutputRange(key, snapshot->contentRevision,
                                                                          snapshot->timeGridRevision, 0, probe, 0, 1)
                  > 0;
}

/** Publishes the playback source of a content again with a copy of its audio: the same samples and revisions in
    another buffer. A time-stretch rebuild queued before no longer matches the published source, and OpenTune's
    render worker drops it (Stage2TimeStretchRebuilder::rebuild compares the buffer). */
bool republishWithCopiedAudio(ContentRenderService& service, const ContentKey& key)
{
    PlaybackReadSource source;
    if (!service.getPlaybackReadSource(key, source) || source.audioBuffer == nullptr)
        return false;
    auto copy = std::make_shared<const juce::AudioBuffer<float>>(*source.audioBuffer);
    service.publishPlaybackSource(key, makePlaybackReadSource(key, source.contentSnapshot, copy, source.audioSampleRate,
                                                              source.renderCache, service.getTimeStretchCache()));
    PlaybackReadSource now;
    return service.getPlaybackReadSource(key, now) && now.audioBuffer == copy;
}

bool renderWorkerIdle(ContentRenderService& service)
{
    return service.renderQueueDepth() == 0 && service.renderInFlight() == 0;
}

} // namespace

void scenarioExportTimeStretch()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    ScopedBridge registered(service, bridge);
    ApiSession api;
    if (!api.open(service))
        return;
    auto* renderService = processor->getContentRenderService();
    if (!CONTROL_CHECK(renderService != nullptr))
        return;
    const auto media = exportDirectory("stretch-media");
    const auto out = exportDirectory("stretch-out");
    const auto handles = handlesOf(api, *processor);

    // 1. The grid changes while the render is settled: OpenTune queues the time-stretch rebuild itself
    // (setContentTimeGrid). While the render worker holds it back, the export waits and writes nothing; once the
    // rebuild ran, the file holds the stretched render, not silence.
    const auto queued = addClip(*processor, media, "queued", 0, 1.0, 3.0, Analysis::Ready);
    CONTROL_CHECK(settleRender(*processor, queued.key));
    renderService->pauseRenderWorker();
    CONTROL_CHECK(processor->setContentTimeGrid(queued.key, stretchedGrid(queued.durationSec)));
    CONTROL_CHECK(!stretchOutputCurrent(*processor, queued.key));
    const auto queuedPath = (out / "queued.wav").u8string();
    const auto queuedJob = startExport(api, clipParams(handles.placement(queued.placementId), queuedPath));
    CONTROL_CHECK(pumpMessagesUntil([&] { return isWaiting(jobOf(service, queuedJob)); }, kDriveTimeoutMs));
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(isWaiting(jobOf(service, queuedJob)));
    CONTROL_CHECK(filesIn(out).empty());
    renderService->resumeRenderWorker();
    const auto written = waitJob(api, queuedJob);
    CONTROL_CHECK(memberOf(written, "state").text() == "succeeded");
    CONTROL_CHECK(stretchOutputCurrent(*processor, queued.key));
    checkFormat(resultOf(written), 1, queued.durationSec, queued.startSec);
    CONTROL_CHECK(peakOf(queuedPath) > kRenderLevel / 2);

    // 2. The grid changed before the render settled, and the render settled without OpenTune's completion
    // callback: nothing queued the rebuild. The job queues it (as setContentTimeGrid would) and waits for it.
    const auto unqueued = addClip(*processor, media, "unqueued", 1, 0.5, 2.0, Analysis::Ready);
    CONTROL_CHECK(processor->setContentTimeGrid(unqueued.key, stretchedGrid(unqueued.durationSec)));
    CONTROL_CHECK(settleRender(*processor, unqueued.key));
    CONTROL_CHECK(pumpMessagesUntil(
        [renderService] { return renderService->renderQueueDepth() == 0 && renderService->renderInFlight() == 0; },
        kDriveTimeoutMs));
    CONTROL_CHECK(!stretchOutputCurrent(*processor, unqueued.key));
    const auto unqueuedPath = (out / "unqueued.wav").u8string();
    const auto rebuilt = exportAndWait(api, clipParams(handles.placement(unqueued.placementId), unqueuedPath));
    CONTROL_CHECK(memberOf(rebuilt, "state").text() == "succeeded");
    CONTROL_CHECK(stretchOutputCurrent(*processor, unqueued.key));
    checkFormat(resultOf(rebuilt), 1, unqueued.durationSec, unqueued.startSec);
    CONTROL_CHECK(peakOf(unqueuedPath) > kRenderLevel / 2);

    // 3. With the identity grid nothing is stretched, and the export does not wait for a rebuild.
    const auto plain = addClip(*processor, media, "plain", 2, 0.0, 1.5, Analysis::Ready);
    CONTROL_CHECK(settleRender(*processor, plain.key));
    const auto plainJob =
        exportAndWait(api, clipParams(handles.placement(plain.placementId), (out / "plain.wav").u8string()));
    CONTROL_CHECK(memberOf(plainJob, "state").text() == "succeeded");

    // The cases below would wait for the wait limit if the job did not end or ask again; a short limit turns that
    // into NOT_READY(export_wait_timeout) instead of a stalled test.
    const ScopedWaitLimit waitLimit(20000);

    // 4. A content to export dry with a stretched grid has no render to stretch, so OpenTune has no audio of it for
    // the file: the job ends at once, without a file, with NOT_READY(export_dry_stretched), which is not retryable.
    // The content is dry because its analysis failed (with allowDry) or because it has no voiced audio.
    {
        const auto failed = addClip(*processor, media, "failed", 3, 0.0, 2.0, Analysis::Failed);
        CONTROL_CHECK(processor->setContentTimeGrid(failed.key, stretchedGrid(failed.durationSec)));
        const auto path = (out / "failed.wav").u8string();
        const auto job =
            exportAndWait(api, clipParams(handles.placement(failed.placementId), path, R"(,"allowDry":true)"));
        CONTROL_CHECK(failedWith(job, "NOT_READY", "export_dry_stretched"));
        CONTROL_CHECK(!memberOf(memberOf(memberOf(job, "error"), "data"), "retryable").asBoolean());
        CONTROL_CHECK(!juceFileOf(path).exists());
        const auto unvoiced = addClip(*processor, media, "unvoiced", 4, 0.0, 2.0, Analysis::Unvoiced);
        CONTROL_CHECK(processor->setContentTimeGrid(unvoiced.key, stretchedGrid(unvoiced.durationSec)));
        const auto unvoicedPath = (out / "unvoiced.wav").u8string();
        const auto unvoicedJob = exportAndWait(api, clipParams(handles.placement(unvoiced.placementId), unvoicedPath));
        CONTROL_CHECK(failedWith(unvoicedJob, "NOT_READY", "export_dry_stretched"));
        CONTROL_CHECK(!memberOf(memberOf(memberOf(unvoicedJob, "error"), "data"), "retryable").asBoolean());
        CONTROL_CHECK(!juceFileOf(unvoicedPath).exists());
    }

    // 5. The render worker drops the rebuild the job asked for (the published playback source changed meanwhile,
    // without a new revision): once the worker is idle and the output still not current, the job asks again, and
    // the file holds the stretched render.
    {
        const auto dropped = addClip(*processor, media, "dropped", 5, 0.0, 2.0, Analysis::Ready);
        CONTROL_CHECK(processor->setContentTimeGrid(dropped.key, stretchedGrid(dropped.durationSec)));
        CONTROL_CHECK(settleRender(*processor, dropped.key));
        CONTROL_CHECK(pumpMessagesUntil([renderService] { return renderWorkerIdle(*renderService); }, kDriveTimeoutMs));
        renderService->pauseRenderWorker();
        const auto path = (out / "dropped.wav").u8string();
        const auto jobId = startExport(api, clipParams(handles.placement(dropped.placementId), path));
        // The job's request waits in the paused worker's queue; the copied audio makes it stale.
        CONTROL_CHECK(
            pumpMessagesUntil([renderService] { return renderService->renderQueueDepth() == 1; }, kDriveTimeoutMs));
        CONTROL_CHECK(republishWithCopiedAudio(*renderService, dropped.key));
        renderService->resumeRenderWorker();
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        CONTROL_CHECK(stretchOutputCurrent(*processor, dropped.key));
        CONTROL_CHECK(peakOf(path) > kRenderLevel / 2);
    }

    // 6. Every rebuild the job asks for is dropped: after kControlExportMaxStretchRequests requests for the same
    // revisions the job gives up with NOT_READY (no reason), without a file.
    {
        const auto stubborn = addClip(*processor, media, "stubborn", 6, 0.0, 2.0, Analysis::Ready);
        CONTROL_CHECK(processor->setContentTimeGrid(stubborn.key, stretchedGrid(stubborn.durationSec)));
        CONTROL_CHECK(settleRender(*processor, stubborn.key));
        CONTROL_CHECK(pumpMessagesUntil([renderService] { return renderWorkerIdle(*renderService); }, kDriveTimeoutMs));
        renderService->pauseRenderWorker();
        const auto path = (out / "stubborn.wav").u8string();
        const auto jobId = startExport(api, clipParams(handles.placement(stubborn.placementId), path));
        int requests = 0;
        while (requests <= kControlExportMaxStretchRequests) {
            // A request of the job waits in the paused worker's queue, or the job ended.
            if (!CONTROL_CHECK(pumpMessagesUntil(
                    [&] { return renderService->renderQueueDepth() == 1 || isTerminal(jobOf(service, jobId)); },
                    kDriveTimeoutMs)))
                break;
            if (renderService->renderQueueDepth() != 1)
                break;
            ++requests;
            CONTROL_CHECK(republishWithCopiedAudio(*renderService, stubborn.key));
            renderService->resumeRenderWorker();
            CONTROL_CHECK(
                pumpMessagesUntil([renderService] { return renderWorkerIdle(*renderService); }, kDriveTimeoutMs));
            renderService->pauseRenderWorker();
        }
        renderService->resumeRenderWorker();
        CONTROL_CHECK(requests == kControlExportMaxStretchRequests);
        const auto gaveUp = waitJob(api, jobId);
        CONTROL_CHECK(failedWith(gaveUp, "NOT_READY", "export_stretch_dropped"));
        CONTROL_CHECK(memberOf(memberOf(memberOf(gaveUp, "error"), "data"), "retryable").asBoolean());
        CONTROL_CHECK(!stretchOutputCurrent(*processor, stubborn.key));
        CONTROL_CHECK(!juceFileOf(path).exists());
    }
}

// ---- Boundary cases: edits right before an export, cancellation, destruction, other paths ------------------

namespace {

/** Levels of the synthetic render for the versions of a content: the published level of a revision tells which
    version a file holds. */
constexpr float kLevelPlus = 0.12f;
constexpr float kLevelMinus = 0.2f;
constexpr float kLevelTolerance = 0.004f;

/** The smallest and the largest absolute sample of channel 0 of a WAV file between two times, without the 10% at
    each end of that span. */
struct LevelRange
{
    bool ok = false;
    float low = 0.0f;
    float high = 0.0f;
};

LevelRange levelRange(const std::string& path, double fromSec, double toSec)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juceFileOf(path)));
    if (reader == nullptr)
        return {};
    const auto margin = (toSec - fromSec) * 0.1;
    const auto first = static_cast<juce::int64>((fromSec + margin) * reader->sampleRate);
    const auto last =
        std::min<juce::int64>(static_cast<juce::int64>((toSec - margin) * reader->sampleRate), reader->lengthInSamples);
    if (last <= first)
        return {};
    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), static_cast<int>(last - first));
    reader->read(&audio, 0, audio.getNumSamples(), first, true, true);
    LevelRange range;
    range.ok = true;
    range.low = 1e9f;
    for (int i = 0; i < audio.getNumSamples(); ++i) {
        const auto value = std::abs(audio.getSample(0, i));
        range.low = std::min(range.low, value);
        range.high = std::max(range.high, value);
    }
    return range;
}

/** The file holds one render, whole: a constant level within the tolerance of the expected one. A file that mixes
    two renders, or is silent, has a range that does not fit. */
bool holdsLevel(const std::string& path, double fromSec, double toSec, float expected)
{
    const auto range = levelRange(path, fromSec, toSec);
    const bool ok = range.ok && std::abs(range.low - expected) <= kLevelTolerance
                    && std::abs(range.high - expected) <= kLevelTolerance;
    note(path.substr(path.find_last_of("/\\") + 1) + ": levels " + std::to_string(range.low) + " .. "
         + std::to_string(range.high) + ", expected " + std::to_string(expected));
    return ok;
}

/** What each version of a content renders to, by the revision the content has (the snapshot's contentRevision). */
class VersionLevels
{
public:
    void record(OpenTuneAudioProcessor& processor, const ContentKey& key, float level)
    {
        if (const auto snapshot = processor.getContentSnapshot(key))
            levels_[snapshot->contentRevision] = level;
    }

    float levelNow(OpenTuneAudioProcessor& processor, const ContentKey& key) const
    {
        const auto snapshot = processor.getContentSnapshot(key);
        const auto found = snapshot != nullptr ? levels_.find(snapshot->contentRevision) : levels_.end();
        return found != levels_.end() ? found->second : kRenderLevel;
    }

    /** Publishes every pending block of the content as the render of its current version. */
    int publish(OpenTuneAudioProcessor& processor, const ContentKey& key) const
    {
        const auto cache = processor.getContentRenderService()->getRenderCache(key);
        return cache != nullptr ? completePendingRender(*cache, true, levelNow(processor, key)) : 0;
    }

private:
    std::map<std::uint64_t, float> levels_;
};

std::string editNotesParams(const std::string& contentId, const std::string& revision, int cents)
{
    return "{\"contentId\":" + quoted(contentId) + ",\"expectedRevision\":" + quoted(revision)
           + R"(,"ops":[{"op":"shiftCents","select":{"all":true},"deltaCents":)" + std::to_string(cents) + "}]}";
}

std::string autoTuneParams(const std::string& contentId, const std::string& revision)
{
    return "{\"contentId\":" + quoted(contentId) + ",\"mode\":\"regenerate\",\"expectedRevision\":" + quoted(revision)
           + R"(,"key":{"root":0,"scale":"major"}})";
}

std::string trackParams(int trackId, const std::string& path)
{
    return R"({"kind":"track","target":{"trackId":)" + std::to_string(trackId) + R"(},"path":)" + quoted(path) + "}";
}

std::string phaseOf(ControlService& service, const std::string& jobId)
{
    return memberOf(memberOf(jobOf(service, jobId), "progress"), "phase").text();
}

/** The revision of a content in a job's contents[]; empty when the job does not list it. */
std::string recordedRevision(const JsonValue& result, const std::string& contentId)
{
    const auto& contents = memberOf(result, "contents");
    for (std::size_t i = 0; i < contents.size(); ++i)
        if (memberOf(contents.at(i), "contentId").text() == contentId)
            return memberOf(contents.at(i), "revision").text();
    return {};
}

} // namespace

void scenarioExportAfterEdit()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    ScopedBridge registered(service, bridge);
    ApiSession api;
    if (!api.open(service))
        return;
    auto* renderService = processor->getContentRenderService();
    if (!CONTROL_CHECK(renderService != nullptr))
        return;
    const auto media = exportDirectory("after-edit-media");
    const auto out = exportDirectory("after-edit-out");
    const auto outPath = [&out](const std::string& name) { return (out / name).u8string(); };
    const auto handles = handlesOf(api, *processor);
    VersionLevels levels;

    const auto edited = addClip(*processor, media, "edited", 0, 1.0, 2.0, Analysis::Ready);
    CONTROL_CHECK(setNotes(*processor, edited.key, {{0.2, 0.8, 60.0}, {1.0, 1.6, 64.0}}));
    CONTROL_CHECK(settleRender(*processor, edited.key, kRenderLevel));
    CONTROL_CHECK(settleBackgroundWork(*processor));
    levels.record(*processor, edited.key, kRenderLevel);
    const auto contentId = handles.content(edited.key);
    const auto placementId = handles.placement(edited.placementId);

    // 1. An edit through the API, then an export at once, while OpenTune's render worker is held back: the render of
    //    the new revision is not there, so the job must not succeed (NOT_READY, retryable, no file) and must not
    //    export the content as dry. Once the render of the new revision is published the export succeeds with it.
    const auto editThenExport = [&](const std::string& label, const char* method,
                                    const std::function<std::string(const std::string&)>& params, float level) {
        renderService->pauseRenderWorker();
        const auto edit = api.result(method, json(params(revisionOf(api, contentId))));
        CONTROL_CHECK(memberOf(edit, "applied").asBoolean() && !memberOf(edit, "noOp").asBoolean());
        levels.record(*processor, edited.key, level);
        const auto revision = revisionOf(api, contentId);
        const auto early = outPath(label + "-early.wav");
        {
            const ScopedWaitLimit limit(kShortWaitLimitMs);
            const auto job = exportAndWait(api, clipParams(placementId, early));
            CONTROL_CHECK(failedWith(job, "NOT_READY", "export_wait_timeout"));
            CONTROL_CHECK(memberOf(memberOf(memberOf(job, "error"), "data"), "retryable").asBoolean());
            CONTROL_CHECK(memberOf(job, "result").isNull() && !juceFileOf(early).exists()
                          && !processor->isExportLockHeld());
        }
        CONTROL_CHECK(levels.publish(*processor, edited.key) > 0);
        renderService->resumeRenderWorker();
        const auto path = outPath(label + ".wav");
        const auto job = exportAndWait(api, clipParams(placementId, path));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = resultOf(job);
        CONTROL_CHECK(toJsonString(memberOf(result, "dryContents")) == "[]");
        CONTROL_CHECK(recordedRevision(result, contentId) == revision && revisionOf(api, contentId) == revision);
        CONTROL_CHECK(holdsLevel(path, 0.0, 2.0, level));
    };
    editThenExport(
        "notes", "content.editNotes",
        [&](const std::string& revision) { return editNotesParams(contentId, revision, 100); }, kLevelPlus);
    editThenExport(
        "auto", "content.autoTune", [&](const std::string& revision) { return autoTuneParams(contentId, revision); },
        kLevelMinus);

    // 2. Two versions that can be told apart (+100 and -100 cents) race with the export: the API's edit comes before
    //    the call, the user's edit while the job waits. The file is the whole of one version, listed with the
    //    revision it was made from, or the job fails with REVISION_CONFLICT(changed_during_export); never a mix, a
    //    silent file or a dry one.
    {
        renderService->pauseRenderWorker();
        const auto plusParams = editNotesParams(contentId, revisionOf(api, contentId), 100);
        CONTROL_CHECK(memberOf(api.result("content.editNotes", json(plusParams)), "applied").asBoolean());
        levels.record(*processor, edited.key, kLevelPlus);
        const auto plusRevision = revisionOf(api, contentId);
        const auto path = outPath("race-waiting.wav");
        const auto jobId = startExport(api, clipParams(placementId, path));
        CONTROL_CHECK(pumpMessagesUntil([&] { return phaseOf(service, jobId) == "waiting"; }, kDriveTimeoutMs));
        // The user's edit: the notes change again, and the editor asks for the render of the new version.
        CONTROL_CHECK(setNotes(*processor, edited.key, {{0.2, 0.8, 59.0}, {1.0, 1.6, 63.0}}));
        processor->requestFullContentRender(edited.key);
        levels.record(*processor, edited.key, kLevelMinus);
        const auto minusRevision = revisionOf(api, contentId);
        CONTROL_CHECK(minusRevision != plusRevision);
        CONTROL_CHECK(levels.publish(*processor, edited.key) > 0);
        renderService->resumeRenderWorker();
        const auto job = waitJob(api, jobId);
        if (memberOf(job, "state").text() == "succeeded") {
            const auto& result = resultOf(job);
            const auto recorded = recordedRevision(result, contentId);
            CONTROL_CHECK(toJsonString(memberOf(result, "dryContents")) == "[]");
            CONTROL_CHECK(recorded == plusRevision || recorded == minusRevision);
            CONTROL_CHECK(holdsLevel(path, 0.0, 2.0, recorded == plusRevision ? kLevelPlus : kLevelMinus));
        } else {
            CONTROL_CHECK(failedWith(job, "REVISION_CONFLICT", "changed_during_export"));
            CONTROL_CHECK(!juceFileOf(path).exists());
        }
        CONTROL_CHECK(!processor->isExportLockHeld());
    }

    // 3. The user's edit comes while the file is written: the render of the version the job recorded is the one in
    //    the file, but the content changed since, so the job fails and the target is not touched.
    {
        const auto other = addClip(*processor, media, "queued", 1, 0.0, 1.0, Analysis::Ready);
        renderService->pauseRenderWorker();
        processor->requestFullContentRender(other.key);
        // The version before the user's edit: the notes as they were at the start, rendered as +100 cents.
        CONTROL_CHECK(setNotes(*processor, edited.key, {{0.2, 0.8, 60.0}, {1.0, 1.6, 64.0}}));
        processor->requestFullContentRender(edited.key);
        levels.record(*processor, edited.key, kLevelPlus);
        CONTROL_CHECK(levels.publish(*processor, edited.key) > 0);
        const auto path = outPath("race-writing.wav");
        CONTROL_CHECK(juceFileOf(path).replaceWithText("kept"));
        const auto jobId = startExport(api, clipParams(placementId, path, R"(,"ifExists":"overwrite")"));
        CONTROL_CHECK(pumpMessagesUntil([&] { return phaseOf(service, jobId) == "writing"; }, kDriveTimeoutMs));
        CONTROL_CHECK(setNotes(*processor, edited.key, {{0.2, 0.8, 58.0}, {1.0, 1.6, 62.0}}));
        renderService->resumeRenderWorker();
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(failedWith(job, "REVISION_CONFLICT", "changed_during_export"));
        CONTROL_CHECK(juceFileOf(path).loadFileAsString() == "kept" && !processor->isExportLockHeld());
        // Only the target is left in the directory: the temporary file was deleted.
        for (const auto& name : filesIn(out))
            CONTROL_CHECK(name.empty() || name[0] != '.');
    }

    // 4. The job waits for another content of the export while the user edits a settled one: the export is of the
    //    new version of that content, with nothing listed as dry.
    {
        const auto settled = addClip(*processor, media, "settled", 2, 0.0, 2.0, Analysis::Ready);
        CONTROL_CHECK(setNotes(*processor, settled.key, {{0.2, 0.8, 60.0}, {1.0, 1.6, 64.0}}));
        CONTROL_CHECK(settleRender(*processor, settled.key, kRenderLevel));
        const auto late = addClip(*processor, media, "late", 2, 2.5, 2.0, Analysis::Running);
        renderService->pauseRenderWorker();
        const auto path = outPath("race-other.wav");
        const auto jobId = startExport(api, trackParams(2, path));
        CONTROL_CHECK(pumpMessagesUntil([&] { return phaseOf(service, jobId) == "waiting"; }, kDriveTimeoutMs));
        CONTROL_CHECK(setNotes(*processor, settled.key, {{0.2, 0.8, 59.0}, {1.0, 1.6, 63.0}}));
        processor->requestFullContentRender(settled.key);
        levels.record(*processor, settled.key, kLevelMinus);
        // The analysis of the other content finishes, with a correction, so that the content has something to render.
        auto* owner = processor->getStandaloneContentRepository()->findClip(late.key);
        if (CONTROL_CHECK(owner != nullptr))
            owner->applyOriginalF0(correctedCurve(late.durationSec));
        CONTROL_CHECK(processor->setContentOriginalF0State(late.key, OriginalF0State::Ready));
        // The job asks for the render of the other content once its analysis is ready; the pump lets it.
        CONTROL_CHECK(pumpMessagesUntil(
            [&] {
                levels.publish(*processor, late.key);
                const auto cache = renderService->getRenderCache(late.key);
                return cache != nullptr && cache->getChunkStats().total() > 0
                       && !cache->getChunkStats().hasActiveWork();
            },
            kDriveTimeoutMs));
        CONTROL_CHECK(levels.publish(*processor, settled.key) > 0);
        renderService->resumeRenderWorker();
        const auto job = waitJob(api, jobId);
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = resultOf(job);
        const auto settledId = handles.content(settled.key);
        CONTROL_CHECK(toJsonString(memberOf(result, "dryContents")) == "[]");
        CONTROL_CHECK(recordedRevision(result, settledId) == revisionOf(api, settledId));
        CONTROL_CHECK(holdsLevel(path, 0.0, 2.0, kLevelMinus));
        CONTROL_CHECK(holdsLevel(path, 2.5, 4.5, kRenderLevel));
    }
    const auto dispatcher = service.dispatcher();
    CONTROL_CHECK(dispatcher != nullptr && dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

// ---- Cancelling an export that would overwrite -------------------------------------------------------------

void scenarioExportCancelOverwrite()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    ScopedBridge registered(service, bridge);
    ApiSession api;
    if (!api.open(service))
        return;
    const auto media = exportDirectory("cancel-media");
    const auto out = exportDirectory("cancel-out");
    const auto handles = handlesOf(api, *processor);
    const auto pending = addClip(*processor, media, "pending", 0, 0.0, 1.0, Analysis::Running);
    const auto steady = addClip(*processor, media, "steady", 1, 0.0, 1.0, Analysis::Ready);
    CONTROL_CHECK(setNotes(*processor, steady.key, {{0.1, 0.5, 60.0}}));
    CONTROL_CHECK(settleBackgroundWork(*processor));
    const auto target = (out / "target.wav").u8string();
    CONTROL_CHECK(juceFileOf(target).replaceWithText("original"));
    const auto setKey = [&] {
        return json("{\"contentId\":" + quoted(handles.content(steady.key)) + R"(,"key":{"root":0,"scale":"major"}})");
    };

    // While the export waits for the analysis, a write is refused because of the export lock ...
    const auto jobId =
        startExport(api, clipParams(handles.placement(pending.placementId), target, R"(,"ifExists":"overwrite")"));
    CONTROL_CHECK(pumpMessagesUntil([&] { return phaseOf(service, jobId) == "waiting"; }, kDriveTimeoutMs));
    CONTROL_CHECK(api.fails("content.setKey", setKey(), "BUSY", "export_in_progress"));
    // ... and after its cancellation the target holds what it held, and nothing is busy any more.
    CONTROL_CHECK(
        memberOf(api.result("job.cancel", json(R"({"jobId":)" + quoted(jobId) + "}")), "cancelRequested").asBoolean());
    const auto job = waitJob(api, jobId);
    CONTROL_CHECK(memberOf(job, "state").text() == "cancelled");
    CONTROL_CHECK(juceFileOf(target).loadFileAsString() == "original");
    CONTROL_CHECK((filesIn(out) == std::vector<std::string>{"target.wav"}) && !processor->isExportLockHeld());
    const auto set = api.result("content.setKey", setKey());
    CONTROL_CHECK(memberOf(set, "applied").asBoolean() && !memberOf(set, "noOp").asBoolean());
}

// ---- Destroying the processor while an export job exists ---------------------------------------------------

void scenarioExportDestroy()
{
    const auto media = exportDirectory("destroy-media");

    // 1. The job waits for an analysis that never finishes: stopping the service ends it, releases the export lock
    //    and leaves no file, within the shutdown bound; destroying the processor afterwards is quiet.
    {
        const auto out = exportDirectory("destroy-waiting");
        auto processor = startedProcessor();
        auto& service = processor->getControlService();
        if (!service.isRunning())
            return;
        ApiSession api;
        if (!api.open(service))
            return;
        const auto handles = handlesOf(api, *processor);
        const auto clip = addClip(*processor, media, "never", 0, 0.0, 1.0, Analysis::Running);
        const auto jobId =
            startExport(api, clipParams(handles.placement(clip.placementId), (out / "never.wav").u8string()));
        CONTROL_CHECK(pumpMessagesUntil([&] { return phaseOf(service, jobId) == "waiting"; }, kDriveTimeoutMs));
        CONTROL_CHECK(processor->isExportLockHeld());
        const auto started = Clock::now();
        service.stop();
        const auto elapsed = millisecondsSince(started);
        note("the service stopped in " + std::to_string(elapsed) + " ms");
        CONTROL_CHECK(elapsed <= kShutdownBoundMs);
        CONTROL_CHECK(!processor->isExportLockHeld() && filesIn(out).empty());
        const auto destroyedAt = Clock::now();
        processor.reset();
        CONTROL_CHECK(millisecondsSince(destroyedAt) <= kShutdownBoundMs);
    }

    // 2. The job is writing: the writer waits for OpenTune's render worker, which is held back by a queued render.
    //    The export lock is released and no temporary file is left within the bound once the worker runs again; a
    //    target that the export would have replaced is untouched.
    {
        const auto out = exportDirectory("destroy-writing");
        auto processor = startedProcessor();
        auto& service = processor->getControlService();
        if (!service.isRunning())
            return;
        ApiSession api;
        if (!api.open(service))
            return;
        auto* renderService = processor->getContentRenderService();
        if (!CONTROL_CHECK(renderService != nullptr))
            return;
        const auto handles = handlesOf(api, *processor);
        const auto clip = addClip(*processor, media, "writing", 0, 0.0, 1.0, Analysis::Ready);
        CONTROL_CHECK(settleRender(*processor, clip.key));
        const auto queued = addClip(*processor, media, "queued", 1, 0.0, 1.0, Analysis::Ready);
        renderService->pauseRenderWorker();
        processor->requestFullContentRender(queued.key);
        const auto target = (out / "writing.wav").u8string();
        CONTROL_CHECK(juceFileOf(target).replaceWithText("original"));
        const auto jobId =
            startExport(api, clipParams(handles.placement(clip.placementId), target, R"(,"ifExists":"overwrite")"));
        CONTROL_CHECK(pumpMessagesUntil([&] { return phaseOf(service, jobId) == "writing"; }, kDriveTimeoutMs));
        CONTROL_CHECK(processor->isExportLockHeld());
        std::thread resumer([renderService] {
            sleepMs(kQuietPumpMs);
            renderService->resumeRenderWorker();
        });
        note("files before the stop: " + std::to_string(filesIn(out).size())
             + " (the target and, while the writer has "
               "opened it, its temporary file)");
        const auto started = Clock::now();
        service.stop();
        const auto elapsed = millisecondsSince(started);
        resumer.join();
        note("the service stopped in " + std::to_string(elapsed) + " ms");
        CONTROL_CHECK(elapsed <= kShutdownBoundMs);
        CONTROL_CHECK(!processor->isExportLockHeld());
        CONTROL_CHECK(filesIn(out) == std::vector<std::string>{"writing.wav"}
                      && juceFileOf(target).loadFileAsString() == "original");
        processor.reset();
    }
}

// ---- Folders and files with spaces and Chinese names --------------------------------------------------------

namespace {

/** Installs a synthetic analysis as the import job's analysis request for the life of this object: the state
    Extracting at once, the finished analysis from a later message (as OpenTune commits one), no model needed. With
    requestRender the finished analysis ends as OpenTune's own does (OpenTuneAudioProcessor::requestContentRefresh):
    with a request for the full render of the content once its state is Ready. */
class ScopedSyntheticImportAnalysis
{
public:
    explicit ScopedSyntheticImportAnalysis(bool requestRender = false)
    {
        auto alive = alive_;
        controlSetImportAnalysisForTesting(
            [alive, requestRender](OpenTuneAudioProcessor& processor,
                                   const OpenTuneAudioProcessor::ContentRefreshRequest& request) {
                if (!processor.setContentOriginalF0State(request.contentKey, OriginalF0State::Extracting))
                    return false;
                auto* target = &processor;
                juce::MessageManager::callAsync([alive, target, request, requestRender] {
                    if (!alive->load())
                        return;
                    const auto snapshot = target->getContentSnapshot(request.contentKey);
                    auto* owner = target->getStandaloneContentRepository()->findClip(request.contentKey);
                    if (snapshot == nullptr || snapshot->audioBuffer == nullptr || owner == nullptr)
                        return;
                    owner->applyOriginalF0(syntheticCurve(snapshot->audioBuffer->getNumSamples() / kAudioSampleRate));
                    target->setContentOriginalF0State(request.contentKey, OriginalF0State::Ready);
                    if (requestRender)
                        target->requestFullContentRender(request.contentKey);
                });
                return true;
            });
    }

    ~ScopedSyntheticImportAnalysis()
    {
        *alive_ = false;
        controlSetImportAnalysisForTesting({});
    }

    ScopedSyntheticImportAnalysis(const ScopedSyntheticImportAnalysis&) = delete;
    ScopedSyntheticImportAnalysis& operator=(const ScopedSyntheticImportAnalysis&) = delete;

private:
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace

void scenarioExportUnicodePath()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    bridge.visibleTracks = 2;
    ScopedBridge registered(service, bridge);
    const ScopedSyntheticImportAnalysis analysis;
    ApiSession api;
    if (!api.open(service))
        return;
    // The folder and the file are named in Chinese with a space in each (UTF-8 escapes keep this source ASCII):
    // <data>/export/<folderName>/<stem>.wav
    const std::string folderName = "\xE5\xAF\xBC\xE5\x87\xBA \xE6\xB5\x8B\xE8\xAF\x95";
    const std::string stem = "\xE4\xBA\xBA\xE5\xA3\xB0 \xE4\xB8\x80";
    const std::string suffix = " \xE5\xAF\xBC\xE5\x87\xBA";
    const auto directory = exportDirectory(folderName);
    CONTROL_CHECK(directory.filename().u8string() == folderName);
    const auto source = writeWav(directory, stem, 2.0);
    CONTROL_CHECK(source == (directory / std::filesystem::u8path(stem + ".wav")).u8string()
                  && juceFileOf(source).existsAsFile());

    // Import: the path comes back as given, and the project shows it as the clip's source file.
    const auto start =
        api.result("import.start", json(R"({"items":[{"path":)" + quoted(source) + R"(}],"layout":"single"})"));
    const auto importJob = waitJob(api, memberOf(memberOf(start, "job"), "jobId").text());
    CONTROL_CHECK(memberOf(importJob, "state").text() == "succeeded");
    const auto& items = memberOf(resultOf(importJob), "items");
    if (!CONTROL_CHECK(items.size() == 1))
        return;
    const auto& item = items.at(0);
    CONTROL_CHECK(memberOf(item, "imported").asBoolean() && memberOf(item, "path").text() == source
                  && memberOf(item, "f0State").text() == "ready");
    const auto project = api.result("project.get");
    CONTROL_CHECK(memberOf(project, "placements").size() == 1
                  && memberOf(memberOf(project, "placements").at(0), "sourcePath").text() == source);
    const auto placementId = memberOf(item, "placementId").text();
    const auto contents = processor->getStandaloneContentRepository()->getAllClips();
    if (!CONTROL_CHECK(contents.size() == 1))
        return;
    CONTROL_CHECK(settleRender(*processor, contents.front()));

    // Export next to the source, with the same kind of name: the path comes back as given, the file reads back.
    const auto target = (directory / std::filesystem::u8path(stem + suffix + ".wav")).u8string();
    const auto job = exportAndWait(api, clipParams(placementId, target));
    CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
    CONTROL_CHECK(memberOf(resultOf(job), "path").text() == target);
    checkFormat(resultOf(job), 1, 2.0, 0.0);
    CONTROL_CHECK(juceFileOf(target).existsAsFile() && std::filesystem::exists(std::filesystem::u8path(target)));
    // rename numbers the name and keeps its other parts; the source itself is never written; error refuses.
    const auto renamed = exportAndWait(api, clipParams(placementId, target, R"(,"ifExists":"rename")"));
    CONTROL_CHECK(memberOf(renamed, "state").text() == "succeeded"
                  && memberOf(resultOf(renamed), "path").text()
                         == (directory / std::filesystem::u8path(stem + suffix + " (2).wav")).u8string());
    CONTROL_CHECK(api.fails("export.start", json(clipParams(placementId, source, R"(,"ifExists":"overwrite")")),
                            "INVALID_ARGUMENT", "target_is_loaded_media"));
    CONTROL_CHECK(api.fails("export.start", json(clipParams(placementId, target)), "ALREADY_EXISTS", "target_exists"));
    CONTROL_CHECK(filesIn(directory).size() == 3);
}

// ---- The reasons of dryContents --------------------------------------------------------------------------

/**
 * dryContents[].reason (PROTOCOL.md section 19 rule 6; the schema of export.start: unvoiced for no voiced audio,
 * nothingToRender, analysisFailed with allowDry).
 *
 * 1. A clip of silence whose pitch analysis ended Ready with no voiced frame (what importing a silent file gives): the
 *    analysis found no voiced audio, so the clip is listed as unvoiced, also when its render (all blank) is settled,
 *    and when the analysis ended Ready without any F0 frame at all.
 * 2. A clip imported with import.start in the openTune scheme and never edited, exported at once: without notes or
 *    corrections there is nothing to render, so it is exported uncorrected and listed as nothingToRender. When the
 *    finished analysis asked for the render, as OpenTune's own does, its blocks end blank. When no render was ever
 *    requested for the content, the export neither asks for one nor waits for OpenTune's render worker, so it
 *    succeeds at once even while that worker is held back. Render blocks that are pending are still waited for,
 *    unless they will never be rendered (a second request at the same revision left them out of reach of the worker)
 *    and the export would read the content's own audio; when the blocks or the published render still hold the
 *    rendered audio of an earlier version with a correction, the export waits and fails at the wait limit instead.
 */
void scenarioExportDryReasons()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    bridge.visibleTracks = 4;
    ScopedBridge registered(service, bridge);
    processor->getAppPreferences().setAudioEditingScheme(AudioEditingScheme::Scheme::CorrectedF0Primary);
    ApiSession api;
    if (!api.open(service))
        return;
    auto* renderService = processor->getContentRenderService();
    if (!CONTROL_CHECK(renderService != nullptr))
        return;
    const auto media = exportDirectory("dry-media");
    const auto out = exportDirectory("dry-out");
    const auto outPath = [&out](const std::string& name) { return (out / name).u8string(); };
    const auto handles = handlesOf(api, *processor);
    // An export under the wait limit of this scenario.
    const auto exportWithin = [&api](const std::string& params) {
        const ScopedWaitLimit limit(kPromptWaitLimitMs);
        return exportAndWait(api, params);
    };
    const auto keyOf = [&processor, &handles](const std::string& contentId) {
        for (const auto& key : processor->getStandaloneContentRepository()->getAllClips())
            if (handles.content(key) == contentId)
                return key;
        return ContentKey{};
    };

    // 1. Silence analysed Ready without a voiced frame: unvoiced, before and after its blank render settled.
    const auto silent = addClip(*processor, media, "silent", 0, 0.0, 2.0, Analysis::ReadySilent);
    CONTROL_CHECK(settleBackgroundWork(*processor));
    const auto silentId = handles.content(silent.key);
    {
        const auto job = exportWithin(clipParams(handles.placement(silent.placementId), outPath("silent.wav")));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        note("silent: dryContents " + toJsonString(memberOf(resultOf(job), "dryContents")));
        CONTROL_CHECK(toJsonString(memberOf(resultOf(job), "dryContents")) == dryJson(silentId, "unvoiced"));
    }
    processor->requestFullContentRender(silent.key);
    CONTROL_CHECK(settleBackgroundWork(*processor));
    {
        const auto job =
            exportWithin(clipParams(handles.placement(silent.placementId), outPath("silent-rendered.wav")));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        CONTROL_CHECK(toJsonString(memberOf(resultOf(job), "dryContents")) == dryJson(silentId, "unvoiced"));
    }
    // The same for an analysis that ended Ready with an empty curve.
    {
        const auto empty = addClip(*processor, media, "empty", 1, 0.0, 1.0, Analysis::Ready);
        auto* owner = processor->getStandaloneContentRepository()->findClip(empty.key);
        if (CONTROL_CHECK(owner != nullptr)) {
            auto curve = std::make_shared<PitchCurve>();
            curve->setHopSize(kF0HopSize);
            curve->setSampleRate(kF0SampleRate);
            owner->applyOriginalF0(curve);
        }
        CONTROL_CHECK(settleBackgroundWork(*processor));
        const auto job = exportWithin(clipParams(handles.placement(empty.placementId), outPath("empty.wav")));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        CONTROL_CHECK(toJsonString(memberOf(resultOf(job), "dryContents"))
                      == dryJson(handles.content(empty.key), "unvoiced"));
    }

    // 2. Imported, analysed, never edited, exported at once.
    int trackId = 2;
    const auto importUntouched = [&](const std::string& name) -> std::optional<std::pair<std::string, std::string>> {
        const auto source = writeWav(media, name, 2.0);
        const auto start =
            api.result("import.start", json(R"({"items":[{"path":)" + quoted(source)
                                            + R"(}],"layout":"single","trackId":)" + std::to_string(trackId++) + "}"));
        const auto importJob = waitJob(api, memberOf(memberOf(start, "job"), "jobId").text());
        CONTROL_CHECK(memberOf(importJob, "state").text() == "succeeded");
        const auto& items = memberOf(resultOf(importJob), "items");
        if (!CONTROL_CHECK(items.size() == 1 && memberOf(items.at(0), "f0State").text() == "ready"))
            return std::nullopt;
        const auto contentId = memberOf(items.at(0), "contentId").text();
        const auto details = api.result("content.getDetails", json("{\"contentId\":" + quoted(contentId) + "}"));
        note(name + ": render before the export " + toJsonString(memberOf(details, "render")));
        return std::make_pair(contentId, memberOf(items.at(0), "placementId").text());
    };
    const auto exportsDry = [&](const std::string& name, const std::string& contentId, const std::string& placementId) {
        const auto started = Clock::now();
        const auto job = exportWithin(clipParams(placementId, outPath(name + ".wav")));
        note(name + ": export " + memberOf(job, "state").text() + " after " + std::to_string(millisecondsSince(started))
             + " ms, dryContents " + toJsonString(memberOf(resultOf(job), "dryContents")));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = resultOf(job);
        checkFormat(result, 1, 2.0, 0.0);
        CONTROL_CHECK(toJsonString(memberOf(result, "dryContents")) == dryJson(contentId, "nothingToRender"));
        CONTROL_CHECK(recordedRevision(result, contentId) == revisionOf(api, contentId));
        CONTROL_CHECK(!processor->isExportLockHeld());
    };
    // 2a. The finished analysis asked for the render, as OpenTune's own does; its blocks end blank.
    {
        const ScopedSyntheticImportAnalysis analysis(true);
        if (const auto clip = importUntouched("imported-rendered"))
            exportsDry("imported-rendered", clip->first, clip->second);
    }
    // 2b. No render was ever requested for the content (its render is not current, publishedRevision unsettled), and
    //     OpenTune's render worker does not get to a render queued now (it is held back, as OpenTune holds it while it
    //     reconfigures the vocoder): the content has nothing to render, so the export neither asks for a render nor
    //     waits for one, and succeeds at once instead of at the wait limit.
    {
        const ScopedSyntheticImportAnalysis analysis(false);
        if (const auto clip = importUntouched("imported")) {
            renderService->pauseRenderWorker();
            exportsDry("imported", clip->first, clip->second);
            // The job asked for no render.
            const auto cache = renderService->getRenderCache(keyOf(clip->first));
            CONTROL_CHECK(cache == nullptr || cache->getChunkStats().total() == 0);
            renderService->resumeRenderWorker();
        }
    }
    // 2c. Render blocks of the content are pending (its render was asked for and the worker has not run them): the
    //     export waits for them even though they will end blank, and succeeds once they did.
    {
        const ScopedSyntheticImportAnalysis analysis(false);
        if (const auto clip = importUntouched("imported-pending")) {
            const auto key = keyOf(clip->first);
            renderService->pauseRenderWorker();
            processor->requestFullContentRender(key);
            const auto cache = renderService->getRenderCache(key);
            CONTROL_CHECK(cache != nullptr && cache->getChunkStats().hasActiveWork());
            {
                const ScopedWaitLimit pendingLimit(kPendingBlocksWaitLimitMs);
                const auto job = exportAndWait(api, clipParams(clip->second, outPath("imported-pending-early.wav")));
                CONTROL_CHECK(failedWith(job, "NOT_READY", "export_wait_timeout"));
                CONTROL_CHECK(!juceFileOf(outPath("imported-pending-early.wav")).exists());
            }
            renderService->resumeRenderWorker();
            CONTROL_CHECK(settleBackgroundWork(*processor));
            exportsDry("imported-pending", clip->first, clip->second);
        }
    }
    // 2d. The finished analysis asked for the render while OpenTune's render worker was held back, and the render is
    //     asked for once more at the same revision: upstream RenderCache::reconcileFullPlanAndRequest then keeps the
    //     pending blocks pending but out of the set the worker claims from, so they stay pending with the worker idle
    //     until the content changes. The content has nothing to render and no block holds rendered audio, so the
    //     export does not wait for them. The background work of this processor does not settle after it, so the cases
    //     below (2e, 2f) use contents of their own and never call settleBackgroundWork().
    {
        const ScopedSyntheticImportAnalysis analysis(true);
        renderService->pauseRenderWorker();
        const auto clip = importUntouched("imported-stuck");
        const auto key = clip ? keyOf(clip->first) : ContentKey{};
        const auto cache = renderService->getRenderCache(key);
        if (CONTROL_CHECK(clip.has_value() && cache != nullptr && cache->getChunkStats().pending > 0)) {
            processor->requestFullContentRender(key);
            renderService->resumeRenderWorker();
            pumpMessagesFor(kQuietPumpMs);
            note("imported-stuck: " + std::to_string(cache->getChunkStats().pending) + " blocks pending, "
                 + std::to_string(cache->getPendingJobs().size()) + " of them to claim, render queue "
                 + std::to_string(renderService->renderQueueDepth()));
            // The case under test: blocks pending, none running, none to claim. Should upstream render them again,
            // this fails first, so that the need for pendingBlocksStuck is looked at again.
            const auto stats = cache->getChunkStats();
            CONTROL_CHECK(stats.pending > 0 && stats.running == 0 && cache->getPendingJobs().empty());
            exportsDry("imported-stuck", clip->first, clip->second);
        } else {
            renderService->resumeRenderWorker();
        }
    }
    // 2e. The same blocks out of reach of the worker, but the content had a correction that was rendered and published,
    //     and the correction is gone since: the content has nothing to render now, yet its blocks still hold the
    //     rendered audio of the earlier version, which the export would read. The export never lists that as dry: it
    //     waits, and fails at the wait limit with NOT_READY(export_wait_timeout), without a file.
    {
        const auto clip = addClip(*processor, media, "was-corrected", 6, 0.0, 2.0, Analysis::Ready);
        auto* owner = processor->getStandaloneContentRepository()->findClip(clip.key);
        const auto cache = renderService->getOrCreateRenderCache(clip.key);
        if (CONTROL_CHECK(owner != nullptr && cache != nullptr)) {
            owner->applyOriginalF0(correctedCurve(clip.durationSec));
            renderService->pauseRenderWorker();
            processor->requestFullContentRender(clip.key);
            CONTROL_CHECK(completePendingRender(*cache, true) > 0 && cache->isCanonicalSettled());
            // The user takes the correction back: a new revision without any correction segment.
            owner->applyOriginalF0(syntheticCurve(clip.durationSec));
            processor->requestFullContentRender(clip.key);
            processor->requestFullContentRender(clip.key);
            renderService->resumeRenderWorker();
            pumpMessagesFor(kQuietPumpMs);
            const auto stats = cache->getChunkStats();
            CONTROL_CHECK(stats.pending > 0 && stats.running == 0 && stats.failed == 0
                          && cache->getPendingJobs().empty() && cache->getStateSnapshot().hasPublishedAudio);
            const auto path = outPath("was-corrected.wav");
            // Longer than kControlExportStuckBlocksMs, so that the job does take the blocks for stuck ones.
            const auto job = exportWithin(clipParams(handles.placement(clip.placementId), path));
            note("was-corrected: " + toJsonString(memberOf(job, "state")) + ", error "
                 + toJsonString(memberOf(memberOf(memberOf(job, "error"), "data"), "kind")));
            CONTROL_CHECK(failedWith(job, "NOT_READY", "export_wait_timeout"));
            CONTROL_CHECK(memberOf(job, "result").isNull() && !juceFileOf(path).exists());
            CONTROL_CHECK(!processor->isExportLockHeld());
        }
    }
    // 2f. As 2e, but the blocks no longer hold that audio: the render of the version without the correction failed in
    //     every block (a failed block gives up its audio), was asked for again, and is out of reach of the worker after
    //     one more request at the same revision. Only the published render still holds the corrected audio, which the
    //     export would read; the export waits as well.
    {
        const auto clip = addClip(*processor, media, "was-corrected-failed", 7, 0.0, 2.0, Analysis::Ready);
        auto* owner = processor->getStandaloneContentRepository()->findClip(clip.key);
        const auto cache = renderService->getOrCreateRenderCache(clip.key);
        if (CONTROL_CHECK(owner != nullptr && cache != nullptr)) {
            owner->applyOriginalF0(correctedCurve(clip.durationSec));
            renderService->pauseRenderWorker();
            processor->requestFullContentRender(clip.key);
            CONTROL_CHECK(completePendingRender(*cache, true) > 0 && cache->isCanonicalSettled());
            owner->applyOriginalF0(syntheticCurve(clip.durationSec));
            processor->requestFullContentRender(clip.key);
            CONTROL_CHECK(completePendingRender(*cache, false) > 0);
            processor->requestFullContentRender(clip.key);
            processor->requestFullContentRender(clip.key);
            renderService->resumeRenderWorker();
            pumpMessagesFor(kQuietPumpMs);
            const auto stats = cache->getChunkStats();
            CONTROL_CHECK(stats.pending > 0 && stats.running == 0 && stats.failed == 0
                          && cache->getPendingJobs().empty() && !cache->getStateSnapshot().hasPublishedAudio);
            const auto path = outPath("was-corrected-failed.wav");
            const auto job = exportWithin(clipParams(handles.placement(clip.placementId), path));
            CONTROL_CHECK(failedWith(job, "NOT_READY", "export_wait_timeout"));
            CONTROL_CHECK(memberOf(job, "result").isNull() && !juceFileOf(path).exists());
            CONTROL_CHECK(!processor->isExportLockHeld());
        }
    }
    const auto dispatcher = service.dispatcher();
    CONTROL_CHECK(dispatcher != nullptr && dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

} // namespace ControlTest
} // namespace OpenTune
