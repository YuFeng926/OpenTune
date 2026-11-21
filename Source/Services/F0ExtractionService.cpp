#include "F0ExtractionService.h"
#include "../Inference/F0InferenceService.h"
#include "../Utils/AppLogger.h"
#include <juce_events/juce_events.h>
#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
#include "../Inference/ModelFactory.h"
#include "../Services/F0AnalysisCache.h"
#include "../Services/ImportedClipF0Extraction.h"
#include "../Runtime/ProcessF0Runtime.h"
#include "../Utils/AppPreferences.h"
#include "../Utils/ModelPathResolver.h"
#include <algorithm>
#endif

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <unordered_map>

#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
#include <optional>
#endif

namespace OpenTune {
namespace {
std::atomic<uint64_t> nextF0TaskId{1};

F0TaskId allocateTaskId() noexcept
{
    return F0TaskId{nextF0TaskId.fetch_add(1, std::memory_order_relaxed)};
}
}

struct F0ExtractionService::SharedState
{
    using Commit = F0ExtractionService::CommitFn;
    using ResultType = F0ExtractionService::Result;

    struct DispatcherFailure
    {
        Commit callback;
        std::shared_ptr<ResultType> result;
    };

    class DispatcherFailureMailbox final : public juce::AsyncUpdater
    {
    public:
        void enqueue(Commit callback, std::shared_ptr<ResultType> result);
        void cancelAndClear();
        void handleAsyncUpdate() override;

    private:
        std::mutex mutex_;
        std::deque<DispatcherFailure> queue_;
    };

#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
    struct AraOrigin
    {
        F0TaskId taskId;
        uint64_t token{0};
        uint64_t submissionOrder{0};
        AraF0Pcm pcm;
        std::vector<AraF0Receiver> receivers;
        std::shared_ptr<F0RunOwnerState> ownerState;
        std::optional<F0ExtractionConfigSnapshot> config;
        std::string initializationError;
        F0AnalysisCache::KeyConfig keyConfig;
        F0AnalysisCache::Digest pcmDigest{};
        F0AnalysisCache::Digest digest{};
        bool hasTrustedDigest{false};
        bool deliveryClaimed{false};
    };

    struct AraGroup
    {
        F0TaskId groupId;
        F0TaskId ownerTaskId;
        F0TaskId publisherTaskId;
        bool hasDigest{false};
        F0AnalysisCache::Digest digest{};
        bool ownsQueueSlot{true};
        bool publicationGranted{false};
        SharedAraF0Outcome outcome;
        std::vector<std::shared_ptr<AraOrigin>> origins;
    };
#endif

    enum class Phase : uint8_t { General, Prepare, Execute };

    struct Task
    {
        F0TaskId taskId;
        F0RequestKey requestKey;
        uint64_t token{0};
        Phase phase{Phase::General};
        std::shared_ptr<F0RunOwnerState> ownerState;
        F0ExtractionService::ExecuteFn execute;
        F0ExtractionService::CommitFn commit;
        F0ExtractionService::CommitFn dispatcherFailure;
#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
        std::shared_ptr<AraOrigin> origin;
        F0TaskId groupId;
#endif
    };

    struct ActiveEntry
    {
        uint64_t token{0};
        std::shared_ptr<F0RunOwnerState> ownerState;
        std::weak_ptr<F0InferenceService> runService;
        bool ownsQueueSlot{false};
#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
        F0TaskId groupId;
        std::shared_ptr<AraOrigin> origin;
#endif
    };

    explicit SharedState(F0ExtractionService::WorkerTopology workerTopology)
        : topology(workerTopology) {}

