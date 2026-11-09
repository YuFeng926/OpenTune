// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scenarios of the jobs (job.get, job.cancel, retention, shutdown), the write envelope with the busy
// checks, and import.start, against the real processor without a window or an audio device. Imported
// clips get a pitch analysis made of synthetic F0 through the import job's analysis seam, committed the
// way OpenTune commits a finished analysis, so no model is needed. Registered with the other processor
// scenarios in ControlProcessorTests.cpp.

#include "ControlContentFixtures.h"
#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Content/EditableContentSnapshot.h"
#include "Content/StandaloneClipContent.h"
#include "Content/StandaloneContentRepository.h"
#include "Control/ControlDispatcher.h"
#include "Control/ControlJobs.h"
#include "Control/Ops/ImportOps.h"
#include "Control/Ops/OpsRegistry.h"
#include "Control/Ops/WriteEnvelope.h"
#include "Utils/AppLogger.h"
#include "Utils/AppPreferences.h"
#include "Utils/PitchCurve.h"
#include "Utils/ProjectSession.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace OpenTune {
namespace ControlTest {

namespace {

/** Sample rate of the test audio (OpenTune stores audio at 44.1 kHz). */
constexpr double kAudioSampleRate = 44100.0;
/** Geometry of the synthetic pitch analysis, the one the F0 extractor produces: 16 kHz, hop 160. */
constexpr double kF0SampleRate = 16000.0;
constexpr int kF0HopSize = 160;
/** The two pitches of the test tone (A3, then B3 from the middle) and its unvoiced tail. */
constexpr float kFirstHz = 220.0f;
constexpr float kSecondHz = 246.94f;
constexpr double kUnvoicedTailSec = 0.3;
constexpr double kSecTolerance = 0.01;
/** The longest hold of job.get, used where a job must end first. */
constexpr int kJobWaitMs = 30000;
/** A short hold of job.get, and the slack of its response time. */
constexpr int kShortWaitMs = 300;
constexpr int kWaitSlackMs = 1000;
/** Bound of a client exchange that includes a job of these tests. */
constexpr int kJobExchangeMs = 60000;
/** How often the thread of a work checks whether it should end. */
constexpr int kWorkerPollMs = 5;

// ---- Files ------------------------------------------------------------------------------------------

juce::File juceFileOf(const std::filesystem::path& path)
{
    return juce::File(juce::String::fromUTF8(path.u8string().c_str()));
}

std::filesystem::path mediaDirectory(const std::string& name)
{
    const auto directory = isolatedDataDirectory() / "media" / name;
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

/** A WAV file of a voiced tone (A3, then B3) of the given length; returns its absolute path (UTF-8). */
std::string writeTone(const std::filesystem::path& directory, const std::string& name, double durationSec,
                      int channels = 1, double sampleRate = kAudioSampleRate, int bitsPerSample = 16)
{
    const auto samples = static_cast<int>(std::llround(durationSec * sampleRate));
    juce::AudioBuffer<float> audio(channels, samples);
    for (int i = 0; i < samples; ++i) {
        const double hz = i < samples / 2 ? kFirstHz : kSecondHz;
        const auto value =
            0.25f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / sampleRate));
        for (int channel = 0; channel < channels; ++channel)
            audio.setSample(channel, i, value);
    }
    const auto path = directory / (name + ".wav");
    const auto file = juceFileOf(path);
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream().release());
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(sampleRate)
                             .withNumChannels(channels)
                             .withBitsPerSample(bitsPerSample);
    auto writer = stream != nullptr ? wav.createWriterFor(stream, options) : nullptr;
    CONTROL_CHECK(writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, samples));
    return path.u8string();
}

/** A file named .wav whose content is no audio. */
std::string writeGarbage(const std::filesystem::path& directory, const std::string& name)
{
    const auto path = directory / name;
    CONTROL_CHECK(juceFileOf(path).replaceWithText("this is not audio"));
    return path.u8string();
}

std::string absentPath(const std::filesystem::path& directory, const std::string& name)
{
    const auto path = directory / name;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return path.u8string();
}

std::string logText()
{
    const auto file = AppLogger::getCurrentLogFile();
    return file == juce::File{} ? std::string() : file.loadFileAsString().toStdString();
}

std::size_t occurrences(const std::string& text, const std::string& part)
{
    std::size_t count = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
        ++count;
    return count;
}

// ---- The synthetic analysis ------------------------------------------------------------------------

/** F0 of a test tone: A3, then B3 from the middle, an unvoiced tail. */
std::shared_ptr<PitchCurve> syntheticCurve(double durationSec)
{
    const auto frames = static_cast<std::size_t>(std::llround(durationSec * kF0SampleRate / kF0HopSize));
    const auto tail = static_cast<std::size_t>(std::llround(kUnvoicedTailSec * kF0SampleRate / kF0HopSize));
    std::vector<float> f0(frames, kFirstHz);
    for (auto frame = frames / 2; frame < frames; ++frame)
        f0[frame] = kSecondHz;
    for (auto frame = frames > tail ? frames - tail : 0; frame < frames; ++frame)
        f0[frame] = 0.0f;
    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(kF0HopSize);
    curve->setSampleRate(kF0SampleRate);
    curve->setOriginalF0(f0);
    return curve;
}

/**
 * The steps of the F0 completion in OpenTuneAudioProcessor::requestContentRefresh with a synthetic
 * curve: the curve goes to the content owner (StandaloneClipContent::applyOriginalF0, which the
 * processor's private writeOriginalF0ToOwner calls for a Standalone clip), then the requested note
 * generation with its callback, then the state Ready.
 */
void commitSyntheticAnalysis(OpenTuneAudioProcessor& processor,
                             const OpenTuneAudioProcessor::ContentRefreshRequest& request)
{
    const auto snapshot = processor.getContentSnapshot(request.contentKey);
    auto* owner = processor.getStandaloneContentRepository()->findClip(request.contentKey);
    if (snapshot == nullptr || snapshot->audioBuffer == nullptr || owner == nullptr)
        return;
    owner->applyOriginalF0(syntheticCurve(snapshot->audioBuffer->getNumSamples() / kAudioSampleRate));
    if (request.generateNotesWholeContentOnReady
        && processor.generateNotesOnlyByContentKey(request.contentKey, request.noteGenerationParams)
        && request.onNotesGenerated)
        request.onNotesGenerated();
    processor.setContentOriginalF0State(request.contentKey, OriginalF0State::Ready);
}

/**
 * Installs the synthetic analysis as the import job's analysis request for the life of this object: it
 * sets the state Extracting at once, as requestContentRefresh does, and commits the analysis from a later
 * message (or never, with completes false). With fails the later message fails the analysis as the
 * processor's completion of a failed extraction does: the reason f0_failed, then the state Failed. Posted
 * messages do nothing once the object is gone.
 */
class SyntheticAnalysis
{
public:
    explicit SyntheticAnalysis(bool completes = true, bool fails = false)
    {
        auto alive = alive_;
        controlSetImportAnalysisForTesting(
            [alive, completes, fails](OpenTuneAudioProcessor& processor,
                                      const OpenTuneAudioProcessor::ContentRefreshRequest& request) {
                if (!processor.setContentOriginalF0State(request.contentKey, OriginalF0State::Extracting))
                    return false;
                if (completes) {
                    auto* target = &processor;
                    juce::MessageManager::callAsync([alive, target, request, fails] {
                        if (!alive->load())
                            return;
                        if (fails) {
                            failAnalysis(*target, request.contentKey, false);
                        } else {
                            commitSyntheticAnalysis(*target, request);
                        }
                    });
                }
                return true;
            });
    }

    ~SyntheticAnalysis()
    {
        *alive_ = false;
        controlSetImportAnalysisForTesting({});
    }

    SyntheticAnalysis(const SyntheticAnalysis&) = delete;
    SyntheticAnalysis& operator=(const SyntheticAnalysis&) = delete;

private:
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

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

// ---- Requests and results --------------------------------------------------------------------------

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

/** The job of jobId once it is terminal (job.get with the longest hold), or null. */
JsonValue waitJob(ApiSession& api, const std::string& jobId)
{
    const auto result = api.result("job.get", json(R"({"jobId":)" + quoted(jobId) + R"(,"waitMs":30000})"));
    return memberOf(result, "job");
}

/** Starts an import and returns its job once terminal; the start must hand the work to a job. */
JsonValue importAndWait(ApiSession& api, const std::string& params)
{
    const auto start = api.result("import.start", json(params));
    const auto& job = memberOf(start, "job");
    if (!CONTROL_CHECK(job.isObject() && memberOf(job, "kind").text() == "import"))
        return JsonValue();
    CONTROL_CHECK(!memberOf(start, "applied").asBoolean() && memberOf(start, "undo").isNull()
                  && memberOf(start, "changes").size() == 0 && memberOf(start, "created").size() == 0);
    const auto& diff = memberOf(start, "revisionDiff");
    CONTROL_CHECK(memberOf(diff, "arrangement").isNull() && memberOf(diff, "contents").size() == 0);
    return waitJob(api, memberOf(job, "jobId").text());
}

std::vector<std::string> warningCodes(const JsonValue& result)
{
    std::vector<std::string> codes;
    const auto& warnings = memberOf(result, "warnings");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        codes.push_back(memberOf(warnings.at(i), "code").text());
    return codes;
}

/**
 * The revert recipe that an import result gives for the clips: one arrangement.edit op delete of exactly these
 * placements, expecting the arrangement revision of the result's tokens, with requiresConsent and no consent member
 * (PROTOCOL.md section 15.2 rules 3 and 4), as the string a result must carry.
 */
std::string deleteRecipe(const JsonValue& result, const std::vector<std::string>& placementIds)
{
    auto ids = JsonValue::array();
    for (const auto& id : placementIds)
        ids.append(JsonValue::string(id));
    auto op = JsonValue::object();
    op.appendMember("op", JsonValue::string("delete"));
    op.appendMember("placementIds", std::move(ids));
    auto ops = JsonValue::array();
    ops.append(std::move(op));
    auto params = JsonValue::object();
    params.appendMember("expectedArrangementRevision", memberOf(memberOf(result, "tokens"), "arrangementRevision"));
    params.appendMember("ops", std::move(ops));
    auto recipe = JsonValue::object();
    recipe.appendMember("method", JsonValue::string("arrangement.edit"));
    recipe.appendMember("params", std::move(params));
    recipe.appendMember("requiresConsent", JsonValue::boolean(true));
    return toJsonString(recipe);
}

/** The undo descriptor of an import result that carries the delete recipe of the clips. */
std::string importUndo(const JsonValue& result, const std::vector<std::string>& placementIds)
{
    return R"({"undoable":false,"reason":"ui_parity","revert":)" + deleteRecipe(result, placementIds) + "}";
}

/** The placementId of the item of an import result. */
std::string placementOf(const JsonValue& result, std::size_t item)
{
    return memberOf(memberOf(result, "items").at(item), "placementId").text();
}

/**
 * Sends a revert recipe the way a client does after the end user agreed: unchanged, plus the consent member of its
 * method (arrangement.edit takes confirm with op delete). The recipe must say that it needs consent and must not carry
 * the member itself.
 */
JsonValue sendDeleteRecipeWithConsent(ApiSession& api, const JsonValue& recipe)
{
    CONTROL_CHECK(memberOf(recipe, "method").text() == "arrangement.edit"
                  && memberOf(recipe, "requiresConsent").isBoolean()
                  && memberOf(recipe, "requiresConsent").asBoolean());
    CONTROL_CHECK(memberOf(memberOf(recipe, "params"), "confirm").isNull());
    auto params = memberOf(recipe, "params");
    params.appendMember("confirm", JsonValue::boolean(true));
    return api.result("arrangement.edit", std::move(params));
}

bool near(const JsonValue& value, double expected, double tolerance = kSecTolerance)
{
    return value.isNumber() && std::abs(value.asDouble() - expected) <= tolerance;
}

const JsonValue& errorKindOf(const JsonValue& errorObject)
{
    return memberOf(memberOf(errorObject, "data"), "kind");
}

const JsonValue& errorReasonOf(const JsonValue& errorObject)
{
    return memberOf(memberOf(errorObject, "data"), "reason");
}

/** A job of the fixture: runs until released, then succeeds; ends cancelled when cancellation was
    requested; counts its steps. */
struct FixtureJob
{
    std::atomic<bool> release{false};
    std::atomic<int> steps{0};
    /** While set, a requested cancellation waits, so that the job stays running and cancel-requested. */
    std::atomic<bool> holdCancellation{false};
};

ControlJobWork fixtureWork(std::shared_ptr<FixtureJob> fixture)
{
    return [fixture](ControlJobStep& step) {
        ++fixture->steps;
        if (step.cancelRequested()) {
            if (!fixture->holdCancellation.load())
                step.cancel();
        } else if (fixture->release.load()) {
            step.succeed(JsonValue::object());
        }
    };
}

/** The work of a job that owns a thread, as work that decodes a file does: its destructor ends and joins
    the thread. Counts its steps and records its destruction. */
struct ThreadedJob
{
    std::atomic<int> steps{0};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> threadRunning{false};
};

ControlJobWork threadedWork(std::shared_ptr<ThreadedJob> record)
{
    struct Owner
    {
        explicit Owner(std::shared_ptr<ThreadedJob> recordIn) : record(std::move(recordIn))
        {
            thread = std::thread([this] {
                record->threadRunning = true;
                while (!stop.load())
                    sleepMs(kWorkerPollMs);
                record->threadRunning = false;
            });
        }
        ~Owner()
        {
            stop = true;
            thread.join();
            record->destroyed = true;
        }
        std::shared_ptr<ThreadedJob> record;
        std::atomic<bool> stop{false};
        std::thread thread;
    };
    auto owner = std::make_shared<Owner>(std::move(record));
    return [owner](ControlJobStep&) { ++owner->record->steps; };
}

} // namespace

