// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scenarios of app.quit against the real processor, with a test bridge that records the request to close the window
// instead of ending the process (TestUiBridge::requestQuit). They cover what the specification says about the call
// (PROTOCOL.md sections 3.6, 13.2, 14.1 and 20.2; methods/app.quit.schema.json): the order of the checks and that a
// refusal changes nothing, the result, that the result is written before the exit step runs, that the stopping state
// begins when the call decides to quit, and that the instance has ended afterwards (connections closed, no listener,
// no discovery file). Registered with the other processor scenarios in ControlProcessorTests.cpp.

#include "ControlProcessorSupport.h"
#include "TestUiBridge.h"

#include "Control/ControlDispatcher.h"
#include "Control/ControlJobs.h"
#include "Control/Ops/OpsRegistry.h"
#include "PluginProcessor.h"
#include "Utils/AppLogger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OpenTune {
namespace ControlTest {

namespace {

/** Bound of one vector's exchange while this thread pumps messages. */
constexpr int kVectorExchangeMs = 30000;
/** The wait for the instance to end after the result of a call that quits (vectors/README.md, "Vectors that end the
    instance", rule 1 and 3). */
constexpr int kEndsSessionWaitMs = 15000;
/** The longest exitDeadlineMs a client can rely on: runners wait this long for the process to end. */
constexpr int kLongestRunnerWaitMs = 15000;
/** The time the exit step has, once the client has the result, to look at the instance in the test's hook. */
constexpr int kReplySeenMs = 5000;
/** The most the vectors' own bookkeeping takes before the first request of a vector. */
constexpr int kQuietAfterRefusalMs = 20;
/** What the exit step logs when the window stays open although the client was told quitting: true
    (AppQuitOps.cpp, endInstance()); the reason follows in brackets. */
constexpr const char* kWindowNotClosedLog =
    "the client was told quitting: true, but the OpenTune window was not closed";
/** What the dispatcher logs when the exit step cannot be queued because the liveness token of the message-thread
    marshalling is dead: the control service is already stopping (ControlDispatcher::runAfterReply()). */
constexpr const char* kExitStepServiceStoppingLog =
    "the exit step of a method that ends the instance was not queued because the control service is already stopping";
/** What the dispatcher logs when the exit step cannot be queued although the token is alive: the message queue refused
    the step (ControlDispatcher::runAfterReply()). */
constexpr const char* kExitStepRefusedLog =
    "the exit step of a method that ends the instance was refused by the message queue";

JsonValue jsonOf(const std::string& text)
{
    auto parsed = parseJson(text);
    CONTROL_CHECK(parsed.ok);
    return parsed.value;
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

/** Reports the incomplete capabilities (none now), and leaves out the withheld capabilities, for the dispatchers
    created during its life; restores the defaults afterwards. */
class ScopedCapabilities
{
public:
    explicit ScopedCapabilities(std::vector<std::string> withheld = {})
    {
        controlSetReportIncompleteCapabilitiesForTesting(true);
        controlSetWithheldCapabilitiesForTesting(std::move(withheld));
    }
    ~ScopedCapabilities()
    {
        controlSetWithheldCapabilitiesForTesting({});
        controlSetReportIncompleteCapabilitiesForTesting(false);
    }

    ScopedCapabilities(const ScopedCapabilities&) = delete;
    ScopedCapabilities& operator=(const ScopedCapabilities&) = delete;
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

/**
 * A started processor with a test bridge registered, which closes its window by counting the request
 * (TestUiBridge::requestQuit()). Members in the order of their construction: the seam before the processor, the bridge
 * after it; they go in the reverse order, so that the bridge is unregistered while the processor still exists.
 */
struct QuitFixture
{
    explicit QuitFixture(std::vector<std::string> withheld = {}, bool withBridge = true)
        : capabilities(std::move(withheld))
        , processor(startedProcessor())
    {
        if (withBridge)
            registered = std::make_unique<ScopedBridge>(processor->getControlService(), bridge);
    }

    ControlService& service() { return processor->getControlService(); }
    /** True when the service listens and a session is open. */
    bool open() { return service().isRunning() && api.open(service()); }

    ScopedCapabilities capabilities;
    std::unique_ptr<OpenTuneAudioProcessor> processor;
    TestUiBridge bridge;
    std::unique_ptr<ScopedBridge> registered;
    ApiSession api;
};

/** The result of a call that carried out the quit, as the schema describes it: exactly its members, quitting true, the
    path or null with its reason, an exitDeadlineMs a client can rely on, the warning that discarding changes needs. */
void checkQuitResult(const JsonValue& result, bool discarded, const std::string* projectPath)
{
    if (!CONTROL_CHECK(result.isObject()))
        return;
    std::set<std::string> members;
    for (std::size_t i = 0; i < result.size(); ++i)
        members.insert(result.keyAt(i));
    std::set<std::string> expected = {"quitting", "discardedUnsavedChanges", "projectPath", "exitDeadlineMs",
                                      "warnings"};
    if (projectPath == nullptr)
        expected.insert("nullReasons");
    CONTROL_CHECK(members == expected);
    CONTROL_CHECK(memberOf(result, "quitting").isBoolean() && memberOf(result, "quitting").asBoolean());
    CONTROL_CHECK(memberOf(result, "discardedUnsavedChanges").isBoolean()
                  && memberOf(result, "discardedUnsavedChanges").asBoolean() == discarded);
    if (projectPath != nullptr) {
        CONTROL_CHECK(memberOf(result, "projectPath").isString()
                      && memberOf(result, "projectPath").text() == *projectPath);
    } else {
        CONTROL_CHECK(memberOf(result, "projectPath").isNull());
        const auto& reasons = memberOf(result, "nullReasons");
        CONTROL_CHECK(reasons.size() == 1 && memberOf(reasons, "projectPath").text() == "no_project_path");
    }
    const auto& deadline = memberOf(result, "exitDeadlineMs");
    const double value = deadline.asDouble();
    CONTROL_CHECK(deadline.isNumber() && value == std::floor(value) && value >= 1 && value <= kLongestRunnerWaitMs);
    const auto& warnings = memberOf(result, "warnings");
    if (!discarded) {
        CONTROL_CHECK(warnings.isArray() && warnings.size() == 0);
        return;
    }
    if (!CONTROL_CHECK(warnings.isArray() && warnings.size() == 1))
        return;
    const auto& warning = warnings.at(0);
    CONTROL_CHECK(memberOf(warning, "code").text() == "IRREVERSIBLE" && warning.size() == 3);
    const auto& subject = memberOf(warning, "subject");
    CONTROL_CHECK(subject.size() == 1 && memberOf(subject, "singleton").text() == "project");
    CONTROL_CHECK(warning.find("data") != nullptr && memberOf(warning, "data").isNull());
}

/**
 * Waits for the instance to end after a call that quits, as vectors/README.md rule 3 asks as far as a headless test
 * can see: the connection of the session is closed by the server within kEndsSessionWaitMs and sent no byte more, the
 * window was asked to close exactly once, no connection is accepted any more and the discovery file is gone. (The exit
 * status of a process is observed by the run against the real executable.) Empty on success, otherwise why not. This
 * thread pumps messages meanwhile: the exit step runs on it.
 */
std::string waitForEnd(QuitFixture& f, int port, const std::filesystem::path& discoveryFile)
{
    std::size_t bytes = 0;
    bool closed = false;
    runWhilePumping([&] { closed = f.api.client().waitClosed(kEndsSessionWaitMs, bytes); }, kEndsSessionWaitMs + 5000);
    if (!closed)
        return "the server did not close the connection";
    if (bytes != 0)
        return "the server sent bytes after the result";
    if (!pumpMessagesUntil([&f] { return f.bridge.quitRequests > 0; }, kEndsSessionWaitMs))
        return "the window was not asked to close";
    pumpMessagesFor(kQuietPumpMs);
    if (f.bridge.quitRequests != 1)
        return "the window was asked to close " + std::to_string(f.bridge.quitRequests) + " times";
    if (ControlSocket::connectToLoopback(port, 500).isValid())
        return "a connection was accepted after the instance ended";
    if (std::filesystem::exists(discoveryFile))
        return "the discovery file is still there";
    if (f.service().isRunning() || !f.service().isStopping())
        return "the control service is not stopped";
    return {};
}

/** Sends app.quit and returns its result, then waits for the instance to end. */
JsonValue quitAndWait(QuitFixture& f, JsonValue params = JsonValue::object())
{
    const int port = f.service().port();
    const auto discoveryFile = std::filesystem::u8path(f.service().discoveryFilePath());
    const auto result = f.api.result("app.quit", std::move(params));
    const auto failure = waitForEnd(f, port, discoveryFile);
    if (!failure.empty())
        note("the instance did not end: " + failure);
    CONTROL_CHECK(failure.empty());
    return result;
}

/** app.quit with these params is refused with this error and executed false, and then nothing has happened: the
    window was not asked to close, the instance runs, is not stopping and answers a request. */
bool refuses(QuitFixture& f, const std::string& params, const char* kind, const char* reason)
{
    bool ok = f.api.fails("app.quit", jsonOf(params), kind, reason);
    // A refusal never schedules an exit step; one that did would run within moments of the response.
    pumpMessagesFor(kQuietAfterRefusalMs);
    ok = CONTROL_CHECK(f.bridge.quitRequests == 0) && ok;
    ok = CONTROL_CHECK(f.service().isRunning() && !f.service().isStopping()) && ok;
    ok = CONTROL_CHECK(!f.service().dispatcher()->isStopping()) && ok;
    ok = CONTROL_CHECK(f.api.result("session.info").isObject()) && ok;
    if (!ok)
        note("app.quit " + params + ": expected " + kind + "(" + reason + ")");
    return ok;
}

/** A job of the fixture that runs until released (or cancelled). */
struct HeldJob
{
    std::atomic<bool> release{false};
    std::string id;
};

std::string jobState(QuitFixture& f, const std::string& jobId)
{
    JsonValue job;
    if (f.service().jobs()->get(jobId, job) != ControlJobs::Lookup::Found)
        return "unknown";
    return memberOf(job, "state").text();
}

/** Starts a job of this kind and waits until it runs; null when it does not. */
std::shared_ptr<HeldJob> startHeldJob(QuitFixture& f, const std::string& kind)
{
    auto held = std::make_shared<HeldJob>();
    ControlJobWork work = [held](ControlJobStep& step) {
        if (step.cancelRequested())
            step.cancel();
        else if (held->release.load())
            step.succeed(JsonValue::object());
    };
    const auto started = f.service().jobs()->start(kind, true, work);
    if (started.jobId.empty())
        return nullptr;
    held->id = started.jobId;
    if (!pumpMessagesUntil([&f, held] { return jobState(f, held->id) == "running"; }, kReadTimeoutMs))
        return nullptr;
    return held;
}

/** Lets a held job succeed and waits until it has. */
bool finishHeldJob(QuitFixture& f, const std::shared_ptr<HeldJob>& held)
{
    held->release = true;
    return pumpMessagesUntil([&f, held] { return jobState(f, held->id) == "succeeded"; }, kReadTimeoutMs);
}

/** The events of ControlDispatcher::setAfterReplyObserverForTesting(), in the order they happened, with whether each
    was seen on the message thread. */
class AfterReplyLog
{
public:
    using Event = ControlDispatcher::AfterReplyEvent;

    explicit AfterReplyLog(ControlDispatcher& dispatcher)
    {
        dispatcher.setAfterReplyObserverForTesting([this](Event event) {
            const std::lock_guard<std::mutex> lock(mutex_);
            events_.emplace_back(event, juce::MessageManager::existsAndIsCurrentThread());
        });
    }

    std::vector<std::pair<Event, bool>> events() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

    /** The reply was written by a connection thread, and only after it the exit step started on the message thread. */
    bool inOrder() const
    {
        const auto seen = events();
        return seen.size() == 2 && seen[0].first == Event::ReplyWritten && !seen[0].second
               && seen[1].first == Event::ExitStepStarted && seen[1].second;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<Event, bool>> events_;
};

/** A job that ends at once in the given state, so that it is terminal and no longer keeps the instance busy. */
bool startTerminalJob(QuitFixture& f, const std::string& kind, bool fails)
{
    ControlJobWork work = [fails](ControlJobStep& step) {
        if (fails)
            step.fail(ControlJobError{"INTERNAL", "", "a failed job of the fixture", false});
        else
            step.cancel();
    };
    const auto started = f.service().jobs()->start(kind, true, work);
    if (started.jobId.empty())
        return false;
    const std::string jobId = started.jobId;
    return pumpMessagesUntil(
        [&f, jobId] {
            const auto state = jobState(f, jobId);
            return state == "failed" || state == "cancelled";
        },
        kReadTimeoutMs);
}

/**
 * The exit step finds that the window does not close, which the result could not know: the client was told quitting:
 * true. editorGone false: the editor's window refuses when it is asked (TestUiBridge::refusesClose); true: the editor
 * is gone when the exit step runs (its bridge is unregistered between the result and the step). Either way the control
 * service is stopped first (the interface is down before the window is asked, never after), nothing is closed, the
 * application keeps running, and the log says what the client was told and why the window stayed.
 */
void windowNotClosed(bool editorGone)
{
    QuitFixture f;
    if (!f.open())
        return;
    auto& service = f.service();
    const auto dispatcher = service.dispatcher();
    const int port = service.port();
    const auto discoveryFile = std::filesystem::u8path(service.discoveryFilePath());
    const std::string expectedLine = std::string(kWindowNotClosedLog) + " ("
                                     + (editorGone ? "the editor is gone" : "the editor did not close it") + ")";
    const auto logged = occurrences(logText(), expectedLine);

    // What the instance is like at the moment the window is asked (refusesClose only: an editor that is gone is not).
    struct Asked
    {
        int calls = 0;
        bool onMessageThread = false;
        bool serviceStopped = false;
        bool dispatcherStopping = false;
        bool discoveryGone = false;
    } asked;
    if (editorGone) {
        // The registration goes on the message thread before the exit step: both are queued from the connection thread,
        // the first by this observer (ReplyWritten comes before runAfterReply() posts the step), and the queue is FIFO.
        dispatcher->setAfterReplyObserverForTesting([&f](ControlDispatcher::AfterReplyEvent event) {
            if (event == ControlDispatcher::AfterReplyEvent::ReplyWritten)
                juce::MessageManager::callAsync([&f] { f.registered.reset(); });
        });
    } else {
        f.bridge.refusesClose = true;
        f.bridge.onQuitRequest = [&] {
            ++asked.calls;
            asked.onMessageThread = juce::MessageManager::existsAndIsCurrentThread();
            asked.serviceStopped = !service.isRunning() && service.isStopping();
            asked.dispatcherStopping = dispatcher->isStopping();
            asked.discoveryGone = !std::filesystem::exists(discoveryFile);
        };
    }

    // The client is told that the instance quits, as it is when the window closes.
    checkQuitResult(f.api.result("app.quit"), false, nullptr);
    const bool logLine =
        pumpMessagesUntil([&] { return occurrences(logText(), expectedLine) == logged + 1; }, kEndsSessionWaitMs);
    CONTROL_CHECK(logLine);
    if (!logLine)
        note("the log has no line " + expectedLine);
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(occurrences(logText(), expectedLine) == logged + 1);

    // The window was asked once (or not at all, without an editor) and nothing was closed.
    CONTROL_CHECK(f.bridge.quitRequests == 0 && f.bridge.refusedQuitRequests == (editorGone ? 0 : 1));
    if (!editorGone) {
        CONTROL_CHECK(asked.calls == 1 && asked.onMessageThread);
        // The service was stopped before the window was asked.
        CONTROL_CHECK(asked.serviceStopped && asked.dispatcherStopping && asked.discoveryGone);
    }
    // The control API is stopped all the same: connections closed, no listener, no discovery file; the application
    // keeps running (this test is still here to look).
    std::size_t bytes = 0;
    bool closed = false;
    runWhilePumping([&] { closed = f.api.client().waitClosed(kEndsSessionWaitMs, bytes); }, kEndsSessionWaitMs + 5000);
    CONTROL_CHECK(closed && bytes == 0);
    CONTROL_CHECK(!service.isRunning() && service.isStopping() && dispatcher->isStopping());
    CONTROL_CHECK(!ControlSocket::connectToLoopback(port, 500).isValid());
    CONTROL_CHECK(!std::filesystem::exists(discoveryFile));
}

/**
 * The exit step cannot be queued because the token is dead (ControlDispatcher::runAfterReply()): the observer kills
 * the liveness token of the message-thread marshalling as soon as the result is written, which makes post() return
 * false and isAlive() false, as when the service has been stopped on the message thread. The client has the result
 * with quitting: true; the exit step never runs, so the window is not asked; the dispatcher answers SHUTTING_DOWN;
 * and the log says that the service is already stopping and nothing is left to stop: not that anything stays up.
 */
void exitStepNotQueued()
{
    QuitFixture f;
    if (!f.open())
        return;
    auto& service = f.service();
    const auto dispatcher = service.dispatcher();
    const auto messageThread = service.messageThread();
    // ControlMessageThread::stop() takes its own lock and is safe on the connection thread that this observer runs on.
    dispatcher->setAfterReplyObserverForTesting([messageThread](ControlDispatcher::AfterReplyEvent event) {
        if (event == ControlDispatcher::AfterReplyEvent::ReplyWritten)
            messageThread->stop();
    });
    const auto logged = occurrences(logText(), kExitStepServiceStoppingLog);
    const auto loggedRefused = occurrences(logText(), kExitStepRefusedLog);

    checkQuitResult(f.api.result("app.quit"), false, nullptr);
    const bool logLine = pumpMessagesUntil(
        [&] { return occurrences(logText(), kExitStepServiceStoppingLog) == logged + 1; }, kEndsSessionWaitMs);
    CONTROL_CHECK(logLine);
    if (!logLine)
        note(std::string("the log has no line with: ") + kExitStepServiceStoppingLog);
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(occurrences(logText(), kExitStepServiceStoppingLog) == logged + 1);
    // The line says what the client was told and that nothing is left to stop; it is not the line of a refusing queue.
    CONTROL_CHECK(occurrences(logText(), kExitStepRefusedLog) == loggedRefused);
    const auto log = logText();
    const auto line = log.find(kExitStepServiceStoppingLog);
    CONTROL_CHECK(line != std::string::npos && log.find("quits", line) != std::string::npos
                  && log.find("SHUTTING_DOWN", line) != std::string::npos
                  && log.find("nothing left to stop", line) != std::string::npos);

    CONTROL_CHECK(!messageThread->isAlive() && messageThread->isStopped());
    CONTROL_CHECK(f.bridge.quitRequests == 0 && f.bridge.refusedQuitRequests == 0);
    CONTROL_CHECK(dispatcher->isStopping());
    CONTROL_CHECK(f.api.fails("session.info", JsonValue::object(), "SHUTTING_DOWN", ""));
}

/**
 * The exit step cannot be queued because the message queue refuses it while the token is alive
 * (ControlDispatcher::runAfterReply()), as while the application quits. The observer calls
 * juce::MessageManager::stopDispatchLoop() as soon as the result is written, which is what JUCEApplicationBase::quit()
 * does, and returns once juce::MessageManager::hasStopMessageBeenSent() holds: from then on
 * juce::MessageManager::callAsync() returns false, so post() fails with the token still alive (isAlive() true). The
 * client has the result with quitting: true; the exit step never runs, so the window is not asked and the service is
 * not stopped; the dispatcher answers SHUTTING_DOWN; and the log says that the queue refused the step and that the
 * listener and the discovery file stay until the application closes itself.
 *
 * Why the observer waits: when the flag is set depends on the platform (JUCE 8.0.12, the vendored JUCE).
 *  - Windows and Linux use the generic MessageManager::stopDispatchLoop() in
 *    juce_events/messages/juce_MessageManager.cpp (juce_events/native/juce_Messaging_linux.cpp defines no
 *    stopDispatchLoop() of its own, and the generic one is compiled for everything but macOS, iOS and Android). It
 *    posts the quit message and then sets quitMessagePosted, from any thread, so the flag is set when the call returns.
 *  - macOS has MessageManager::stopDispatchLoop() in juce_events/native/juce_MessageManager_mac.mm. On the message
 *    thread it sets the flag and calls shutdownNSApp(). On any other thread, which is where the observer runs (the
 *    connection thread that wrote the result), it only posts a QuitCallback, and the flag is set when the message
 *    thread dispatches that callback. Without the wait, the post of the exit step would still succeed there.
 * The wait is bounded by kEndsSessionWaitMs. It holds no lock and no lease of the processor: the observer runs on the
 * connection thread after the result was written, outside ControlMessageThread::call() (see ControlDispatcher::
 * handleRequest()), and ControlServer holds no mutex while a handler runs. The message thread, which is this thread,
 * dispatches the QuitCallback meanwhile: ApiSession::request() pumps messages until the client has read the result,
 * and the pumpMessagesUntil() right after it pumps again, whether the callback was posted before or after the client
 * finished. On macOS QuitCallback also calls shutdownNSApp() ([NSApp stop:] and starting AppKit's periodic events),
 * which only matters to an NSApplication run loop; the pump of these tests is CFRunLoopRunInMode() (see
 * ControlHeadlessSupport.h), which does not dequeue NSEvents, so it does not change what the pump dispatches.
 *
 * Run it last in its process: quitMessagePosted is set only by the two stopDispatchLoop() implementations above (and by
 * ~MessageManager()) and nothing resets it, so on every platform it stays set for the rest of the process: every
 * message posted afterwards is dropped (MessageBase::post() checks it) and runDispatchLoop() returns at once. This
 * was observed by running the tests on Windows and on macOS. startedProcessor() fails the scenario when a
 * case starts after the flag was set. The pump of the tests dispatches what was posted before, which is all this case
 * needs.
 */
void exitStepRefusedByQueue()
{
    QuitFixture f;
    if (!f.open())
        return;
    auto& service = f.service();
    const auto dispatcher = service.dispatcher();
    const auto messageThread = service.messageThread();
    const auto discoveryFile = std::filesystem::u8path(service.discoveryFilePath());
    CONTROL_CHECK(messageThread->isAlive());
    // Here the connection thread that writes the result calls stopDispatchLoop() and waits (bounded, no lease held)
    // until the flag is set; whether it was is checked below, on this thread.
    dispatcher->setAfterReplyObserverForTesting([](ControlDispatcher::AfterReplyEvent event) {
        if (event != ControlDispatcher::AfterReplyEvent::ReplyWritten)
            return;
        auto* messages = juce::MessageManager::getInstance();
        messages->stopDispatchLoop();
        waitUntil([messages] { return messages->hasStopMessageBeenSent(); }, kEndsSessionWaitMs);
    });
    const auto logged = occurrences(logText(), kExitStepRefusedLog);
    const auto loggedStopping = occurrences(logText(), kExitStepServiceStoppingLog);

    checkQuitResult(f.api.result("app.quit"), false, nullptr);
    const bool logLine = pumpMessagesUntil([&] { return occurrences(logText(), kExitStepRefusedLog) == logged + 1; },
                                           kEndsSessionWaitMs);
    CONTROL_CHECK(logLine);
    if (!logLine)
        note(std::string("the log has no line with: ") + kExitStepRefusedLog);
    // The flag never goes back, so this is also what the observer waited for; the checks below still run when it is
    // not set, and nothing here can hang.
    const bool stopTookEffect = juce::MessageManager::getInstance()->hasStopMessageBeenSent();
    CONTROL_CHECK(stopTookEffect);
    if (!stopTookEffect)
        note("stopDispatchLoop() did not take effect within kEndsSessionWaitMs: the message queue still accepts "
             "messages, so the exit step was not refused");
    pumpMessagesFor(kQuietPumpMs);
    CONTROL_CHECK(occurrences(logText(), kExitStepRefusedLog) == logged + 1);
    // The queue refuses, and the token is alive: that is what makes it this case and not the one of
    // exitStepNotQueued().
    CONTROL_CHECK(!juce::MessageManager::callAsync([] {}));
    CONTROL_CHECK(messageThread->isAlive() && !messageThread->isStopped() && !messageThread->post([] {}));
    CONTROL_CHECK(occurrences(logText(), kExitStepServiceStoppingLog) == loggedStopping);
    // The line says what the client was told and what is left running until the application closes itself.
    const auto log = logText();
    const auto line = log.find(kExitStepRefusedLog);
    CONTROL_CHECK(line != std::string::npos && log.find("quits", line) != std::string::npos
                  && log.find("SHUTTING_DOWN", line) != std::string::npos
                  && log.find("discovery file stay until the application closes itself", line) != std::string::npos);

    // The exit step never ran: the window was not asked, and the service was not stopped (the dispatcher refuses new
    // requests, but the listener and the discovery file stay).
    CONTROL_CHECK(f.bridge.quitRequests == 0 && f.bridge.refusedQuitRequests == 0);
    CONTROL_CHECK(dispatcher->isStopping() && service.isRunning());
    CONTROL_CHECK(std::filesystem::exists(discoveryFile));
    CONTROL_CHECK(f.api.fails("session.info", JsonValue::object(), "SHUTTING_DOWN", ""));
}

} // namespace

// ---- The checks, in order ----------------------------------------------------------------------------

void scenarioAppQuit()
{
    QuitFixture f;
    if (!f.open())
        return;
    auto& bridge = f.bridge;
    const auto dispatcher = f.service().dispatcher();
    if (!CONTROL_CHECK(dispatcher != nullptr))
        return;

    AfterReplyLog afterReply(*dispatcher);

    // The method is registered and its capability reported, in the table of the protocol as well.
    CONTROL_CHECK(dispatcher->implements("app.quit"));
    CONTROL_CHECK(std::string(controlMethodCapability("app.quit")) == "app.quit");
    const auto capabilities = dispatcher->capabilities();
    CONTROL_CHECK(std::find(capabilities.begin(), capabilities.end(), "app.quit") != capabilities.end());
    const auto info = f.api.result("session.info");
    CONTROL_CHECK(toJsonString(memberOf(info, "capabilities")).find("\"app.quit\"") != std::string::npos);

    // The closed params: a consent that is not the boolean true is never taken, and nothing else is accepted.
    bridge.project.dirty = true;
    for (const char* params : {R"({"discardUnsaved":"yes"})", R"({"discardUnsaved":1})", R"({"discardUnsaved":null})",
                               R"({"discardUnsaved":"true"})", R"({"force":true})",
                               R"({"discardUnsaved":true,"force":true})", R"({"confirm":true})"})
        CONTROL_CHECK(refuses(f, params, "INVALID_ARGUMENT", "invalid_params"));

    // UNSAVED_CHANGES: the project has unsaved changes and discardUnsaved is not true.
    CONTROL_CHECK(refuses(f, "{}", "UNSAVED_CHANGES", "project_dirty"));
    CONTROL_CHECK(refuses(f, R"({"discardUnsaved":false})", "UNSAVED_CHANGES", "project_dirty"));
    CONTROL_CHECK(bridge.project.dirty && bridge.dirtyMarks == 0);

    // BUSY comes first, for each reason of section 13.2, also with unsaved changes and with the consent to lose them.
    using Busy = ControlUiBridge::BusyState;
    struct Flag
    {
        const char* reason;
        bool Busy::*member;
    };
    const Flag flags[] = {{"pointer_down", &Busy::pointerDown},
                          {"modal_open", &Busy::modalOpen},
                          {"import_in_progress", &Busy::importRunning},
                          {"export_in_progress", &Busy::exportRunning},
                          {"project_operation", &Busy::operationRunning}};
    for (const auto& flag : flags) {
        bridge.busy.*(flag.member) = true;
        CONTROL_CHECK(refuses(f, "{}", "BUSY", flag.reason));
        CONTROL_CHECK(refuses(f, R"({"discardUnsaved":true})", "BUSY", flag.reason));
        bridge.busy.*(flag.member) = false;
    }
    // The order of the reasons: all five at once, then the first of them goes, one after the other.
    for (const auto& flag : flags)
        bridge.busy.*(flag.member) = true;
    for (const auto& flag : flags) {
        CONTROL_CHECK(refuses(f, R"({"discardUnsaved":true})", "BUSY", flag.reason));
        bridge.busy.*(flag.member) = false;
    }
    // An export that holds the export lock, without the editor's flag: the lock of the processor counts as well.
    if (CONTROL_CHECK(f.processor->tryAcquireExportLock())) {
        CONTROL_CHECK(refuses(f, R"({"discardUnsaved":true})", "BUSY", "export_in_progress"));
        f.processor->releaseExportLock();
    }
    // Without busy, the project's changes are what refuses the call.
    CONTROL_CHECK(refuses(f, "{}", "UNSAVED_CHANGES", "project_dirty"));

    // BUSY for a job that has not finished: the reason of the job's kind where section 13.2 has one, job_in_progress
    // for any other kind. While it runs the call is refused (also before the question about unsaved changes); once it
    // has finished, the unsaved changes are what is refused.
    const std::pair<const char*, const char*> kinds[] = {
        {"import", "import_in_progress"},     {"export", "export_in_progress"},
        {"projectOpen", "project_operation"}, {"projectSave", "project_operation"},
        {"projectCopy", "project_operation"}, {"prefsApply", "job_in_progress"},
        {"timeGridSeed", "job_in_progress"},  {"someFutureKind", "job_in_progress"}};
    for (const auto& [kind, reason] : kinds) {
        const auto job = startHeldJob(f, kind);
        if (!CONTROL_CHECK(job != nullptr))
            continue;
        CONTROL_CHECK(refuses(f, "{}", "BUSY", reason));
        CONTROL_CHECK(refuses(f, R"({"discardUnsaved":true})", "BUSY", reason));
        CONTROL_CHECK(finishHeldJob(f, job));
        CONTROL_CHECK(refuses(f, "{}", "UNSAVED_CHANGES", "project_dirty"));
    }
    // Two jobs: the reasons of section 13.2 come before job_in_progress, whichever job started first.
    {
        const auto older = startHeldJob(f, "prefsApply");
        const auto newer = startHeldJob(f, "import");
        if (CONTROL_CHECK(older != nullptr && newer != nullptr)) {
            CONTROL_CHECK(refuses(f, "{}", "BUSY", "import_in_progress"));
            CONTROL_CHECK(finishHeldJob(f, newer));
            CONTROL_CHECK(refuses(f, "{}", "BUSY", "job_in_progress"));
            // A busy flag of the editor comes before any job.
            bridge.busy.pointerDown = true;
            CONTROL_CHECK(refuses(f, "{}", "BUSY", "pointer_down"));
            bridge.busy.pointerDown = false;
            CONTROL_CHECK(finishHeldJob(f, older));
        }
    }
    // A job that failed or was cancelled is terminal: it keeps nothing busy.
    CONTROL_CHECK(startTerminalJob(f, "prefsApply", true));
    CONTROL_CHECK(startTerminalJob(f, "import", false));
    CONTROL_CHECK(refuses(f, "{}", "UNSAVED_CHANGES", "project_dirty"));

    // Every refusal left the project as it was and the instance running: with the consent, the call now quits, and
    // reports that it discarded the changes (no project file, so projectPath is null).
    CONTROL_CHECK(bridge.project.dirty && bridge.dirtyMarks == 0);
    // No refusal got as far as an exit step.
    CONTROL_CHECK(afterReply.events().empty());
    const auto result = quitAndWait(f, jsonOf(R"({"discardUnsaved":true})"));
    checkQuitResult(result, true, nullptr);
    CONTROL_CHECK(afterReply.inOrder());
    CONTROL_CHECK(bridge.project.dirty && bridge.dirtyMarks == 0);
    CONTROL_CHECK(dispatcher->threadProbe().otherThreadAccesses.load() == 0);
}

void scenarioAppQuitWithoutWindow()
{
    // Without an editor: only the editor's window can end the application, so the call is UNSUPPORTED and nothing runs.
    {
        QuitFixture f({}, false);
        if (!f.open())
            return;
        CONTROL_CHECK(f.api.fails("app.quit", jsonOf("{}"), "UNSUPPORTED", "no_editor_window"));
        CONTROL_CHECK(f.api.fails("app.quit", jsonOf(R"({"discardUnsaved":true})"), "UNSUPPORTED", "no_editor_window"));
        CONTROL_CHECK(f.api.fails("app.quit", jsonOf(R"({"discardUnsaved":1})"), "INVALID_ARGUMENT", "invalid_params"));
        pumpMessagesFor(kQuietPumpMs);
        CONTROL_CHECK(f.service().isRunning() && !f.service().isStopping());
        CONTROL_CHECK(f.api.result("session.info").isObject());
    }
    // A bridge whose window cannot be closed (no Standalone window around the editor).
    {
        QuitFixture f;
        if (!f.open())
            return;
        f.bridge.quits = false;
        CONTROL_CHECK(refuses(f, "{}", "UNSUPPORTED", "capability_missing"));
        CONTROL_CHECK(refuses(f, R"({"discardUnsaved":true})", "UNSUPPORTED", "capability_missing"));
        // The window can be closed again: the call goes through.
        f.bridge.quits = true;
        checkQuitResult(quitAndWait(f), false, nullptr);
    }
    // An instance that does not report app.quit: the listener answers, nothing runs.
    {
        QuitFixture f(std::vector<std::string>{"app.quit"});
        if (!f.open())
            return;
        const auto capabilities = f.service().dispatcher()->capabilities();
        CONTROL_CHECK(std::find(capabilities.begin(), capabilities.end(), "app.quit") == capabilities.end());
        CONTROL_CHECK(refuses(f, "{}", "UNSUPPORTED", "capability_missing"));
        CONTROL_CHECK(f.service().dispatcher()->executedCount() == 1 /* the session.info of refuses() */);
    }
    // A window that does not close when the exit step asks, and an editor that is gone by then: the client was told
    // quitting: true, the service is stopped before the window is asked, and the log says what happened.
    windowNotClosed(false);
    windowNotClosed(true);
}

// ---- The result ---------------------------------------------------------------------------------------

void scenarioAppQuitResult()
{
    // A project never saved: projectPath null with its reason; nothing is discarded, there is no warning.
    {
        QuitFixture f;
        if (!f.open())
            return;
        checkQuitResult(quitAndWait(f), false, nullptr);
    }
    // The consent given for a project without changes discards nothing.
    {
        QuitFixture f;
        if (!f.open())
            return;
        checkQuitResult(quitAndWait(f, jsonOf(R"({"discardUnsaved":true})")), false, nullptr);
    }
    // Changes discarded: the flag, and the warning IRREVERSIBLE with the project as its subject.
    {
        QuitFixture f;
        if (!f.open())
            return;
        f.bridge.project.dirty = true;
        checkQuitResult(quitAndWait(f, jsonOf(R"({"discardUnsaved":true})")), true, nullptr);
    }
    // A saved project: its file, an untrusted path that is returned as it is, also with non-ASCII characters.
    const auto directory = isolatedDataDirectory() / std::filesystem::u8path("quit-results");
    for (const bool dirty : {false, true}) {
        QuitFixture f;
        if (!f.open())
            return;
        const std::string path =
            (directory / std::filesystem::u8path("\xe9\xa1\xb9\xe7\x9b\xae \xc3\xbc.otproj")).u8string();
        f.bridge.project.projectPath = path;
        f.bridge.project.dirty = dirty;
        if (dirty) {
            CONTROL_CHECK(refuses(f, R"({"discardUnsaved":false})", "UNSAVED_CHANGES", "project_dirty"));
            checkQuitResult(quitAndWait(f, jsonOf(R"({"discardUnsaved":true})")), true, &path);
        } else {
            checkQuitResult(quitAndWait(f, jsonOf(R"({"discardUnsaved":false})")), false, &path);
        }
        // The project file is never written, the flag never changed.
        CONTROL_CHECK(f.bridge.project.dirty == dirty && f.bridge.dirtyMarks == 0 && f.bridge.projectSaves == 0);
        CONTROL_CHECK(
            !std::filesystem::exists(directory / std::filesystem::u8path("\xe9\xa1\xb9\xe7\x9b\xae \xc3\xbc.otproj")));
    }
}

// ---- Reply first, then quit ---------------------------------------------------------------------------

void scenarioAppQuitSequence()
{
    QuitFixture f;
    if (!f.open())
        return;
    auto& service = f.service();
    const auto dispatcher = service.dispatcher();
    const int port = service.port();
    const auto discoveryFile = std::filesystem::u8path(service.discoveryFilePath());
    CONTROL_CHECK(std::filesystem::exists(discoveryFile));
    AfterReplyLog afterReply(*dispatcher);

    // What the hook finds at the moment the editor's bridge is asked to close the window. The client has read the
    // result by then (the result is written before the exit step is even queued), the control service has stopped
    // (listener closed, discovery file deleted, the dispatcher in the stopping state), and it all happens on the
    // message thread, once.
    struct Seen
    {
        int calls = 0;
        bool onMessageThread = false;
        bool replyReceived = false;
        bool serviceStopped = false;
        bool dispatcherStopping = false;
        bool discoveryGone = false;
        bool listenerClosed = false;
        bool orderedBefore = false;
    } seen;
    std::atomic<bool> replyReceived{false};
    f.bridge.onQuitRequest = [&] {
        ++seen.calls;
        seen.onMessageThread = juce::MessageManager::existsAndIsCurrentThread();
        // The client reads on a thread of its own; it has the result unless the exit step overtook it.
        seen.replyReceived = waitUntil([&replyReceived] { return replyReceived.load(); }, kReplySeenMs);
        seen.serviceStopped = !service.isRunning() && service.isStopping();
        seen.dispatcherStopping = dispatcher->isStopping();
        seen.discoveryGone = !std::filesystem::exists(discoveryFile);
        seen.listenerClosed = !ControlSocket::connectToLoopback(port, 500).isValid();
        // The result was written (on a connection thread) before the exit step started (on this thread).
        seen.orderedBefore = afterReply.inOrder();
    };

    std::optional<std::string> reply;
    std::size_t bytes = 0;
    bool closed = false;
    std::atomic<bool> done{false};
    std::thread client([&] {
        if (f.api.client().sendLine(R"({"jsonrpc":"2.0","id":42,"method":"app.quit","params":{}})")) {
            reply = f.api.client().readLine(kReplySeenMs);
            replyReceived = reply.has_value();
            closed = f.api.client().waitClosed(kEndsSessionWaitMs, bytes);
        }
        done = true;
    });
    const bool finished = pumpMessagesUntil([&done] { return done.load(); }, kEndsSessionWaitMs + kReplySeenMs);
    client.join();
    CONTROL_CHECK(finished);
    pumpMessagesFor(kQuietPumpMs);

    if (CONTROL_CHECK(reply.has_value())) {
        const auto parsed = parseJson(*reply);
        CONTROL_CHECK(parsed.ok && responseIdText(reply) == "42" && parsed.value.find("result") != nullptr);
        checkQuitResult(memberOf(parsed.value, "result"), false, nullptr);
    }
    CONTROL_CHECK(seen.calls == 1 && f.bridge.quitRequests == 1);
    CONTROL_CHECK(seen.onMessageThread);
    CONTROL_CHECK(seen.replyReceived);
    CONTROL_CHECK(seen.serviceStopped);
    CONTROL_CHECK(seen.dispatcherStopping);
    CONTROL_CHECK(seen.discoveryGone);
    CONTROL_CHECK(seen.listenerClosed);
    CONTROL_CHECK(seen.orderedBefore);
    CONTROL_CHECK(afterReply.inOrder());
    CONTROL_CHECK(closed && bytes == 0);
    CONTROL_CHECK(!std::filesystem::exists(discoveryFile));

    // The normal shutdown that follows the window's close finds the service stopped already, and ends within its bound.
    f.registered.reset();
    const auto started = Clock::now();
    f.processor.reset();
    const auto elapsed = millisecondsSince(started);
    note("processor destroyed " + std::to_string(elapsed) + " ms after the quit");
    CONTROL_CHECK(elapsed <= kShutdownBoundMs);
    CONTROL_CHECK(!std::filesystem::exists(discoveryFile));
}

namespace {

/** The stopping state with requests queued behind the quit (see the comment where the three sessions are sent). */
void requestsQueuedBehindTheQuit()
{
    QuitFixture f;
    if (!f.open())
        return;
    auto& service = f.service();
    const auto dispatcher = service.dispatcher();
    const auto messageThread = service.messageThread();
    AfterReplyLog afterReply(*dispatcher);
    ApiSession second;
    ApiSession third;
    if (!CONTROL_CHECK(second.open(service)) || !CONTROL_CHECK(third.open(service)))
        return;
    const double tempoBefore = f.processor->getBpm();
    const int port = service.port();
    const auto discoveryFile = std::filesystem::u8path(service.discoveryFilePath());

    // Three sessions queue their requests for the message thread, in this order, while this thread does not pump: an
    // app.quit, a second app.quit (with the consent) and a write. The first decides to quit and enters the stopping
    // state before either of the others starts, so neither runs: no second request to close the window and no change of
    // the tempo. (A request that is answered at all is answered with SHUTTING_DOWN, executed false; a connection may
    // also be closed by the stop first.)
    const auto send = [&](ApiSession& session, const char* line, std::optional<std::string>& reply,
                          std::atomic<bool>& done) {
        return std::thread([&session, line, &reply, &done] {
            if (session.client().sendLine(line))
                reply = session.client().readLine(kEndsSessionWaitMs);
            done = true;
        });
    };
    std::optional<std::string> first, secondReply, thirdReply;
    std::atomic<bool> firstDone{false}, secondDone{false}, thirdDone{false};
    auto a = send(f.api, R"({"jsonrpc":"2.0","id":1,"method":"app.quit","params":{}})", first, firstDone);
    CONTROL_CHECK(waitUntil([&messageThread] { return messageThread->waitingCalls() == 1; }, kWaiterSettleMs));
    sleepMs(100);
    auto b = send(second, R"({"jsonrpc":"2.0","id":2,"method":"app.quit","params":{"discardUnsaved":true}})",
                  secondReply, secondDone);
    CONTROL_CHECK(waitUntil([&messageThread] { return messageThread->waitingCalls() == 2; }, kWaiterSettleMs));
    sleepMs(100);
    auto c = send(third, R"({"jsonrpc":"2.0","id":3,"method":"transport.setTempo","params":{"bpm":99.5}})", thirdReply,
                  thirdDone);
    CONTROL_CHECK(waitUntil([&messageThread] { return messageThread->waitingCalls() == 3; }, kWaiterSettleMs));
    sleepMs(100);

    const bool finished = pumpMessagesUntil([&] { return firstDone.load() && secondDone.load() && thirdDone.load(); },
                                            kEndsSessionWaitMs + kReplySeenMs);
    a.join();
    b.join();
    c.join();
    CONTROL_CHECK(finished);
    pumpMessagesFor(kQuietPumpMs);

    if (CONTROL_CHECK(first.has_value())) {
        const auto parsed = parseJson(*first);
        CONTROL_CHECK(parsed.ok && parsed.value.find("result") != nullptr);
        checkQuitResult(memberOf(parsed.value, "result"), false, nullptr);
    }
    for (const auto& [name, reply] :
         {std::make_pair("second app.quit", secondReply), std::make_pair("write", thirdReply)}) {
        if (reply) {
            note(std::string(name) + ": " + *reply);
            CONTROL_CHECK(isError(reply, "SHUTTING_DOWN", ""));
            const auto parsed = parseJson(*reply);
            const JsonValue* data = errorData(parsed.value);
            CONTROL_CHECK(data != nullptr && memberOf(*data, "executed").isBoolean()
                          && !memberOf(*data, "executed").asBoolean());
        } else {
            note(std::string(name) + ": the connection closed before an answer");
        }
    }
    // Only the first call ran; the write did not change the tempo; the window was asked to close once.
    CONTROL_CHECK(dispatcher->executedCount() == 1);
    CONTROL_CHECK(f.processor->getBpm() == tempoBefore);
    CONTROL_CHECK(f.bridge.quitRequests == 1);
    // One exit step for the three requests.
    CONTROL_CHECK(afterReply.inOrder());
    CONTROL_CHECK(!service.isRunning() && service.isStopping() && dispatcher->isStopping());
    CONTROL_CHECK(!std::filesystem::exists(discoveryFile));
    CONTROL_CHECK(!ControlSocket::connectToLoopback(port, 500).isValid());
    std::size_t bytes = 0;
    CONTROL_CHECK(f.api.client().waitClosed(kCloseSeenMs, bytes));
    CONTROL_CHECK(second.client().waitClosed(kCloseSeenMs, bytes));
    CONTROL_CHECK(third.client().waitClosed(kCloseSeenMs, bytes));
}

} // namespace

void scenarioAppQuitStopping()
{
    // First, the stopping state when the exit step cannot even be queued, for each of the two reasons: the token is
    // dead (its own instance, gone before the next one) ...
    exitStepNotQueued();
    // ... then three sessions queued behind the quit ...
    requestsQueuedBehindTheQuit();
    // ... and last, the message queue refusing the step while the token is alive: the flag that stopDispatchLoop() sets
    // is never reset, so it stays set for the rest of the process (exitStepRefusedByQueue(), startedProcessor()).
    exitStepRefusedByQueue();
}

// ---- The specification's vectors -----------------------------------------------------------------------

namespace {

/** Why the runner does not run a vector of app/, or empty when it does. */
std::string quitVectorSkip(const JsonValue& vector)
{
    if (memberOf(memberOf(vector, "request"), "method").text() != "app.quit")
        return "runs in ControlProcessorTests.Vectors";
    if (memberOf(vector, "type").text() != "request" || memberOf(vector, "session").text() != "authenticated")
        return "only authenticated request vectors are run here";
    const auto& given = memberOf(vector, "given");
    for (std::size_t i = 0; i < given.size(); ++i) {
        const auto& name = given.keyAt(i);
        if (name != "dirty" && name != "dialogs" && name != "jobs" && name != "files" && name != "projectFile"
            && name != "withoutCapabilities")
            return "precondition given." + name + " is not established by this runner";
    }
    return {};
}

/** Runs one vector of app.quit against a fresh processor with the test bridge. Empty on success. Establishes `given`
    as vectors/README.md says, and for a vector with endsSession checks that the instance has ended. */
std::string runQuitVector(const JsonValue& vector, const std::string& slug)
{
    const auto& given = memberOf(vector, "given");
    std::vector<std::string> withheld;
    const auto& without = memberOf(given, "withoutCapabilities");
    for (std::size_t i = 0; i < without.size(); ++i)
        withheld.push_back(without.at(i).text());
    const bool endsSession = memberOf(vector, "endsSession").isBoolean() && memberOf(vector, "endsSession").asBoolean();
    // README, "Vectors that end the instance", rule 1: a request vector ends the session exactly when it closes.
    if (memberOf(vector, "type").text() == "request"
        && endsSession != (memberOf(vector, "connectionAfter").text() == "closed"))
        return "endsSession and connectionAfter disagree";

    QuitFixture f(withheld);
    if (!f.service().isRunning())
        return "the control API did not start";
    VectorMatcher matcher;
    f.bridge.project.dirty = memberOf(given, "dirty").isBoolean() && memberOf(given, "dirty").asBoolean();
    const auto& dialogs = memberOf(given, "dialogs");
    for (std::size_t i = 0; i < dialogs.size(); ++i) {
        ControlUiBridge::DialogInfo dialog;
        dialog.kind = memberOf(dialogs.at(i), "kind").text();
        f.bridge.dialogs.push_back(dialog);
        f.bridge.busy.modalOpen = true;
    }
    const auto& files = memberOf(given, "files");
    const auto directory = isolatedDataDirectory() / "quit-vectors" / std::filesystem::u8path(slug);
    if (files.size() != 0) {
        if (isolatedDataDirectory().empty())
            return "OPENTUNE_DATA_DIR is not set";
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::filesystem::create_directories(directory, ignored);
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files.at(i);
        const auto bind = memberOf(file, "bind").text();
        if (memberOf(file, "kind").text() != "existing")
            return "a given file of kind " + memberOf(file, "kind").text() + " is not established by this runner";
        const auto extension = file.find("extension") != nullptr ? memberOf(file, "extension").text() : ".wav";
        const auto path = directory / std::filesystem::u8path(bind + extension);
        std::ofstream(path, std::ios::binary) << "an existing file";
        matcher.bindings[bind] = JsonValue::string(path.u8string());
    }
    if (memberOf(given, "projectFile").isString()) {
        const auto bound = matcher.bindings.find(memberOf(given, "projectFile").text());
        if (bound == matcher.bindings.end())
            return "projectFile names a file that was not given";
        f.bridge.project.projectPath = bound->second.text();
    }
    std::vector<std::shared_ptr<HeldJob>> jobs;
    bool lockHeld = false;
    const auto& jobSpecs = memberOf(given, "jobs");
    for (std::size_t i = 0; i < jobSpecs.size(); ++i) {
        const auto& spec = jobSpecs.at(i);
        if (memberOf(spec, "state").text() != "running")
            return "a given job in the state " + memberOf(spec, "state").text() + " is not established by this runner";
        const auto job = startHeldJob(f, memberOf(spec, "kind").text());
        if (job == nullptr)
            return "a given job could not be started";
        matcher.bindings[memberOf(spec, "bind").text()] = JsonValue::string(job->id);
        jobs.push_back(job);
        // README: a running export job holds the export lock.
        if (memberOf(spec, "kind").text() == "export" && !lockHeld)
            lockHeld = f.processor->tryAcquireExportLock();
    }

    if (!f.api.open(f.service()))
        return "session.hello failed";
    const auto& discovery = f.api.discovery();
    for (std::size_t i = 0; i < discovery.size(); ++i)
        matcher.bindings["discovery." + discovery.keyAt(i)] = discovery.at(i);
    const int port = f.service().port();
    const auto discoveryFile = std::filesystem::u8path(f.service().discoveryFilePath());

    std::string failure;
    runWhilePumping(
        [&] {
            auto& connection = f.api.client();
            const auto request = matcher.substitute(memberOf(vector, "request"), failure);
            if (!failure.empty())
                return;
            if (!connection.sendLine(toJsonString(request))) {
                failure = "cannot send the request";
                return;
            }
            failure = matcher.expectLine(connection, memberOf(vector, "response"));
            if (!failure.empty())
                return;
            if (endsSession) {
                // The server closes the connection once it has answered; the wait is 15 s, not 5.
                std::size_t bytes = 0;
                if (!connection.waitClosed(kEndsSessionWaitMs, bytes))
                    failure = "the server did not close the connection within the wait";
                else if (bytes != 0)
                    failure = "the server sent bytes after the response";
                return;
            }
            if (memberOf(vector, "connectionAfter").text() == "open") {
                if (!connection.staysQuiet(kOpenCheckMs)) {
                    failure = "the connection closed or sent more";
                    return;
                }
                // The runner's probe: the instance still runs and answers.
                if (!connection.sendLine(R"({"jsonrpc":"2.0","id":2,"method":"session.info","params":{}})")) {
                    failure = "cannot send the probe";
                    return;
                }
                const auto probe = connection.readLine();
                const auto parsed = probe ? parseJson(*probe) : JsonParseResult{};
                if (!probe || !parsed.ok || parsed.value.find("result") == nullptr)
                    failure = "the instance does not answer the probe";
            }
        },
        kVectorExchangeMs);
    if (failure.empty() && endsSession) {
        // The close the client saw came from the stop in the exit step; its other effects follow in that message.
        if (!pumpMessagesUntil([&f] { return f.bridge.quitRequests > 0; }, kEndsSessionWaitMs))
            failure = "the window was not asked to close";
        else if (ControlSocket::connectToLoopback(port, 500).isValid())
            failure = "a connection was accepted after the instance ended";
        else if (std::filesystem::exists(discoveryFile))
            failure = "the discovery file is still there";
        else if (f.bridge.quitRequests != 1)
            failure = "the window was asked to close more than once";
    } else if (failure.empty() && f.bridge.quitRequests != 0) {
        failure = "the window was asked to close by a call that did not quit";
    }
    for (const auto& job : jobs)
        job->release = true;
    if (lockHeld)
        f.processor->releaseExportLock();
    return failure;
}

/** The vectors of app.quit, all of which must run and pass headless. */
const char* const kMustRunQuitVectors[] = {
    "app/quit-busy",       "app/quit-busy-export",   "app/quit-busy-modal",    "app/quit-capability-missing",
    "app/quit-clean",      "app/quit-dirty-discard", "app/quit-dirty-refused", "app/quit-invalid",
    "app/quit-saved-path",
};

} // namespace

void scenarioAppQuitVectors()
{
    const auto root = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors" / "app";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.path().extension() == ".json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    int passed = 0;
    int skipped = 0;
    std::set<std::string> passedIds;
    VectorReport report("ControlProcessorTests.AppQuitVectors");
    for (const auto& file : files) {
        const auto parsed = parseJson(readFile(file));
        if (!CONTROL_CHECK(parsed.ok))
            continue;
        const auto& vector = parsed.value;
        const auto id = memberOf(vector, "id").text();
        if (const auto reason = quitVectorSkip(vector); !reason.empty()) {
            ++skipped;
            report.skip(id, reason);
            note("SKIP " + id + ": " + reason);
            continue;
        }
        const auto started = Clock::now();
        const auto failure = runQuitVector(vector, file.stem().u8string());
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty())) {
            ++passed;
            passedIds.insert(id);
        }
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    for (const char* id : kMustRunQuitVectors)
        CONTROL_CHECK(passedIds.count(id) == 1);
    note(std::to_string(passed) + " vectors passed, " + std::to_string(skipped) + " skipped (the exit status of the "
         + "process is observed by the run against the real executable)");
}

} // namespace ControlTest
} // namespace OpenTune