    std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver;
    std::deque<Task> queue;
    size_t maxQueueSize{0};
    size_t pendingSlots{0};
    std::condition_variable queueCv;
    std::unordered_map<F0TaskId, ActiveEntry, F0TaskIdHash> activeEntries;
    std::unordered_map<ContentKey, F0TaskId> processorTaskIds;
#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
    std::unordered_map<F0TaskId, std::shared_ptr<AraGroup>, F0TaskIdHash> araGroups;
    std::map<F0AnalysisCache::Digest, F0TaskId> digestGroups;
#endif
    mutable std::mutex entriesMutex;
    uint64_t tokenCounter{1};
    uint64_t submissionCounter{1};
    bool shutdownStarted{false};
    F0ExtractionService::WorkerTopology topology;
    std::shared_ptr<DispatcherFailureMailbox> dispatcherFailureMailbox;

#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
    std::shared_ptr<AraGroup> takeAraGroupLocked(F0TaskId groupId)
    {
        const auto found = araGroups.find(groupId);
        if (found == araGroups.end())
            return {};
        auto group = found->second;
        if (group->hasDigest)
        {
            const auto digest = digestGroups.find(group->digest);
            if (digest != digestGroups.end() && digest->second == groupId)
                digestGroups.erase(digest);
        }
        if (group->ownsQueueSlot)
        {
            --pendingSlots;
            group->ownsQueueSlot = false;
        }
        for (const auto& origin : group->origins)
        {
            const auto active = activeEntries.find(origin->taskId);
            if (active != activeEntries.end() && active->second.origin == origin)
                activeEntries.erase(active);
        }
        araGroups.erase(found);
        return group;
    }
#endif
};

F0ExtractionService::F0ExtractionService(
    int workerCount, size_t maxQueueSize,
    std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver,
    WorkerTopology topology)
    : state_(std::make_shared<SharedState>(topology))
    , f0ServiceResolver_(std::move(f0ServiceResolver))
{
    state_->maxQueueSize = maxQueueSize;
    state_->f0ServiceResolver = f0ServiceResolver_;
    if (topology == WorkerTopology::General)
    {
        state_->dispatcherFailureMailbox = std::make_shared<SharedState::DispatcherFailureMailbox>();
        const int count = (workerCount <= 0) ? 1 : workerCount;
        for (int i = 0; i < count; ++i)
            workers_.emplace_back([state = state_]() { workerLoop(state); });
    }
    else
    {
        jassert(workerCount == 2);
#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
        workers_.emplace_back([state = state_]() { araPreparationLoop(state); });
        workers_.emplace_back([state = state_]() { araExecutionLoop(state); });
#else
        jassertfalse;
#endif
    }
}

F0ExtractionService::~F0ExtractionService()
{
    shutdown();
}

void F0ExtractionService::shutdown()
{
    std::lock_guard<std::mutex> shutdownLock(shutdownMutex_);
    if (state_->dispatcherFailureMailbox)
        state_->dispatcherFailureMailbox->cancelAndClear();
    for (const auto& worker : workers_)
    {
        if (worker.joinable() && worker.get_id() == std::this_thread::get_id())
        {
            AppLogger::error("[F0ExtractionService] shutdown called from worker thread");
            jassertfalse;
            std::terminate();
        }
    }

    std::deque<SharedState::Task> discardedTasks;
    std::unordered_map<F0TaskId, SharedState::ActiveEntry, F0TaskIdHash> discardedEntries;
#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
    std::unordered_map<F0TaskId, std::shared_ptr<SharedState::AraGroup>, F0TaskIdHash> discardedGroups;
    std::map<F0AnalysisCache::Digest, F0TaskId> discardedDigests;
#endif
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex);
        if (!state_->shutdownStarted)
        {
            state_->shutdownStarted = true;
            discardedTasks.swap(state_->queue);
            discardedEntries.swap(state_->activeEntries);
            state_->processorTaskIds.clear();
#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
            discardedGroups.swap(state_->araGroups);
            discardedDigests.swap(state_->digestGroups);
#endif
            state_->pendingSlots = 0;
            for (auto& [taskId, entry] : discardedEntries)
            {
                juce::ignoreUnused(taskId);
                entry.ownerState->closed.store(true, std::memory_order_release);
            }
        }
    }

    state_->queueCv.notify_all();

    std::shared_ptr<F0InferenceService> resolvedService;
    for (auto& [taskId, entry] : discardedEntries)
    {
        juce::ignoreUnused(taskId);
        auto service = entry.runService.lock();
        if (!service && f0ServiceResolver_)
        {
            if (!resolvedService)
                resolvedService = f0ServiceResolver_();
            service = resolvedService;
        }
        if (service)
            service->terminateActiveRun(entry.ownerState);
    }

    for (auto& worker : workers_)
        if (worker.joinable())
            worker.join();
    if (state_->dispatcherFailureMailbox)
        state_->dispatcherFailureMailbox->cancelAndClear();
}

void F0ExtractionService::SharedState::DispatcherFailureMailbox::enqueue(
    F0ExtractionService::CommitFn callback,
    std::shared_ptr<F0ExtractionService::Result> result)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(DispatcherFailure{std::move(callback), std::move(result)});
    }
    triggerAsyncUpdate();
}

void F0ExtractionService::SharedState::DispatcherFailureMailbox::cancelAndClear()
{
    cancelPendingUpdate();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void F0ExtractionService::SharedState::DispatcherFailureMailbox::handleAsyncUpdate()
{
    std::deque<DispatcherFailure> failures;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failures.swap(queue_);
    }

    for (auto& failure : failures)
    {
        try
        {
            failure.callback(std::move(*failure.result));
        }
        catch (const std::exception& e)
        {
            AppLogger::error("[F0ExtractionService] dispatcher-failure completion threw: "
                + juce::String(e.what()));
        }
        catch (...)
        {
            AppLogger::error("[F0ExtractionService] dispatcher-failure completion threw: unknown exception");
        }
    }
}

