// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlServer.h"
#include "Ops/OpsRegistry.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace OpenTune {

class ControlJobs;
class ControlMessageThread;
class ControlUiBridge;
class OpenTuneAudioProcessor;

/**
 * The single entry point of every method on the C++ side, and the request handler of the listener
 * (ControlServerConfig::handler). For each request it:
 * 1. answers SHUTTING_DOWN once serverStopping() began (a request can pass the listener's own stop check
 *    just before the flag is set and reach the handler afterwards);
 * 2. finds the method in the method table (Ops/OpsRegistry.cpp); a defined method this build does not
 *    implement gets UNSUPPORTED(capability_missing);
 * 3. checks params on the network thread (shape and ranges only, never OpenTune state):
 *    INVALID_ARGUMENT(invalid_params);
 * 4. submits the method asynchronously to the message thread, with the dispatch timeout of the limits.
 *    A request that has not started within it is abandoned and answered with TIMEOUT(dispatch_timeout),
 *    executed false, and never runs later. Started work reports its actual result; stop() resolves work
 *    that has not started with SHUTTING_DOWN.
 *    A method whose schema says x-otcp.mainThread false (ControlMethod::runOffMainThread, app.getLog)
 *    runs on the connection thread instead, with the immutable facts snapshot only: it never waits for
 *    the message thread, so it never answers TIMEOUT (PROTOCOL.md section 13.3 rule 2);
 * 5. maps the method's Result to the response: the result object, or the error that controlMapError
 *    gives (the only place where OpenTune errors become protocol errors). `executed` is what the
 *    operation stated (controlOpError or controlOpErrorAfterChange); for any other failure of a write
 *    it is true, because the write may have changed state;
 * 6. for a method that ends the instance (app.quit: ControlOpContext::endInstanceAfterReply), only after the
 *    result was written to the connection, posts the method's exit step to the message thread. The dispatcher is
 *    in the stopping state from the moment the method returned its result, so that every other request is answered
 *    with SHUTTING_DOWN; the exit step stops the control service (connections close, the discovery file goes) and
 *    quits the application.
 *
 * Methods whose schema says mainThread false and that have ControlMethod::runWithoutMessageThread
 * (job.get, job.cancel) skip step 4 as well: they run on the connection thread with the jobs registry
 * only, and job.get with waitMs answers later from the registry's own thread, so the connection keeps
 * serving the session's other requests meanwhile.
 *
 * Every access to OpenTune state goes through ControlOpContext, which refuses any thread other than the
 * message thread, always (not only in debug builds). Methods that run off the message thread get no
 * ControlOpContext at all.
 *
 * Main-thread requests leave the connection reader available after submission. Submissions from one
 * session reach the message thread in arrival order (PROTOCOL.md section 3.3 rule 6); different sessions
 * do not wait for each other.
 *
 * Threading: handleRequest() on connection threads, concurrently; serverStopping() and setInstanceId() on
 * the thread that starts and stops the listener (the message thread in OpenTune); the accessors from any
 * thread.
 */
class ControlDispatcher final : public ControlRequestHandler
{
public:
    /** What the dispatcher works with. */
    struct Setup
    {
        /** Initialised (ensureRuntimeInitialized()) before the listener starts; reached only on the
            message thread, while the liveness token of messageThread is alive. */
        OpenTuneAudioProcessor* processor = nullptr;
        std::shared_ptr<ControlMessageThread> messageThread;
        /** The limits of the listener; session.info reports them, dispatchTimeoutMs bounds the wait. */
        ControlLimits limits;
        /** The build identity of the listener, for session.info. */
        ControlBuildIdentity build;
        /** The editor's bridge, or nullptr; called on the message thread for each request. */
        std::function<ControlUiBridge*()> uiBridge;
        /** The jobs of the instance (job.get, job.cancel and the methods that start jobs); stopped by
            ControlService::stop() before the listener. */
        std::shared_ptr<ControlJobs> jobs;
        /** What the instance knows about OpenTune's undo history (edit.*, and every write that records an
            undo entry); message thread only. */
        std::shared_ptr<ControlHistory> history;
    };

    explicit ControlDispatcher(Setup setup);

    /** session.v1 and the capabilities of the registered methods. */
    std::vector<std::string> capabilities() const override;
    void handleRequest(ControlRequest request, ControlCompletion completion) override;
    /** From now on every request is answered with SHUTTING_DOWN, also one that is waiting for the
        message thread and has not started yet. */
    void serverStopping() override;

    /** The instance id the listener generated, also for the job ids; set after ControlServer::start(),
        before the message loop runs any request. Message thread. */
    void setInstanceId(std::string instanceId);

    /** The session token and the server proof, for redacting app.getLog; set once after
        ControlServer::start(), like the instance id. Message thread. Methods that run off the message
        thread wait for it (bounded by the dispatch timeout): a request can reach the handler between
        ControlServer::start() and this call. */
    void setRedactionSecrets(std::vector<std::string> secrets);

private:
    struct Outcome;

    void execute(const ControlMethod& method, const void* parsed, Outcome& outcome);
    void executeOffMainThread(const ControlMethod& method, const void* parsed, Outcome& outcome);
    static void respond(const Outcome& outcome, const ControlCompletion& completion);
    /** Posts the exit step of a method that ended the instance, after respond() returned. Non-message thread. */
    void runAfterReply(Outcome& outcome);
    /** Sets the stopping flag and wakes the waiters of the facts; idempotent, any thread. */
    void enterStopping();
    std::shared_ptr<const ControlInstanceFacts> currentFacts() const;

    Setup setup_;
    ControlMethodTable methods_;
    std::vector<std::string> capabilities_;
    mutable std::mutex factsMutex_;
    /** Set by setRedactionSecrets() and serverStopping(); methods off the message thread wait on it. */
    std::condition_variable factsChanged_;
    /** Replaced, never changed in place: a reader keeps the copy it got. Guarded by factsMutex_. */
    std::shared_ptr<const ControlInstanceFacts> facts_;
    bool redactionSecretsSet_ = false;
    std::atomic<bool> stopping_{false};
};

} // namespace OpenTune
