// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Helpers shared by the scenarios of OpenTuneControlProcessorTests: a started processor with its control
// service, the discovery file, session.hello, and requests answered while this (the message) thread
// pumps messages. The scenarios of the dispatcher and the read methods are declared at the end and
// defined in ControlDispatcherTests.cpp.

#include "ControlHeadlessSupport.h"

#include "Content/StandaloneContentRepository.h"
#include "Control/ControlJobs.h"
#include "Control/ControlService.h"
#include "Inference/RenderCache.h"
#include "PluginProcessor.h"
#include "Render/ContentRenderService.h"
#include "Runtime/ProcessRenderRuntime.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OpenTune {
namespace ControlTest {

/** From construction of the processor to a listening control API. */
constexpr int kStartTimeoutMs = 5000;
/** Time in which something that should not happen (a start, a queued function) would have happened. */
constexpr int kQuietPumpMs = 300;
/** Bound of the processor destruction while connections and waiters are busy. */
constexpr int kShutdownBoundMs = 2000;
/** Time for a client to see the close of its connection, and for a woken thread to return. */
constexpr int kCloseSeenMs = 1000;
/** Time for a waiting thread to enter ControlMessageThread::call(). */
constexpr int kWaiterSettleMs = 2000;
/** Bound of one exchange of a client thread while the message thread pumps; longer than kReadTimeoutMs. */
constexpr int kClientExchangeMs = 15000;
/** Bound of closing the modal windows a scenario left open, until they are deleted. */
constexpr int kCloseWindowsMs = 2000;
/** Bound of the background work a scenario's setup starts (the render of imported clips), and of the messages
    it posts. Without the models a render fails in well under a second; with them it renders a few seconds of
    audio. */
constexpr int kSettleTimeoutMs = 30000;
/** How long the messages keep being dispatched once the background work is over, before the queue is drained: the
    render worker posts its failure right after it marks the block, so it is posted by then. */
constexpr int kSettleGraceMs = 200;

/** A member of an object, or a null value when absent. */
inline const JsonValue& memberOf(const JsonValue& object, const char* name)
{
    static const JsonValue none;
    const JsonValue* value = object.find(name);
    return value != nullptr ? *value : none;
}

/** The change of the singleton tempo in the changes of a write result, or null when the result lists none. */
inline const JsonValue* findTempoChange(const JsonValue& result)
{
    const auto& changes = memberOf(result, "changes");
    for (std::size_t i = 0; i < changes.size(); ++i)
        if (memberOf(changes.at(i), "entity").text() == "tempo")
            return &changes.at(i);
    return nullptr;
}

/** True when a write result reports no rewritten tempo: no change of the tempo and no tempoRounded side effect. */
inline bool reportsNoTempoChange(const JsonValue& result)
{
    return findTempoChange(result) == nullptr && memberOf(result, "sideEffects").find("tempoRounded") == nullptr;
}

/**
 * Checks what a view switch (ui.setWorkspace, ui.setSelection with openEditor) reports when the transport bar's BPM
 * field rewrote the project tempo from fromBpm to toBpm: the change of the singleton tempo with the member bpm
 * {before, after, unit bpm} and no other member (the time signature is not reported), and the side effect tempoRounded
 * {fromBpm, toBpm, revert}, whose revert is transport.setTempo back to fromBpm, or null (revertReported false: the
 * instance does not report transport.v1). Returns the change of the tempo, or null when the result lists none.
 */
inline const JsonValue* checkTempoRounded(const JsonValue& result, double fromBpm, double toBpm, bool revertReported)
{
    const auto same = [](const JsonValue& value, double expected) {
        return value.isNumber() && std::abs(value.asDouble() - expected) < 1e-9;
    };
    const JsonValue* change = findTempoChange(result);
    if (!CONTROL_CHECK(change != nullptr))
        return nullptr;
    const auto& fields = memberOf(*change, "fields");
    const auto& bpm = memberOf(fields, "bpm");
    CONTROL_CHECK(memberOf(memberOf(*change, "ref"), "singleton").text() == "tempo"
                  && memberOf(*change, "label").text() == "Tempo" && fields.size() == 1);
    CONTROL_CHECK(same(memberOf(bpm, "before"), fromBpm) && same(memberOf(bpm, "after"), toBpm)
                  && memberOf(bpm, "unit").text() == "bpm");
    const auto& effects = memberOf(result, "sideEffects");
    const auto& rounded = memberOf(effects, "tempoRounded");
    CONTROL_CHECK(effects.find("tempoRounded") != nullptr && same(memberOf(rounded, "fromBpm"), fromBpm)
                  && same(memberOf(rounded, "toBpm"), toBpm));
    const auto& revert = memberOf(rounded, "revert");
    if (revertReported)
        CONTROL_CHECK(memberOf(revert, "method").text() == "transport.setTempo"
                      && memberOf(revert, "params").size() == 1
                      && same(memberOf(memberOf(revert, "params"), "bpm"), fromBpm)
                      && !memberOf(revert, "requiresConsent").asBoolean());
    else
        CONTROL_CHECK(revert.isNull() && rounded.find("revert") != nullptr);
    return change;
}

/** The discovery file at path, parsed; checks its size limit (PROTOCOL.md section 5.3). */
inline std::optional<JsonValue> readDiscoveryFile(const std::filesystem::path& path)
{
    const auto text = readFile(path);
    CONTROL_CHECK(!text.empty() && text.size() <= 8192);
    auto parsed = parseJson(text);
    if (!CONTROL_CHECK(parsed.ok))
        return std::nullopt;
    return parsed.value;
}

inline std::string helloLine(const std::string& token)
{
    return R"({"jsonrpc":"2.0","id":1,"method":"session.hello","params":{"token":")" + token
           + R"(","protocol":"1.0","client":{"name":"control-processor-tests","version":"1.0.0"}}})";
}