F0ExtractionService::SubmitResult F0ExtractionService::submit(
    F0RequestKey requestKey, ExecuteFn execute, CommitFn commit,
    CommitFn dispatcherFailure, uint64_t* acceptedRequestToken)
{
    if (acceptedRequestToken != nullptr)
        *acceptedRequestToken = 0;
    if (!requestKey.contentKey.isValid() || !execute || !commit)
        return SubmitResult::InvalidTask;

    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex);
        if (state_->shutdownStarted)
            return SubmitResult::ShuttingDown;
        if (state_->topology != WorkerTopology::General)
            return SubmitResult::InvalidTask;
        if (state_->processorTaskIds.find(requestKey.contentKey) != state_->processorTaskIds.end())
            return SubmitResult::AlreadyInProgress;
        if (state_->queue.size() >= state_->maxQueueSize)
            return SubmitResult::QueueFull;

        const auto taskId = allocateTaskId();
        const auto token = state_->tokenCounter++;
        if (acceptedRequestToken != nullptr)
            *acceptedRequestToken = token;
        auto ownerState = std::make_shared<F0RunOwnerState>();
        SharedState::ActiveEntry active;
        active.token = token;
        active.ownerState = ownerState;
        state_->activeEntries.emplace(taskId, std::move(active));
        state_->processorTaskIds.emplace(requestKey.contentKey, taskId);
        SharedState::Task task;
        task.taskId = taskId;
        task.requestKey = requestKey;
        task.token = token;
        task.ownerState = std::move(ownerState);
        task.execute = std::move(execute);
        task.commit = std::move(commit);
        task.dispatcherFailure = std::move(dispatcherFailure);
        state_->queue.push_back(std::move(task));
    }

    state_->queueCv.notify_one();
    return SubmitResult::Accepted;
}

#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
AraF0Submission F0ExtractionService::submit(
    AraF0DocumentFlights& flights, const AraF0LocalKey& localKey,
    AraF0Pcm pcm, AraF0Receiver receiver)
{
    if (pcm.channel0.empty() || pcm.sourceSampleRate <= 0.0 || pcm.sourceChannelCount <= 0)
        return {AraF0Admission::InvalidInput, {}};

    std::lock_guard<std::mutex> lock(state_->entriesMutex);
    if (state_->shutdownStarted)
        return {AraF0Admission::Closing, {}};
    jassert(state_->topology == WorkerTopology::AraPreparationExecution);

    const auto addReceiver = [](std::vector<AraF0Receiver>& receivers,
                                const AraF0Receiver& incoming) {
        const auto sameTarget = std::find_if(receivers.begin(), receivers.end(),
            [&incoming](const AraF0Receiver& current) {
                return current.target.wrapperGeneration == incoming.target.wrapperGeneration
                    && current.target.birthRevision == incoming.target.birthRevision;
            });
        if (sameTarget == receivers.end())
            receivers.push_back(incoming);
        else
            *sameTarget = incoming;
    };

    const auto local = flights.localTasks.find(localKey);
    if (local != flights.localTasks.end())
    {
        const auto active = state_->activeEntries.find(local->second);
        if (active != state_->activeEntries.end())
        {
            receiver.task = local->second;
            addReceiver(flights.receivers[local->second], receiver);
            addReceiver(active->second.origin->receivers, receiver);
            return {AraF0Admission::Attached, local->second};
        }
        const auto staleTask = local->second;
        flights.localTasks.erase(local);
        flights.receivers.erase(staleTask);
    }

    if (state_->pendingSlots >= state_->maxQueueSize)
        return {AraF0Admission::QueueFull, {}};

    const auto taskId = allocateTaskId();
    const auto token = state_->tokenCounter++;
    auto ownerState = std::make_shared<F0RunOwnerState>();
    auto origin = std::make_shared<SharedState::AraOrigin>();
    origin->taskId = taskId;
    origin->token = token;
    origin->submissionOrder = state_->submissionCounter++;
    origin->pcm = std::move(pcm);
    origin->ownerState = ownerState;
    receiver.task = taskId;
    origin->receivers.push_back(receiver);

    std::weak_ptr<SharedState> weakState = state_;
    std::weak_ptr<F0RunOwnerState> weakOwnerState = ownerState;
    ownerState->registerInferenceService = [weakState, weakOwnerState, taskId](
        const std::shared_ptr<F0InferenceService>& service) {
        const auto state = weakState.lock();
        const auto registeredOwner = weakOwnerState.lock();
        if (!state || !registeredOwner || !service)
            return false;
        std::lock_guard<std::mutex> registrationLock(state->entriesMutex);
        const auto entry = state->activeEntries.find(taskId);
        if (entry == state->activeEntries.end()
            || entry->second.ownerState != registeredOwner
            || registeredOwner->closed.load(std::memory_order_acquire))
            return false;
        entry->second.runService = service;
        return true;
    };

    SharedState::ActiveEntry active;
    active.token = token;
    active.ownerState = ownerState;
    active.ownsQueueSlot = true;
    active.origin = origin;
    state_->activeEntries.emplace(taskId, std::move(active));
    state_->pendingSlots++;
    flights.localTasks.emplace(localKey, taskId);
    flights.receivers[taskId].push_back(receiver);

    SharedState::Task task;
    task.taskId = taskId;
    task.token = token;
    task.phase = SharedState::Phase::Prepare;
    task.ownerState = std::move(ownerState);
    task.origin = std::move(origin);
    state_->queue.push_back(std::move(task));
    state_->queueCv.notify_all();
    return {AraF0Admission::Accepted, taskId};
}

