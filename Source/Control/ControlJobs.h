// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlJson.h"
#include "ControlMessageThread.h"
#include "ControlProtocol.h"

#include <juce_events/juce_events.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace OpenTune {

/** The error of a failed or cancelled job, or of an item of a job result:
    the kind, reason and detail as ControlDispatcher would send them, and executed. detail is printable
    ASCII and never contains text taken from files or projects. */
struct ControlJobError
{
    std::string kind;
    std::string reason;
    std::string detail;
    bool executed = true;
};

/** The error object of a job error: the JSON-RPC code and the retryability of the kind. */
JsonValue controlErrorObjectJson(const ControlJobError& error);

class ControlJobs;

/**
 * What the work of a job sees during one of its steps: whether cancellation was requested, and the
 * calls that report progress or end the job. A step ends the job with exactly one of succeed(),
 * fail() or cancel(); the registry then destroys the work.
 *
 * Threading: message thread, valid only during the step it was passed to.
 */
class ControlJobStep
{
public:
    /** True once job.cancel requested cancellation. */
    bool cancelRequested() const;
    /** Ends the job's cancellability, for a job that passed the point after which it can no longer be
        cancelled. Returns false, and changes nothing, when cancellation was already requested. */
    bool endCancellability();
    /** The job's progress; members without a value are null. */
    void setProgress(std::optional<double> fraction, const char* phase, std::optional<int> itemsDone,
                     std::optional<int> itemsTotal);
    /** Ends the job as succeeded with its result (an object). */
    void succeed(JsonValue result);
    /** Ends the job as failed; result is null or the work it committed before it failed. */
    void fail(ControlJobError error, JsonValue result = JsonValue());
    /** Ends the job as cancelled, with the error CANCELLED(cancelled_by_request); result is null or the
        work it committed before it stopped, and changedState (the error's executed) tells whether it
        committed any. */
    void cancel(JsonValue result = JsonValue(), bool changedState = false);
    /** True once one of succeed(), fail() or cancel() was called. */
    bool finished() const noexcept { return finished_; }
    /** Schedules one message-thread step at deadline, unless an event wakes the registry first. */
    void wakeAt(std::chrono::steady_clock::time_point deadline);
    /** A thread-safe wake callback for work completed outside the message thread. */
    std::function<void()> wakeCallback() const;

private:
    friend class ControlJobs;
    ControlJobStep(ControlJobs& jobs, std::uint64_t seq) : jobs_(jobs), seq_(seq) {}

    ControlJobs& jobs_;
    std::uint64_t seq_;
    bool finished_ = false;
};

/** The work of a job. The registry calls it on the message thread when started or woken, until it ends the
    job, and destroys it on the message thread afterwards or when the registry stops. Each call must
    return quickly and should not run a nested message loop; long work belongs on a thread of the
    work's own, which its destructor joins. */
using ControlJobWork = std::function<void(ControlJobStep& step)>;

/**
 * The jobs of one instance. All registry state and job work belong to the message thread; connection
 * threads submit asynchronous commands through ControlMessageThread.
 *
 * - Ids are j:<instanceIdShort>:<seq>, seq from 1. A job is created queued, becomes running at its first
 *   step and ends succeeded, failed or cancelled; terminal states never change again.
 * - Retention: a terminal job is kept for limits.jobRetentionMs after it finished, unless more than
 *   limits.jobRetentionCount jobs exist; then the terminal jobs that finished first are evicted first.
 *   Jobs that are not terminal are never evicted; when all retained jobs are active, start() refuses with
 *   BUSY(job_limit). Retention is measured with a monotonic clock, so that a change of the system time
 *   never evicts a job early; the timestamps of the job object are wall-clock time.
 * - job.get with waitMs keeps its request pending without blocking the message or connection thread.
 *   The message thread responds when the job becomes terminal, waitMs passes, or the registry stops.
 * - Cancellation: a job created cancellable stays so until it ends, until its work calls
 *   ControlJobStep::endCancellability(), or until job.cancel requests cancellation; the work sees the
 *   request at its next step and stops there.
 *
 * Threading: registry operations, stop() and destruction run on the message thread. Connection
 * commands are posted by ControlDispatcher. Cross-thread producers retain wakeCallback().
 *
 * Re-entrancy: when a step runs a nested message loop, a queued async update may run inside it; that nested
 * update does nothing and the outer dispatch schedules another update after the step returns. stop() called
 * from such a loop cancels queued updates; the works of the jobs that
 * the interrupted tick was running are destroyed when it returns, and none of them is called again.
 * Replies are independent snapshots delivered after the outermost operation has finished its state,
 * work destruction and scheduling. A responder may synchronously destroy the registry. Direct
 * destruction from inside a work step remains invalid: the step must first return.
 */
