// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlJobs.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace OpenTune {

namespace {

using SteadyClock = std::chrono::steady_clock;

/** Longest detail of an error object. */
constexpr std::size_t kMaxDetailBytes = 1024;

constexpr const char* kQueued = "queued";
constexpr const char* kRunning = "running";
constexpr const char* kSucceeded = "succeeded";
constexpr const char* kFailed = "failed";
constexpr const char* kCancelled = "cancelled";

/** Printable ASCII, at most kMaxDetailBytes (errorData.detail). */
std::string printableDetail(const std::string& text)
{
    std::string out;
    for (const char c : text)
        if (c >= 0x20 && c <= 0x7e && out.size() < kMaxDetailBytes)
            out.push_back(c);
    return out;
}

/** A fraction in [0, 1] with four decimals, written independently of the C locale. */
JsonValue fractionJson(double value)
{
    const auto scaled = static_cast<long long>(std::llround(std::clamp(value, 0.0, 1.0) * 10000.0));
    if (scaled >= 10000)
        return JsonValue::integer(1);
    if (scaled <= 0)
        return JsonValue::integer(0);
    std::string digits = std::to_string(scaled);
    digits.insert(0, 4 - digits.size(), '0');
    while (digits.back() == '0')
        digits.pop_back();
    return JsonValue::number("0." + digits, static_cast<double>(scaled) / 10000.0);
}

/** The sequence number of a job id of the form j:<8 lowercase hex>:<seq>, or 0. */
std::uint64_t sequenceOf(const std::string& jobId)
{
    if (jobId.size() < 12 || jobId.compare(0, 2, "j:") != 0 || jobId[10] != ':')
        return 0;
    std::uint64_t seq = 0;
    for (std::size_t i = 11; i < jobId.size(); ++i) {
        const char c = jobId[i];
        if (c < '0' || c > '9' || (i == 11 && c == '0'))
            return 0;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (seq > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return 0;
        seq = seq * 10 + digit;
    }
    return seq;
}

bool isTerminalState(const std::string& state)
{
    return state == kSucceeded || state == kFailed || state == kCancelled;
}

} // namespace

JsonValue controlErrorObjectJson(const ControlJobError& error)
{
    auto data = JsonValue::object();
    data.appendMember("kind", JsonValue::string(error.kind));
    if (!error.reason.empty())
        data.appendMember("reason", JsonValue::string(error.reason));
    data.appendMember("retryable", JsonValue::boolean(controlErrorRetryable(error.kind, error.reason)));
    data.appendMember("detail", JsonValue::string(printableDetail(error.detail)));
    data.appendMember("executed", JsonValue::boolean(error.executed));
    auto object = JsonValue::object();
    object.appendMember("code", JsonValue::integer(controlErrorCode(error.kind, error.reason)));
    object.appendMember("message", JsonValue::string(error.kind));
    object.appendMember("data", std::move(data));
    return object;
}

/** One job. Every member is owned by the message thread; work is called and released there too. */
struct ControlJobs::Job
{
    std::uint64_t seq = 0;
    std::string jobId;
    std::string kind;
    std::string state = kQueued;
    /** Created cancellable and not yet past its point of no return. */
    bool cancellable = false;
    bool cancelRequested = false;
    std::int64_t createdAtMs = 0;
    std::int64_t updatedAtMs = 0;
    std::int64_t finishedAtMs = -1;
    /** Monotonic time of the end, for retention. */
    std::int64_t finishedSteadyMs = 0;
    std::optional<double> fraction;
    std::string phase;
    std::optional<int> itemsDone;
    std::optional<int> itemsTotal;
    JsonValue result;
    std::optional<ControlJobError> error;
    std::string idempotencyKey;
    std::string paramsDigest;
    std::shared_ptr<ControlJobWork> work;
    std::optional<SteadyClock::time_point> wakeAt;