// ---- Jobs ------------------------------------------------------------------------------------------

void scenarioJobs()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    const auto jobs = service.jobs();
    ApiSession api;
    if (!service.isRunning() || !CONTROL_CHECK(jobs != nullptr) || !api.open(service))
        return;
    const auto instanceShort = memberOf(api.discovery(), "instanceId").text().substr(0, 8);

    auto running = std::make_shared<FixtureJob>();
    const auto runningId = jobs->start("export", true, fixtureWork(running)).jobId;
    CONTROL_CHECK(runningId == "j:" + instanceShort + ":1");
    CONTROL_CHECK(pumpMessagesUntil([&running] { return running->steps.load() > 0; }, kReadTimeoutMs));

    // job.get never needs the message thread: this thread does not pump while it is answered.
    {
        std::optional<std::string> line;
        std::thread client([&] {
            if (api.client().sendLine(R"({"jsonrpc":"2.0","id":1,"method":"job.get","params":{"jobId":)"
                                      + quoted(runningId) + "}}"))
                line = api.client().readLine();
        });
        client.join();
        if (CONTROL_CHECK(line.has_value())) {
            const auto job = memberOf(memberOf(json(*line), "result"), "job");
            CONTROL_CHECK(memberOf(job, "jobId").text() == runningId && memberOf(job, "kind").text() == "export");
            CONTROL_CHECK(memberOf(job, "state").text() == "running" && memberOf(job, "cancellable").asBoolean());
            CONTROL_CHECK(memberOf(job, "finishedAtMs").isNull() && memberOf(job, "result").isNull()
                          && memberOf(job, "error").isNull() && memberOf(job, "progress").isObject());
        }
    }

    // A short hold returns the running job once waitMs passed.
    {
        const auto started = Clock::now();
        const auto result = api.result(
            "job.get", json(R"({"jobId":)" + quoted(runningId) + R"(,"waitMs":)" + std::to_string(kShortWaitMs) + "}"));
        const auto elapsed = millisecondsSince(started);
        note("job.get with waitMs " + std::to_string(kShortWaitMs) + " answered after " + std::to_string(elapsed)
             + " ms");
        CONTROL_CHECK(elapsed >= kShortWaitMs && elapsed <= kShortWaitMs + kWaitSlackMs);
        CONTROL_CHECK(memberOf(memberOf(result, "job"), "state").text() == "running");
    }

    // A long poll holds its response on the registry's thread: the same session is served meanwhile, and
    // the response comes as soon as the job ends.
    {
        std::optional<std::string> first;
        std::optional<std::string> second;
        long long heldMs = 0;
        runWhilePumping(
            [&] {
                auto& connection = api.client();
                const auto started = Clock::now();
                connection.sendLine(R"({"jsonrpc":"2.0","id":10,"method":"job.get","params":{"jobId":)"
                                    + quoted(runningId) + R"(,"waitMs":30000}})");
                connection.sendLine(R"({"jsonrpc":"2.0","id":11,"method":"job.get","params":{"jobId":)"
                                    + quoted(runningId) + "}}");
                first = connection.readLine();
                running->release = true;
                second = connection.readLine();
                heldMs = millisecondsSince(started);
            },
            kJobExchangeMs);
        CONTROL_CHECK(first && responseIdText(first) == "11");
        if (CONTROL_CHECK(second && responseIdText(second) == "10")) {
            const auto job = memberOf(memberOf(json(*second), "result"), "job");
            CONTROL_CHECK(memberOf(job, "state").text() == "succeeded" && !memberOf(job, "cancellable").asBoolean());
            CONTROL_CHECK(memberOf(job, "finishedAtMs").isNumber() && memberOf(job, "result").isObject()
                          && memberOf(job, "error").isNull());
        }
        CONTROL_CHECK(heldMs < kJobWaitMs);
    }

    // Unknown and malformed ids, a hold beyond the limit.
    CONTROL_CHECK(api.fails("job.get", json(R"({"jobId":"j:00000000:999999"})"), "NOT_FOUND", "job_not_found"));
    CONTROL_CHECK(
        api.fails("job.get", json(R"({"jobId":"j:)" + instanceShort + R"(:999"})"), "NOT_FOUND", "job_not_found"));
    CONTROL_CHECK(api.fails("job.cancel", json(R"({"jobId":"j:00000000:999999"})"), "NOT_FOUND", "job_not_found"));
    CONTROL_CHECK(api.fails("job.get", json(R"({"jobId":"u:00000000:1"})"), "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("job.get", json(R"({"jobId":"j:00000000:01"})"), "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("job.get", json(R"({"jobId":)" + quoted(runningId) + R"(,"waitMs":45000})"),
                            "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("job.cancel", json(R"({"jobId":)" + quoted(runningId) + R"(,"waitMs":0})"),
                            "INVALID_ARGUMENT", "invalid_params"));

    // Cancellation (PROTOCOL.md section 12.3): a terminal job is a no-op success.
    {
        const auto result = api.result("job.cancel", json(R"({"jobId":)" + quoted(runningId) + "}"));
        CONTROL_CHECK(!memberOf(result, "cancelRequested").asBoolean()
                      && memberOf(memberOf(result, "job"), "state").text() == "succeeded");
    }
    // A job that cannot be cancelled continues unchanged.
    auto saving = std::make_shared<FixtureJob>();
    const auto savingId = jobs->start("projectSave", false, fixtureWork(saving)).jobId;
    CONTROL_CHECK(pumpMessagesUntil([&saving] { return saving->steps.load() > 0; }, kReadTimeoutMs));
    CONTROL_CHECK(
        api.fails("job.cancel", json(R"({"jobId":)" + quoted(savingId) + "}"), "UNSUPPORTED", "job_not_cancellable"));
    CONTROL_CHECK(
        memberOf(memberOf(api.result("job.get", json(R"({"jobId":)" + quoted(savingId) + "}")), "job"), "state").text()
        == "running");
    // A cancellable job: requested at once (cancellable turns false), idempotent, then cancelled at its
    // next step.
    auto analysing = std::make_shared<FixtureJob>();
    analysing->holdCancellation = true;
    const auto analysingId = jobs->start("timeGridSeed", true, fixtureWork(analysing)).jobId;
    CONTROL_CHECK(pumpMessagesUntil([&analysing] { return analysing->steps.load() > 0; }, kReadTimeoutMs));
    {
        const auto first = api.result("job.cancel", json(R"({"jobId":)" + quoted(analysingId) + "}"));
        CONTROL_CHECK(memberOf(first, "cancelRequested").asBoolean()
                      && !memberOf(memberOf(first, "job"), "cancellable").asBoolean());
        const auto again = api.result("job.cancel", json(R"({"jobId":)" + quoted(analysingId) + "}"));
        CONTROL_CHECK(memberOf(again, "cancelRequested").asBoolean()
                      && memberOf(memberOf(again, "job"), "state").text() == "running");
        analysing->holdCancellation = false;
        const auto job = waitJob(api, analysingId);
        CONTROL_CHECK(memberOf(job, "state").text() == "cancelled" && !memberOf(job, "cancellable").asBoolean());
        CONTROL_CHECK(errorKindOf(memberOf(job, "error")).text() == "CANCELLED"
                      && errorReasonOf(memberOf(job, "error")).text() == "cancelled_by_request");
        CONTROL_CHECK(!memberOf(memberOf(memberOf(job, "error"), "data"), "executed").asBoolean());
    }
    saving->release = true;
    CONTROL_CHECK(memberOf(waitJob(api, savingId), "state").text() == "succeeded");

    // Retention (section 12.4) on a registry of its own with an injected clock: 64 retained jobs at most;
    // terminal jobs that finished first are evicted first; active jobs never; terminal jobs leave after
    // jobRetentionMs.
    {
        const ControlLimits limits;
        ControlJobs registry(limits);
        registry.setInstanceShort("0123abcd");
        std::int64_t nowMs = 1000000;
        registry.setClockForTesting([&nowMs] { return nowMs; });
        auto hold = std::make_shared<FixtureJob>();
        auto done = std::make_shared<FixtureJob>();
        done->release = true;
        std::vector<std::string> ids;
        for (int i = 0; i < limits.jobRetentionCount; ++i)
            ids.push_back(registry.start("import", true, fixtureWork(i < 2 ? done : hold)).jobId);
        CONTROL_CHECK(std::none_of(ids.begin(), ids.end(), [](const std::string& id) { return id.empty(); }));
        CONTROL_CHECK(registry.start("import", true, fixtureWork(hold)).refusal == "job_limit");
        CONTROL_CHECK(pumpMessagesUntil(
            [&registry, &limits] {
                return registry.activeCount() == static_cast<std::size_t>(limits.jobRetentionCount - 2);
            },
            kReadTimeoutMs));
        const auto next = registry.start("import", true, fixtureWork(hold));
        CONTROL_CHECK(!next.jobId.empty()
                      && registry.retainedCount() == static_cast<std::size_t>(limits.jobRetentionCount));
        JsonValue job;
        CONTROL_CHECK(registry.get(ids[0], job) == ControlJobs::Lookup::NotFound);
        CONTROL_CHECK(registry.get(ids[1], job) == ControlJobs::Lookup::Found);
        nowMs += limits.jobRetentionMs;
        CONTROL_CHECK(registry.get(ids[1], job) == ControlJobs::Lookup::NotFound);
        CONTROL_CHECK(registry.get(ids[2], job) == ControlJobs::Lookup::Found);
        registry.stop();
        CONTROL_CHECK(registry.start("import", true, fixtureWork(hold)).refusal == "stopping");
        std::optional<ControlJobs::Lookup> lookup;
        registry.waitAsync(ids[2], kJobWaitMs, [&lookup](ControlJobs::Lookup result, JsonValue) { lookup = result; });
        CONTROL_CHECK(lookup == ControlJobs::Lookup::Stopped);
    }

    // Retention follows the monotonic clock: a jump of the system time does not evict a terminal job early
    // (section 12.4, "at least jobRetentionMs"); the timestamps follow the wall clock.
    {
        const ControlLimits limits;
        ControlJobs registry(limits);
        registry.setInstanceShort("0123abcd");
        std::int64_t wallMs = 1000000;
        std::int64_t steadyMs = 5000;
        registry.setClockForTesting([&wallMs] { return wallMs; }, [&steadyMs] { return steadyMs; });
        auto done = std::make_shared<FixtureJob>();
        done->release = true;
        const auto id = registry.start("import", true, fixtureWork(done)).jobId;
        CONTROL_CHECK(pumpMessagesUntil([&registry] { return registry.activeCount() == 0; }, kReadTimeoutMs));
        JsonValue job;
        CONTROL_CHECK(registry.get(id, job) == ControlJobs::Lookup::Found
                      && memberOf(job, "finishedAtMs").asDouble() == 1000000.0);
        wallMs += 2 * static_cast<std::int64_t>(limits.jobRetentionMs);
        CONTROL_CHECK(registry.get(id, job) == ControlJobs::Lookup::Found);
        steadyMs += limits.jobRetentionMs - 1;
        CONTROL_CHECK(registry.get(id, job) == ControlJobs::Lookup::Found);
        steadyMs += 1;
        CONTROL_CHECK(registry.get(id, job) == ControlJobs::Lookup::NotFound);
        registry.stop();
    }

    // stop() from a nested message loop inside a step: the timer that fires inside the loop calls no work,
    // the stop does not destroy the timer under its own callback, the interrupted work goes once its step
    // returned, and no work runs afterwards.
    {
        const ControlLimits limits;
        ControlJobs registry(limits);
        struct Record
        {
            std::atomic<int> steps{0};
            std::atomic<int> stepsInsideLoop{0};
            std::atomic<bool> stoppedInsideStep{false};
            std::atomic<bool> destroyed{false};
        };
        auto record = std::make_shared<Record>();
        /** Owned by the work only: destroyed with it. */
        struct Marker
        {
            explicit Marker(std::shared_ptr<Record> recordIn) : record(std::move(recordIn)) {}
            ~Marker() { record->destroyed = true; }
            Marker(const Marker&) = delete;
            Marker& operator=(const Marker&) = delete;
            std::shared_ptr<Record> record;
        };
        auto marker = std::make_shared<Marker>(record);
        auto* target = &registry;
        ControlJobWork work = [record, marker, target](ControlJobStep&) {
            if (++record->steps > 1)
                return;
            // A nested message loop longer than several ticks, then the stop from inside it.
            pumpMessagesFor(kQuietPumpMs);
            record->stepsInsideLoop = record->steps.load();
            juce::MessageManager::callAsync([target] { target->stop(); });
            pumpMessagesUntil([target] { return target->isStopped(); }, kReadTimeoutMs);
            record->stoppedInsideStep = target->isStopped();
        };
        marker.reset();
        const auto id = registry.start("import", true, std::move(work)).jobId;
        CONTROL_CHECK(!id.empty());
        CONTROL_CHECK(pumpMessagesUntil([&record] { return record->stoppedInsideStep.load(); }, kReadTimeoutMs));
        pumpMessagesFor(kQuietPumpMs);
        CONTROL_CHECK(record->stepsInsideLoop.load() == 1 && record->steps.load() == 1);
        CONTROL_CHECK(registry.isStopped() && record->destroyed.load());
        CONTROL_CHECK(registry.start("import", true, fixtureWork(std::make_shared<FixtureJob>())).refusal
                      == "stopping");
    }

    // `executed` of errors (section 17.4): an operation states it; other errors of a write may have
    // changed state.
    CONTROL_CHECK(controlMapError(controlOpError("BUSY", "pointer_down", "busy")).executed
                  == std::optional<bool>(false));
    const auto afterChange = controlMapError(controlOpErrorAfterChange("IO_ERROR", "write_failed", "partly written"));
    CONTROL_CHECK(afterChange.executed == std::optional<bool>(true) && afterChange.kind == "IO_ERROR"
                  && afterChange.reason == "write_failed" && afterChange.detail == "partly written");
    CONTROL_CHECK(!controlMapError(Error(ErrorCode::UnknownError, "a message")).executed.has_value());
    // A job's error object: code, message, retryability of the kind.
    const auto busy = controlErrorObjectJson({"BUSY", "job_limit", "full", false});
    CONTROL_CHECK(memberOf(busy, "code").asDouble() == -32000 && memberOf(busy, "message").text() == "BUSY");
    CONTROL_CHECK(memberOf(memberOf(busy, "data"), "retryable").asBoolean());
    CONTROL_CHECK(
        memberOf(controlErrorObjectJson({"INVALID_ARGUMENT", "audio_too_long", "long", false}), "code").asDouble()
        == -32602);
}

