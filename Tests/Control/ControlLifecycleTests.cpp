// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Control/ControlJobs.h"
#include "Control/ControlServer.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>

using namespace OpenTune;
using namespace std::chrono_literals;

namespace {

void require(bool ok, const char* detail)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", detail);
        std::abort();
    }
}

template <typename Predicate>
void pumpUntil(Predicate done)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!done() && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    require(done(), "message-thread operation completed");
}

struct WorkLifetime
{
    explicit WorkLifetime(int& destroyedIn) : destroyed(destroyedIn) {}
    ~WorkLifetime()
    {
        require(juce::MessageManager::existsAndIsCurrentThread(), "work destroyed on message thread");
        ++destroyed;
    }
    int& destroyed;
};

void testReplyDestruction()
{
    ControlMessageThread dispatch;
    const auto handle = dispatch.handle();
    for (const bool cancel : {false, true}) {
        int destroyed = 0;
        int replies = 0;
        auto jobs = std::make_unique<ControlJobs>(ControlLimits{}, handle);
        const auto id = jobs->start("test", true,
            [work = std::make_shared<WorkLifetime>(destroyed)](ControlJobStep&) {}).jobId;
        if (cancel) {
            jobs->cancelAsync(id, [&](ControlJobs::Cancel result, JsonValue job) {
                require(result == ControlJobs::Cancel::Requested && job.isObject(), "cancel snapshot");
                ++replies;
                jobs.reset();
            });
        } else {
            jobs->waitAsync(id, 0, [&](ControlJobs::Lookup result, JsonValue job) {
                require(result == ControlJobs::Lookup::Found && job.isObject(), "get snapshot");
                ++replies;
                jobs.reset();
            });
        }
        require(!jobs && replies == 1 && destroyed == 1, "immediate responder destroys owner exactly once");
    }

    // The first terminal responder destroys the owner; the rest consume independent snapshots.
    int destroyed = 0;
    int replies = 0;
    auto jobs = std::make_unique<ControlJobs>(ControlLimits{}, handle);
    const auto id = jobs->start("test", true,
        [work = std::make_shared<WorkLifetime>(destroyed)](ControlJobStep& step) {
            step.succeed(JsonValue::object());
        }).jobId;
    for (int i = 0; i != 2; ++i)
        jobs->waitAsync(id, 1000, [&](ControlJobs::Lookup result, JsonValue job) {
            require(result == ControlJobs::Lookup::Found && job.find("state")->text() == "succeeded",
                    "terminal snapshot survives owner destruction");
            require(destroyed == 1, "work released before terminal response");
            ++replies;
            jobs.reset();
        });
    pumpUntil([&] { return replies == 2; });
    require(!jobs && destroyed == 1, "terminal responder destroys owner after tick cleanup");

    // Deadline delivery can delete the owner while there is still active work to wake.
    destroyed = 0;
    replies = 0;
    jobs = std::make_unique<ControlJobs>(ControlLimits{}, handle);
    const auto pending = jobs->start("deadline", true,
        [work = std::make_shared<WorkLifetime>(destroyed)](ControlJobStep& step) {
            step.wakeAt(std::chrono::steady_clock::now() + 1s);
        }).jobId;
    jobs->waitAsync(pending, 10, [&](ControlJobs::Lookup result, JsonValue) {
        require(result == ControlJobs::Lookup::Found, "wait deadline returns current state");
        ++replies;
        jobs.reset();
    });
    pumpUntil([&] { return replies == 1; });
    require(!jobs && destroyed == 1, "deadline responder destroys owner");
}

void testNestedStop()
{
    ControlMessageThread dispatch;
    auto jobs = std::make_unique<ControlJobs>(ControlLimits{}, dispatch.handle());
    int destroyed = 0;
    int replies = 0;
    int steps = 0;
    bool returnedFromNestedLoop = false;
    const auto id = jobs->start("nested", true,
        [&, work = std::make_shared<WorkLifetime>(destroyed)](ControlJobStep&) {
            ++steps;
            juce::MessageManager::callAsync([&] {
                dispatch.close();
                jobs->stop();
                dispatch.stop();
                require(replies == 0 && destroyed == 0, "nested stop defers response and active work release");
            });
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
            returnedFromNestedLoop = true;
        }).jobId;
    jobs->start("next", true, [&](ControlJobStep&) { ++steps; });
    jobs->waitAsync(id, 1000, [&](ControlJobs::Lookup result, JsonValue) {
        require(result == ControlJobs::Lookup::Stopped, "stop settles waiter");
        require(returnedFromNestedLoop && destroyed == 1, "outer step unwound before response");
        ++replies;
        jobs.reset();
    });
    pumpUntil([&] { return replies == 1; });
    require(steps == 1 && !jobs, "stop prevents later steps and permits responder destruction");
}