    bool terminal() const { return isTerminalState(state); }
};

/** A job.get request waiting for its job. */
struct ControlJobs::Waiter
{
    std::uint64_t seq = 0;
    SteadyClock::time_point deadline;
    WaitResponder respond;
};

struct ControlJobs::Operation
{
    explicit Operation(ControlJobs& ownerIn) : owner(ownerIn) { ++owner.operationDepth_; }
    ~Operation()
    {
        if (--owner.operationDepth_ == 0)
            owner.dispatchReadyWaiters();
    }
    ControlJobs& owner;
};

struct ControlJobs::WakeState : std::enable_shared_from_this<WakeState>
{
    explicit WakeState(ControlJobs& jobsIn, ControlMessageThread::Handle messageThreadIn)
        : owner(&jobsIn), messageThread(std::move(messageThreadIn))
    {}

    void requestWake()
    {
        if (juce::MessageManager::existsAndIsCurrentThread()) {
            if (owner != nullptr)
                owner->wakeOnOwner();
            return;
        }
        if (wakeQueued.exchange(true))
            return;

        const std::weak_ptr<WakeState> weak = shared_from_this();
        const auto clearQueued = [weak] {
            if (const auto state = weak.lock())
                state->wakeQueued = false;
        };
        messageThread.post([weak] {
            if (const auto state = weak.lock()) {
                state->wakeQueued = false;
                if (state->owner != nullptr)
                    state->owner->wakeOnOwner();
            }
        }, clearQueued);
    }

    ControlJobs* owner;
    ControlMessageThread::Handle messageThread;
    std::atomic<bool> wakeQueued{false};
};

struct ControlJobs::DeadlineScheduler
{
    explicit DeadlineScheduler(std::function<void()> dueIn) : due(std::move(dueIn)) {}

    ~DeadlineScheduler() { stop(); }

    void schedule(std::optional<SteadyClock::time_point> next)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping)
                return;
            deadline = next;
            ++revision;
            if (deadline && !worker.joinable())
                worker = std::thread(&DeadlineScheduler::run, this);
        }
        changed.notify_all();
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping)
                return;
            stopping = true;
            deadline.reset();
            ++revision;
        }
        changed.notify_all();
        if (worker.joinable())
            worker.join();
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            if (!deadline) {
                changed.wait(lock, [this] { return stopping || deadline.has_value(); });
                continue;
            }

            const auto scheduledRevision = revision;
            const auto scheduledDeadline = *deadline;
            if (changed.wait_until(lock, scheduledDeadline,
                                   [this, scheduledRevision] { return stopping || revision != scheduledRevision; }))
                continue;

            deadline.reset();
            ++revision;
            lock.unlock();
            due();
            lock.lock();
        }
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::optional<SteadyClock::time_point> deadline;
    std::uint64_t revision = 0;
    bool stopping = false;
    std::function<void()> due;
};

// ---- ControlJobStep -----------------------------------------------------------------------------

bool ControlJobStep::cancelRequested() const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const auto it = jobs_.jobs_.find(seq_);
    return it != jobs_.jobs_.end() && it->second->cancelRequested;
}

bool ControlJobStep::endCancellability()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const auto it = jobs_.jobs_.find(seq_);
    if (it == jobs_.jobs_.end() || it->second->cancelRequested)
        return false;
    it->second->cancellable = false;
    it->second->updatedAtMs = jobs_.now();
    return true;
}

void ControlJobStep::setProgress(std::optional<double> fraction, const char* phase, std::optional<int> itemsDone,
                                 std::optional<int> itemsTotal)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const auto it = jobs_.jobs_.find(seq_);
    if (it == jobs_.jobs_.end() || it->second->terminal())
        return;
    auto& job = *it->second;
    job.fraction = fraction;
    job.phase = phase != nullptr ? phase : "";
    job.itemsDone = itemsDone;
    job.itemsTotal = itemsTotal;
    job.updatedAtMs = jobs_.now();
}