void scenarioJobShutdown()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    const auto jobs = service.jobs();
    ApiSession api;
    if (!service.isRunning() || !CONTROL_CHECK(jobs != nullptr) || !api.open(service))
        return;
    // A running job whose work owns a thread, and a job.get that waits for it.
    auto record = std::make_shared<ThreadedJob>();
    const auto jobId = jobs->start("export", true, threadedWork(record)).jobId;
    CONTROL_CHECK(pumpMessagesUntil([&record] { return record->steps.load() > 0 && record->threadRunning.load(); },
                                    kReadTimeoutMs));
    std::optional<std::string> response;
    std::thread client([&] {
        if (api.client().sendLine(R"({"jsonrpc":"2.0","id":7,"method":"job.get","params":{"jobId":)" + quoted(jobId)
                                  + R"(,"waitMs":30000}})"))
            response = api.client().readLine(kJobWaitMs);
    });
    CONTROL_CHECK(waitUntil([&jobs] { return jobs->waitingCount() == 1; }, kWaiterSettleMs));

    const auto started = Clock::now();
    processor.reset();
    const auto elapsed = millisecondsSince(started);
    note("processor destroyed in " + std::to_string(elapsed) + " ms");
    CONTROL_CHECK(elapsed <= kShutdownBoundMs);
    client.join();
    if (CONTROL_CHECK(response.has_value())) {
        note("response: " + *response);
        CONTROL_CHECK(isError(response, "SHUTTING_DOWN", ""));
        // Keep the parsed response: errorData() points into it.
        const JsonValue parsed = json(*response);
        const JsonValue* data = errorData(parsed);
        CONTROL_CHECK(data != nullptr && !memberOf(*data, "executed").asBoolean());
    }
    // The work was destroyed during the stop, its thread joined; no work runs afterwards.
    CONTROL_CHECK(record->destroyed.load() && !record->threadRunning.load());
    CONTROL_CHECK(jobs->isStopped() && jobs->waitingCount() == 0);
    JsonValue job;
    CONTROL_CHECK(jobs->get(jobId, job) == ControlJobs::Lookup::Stopped);
    const int stepsAtStop = record->steps.load();
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(record->steps.load() == stepsAtStop);
}

// ---- The write envelope ---------------------------------------------------------------------------

void scenarioWriteEnvelope()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    ApiSession api;
    if (!service.isRunning() || !api.open(service))
        return;
    TestUiBridge bridge;
    ControlThreadProbe probe;
    ControlInstanceFacts facts;
    facts.instanceId = memberOf(api.discovery(), "instanceId").text();
    facts.capabilities = {"session.v1", "read.v1"};
    ControlOpContext context(*processor, &bridge, facts, probe);
    auto* arrangement = processor->getStandaloneArrangement();

    const char* const members[] = {"applied", "revisionDiff", "tokens", "undo",     "dirty",
                                   "ui",      "sideEffects",  "noOp",   "dryRun",   "changes",
                                   "created", "removed",      "idMap",  "warnings", "job"};
    const auto hasEveryMember = [&members](const JsonValue& envelope) {
        return std::all_of(std::begin(members), std::end(members),
                           [&envelope](const char* member) { return envelope.find(member) != nullptr; });
    };

    // Committed without changes: noOp, undo no_op, no IRREVERSIBLE.
    {
        ControlWriteEnvelope envelope(context);
        const auto result = envelope.committed(context);
        CONTROL_CHECK(hasEveryMember(result));
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && memberOf(result, "noOp").asBoolean()
                      && !memberOf(result, "dryRun").asBoolean() && memberOf(result, "job").isNull());
        CONTROL_CHECK(toJsonString(memberOf(result, "undo")) == R"({"undoable":false,"reason":"no_op","revert":null})"
                      && warningCodes(result).empty());
        CONTROL_CHECK(memberOf(memberOf(result, "revisionDiff"), "trackState").isNull());
        CONTROL_CHECK(memberOf(result, "ui").isObject() && memberOf(result, "nullReasons").isNull());
    }
    // A no-op keeps the descriptor its method set (section 15.3 rule 5: an undo step that changed
    // nothing); a no-op is never IRREVERSIBLE.
    {
        ControlWriteEnvelope undoStep(context);
        undoStep.setUndoable("u:" + facts.instanceId.substr(0, 8) + ":7", "Move notes", {"NoteAction"});
        const auto result = undoStep.committed(context);
        CONTROL_CHECK(memberOf(result, "noOp").asBoolean() && memberOf(memberOf(result, "undo"), "undoable").asBoolean()
                      && memberOf(memberOf(result, "undo"), "undoId").text()
                             == "u:" + facts.instanceId.substr(0, 8) + ":7");
        ControlWriteEnvelope notUndoable(context);
        notUndoable.setNotUndoable("ui_parity");
        const auto kept = notUndoable.committed(context);
        CONTROL_CHECK(memberOf(kept, "noOp").asBoolean()
                      && memberOf(memberOf(kept, "undo"), "reason").text() == "ui_parity"
                      && warningCodes(kept).empty());
    }
    // A write that only had a side effect is no no-op.
    {
        ControlWriteEnvelope envelope(context);
        auto effect = JsonValue::object();
        effect.appendMember("from", JsonValue::string("arrangement"));
        effect.appendMember("to", JsonValue::string("pianoRoll"));
        envelope.addSideEffect("workspaceSwitched", std::move(effect));
        envelope.setNotUndoable("not_persisted_state");
        const auto result = envelope.committed(context);
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean());
        CONTROL_CHECK(memberOf(memberOf(result, "sideEffects"), "workspaceSwitched").isObject()
                      && memberOf(memberOf(result, "undo"), "reason").text() == "not_persisted_state"
                      && warningCodes(result).empty());
    }
    // Committed with a change that a recipe reverts: revisionDiff shows it, dirty after the mark.
    {
        ControlWriteEnvelope envelope(context);
        CONTROL_CHECK(arrangement->setTrackMuted(0, true));
        controlMarkDirty(context);
        auto ref = JsonValue::object();
        ref.appendMember("trackId", JsonValue::integer(0));
        auto fields = json(R"({"muted":{"before":false,"after":true,"unit":null}})");
        envelope.addChange("track", ref, JsonValue::string("Track 1"), fields);
        envelope.setNotUndoable("no_upstream_undo",
                                controlRevertRecipe("track.set", json(R"({"trackId":0,"mute":false})")));
        const auto result = envelope.committed(context);
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean());
        CONTROL_CHECK(memberOf(memberOf(result, "revisionDiff"), "trackState").isObject());
        CONTROL_CHECK(memberOf(result, "changes").size() == 1 && warningCodes(result).empty());
        CONTROL_CHECK(memberOf(memberOf(memberOf(result, "undo"), "revert"), "method").text() == "track.set");
        CONTROL_CHECK(!memberOf(memberOf(result, "dirty"), "before").asBoolean()
                      && memberOf(memberOf(result, "dirty"), "after").asBoolean());
    }
    // Neither undoable nor revertible: IRREVERSIBLE, once.
    {
        ControlWriteEnvelope envelope(context);
        CONTROL_CHECK(arrangement->setTrackMuted(0, false));
        envelope.addWarning("IRREVERSIBLE");
        envelope.setNotUndoable("ui_parity");
        const auto result = envelope.committed(context);
        CONTROL_CHECK(warningCodes(result) == std::vector<std::string>{"IRREVERSIBLE"});
        CONTROL_CHECK(memberOf(memberOf(result, "undo"), "revert").isNull());
    }
    // View state is exempt from IRREVERSIBLE; an undoable change carries its descriptor.
    {
        ControlWriteEnvelope envelope(context);
        CONTROL_CHECK(arrangement->setTrackSolo(0, true));
        envelope.setNotUndoable("not_persisted_state");
        CONTROL_CHECK(warningCodes(envelope.committed(context)).empty());
        ControlWriteEnvelope undoable(context);
        CONTROL_CHECK(arrangement->setTrackSolo(0, false));
        undoable.setUndoable("u:" + facts.instanceId.substr(0, 8) + ":1", "Solo", {"TrackStateAction"});
        const auto result = undoable.committed(context);
        const auto& undo = memberOf(result, "undo");
        CONTROL_CHECK(memberOf(undo, "undoable").asBoolean() && memberOf(undo, "steps").asDouble() == 1
                      && memberOf(undo, "innerActions").size() == 1 && warningCodes(result).empty());
    }
    // A prediction and a job hand-over change nothing.
    {
        ControlWriteEnvelope envelope(context);
        envelope.addChange("track", json(R"({"trackId":0})"), JsonValue(),
                           json(R"({"muted":{"before":false,"after":true,"unit":null}})"));
        const auto predicted = envelope.predicted(context);
        CONTROL_CHECK(!memberOf(predicted, "applied").asBoolean() && memberOf(predicted, "dryRun").asBoolean()
                      && memberOf(predicted, "undo").isNull() && memberOf(predicted, "changes").size() == 1);
        // What a method records for its job's result does not show in the hand-over (section 14.2 rule 3).
        ControlWriteEnvelope handed(context);
        handed.addChange("track", json(R"({"trackId":0})"), JsonValue(),
                         json(R"({"muted":{"before":false,"after":true,"unit":null}})"));
        handed.addSideEffect("tracksRevealed", json(R"({"trackIds":[1],"revert":null})"));
        const auto result =
            handed.handedToJob(context, json(R"({"jobId":"j:01234567:1","kind":"import","state":"queued"})"));
        CONTROL_CHECK(!memberOf(result, "applied").asBoolean() && !memberOf(result, "dryRun").asBoolean()
                      && !memberOf(result, "noOp").asBoolean()
                      && memberOf(memberOf(result, "job"), "kind").text() == "import"
                      && memberOf(result, "undo").isNull());
        CONTROL_CHECK(memberOf(result, "changes").size() == 0 && memberOf(result, "created").size() == 0
                      && memberOf(result, "removed").size() == 0 && memberOf(result, "idMap").size() == 0);
        CONTROL_CHECK(toJsonString(memberOf(result, "revisionDiff"))
                      == R"({"arrangement":null,"trackState":null,"contents":[]})");
        CONTROL_CHECK(toJsonString(memberOf(result, "sideEffects")) == "{}");
    }
    // Without an editor the UI snapshot is null with its reason.
    {
        ControlOpContext withoutEditor(*processor, nullptr, facts, probe);
        ControlWriteEnvelope envelope(withoutEditor);
        const auto result = envelope.committed(withoutEditor);
        CONTROL_CHECK(memberOf(result, "ui").isNull()
                      && memberOf(memberOf(result, "nullReasons"), "ui").text() == "no_editor");
        CONTROL_CHECK(!controlCheckBusy(withoutEditor).has_value());
    }
    // The busy checks (section 13.2), in their order; ui.dismiss ignores modal dialogs.
    {
        CONTROL_CHECK(!controlCheckBusy(context).has_value());
        const auto reasonOf = [&context](ControlBusyScope scope) {
            const auto busy = controlCheckBusy(context, scope);
            return busy ? controlMapError(*busy).reason : std::string();
        };
        bridge.busy.operationRunning = true;
        CONTROL_CHECK(reasonOf(ControlBusyScope::Write) == "project_operation");
        bridge.busy.exportRunning = true;
        CONTROL_CHECK(reasonOf(ControlBusyScope::Write) == "export_in_progress");
        bridge.busy.importRunning = true;
        CONTROL_CHECK(reasonOf(ControlBusyScope::Write) == "import_in_progress");
        bridge.busy.modalOpen = true;
        CONTROL_CHECK(reasonOf(ControlBusyScope::Write) == "modal_open");
        CONTROL_CHECK(reasonOf(ControlBusyScope::DialogDismiss) == "import_in_progress");
        bridge.busy.pointerDown = true;
        CONTROL_CHECK(reasonOf(ControlBusyScope::Write) == "pointer_down");
        const auto busy = controlCheckBusy(context);
        CONTROL_CHECK(busy && controlMapError(*busy).kind == "BUSY"
                      && controlMapError(*busy).executed == std::optional<bool>(false));
    }
    // A job keeps the environment of the operation that started it and makes a context for each step.
    {
        const auto environment = context.environment();
        ControlOpContext step(environment);
        CONTROL_CHECK(&step.processor() == processor.get() && step.bridge() == &bridge && step.jobs() == nullptr);
        CONTROL_CHECK(step.instanceShort() == facts.instanceId.substr(0, 8));
    }
    CONTROL_CHECK(probe.otherThreadAccesses.load() == 0);
}
// ---- import.start ---------------------------------------------------------------------------------

