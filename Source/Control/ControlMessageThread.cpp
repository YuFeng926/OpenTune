// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlMessageThread.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace OpenTune {

namespace {

using Clock = std::chrono::steady_clock;

struct AsyncCallState;
using AsyncDeadlines = std::multimap<Clock::time_point, std::shared_ptr<AsyncCallState>>;

struct AsyncCompletion
{
    std::function<void(ControlMessageThread::CallResult)> callback;
    ControlMessageThread::CallResult result;
};

struct AsyncCallState
{
    enum class Phase {
        Queued,
        Running,
        Finished,
        TimedOut,
        Stopped
    };

    Phase phase = Phase::Queued;
    Clock::time_point expiresAt;
    AsyncDeadlines::iterator deadline;
    std::function<void(ControlMessageThread::CallResult)> completion;
};

struct PostedCallState;
using PostedCalls = std::list<std::shared_ptr<PostedCallState>>;

struct PostedCallState
{
    enum class Phase {
        Queued,
        Running,
        Stopped
    };

    Phase phase = Phase::Queued;
    PostedCalls::iterator pending;
    std::function<void()> fn;
    std::function<void()> onStopped;
};

void completeAsync(std::function<void(ControlMessageThread::CallResult)> completion,
                   ControlMessageThread::CallResult result) noexcept
{
    if (!completion)
        return;
    try {
        completion(result);
    } catch (...) {
    }
}

void runPosted(std::function<void()> fn) noexcept
{
    if (!fn)
        return;
    try {
        fn();
    } catch (...) {
    }
}

} // namespace

/** State shared by the owner, its queued functions and pending submissions; it outlives the owner as
    long as one of them still refers to it. */
struct ControlMessageThread::Shared
{
    mutable std::mutex mutex;
    std::condition_variable changed;
    /** The liveness token. Cleared by stop() while holding mutex; queued functions read it atomically. */
    std::atomic<bool> alive{true};
    int waiting = 0;
    AsyncDeadlines deadlines;
    std::deque<AsyncCompletion> ready;
    PostedCalls posts;
};

ControlMessageThread::ControlMessageThread() : shared_(std::make_shared<Shared>()) {}

ControlMessageThread::~ControlMessageThread()
{
    stop();
}

ControlMessageThread::Handle ControlMessageThread::handle()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (shared_->alive.load() && !deadlineThread_.joinable())
        deadlineThread_ = std::thread(&ControlMessageThread::timeoutLoop, shared_);
    return Handle(shared_);
}

bool ControlMessageThread::Handle::post(std::function<void()> fn, std::function<void()> onStopped) const
{
    if (!fn) {
        runPosted(std::move(onStopped));
        return false;
    }

    auto shared = shared_;
    auto state = std::make_shared<PostedCallState>();
    state->fn = std::move(fn);
    state->onStopped = std::move(onStopped);
    std::function<void()> rejected;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (!shared->alive.load()) {
            state->phase = PostedCallState::Phase::Stopped;
            rejected = std::move(state->onStopped);
        } else {
            shared->posts.push_back(state);
            state->pending = std::prev(shared->posts.end());
            queued = true;
        }
    }
    if (!queued) {
        runPosted(std::move(rejected));
        return false;
    }

    // juce::MessageManager::callAsync: the only way the control API reaches the message thread.
    const bool posted = juce::MessageManager::callAsync([shared, state] {
        std::function<void()> run;
        std::function<void()> reject;
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (state->phase != PostedCallState::Phase::Queued)
                return;
            shared->posts.erase(state->pending);
            if (shared->alive.load()) {
                state->phase = PostedCallState::Phase::Running;
                run = std::move(state->fn);
            } else {
                state->phase = PostedCallState::Phase::Stopped;
                reject = std::move(state->onStopped);
            }
        }
        runPosted(std::move(run));
        runPosted(std::move(reject));
    });

    if (!posted) {
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (state->phase == PostedCallState::Phase::Queued) {
                shared->posts.erase(state->pending);
                state->phase = PostedCallState::Phase::Stopped;
                rejected = std::move(state->onStopped);
            }
        }
        runPosted(std::move(rejected));
    }
    return posted;
}