/** Connects and completes session.hello; returns the hello result. The listener answers session.hello
    itself, so this works without pumping messages. */
inline std::optional<JsonValue> authenticate(TestClient& client, int port, const std::string& token)
{
    if (!client.connect(port) || !client.sendLine(helloLine(token)))
        return std::nullopt;
    const auto line = client.readLine();
    if (!line)
        return std::nullopt;
    auto parsed = parseJson(*line);
    if (!parsed.ok || parsed.value.find("result") == nullptr)
        return std::nullopt;
    return *parsed.value.find("result");
}

/**
 * Waits until the background work of the processor's contents is over, then dispatches the messages that work
 * posted, so that a scenario's timed requests do not race with it. Message thread only; returns whether both
 * happened within kSettleTimeoutMs.
 *
 * Importing a clip requests its render (RenderCache::reconcileFullPlanAndRequest). Without the models, as on
 * the build machines, that render fails, and upstream ProcessRenderRuntime::notifyApplicationRenderFailure()
 * posts a ConfirmDialogContent::showDiagnostic() dialog to the message thread. Building that window is the one
 * long message of such a scenario (about 0.4 s with a GPU; longer without one), and a request that waits for the
 * message thread meanwhile waits as long. The work is over once the render worker has nothing queued or running
 * and no render cache has a pending or running block; the failure is posted right after its block is marked, so
 * after kSettleGraceMs more the messages posted until then are dispatched (drainPostedMessages).
 */
inline bool settleBackgroundWork(OpenTuneAudioProcessor& processor, int timeoutMs = kSettleTimeoutMs)
{
    auto* service = processor.getContentRenderService();
    auto* repository = processor.getStandaloneContentRepository();
    if (service == nullptr || repository == nullptr)
        return true;
    const auto started = Clock::now();
    const bool idle = pumpMessagesUntil(
        [service, repository] {
            if (service->renderQueueDepth() != 0 || service->renderInFlight() != 0)
                return false;
            for (const auto& key : repository->getAllClips())
                if (const auto cache = service->getRenderCache(key))
                    if (cache->getChunkStats().hasActiveWork())
                        return false;
            return true;
        },
        timeoutMs);
    pumpMessagesFor(kSettleGraceMs);
    const auto remaining = std::max<long long>(0, timeoutMs - millisecondsSince(started));
    const bool drained = drainPostedMessages(static_cast<int>(remaining));
    if (!idle || !drained)
        note("the background work of the scenario's setup did not settle within " + std::to_string(timeoutMs)
             + " ms (render work idle: " + (idle ? "yes" : "no") + ", messages drained: " + (drained ? "yes" : "no")
             + ")");
    return idle && drained;
}

/**
 * Takes one owner of OpenTune's process-wide render runtime (ProcessRenderRuntime::retainOwner) for the rest of the
 * test process; startedProcessor() calls it before it constructs the first processor of a scenario.
 *
 * Why: the runtime shuts down whenever its last owner, a processor, goes away, and starts again with the next one.
 * That shutdown (releaseOwner, shutdownLocked) waits on workerExitCv_ under shutdownMutex_ until the control worker
 * has set workerExitDispatchAttempted_; the worker sets it and notifies without that mutex, so the notification can
 * fall between the waiter's check and its wait and be lost, and the destruction of the processor then never returns.
 * In the application the runtime starts once and shuts down at exit; a scenario that creates and destroys many
 * processors (those of the vectors) took that chance at every processor, and the slower timing under a thread
 * sanitizer makes the window wide enough to hit. With this owner the count never falls to zero between processors,
 * so no shutdown runs. It is never released, so that the test process never takes that chance either: the runtime's
 * control worker ends with the process.
 *
 * Only scenarios that start the control API take it: starting the runtime initialises OpenTune's logger (it logs
 * when a model file is missing), and the scenarios that construct a processor without the API (Disabled and the
 * others in ControlProcessorTests.cpp) check that nothing was initialised.
 */
inline void holdRenderRuntimeForProcess()
{
    static const bool held = [] {
        ProcessRenderRuntime::getInstance().retainOwner();
        return true;
    }();
    (void)held;
}

/**
 * The failure messages that OpenTune keeps with a content whose pitch analysis failed
 * (EditableContentSnapshot::originalF0FailureReason, stored by requestContentRefresh through
 * OpenTuneAudioProcessor::setContentOriginalF0FailureReason): the message of the F0 extraction for audio without a
 * voiced frame, which the API reports as f0_empty, and any other one, reported as f0_failed.
 */
inline constexpr const char* kUnvoicedF0FailureMessage = "f0_empty_or_unvoiced";
inline constexpr const char* kOtherF0FailureMessage = "F0 extraction failed";