void scenarioImport()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    bridge.visibleTracks = 1;
    ScopedBridge registered(service, bridge);
    SyntheticAnalysis analysis;
    ApiSession api;
    if (!api.open(service))
        return;
    auto& preferences = processor->getAppPreferences();
    const auto copiesBefore = controlImportCommitCopiesForTesting();
    const auto media = mediaDirectory("import");
    const auto take1 = writeTone(media, "take1", 2.0);
    const auto take2 = writeTone(media, "take2", 1.5, 2, 48000.0); // stereo, resampled
    const auto take3 = writeTone(media, "take3", 1.0);
    const auto garbled = writeGarbage(media, "garbled.wav");

    // 1. openTune scheme: one file onto track 0 at 1 s; it is focused, no notes are generated.
    preferences.setAudioEditingScheme(AudioEditingScheme::Scheme::CorrectedF0Primary);
    // The arrangement is shown, the keyboard focus is in the tempo field and the tempo has two decimals, which a switch
    // of the view would round: the focus of an import loads the clip into the piano roll without switching the view
    // (PROTOCOL.md section 21 rule 2), so the tempo stays and the result reports nothing about it (section 14.6).
    bridge.view.workspace = "arrangement";
    bridge.focusTempoField(*processor);
    processor->setBpm(120.27);
    {
        const auto job = importAndWait(api, R"({"items":[{"path":)" + quoted(take1)
                                                + R"(}],"layout":"single","trackId":0,"startTimelineSec":1})");
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = memberOf(job, "result");
        const auto& item = memberOf(result, "items").at(0);
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean()
                      && memberOf(result, "job").isNull());
        CONTROL_CHECK(memberOf(item, "imported").asBoolean() && memberOf(item, "trackId").asDouble() == 0
                      && near(memberOf(item, "startTimelineSec"), 1.0) && near(memberOf(item, "durationSec"), 2.0));
        CONTROL_CHECK(memberOf(item, "channelCount").asDouble() == 1 && !memberOf(item, "resampled").asBoolean()
                      && near(memberOf(item, "originalSampleRateHz"), kAudioSampleRate));
        CONTROL_CHECK(memberOf(item, "f0State").text() == "ready" && !memberOf(item, "notesGenerated").asBoolean()
                      && memberOf(item, "notesGeneratedBy").isNull() && memberOf(item, "error").isNull());
        CONTROL_CHECK(toJsonString(memberOf(result, "counts")) == R"({"requested":1,"imported":1,"failed":0})");
        // No undo entry (ui_parity); the revert recipe deletes the clip, with the consent of the end user, so the
        // result is not IRREVERSIBLE.
        CONTROL_CHECK(toJsonString(memberOf(result, "undo")) == importUndo(result, {placementOf(result, 0)}));
        CONTROL_CHECK(warningCodes(result).empty());
        CONTROL_CHECK(memberOf(result, "created").size() == 2);
        const auto& diff = memberOf(result, "revisionDiff");
        CONTROL_CHECK(memberOf(diff, "arrangement").isObject() && memberOf(diff, "contents").size() == 1);
        const auto& changed = memberOf(memberOf(diff, "contents").at(0), "changed");
        CONTROL_CHECK(memberOf(memberOf(changed, "content"), "before").isNull());
        CONTROL_CHECK(toJsonString(memberOf(result, "dirty")) == R"({"before":false,"after":true})");
        const auto& tokens = memberOf(result, "tokens");
        CONTROL_CHECK(memberOf(memberOf(tokens, "contents").at(0), "contentId").text()
                      == memberOf(item, "contentId").text());
        CONTROL_CHECK(memberOf(memberOf(tokens, "placements").at(0), "placementId").text()
                      == memberOf(item, "placementId").text());
        CONTROL_CHECK(memberOf(result, "sideEffects").size() == 0);
        CONTROL_CHECK(bridge.focused.size() == 1 && bridge.focused.back().first == 0 && bridge.structureChanges == 1);
        // The default focus selects the imported clip, as the UI's import does.
        auto& arrangement = *processor->getStandaloneArrangement();
        CONTROL_CHECK(arrangement.getActiveTrackId() == 0 && arrangement.getSelectedPlacementId(0) != 0);
        CONTROL_CHECK(memberOf(memberOf(result, "ui"), "activeTrackId").asDouble() == 0);
        // The clip is loaded into the piano roll and the view stays: no switch, no tempo rounded.
        CONTROL_CHECK(bridge.view.pianoRollPlacementId == arrangement.getSelectedPlacementId(0)
                      && bridge.view.workspace == "arrangement" && bridge.workspaceSwitches == 0);
        CONTROL_CHECK(reportsNoTempoChange(result) && std::abs(processor->getBpm() - 120.27) < 1e-9);
        // project.get shows the clip with its source file.
        const auto project = api.result("project.get");
        const auto& placements = memberOf(project, "placements");
        CONTROL_CHECK(placements.size() == 1 && memberOf(placements.at(0), "sourcePath").text() == take1);
    }
    bridge.focusElsewhere();
    processor->setBpm(120.0);

    // 2. openDyne scheme, sequential onto the first track without clips (track 1, hidden: revealed); the
    //    focus makes OpenTune generate the notes of the last clip.
    preferences.setAudioEditingScheme(AudioEditingScheme::Scheme::NotesPrimary);
    {
        const auto job = importAndWait(api, R"({"items":[{"path":)" + quoted(take1) + R"(},{"path":)" + quoted(take2)
                                                + R"(}],"layout":"sequential","newTrack":true})");
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = memberOf(job, "result");
        const auto& first = memberOf(result, "items").at(0);
        const auto& second = memberOf(result, "items").at(1);
        CONTROL_CHECK(memberOf(first, "trackId").asDouble() == 1 && memberOf(second, "trackId").asDouble() == 1);
        CONTROL_CHECK(near(memberOf(first, "startTimelineSec"), 0.0)
                      && near(memberOf(second, "startTimelineSec"), memberOf(first, "startTimelineSec").asDouble()
                                                                        + memberOf(first, "durationSec").asDouble()));
        CONTROL_CHECK(memberOf(second, "channelCount").asDouble() == 2 && memberOf(second, "resampled").asBoolean()
                      && near(memberOf(second, "originalSampleRateHz"), 48000.0)
                      && near(memberOf(second, "durationSec"), 1.5));
        CONTROL_CHECK(!memberOf(first, "notesGenerated").asBoolean() && memberOf(second, "notesGenerated").asBoolean()
                      && memberOf(second, "notesGeneratedBy").text() == "focus");
        CONTROL_CHECK(
            (warningCodes(result) == std::vector<std::string>{"TRACK_AUTO_REVEALED", "SELECTION_GENERATED_NOTES"}));
        CONTROL_CHECK(toJsonString(memberOf(result, "undo"))
                      == importUndo(result, {placementOf(result, 0), placementOf(result, 1)}));
        const auto& sideEffects = memberOf(result, "sideEffects");
        CONTROL_CHECK(toJsonString(memberOf(memberOf(sideEffects, "notesGenerated"), "contentIds"))
                      == "[" + quoted(memberOf(second, "contentId").text()) + "]");
        // The notes belong to the focused clip alone: the recipe of the side effect deletes that clip, and not the
        // first one.
        CONTROL_CHECK(toJsonString(memberOf(memberOf(sideEffects, "notesGenerated"), "revert"))
                      == deleteRecipe(result, {placementOf(result, 1)}));
        CONTROL_CHECK(toJsonString(memberOf(memberOf(sideEffects, "tracksRevealed"), "trackIds")) == "[1]");
        CONTROL_CHECK(bridge.visibleTracks == 2 && bridge.revealCalls == 1);
        CONTROL_CHECK(bridge.focused.size() == 2 && bridge.focused.back().first == 1);
        const auto notes = api.result("content.getNotes",
                                      json(R"({"contentId":)" + quoted(memberOf(second, "contentId").text()) + "}"));
        CONTROL_CHECK(memberOf(notes, "notes").size() >= 1);
        CONTROL_CHECK(memberOf(memberOf(result, "revisionDiff"), "contents").size() == 2);
    }

    // 3. Notes on request in separate tracks without focus; a file that cannot be decoded fails alone.
    {
        const auto focusedBefore = bridge.focused.size();
        auto& arrangement = *processor->getStandaloneArrangement();
        const int activeBefore = arrangement.getActiveTrackId();
        const auto selectedBefore = arrangement.getSelectedPlacementId(activeBefore);
        CONTROL_CHECK(activeBefore == 1 && selectedBefore != 0); // selected by the focus of the import above
        const auto job =
            importAndWait(api, R"({"items":[{"path":)" + quoted(take3) + R"(},{"path":)" + quoted(garbled)
                                   + R"(}],"layout":"separateTracks","trackId":3,"generateNotes":true,"focus":false})");
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = memberOf(job, "result");
        const auto& first = memberOf(result, "items").at(0);
        const auto& second = memberOf(result, "items").at(1);
        CONTROL_CHECK(memberOf(first, "trackId").asDouble() == 3 && memberOf(first, "notesGenerated").asBoolean()
                      && memberOf(first, "notesGeneratedBy").text() == "request");
        CONTROL_CHECK(!memberOf(second, "imported").asBoolean() && memberOf(second, "trackId").isNull()
                      && memberOf(second, "f0State").isNull() && !memberOf(second, "notesGenerated").asBoolean());
        CONTROL_CHECK(errorKindOf(memberOf(second, "error")).text() == "UNSUPPORTED"
                      && errorReasonOf(memberOf(second, "error")).text() == "unsupported_format");
        CONTROL_CHECK(toJsonString(memberOf(result, "counts")) == R"({"requested":2,"imported":1,"failed":1})");
        CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"TRACK_AUTO_REVEALED", "TRACK_AUTO_REVEALED"}));
        // Only the clip that was committed is listed; the file that failed has none.
        CONTROL_CHECK(toJsonString(memberOf(result, "undo")) == importUndo(result, {placementOf(result, 0)}));
        CONTROL_CHECK(memberOf(memberOf(result, "sideEffects"), "notesGenerated").isNull());
        CONTROL_CHECK(bridge.focused.size() == focusedBefore);
        // focus false leaves the selection as it was. OpenTune's commit selects the clip it commits; the job puts the
        // selection back, so the editor's heartbeat cannot load the clip and the openDyne scheme generates no notes
        // for it (methods/import.start.schema.json, PROTOCOL.md section 21 rule 2).
        CONTROL_CHECK(arrangement.getActiveTrackId() == activeBefore
                      && arrangement.getSelectedPlacementId(activeBefore) == selectedBefore);
        CONTROL_CHECK(memberOf(memberOf(result, "ui"), "activeTrackId").asDouble() == activeBefore);
        CONTROL_CHECK(memberOf(api.result("project.get"), "activeTrackId").asDouble() == activeBefore);
    }

    // 3b. focus false on the active track, which already has a selected clip: the commit selects the new clip on that
    //     track, and the job puts the selection back.
    {
        const auto focusedBefore = bridge.focused.size();
        auto& arrangement = *processor->getStandaloneArrangement();
        const int activeBefore = arrangement.getActiveTrackId();
        const auto selectedBefore = arrangement.getSelectedPlacementId(activeBefore);
        CONTROL_CHECK(activeBefore == 1 && selectedBefore != 0);
        const auto job = importAndWait(api, R"({"items":[{"path":)" + quoted(take3)
                                                + R"(}],"layout":"single","trackId":1,"focus":false})");
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = memberOf(job, "result");
        const auto& item = memberOf(result, "items").at(0);
        CONTROL_CHECK(memberOf(item, "imported").asBoolean() && !memberOf(item, "notesGenerated").asBoolean()
                      && memberOf(item, "notesGeneratedBy").isNull());
        CONTROL_CHECK(bridge.focused.size() == focusedBefore);
        CONTROL_CHECK(arrangement.getActiveTrackId() == activeBefore
                      && arrangement.getSelectedPlacementId(activeBefore) == selectedBefore);
        CONTROL_CHECK(memberOf(memberOf(result, "ui"), "activeTrackId").asDouble() == activeBefore);
    }

    // 4. Checks before the job: nothing starts and executed is false.
    const auto retained = service.jobs()->retainedCount();
    const auto take1Item = R"({"items":[{"path":)" + quoted(take1) + "}]";
    CONTROL_CHECK(api.fails("import.start", json(R"({"items":[{"path":"take1.wav"}],"layout":"single"})"),
                            "INVALID_ARGUMENT", "path_not_absolute"));
    CONTROL_CHECK(api.fails("import.start",
                            json(R"({"items":[{"path":)" + quoted(take1 + ".txt") + R"(}],"layout":"single"})"),
                            "INVALID_ARGUMENT", "path_bad_extension"));
    const int tracks = processor->getStandaloneArrangement()->getNumTracks();
    CONTROL_CHECK(api.fails("import.start",
                            json(take1Item + R"(,"layout":"single","trackId":)" + std::to_string(tracks) + "}"),
                            "NOT_FOUND", "unknown_handle"));
    CONTROL_CHECK(api.fails("import.start",
                            json(R"({"items":[{"path":)" + quoted(take1) + R"(},{"path":)" + quoted(take2)
                                 + R"(}],"layout":"separateTracks","trackId":)" + std::to_string(tracks - 1) + "}"),
                            "INVALID_ARGUMENT", "track_capacity"));
    CONTROL_CHECK(api.fails(
        "import.start",
        json(R"({"items":[{"path":)" + quoted(take1) + R"(},{"path":)" + quoted(take2) + R"(}],"layout":"single"})"),
        "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("import.start", json(take1Item + R"(,"layout":"single","trackId":0,"newTrack":false})"),
                            "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(api.fails("import.start", json(take1Item + R"(,"layout":"single","idempotencyKey":"a b"})"),
                            "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(
        api.fails("import.start", json(take1Item + R"(,"layout":"diagonal"})"), "INVALID_ARGUMENT", "invalid_params"));
    // The UI has priority: every busy condition refuses the write; reads and job.get are never refused.
    const std::pair<bool ControlUiBridge::BusyState::*, const char*> conditions[] = {
        {&ControlUiBridge::BusyState::pointerDown, "pointer_down"},
        {&ControlUiBridge::BusyState::modalOpen, "modal_open"},
        {&ControlUiBridge::BusyState::importRunning, "import_in_progress"},
        {&ControlUiBridge::BusyState::exportRunning, "export_in_progress"},
        {&ControlUiBridge::BusyState::operationRunning, "project_operation"},
    };
    for (const auto& [member, reason] : conditions) {
        bridge.busy = {};
        bridge.busy.*member = true;
        CONTROL_CHECK(
            api.fails("import.start", json(take1Item + R"(,"layout":"single","trackId":4})"), "BUSY", reason));
        CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 5);
    }
    CONTROL_CHECK(api.result("job.get", json(R"({"jobId":"j:)"
                                             + memberOf(api.discovery(), "instanceId").text().substr(0, 8) + R"(:1"})"))
                      .isObject());
    bridge.busy = {};
    CONTROL_CHECK(service.jobs()->retainedCount() == retained);

    // 5. Idempotency (section 12.5): the same key and params return the same job, other params fail.
    {
        const auto params =
            take1Item + R"(,"layout":"single","trackId":5,"focus":false,"idempotencyKey":"take1-on-5"})";
        const auto first = api.result("import.start", json(params));
        const auto jobId = memberOf(memberOf(first, "job"), "jobId").text();
        const auto again = api.result("import.start", json(params));
        CONTROL_CHECK(!jobId.empty() && memberOf(memberOf(again, "job"), "jobId").text() == jobId);
        CONTROL_CHECK(api.fails(
            "import.start",
            json(take1Item + R"(,"layout":"single","trackId":6,"focus":false,"idempotencyKey":"take1-on-5"})"),
            "INVALID_ARGUMENT", "idempotency_key_reused"));
        CONTROL_CHECK(memberOf(waitJob(api, jobId), "state").text() == "succeeded");
        const auto afterEnd = api.result("import.start", json(params));
        CONTROL_CHECK(memberOf(memberOf(afterEnd, "job"), "jobId").text() == jobId
                      && memberOf(memberOf(afterEnd, "job"), "state").text() == "succeeded");
        CONTROL_CHECK(processor->getStandaloneArrangement()->getNumPlacements(5) == 1);
    }
    // No project was opened, so no clip id could be in use: every commit was a single one without a copy
    // of the audio, as the UI's.
    CONTROL_CHECK(controlImportCommitCopiesForTesting() == copiesBefore);
    const auto dispatcher = service.dispatcher();
    CONTROL_CHECK(dispatcher != nullptr && dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

void scenarioImportAfterOpen()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    ApiSession api;
    if (!service.isRunning() || !api.open(service))
        return;
    SyntheticAnalysis analysis;
    const auto media = mediaDirectory("reopen");
    const auto take1 = writeTone(media, "take1", 1.0);
    const auto take2 = writeTone(media, "take2", 1.0);
    const auto take3 = writeTone(media, "take3", 1.0);
    for (const auto& [path, track] : {std::pair<std::string, int>{take1, 0}, std::pair<std::string, int>{take2, 1}})
        CONTROL_CHECK(
            memberOf(importAndWait(api, R"({"items":[{"path":)" + quoted(path) + R"(}],"layout":"single","trackId":)"
                                            + std::to_string(track) + "}"),
                     "state")
                .text()
            == "succeeded");

    // Save and open the project with the editor's two-phase flows: opening clears the contents and
    // restarts clip ids at 1, while the project's clips keep theirs.
    {
        ProjectSession session(*processor, processor->getAppPreferences());
        const auto directory = isolatedDataDirectory() / "project";
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        const auto file = juceFileOf(directory / "reopen.otproj");
        auto task = session.prepareSave(file);
        CONTROL_CHECK(ProjectSession::executeSaveToFile(task).ok());
        auto prepared = session.prepareOpen(file, Language::English);
        if (!CONTROL_CHECK(prepared.ok()))
            return;
        CONTROL_CHECK(session.commitPreparedOpen(std::move(prepared).value()).ok());
    }
    CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 2);

    // The import commits although the first clip ids are in use; the repeated commits are logged, and the
    // attempts that may be repeated commit a copy of the audio. focus false: the failed attempts and the accepted
    // one leave the selection as it was.
    const auto repeatedBefore = occurrences(logText(), "was repeated");
    const auto copiesBefore = controlImportCommitCopiesForTesting();
    auto& arrangement = *processor->getStandaloneArrangement();
    const int activeBefore = arrangement.getActiveTrackId();
    const auto selectedBefore = arrangement.getSelectedPlacementId(activeBefore);
    const auto job = importAndWait(api, R"({"items":[{"path":)" + quoted(take3)
                                            + R"(}],"layout":"single","trackId":2,"focus":false})");
    CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
    const auto& item = memberOf(memberOf(job, "result"), "items").at(0);
    CONTROL_CHECK(memberOf(item, "imported").asBoolean() && memberOf(item, "f0State").text() == "ready");
    CONTROL_CHECK(occurrences(logText(), "was repeated") == repeatedBefore + 1);
    // The result carries the warning IMPORT_COMMIT_REPEATED for the placement, with the number of repeats.
    {
        const auto& warnings = memberOf(memberOf(job, "result"), "warnings");
        std::size_t found = 0;
        for (std::size_t i = 0; i < warnings.size(); ++i) {
            const auto& warning = warnings.at(i);
            if (memberOf(warning, "code").text() != "IMPORT_COMMIT_REPEATED")
                continue;
            ++found;
            CONTROL_CHECK(memberOf(memberOf(warning, "subject"), "placementId").text()
                          == memberOf(item, "placementId").text());
            CONTROL_CHECK(memberOf(memberOf(warning, "data"), "repeatCount").asDouble() >= 1);
        }
        CONTROL_CHECK(found == 1);
    }
    CONTROL_CHECK(controlImportCommitCopiesForTesting() > copiesBefore);
    // The repeated commit selected the clip it finally accepted; the job put the selection back.
    CONTROL_CHECK(arrangement.getActiveTrackId() == activeBefore
                  && arrangement.getSelectedPlacementId(activeBefore) == selectedBefore);
    const auto project = api.result("project.get");
    CONTROL_CHECK(memberOf(project, "placements").size() == 3 && memberOf(project, "contents").size() == 3);

    // Every clip id is now behind the one that import received: the next import commits once, without a
    // copy, and nothing is repeated.
    const auto take4 = writeTone(media, "take4", 1.0);
    const auto copiesAfter = controlImportCommitCopiesForTesting();
    const auto next =
        importAndWait(api, R"({"items":[{"path":)" + quoted(take4) + R"(}],"layout":"single","trackId":3})");
    CONTROL_CHECK(memberOf(next, "state").text() == "succeeded");
    CONTROL_CHECK(memberOf(memberOf(memberOf(next, "result"), "items").at(0), "imported").asBoolean());
    // Committed at once: no warning.
    {
        const auto& nextWarnings = memberOf(memberOf(next, "result"), "warnings");
        for (std::size_t i = 0; i < nextWarnings.size(); ++i)
            CONTROL_CHECK(memberOf(nextWarnings.at(i), "code").text() != "IMPORT_COMMIT_REPEATED");
    }
    CONTROL_CHECK(controlImportCommitCopiesForTesting() == copiesAfter);
    CONTROL_CHECK(occurrences(logText(), "was repeated") == repeatedBefore + 1);
    // This one focuses the clip it commits, as the default says.
    CONTROL_CHECK(arrangement.getActiveTrackId() == 3 && arrangement.getSelectedPlacementId(3) != 0);
    CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 4);
}