void ControlJobStep::succeed(JsonValue result)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const auto it = jobs_.jobs_.find(seq_);
    if (finished_ || it == jobs_.jobs_.end())
        return;
    jobs_.finish(*it->second, kSucceeded, std::nullopt,
                 result.isObject() ? std::move(result) : JsonValue::object());
    finished_ = true;
}

void ControlJobStep::fail(ControlJobError error, JsonValue result)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const auto it = jobs_.jobs_.find(seq_);
    if (finished_ || it == jobs_.jobs_.end())
        return;
    jobs_.finish(*it->second, kFailed, std::move(error), std::move(result));
    finished_ = true;
}

void ControlJobStep::cancel(JsonValue result, bool changedState)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const auto it = jobs_.jobs_.find(seq_);
    if (finished_ || it == jobs_.jobs_.end())
        return;
    jobs_.finish(
        *it->second, kCancelled,
        ControlJobError{"CANCELLED", "cancelled_by_request", "job.cancel was called for the job", changedState},
        std::move(result));
    finished_ = true;
}

void ControlJobStep::wakeAt(std::chrono::steady_clock::time_point deadline)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const auto it = jobs_.jobs_.find(seq_);
    if (it == jobs_.jobs_.end() || it->second->terminal())
        return;
    if (deadline <= SteadyClock::now()) {
        it->second->wakeAt.reset();
        jobs_.wakeOnOwner();
    } else {
        it->second->wakeAt = deadline;
        jobs_.rescheduleDeadline();
    }
}

std::function<void()> ControlJobStep::wakeCallback() const
{
    return jobs_.wakeCallback();
}

// ---- ControlJobs --------------------------------------------------------------------------------

ControlJobs::ControlJobs(const ControlLimits& limits, ControlMessageThread::Handle messageThread)
    : limits_(limits)
    , wakeState_(std::make_shared<WakeState>(*this, messageThread))
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const std::weak_ptr<WakeState> weakWake = wakeState_;
    deadlineScheduler_ = std::make_unique<DeadlineScheduler>([weakWake, messageThread] {
        messageThread.post([weakWake] {
            if (const auto state = weakWake.lock(); state && state->owner != nullptr)
                state->owner->deadlineReached();
        });
    });
}

ControlJobs::~ControlJobs()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    jassert(operationDepth_ == 0);
    wakeState_->owner = nullptr;
    stop();
}

void ControlJobs::setInstanceShort(std::string instanceShort)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    instanceShort_ = std::move(instanceShort);
}

std::int64_t ControlJobs::now() const
{
    return static_cast<std::int64_t>(juce::Time::currentTimeMillis());
}

std::int64_t ControlJobs::steadyNow() const
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count());
}

bool ControlJobs::expired(const Job& job, std::int64_t steadyNowMs) const
{
    return job.terminal() && job.finishedSteadyMs + limits_.jobRetentionMs <= steadyNowMs;
}

ControlJobs::Started ControlJobs::start(const std::string& kind, bool cancellable, ControlJobWork work,
                                        const std::string& idempotencyKey, const std::string& paramsDigest)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    Started started;
    if (stopped_) {
        started.refusal = "stopping";
        return started;
    }
    const auto nowMs = now();
    evict(steadyNow());
    if (static_cast<int>(jobs_.size()) >= limits_.jobRetentionCount) {
        started.refusal = "job_limit";
        return started;
    }
    auto job = std::make_unique<Job>();
    job->seq = nextSeq_++;
    job->jobId = "j:" + instanceShort_ + ":" + std::to_string(job->seq);
    job->kind = kind;
    job->cancellable = cancellable;
    job->createdAtMs = nowMs;
    job->updatedAtMs = nowMs;
    job->idempotencyKey = idempotencyKey;
    job->paramsDigest = paramsDigest;
    job->work = std::make_shared<ControlJobWork>(std::move(work));
    started.jobId = job->jobId;
    jobs_.emplace(job->seq, std::move(job));
    wakeOnOwner();
    return started;
}