bool F0ExtractionService::isActive(F0TaskId taskId) const
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex);
    return state_->activeEntries.find(taskId) != state_->activeEntries.end();
}

void F0ExtractionService::abortTask(F0TaskId taskId, AraF0Abort reason)
{
    juce::ignoreUnused(reason);
    std::shared_ptr<F0RunOwnerState> ownerState;
    std::shared_ptr<F0InferenceService> runService;
    std::shared_ptr<SharedState::AraGroup> retiredGroup;
    std::unordered_map<F0TaskId, SharedState::ActiveEntry, F0TaskIdHash>::node_type retiredEntry;
    F0TaskId handoffGroupId;
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex);
        const auto active = state_->activeEntries.find(taskId);
        if (active == state_->activeEntries.end())
            return;

        ownerState = active->second.ownerState;
        ownerState->closed.store(true, std::memory_order_release);
        runService = active->second.runService.lock();
        const auto origin = active->second.origin;
        const auto groupId = active->second.groupId;
        if (active->second.ownsQueueSlot)
            --state_->pendingSlots;
        retiredEntry = state_->activeEntries.extract(active);
        state_->queue.erase(std::remove_if(state_->queue.begin(), state_->queue.end(),
            [taskId](const SharedState::Task& task) { return task.taskId == taskId; }),
            state_->queue.end());

        if (groupId)
        {
            const auto groupIt = state_->araGroups.find(groupId);
            if (groupIt != state_->araGroups.end())
            {
                auto& group = *groupIt->second;
                group.origins.erase(std::remove(group.origins.begin(), group.origins.end(), origin),
                                    group.origins.end());
                if (group.ownerTaskId == taskId && !group.publicationGranted && !group.outcome)
                {
                    if (group.origins.empty())
                        retiredGroup = state_->takeAraGroupLocked(groupId);
                    else
                    {
                        group.ownerTaskId = {};
                        handoffGroupId = groupId;
                    }
                }
                else if (group.origins.empty())
                {
                    retiredGroup = state_->takeAraGroupLocked(groupId);
                }
            }
        }
    }

    if (runService)
        runService->terminateActiveRun(ownerState);

    if (handoffGroupId)
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex);
        const auto groupIt = state_->araGroups.find(handoffGroupId);
        if (!state_->shutdownStarted && groupIt != state_->araGroups.end())
        {
            auto& group = *groupIt->second;
            std::shared_ptr<SharedState::AraOrigin> successor;
            for (const auto& candidate : group.origins)
            {
                const auto candidateEntry = state_->activeEntries.find(candidate->taskId);
                if (candidateEntry == state_->activeEntries.end()
                    || candidate->ownerState->closed.load(std::memory_order_acquire))
                    continue;
                if (!successor || candidate->submissionOrder < successor->submissionOrder)
                    successor = candidate;
            }
            if (successor)
            {
                group.ownerTaskId = successor->taskId;
                SharedState::Task executeTask;
                executeTask.taskId = successor->taskId;
                executeTask.token = successor->token;
                executeTask.phase = SharedState::Phase::Execute;
                executeTask.ownerState = successor->ownerState;
                executeTask.origin = successor;
                executeTask.groupId = handoffGroupId;
                state_->queue.push_back(std::move(executeTask));
            }
            else
            {
                retiredGroup = state_->takeAraGroupLocked(handoffGroupId);
            }
        }
    }
    state_->queueCv.notify_all();
}

void F0ExtractionService::revokeReceiver(
    AraF0DocumentFlights& flights, uint64_t wrapperGeneration)
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex);
    const auto revoke = [wrapperGeneration](std::vector<AraF0Receiver>& receivers) {
        receivers.erase(std::remove_if(receivers.begin(), receivers.end(),
            [wrapperGeneration](const AraF0Receiver& receiver) {
                return receiver.target.wrapperGeneration == wrapperGeneration;
            }), receivers.end());
    };
    for (auto& [taskId, receivers] : flights.receivers)
    {
        revoke(receivers);
        const auto active = state_->activeEntries.find(taskId);
        if (active != state_->activeEntries.end() && active->second.origin)
            revoke(active->second.origin->receivers);
    }
}

