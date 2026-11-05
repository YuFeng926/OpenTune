// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlMessageThread.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace OpenTune {

/** State shared by the owner, its queued functions and the threads waiting in call(); it outlives the
    owner as long as one of them still refers to it. */
struct ControlMessageThread::Shared
{
    mutable std::mutex mutex;
    std::condition_variable changed;
    /** The liveness token. Cleared by stop() while holding mutex, so that a waiting call() cannot miss
        it; read without the mutex on the message thread, where stop() also runs. */
    std::atomic<bool> alive{true};
    int waiting = 0;
};

namespace {

/** Progress of one call(); guarded by ControlMessageThread::Shared::mutex. */
struct CallState
{
    enum class Phase {
        Queued,
        Running,
        Finished,
        Abandoned
    };
    Phase phase = Phase::Queued;
    bool threw = false;
};

} // namespace

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

ControlMessageThread::CallResult ControlMessageThread::call(std::function<void()> fn, int timeoutMs)
{
    if (!fn || juce::MessageManager::existsAndIsCurrentThread())
        return CallResult::NotPosted;
    // Only local copies are used from here on: the owner may be destroyed while this thread waits.
    auto shared = shared_;
    auto state = std::make_shared<CallState>();
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (!shared->alive.load())
            return CallResult::Stopped;
        ++shared->waiting;
    }
    const bool posted = juce::MessageManager::callAsync([shared, state, fn = std::move(fn)] {
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (!shared->alive.load() || state->phase != CallState::Phase::Queued)
                return;
            state->phase = CallState::Phase::Running;
        }
        bool threw = false;
        try {
            fn();
        } catch (...) {
            threw = true;
        }
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            state->phase = CallState::Phase::Finished;
            state->threw = threw;
        }
        shared->changed.notify_all();
    });

    std::unique_lock<std::mutex> lock(shared->mutex);
    if (!posted) {
        --shared->waiting;
        return CallResult::NotPosted;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
    shared->changed.wait_until(lock, deadline,
                               [&] { return state->phase == CallState::Phase::Finished || !shared->alive.load(); });
    // Started on the message thread: it finishes there, and a timeout must not hide that it ran.
    if (state->phase == CallState::Phase::Running)
        shared->changed.wait(lock, [&] { return state->phase == CallState::Phase::Finished; });
    --shared->waiting;
    if (state->phase == CallState::Phase::Finished)
        return state->threw ? CallResult::Failed : CallResult::Done;
    state->phase = CallState::Phase::Abandoned;
    return shared->alive.load() ? CallResult::TimedOut : CallResult::Stopped;
}

void ControlMessageThread::stop()
{
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (!shared_->alive.exchange(false))
            return;
    }
    shared_->changed.notify_all();
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