void ControlJobs::wake() noexcept
{
    wakeOnOwner();
}

void ControlJobs::wakeOnOwner() noexcept
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (!stopped_)
        triggerAsyncUpdate();
}

std::function<void()> ControlJobs::wakeCallback() const
{
    const std::weak_ptr<WakeState> weak = wakeState_;
    return [weak] {
        if (const auto state = weak.lock())
            state->requestWake();
    };
}

std::string ControlJobs::findByKey(const std::string& kind, const std::string& idempotencyKey,
                                   std::string& paramsDigest) const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    // Retention decides what is still there; a job evicted by age is evicted at the next start().
    const auto steadyNowMs = steadyNow();
    for (const auto& [seq, job] : jobs_) {
        if (!expired(*job, steadyNowMs) && !job->idempotencyKey.empty() && job->kind == kind
            && job->idempotencyKey == idempotencyKey) {
            paramsDigest = job->paramsDigest;
            return job->jobId;
        }
    }
    return {};
}

std::string ControlJobs::findActiveByDigest(const std::string& kind, const std::string& paramsDigest) const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    for (const auto& [seq, job] : jobs_)
        if (!job->terminal() && job->kind == kind && !paramsDigest.empty() && job->paramsDigest == paramsDigest)
            return job->jobId;
    return {};
}

void ControlJobs::evict(std::int64_t steadyNowMs)
{
    // Terminal jobs older than the retention time first, then, while the registry is full, the
    // terminal jobs that finished first (on a tie the lower sequence number, the first in the map).
    // Jobs that are not terminal stay.
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (expired(*it->second, steadyNowMs))
            it = jobs_.erase(it);
        else
            ++it;
    }
    while (static_cast<int>(jobs_.size()) >= limits_.jobRetentionCount) {
        auto oldest = jobs_.end();
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
            if (it->second->terminal()
                && (oldest == jobs_.end() || it->second->finishedSteadyMs < oldest->second->finishedSteadyMs))
                oldest = it;
        if (oldest == jobs_.end())
            return;
        jobs_.erase(oldest);
    }
}

ControlJobs::Job* ControlJobs::find(const std::string& jobId) const
{
    const auto it = jobs_.find(sequenceOf(jobId));
    if (it == jobs_.end() || it->second->jobId != jobId)
        return nullptr;
    // A job retained beyond its time is gone for every reader.
    if (expired(*it->second, steadyNow()))
        return nullptr;
    return it->second.get();
}

JsonValue ControlJobs::jobJson(const Job& job) const
{
    auto progress = JsonValue::object();
    progress.appendMember("fraction", job.fraction ? fractionJson(*job.fraction) : JsonValue());
    progress.appendMember("phase", job.phase.empty() ? JsonValue() : JsonValue::string(job.phase));
    progress.appendMember("itemsDone", job.itemsDone ? JsonValue::integer(*job.itemsDone) : JsonValue());
    progress.appendMember("itemsTotal", job.itemsTotal ? JsonValue::integer(*job.itemsTotal) : JsonValue());

    auto json = JsonValue::object();
    json.appendMember("jobId", JsonValue::string(job.jobId));
    json.appendMember("kind", JsonValue::string(job.kind));
    json.appendMember("state", JsonValue::string(job.state));
    json.appendMember("cancellable", JsonValue::boolean(job.cancellable && !job.cancelRequested && !job.terminal()));
    json.appendMember("createdAtMs", JsonValue::integer(job.createdAtMs));
    json.appendMember("updatedAtMs", JsonValue::integer(job.updatedAtMs));
    json.appendMember("finishedAtMs", job.finishedAtMs >= 0 ? JsonValue::integer(job.finishedAtMs) : JsonValue());
    json.appendMember("progress", std::move(progress));
    json.appendMember("result", job.terminal() ? job.result : JsonValue());
    json.appendMember("error", job.error ? controlErrorObjectJson(*job.error) : JsonValue());
    return json;
}