void testDeadlineArbitration()
{
    ControlMessageThread dispatch;
    const auto handle = dispatch.handle();
    std::promise<void> workerEntered;
    std::promise<void> releaseWorker;
    auto release = releaseWorker.get_future().share();
    int ran = 0;
    std::atomic<int> replies{0};
    handle.submit([&] { ++ran; }, 0, [&](ControlMessageThread::CallResult result) {
        require(result == ControlMessageThread::CallResult::TimedOut, "worker timeout");
        ++replies;
        workerEntered.set_value();
        release.wait();
    });
    require(workerEntered.get_future().wait_for(2s) == std::future_status::ready, "deadline worker entered");

    // Deliberately keep the deadline worker inside an earlier completion. The message thread itself
    // must decide that the next zero-deadline request cannot acquire execution rights.
    handle.submit([&] { ++ran; }, 0, [&](ControlMessageThread::CallResult result) {
        require(result == ControlMessageThread::CallResult::TimedOut, "message thread rejects expired request");
        require(juce::MessageManager::existsAndIsCurrentThread(), "delayed worker did not arbitrate this request");
        ++replies;
    });
    pumpUntil([&] { return replies == 2; });
    require(ran == 0 && handle.waitingCalls() == 0, "expired requests never execute");
    releaseWorker.set_value();

    handle.submit([&] {
        ++ran;
        dispatch.stop();
    }, 1000, [&](ControlMessageThread::CallResult result) {
        require(result == ControlMessageThread::CallResult::Done, "running request reports actual result after stop");
        ++replies;
    });
    pumpUntil([&] { return replies == 3; });
    require(ran == 1 && handle.waitingCalls() == 0, "each submission has one terminal delivery");
}

void testSubmissionDuringStop()
{
    ControlMessageThread dispatch;
    const auto handle = dispatch.handle();
    std::array<std::atomic<int>, 128> replies{};
    std::promise<void> firstPosted;
    std::thread connection([&] {
        for (std::size_t i = 0; i != replies.size(); ++i) {
            handle.submit([] { require(false, "queued submission cannot run after close"); }, 10000,
                          [&, i](ControlMessageThread::CallResult result) {
                require(result == ControlMessageThread::CallResult::Stopped, "stopping submission result");
                ++replies[i];
            });
            if (i == 0)
                firstPosted.set_value();
        }
    });
    require(firstPosted.get_future().wait_for(2s) == std::future_status::ready, "first submission registered");
    dispatch.stop();
    connection.join();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    for (auto& count : replies)
        require(count == 1, "publication and close settle every request exactly once");
    require(handle.waitingCalls() == 0, "no requests retained after close");
}

void testStopReplyDestruction()
{
    for (const bool posted : {false, true}) {
        auto dispatch = std::make_unique<ControlMessageThread>();
        const auto handle = dispatch->handle();
        int replies = 0;
        const auto stopped = [&] {
            ++replies;
            dispatch.reset();
        };
        if (posted)
            handle.post([] { require(false, "stopped post cannot run"); }, stopped);
        else
            handle.submit([] { require(false, "stopped submission cannot run"); }, 10000,
                          [stopped](ControlMessageThread::CallResult result) {
                require(result == ControlMessageThread::CallResult::Stopped, "owner destruction follows stop result");
                stopped();
            });
        dispatch->stop();
        require(!dispatch && replies == 1 && !handle.isAlive(), "stop responder destroys dispatch owner");
        require(!handle.post([] {}, [&] { ++replies; }) && replies == 2, "handle rejects after owner destruction");
    }
}

class DelayedConnection final : public ControlRequestHandler
{
public:
    explicit DelayedConnection(ControlMessageThread::Handle handleIn) : handle(std::move(handleIn)) {}
    std::vector<std::string> capabilities() const override { return {"session.v1"}; }
    void handleRequest(ControlRequest, ControlCompletion completion) override
    {
        entered.set_value();
        release.get_future().wait();
        handle.post([] { require(false, "closed handle cannot access destroyed registry"); }, [completion] {
            completion.respondError("SHUTTING_DOWN", {}, "stopped", false);
        });
        exited.set_value();
    }
    ControlMessageThread::Handle handle;
    std::promise<void> entered, release, exited;
};

struct ServerFixture
{
    explicit ServerFixture(std::shared_ptr<ControlRequestHandler> handler)
        : directory(juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getNonexistentChildFile("opentune-control-lifecycle", "", false))
    {
        ControlServerConfig config;
        config.discoveryDirectory = directory.getFullPathName().toStdString();
        config.handler = std::move(handler);
        config.build.appVersion = "test";
        config.build.commit = std::string(40, '0');
        config.build.sourceUrl = "https://example.com/opentune";
        config.limits.maxLineBytes = 32 * 1024 * 1024;
        server = std::make_unique<ControlServer>(std::move(config));
        std::string error;
        require(server->start(error), error.c_str());
        require(client.connect("127.0.0.1", server->port()), "connect to production server");
        const auto hello = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session.hello\",\"params\":{"
                           "\"protocol\":\"1.0\",\"client\":{\"name\":\"lifecycle-test\",\"version\":\"1\"},"
                           "\"token\":\"" + server->secrets().front() + "\"}}\n";
        send(hello);
        require(readLine().find("\"result\"") != std::string::npos, "authenticate production session");
    }
    ~ServerFixture()
    {
        client.close();
        server.reset();
        directory.deleteRecursively();
    }
    void send(const std::string& line)
    {
        require(client.write(line.data(), static_cast<int>(line.size())) == static_cast<int>(line.size()),
                "write client request");
    }
    std::string readLine()
    {
        std::string line;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        for (;;) {
            const auto end = received.find('\n');
            if (end != std::string::npos) {
                line = received.substr(0, end + 1);
                received.erase(0, end + 1);
                return line;
            }
            require(std::chrono::steady_clock::now() < deadline, "complete reply received within bound");
            if (client.waitUntilReady(true, 100) <= 0)
                continue;
            char bytes[65536];
            const auto count = client.read(bytes, sizeof(bytes), false);
            require(count > 0, "connection remains open through reply LF");
            received.append(bytes, static_cast<std::size_t>(count));
        }
    }
    juce::File directory;
    std::unique_ptr<ControlServer> server;
    juce::StreamingSocket client;
    std::string received;
};