void ControlMessageThread::Handle::submit(std::function<void()> fn, int timeoutMs,
                                         std::function<void(CallResult)> completion) const
{
    auto shared = shared_;
    auto state = std::make_shared<AsyncCallState>();
    state->expiresAt = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
    state->completion = std::move(completion);

    std::function<void(CallResult)> immediateCompletion;
    CallResult immediateResult = CallResult::NotPosted;
    {
        // Publish the queued message and its deadline under the same state lock. Neither the message
        // thread nor the deadline worker can claim the request before registration is complete.
        std::unique_lock<std::mutex> lock(shared->mutex);
        if (!fn) {
            immediateCompletion = std::move(state->completion);
        } else if (!shared->alive.load()) {
            immediateCompletion = std::move(state->completion);
            immediateResult = CallResult::Stopped;
        } else {
            state->deadline = shared->deadlines.emplace(state->expiresAt, state);
            ++shared->waiting;
            const bool posted = juce::MessageManager::callAsync([shared, state, fn = std::move(fn)] {
                std::function<void(CallResult)> expiredCompletion;
                bool expired = false;
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    if (state->phase != AsyncCallState::Phase::Queued)
                        return;
                    shared->deadlines.erase(state->deadline);
                    expired = Clock::now() >= state->expiresAt;
                    if (expired) {
                        state->phase = AsyncCallState::Phase::TimedOut;
                        --shared->waiting;
                        expiredCompletion = std::move(state->completion);
                    } else {
                        state->phase = AsyncCallState::Phase::Running;
                    }
                }
                shared->changed.notify_all();
                if (expired) {
                    completeAsync(std::move(expiredCompletion), CallResult::TimedOut);
                    return;
                }

                bool threw = false;
                try {
                    fn();
                } catch (...) {
                    threw = true;
                }

                std::function<void(CallResult)> stoppedCompletion;
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    state->phase = AsyncCallState::Phase::Finished;
                    --shared->waiting;
                    const auto result = threw ? CallResult::Failed : CallResult::Done;
                    if (shared->alive.load())
                        shared->ready.push_back({std::move(state->completion), result});
                    else
                        stoppedCompletion = std::move(state->completion);
                }
                shared->changed.notify_all();
                completeAsync(std::move(stoppedCompletion), threw ? CallResult::Failed : CallResult::Done);
            });
            if (!posted) {
                shared->deadlines.erase(state->deadline);
                state->phase = AsyncCallState::Phase::Finished;
                --shared->waiting;
                immediateCompletion = std::move(state->completion);
            }
        }
    }
    shared->changed.notify_all();
    completeAsync(std::move(immediateCompletion), immediateResult);
}

void ControlMessageThread::timeoutLoop(std::shared_ptr<Shared> shared)
{
    std::unique_lock<std::mutex> lock(shared->mutex);
    for (;;) {
        if (!shared->ready.empty()) {
            auto completion = std::move(shared->ready.front());
            shared->ready.pop_front();
            lock.unlock();
            completeAsync(std::move(completion.callback), completion.result);
            lock.lock();
            continue;
        }
        if (!shared->alive.load()) {
            return;
        }
        if (shared->deadlines.empty()) {
            shared->changed.wait(lock);
            continue;
        }

        const auto first = shared->deadlines.begin();
        const auto state = first->second;
        if (Clock::now() < first->first) {
            const auto deadline = first->first;
            shared->changed.wait_until(lock, deadline);
            continue;
        }

        shared->deadlines.erase(first);
        state->phase = AsyncCallState::Phase::TimedOut;
        --shared->waiting;
        auto completion = std::move(state->completion);
        lock.unlock();
        completeAsync(std::move(completion), CallResult::TimedOut);
        lock.lock();
    }
}

void ControlMessageThread::close()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    auto shared = shared_;
    std::vector<std::function<void(CallResult)>> completions;
    std::vector<std::function<void()>> postStops;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->alive.exchange(false)) {
            for (const auto& state : shared->posts) {
                state->phase = PostedCallState::Phase::Stopped;
                if (state->onStopped)
                    postStops.push_back(std::move(state->onStopped));
                state->fn = {};
            }
            shared->posts.clear();
            for (auto& [deadline, state] : shared->deadlines) {
                (void)deadline;
                state->phase = AsyncCallState::Phase::Stopped;
                --shared->waiting;
                completions.push_back(std::move(state->completion));
            }
            shared->deadlines.clear();
        }
    }
    shared->changed.notify_all();
    for (auto& onStopped : postStops)
        runPosted(std::move(onStopped));
    for (auto& completion : completions)
        completeAsync(std::move(completion), CallResult::Stopped);
}

void ControlMessageThread::stop()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    // A close responder may destroy this owner. Finish using its members before delivery.
    auto worker = std::move(deadlineThread_);
    close();
    if (worker.joinable())
        worker.join();
}

bool ControlMessageThread::Handle::isAlive() const
{
    return shared_ && shared_->alive.load();
}

int ControlMessageThread::Handle::waitingCalls() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->waiting;
}

} // namespace OpenTune