void scenarioImportCancel()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    bridge.visibleTracks = 4;
    ScopedBridge registered(service, bridge);
    SyntheticAnalysis analysis;
    ApiSession api;
    if (!api.open(service))
        return;
    const auto media = mediaDirectory("cancel");
    std::string items;
    for (int i = 1; i <= 3; ++i)
        items += std::string(i > 1 ? "," : "") + R"({"path":)"
                 + quoted(writeTone(media, "take" + std::to_string(i), 1.0)) + "}";
    auto& arrangement = *processor->getStandaloneArrangement();
    const int activeBefore = arrangement.getActiveTrackId();
    const auto selectedBefore = arrangement.getSelectedPlacementId(activeBefore);
    const auto start = api.result(
        "import.start", json(R"({"items":[)" + items + R"(],"layout":"sequential","trackId":0,"startTimelineSec":0})"));
    const auto jobId = memberOf(memberOf(start, "job"), "jobId").text();
    // The first commit; then the user holds the mouse button, so the next commit waits.
    CONTROL_CHECK(pumpMessagesUntil([&bridge] { return bridge.structureChanges >= 1; }, kReadTimeoutMs));
    bridge.busy.pointerDown = true;
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(bridge.structureChanges == 1);
    // A cancellation commits nothing, so it does not wait for the UI: the job ends while the button is
    // still held, and it focuses no clip under the user's hands.
    const auto cancel = api.result("job.cancel", json(R"({"jobId":)" + quoted(jobId) + "}"));
    CONTROL_CHECK(memberOf(cancel, "cancelRequested").asBoolean());
    const auto job = waitJob(api, jobId);
    CONTROL_CHECK(bridge.busy.pointerDown && bridge.structureChanges == 1 && bridge.focused.empty());
    bridge.busy.pointerDown = false;
    CONTROL_CHECK(memberOf(job, "state").text() == "cancelled");
    const auto& error = memberOf(job, "error");
    CONTROL_CHECK(errorKindOf(error).text() == "CANCELLED" && errorReasonOf(error).text() == "cancelled_by_request"
                  && memberOf(memberOf(error, "data"), "executed").asBoolean());
    const auto& result = memberOf(job, "result");
    const auto& resultItems = memberOf(result, "items");
    CONTROL_CHECK(resultItems.size() == 3 && memberOf(resultItems.at(0), "imported").asBoolean());
    CONTROL_CHECK(!memberOf(resultItems.at(1), "imported").asBoolean()
                  && memberOf(resultItems.at(1), "error").isNull());
    CONTROL_CHECK(!memberOf(resultItems.at(2), "imported").asBoolean()
                  && memberOf(resultItems.at(2), "error").isNull());
    CONTROL_CHECK(toJsonString(memberOf(result, "counts")) == R"({"requested":3,"imported":1,"failed":0})");
    // The committed clip stays; the way back is reported: no undo entry, and a revert recipe that deletes the clips
    // that were committed (the two that were never committed are not in it).
    CONTROL_CHECK(memberOf(result, "applied").asBoolean() && memberOf(result, "created").size() == 2);
    CONTROL_CHECK(toJsonString(memberOf(result, "undo")) == importUndo(result, {placementOf(result, 0)}));
    CONTROL_CHECK(warningCodes(result).empty());
    CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 1);
    CONTROL_CHECK(bridge.dirtyMarks >= 1);
    // The commit selected the clip it committed; the cancelled job put the selection back, as it focuses no clip
    // under the user's hands.
    CONTROL_CHECK(arrangement.getActiveTrackId() == activeBefore
                  && arrangement.getSelectedPlacementId(activeBefore) == selectedBefore);
}

