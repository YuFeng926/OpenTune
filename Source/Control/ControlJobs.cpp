// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlJobs.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace OpenTune {

namespace {

using SteadyClock = std::chrono::steady_clock;

/** Interval of the tick that advances running jobs on the message thread. */
constexpr int kTickIntervalMs = 20;
/** Longest detail of an error object (PROTOCOL.md section 17.1). */
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

/** One job. Every member is guarded by ControlJobs::mutex_; work is only called and released on the
    message thread. */
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

    bool terminal() const { return isTerminalState(state); }
};

/** A job.get that waits for its job (section 12.2). */
struct ControlJobs::Waiter
{
    std::uint64_t seq = 0;
    SteadyClock::time_point deadline;
    WaitResponder respond;
};

/** The message-thread timer that advances running jobs; runs only while a job is active. */
class ControlJobs::Ticker : public juce::Timer
{
public:
    explicit Ticker(ControlJobs& jobs) : jobs_(jobs) {}
    ~Ticker() override { stopTimer(); }

    void timerCallback() override
    {
        // stop() called from a nested message loop inside tick() leaves this object alive (see stop()).
        if (!jobs_.tick())
            stopTimer();
    }

private:
    ControlJobs& jobs_;
};

// ---- ControlJobStep -----------------------------------------------------------------------------

bool ControlJobStep::cancelRequested() const
{
    std::lock_guard<std::mutex> lock(jobs_.mutex_);
    const auto it = jobs_.jobs_.find(seq_);
    return it != jobs_.jobs_.end() && it->second->cancelRequested;
}

bool ControlJobStep::endCancellability()
{
    std::lock_guard<std::mutex> lock(jobs_.mutex_);
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
    std::lock_guard<std::mutex> lock(jobs_.mutex_);
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
    {
        std::lock_guard<std::mutex> lock(jobs_.mutex_);
        const auto it = jobs_.jobs_.find(seq_);
        if (finished_ || it == jobs_.jobs_.end())
            return;
        jobs_.finishLocked(*it->second, kSucceeded, std::nullopt,
                           result.isObject() ? std::move(result) : JsonValue::object());
        finished_ = true;
    }
    jobs_.changed_.notify_all();
}

void ControlJobStep::fail(ControlJobError error, JsonValue result)
{
    {
        std::lock_guard<std::mutex> lock(jobs_.mutex_);
        const auto it = jobs_.jobs_.find(seq_);
        if (finished_ || it == jobs_.jobs_.end())
            return;
        jobs_.finishLocked(*it->second, kFailed, std::move(error), std::move(result));
        finished_ = true;
    }
    jobs_.changed_.notify_all();
}

void ControlJobStep::cancel(JsonValue result, bool changedState)
{
    {
        std::lock_guard<std::mutex> lock(jobs_.mutex_);
        const auto it = jobs_.jobs_.find(seq_);
        if (finished_ || it == jobs_.jobs_.end())
            return;
        jobs_.finishLocked(
            *it->second, kCancelled,
            ControlJobError{"CANCELLED", "cancelled_by_request", "job.cancel was called for the job", changedState},
            std::move(result));
        finished_ = true;
    }
    jobs_.changed_.notify_all();
}

// ---- ControlJobs --------------------------------------------------------------------------------

ControlJobs::ControlJobs(const ControlLimits& limits)
    : limits_(limits)
    , clock_([] { return static_cast<std::int64_t>(juce::Time::currentTimeMillis()); })
    , steadyClock_([] {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count());
    })
{}

ControlJobs::~ControlJobs()
{
    // Never inside a step's nested message loop: the interrupted tick would return into a destroyed
    // registry (see the class comment).
    jassert(tickDepth_ == 0);
    // ControlService stops the registry on the message thread first; this only covers a registry that a
    // test creates and destroys on the message thread.
    stop();
}

void ControlJobs::setInstanceShort(std::string instanceShort)
{
    std::lock_guard<std::mutex> lock(mutex_);
    instanceShort_ = std::move(instanceShort);
}

void ControlJobs::setClockForTesting(Clock clock, Clock steadyClock)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!steadyClock)
        steadyClock = clock;
    if (clock)
        clock_ = std::move(clock);
    if (steadyClock)
        steadyClock_ = std::move(steadyClock);
}

std::int64_t ControlJobs::now() const
{
    return clock_();
}

std::int64_t ControlJobs::steadyNow() const
{
    return steadyClock_();
}