void testDelayedConnectionRelease()
{
    static_assert(!std::is_copy_constructible_v<ControlMessageThread>);
    auto dispatch = std::make_unique<ControlMessageThread>();
    int destroyed = 0;
    auto jobs = std::make_unique<ControlJobs>(ControlLimits{}, dispatch->handle());
    jobs->start("owned", true, [work = std::make_shared<WorkLifetime>(destroyed)](ControlJobStep&) {});
    auto connection = std::make_shared<DelayedConnection>(dispatch->handle());
    ServerFixture fixture(connection);
    fixture.send("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"session.info\",\"params\":{}}\n");
    require(connection->entered.get_future().wait_for(2s) == std::future_status::ready, "connection held in handler");
    dispatch->close();
    jobs->stop();
    dispatch->stop();
    fixture.server.reset(); // Real bounded join leaves this connection thread finishing later.
    jobs.reset();
    dispatch.reset();
    require(destroyed == 1, "message-thread owners released while connection is delayed");
    auto exited = connection->exited.get_future();
    connection->release.set_value();
    require(exited.wait_for(2s) == std::future_status::ready, "late connection releases only submission/reply state");
}

class FinalReplyHandler final : public ControlRequestHandler
{
public:
    std::vector<std::string> capabilities() const override { return {"session.v1", "app.quit"}; }
    void handleRequest(ControlRequest request, ControlCompletion completion) override
    {
        (request.method == "app.quit" ? accepted : otherAccepted).set_value(std::move(completion));
    }
    std::promise<ControlCompletion> accepted, otherAccepted;
};

void testFinalReply(int mode)
{
    auto handler = std::make_shared<FinalReplyHandler>();
    ServerFixture fixture(handler);
    fixture.send("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"app.quit\",\"params\":{}}\n");
    auto accepted = handler->accepted.get_future();
    require(accepted.wait_for(2s) == std::future_status::ready, "final request accepted");
    auto completion = accepted.get();
    fixture.send("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"session.info\",\"params\":{}}\n");
    auto otherAccepted = handler->otherAccepted.get_future();
    require(otherAccepted.wait_for(2s) == std::future_status::ready, "second request accepted on same channel");
    auto other = otherAccepted.get();
    auto result = JsonValue::object();
    // A real payload larger than the kernel send buffer makes partial writes observable.
    const bool large = mode != 0 && mode != 4;
    result.appendMember("payload", JsonValue::string(std::string(large ? 16 * 1024 * 1024 : 32, 'x')));
    std::promise<ControlCompletion::SendResult> sent;
    auto terminal = sent.get_future();
    std::atomic<int> notifications{0};
    if (mode == 4) {
        fixture.client.close();
        fixture.server->stop();
    }
    completion.respondFinalResult(result, [&](ControlCompletion::SendResult state) {
        ++notifications;
        require(!completion.isPending(), "callback sees settled completion");
        other.respondResult(JsonValue::object()); // Re-enters the same channel mutex.
        sent.set_value(state);
    });
    completion.respondFinalResult(result, [&](ControlCompletion::SendResult) {
        require(false, "duplicate completion cannot deliver a second send event");
    });
    if (large)
        require(terminal.wait_for(0ms) == std::future_status::timeout, "buffered response has no send completion yet");
    if (mode == 0 || mode == 1) {
        const auto line = fixture.readLine();
        require(line.back() == '\n' && parseJson(line).ok, "entire final response including LF reached peer");
    } else if (mode == 2) {
        fixture.client.close();
    }
    require(terminal.wait_for(3s) == std::future_status::ready, "send has a bounded terminal event");
    const auto state = terminal.get();
    require((mode <= 1) == (state == ControlCompletion::SendResult::Written), "send terminal matches real socket outcome");
    require(notifications == 1, "send terminal delivered once");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testReplyDestruction();
    testNestedStop();
    testDeadlineArbitration();
    testSubmissionDuringStop();
    testStopReplyDestruction();
    testDelayedConnectionRelease();
    for (int mode = 0; mode != 5; ++mode)
        testFinalReply(mode); // complete, partial then read, disconnect, no read, closed before response
    std::puts("PASS: control ownership, reentrant replies, nested stop, deadlines and socket send completion");
    return 0;
}