// ---- The revert recipe of an import ---------------------------------------------------------------

namespace {

/** Leaves the capabilities out of the dispatchers created during its life (the seam of OpsRegistry.h). */
class ScopedWithheldCapabilities
{
public:
    explicit ScopedWithheldCapabilities(std::vector<std::string> withheld)
    {
        controlSetWithheldCapabilitiesForTesting(std::move(withheld));
    }
    ~ScopedWithheldCapabilities() { controlSetWithheldCapabilitiesForTesting({}); }

    ScopedWithheldCapabilities(const ScopedWithheldCapabilities&) = delete;
    ScopedWithheldCapabilities& operator=(const ScopedWithheldCapabilities&) = delete;
};

/** What a client reads of the clips and the view in project.get: unchanged by a delete that takes an import back. The
    project's dirty flag is left out: the import marked the project as changed and nothing takes that back. */
std::string clipsView(ApiSession& api)
{
    const auto project = api.result("project.get");
    std::string view;
    for (const char* member : {"placements", "contents", "tracks", "visibleTrackCount", "activeTrackId", "selection"})
        view += std::string(member) + "=" + toJsonString(memberOf(project, member)) + "\n";
    view += "asOf=" + toJsonString(memberOf(project, "asOf")) + "\n";
    return view;
}

/** With arrangement.v1 the revert recipes of an import delete the imported clips: sent with the consent of the end
    user, they bring the project back to its state before the import. */
void importRevertRoundTrip()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    bridge.visibleTracks = 1;
    ScopedBridge registered(service, bridge);
    SyntheticAnalysis analysis;
    ApiSession api;
    if (!api.open(service))
        return;
    CONTROL_CHECK(service.dispatcher()->implements("arrangement.edit"));
    const auto media = mediaDirectory("revert");
    const auto take1 = writeTone(media, "take1", 2.0);
    const auto take2 = writeTone(media, "take2", 1.5);
    // The openDyne scheme: the focused clip gets notes, the side effect that has a recipe of its own.
    processor->getAppPreferences().setAudioEditingScheme(AudioEditingScheme::Scheme::NotesPrimary);
    const auto params = R"({"items":[{"path":)" + quoted(take1) + R"(},{"path":)" + quoted(take2)
                        + R"(}],"layout":"sequential","trackId":0,"startTimelineSec":0})";
    const auto before = clipsView(api);
    CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 0);

    // 1. The recipe of undo deletes both clips. Without the consent of the end user the delete is refused and changes
    //    nothing; the recipe is sent unchanged apart from confirm, and the project is as it was.
    {
        const auto job = importAndWait(api, params);
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = memberOf(job, "result");
        CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 2);
        const auto& recipe = memberOf(memberOf(result, "undo"), "revert");
        CONTROL_CHECK(toJsonString(recipe) == deleteRecipe(result, {placementOf(result, 0), placementOf(result, 1)}));
        CONTROL_CHECK(
            api.fails("arrangement.edit", memberOf(recipe, "params"), "INVALID_ARGUMENT", "confirm_required"));
        CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 2);
        const auto reverted = sendDeleteRecipeWithConsent(api, recipe);
        CONTROL_CHECK(memberOf(reverted, "applied").asBoolean() && memberOf(reverted, "removed").size() >= 2);
        CONTROL_CHECK(clipsView(api) == before);
    }

    // 2. The recipe of sideEffects.notesGenerated deletes the focused clip alone, which is the one with the notes;
    //    the other clip stays. The recipe of undo then names a clip that is gone: it fails and changes nothing.
    std::string keptClip;
    {
        const auto job = importAndWait(api, params);
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = memberOf(job, "result");
        const auto& notes = memberOf(memberOf(result, "sideEffects"), "notesGenerated");
        CONTROL_CHECK(toJsonString(memberOf(notes, "revert")) == deleteRecipe(result, {placementOf(result, 1)}));
        const auto reverted = sendDeleteRecipeWithConsent(api, memberOf(notes, "revert"));
        CONTROL_CHECK(memberOf(reverted, "applied").asBoolean());
        keptClip = placementOf(result, 0);
        const auto project = api.result("project.get");
        CONTROL_CHECK(memberOf(project, "placements").size() == 1
                      && memberOf(memberOf(project, "placements").at(0), "placementId").text() == keptClip);
        auto gone = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
        gone.appendMember("confirm", JsonValue::boolean(true));
        CONTROL_CHECK(api.fails("arrangement.edit", std::move(gone), "NOT_FOUND", "unknown_handle"));
        CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 1);
    }

    // 3. The recipe expects the arrangement as the import left it (rule 3): once another change moved a clip, it
    //    fails with REVISION_CONFLICT and deletes nothing.
    {
        const auto next = R"({"items":[{"path":)" + quoted(take2) + R"(}],"layout":"single","trackId":0})";
        const auto job = importAndWait(api, next);
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = memberOf(job, "result");
        CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 2);
        const auto revision = memberOf(memberOf(result, "tokens"), "arrangementRevision").text();
        const auto move =
            json(R"({"expectedArrangementRevision":)" + quoted(revision) + R"(,"ops":[{"op":"move","placementId":)"
                 + quoted(std::string(keptClip)) + R"(,"startTimelineSec":8}]})");
        CONTROL_CHECK(memberOf(api.result("arrangement.edit", move), "applied").asBoolean());
        auto stale = memberOf(memberOf(memberOf(result, "undo"), "revert"), "params");
        stale.appendMember("confirm", JsonValue::boolean(true));
        CONTROL_CHECK(api.fails("arrangement.edit", std::move(stale), "REVISION_CONFLICT", "arrangement_changed"));
        CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == 2);
    }
}

/** Without arrangement.v1 no request can delete the clips: revert is null and the result is IRREVERSIBLE. */
void importRevertWithoutArrangement()
{
    ScopedWithheldCapabilities withheld({"arrangement.v1"});
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    bridge.visibleTracks = 1;
    ScopedBridge registered(service, bridge);
    SyntheticAnalysis analysis;
    ApiSession api;
    if (!api.open(service))
        return;
    const auto capabilities = service.dispatcher()->capabilities();
    CONTROL_CHECK(std::find(capabilities.begin(), capabilities.end(), "arrangement.v1") == capabilities.end());
    CONTROL_CHECK(std::find(capabilities.begin(), capabilities.end(), "import.v1") != capabilities.end());
    processor->getAppPreferences().setAudioEditingScheme(AudioEditingScheme::Scheme::NotesPrimary);
    const auto take = writeTone(mediaDirectory("revert-none"), "take", 1.0);
    const auto job = importAndWait(api, R"({"items":[{"path":)" + quoted(take)
                                            + R"(}],"layout":"single","trackId":0,"startTimelineSec":0})");
    CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
    const auto& result = memberOf(job, "result");
    CONTROL_CHECK(memberOf(memberOf(result, "items").at(0), "imported").asBoolean());
    CONTROL_CHECK(toJsonString(memberOf(result, "undo")) == R"({"undoable":false,"reason":"ui_parity","revert":null})");
    CONTROL_CHECK((warningCodes(result) == std::vector<std::string>{"SELECTION_GENERATED_NOTES", "IRREVERSIBLE"}));
    CONTROL_CHECK(memberOf(memberOf(memberOf(result, "sideEffects"), "notesGenerated"), "revert").isNull());
}

} // namespace

