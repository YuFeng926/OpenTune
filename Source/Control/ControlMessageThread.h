// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace OpenTune {

/** Message-thread owner of the dispatch deadline worker. Connections retain only a Handle.
    Queued operations use the shared liveness state; their business owners belong to the message
    thread and remain alive until an executing operation returns. Completions retain result snapshots
    and connection state only. Construction, handle(), close(), stop() and destruction: message thread. */
class ControlMessageThread
{
    struct Shared;

public:
    enum class CallResult { Done, Failed, TimedOut, Stopped, NotPosted };

    /** Copyable submission state, safe to retain and release on any thread after the owner stops. */
    class Handle
    {
    public:
        Handle() = default;
        /** Queues a message-thread operation. onStopped runs exactly once if it cannot start,
            on the rejecting/stopping thread; it must only use independent reply state. */
        bool post(std::function<void()> fn, std::function<void()> onStopped = {}) const;
        /** Fixes a deadline at submission. Queued work times out; started work reports its actual
            result. Completion runs on the deadline worker, message thread or rejecting thread. */
        void submit(std::function<void()> fn, int timeoutMs, std::function<void(CallResult)> completion) const;
        bool isAlive() const;
        int waitingCalls() const;

    private:
        friend class ControlMessageThread;
        explicit Handle(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}
        std::shared_ptr<Shared> shared_;
    };

    ControlMessageThread();
    ~ControlMessageThread();
    ControlMessageThread(const ControlMessageThread&) = delete;
    ControlMessageThread& operator=(const ControlMessageThread&) = delete;

    /** Starts the worker on first use and returns the shared submission handle. */
    Handle handle();
    /** Closes submission and settles queued requests. Idempotent; running operations finish normally. */
    void close();
    /** Closes submission and joins the worker. Idempotent. */
    void stop();

private:
    static void timeoutLoop(std::shared_ptr<Shared> shared);
    std::shared_ptr<Shared> shared_;
    std::thread deadlineThread_;
};

} // namespace OpenTune