/**
 * Fails the analysis of a content the way requestContentRefresh does: the state Failed, then the message. The state
 * comes first because OpenTune clears the message whenever the state changes to another one.
 */
inline bool failAnalysis(OpenTuneAudioProcessor& processor, const ContentKey& key, bool unvoiced)
{
    return processor.setContentOriginalF0State(key, OriginalF0State::Failed)
           && processor.setContentOriginalF0FailureReason(key, unvoiced ? kUnvoicedF0FailureMessage
                                                                        : kOtherF0FailureMessage);
}

/**
 * The language that the scenarios of this process run in, bound for the rest of the process the way an editor binds
 * the language of its window (LocalizationManager::ScopedLanguageBinding). OpenTune takes the descriptions of undo
 * entries with LOC_RAW, which asks the bound language (an unbound one asserts); the API does the same, so a scenario
 * that compares a description takes the expected text from this state, and one that checks that the description
 * follows the window sets the language here and puts it back. English until a scenario changes it.
 */
inline std::shared_ptr<LocalizationManager::LanguageState> testLanguageState()
{
    static const auto state =
        std::make_shared<LocalizationManager::LanguageState>(LocalizationManager::LanguageState{Language::English});
    static const LocalizationManager::ScopedLanguageBinding binding(state);
    return state;
}

/**
 * Constructs a processor with the API enabled and pumps messages until it listens.
 *
 * Fails the scenario when juce::MessageManager::stopDispatchLoop() was called earlier in this process: the flag it sets
 * is never reset (see ControlHeadlessSupport.h), so the message queue would refuse everything this processor posts. The
 * case that stops the loop (ControlQuitTests.cpp, exitStepRefusedByQueue()) must run last in its process.
 */
inline std::unique_ptr<OpenTuneAudioProcessor> startedProcessor()
{
    if (auto* messages = juce::MessageManager::getInstanceWithoutCreating()) {
        const bool loopStopped = messages->hasStopMessageBeenSent();
        CONTROL_CHECK(!loopStopped);
        if (loopStopped)
            note("an earlier case of this process stopped the message loop (juce::MessageManager::stopDispatchLoop()), "
                 "which the queue never recovers from; exitStepRefusedByQueue() must run last in its process");
    }
    holdRenderRuntimeForProcess();
    testLanguageState();
    auto processor = std::make_unique<OpenTuneAudioProcessor>();
    auto& service = processor->getControlService();
    const auto started = Clock::now();
    if (CONTROL_CHECK(pumpMessagesUntil([&service] { return service.isRunning(); }, kStartTimeoutMs)))
        note("listening " + std::to_string(millisecondsSince(started)) + " ms after construction");
    return processor;
}

/**
 * Runs exchange on a client thread while this thread, the message thread, pumps messages, so that the
 * requests it sends can run on the message thread. Returns whether it finished within timeoutMs; it
 * always joins the client thread before returning.
 */
template <typename Exchange>
bool runWhilePumping(Exchange&& exchange, int timeoutMs = kClientExchangeMs)
{
    std::atomic<bool> done{false};
    std::thread client([&exchange, &done] {
        exchange();
        done = true;
    });
    const bool finished = pumpMessagesUntil([&done] { return done.load(); }, timeoutMs);
    client.join();
    return finished;
}

/**
 * Establishes given.jobs of a vector whose jobs are all failed ({bind, kind, state "failed", cancellable false,
 * error {kind, reason}}, vectors/README.md): each is started as a job of its kind that fails at once with its
 * error (executed false), and its id is bound to the name `bind`. Pumps messages until every job has failed.
 * Returns false when a job cannot be started or does not fail within kReadTimeoutMs.
 */
inline bool establishFailedJobs(ControlJobs& jobs, const JsonValue& given, VectorMatcher& matcher)
{
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& spec = given.at(i);
        const auto& error = memberOf(spec, "error");
        const ControlJobError failure{memberOf(error, "kind").text(), memberOf(error, "reason").text(),
                                      "a failed job of the fixture", false};
        const auto started =
            jobs.start(memberOf(spec, "kind").text(), true, [failure](ControlJobStep& step) { step.fail(failure); });
        if (started.jobId.empty())
            return false;
        matcher.bindings[memberOf(spec, "bind").text()] = JsonValue::string(started.jobId);
        const std::string jobId = started.jobId;
        if (!pumpMessagesUntil(
                [&jobs, jobId] {
                    JsonValue job;
                    return jobs.get(jobId, job) == ControlJobs::Lookup::Found
                           && memberOf(job, "state").text() == "failed";
                },
                kReadTimeoutMs))
            return false;
    }
    return true;
}

/**
 * Closes the modal windows still open on this (the message) thread and pumps messages until they are
 * deleted. Returns how many windows it closed, or -1 when one was not deleted within kCloseWindowsMs.
 * runScenario calls it after every scenario, once the scenario's processors are destroyed and before the
 * ScopedJuceInitialiser_GUI goes away, so that no window outlives the scenario that opened it.
 *
 * Why: without the models a render fails, and upstream ProcessRenderRuntime::notifyApplicationRenderFailure()
 * then posts ConfirmDialogContent::showDiagnostic(), a modal DialogWindow (deleted when dismissed) that no
 * one closes in a headless test, so ~ModalComponentManager deletes it at shutdown. ConfirmDialogContent
 * listens to its DialogWindow and stops listening in its destructor through
 * Component::SafePointer<DialogWindow>, whose getComponent() is a dynamic_cast. The window deletes the
 * content it owns in ~ResizableWindow, when it is no longer a DialogWindow: the cast yields null, the
 * listener stays registered, and ~Component of the window then calls it on the freed content
 * (heap-use-after-free under ASan). Dismissing the window (exitModalState, cancelAllModalComponents)
 * deletes it the same way, so that alone does not help. clearContentComponent() while the window is still
 * whole deletes the content first, which lets it stop listening; the empty window is then dismissed and
 * deleted.
 */