bool ControlJobs::expiredLocked(const Job& job, std::int64_t steadyNowMs) const
{
    return job.terminal() && job.finishedSteadyMs + limits_.jobRetentionMs <= steadyNowMs;
}

ControlJobs::Started ControlJobs::start(const std::string& kind, bool cancellable, ControlJobWork work,
                                        const std::string& idempotencyKey, const std::string& paramsDigest)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    Started started;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            started.refusal = "stopping";
            return started;
        }
        const auto nowMs = now();
        evictLocked(steadyNow());
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
    }
    if (ticker_ == nullptr)
        ticker_ = std::make_unique<Ticker>(*this);
    if (!ticker_->isTimerRunning())
        ticker_->startTimer(kTickIntervalMs);
    return started;
}

std::string ControlJobs::findByKey(const std::string& kind, const std::string& idempotencyKey,
                                   std::string& paramsDigest) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Retention decides what is still there; a job evicted by age is evicted at the next start().
    const auto steadyNowMs = steadyNow();
    for (const auto& [seq, job] : jobs_) {
        if (!expiredLocked(*job, steadyNowMs) && !job->idempotencyKey.empty() && job->kind == kind
            && job->idempotencyKey == idempotencyKey) {
            paramsDigest = job->paramsDigest;
            return job->jobId;
        }
    }
    return {};
}

std::string ControlJobs::findActiveByDigest(const std::string& kind, const std::string& paramsDigest) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [seq, job] : jobs_)
        if (!job->terminal() && job->kind == kind && !paramsDigest.empty() && job->paramsDigest == paramsDigest)
            return job->jobId;
    return {};
}

void ControlJobs::evictLocked(std::int64_t steadyNowMs)
{
    // Terminal jobs older than the retention time first, then, while the registry is full, the
    // terminal jobs that finished first (on a tie the lower sequence number, the first in the map).
    // Jobs that are not terminal stay.
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (expiredLocked(*it->second, steadyNowMs))
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

ControlJobs::Job* ControlJobs::findLocked(const std::string& jobId) const
{
    const auto it = jobs_.find(sequenceOf(jobId));
    if (it == jobs_.end() || it->second->jobId != jobId)
        return nullptr;
    // A job retained beyond its time is gone for every reader (section 12.4).
    if (expiredLocked(*it->second, steadyNow()))
        return nullptr;
    return it->second.get();
}

JsonValue ControlJobs::jobJsonLocked(const Job& job) const
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

void ControlJobs::finishLocked(Job& job, const char* state, std::optional<ControlJobError> error, JsonValue result)
{
    job.state = state;
    job.error = std::move(error);
    job.result = std::move(result);
    job.cancellable = false;
    job.finishedAtMs = now();
    job.finishedSteadyMs = steadyNow();
    job.updatedAtMs = job.finishedAtMs;
}

ControlJobs::Lookup ControlJobs::get(const std::string& jobId, JsonValue& job) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return Lookup::Stopped;
    const Job* found = findLocked(jobId);
    if (found == nullptr)
        return Lookup::NotFound;
    job = jobJsonLocked(*found);
    return Lookup::Found;
}

JsonValue ControlJobs::jobRef(const std::string& jobId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Job* found = findLocked(jobId);
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
    Lookup lookup = Lookup::NotFound;
    JsonValue job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            lookup = Lookup::Stopped;
        } else if (const Job* found = findLocked(jobId)) {
            if (waitMs > 0 && !found->terminal()) {
                waiters_.push_back(
                    {found->seq, SteadyClock::now() + std::chrono::milliseconds(waitMs), std::move(respond)});
                ensureWaiterThreadLocked();
                changed_.notify_all();
                return;
            }
            lookup = Lookup::Found;
            job = jobJsonLocked(*found);
        }
    }
    respond(lookup, std::move(job));
}

void ControlJobs::ensureWaiterThreadLocked()
{
    if (!waiterThread_.joinable())
        waiterThread_ = std::thread(&ControlJobs::waiterLoop, this);
}

