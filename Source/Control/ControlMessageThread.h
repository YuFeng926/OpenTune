// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>

namespace OpenTune {

/**
 * Message-thread marshalling of the control API, guarded by a liveness token.
 *
 * OpenTune changes content only on the message thread. The control API reaches the message thread
 * only through this class: post() and submit() queue functions with juce::MessageManager::callAsync.
 * submit() uses one shared deadline and completion worker for asynchronous calls. Every queued function
 * captures the liveness token of this object (a flag in state shared with the queued functions, not the
 * object itself) and runs only while the token is alive. stop() kills the token: functions still in the
 * queue then return without doing anything when the message loop reaches them. The owner is destroyed on
 * the message thread, so a queued function that observes a live token keeps the owner alive until it returns.
 *
 * Why the check is enough: queued functions and owner destruction both run on the message thread, so a function that
 * found the token alive keeps its owner alive until it returns (unless it destroys the owner).
 *
 * Threading: post(), submit(), stop(), isStopped(), isAlive() and waitingCalls() from any thread;
 * destruction on the message thread.
 */
class ControlMessageThread
{
public:
    /** How an asynchronous submission ended. */
    enum class CallResult {
        /** The function ran and returned. */
        Done,
        /** The function ran and threw; the exception was caught on the message thread. */
        Failed,
        /** The timeout passed before the function started. It never runs. */
        TimedOut,
        /** stop() was called before the function started. It never runs. */
        Stopped,
        /** Nothing was queued: no function, the message queue refused it (for example while the
            application quits). */
        NotPosted
    };

    ControlMessageThread();
    /** Calls stop(). */
    ~ControlMessageThread();

    ControlMessageThread(const ControlMessageThread&) = delete;
    ControlMessageThread& operator=(const ControlMessageThread&) = delete;

    /**
     * Queues fn for the message thread. fn runs only if the token is still alive when the message loop
     * reaches it; it must not throw. Returns false, and drops fn, after stop() or when the message
     * queue refuses the message (juce::MessageManager::callAsync returned false).
     */
    bool post(std::function<void()> fn);

    /** Queues fn for the message thread. A queued function that has not started by timeoutMs never runs; a started
        function reports its actual result. Accepted work completes on the shared worker, or on the message thread
        when stop() has ended that worker. Rejected and stopped queued work completes on the calling thread. The timeout
        starts after the message queue accepts the function. */
    void submit(std::function<void()> fn, int timeoutMs, std::function<void(CallResult)> completion);

    /** Kills the token (queued functions become no-ops) and completes queued submissions with Stopped. Idempotent. */
    void stop();

    /** True after stop(). */
    bool isStopped() const;

    /** True until stop(): the liveness token is alive. Read-only, the complement of isStopped(). A failed post() tells
        the two reasons apart with it: false means the service has been stopped, so nothing is left to clean up; true
        means the message queue refused the message (juce::MessageManager::callAsync returned false, for example while
        the application quits) and the service is still up. */
    bool isAlive() const;

    /** Number of asynchronous submissions awaiting a terminal result. */
    int waitingCalls() const;

private:
    struct Shared;
    static void timeoutLoop(std::shared_ptr<Shared> shared);
    std::shared_ptr<Shared> shared_;
};

} // namespace OpenTune