inline int closeLeftoverModalWindows()
{
    std::vector<juce::Component::SafePointer<juce::Component>> closing;
    int closed = 0;
    const bool deleted = pumpMessagesUntil(
        [&closing, &closed] {
            // Every round, so that a window opened by a message this pump dispatched is closed as well.
            std::vector<juce::Component::SafePointer<juce::Component>> open;
            if (auto* manager = juce::ModalComponentManager::getInstanceWithoutCreating())
                for (int i = 0; i < manager->getNumModalComponents(); ++i)
                    open.emplace_back(manager->getModalComponent(i));
            for (const auto& window : open) {
                if (auto* resizable = dynamic_cast<juce::ResizableWindow*>(window.getComponent()))
                    resizable->clearContentComponent();
                if (auto* component = window.getComponent())
                    component->exitModalState(0);
                closing.push_back(window);
                ++closed;
            }
            closing.erase(std::remove_if(closing.begin(), closing.end(),
                                         [](const auto& window) { return window.getComponent() == nullptr; }),
                          closing.end());
            return closing.empty();
        },
        kCloseWindowsMs);
    return deleted ? closed : -1;
}

/** An authenticated session of a started processor that sends requests and reads their responses. */
class ApiSession
{
public:
    /** Reads the discovery file of the service and completes session.hello. */
    bool open(const ControlService& service)
    {
        const auto discovery = readDiscoveryFile(std::filesystem::u8path(service.discoveryFilePath()));
        if (!discovery)
            return false;
        discovery_ = *discovery;
        return CONTROL_CHECK(authenticate(client_, service.port(), memberOf(discovery_, "token").text()).has_value());
    }

    /**
     * One request and its response, sent and read on a client thread while this thread pumps messages.
     * Returns the parsed response, or null when none arrived within kReadTimeoutMs.
     *
     * The response is the line with this request's id (or with no id, which the server sends only for a line
     * it could not parse). A request without a response in time is a failure of the scenario, reported with
     * the longest single message dispatch on this thread meanwhile and the modal windows before and after, which
     * tell a message thread kept busy by another message from a slow method. Its response, when it arrives
     * later, is discarded with a note on how late it was, so that every later request still reads its own
     * response instead of the one before it.
     */
    JsonValue request(const std::string& method, JsonValue params = JsonValue::object())
    {
        const auto id = ++nextId_;
        auto request = JsonValue::object();
        request.appendMember("jsonrpc", JsonValue::string("2.0"));
        request.appendMember("id", JsonValue::integer(id));
        request.appendMember("method", JsonValue::string(method));
        request.appendMember("params", std::move(params));
        const std::string line = toJsonString(request);
        std::optional<std::string> response;
        bool sentOk = false;
        std::vector<std::int64_t> late;
        const int modalBefore = openModalWindows();
        longestMessageDispatchMs() = 0;
        const auto sent = Clock::now();
        sent_[id] = Sent{method, sent};
        runWhilePumping([this, &line, &response, &sentOk, &late, id, sent] {
            sentOk = client_.sendLine(line);
            if (!sentOk)
                return;
            for (;;) {
                const auto left = kReadTimeoutMs - millisecondsSince(sent);
                if (left <= 0)
                    return;
                auto next = client_.readLine(static_cast<int>(left));
                if (!next)
                    return;
                const auto other = responseId(*next);
                if (!other || *other == id) {
                    response = std::move(next);
                    return;
                }
                late.push_back(*other);
            }
        });
        for (const auto other : late)
            noteLateResponse(other);
        if (!response) {
            CONTROL_CHECK(response.has_value());
            if (!sentOk) {
                // Nothing was sent, so no response will ever arrive for this id.
                sent_.erase(id);
                note(method + " (request " + std::to_string(id) + "): the request could not be sent");
            } else
                note(method + " (request " + std::to_string(id) + "): no response within "
                     + std::to_string(kReadTimeoutMs)
                     + " ms; the longest single message dispatch on this thread meanwhile took "
                     + std::to_string(longestMessageDispatchMs()) + " ms; open modal windows "
                     + std::to_string(modalBefore) + " -> " + std::to_string(openModalWindows()));
            return JsonValue();
        }
        sent_.erase(id);
        auto parsed = parseJson(*response);
        return parsed.ok ? parsed.value : JsonValue();
    }

    /** The result of a successful response, or null. */
    JsonValue result(const std::string& method, JsonValue params = JsonValue::object())
    {
        const auto response = request(method, std::move(params));
        const JsonValue* result = response.find("result");
        if (!CONTROL_CHECK(result != nullptr))
            note(method + " failed: " + toJsonString(response));
        return result != nullptr ? *result : JsonValue();
    }