class ControlJobs : private juce::AsyncUpdater
{
public:
    /** How a lookup ended. */
    enum class Lookup {
        Found,
        NotFound,
        Stopped
    };
    /** How job.cancel ended. */
    enum class Cancel {
        Requested,
        AlreadyRequested,
        Terminal,
        NotCancellable,
        NotFound,
        Stopped
    };
    /** Result of start(): the job id, or empty with the refusal ("job_limit", or "stopping" after stop()). */
    struct Started
    {
        std::string jobId;
        std::string refusal;
    };
    ControlJobs(const ControlLimits& limits, ControlMessageThread::Handle messageThread);
    /** Calls stop() when it was not called; see the class threading rules. Never inside the nested
        message loop of a step (see Re-entrancy). */
    ~ControlJobs() override;

    ControlJobs(const ControlJobs&) = delete;
    ControlJobs& operator=(const ControlJobs&) = delete;

    /** The instance part of the job ids; set once the listener generated the instance id. Message thread. */
    void setInstanceShort(std::string instanceShort);

    /**
     * Creates a job of the given kind and queues its first step. idempotencyKey and
     * paramsDigest, when given, let findByKey() find the job while it is retained. Message thread.
     */
    Started start(const std::string& kind, bool cancellable, ControlJobWork work,
                  const std::string& idempotencyKey = {}, const std::string& paramsDigest = {});

    /** The retained job of that kind started with that idempotency key: its id and the params digest it
        was started with; empty id when none. Message thread. */
    std::string findByKey(const std::string& kind, const std::string& idempotencyKey, std::string& paramsDigest) const;

    /** The job of that kind that is not terminal and was started with that params digest; empty when none.
        For methods whose repeated call with the same params returns the job that still runs
        (export.start). Message thread. */
    std::string findActiveByDigest(const std::string& kind, const std::string& paramsDigest) const;

    /** The job object. Message thread. */
    Lookup get(const std::string& jobId, JsonValue& job) const;
    /** {jobId, kind, state} of a job, or null. Message thread. */
    JsonValue jobRef(const std::string& jobId) const;

    /** Responder of waitAsync(): called exactly once with the lookup result and the job object. */
    using WaitResponder = std::function<void(Lookup, JsonValue job)>;
    /**
     * job.get responds at once when waitMs is 0, the job is terminal or unknown;
     * otherwise when the job becomes terminal or waitMs passes, whichever comes first, and with Stopped
     * when stop() is called meanwhile. Message thread; never blocks. Responses follow the outermost
     * owner operation's cleanup and may destroy the owner.
     */
    void waitAsync(const std::string& jobId, int waitMs, WaitResponder respond);

    /** Cancels a job and delivers its snapshot after owner cleanup. Message thread. */
    using CancelResponder = std::function<void(Cancel, JsonValue job)>;
    void cancelAsync(const std::string& jobId, CancelResponder respond);

    /**
     * Stops the registry: waiting job.get requests are answered with Stopped at once, the deadline
     * scheduler stops, and the work of every job is destroyed on this thread (called from a nested message
     * loop inside a step: the works of the interrupted tick when it returns); no work is scheduled afterwards.
     * start() refuses from now on. Message thread; idempotent.
     */
    void stop();

    /** Wakes active work after a state change. Message thread. */
    void wake() noexcept;
    /** A callback safe to retain beyond this registry's lifetime. Any thread. */
    std::function<void()> wakeCallback() const;

    /** True once stop() was called. Message thread. */
    bool isStopped() const;
    /** The kinds of the jobs that are not terminal (queued or running), one entry per job, in the order the jobs were
        started. app.quit refuses while there is one. Message thread. */
    std::vector<std::string> activeKinds() const;

private:
    friend class ControlJobStep;
    struct Job;
    struct Waiter;
    struct Operation;
    struct WakeState;
    struct DeadlineScheduler;

    /** Wall-clock time of the timestamps. */
    std::int64_t now() const;
    /** Monotonic time of retention. */
    std::int64_t steadyNow() const;
    /** Advances every active job once. Message thread. */
    void tick();
    /** A terminal job retained beyond its time, which every reader treats as gone. */
    bool expired(const Job& job, std::int64_t steadyNowMs) const;
    void evict(std::int64_t steadyNowMs);
    Job* find(const std::string& jobId) const;
    JsonValue jobJson(const Job& job) const;
    void finish(Job& job, const char* state, std::optional<ControlJobError> error, JsonValue result);
    void deadlineReached();
    void rescheduleDeadline();
    void resolveWaiters(std::uint64_t seq, Lookup lookup, const JsonValue& job);
    void dispatchReadyWaiters();
    void wakeOnOwner() noexcept;
    void handleAsyncUpdate() override;

    ControlLimits limits_;
    std::string instanceShort_ = "00000000";
    std::uint64_t nextSeq_ = 1;
    std::map<std::uint64_t, std::unique_ptr<Job>> jobs_;
    std::vector<Waiter> waiters_;
    std::vector<std::function<void()>> readyWaiters_;
    std::shared_ptr<WakeState> wakeState_;
    std::unique_ptr<DeadlineScheduler> deadlineScheduler_;
    bool stopped_ = false;
    /** Outermost operation owns cleanup and result delivery, including nested message-loop calls. */
    int operationDepth_ = 0;
    /** A nested async callback was consumed while an outer tick still owned the work. Message thread only. */
    bool deferredUpdate_ = false;
};

} // namespace OpenTune