void scenarioImportRevert()
{
    importRevertRoundTrip();
    importRevertWithoutArrangement();
}

// ---- Every write against every busy condition ---------------------------------------------------------

namespace {

/** One write method with params that pass every check that comes before the busy check. */
struct WriteCall
{
    const char* label;
    const char* method;
    std::string params;
};

/** Everything a client or the user could see of the project, the content, the undo history, the jobs and the
    export lock, as one string: a refused write must leave it as it was. */
std::string observableState(ApiSession& api, OpenTuneAudioProcessor& processor, TestUiBridge& bridge,
                            ControlService& service, const std::string& contentId)
{
    std::string state = toJsonString(api.result("project.get"));
    state += toJsonString(api.result("content.getNotes", json("{\"contentId\":" + quoted(contentId) + "}")));
    state += toJsonString(api.result("content.getDetails", json("{\"contentId\":" + quoted(contentId) + "}")));
    state += "|dirty " + std::to_string(bridge.dirtyMarks) + (bridge.project.dirty ? "y" : "n");
    state += "|structure " + std::to_string(bridge.structureChanges) + "/"
             + std::to_string(bridge.undoRedoNotifications) + "/" + std::to_string(bridge.timeGridNotifications);
    state += "|undo " + std::to_string(processor.getUndoManager().size());
    state += "|jobs " + std::to_string(service.jobs()->retainedCount());
    state += processor.isExportLockHeld() ? "|locked" : "|free";
    return state;
}

} // namespace

void scenarioBusyMatrix()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    bridge.visibleTracks = 2;
    ScopedBridge registered(service, bridge);
    ClipSpec spec;
    spec.name = "busy";
    spec.trackId = 0;
    spec.startTimelineSec = 2.0;
    spec.durationSec = 6.0;
    spec.notes = {{0.5, 1.5, 64.0}, {2.0, 3.0, 69.0}};
    spec.gridHandlesContentSec = {1.0, 2.0, 3.0};
    DetectedKey key;
    key.root = Key::A;
    key.scale = Scale::Minor;
    key.origin = Origin::Manual;
    key.confidence = 1.0f;
    spec.key = key;
    const auto clip = fixtureAddClip(*processor, spec);
    CONTROL_CHECK(settleBackgroundWork(*processor));
    ApiSession api;
    if (!api.open(service) || !CONTROL_CHECK(clip.handleIds.size() == 3))
        return;
    const auto handles = fixtureHandlesOf(api.result("project.get"), 0, 2.0);
    if (!CONTROL_CHECK(handles.has_value()))
        return;
    const auto& contentId = handles->contentId;
    const auto& placementId = handles->placementId;
    const auto arrangementRevision =
        memberOf(memberOf(api.result("project.get"), "asOf"), "arrangementRevision").text();
    const auto revision = memberOf(handles->tokens, "revision").text();
    const auto gridRevision = memberOf(handles->tokens, "gridRevision").text();
    CONTROL_CHECK(!revision.empty() && !gridRevision.empty());
    const auto media = mediaDirectory("busy-matrix");
    const auto tone = writeTone(media, "take", 1.0);
    const auto target = absentPath(media, "exported.wav");

    const std::string content = "\"contentId\":" + quoted(contentId);
    const std::string expected = ",\"expectedRevision\":" + quoted(revision);
    const std::vector<WriteCall> calls = {
        {"import.start", "import.start", R"({"items":[{"path":)" + quoted(tone) + R"(}],"layout":"single"})"},
        {"export.start", "export.start", R"({"kind":"master","path":)" + quoted(target) + "}"},
        {"content.setKey", "content.setKey", "{" + content + R"(,"key":{"root":0,"scale":"major"}})"},
        {"content.autoTune regenerate", "content.autoTune",
         "{" + content + expected + R"(,"mode":"regenerate","key":{"root":0,"scale":"major"}})"},
        {"content.autoTune snapNotes", "content.autoTune",
         "{" + content + expected + R"(,"mode":"snapNotes","key":{"root":0,"scale":"major"}})"},
        {"content.editNotes", "content.editNotes",
         "{" + content + expected + R"(,"ops":[{"op":"shiftCents","select":{"all":true},"deltaCents":100}]})"},
        {"content.setPitchShift", "content.setPitchShift",
         "{" + content + R"(,"pitchShift":{"semitones":1,"cents":0}})"},
        {"edit.undo", "edit.undo", R"({"force":true})"},
        {"edit.redo", "edit.redo", R"({"force":true})"},
        {"content.shiftTiming", "content.shiftTiming",
         "{" + content + ",\"expectedGridRevision\":" + quoted(gridRevision)
             + R"(,"range":{"startOutputSec":1.5,"endOutputSec":2.5},"deltaSec":0.25})"},
        {"content.editTimeGrid seed", "content.editTimeGrid", "{" + content + R"(,"op":"seed"})"},
        {"content.editTimeGrid moveHandles", "content.editTimeGrid",
         "{" + content + ",\"op\":\"moveHandles\",\"expectedGridRevision\":" + quoted(gridRevision)
             + ",\"moves\":[{\"handleId\":" + quoted(clip.handleIds[0]) + ",\"outputSec\":1.1}]}"},
        {"arrangement.edit", "arrangement.edit",
         "{\"expectedArrangementRevision\":" + quoted(arrangementRevision)
             + ",\"ops\":[{\"op\":\"move\",\"placementId\":" + quoted(placementId) + ",\"startTimelineSec\":4.0}]}"},
        {"arrangement.copy", "arrangement.copy",
         "{\"sources\":[{\"placementId\":" + quoted(placementId)
             + "}],\"mode\":\"full\",\"toTrackId\":1,\"startTimelineSec\":12.0,\"expectedArrangementRevision\":"
             + quoted(arrangementRevision) + "}"},
    };
    struct Condition
    {
        const char* name;
        bool ControlUiBridge::BusyState::*flag;
        const char* reason;
    };
    const Condition conditions[] = {
        {"the pointer is down", &ControlUiBridge::BusyState::pointerDown, "pointer_down"},
        {"a modal dialog is open", &ControlUiBridge::BusyState::modalOpen, "modal_open"},
        {"the editor exports", &ControlUiBridge::BusyState::exportRunning, "export_in_progress"},
        {"the editor imports", &ControlUiBridge::BusyState::importRunning, "import_in_progress"},
        {"the editor saves or opens a project", &ControlUiBridge::BusyState::operationRunning, "project_operation"},
    };

    const auto before = observableState(api, *processor, bridge, service, contentId);
    CONTROL_CHECK(before == observableState(api, *processor, bridge, service, contentId));
    // Every cell: BUSY with the condition's reason and executed false (ApiSession::fails), and nothing a client or
    // the user could see changed, read once the condition is over.
    int cells = 0;
    const auto cell = [&](const WriteCall& call, const char* condition, const char* reason) {
        const bool refused = CONTROL_CHECK(api.fails(call.method, json(call.params), "BUSY", reason));
        bridge.busy = {};
        if (processor->isExportLockHeld())
            processor->releaseExportLock();
        const bool unchanged = CONTROL_CHECK(observableState(api, *processor, bridge, service, contentId) == before);
        if (!refused || !unchanged)
            note(std::string("the cell ") + call.label + " x " + condition + " failed");
        ++cells;
    };
    for (const auto& call : calls) {
        for (const auto& condition : conditions) {
            bridge.busy = {};
            bridge.busy.*(condition.flag) = true;
            cell(call, condition.name, condition.reason);
        }
        // The export lock that the UI and the API share, held by an export that is not the editor's flag.
        bridge.busy = {};
        CONTROL_CHECK(processor->tryAcquireExportLock());
        cell(call, "the export lock", "export_in_progress");
    }
    CONTROL_CHECK(cells == static_cast<int>(calls.size()) * 6);
    CONTROL_CHECK(!juceFileOf(std::filesystem::u8path(target)).exists());

    // Once nothing is busy any more the writes are not refused.
    const auto set = api.result("content.setKey", json("{" + content + R"(,"key":{"root":0,"scale":"major"}})"));
    CONTROL_CHECK(memberOf(set, "applied").asBoolean() && !memberOf(set, "noOp").asBoolean());
    const auto dispatcher = service.dispatcher();
    CONTROL_CHECK(dispatcher != nullptr && dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

// ---- A client that goes away while its job.get waits ---------------------------------------------------

void scenarioJobDisconnect()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    const auto jobs = service.jobs();
    ApiSession api;
    if (!service.isRunning() || !CONTROL_CHECK(jobs != nullptr) || !api.open(service))
        return;
    // A running job whose work owns a thread (as the work of an export does).
    auto record = std::make_shared<ThreadedJob>();
    const auto jobId = jobs->start("export", true, threadedWork(record)).jobId;
    CONTROL_CHECK(pumpMessagesUntil([&record] { return record->steps.load() > 0 && record->threadRunning.load(); },
                                    kReadTimeoutMs));

    // A client of its own sends job.get and closes its connection while it waits. A request in flight when the
    // connection closes runs to completion and its response is discarded (PROTOCOL.md section 3), so the waiter
    // ends at its own deadline, not at the close; with a short hold that is soon, and nothing else is affected.
    const auto goAway = [&](int waitMs) {
        TestClient waiter;
        CONTROL_CHECK(authenticate(waiter, service.port(), memberOf(api.discovery(), "token").text()).has_value());
        CONTROL_CHECK(waiter.sendLine(R"({"jsonrpc":"2.0","id":7,"method":"job.get","params":{"jobId":)" + quoted(jobId)
                                      + R"(,"waitMs":)" + std::to_string(waitMs) + "}}"));
        CONTROL_CHECK(waitUntil([&jobs] { return jobs->waitingCount() == 1; }, kWaiterSettleMs));
        waiter.close();
    };
    constexpr int kShortHoldMs = 600;
    goAway(kShortHoldMs);
    const auto closedAt = Clock::now();
    CONTROL_CHECK(waitUntil([&jobs] { return jobs->waitingCount() == 0; }, kShortHoldMs + kCloseSeenMs));
    note("the waiter was gone " + std::to_string(millisecondsSince(closedAt)) + " ms after the close");
    CONTROL_CHECK(record->threadRunning.load() && !record->destroyed.load());

    // And one with the longest hold, whose client is gone when the processor is destroyed.
    goAway(30000);

    // The job is untouched, and a new connection (the first one as well) gets it.
    ApiSession second;
    if (CONTROL_CHECK(second.open(service))) {
        const auto job = memberOf(second.result("job.get", json(R"({"jobId":)" + quoted(jobId) + "}")), "job");
        CONTROL_CHECK(memberOf(job, "state").text() == "running" && memberOf(job, "jobId").text() == jobId);
    }
    CONTROL_CHECK(
        memberOf(memberOf(api.result("job.get", json(R"({"jobId":)" + quoted(jobId) + "}")), "job"), "state").text()
        == "running");
    CONTROL_CHECK(record->threadRunning.load() && !record->destroyed.load());

    // Destroying the processor then is within the bound, and the work is destroyed with its thread joined.
    const auto started = Clock::now();
    processor.reset();
    const auto elapsed = millisecondsSince(started);
    note("processor destroyed in " + std::to_string(elapsed) + " ms");
    CONTROL_CHECK(elapsed <= kShutdownBoundMs);
    CONTROL_CHECK(record->destroyed.load() && !record->threadRunning.load());
}

// ---- Imports that fail -----------------------------------------------------------------------------------