    /** True when the response is an error of this kind and reason (reason "" for none) with executed
        false, as every error of a read is. */
    bool fails(const std::string& method, JsonValue params, const std::string& kind, const std::string& reason)
    {
        const auto response = request(method, std::move(params));
        const JsonValue* data = errorData(response);
        const bool ok =
            data != nullptr && memberOf(*data, "kind").text() == kind
            && (reason.empty() ? data->find("reason") == nullptr : memberOf(*data, "reason").text() == reason)
            && memberOf(*data, "executed").isBoolean() && !memberOf(*data, "executed").asBoolean();
        if (!ok)
            note(method + ": expected " + kind + "(" + reason + "), got " + toJsonString(response));
        return ok;
    }

    /** True when the response is an error of this kind and reason (reason "" for none) with executed true, as an error
        after a change is, and a detail that contains detailPart. */
    bool failsAfterChange(const std::string& method, JsonValue params, const std::string& kind,
                          const std::string& reason, const std::string& detailPart)
    {
        const auto response = request(method, std::move(params));
        const JsonValue* data = errorData(response);
        const bool ok =
            data != nullptr && memberOf(*data, "kind").text() == kind
            && (reason.empty() ? data->find("reason") == nullptr : memberOf(*data, "reason").text() == reason)
            && memberOf(*data, "executed").isBoolean() && memberOf(*data, "executed").asBoolean()
            && memberOf(*data, "detail").text().find(detailPart) != std::string::npos;
        if (!ok)
            note(method + ": expected " + kind + "(" + reason + ") with executed true and \"" + detailPart
                 + "\" in the detail, got " + toJsonString(response));
        return ok;
    }

    const JsonValue& discovery() const noexcept { return discovery_; }
    TestClient& client() noexcept { return client_; }

private:
    struct Sent
    {
        std::string method;
        Clock::time_point at;
    };

    /** The integer id of a response line, or nullopt when it has none (or is not JSON). */
    static std::optional<std::int64_t> responseId(const std::string& line)
    {
        const auto parsed = parseJson(line);
        if (!parsed.ok)
            return std::nullopt;
        const JsonValue* id = parsed.value.find("id");
        if (id == nullptr || !id->isNumber())
            return std::nullopt;
        return static_cast<std::int64_t>(id->asDouble());
    }

    static int openModalWindows()
    {
        const auto* manager = juce::ModalComponentManager::getInstanceWithoutCreating();
        return manager != nullptr ? manager->getNumModalComponents() : 0;
    }

    /** A response that arrived for an earlier request, while this session waited for a later one. */
    void noteLateResponse(std::int64_t id)
    {
        const auto found = sent_.find(id);
        if (found == sent_.end()) {
            note("discarded a response to request " + std::to_string(id) + ", which this session did not wait for");
            return;
        }
        note("discarded the late response to " + found->second.method + " (request " + std::to_string(id) + "), "
             + std::to_string(millisecondsSince(found->second.at)) + " ms after it was sent");
        sent_.erase(found);
    }

    TestClient client_;
    JsonValue discovery_;
    std::int64_t nextId_ = 100;
    /** The requests whose response has not been read: the one in flight and those that got none in time. */
    std::map<std::int64_t, Sent> sent_;
};

// Scenarios of the dispatcher and the read methods (ControlDispatcherTests.cpp).

/** session.info, project.get and the content reads through the dispatcher, with and without an editor
    bridge, including pagination, cursors, analysis states, errors and a reopened project. */
void scenarioReads();
/** Every access to OpenTune state of the operations happens on the message thread; an access from
    another thread is refused and counted. */
void scenarioThreadProbe();
/** A request waits for the message thread when the processor is destroyed: it is answered with
    SHUTTING_DOWN (or TIMEOUT with executed false) and never runs. */
void scenarioDispatchShutdown();
/** A request that does not start within dispatchTimeoutMs gets TIMEOUT(dispatch_timeout) with executed
    false and never runs later. */
void scenarioDispatchTimeout();
/** The specification's vectors of the implemented methods, against the real dispatcher. */
void scenarioVectors();
/** content.getDetails, transport.get, prefs.get and app.getLog (including the redaction of the log) against
    the real dispatcher, with the clips, analysis failures and preferences they report. */
void scenarioDetails();
/** Every method of every capability the instance reports is registered in OpsRegistry, apart from an
    explicit list of exemptions that must stay empty. */
void scenarioCompleteness();
/** The methods run off the message thread exactly when their schema says mainThread false, and
    app.getLog answers while the message thread is held, without TIMEOUT. */
void scenarioOffMainThread();

// Scenarios of the jobs, the write envelope and import.start (ControlJobTests.cpp).

/** job.get and job.cancel: answered without the message thread, the long poll (also while the session
    sends other requests), cancellation per state, retention and eviction, and `executed` of errors. */
void scenarioJobs();
/** A job.get waits for a running job when the processor is destroyed: SHUTTING_DOWN within the shutdown
    bound, and no job work runs afterwards. */
void scenarioJobShutdown();
/** The write envelope in its four states, the IRREVERSIBLE rule, the UI snapshot and the busy checks. */
void scenarioWriteEnvelope();
/** import.start in both editing schemes and all layouts, focus and note generation, revealed tracks,
    item failures, the checks before the job, the busy checks and idempotency. */