void ControlJobs::finish(Job& job, const char* state, std::optional<ControlJobError> error, JsonValue result)
{
    job.state = state;
    job.error = std::move(error);
    job.result = std::move(result);
    job.cancellable = false;
    job.finishedAtMs = now();
    job.finishedSteadyMs = steadyNow();
    job.updatedAtMs = job.finishedAtMs;
    job.wakeAt.reset();
    resolveWaiters(job.seq, Lookup::Found, jobJson(job));
    rescheduleDeadline();
}

ControlJobs::Lookup ControlJobs::get(const std::string& jobId, JsonValue& job) const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (stopped_)
        return Lookup::Stopped;
    const Job* found = find(jobId);
    if (found == nullptr)
        return Lookup::NotFound;
    job = jobJson(*found);
    return Lookup::Found;
}

JsonValue ControlJobs::jobRef(const std::string& jobId) const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    const Job* found = find(jobId);
    if (found == nullptr)
        return JsonValue();
    auto ref = JsonValue::object();
    ref.appendMember("jobId", JsonValue::string(found->jobId));
    ref.appendMember("kind", JsonValue::string(found->kind));
    ref.appendMember("state", JsonValue::string(found->state));
    return ref;
}

void ControlJobs::waitAsync(const std::string& jobId, int waitMs, WaitResponder respond)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    Operation operation(*this);
    if (stopped_) {
        readyWaiters_.push_back([respond = std::move(respond)] { respond(Lookup::Stopped, JsonValue()); });
        return;
    }
    const Job* found = find(jobId);
    if (found == nullptr) {
        readyWaiters_.push_back([respond = std::move(respond)] { respond(Lookup::NotFound, JsonValue()); });
        return;
    }
    if (waitMs > 0 && !found->terminal()) {
        waiters_.push_back({found->seq, SteadyClock::now() + std::chrono::milliseconds(waitMs),
                            std::move(respond)});
        rescheduleDeadline();
        return;
    }
    readyWaiters_.push_back([respond = std::move(respond), job = jobJson(*found)]() mutable {
        respond(Lookup::Found, std::move(job));
    });
}

void ControlJobs::cancelAsync(const std::string& jobId, CancelResponder respond)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    Operation operation(*this);
    Cancel outcome = Cancel::NotFound;
    if (stopped_) {
        readyWaiters_.push_back([respond = std::move(respond)] { respond(Cancel::Stopped, JsonValue()); });
        return;
    }
    Job* found = find(jobId);
    if (found == nullptr) {
        readyWaiters_.push_back([respond = std::move(respond)] { respond(Cancel::NotFound, JsonValue()); });
        return;
    }
    if (found->terminal()) {
        outcome = Cancel::Terminal;
    } else if (found->cancelRequested) {
        outcome = Cancel::AlreadyRequested;
    } else if (!found->cancellable) {
        outcome = Cancel::NotCancellable;
    } else {
        found->cancelRequested = true;
        found->updatedAtMs = now();
        outcome = Cancel::Requested;
    }
    if (outcome == Cancel::Requested)
        wakeOnOwner();
    readyWaiters_.push_back([respond = std::move(respond), outcome, job = jobJson(*found)]() mutable {
        respond(outcome, std::move(job));
    });
}

void ControlJobs::resolveWaiters(std::uint64_t seq, Lookup lookup, const JsonValue& job)
{
    for (auto it = waiters_.begin(); it != waiters_.end();) {
        if (it->seq != seq) {
            ++it;
            continue;
        }
        readyWaiters_.push_back([respond = std::move(it->respond), lookup, job]() mutable {
            respond(lookup, std::move(job));
        });
        it = waiters_.erase(it);
    }
}

void ControlJobs::dispatchReadyWaiters()
{
    auto ready = std::move(readyWaiters_);
    readyWaiters_.clear();
    // From this point only independent snapshots are touched. A responder may delete the registry.
    for (auto& respond : ready)
        respond();
}

