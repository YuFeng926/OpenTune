// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>

namespace OpenTune {

/**
 * Message-thread marshalling of the control API, guarded by a liveness token.
 *
 * OpenTune changes content only on the message thread. The control API reaches the message thread
 * only through this class: post() queues a function with juce::MessageManager::callAsync, call() does
 * the same from another thread and waits for it. Every queued function captures the liveness token of
 * this object (a flag in state shared with the queued functions, not the object itself) and runs only
 * while the token is alive. stop() kills the token: functions still in the queue then return without
 * doing anything when the message loop reaches them, even after this object and its owner are gone.
 *
 * Why the check is enough: queued functions run on the message thread, and stop() and the destruction
 * of the owner happen on the message thread too, so a function that found the token alive keeps its
 * owner alive until it returns (unless the function itself destroys the owner).
 *
 * A thread waiting in call() waits on this object's own mutex and condition variable. It must not hold
 * any lock or lease of the processor while it waits (in particular no
 * ProcessRenderRuntime::CompletionCallbackLease): the message thread may need them to make progress.
 * stop() wakes every waiting call().
 *
 * Threading: post(), call(), isStopped(), isAlive() and waitingCalls() from any thread; stop() and destruction on
 * the message thread.
 */
class ControlMessageThread
{
public:
    /** How call() ended. */
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
            application quits), or call() was used on the message thread itself. */
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

    /**
     * From a thread other than the message thread: runs fn on the message thread and waits until it
     * returned, timeoutMs passed or stop() was called.
     *
     * TimedOut and Stopped guarantee that fn has not run and never will run: the call is abandoned
     * under the same mutex that fn's start takes. If fn has already started when the timeout passes or
     * stop() is called, call() waits until it returns and reports Done or Failed, so that a caller
     * never reports "not executed" for work that was executed.
     *
     * fn must not stop the control service synchronously (for example an operation that closes the
     * application or destroys the processor): ControlService::stop() joins the connection thread that
     * waits here for fn, so the join would only end at its bounded timeout and detach the thread. Such
     * an operation posts the stop with post() from fn and returns.
     */
    CallResult call(std::function<void()> fn, int timeoutMs);

    /** Kills the token (queued functions become no-ops) and wakes every waiting call(), which then
        returns Stopped unless its function already started. Message thread; idempotent. */
    void stop();

    /** True after stop(). */
    bool isStopped() const;

    /** True until stop(): the liveness token is alive. Read-only, the complement of isStopped(). A failed post() tells
        the two reasons apart with it: false means the service has been stopped, so nothing is left to clean up; true
        means the message queue refused the message (juce::MessageManager::callAsync returned false, for example while
        the application quits) and the service is still up. */
    bool isAlive() const;

    /** Number of call()s currently waiting; for diagnostics and tests. */
    int waitingCalls() const;

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

} // namespace OpenTune