void scenarioImport();
/** Importing after a project was opened succeeds although OpenTune reuses clip ids then. */
void scenarioImportAfterOpen();
/** A three-file import cancelled after its first commit reports the committed clip. */
void scenarioImportCancel();
/** The revert recipes of an import delete the imported clips and bring the project back (arrangement.v1); without
    that capability revert is null and the result is IRREVERSIBLE. */
void scenarioImportRevert();
/** The specification's vectors of job.get, job.cancel and import.start, against the real dispatcher. */
void scenarioJobVectors();
/** Every write method (import, export, key, AUTO in both modes, note edits, pitch shift, undo, redo, time-grid
    edits) against every busy condition: BUSY with its reason, executed false, and nothing changed. */
void scenarioBusyMatrix();
/** A client closes its connection while its job.get waits: the waiter is gone, the job and a new connection are
    fine, and destroying the processor afterwards is within the bound. */
void scenarioJobDisconnect();
/** An import whose pitch analysis fails keeps its clip and reports ANALYSIS_FAILED(f0_failed); audio that is too long
    and files with too many channels fail alone with their reasons. */
void scenarioImportFailures();

// Scenarios of export.start and the export lock (ControlExportTests.cpp).

/** export.start of a clip, a track and the master mix: the result, dry contents, ifExists, the path
    checks before the job (also loaded media by file identity), repeated calls, the export lock that the
    UI shares and the busy checks of other writes while an export runs. */
void scenarioExport();
/** The failures of an export job: ANALYSIS_FAILED, RENDER_FAILED after one retry, REVISION_CONFLICT while
    the file is written, NOT_READY after the wait limit, and cancellation while it waits; the target is
    never touched and the export lock is released. */
void scenarioExportFailures();
/** A deferred render retry in the render runtime keeps the export job waiting although every content is ready: it
    ends with NOT_READY(export_wait_timeout) at the wait limit, leaving the target as it was, and writes the file once
    the retry is gone. */
void scenarioExportDeferredRetry();
/** The specification's vectors of export.start, against the real dispatcher. */
void scenarioExportVectors();
/** Edits through the API followed at once by an export, with the render of the new revision held back or published,
    and versions that race with the export: the file is one whole version, or the job fails with
    REVISION_CONFLICT, never a mix, a silent file or one listed as dry. */
void scenarioExportAfterEdit();
/** An export that would overwrite a file and is cancelled while it waits leaves the target as it was and releases
    the lock. */
void scenarioExportCancelOverwrite();
/** An export job that waits, and one that writes, when the service stops: the export lock is released, no temporary
    file is left and the stop is within the bound. */
void scenarioExportDestroy();
/** Import and export in a folder and a file named in Chinese with spaces: the paths round trip. */
void scenarioExportUnicodePath();
/** The reasons of dryContents: a content whose analysis found no voiced frame is listed as unvoiced, and a clip
    imported and never edited, exported at once, as nothingToRender without waiting. */
void scenarioExportDryReasons();
/** An export of a clip whose time grid is not the identity waits until OpenTune rebuilt its time-stretch output
    at the current revisions, also when the job has to queue that rebuild itself, and never writes the clip silent. */
void scenarioExportTimeStretch();

// Scenarios of the undo history, the key and AUTO (ControlEditTests.cpp).

/** edit.getHistory, edit.undo and edit.redo: undo and redo restore the exact states, conflicts leave the user's
    edits as they are, force and the undoIds of UI entries, unsafe entries, changes without an undo entry, dirty,
    the editor's refresh and a project opened since. */
void scenarioUndo();
/** content.setKey: the key aspect only, the revert recipe round trip, key_changed, no-op, the UI's scale, the
    readiness gate and the params. */
void scenarioSetKey();
/** content.autoTune regenerate and notesOnly: hand-computed removed and dropped notes, created notes in range and
    in key, one undo step for several ranges, dryRun, defaults of the params, the checks before the change. */
void scenarioAutoTune();
/** content.editNotes: a table of hand-computed ops (every op, selections by index, range and all, several ops in
    one call), dryRun equal to the commit, tokens equal to a fresh read, undo and redo back to the exact states,
    the checks before the change, no-ops and the params. */
void scenarioEditNotes();
/** Several notes edited in one call after a render: every block of every edited note is queued and rendered anew
    (content.editNotes, AUTO and SNAP), and the file of an export holds the new pitches. It runs about 12 s on a
    developer machine, so it has a timeout of its own. */
void scenarioEditMultiNoteRender();
/** content.setPitchShift: revision stays while the pitchShift component changes (also for the undo, the redo,
    the UI's Ctrl+Z and the UI's dialog), KEY_NOT_TRANSPOSED, no-ops, undo back to the exact state, and the undo of
    a shift after notesOnly, which puts back the shift only. */
void scenarioSetPitchShift();
/** content.autoTune mode snapNotes: the hand-computed snap of every note, ranges as selections, dryRun equal to
    the commit, one undo step for several ranges, the no-op of a second snap, clipKey. */
void scenarioSnapNotes();
/** BUSY(pointer_down), the pointer released, the user's edit, the retry with the old revision (REVISION_CONFLICT), a
    fresh read and the write that then succeeds. */
void scenarioEditConflictRetry();
/** A key change leaves revision as it is: AUTO with an explicit key and the older revision succeeds, and clipKey
    with the older key revision conflicts. */