void F0ExtractionService::revokeTaskReceivers(
    AraF0DocumentFlights& flights, const std::vector<F0TaskId>& taskIds)
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex);
    for (const auto taskId : taskIds)
    {
        flights.receivers.erase(taskId);
        const auto active = state_->activeEntries.find(taskId);
        if (active != state_->activeEntries.end() && active->second.origin)
            active->second.origin->receivers.clear();
    }
}

void F0ExtractionService::detachDocument(AraF0DocumentFlights& flights)
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex);
    for (const auto& [taskId, receivers] : flights.receivers)
    {
        juce::ignoreUnused(receivers);
        const auto active = state_->activeEntries.find(taskId);
        if (active != state_->activeEntries.end() && active->second.origin)
            active->second.origin->receivers.clear();
    }
    flights.receivers.clear();
    flights.localTasks.clear();
}
#endif

bool F0ExtractionService::isActive(F0RequestKey requestKey) const
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex);
    const auto task = state_->processorTaskIds.find(requestKey.contentKey);
    return task != state_->processorTaskIds.end()
        && state_->activeEntries.find(task->second) != state_->activeEntries.end();
}

void F0ExtractionService::cancel(F0RequestKey requestKey)
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex);
    const auto task = state_->processorTaskIds.find(requestKey.contentKey);
    if (task == state_->processorTaskIds.end())
        return;
    state_->activeEntries.erase(task->second);
    state_->processorTaskIds.erase(task);
}

void F0ExtractionService::workerLoop(std::shared_ptr<SharedState> state)
{
    while (true)
    {
        SharedState::Task task;
        {
            std::unique_lock<std::mutex> lock(state->entriesMutex);
            state->queueCv.wait(lock, [&state]() {
                return state->shutdownStarted
                    || std::find_if(state->queue.begin(), state->queue.end(), [](const auto& queued) {
                        return queued.phase == SharedState::Phase::General;
                    }) != state->queue.end();
            });
            if (state->shutdownStarted)
                return;

            const auto queued = std::find_if(state->queue.begin(), state->queue.end(), [](const auto& item) {
                return item.phase == SharedState::Phase::General;
            });
            task = std::move(*queued);
            state->queue.erase(queued);
            const auto active = state->activeEntries.find(task.taskId);
            if (active == state->activeEntries.end() || active->second.token != task.token)
                continue;
        }

        Result result;
        try
        {
            result = task.execute(task.ownerState);
        }
        catch (const std::exception& e)
        {
            AppLogger::error("[F0ExtractionService] Task execution threw std::exception: "
                + juce::String(e.what()));
            result.success = false;
            result.errorMessage = "F0 extraction task threw std::exception: " + std::string(e.what());
        }
        catch (...)
        {
            AppLogger::error("[F0ExtractionService] Task execution threw unknown exception");
            result.success = false;
            result.errorMessage = "F0 extraction task threw an unknown exception";
        }

        result.requestKey = task.requestKey;
        result.requestToken = task.token;

        CommitFn commit;
        CommitFn dispatcherFailure;
        bool shouldDispatch = false;
        {
            std::lock_guard<std::mutex> lock(state->entriesMutex);
            const auto active = state->activeEntries.find(task.taskId);
            if (!state->shutdownStarted && active != state->activeEntries.end()
                && active->second.token == task.token)
            {
                shouldDispatch = true;
                const auto keyed = state->processorTaskIds.find(task.requestKey.contentKey);
                if (keyed != state->processorTaskIds.end() && keyed->second == task.taskId)
                    state->processorTaskIds.erase(keyed);
                state->activeEntries.erase(active);
                commit = std::move(task.commit);
                dispatcherFailure = std::move(task.dispatcherFailure);
            }
        }

        if (!shouldDispatch)
            continue;

        auto resultForDispatch = std::make_shared<Result>(std::move(result));
        const bool posted = juce::MessageManager::callAsync(
            [commit = std::move(commit), resultForDispatch]() mutable {
                try
                {
                    commit(std::move(*resultForDispatch));
                }
                catch (const std::exception& e)
                {
                    AppLogger::error("[F0ExtractionService] completion threw: "
                        + juce::String(e.what()));
                }
                catch (...)
                {
                    AppLogger::error("[F0ExtractionService] completion threw: unknown exception");
                }
            });
        if (!posted)
        {
            if (dispatcherFailure && state->dispatcherFailureMailbox)
                state->dispatcherFailureMailbox->enqueue(
                    std::move(dispatcherFailure), std::move(resultForDispatch));
            else
                AppLogger::error("[F0ExtractionService] completion dispatcher rejected; hard lifecycle failure");
        }
    }
}

