// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlDispatcher.h"

#include "ContentHandles.h"
#include "ControlJobs.h"
#include "ControlLogTail.h"
#include "ControlMessageThread.h"

#include "Utils/AppLogger.h"

#include <juce_events/juce_events.h>

#include <chrono>
#include <optional>
#include <utility>

namespace OpenTune {

/** What execute() produced on the message thread, read by the waiting connection thread after call()
    returned Done. */
struct ControlDispatcher::Outcome
{
    std::optional<JsonValue> result;
    ControlErrorReply error;
    /** `executed` of the error: whether state may have changed. */
    bool executed = false;
    /** The method ended the instance (ControlOpContext::endInstanceAfterReply): what handleRequest() posts to the
        message thread once the result is written. Only with a result. */
    std::function<void()> afterReply;
};

namespace {

void logFailure(const std::string& method, const char* what)
{
    // The method name is one of the registered protocol names; nothing of the request is logged.
    AppLogger::logNoThrow(juce::String("control API: ") + juce::String(method) + " " + what);
}

} // namespace

ControlDispatcher::ControlDispatcher(Setup setup) : setup_(std::move(setup))
{
    jassert(setup_.processor != nullptr && setup_.messageThread != nullptr);
    registerControlMethods(methods_);
    capabilities_ = methods_.capabilities();
    auto facts = std::make_shared<ControlInstanceFacts>();
    facts->limits = setup_.limits;
    facts->build = setup_.build;
    facts->capabilities = capabilities_;
    // juce::File::getSpecialLocation(userHomeDirectory): the user's home directory, taken once here on the
    // message thread, so that app.getLog, which runs on connection threads, only reads a copy.
    facts->homeDirectorySpellings = controlHomeDirectorySpellings(
        juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName().toStdString());
    facts_ = std::move(facts);
}

std::vector<std::string> ControlDispatcher::capabilities() const
{
    return capabilities_;
}

void ControlDispatcher::setInstanceId(std::string instanceId)
{
    if (setup_.jobs != nullptr)
        setup_.jobs->setInstanceShort(controlInstanceShort(instanceId));
    std::lock_guard<std::mutex> lock(factsMutex_);
    auto facts = std::make_shared<ControlInstanceFacts>(*facts_);
    facts->instanceId = std::move(instanceId);
    facts_ = std::move(facts);
}

void ControlDispatcher::setRedactionSecrets(std::vector<std::string> secrets)
{
    {
        std::lock_guard<std::mutex> lock(factsMutex_);
        auto facts = std::make_shared<ControlInstanceFacts>(*facts_);
        facts->redactionSecrets = std::move(secrets);
        facts_ = std::move(facts);
        redactionSecretsSet_ = true;
    }
    factsChanged_.notify_all();
}

std::shared_ptr<const ControlInstanceFacts> ControlDispatcher::currentFacts() const
{
    std::lock_guard<std::mutex> lock(factsMutex_);
    return facts_;
}

void ControlDispatcher::serverStopping()
{
    enterStopping();
}

void ControlDispatcher::enterStopping()
{
    // Requests that reach handleRequest() from now on answer themselves with SHUTTING_DOWN (they may have
    // passed the listener's stop check just before its flag was set). Functions already queued for the
    // message thread check the flag before they start; ControlService::stop() has woken their waiters
    // already by stopping the message-thread marshalling. A method off the message thread that waits for
    // the redaction secrets is woken here.
    {
        std::lock_guard<std::mutex> lock(factsMutex_);
        stopping_ = true;
    }
    factsChanged_.notify_all();
}

void ControlDispatcher::handleRequest(ControlRequest request, ControlCompletion completion)
{
    if (stopping_.load()) {
        completion.respondError("SHUTTING_DOWN", {}, "the control API is stopping", false);
        return;
    }
    const ControlMethod* method = methods_.find(request.method);
    if (method == nullptr) {
        // A defined method of a capability this instance reports but does not implement yet; an
        // instance that does not report the capability never gets here (ControlServer answers first).
        const char* capability = controlMethodCapability(request.method);
        completion.respondError("UNSUPPORTED", "capability_missing",
                                request.method + " (capability " + (capability != nullptr ? capability : "?")
                                    + ") is not implemented by this build",
                                false);
        return;
    }
    // Network thread: the shape of params only.
    std::shared_ptr<const void> parsed;
    std::string why;
    if (!method->parse(request.params, parsed, why)) {
        completion.respondError("INVALID_ARGUMENT", "invalid_params", why, false);
        return;
    }
    if (!method->mainThread && method->runWithoutMessageThread) {
        // job.get and job.cancel: the jobs registry only, never the message thread (PROTOCOL.md section 13.3
        // rule 2). The reply may come later from the registry's thread; it captures only the completion.
        if (setup_.jobs == nullptr) {
            completion.respondError("INTERNAL", {}, "the method is not available", false);
            return;
        }
        method->runWithoutMessageThread(*setup_.jobs, parsed.get(), [completion](Result<JsonValue> result) {
            if (result.ok()) {
                completion.respondResult(result.value());
                return;
            }
            const auto reply = controlMapError(result.error());
            completion.respondError(reply.kind, reply.reason, reply.detail, reply.executed.value_or(false));
        });
        return;
    }

    auto outcome = std::make_shared<Outcome>();
    if (!method->mainThread) {
        // x-otcp.mainThread false without the jobs registry (app.getLog): runs here, on the connection
        // thread, and never waits for the message thread, so a busy main thread never delays it and it
        // never answers TIMEOUT.
        executeOffMainThread(*method, parsed.get(), *outcome);
        respond(*outcome, completion);
        return;
    }
    // `this` and `method` stay valid: call() returns only after the function finished or once it can no
    // longer start (TimedOut, Stopped), and the dispatcher outlives the request it is handling. The
    // waiting thread holds no lock and no lease of the processor.
    const auto callResult = setup_.messageThread->call(
        [this, method, parsed, outcome] { execute(*method, parsed.get(), *outcome); }, setup_.limits.dispatchTimeoutMs);
    switch (callResult) {
        case ControlMessageThread::CallResult::Done:
            respond(*outcome, completion);
            // Only now, with the result written to the connection (or discarded because the peer is gone): a method
            // that ended the instance gets its exit step, which stops the service and quits (PROTOCOL.md section 14.1).
            if (outcome->afterReply)
                notifyAfterReply(AfterReplyEvent::ReplyWritten);
            runAfterReply(*outcome);
            return;
        case ControlMessageThread::CallResult::Failed:
            logFailure(method->name, "failed on the message thread");
            completion.respondError("INTERNAL", {}, "the method failed", method->write);
            return;
        case ControlMessageThread::CallResult::TimedOut:
            // Abandoned before it started: it never runs (ControlMessageThread::call()).
            completion.respondError("TIMEOUT", "dispatch_timeout",
                                    "the request did not start on the main thread within dispatchTimeoutMs", false);
            return;
        case ControlMessageThread::CallResult::Stopped:
        case ControlMessageThread::CallResult::NotPosted:
            // The marshalling stopped, or the message loop accepts no more work (OpenTune quits).
            completion.respondError("SHUTTING_DOWN", {}, "the control API is stopping", false);
            return;
    }
}

void ControlDispatcher::execute(const ControlMethod& method, const void* parsed, Outcome& outcome)
{
    // Always on: only the message thread may run methods (call() guarantees it; this check proves it).
    if (!juce::MessageManager::existsAndIsCurrentThread()) {
        ++probe_.otherThreadAccesses;
        jassertfalse;
        outcome.error = {"INTERNAL", "", "the method was not run on the main thread"};
        return;
    }
    if (stopping_.load()) {
        outcome.error = {"SHUTTING_DOWN", "", "the control API is stopping"};
        return;
    }
    ++executed_;
    const auto facts = currentFacts();
    try {
        ControlOpContext context(*setup_.processor, setup_.uiBridge ? setup_.uiBridge() : nullptr, *facts, probe_,
                                 setup_.jobs.get(), setup_.uiBridge, setup_.history);
        auto result = method.run(context, parsed);
        if (result.ok()) {
            outcome.result = std::move(result).value();
            if (auto action = context.takeEndInstanceAction()) {
                // The method decided to end the instance (app.quit). The stopping state starts here, on the message
                // thread and before any other request can run: a request of another session that was queued behind this
                // one is answered with SHUTTING_DOWN and cannot change what the method has just checked. The result
                // itself is still written (respond() does not look at the flag), and the exit step follows it.
                enterStopping();
                outcome.afterReply = std::move(action);
            }
        } else {
            outcome.error = controlMapError(result.error());
            // As the operation stated it: controlOpError fails before any change, controlOpErrorAfterChange
            // after one. Another error of a write may have changed state.
            outcome.executed = outcome.error.executed.value_or(method.write);
        }
    } catch (const ControlThreadViolation&) {
        outcome.error = {"INTERNAL", "", "OpenTune state was reached from a thread other than the main thread"};
        outcome.executed = false;
    } catch (...) {
        logFailure(method.name, "failed with an exception");
        outcome.error = {"INTERNAL", "", "the method failed"};
        outcome.executed = method.write;
    }
}

void ControlDispatcher::executeOffMainThread(const ControlMethod& method, const void* parsed, Outcome& outcome)
{
    if (!method.runOffMainThread) {
        outcome.error = {"INTERNAL", "", "the method is not available"};
        return;
    }
    std::shared_ptr<const ControlInstanceFacts> facts;
    {
        // ControlService sets the secrets right after ControlServer::start() returned, so a request can
        // only arrive before them by a moment; the bound is a safety net, not a wait anyone should see.
        std::unique_lock<std::mutex> lock(factsMutex_);
        factsChanged_.wait_for(lock, std::chrono::milliseconds(setup_.limits.dispatchTimeoutMs),
                               [this] { return redactionSecretsSet_ || stopping_.load(); });
        if (redactionSecretsSet_)
            facts = facts_;
    }
    if (stopping_.load()) {
        outcome.error = {"SHUTTING_DOWN", "", "the control API is stopping"};
        return;
    }
    if (facts == nullptr) {
        // Without the secrets nothing could be redacted; never answer without them.
        logFailure(method.name, "found the instance facts incomplete");
        outcome.error = {"INTERNAL", "", "the instance is not ready"};
        return;
    }
    ++offMainThread_;
    try {
        auto result = method.runOffMainThread(*facts, parsed);
        if (result.ok()) {
            outcome.result = std::move(result).value();
        } else {
            outcome.error = controlMapError(result.error());
            outcome.executed = false; // reads change nothing
        }
    } catch (...) {
        logFailure(method.name, "failed with an exception");
        outcome.error = {"INTERNAL", "", "the method failed"};
        outcome.executed = method.write;
    }
}

void ControlDispatcher::respond(const Outcome& outcome, const ControlCompletion& completion)
{
    if (outcome.result)
        completion.respondResult(*outcome.result);
    else
        completion.respondError(outcome.error.kind, outcome.error.reason, outcome.error.detail, outcome.executed);
}

void ControlDispatcher::runAfterReply(Outcome& outcome)
{
    if (!outcome.afterReply)
        return;
    // ControlMessageThread::post(): the action runs on the message thread, in the message loop, and never inside the
    // call() that this connection thread has finished (the thread rules of Source/Control/README.md: a method run by
    // call() must not stop the service itself). It does nothing when the service has stopped meanwhile: the
    // application is then ending anyway.
    auto step = [this, action = std::move(outcome.afterReply)] {
        notifyAfterReply(AfterReplyEvent::ExitStepStarted);
        action();
    };
    if (!setup_.messageThread->post(std::move(step))) {
        // The state this leaves behind: the dispatcher is in its stopping state (enterStopping() ran when the method
        // returned) and the client has the result with `quitting: true`, but the exit step never runs. post() fails
        // for one of two reasons, which isAlive() tells apart:
        //  - the liveness token is dead: the service has been stopped on the message thread, which leaves nothing to
        //    clean up (the listener, the connections and the discovery file are gone or going), so the step has
        //    nothing left to do;
        //  - the token is alive and the message queue refused the message, for example while the application quits:
        //    the service, the listener, the open connections and the discovery file stay as they are, until the
        //    application's own shutdown stops the service (the first statement of the processor's destructor); until
        //    it does, every request is answered with SHUTTING_DOWN.
        //
        // isAlive() is read after post() failed, and the token never comes back to life. So an alive token at that
        // point was alive when post() failed, and the first log below ("refused by the message queue") is exact. A dead
        // token is usually the first case, but it can also be the second one: the queue refused the step while the
        // token was alive, and the service stopped before isAlive() was read. The log then says that the service is
        // already stopping. That is still true when it is written (the service is stopping and there is nothing left to
        // stop), so the line is harmless; only the reason that post() failed may have been the queue's refusal.
        //
        // There is no cleanup that this thread can do. ControlService::stop() runs on the message thread, which is the
        // thread that has just refused to take work, and calling it here would make the service join this very
        // connection thread (ControlMessageThread::call() describes the same rule). So the case is only logged, in
        // words that say what the client was told and what is left running.
        if (setup_.messageThread->isAlive())
            AppLogger::logNoThrow(
                "control API: the exit step of a method that ends the instance was refused by the message queue: the "
                "client was told that the instance quits and every request now gets SHUTTING_DOWN, but the listener "
                "and the discovery file stay until the application closes itself and its own shutdown stops the "
                "control service");
        else
            AppLogger::logNoThrow(
                "control API: the exit step of a method that ends the instance was not queued because the control "
                "service is already stopping: the client was told that the instance quits and every request now gets "
                "SHUTTING_DOWN, and there is nothing left to stop");
    }
}

void ControlDispatcher::setAfterReplyObserverForTesting(std::function<void(AfterReplyEvent)> observer)
{
    std::lock_guard<std::mutex> lock(observerMutex_);
    afterReplyObserver_ = std::make_shared<const std::function<void(AfterReplyEvent)>>(std::move(observer));
}

void ControlDispatcher::notifyAfterReply(AfterReplyEvent event)
{
    std::shared_ptr<const std::function<void(AfterReplyEvent)>> observer;
    {
        std::lock_guard<std::mutex> lock(observerMutex_);
        observer = afterReplyObserver_;
    }
    if (observer != nullptr && *observer)
        (*observer)(event);
}

} // namespace OpenTune
