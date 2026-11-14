// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlMessageThread.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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
    bool posting = true;
    bool hasDeadline = false;
    AsyncDeadlines::iterator deadline;
    std::function<void(ControlMessageThread::CallResult)> completion;
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
    std::thread deadlineThread;
    std::thread::id deadlineThreadId;
    std::mutex deadlineJoinMutex;
};

ControlMessageThread::ControlMessageThread() : shared_(std::make_shared<Shared>()) {}

ControlMessageThread::~ControlMessageThread()
{
    stop();
}

bool ControlMessageThread::post(std::function<void()> fn)
{
    if (!fn || !shared_->alive.load())
        return false;
    auto shared = shared_;
    // juce::MessageManager::callAsync: the only way the control API reaches the message thread.
    return juce::MessageManager::callAsync([shared, fn = std::move(fn)] {
        if (shared->alive.load())
            fn();
    });
}

void ControlMessageThread::submit(std::function<void()> fn, int timeoutMs,
                                  std::function<void(CallResult)> completion)
{
    auto shared = shared_;
    auto state = std::make_shared<AsyncCallState>();
    state->completion = std::move(completion);

    std::function<void(CallResult)> immediateCompletion;
    CallResult immediateResult = CallResult::NotPosted;
    bool completeImmediately = false;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (!fn) {
            immediateCompletion = std::move(state->completion);
            completeImmediately = true;
        } else if (!shared->alive.load()) {
            immediateCompletion = std::move(state->completion);
            immediateResult = CallResult::Stopped;
            completeImmediately = true;
        } else {
            state->deadline = shared->deadlines.emplace(Clock::time_point::max(), state);
            state->hasDeadline = true;
            ++shared->waiting;
            try {
                if (!shared->deadlineThread.joinable()) {
                    shared->deadlineThread = std::thread(&ControlMessageThread::timeoutLoop, shared);
                    shared->deadlineThreadId = shared->deadlineThread.get_id();
                }
            } catch (...) {
                shared->deadlines.erase(state->deadline);
                state->hasDeadline = false;
                --shared->waiting;
                immediateCompletion = std::move(state->completion);
                completeImmediately = true;
            }
        }
    }
    if (completeImmediately) {
        completeAsync(std::move(immediateCompletion), immediateResult);
        return;
    }

    const bool posted = juce::MessageManager::callAsync([shared, state, fn = std::move(fn)] {
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (!shared->alive.load() || state->phase != AsyncCallState::Phase::Queued)
                return;
            state->phase = AsyncCallState::Phase::Running;
            if (state->hasDeadline) {
                shared->deadlines.erase(state->deadline);
                state->hasDeadline = false;
            }
        }
        shared->changed.notify_all();

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

    std::function<void(CallResult)> postFailureCompletion;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (!posted && state->phase == AsyncCallState::Phase::Queued) {
            if (state->hasDeadline) {
                shared->deadlines.erase(state->deadline);
                state->hasDeadline = false;
            }
            state->phase = AsyncCallState::Phase::Finished;
            --shared->waiting;
            postFailureCompletion = std::move(state->completion);
        } else if (posted) {
            if (state->phase == AsyncCallState::Phase::Queued) {
                if (state->hasDeadline)
                    shared->deadlines.erase(state->deadline);
                const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
                state->deadline = shared->deadlines.emplace(deadline, state);
                state->hasDeadline = true;
            }
            state->posting = false;
        }
    }
    shared->changed.notify_all();
    completeAsync(std::move(postFailureCompletion), CallResult::NotPosted);
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
        if (!shared->alive.load())
            return;
        if (shared->deadlines.empty()) {
            shared->changed.wait(lock);
            continue;
        }

        const auto first = shared->deadlines.begin();
        const auto state = first->second;
        if (state->posting) {
            shared->changed.wait(lock);
            continue;
        }
        if (Clock::now() < first->first) {
            const auto deadline = first->first;
            shared->changed.wait_until(lock, deadline);
            continue;
        }

        shared->deadlines.erase(first);
        state->hasDeadline = false;
        if (state->phase != AsyncCallState::Phase::Queued)
            continue;
        state->phase = AsyncCallState::Phase::TimedOut;
        --shared->waiting;
        auto completion = std::move(state->completion);
        lock.unlock();
        completeAsync(std::move(completion), CallResult::TimedOut);
        lock.lock();
    }
}

void ControlMessageThread::stop()
{
    std::vector<std::function<void(CallResult)>> completions;
    std::thread::id deadlineThreadId;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        deadlineThreadId = shared_->deadlineThreadId;
        if (shared_->alive.exchange(false)) {
            for (auto& [deadline, state] : shared_->deadlines) {
                (void)deadline;
                state->hasDeadline = false;
                if (state->phase == AsyncCallState::Phase::Queued) {
                    state->phase = AsyncCallState::Phase::Stopped;
                    --shared_->waiting;
                    completions.push_back(std::move(state->completion));
                }
            }
            shared_->deadlines.clear();
        }
    }
    shared_->changed.notify_all();
    for (auto& completion : completions)
        completeAsync(std::move(completion), CallResult::Stopped);
    if (deadlineThreadId != std::this_thread::get_id()) {
        std::lock_guard<std::mutex> joinLock(shared_->deadlineJoinMutex);
        if (shared_->deadlineThread.joinable())
            shared_->deadlineThread.join();
    }
}

bool ControlMessageThread::isStopped() const
{
    return !shared_->alive.load();
}

bool ControlMessageThread::isAlive() const
{
    return shared_->alive.load();
}

int ControlMessageThread::waitingCalls() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->waiting;
}

} // namespace OpenTune