void ControlJobs::rescheduleDeadline()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (stopped_)
        return;
    std::optional<SteadyClock::time_point> next;
    for (const auto& waiter : waiters_)
        if (!next || waiter.deadline < *next)
            next = waiter.deadline;
    for (const auto& [seq, job] : jobs_) {
        (void)seq;
        if (job->wakeAt && (!next || *job->wakeAt < *next))
            next = job->wakeAt;
    }
    deadlineScheduler_->schedule(next);
}

void ControlJobs::deadlineReached()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    Operation operation(*this);
    if (stopped_)
        return;

    const auto current = SteadyClock::now();
    bool jobsDue = false;
    for (auto& [seq, job] : jobs_) {
        (void)seq;
        if (job->wakeAt && *job->wakeAt <= current) {
            job->wakeAt.reset();
            jobsDue = true;
        }
    }
    for (auto it = waiters_.begin(); it != waiters_.end();) {
        if (it->deadline > current) {
            ++it;
            continue;
        }
        const auto found = jobs_.find(it->seq);
        const auto lookup = found == jobs_.end() ? Lookup::NotFound : Lookup::Found;
        auto job = found == jobs_.end() ? JsonValue() : jobJson(*found->second);
        readyWaiters_.push_back([respond = std::move(it->respond), lookup, job = std::move(job)]() mutable {
            respond(lookup, std::move(job));
        });
        it = waiters_.erase(it);
    }
    rescheduleDeadline();
    if (jobsDue)
        wakeOnOwner();
}

void ControlJobs::handleAsyncUpdate()
{
    if (operationDepth_ > 0) {
        deferredUpdate_ = true;
        return;
    }
    Operation operation(*this);
    deferredUpdate_ = false;
    tick();
    if (deferredUpdate_ && !stopped_) {
        deferredUpdate_ = false;
        triggerAsyncUpdate();
    }
}

void ControlJobs::tick()
{
    if (stopped_)
        return;

    std::vector<std::pair<std::uint64_t, std::shared_ptr<ControlJobWork>>> active;
    for (auto& [seq, job] : jobs_) {
        if (job->terminal() || job->work == nullptr)
            continue;
        job->wakeAt.reset();
        if (job->state == kQueued) {
            job->state = kRunning;
            job->updatedAtMs = now();
        }
        active.emplace_back(seq, job->work);
    }

    for (auto& [seq, work] : active) {
        if (stopped_)
            return;
        ControlJobStep step(*this, seq);
        try {
            (*work)(step);
        } catch (...) {
            if (!step.finished())
                step.fail({"INTERNAL", "", "the job failed", true});
        }
        if (step.finished()) {
            const auto it = jobs_.find(seq);
            if (it != jobs_.end())
                it->second->work.reset();
            work.reset();
        }
    }
    rescheduleDeadline();
}

void ControlJobs::stop()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    Operation operation(*this);
    if (stopped_)
        return;
    stopped_ = true;
    deadlineScheduler_->stop();

    std::vector<std::shared_ptr<ControlJobWork>> works;
    for (auto& [seq, job] : jobs_) {
        (void)seq;
        if (job->work != nullptr)
            works.push_back(std::move(job->work));
    }
    for (auto& waiter : waiters_)
        readyWaiters_.push_back([respond = std::move(waiter.respond)] { respond(Lookup::Stopped, JsonValue()); });
    waiters_.clear();
    cancelPendingUpdate();
    works.clear();
}

bool ControlJobs::isStopped() const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    return stopped_;
}

std::vector<std::string> ControlJobs::activeKinds() const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    std::vector<std::string> kinds;
    // jobs_ is ordered by the sequence number, which is the order the jobs were started in.
    for (const auto& entry : jobs_)
        if (!entry.second->terminal())
            kinds.push_back(entry.second->kind);
    return kinds;
}

} // namespace OpenTune