void scenarioTokenAspects();
/** edit.v1 is reported, with all four of its methods registered; history.v1 as well. */
void scenarioEditCapability();
/** The specification's vectors of edit.*, content.setKey, content.autoTune, content.editNotes and
    content.setPitchShift, against the real dispatcher. */
void scenarioEditVectors();

// Scenarios of the time grid (ControlTimingTests.cpp).

/** content.shiftTiming and content.editTimeGrid op moveHandles: hand-computed handle positions, the two passes of the
    time tool's limits, undo back to the grid before (also with OpenTune's own undo), tokens equal to a fresh read,
    grid_changed, grid_not_seeded, the handle checks, dryRun, no-ops, the readiness gate, busy and the editor's
    notification. */
void scenarioTiming();
/** content.editTimeGrid op seed as a job: handles where OpenTune's timing analysis puts them, a grid with handles
    left as it is, cancellation, and the failures of the checks the job makes again (content removed, busy, analysis
    not ready, analysis failed); a failed or cancelled job writes no handle. */
void scenarioTimeGridSeed();
/** A time-grid edit leaves revision as it is: a note edit by index with the revision from before it succeeds. */
void scenarioTimingTokenAspects();
/** The specification's vectors of content.shiftTiming and content.editTimeGrid, against the real dispatcher. */
void scenarioTimingVectors();

// Scenarios of project files (ControlProjectTests.cpp).

/** Save as, change, open the saved file again: notes equal, handles of the epoch before stale, flags, lists, the final
    rename and the recent projects on the way. */
void scenarioProjectRoundTrip();
/** project.open keeps the track count the file was saved with (the count is project state), leaves the project not
    dirty and reports the tracks that the open added to the view as sideEffects.tracksRevealed. */
void scenarioProjectOpenTrackCount();
/** The checks before a job, busy, the cancellation rules, a target that appears while the job waits, changes made
    while a file is prepared, a project that cannot be read. */
void scenarioProjectJobs();
/** Without an editor: the save and open methods say so, project.clearRecent works. */
void scenarioProjectWithoutEditor();
/** project.saveCopy: the copy is a file and nothing else (the project keeps its file, its unsaved-changes flag, the
    recent projects and the undo history, the editor is told nothing, the main file is the same bytes), holds the
   project as it is in memory and opens as a project of its own. */
void scenarioProjectSaveCopy();
/** project.saveCopy: the checks (params, path, an existing target of any kind including the project's own file,
    busy), a running copy, a target that appears meanwhile, a write that fails: nothing is replaced or left behind. */
void scenarioProjectSaveCopyChecks();
/** The capability project.copy: reported with the method, withheld by an instance that lacks it, and a bridge without
    a project session. */
void scenarioProjectSaveCopyCapability();
/** A project with tuned notes saved and opened again keeps the tuning: the correction segments and their four
    parameters right after the open, then, once the analysis is done again, the corrected flags of the notes and the
    corrected pitch line on the voiced frames. */
void scenarioProjectReopenKeepsTuning();
/** The specification's vectors of project.save, project.saveAs, project.open and project.clearRecent, against the
    real dispatcher with a real project session and a worker pool. */
void scenarioProjectVectors();

// Scenarios of the transport and the tracks (ControlTracksTests.cpp).

/** transport.control, transport.setTempo and transport.setLoop: every action, the position that play from start
    returns to, no-ops, the warnings of a missing audio device and of the loop flag, the revert recipe of the tempo,
    the checks, busy and the editor's notifications. */
void scenarioTransport();
/** track.set, track.add, track.duplicate and track.delete: values, clamping, solo and audibility, the revert recipe,
    the limits, the clip copies of both modes, the removal of a content with its last clip, the checks and the answer
    without an editor. */
void scenarioTracks();
/** The barrier of a track deletion in the undo history: entries recorded through the API that address a moved track,
    entries of the UI, force, the warnings, and an entry whose content left the project. */
void scenarioTrackBarrier();
/** The specification's vectors of the transport and the tracks, and those of the undo history that need the barrier,
    against the real dispatcher with a test bridge and a fake audio device. */
void scenarioTrackVectors();

// Scenarios of the dialogs and the onboarding (ControlUiTests.cpp).

/** ui.getState: the capability with ui.onboardingClose, the busy flags, the dialogs from the bottom without file
    choosers, titles, the onboarding, the scale, without an editor. */
void scenarioUiState();
/** ui.dismiss: params, busy, closing from the top, dialog_commits_on_close, a committed colour without tracks.v1, file
    choosers, a dialog that does not close or opens another, and target onboarding (prevent, close, choice dialog). */
void scenarioUiDismiss();
/** target onboarding on an instance without ui.onboardingClose: prevents, does not close. */
void scenarioUiOnboardingWithheld();
/** ui.setWorkspace: the capability, the params, both switches with the write envelope (one change, the side effect,
    view state, an empty revisionDiff, dirty as it was), the revert recipe sent back, the no-op, busy, a bridge that
    cannot switch or did not, without an editor, and an instance that withholds the capability. */
void scenarioUiWorkspace();
/** The specification's vectors of ui/ (those of ui.setSelection and the scenarios of ui.setWorkspace run in
    scenarioSelectionVectors). */
void scenarioUiVectors();

// Scenarios of preferences and the audio device (ControlPrefsTests.cpp).