void scenarioImportFailures()
{
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return;
    TestUiBridge bridge;
    bridge.visibleTracks = 2;
    ScopedBridge registered(service, bridge);
    ApiSession api;
    if (!api.open(service))
        return;
    const auto media = mediaDirectory("import-failures");
    const auto oneItem = [](const std::string& path) {
        return R"({"items":[{"path":)" + quoted(path) + R"(}],"layout":"single","trackId":0})";
    };

    // 1. The pitch analysis fails: the clip stays in the project, its f0State is failed, and the item says
    //    ANALYSIS_FAILED(f0_failed); the job itself succeeded.
    {
        const SyntheticAnalysis failing(true, true);
        const auto path = writeTone(media, "doomed", 2.0);
        const auto job = importAndWait(api, oneItem(path));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = memberOf(job, "result");
        const auto& item = memberOf(result, "items").at(0);
        note("item: " + toJsonString(item));
        CONTROL_CHECK(memberOf(item, "imported").asBoolean() && memberOf(item, "f0State").text() == "failed");
        // The import was executed and its clip stays: executed true (PROTOCOL.md section 17.4).
        const auto& executed = memberOf(memberOf(memberOf(item, "error"), "data"), "executed");
        CONTROL_CHECK(errorKindOf(memberOf(item, "error")).text() == "ANALYSIS_FAILED"
                      && errorReasonOf(memberOf(item, "error")).text() == "f0_failed" && executed.isBoolean()
                      && executed.asBoolean());
        CONTROL_CHECK(toJsonString(memberOf(result, "counts")) == R"({"requested":1,"imported":1,"failed":0})");
        CONTROL_CHECK(memberOf(result, "applied").asBoolean() && !memberOf(result, "noOp").asBoolean());
        const auto project = api.result("project.get");
        const auto& placements = memberOf(project, "placements");
        CONTROL_CHECK(placements.size() == 1 && memberOf(placements.at(0), "sourcePath").text() == path);
        const auto details = api.result("content.getDetails",
                                        json(R"({"contentId":)" + quoted(memberOf(item, "contentId").text()) + "}"));
        CONTROL_CHECK(memberOf(memberOf(details, "analysis"), "f0State").text() == "failed");
    }

    // The checks of the file itself: each fails alone, nothing is imported.
    const SyntheticAnalysis analysis;
    const auto retained = service.jobs()->retainedCount();
    const auto placementsBefore = memberOf(api.result("project.get"), "placements").size();
    const auto failsWith = [&](const std::string& path, const char* kind, const char* reason) {
        const auto job = importAndWait(api, oneItem(path));
        CONTROL_CHECK(memberOf(job, "state").text() == "succeeded");
        const auto& result = memberOf(job, "result");
        const auto& item = memberOf(result, "items").at(0);
        const bool ok = !memberOf(item, "imported").asBoolean() && memberOf(item, "f0State").isNull()
                        && errorKindOf(memberOf(item, "error")).text() == kind
                        && errorReasonOf(memberOf(item, "error")).text() == reason;
        if (!ok)
            note("expected " + std::string(kind) + "(" + reason + "), got " + toJsonString(item));
        CONTROL_CHECK(ok);
        CONTROL_CHECK(toJsonString(memberOf(result, "counts")) == R"({"requested":1,"imported":0,"failed":1})");
        CONTROL_CHECK(memberOf(result, "noOp").asBoolean());
    };
    // 2. 601 s at 8 kHz, 8 bit, mono: a small file, but longer than import.maxAnalysisSec (600 s).
    failsWith(writeTone(media, "long", 601.0, 1, 8000.0, 8), "INVALID_ARGUMENT", "audio_too_long");
    // 3. More than two channels.
    failsWith(writeTone(media, "surround", 1.0, 3), "UNSUPPORTED", "too_many_channels");
    CONTROL_CHECK(memberOf(api.result("project.get"), "placements").size() == placementsBefore);
    CONTROL_CHECK(service.jobs()->retainedCount() == retained + 2);
    const auto info = api.result("session.info");
    CONTROL_CHECK(memberOf(memberOf(info, "import"), "maxAnalysisSec").asDouble() == 600.0);
}

// ---- The specification's vectors -------------------------------------------------------------------

namespace {

const std::set<std::string> kVectorMethods = {"job.get", "job.cancel", "import.start", "project.get", "session.info"};
const std::set<std::string> kVectorCapabilities = {"session.v1", "read.v1", "jobs.v1", "import.v1"};

std::string jobVectorSkipReason(const JsonValue& vector)
{
    if (kVectorCapabilities.count(memberOf(vector, "capability").text()) == 0)
        return "needs capability " + memberOf(vector, "capability").text() + ", which this runner does not cover";
    const auto& extra = memberOf(vector, "extraCapabilities");
    for (std::size_t i = 0; i < extra.size(); ++i)
        if (kVectorCapabilities.count(extra.at(i).text()) == 0)
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
        if (kVectorMethods.count(method) == 0)
            return "uses " + method + ", which this runner does not cover";
    const auto& given = memberOf(vector, "given");
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& name = given.keyAt(i);
        if (name == "dirty" || name == "editingScheme" || name == "clipOnEveryTrack" || name == "withoutCapabilities")
            continue;
        if (name == "files") {
            for (std::size_t f = 0; f < given.at(i).size(); ++f) {
                const auto kind = memberOf(given.at(i).at(f), "kind").text();
                if (kind != "audio" && kind != "absent" && kind != "undecodable")
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
                if (memberOf(clip, "analysis").text() != "ready" || clip.size() > 5)
                    return "clips other than plain analysed ones are not established by this runner";
            }
            continue;
        }
        return "precondition given." + name + " is not established by this runner";
    }
    const auto& without = memberOf(given, "withoutCapabilities");
    for (std::size_t i = 0; i < without.size(); ++i)
        if (kVectorCapabilities.count(without.at(i).text()) != 0)
            return "removing the reported capability " + without.at(i).text() + " is not supported by this runner";
    return {};
}

/** Adds a clip of a voiced tone with a finished analysis (no notes) at the start of the track. */
bool addAnalysedClip(OpenTuneAudioProcessor& processor, const std::filesystem::path& directory, const std::string& name,
                     int trackId, double startSec, double durationSec)
{
    const auto path = writeTone(directory, name, durationSec);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(juce::File(juce::String::fromUTF8(path.c_str()))));
    if (reader == nullptr)
        return false;
    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
    OpenTuneAudioProcessor::PreparedImport prepared;
    if (!processor.prepareImport(std::move(audio), reader->sampleRate, juce::String(name),
                                 juce::String::fromUTF8(path.c_str()), prepared))
        return false;
    OpenTuneAudioProcessor::ImportPlacement placement;
    placement.trackId = trackId;
    placement.timelineStartSeconds = startSec;
    const auto committed = processor.commitPreparedImportAsPlacement(std::move(prepared), placement);
    if (!committed.isValid())
        return false;
    OpenTuneAudioProcessor::ContentRefreshRequest request;
    request.contentKey = committed.contentKey;
    commitSyntheticAnalysis(processor, request);
    return true;
}

/** Establishes given.jobs: each job in its state, its id bound to its name. */
bool establishJobs(ControlJobs& jobs, const JsonValue& given, VectorMatcher& matcher,
                   std::vector<std::shared_ptr<FixtureJob>>& fixtures)
{
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& spec = given.at(i);
        const auto state = memberOf(spec, "state").text();
        const auto& error = memberOf(spec, "error");
        auto fixture = std::make_shared<FixtureJob>();
        ControlJobWork work = fixtureWork(fixture);
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
        const auto started = jobs.start(memberOf(spec, "kind").text(),
                                        memberOf(spec, "cancellable").asBoolean() || state != "running", work);
        if (started.jobId.empty())
            return false;
        matcher.bindings[memberOf(spec, "bind").text()] = JsonValue::string(started.jobId);
        const std::string jobId = started.jobId;
        const bool reached = pumpMessagesUntil(
            [&jobs, jobId, state] {
                JsonValue job;
                return jobs.get(jobId, job) == ControlJobs::Lookup::Found && memberOf(job, "state").text() == state;
            },
            kReadTimeoutMs);
        if (!reached)
            return false;
        fixtures.push_back(std::move(fixture));
    }
    return true;
}

/** Runs one vector of job.get, job.cancel or import.start against a fresh processor with a test bridge
    and the synthetic analysis. Empty on success. */
std::string runJobVector(const JsonValue& vector, const std::string& slug)
{
    const auto& given = memberOf(vector, "given");
    // given.withoutCapabilities: the instance does not report these (import.start then has no revert recipe, which
    // needs arrangement.v1). The dispatcher takes the capabilities when it is created, so they are set first.
    std::vector<std::string> withheld;
    const auto& without = memberOf(given, "withoutCapabilities");
    for (std::size_t i = 0; i < without.size(); ++i)
        withheld.push_back(without.at(i).text());
    ScopedWithheldCapabilities capabilities(std::move(withheld));
    auto processor = startedProcessor();
    auto& service = processor->getControlService();
    if (!service.isRunning())
        return "the control API did not start";
    TestUiBridge bridge;
    bridge.project.dirty = memberOf(given, "dirty").isBoolean() && memberOf(given, "dirty").asBoolean();
    ScopedBridge registered(service, bridge);
    SyntheticAnalysis analysis;
    processor->getAppPreferences().setAudioEditingScheme(memberOf(given, "editingScheme").text() == "openDyne"
                                                             ? AudioEditingScheme::Scheme::NotesPrimary
                                                             : AudioEditingScheme::Scheme::CorrectedF0Primary);
    const auto directory = mediaDirectory("vectors-" + slug);
    VectorMatcher matcher;

    const auto& files = memberOf(given, "files");
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files.at(i);
        const auto bind = memberOf(file, "bind").text();
        const auto kind = memberOf(file, "kind").text();
        std::string path;
        if (kind == "audio")
            path = writeTone(directory, bind, memberOf(file, "durationSec").asDouble());
        else if (kind == "undecodable")
            path = writeGarbage(directory, bind + ".wav");
        else
            path = absentPath(directory, bind + ".wav");
        matcher.bindings[bind] = JsonValue::string(path);
    }
    auto* arrangement = processor->getStandaloneArrangement();
    const auto& clips = memberOf(given, "clips");
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto& clip = clips.at(i);
        if (!addAnalysedClip(*processor, directory, "clip" + std::to_string(i),
                             static_cast<int>(memberOf(clip, "trackId").asDouble()),
                             memberOf(clip, "startTimelineSec").asDouble(), memberOf(clip, "durationSec").asDouble()))
            return "the clip " + memberOf(clip, "bind").text() + " could not be added";
    }
    if (memberOf(given, "clipOnEveryTrack").asBoolean())
        for (int trackId = 0; trackId < arrangement->getNumTracks(); ++trackId)
            if (arrangement->getNumPlacements(trackId) == 0
                && !addAnalysedClip(*processor, directory, "fill" + std::to_string(trackId), trackId, 0.0, 2.0))
                return "a clip on every track could not be added";
    std::vector<std::shared_ptr<FixtureJob>> fixtures;
    if (!establishJobs(*service.jobs(), memberOf(given, "jobs"), matcher, fixtures))
        return "given.jobs could not be established";

    ApiSession api;
    if (!api.open(service))
        return "session.hello failed";
    const auto& discovery = api.discovery();
    for (std::size_t i = 0; i < discovery.size(); ++i)
        matcher.bindings["discovery." + discovery.keyAt(i)] = discovery.at(i);
    const int lastTrack = arrangement->getNumTracks() - 1;
    matcher.bindings["runner.lastTrackId"] = JsonValue::integer(lastTrack);
    matcher.bindings["runner.unknownTrackId"] = JsonValue::integer(lastTrack + 1);

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
    return failure;
}

} // namespace

void scenarioJobVectors()
{
    const auto root = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors";
    std::vector<std::filesystem::path> files;
    for (const char* area : {"job", "import"})
        for (const auto& entry : std::filesystem::directory_iterator(root / area))
            if (entry.path().extension() == ".json")
                files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    int passed = 0;
    int skipped = 0;
    VectorReport report("ControlProcessorTests.JobVectors");
    for (const auto& file : files) {
        const auto parsed = parseJson(readFile(file));
        if (!CONTROL_CHECK(parsed.ok))
            continue;
        const auto& vector = parsed.value;
        const auto id = memberOf(vector, "id").text();
        if (const auto reason = jobVectorSkipReason(vector); !reason.empty()) {
            ++skipped;
            report.skip(id, reason);
            note("SKIP " + id + ": " + reason);
            continue;
        }
        const auto started = Clock::now();
        auto slug = id;
        std::replace(slug.begin(), slug.end(), '/', '-');
        const auto failure = runJobVector(vector, slug);
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty()))
            ++passed;
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    // Every vector of job/ and import/ runs headless.
    CONTROL_CHECK(skipped == 0 && passed >= 18);
    note(std::to_string(passed) + " vectors passed, " + std::to_string(skipped) + " skipped");
}

} // namespace ControlTest
} // namespace OpenTune