#if defined(OPENTUNE_HAS_ARA_F0_QUEUE)
void F0ExtractionService::araPreparationLoop(std::shared_ptr<SharedState> state)
{
    while (true)
    {
        SharedState::Task task;
        {
            std::unique_lock<std::mutex> lock(state->entriesMutex);
            state->queueCv.wait(lock, [&state]() {
                return state->shutdownStarted
                    || std::find_if(state->queue.begin(), state->queue.end(), [](const auto& queued) {
                        return queued.phase == SharedState::Phase::Prepare;
                    }) != state->queue.end();
            });
            if (state->shutdownStarted)
                return;
            const auto queued = std::find_if(state->queue.begin(), state->queue.end(), [](const auto& item) {
                return item.phase == SharedState::Phase::Prepare;
            });
            task = std::move(*queued);
            state->queue.erase(queued);
        }

        auto origin = task.origin;
        auto& runtime = ProcessF0Runtime::getInstance();
        if (!runtime.isReady())
            runtime.initialize(ModelPathResolver::getModelsDirectory(), &origin->initializationError);
        auto inferenceService = state->f0ServiceResolver ? state->f0ServiceResolver() : nullptr;
        if (!inferenceService)
        {
            if (origin->initializationError.empty())
                origin->initializationError = "F0 runtime has no inference service after initialization";
        }
        else
        {
            origin->config = inferenceService->snapshotExtractionConfig(true);
        }

        if (origin->config && origin->config->modelWeightDigestAvailable)
        {
            auto& key = origin->keyConfig;
            key.schemaVersion = F0AnalysisCache::kSchemaVersion;
            key.algorithmVersion = origin->config->algorithmVersion;
            key.resamplingAlgorithmVersion = origin->config->resamplingAlgorithmVersion;
            key.f0AlgorithmVersion = origin->config->algorithmVersion;
            key.energyAlgorithmVersion = kFrameEnergyAlgorithmVersion;
            key.modelType = static_cast<int>(origin->config->modelType);
            key.modelWeightDigest = origin->config->modelWeightDigest;
            key.confidence = origin->config->confidence;
            key.f0Min = origin->config->f0Min;
            key.f0Max = origin->config->f0Max;
            key.backend = origin->config->backend;
            key.sourceSampleRate = origin->pcm.sourceSampleRate;
            key.sourceChannelCount = origin->pcm.sourceChannelCount;
            key.channelPolicyVersion = F0AnalysisCache::kChannelZeroPolicyVersion;
            key.windowSampleCount = origin->pcm.channel0.size();
            origin->pcmDigest = F0AnalysisCache::digestPcm(
                origin->pcm.channel0.data(), origin->pcm.channel0.size());
            origin->digest = F0AnalysisCache::buildKey(key, origin->pcmDigest);
            origin->hasTrustedDigest = true;
        }

        bool joined = false;
        {
            std::lock_guard<std::mutex> lock(state->entriesMutex);
            const auto active = state->activeEntries.find(task.taskId);
            if (state->shutdownStarted || active == state->activeEntries.end()
                || active->second.token != task.token
                || origin->ownerState->closed.load(std::memory_order_acquire))
                continue;

            std::shared_ptr<SharedState::AraGroup> group;
            if (origin->hasTrustedDigest)
            {
                const auto matching = state->digestGroups.find(origin->digest);
                if (matching != state->digestGroups.end())
                {
                    const auto existing = state->araGroups.find(matching->second);
                    if (existing != state->araGroups.end())
                    {
                        group = existing->second;
                        group->origins.push_back(origin);
                        active->second.groupId = group->groupId;
                        active->second.ownsQueueSlot = false;
                        --state->pendingSlots;
                        joined = true;
                    }
                }
            }

            if (!group)
            {
                group = std::make_shared<SharedState::AraGroup>();
                group->groupId = task.taskId;
                group->ownerTaskId = task.taskId;
                group->hasDigest = origin->hasTrustedDigest;
                group->digest = origin->digest;
                group->origins.push_back(origin);
                active->second.groupId = group->groupId;
                active->second.ownsQueueSlot = false;
                state->araGroups.emplace(group->groupId, group);
                if (group->hasDigest)
                    state->digestGroups.emplace(group->digest, group->groupId);
            }

            if (!joined)
            {
                SharedState::Task executeTask;
                executeTask.taskId = task.taskId;
                executeTask.token = task.token;
                executeTask.phase = SharedState::Phase::Execute;
                executeTask.ownerState = task.ownerState;
                executeTask.origin = std::move(origin);
                executeTask.groupId = group->groupId;
                state->queue.push_back(std::move(executeTask));
            }
        }
        state->queueCv.notify_all();
    }
}