/** prefs.set for each setting: changes, warnings, the revert recipe sent back unchanged, no-ops, several changes in one
    call, the closed params and BUSY. */
void scenarioPrefsRoundTrip();
/** A change of the editing scheme as a job: the call changes nothing, the job waits for the editor, the notes the
   editor generates, the revert recipe, the wait that ends without the editor and puts every preference back, and a job
   that the stopping control API destroys. */
void scenarioPrefsSchemeJob();
/** processing.options of prefs.get, and the vocoder weight, the rendering priority and the pitch model of prefs.set:
    values that are not on offer, no-ops, changes with their warnings and side effects, the processor that refuses a
    pitch model, the revert recipes, all members in one call. The render runtime is replaced by a test double
    (ControlPrefsBackend). */
void scenarioPrefsOptions();
/** The wait of the prefsApply job for the vocoder after a new weight or rendering priority: no wait when the vocoder
   was not ready before the change, the wait until it is ready again, the time-out that takes the preferences back. */
void scenarioPrefsVocoderJob();
/** The vocoder wait together with the other waits of the job: the editing scheme, another write, a modal dialog and the
    stopping control API. */
void scenarioPrefsVocoderOverlap();
/** The specification's vectors of prefs.set, against the real dispatcher. */
void scenarioPrefsVectors();
/** audio.get and audio.set on a fake device manager: what is on offer, values that are not, every step of a change, the
    rollback when the device does not open (also when the previous setup does not), no device, ASIO transitions. */
void scenarioAudioDevice();
/** The specification's vectors of audio.get and audio.set, against the real dispatcher. */
void scenarioAudioVectors();

// Scenarios of the arrangement (ControlArrangementTests.cpp).

/** arrangement.edit: every op with its result, its checks and their order, clamping, FADES_RECLAMPED, overlaps, a
    hidden track shown, several ops in one call and one undo entry, dryRun, the closed params, busy and a stale
    arrangement revision. */
void scenarioArrangement();
/** The undo entries of arrangement.edit: a trim's undo restores the fades it shortened, undo and redo of the in-place
    ops, the entries of split, merge and delete that edit.undo refuses, and their revert recipes. */
void scenarioArrangementUndo();
/** arrangement.copy: both modes, several sources, the warnings, a hidden track, the revert recipe sent back, the token
    conflicts and a change elsewhere that keeps a token valid. */
void scenarioArrangementCopy();
/** The specification's vectors of arrangement.edit and arrangement.copy (and the track deletion that needs a move),
    against the real dispatcher. */
void scenarioArrangementVectors();

// Scenarios of the selection (ControlSelectionTests.cpp).

/** ui.setSelection: the capability and its methods, the params, NOT_FOUND, UNSUPPORTED without an editor, BUSY, a clip,
    a track, openEditor, the scale, the no-op, the revert recipe sent back, dryRun changing nothing. */
void scenarioSelection();
/** Both editing schemes, a clip with and without notes: the prediction of dryRun equals the committed result, and
   dryRun leaves every field of the state as it was. */
void scenarioSelectionMatrix();
/** The specification's vectors of ui.setSelection, against the real dispatcher with a test bridge that selects as the
    editor does. */
void scenarioSelectionVectors();

// Scenarios of app.quit (ControlQuitTests.cpp).

/** app.quit: the capability, the closed params, UNSAVED_CHANGES, every BUSY reason and every job kind with the order of
    the checks, and that each refusal changes nothing; then the call that quits. */
void scenarioAppQuit();
/** app.quit without an editor, with a bridge whose window cannot be closed, and on an instance that does not report the
    capability. */
void scenarioAppQuitWithoutWindow();
/** The result: the members, the project path (also non-ASCII), the warning IRREVERSIBLE, exitDeadlineMs. */
void scenarioAppQuitResult();
/** The result is written before the exit step: at the window's close the client has the result, the service is stopped,
    the discovery file is gone and the listener is closed; the normal shutdown that follows is within its bound. */
void scenarioAppQuitSequence();
/** The stopping state begins when the call decides to quit: a second app.quit and a write queued behind it are answered
    with SHUTTING_DOWN and do not run. */
void scenarioAppQuitStopping();
/** The specification's vectors of app.quit (the nine of vectors/app/quit-*), against the real dispatcher with a test
    bridge; the vectors with endsSession also check that the instance has ended. */
void scenarioAppQuitVectors();

// Scenarios of the clip ids after a project was opened (ControlArrangementTests.cpp).

/** controlSkipUsedClipIds() and its companions on a repository with holes in its ids and a retired clip that was not
    reclaimed: what is skipped, that no probe clip is left, that nothing is taken when no id is in the way, and the
    record that makes the next call skip nothing, runs of free ids for several clips, and huge ids. */
void scenarioClipIdSkip();
/** Splitting, merging, copying (both modes) and duplicating a track (mode clone) right after a project was opened
    succeed at once and leave no content without a clip; refused calls and predictions leave the counter alone; ids
    with a hole, and clips with huge ids (2^40, the largest 64-bit value) that must not make the first call wait. */
void scenarioClipIdsAfterOpen();
/** Without a project opened before, the same methods skip nothing and take no ids: the new clips get consecutive ids.
 */
void scenarioClipIdsWithoutOpen();

} // namespace ControlTest
} // namespace OpenTune