void ControlJobs::waiterLoop()
{
    struct Ready
    {
        WaitResponder respond;
        Lookup lookup;
        JsonValue job;
    };
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        std::vector<Ready> ready;
        const auto nowTime = SteadyClock::now();
        auto next = SteadyClock::time_point::max();
        for (auto it = waiters_.begin(); it != waiters_.end();) {
            const auto found = jobs_.find(it->seq);
            const Job* job = found != jobs_.end() ? found->second.get() : nullptr;
            if (stopped_) {
                ready.push_back({std::move(it->respond), Lookup::Stopped, JsonValue()});
            } else if (job == nullptr) {
                ready.push_back({std::move(it->respond), Lookup::NotFound, JsonValue()});
            } else if (job->terminal() || it->deadline <= nowTime) {
                ready.push_back({std::move(it->respond), Lookup::Found, jobJsonLocked(*job)});
            } else {
                next = std::min(next, it->deadline);
                ++it;
                continue;
            }
            it = waiters_.erase(it);
        }
        if (!ready.empty()) {
            // Responding only queues a line on the connection; never under the registry's mutex.
            lock.unlock();
            for (auto& entry : ready)
                entry.respond(entry.lookup, std::move(entry.job));
            ready.clear();
            lock.lock();
            continue;
        }
        if (stopped_)
            return;
        if (next == SteadyClock::time_point::max())
            changed_.wait(lock);
        else
            changed_.wait_until(lock, next);
    }
}

ControlJobs::Cancel ControlJobs::cancel(const std::string& jobId, JsonValue& job)
{
    Cancel outcome = Cancel::NotFound;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
            return Cancel::Stopped;
        Job* found = findLocked(jobId);
        if (found == nullptr)
            return Cancel::NotFound;
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
        job = jobJsonLocked(*found);
    }
    changed_.notify_all();
    return outcome;
}

bool ControlJobs::tick()
{
    // A step that runs a nested message loop lets the timer fire inside it. The outer tick is still in
    // that step, so the nested tick calls no work (no step is ever re-entered).
    if (tickDepth_ > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        return !stopped_;
    }
    struct DepthGuard
    {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
    } depthGuard(tickDepth_);

    std::vector<std::pair<std::uint64_t, std::shared_ptr<ControlJobWork>>> active;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
            return false;
        for (auto& [seq, job] : jobs_) {
            if (job->terminal() || job->work == nullptr)
                continue;
            if (job->state == kQueued) {
                job->state = kRunning;
                job->updatedAtMs = now();
            }
            active.emplace_back(seq, job->work);
        }
    }
    for (auto& [seq, work] : active) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_)
                return false;
        }
        ControlJobStep step(*this, seq);
        try {
            (*work)(step);
        } catch (...) {
            if (!step.finished())
                step.fail({"INTERNAL", "", "the job failed", true});
        }
        if (step.finished()) {
            // The work is destroyed here, on the message thread, outside the registry's mutex.
            std::shared_ptr<ControlJobWork> released;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (const auto it = jobs_.find(seq); it != jobs_.end())
                    released = std::move(it->second->work);
            }
            released.reset();
            work.reset();
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(jobs_.begin(), jobs_.end(),
                       [](const auto& entry) { return !entry.second->terminal() && entry.second->work != nullptr; });
}

void ControlJobs::stop()
{
    std::vector<std::shared_ptr<ControlJobWork>> works;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopped_) {
            stopped_ = true;
            for (auto& [seq, job] : jobs_)
                if (job->work != nullptr)
                    works.push_back(std::move(job->work));
        }
    }
    // The waiter thread answers every waiting job.get with Stopped, then ends.
    changed_.notify_all();
    if (waiterThread_.joinable() && waiterThread_.get_id() != std::this_thread::get_id())
        waiterThread_.join();
    if (ticker_ != nullptr) {
        ticker_->stopTimer();
        // Called from a nested message loop inside a step, the ticker's timerCallback() has not returned
        // yet and still uses the ticker: it is destroyed with the registry instead.
        if (tickDepth_ == 0)
            ticker_.reset();
    }
    // The work of every job is destroyed here, except the works the interrupted tick still holds (they go
    // when it returns); whatever threads a work owns are joined by its destructor.
    works.clear();
}

bool ControlJobs::isStopped() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}

std::size_t ControlJobs::retainedCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

std::size_t ControlJobs::activeCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(jobs_.begin(), jobs_.end(), [](const auto& entry) { return !entry.second->terminal(); }));
}

std::vector<std::string> ControlJobs::activeKinds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> kinds;
    // jobs_ is ordered by the sequence number, which is the order the jobs were started in.
    for (const auto& entry : jobs_)
        if (!entry.second->terminal())
            kinds.push_back(entry.second->kind);
    return kinds;
}

std::size_t ControlJobs::waitingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

} // namespace OpenTune