void F0ExtractionService::araExecutionLoop(std::shared_ptr<SharedState> state)
{
    while (true)
    {
        SharedState::Task task;
        {
            std::unique_lock<std::mutex> lock(state->entriesMutex);
            state->queueCv.wait(lock, [&state]() {
                return state->shutdownStarted
                    || std::find_if(state->queue.begin(), state->queue.end(), [](const auto& queued) {
                        return queued.phase == SharedState::Phase::Execute;
                    }) != state->queue.end();
            });
            if (state->shutdownStarted)
                return;
            const auto queued = std::find_if(state->queue.begin(), state->queue.end(), [](const auto& item) {
                return item.phase == SharedState::Phase::Execute;
            });
            task = std::move(*queued);
            state->queue.erase(queued);
            const auto group = state->araGroups.find(task.groupId);
            const auto active = state->activeEntries.find(task.taskId);
            if (group == state->araGroups.end() || active == state->activeEntries.end()
                || group->second->ownerTaskId != task.taskId
                || active->second.token != task.token)
                continue;
        }

        auto origin = task.origin;
        Result result;
        std::string actualBackend;
        F0AnalysisCache::KeyConfig publishKey = origin->keyConfig;
        bool cacheHit = false;
        try
        {
            if (origin->ownerState->closed.load(std::memory_order_acquire))
            {
                result.errorMessage = "ARA F0 owner lease was closed before execution";
            }
            else if (!origin->config)
            {
                result.errorMessage = origin->initializationError.empty()
                    ? "F0 extraction configuration is unavailable"
                    : origin->initializationError;
            }
            else
            {
                auto inferenceService = state->f0ServiceResolver ? state->f0ServiceResolver() : nullptr;
                if (!inferenceService)
                {
                    result.errorMessage = "F0 runtime has no inference service after preparation";
                }
                else
                {
                    const bool cacheEnabled = AppPreferences::readAraF0AnalysisCacheEnabledFromDisk();
                    const auto keyHex = origin->hasTrustedDigest
                        ? F0AnalysisCache::keyToHex(origin->digest)
                        : std::string("unavailable");
                    if (cacheEnabled && origin->hasTrustedDigest)
                    {
                        if (auto cached = F0AnalysisCache::tryLoad(
                                F0AnalysisCache::cacheDirectory(), origin->digest))
                        {
                            F0AnalysisCache::touch(F0AnalysisCache::cacheDirectory(), origin->digest);
                            result.success = true;
                            result.hopSize = cached->hopSize;
                            result.f0SampleRate = static_cast<int>(cached->f0SampleRate);
                            result.f0 = std::move(cached->originalF0);
                            result.energy = std::move(cached->energy);
                            cacheHit = true;
                            AppLogger::log("ARA-F0 cache_hit key="
                                + juce::String(keyHex.substr(0, 16)));
                        }
                    }

                    if (!result.success)
                    {
                        if (!origin->ownerState->registerInferenceService
                            || !origin->ownerState->registerInferenceService(inferenceService))
                        {
                            result.errorMessage = "ARA F0 owner lease closed before inference admission";
                        }
                        else
                        {
                            const auto& config = *origin->config;
                            const auto configurationSummary = "model="
                                + std::to_string(static_cast<int>(config.modelType))
                                + " backend=" + config.backend
                                + " confidence=" + std::to_string(config.confidence)
                                + " f0Min=" + std::to_string(config.f0Min)
                                + " f0Max=" + std::to_string(config.f0Max);
                            AppLogger::log("ARA-F0 inference_start key="
                                + juce::String(origin->hasTrustedDigest
                                    ? keyHex.substr(0, 16)
                                    : cacheEnabled ? "unavailable" : "disabled")
                                + " " + juce::String(configurationSummary));

                            int hopSize = 0;
                            int f0SampleRate = 0;
                            auto extraction = inferenceService->extractF0ForAnalysis(
                                origin->pcm.channel0.data(), origin->pcm.channel0.size(),
                                static_cast<int>(origin->pcm.sourceSampleRate), origin->ownerState,
                                &config, hopSize, f0SampleRate, actualBackend);
                            if (!extraction.ok())
                            {
                                result.errorMessage = extraction.error().fullMessage();
                            }
                            else if (extraction.value().empty())
                            {
                                result.errorMessage = "F0 inference returned an empty result";
                            }
                            else
                            {
                                if (origin->hasTrustedDigest && publishKey.backend != actualBackend)
                                {
                                    publishKey.backend = actualBackend;
                                }
                                result.success = true;
                                result.f0 = extraction.value();
                                result.hopSize = hopSize;
                                result.f0SampleRate = f0SampleRate;
                                result.energy = computeFrameEnergy(
                                    origin->pcm.channel0.data(),
                                    static_cast<int>(origin->pcm.channel0.size()),
                                    static_cast<int>(origin->pcm.sourceSampleRate),
                                    result.f0, f0SampleRate, hopSize);
                            }
                        }
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            result.success = false;
            result.errorMessage = "ARA F0 execution threw std::exception: " + std::string(e.what());
            AppLogger::error("[F0ExtractionService] ARA execution threw: " + juce::String(e.what()));
        }
        catch (...)
        {
            result.success = false;
            result.errorMessage = "ARA F0 execution threw an unknown exception";
            AppLogger::error("[F0ExtractionService] ARA execution threw an unknown exception");
        }

        bool publicationGranted = false;
        {
            std::lock_guard<std::mutex> lock(state->entriesMutex);
            const auto group = state->araGroups.find(task.groupId);
            const auto active = state->activeEntries.find(task.taskId);
            if (!state->shutdownStarted && group != state->araGroups.end()
                && group->second->ownerTaskId == task.taskId
                && active != state->activeEntries.end()
                && active->second.token == task.token
                && !origin->ownerState->closed.load(std::memory_order_acquire))
            {
                group->second->publicationGranted = true;
                group->second->publisherTaskId = task.taskId;
                publicationGranted = true;
            }
        }
        if (!publicationGranted)
            continue;

        F0AnalysisCache::Digest publishDigest = origin->digest;
        if (origin->hasTrustedDigest && origin->config && result.success && !cacheHit
            && AppPreferences::readAraF0AnalysisCacheEnabledFromDisk())
        {
            const auto currentModelDigest = ModelFactory::getModelWeightDigest(origin->config->modelPath);
            if (currentModelDigest.has_value()
                && *currentModelDigest == origin->config->modelWeightDigest)
            {
                if (publishKey.backend != origin->config->backend)
                    publishDigest = F0AnalysisCache::buildKey(publishKey, origin->pcmDigest);
                F0AnalysisCache::Record record;
                record.hopSize = result.hopSize;
                record.f0SampleRate = static_cast<double>(result.f0SampleRate);
                record.originalF0 = result.f0;
                record.energy = result.energy;
                F0AnalysisCache::publish(F0AnalysisCache::cacheDirectory(), publishDigest, record);
            }
            else
            {
                AppLogger::log("ARA-F0 cache_publish_skipped key="
                    + juce::String(F0AnalysisCache::keyToHex(origin->digest).substr(0, 16))
                    + " reason=model_digest_changed");
            }
        }

        auto outcome = std::make_shared<const Result>(std::move(result));
        bool shouldDispatch = false;
        std::shared_ptr<SharedState::AraGroup> retiredGroup;
        {
            std::lock_guard<std::mutex> lock(state->entriesMutex);
            const auto group = state->araGroups.find(task.groupId);
            if (group != state->araGroups.end()
                && group->second->publisherTaskId == task.taskId
                && group->second->publicationGranted)
            {
                group->second->outcome = std::move(outcome);
                shouldDispatch = std::any_of(group->second->origins.begin(), group->second->origins.end(),
                    [](const auto& candidate) { return !candidate->receivers.empty(); });
                if (!shouldDispatch)
                    retiredGroup = state->takeAraGroupLocked(task.groupId);
            }
        }
        if (shouldDispatch)
        {
            const std::weak_ptr<SharedState> weakState = state;
            const bool posted = juce::MessageManager::callAsync([weakState, groupId = task.groupId]() {
                if (auto sharedState = weakState.lock())
                    F0ExtractionService::deliverAraGroup(std::move(sharedState), groupId);
            });
            if (!posted)
            {
                AppLogger::log("ARA-F0 completion dispatcher rejected");
                releaseAraGroup(state, task.groupId);
            }
        }
    }
}

void F0ExtractionService::deliverAraGroup(
    std::shared_ptr<SharedState> state, F0TaskId groupId)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    for (;;)
    {
        std::weak_ptr<AraF0CompletionSink> sink;
        F0TaskId taskId;
        SharedAraF0Outcome outcome;
        std::shared_ptr<SharedState::AraGroup> retiredGroup;
        {
            std::lock_guard<std::mutex> lock(state->entriesMutex);
            const auto found = state->araGroups.find(groupId);
            if (state->shutdownStarted || found == state->araGroups.end())
                return;
            auto& group = *found->second;
            outcome = group.outcome;
            for (const auto& origin : group.origins)
            {
                if (origin->deliveryClaimed || origin->receivers.empty())
                    continue;
                taskId = origin->taskId;
                sink = origin->receivers.front().sink;
                origin->deliveryClaimed = true;
                break;
            }
            if (!taskId)
                retiredGroup = state->takeAraGroupLocked(groupId);
        }
        if (retiredGroup)
            return;
        if (taskId)
        {
            if (auto completionSink = sink.lock())
                completionSink->complete(taskId, std::move(outcome));
            continue;
        }
        return;
    }
}

void F0ExtractionService::releaseAraGroup(
    std::shared_ptr<SharedState> state, F0TaskId groupId)
{
    std::shared_ptr<SharedState::AraGroup> retiredGroup;
    {
        std::lock_guard<std::mutex> lock(state->entriesMutex);
        retiredGroup = state->takeAraGroupLocked(groupId);
    }
}
#endif

} // namespace OpenTune
