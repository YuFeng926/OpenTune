#include "F0ExtractionService.h"
#include "../Inference/ModelFactory.h"
#include "../Runtime/ProcessF0Runtime.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include "../Utils/AppLogger.h"
#include "../Utils/ModelPathResolver.h"
#include "ImportedClipF0Extraction.h"
#include <juce_cryptography/juce_cryptography.h>
#include <juce_events/juce_events.h>
#include <algorithm>
#include <exception>
#include <limits>
#include <optional>

namespace OpenTune {

F0ExtractionService::F0ExtractionService(int workerCount, size_t maxQueueSize,
                                          std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver)
    : state_(std::make_shared<SharedState>())
    , f0ServiceResolver_(std::move(f0ServiceResolver))
{
    state_->maxQueueSize_ = maxQueueSize;
    state_->processorRunOwner_ = std::make_shared<F0RunOwnerState>();
    state_->dispatcherFailureMailbox_ = std::make_shared<SharedState::DispatcherFailureMailbox>();
    const int count = std::max(1, workerCount);
    workers_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        workers_.emplace_back([state = state_] { processorWorkerLoop(state); });
}

F0ExtractionService::F0ExtractionService(size_t maxQueueSize)
    : state_(std::make_shared<SharedState>())
{
    state_->maxQueueSize_ = maxQueueSize;
    workers_.reserve(2);
    workers_.emplace_back([state = state_] { prepareAraTasks(state); });
    workers_.emplace_back([state = state_] { executeAraTasks(state); });
}

F0ExtractionService::~F0ExtractionService()
{
    shutdown();
}

void F0ExtractionService::shutdown()
{
    std::call_once(shutdownOnce_, [this] {
        auto& state = *state_;
        if (state.dispatcherFailureMailbox_)
            state.dispatcherFailureMailbox_->cancelAndClear();
        for (const auto& worker : workers_) {
            if (worker.get_id() == std::this_thread::get_id()) {
                AppLogger::error("[F0ExtractionService] shutdown called from worker thread");
                jassertfalse;
                std::terminate();
            }
        }

        std::deque<Job> discardedTasks;
        ProcessorEntries discardedProcessorEntries;
        std::set<F0TaskId> araTasks;
        {
            std::lock_guard<std::mutex> lock(state.entriesMutex_);
            state.shutdownStarted_ = true;
            // Revoke delivery before detaching queued work. Any undispatched
            // result is released as a value without calling its owner.
            discardedProcessorEntries.swap(state.processorEntries_);
            for (auto& [id, entry] : state.araEntries_) {
                entry.sink.reset();
                araTasks.insert(id);
            }
            discardedTasks.swap(state.queue_);
            if (state.processorRunOwner_)
                state.processorRunOwner_->closed.store(true, std::memory_order_release);
        }

        abortTasks(araTasks, AraF0Abort::Orphaned);
        if (f0ServiceResolver_) {
            if (auto svc = f0ServiceResolver_())
                svc->terminateActiveRun(state.processorRunOwner_);
        }

        state.queueCv_.notify_all();
        for (auto& worker : workers_)
            worker.join();
        workers_.clear();
        if (state.dispatcherFailureMailbox_)
            state.dispatcherFailureMailbox_->cancelAndClear();
    });
}

void F0ExtractionService::SharedState::DispatcherFailureMailbox::enqueue(
    CommitFn callback, std::shared_ptr<Result> result)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.emplace_back(std::move(callback), std::move(result));
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
    std::deque<std::pair<CommitFn, std::shared_ptr<Result>>> failures;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failures.swap(queue_);
    }

    for (auto& [callback, result] : failures) {
        try {
            callback(std::move(*result));
        } catch (const std::exception& e) {
            AppLogger::error("[F0ExtractionService] dispatcher-failure completion threw: "
                + juce::String(e.what()));
        } catch (...) {
            AppLogger::error("[F0ExtractionService] dispatcher-failure completion threw: unknown exception");
        }
    }
}

F0ExtractionService::SubmitResult F0ExtractionService::submit(F0RequestKey requestKey, ExecuteFn execute,
                                                              CommitFn commit, CommitFn dispatcherFailure)
{
    if (!requestKey.contentKey.isValid() || !execute || !commit) {
        return SubmitResult::InvalidTask;
    }

    // 单一临界区：检查 shutdown、查重、容量检查、分配 token、插入 active、enqueue。
    // 队列满直接 QueueFull，不创建 active 条目。与 shutdown 线性化。
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex_);
        if (state_->shutdownStarted_)
            return SubmitResult::ShuttingDown;

        if (state_->processorEntries_.find(requestKey.contentKey) != state_->processorEntries_.end()) {
            return SubmitResult::AlreadyInProgress;
        }

        if (state_->queue_.size() >= state_->maxQueueSize_) {
            return SubmitResult::QueueFull;
        }

        const uint64_t token = state_->tokenCounter_++;
        state_->processorEntries_.emplace(requestKey.contentKey,
            ProcessorEntry{token, std::move(commit), std::move(dispatcherFailure), {}});
        state_->queue_.emplace_back(Task{{requestKey, token}, std::move(execute)});
    }

    // 解锁后唤醒单个 worker。
    state_->queueCv_.notify_one();
    return SubmitResult::Accepted;
}

void F0ExtractionService::cancel(F0RequestKey requestKey)
{
    ProcessorEntries::node_type discarded;
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex_);
        discarded = state_->processorEntries_.extract(requestKey.contentKey);
    }
}

bool AraF0ConfigTag::operator==(const AraF0ConfigTag& rhs) const noexcept
{
    const auto& a = extraction;
    const auto& b = rhs.extraction;
    // Initial automatic detection preserves its policy revision across cold startup.
    return model == rhs.model && sourceSampleRate == rhs.sourceSampleRate
        && a.selectionRevision == b.selectionRevision && a.modelType == b.modelType
        && a.confidence == b.confidence && a.f0Min == b.f0Min && a.f0Max == b.f0Max
        && a.backend.revision == b.backend.revision;
}

F0TaskId AraF0DocumentFlights::taskForReceiver(uint64_t wrapperGeneration) const
{
    for (const auto& [id, receivers] : byTaskId)
        if (receivers.find(wrapperGeneration) != receivers.end())
            return id;
    return {};
}

AraF0DocumentFlights::Receivers AraF0DocumentFlights::take(F0TaskId taskId)
{
    auto entry = byTaskId.extract(taskId);
    if (entry.empty()) // A previously invoked document sink can revoke this flight during delivery.
        return {};
    const auto key = std::find_if(byLocalKey.begin(), byLocalKey.end(),
        [taskId](const auto& item) { return item.second == taskId; });
    jassert(key != byLocalKey.end());
    byLocalKey.erase(key);
    for (auto it = bySource.begin(); it != bySource.end(); ++it)
    {
        if (it->second.erase(taskId) > 0)
        {
            if (it->second.empty())
                bySource.erase(it);
            break;
        }
    }
    return std::move(entry.mapped());
}

AraF0ConfigTag F0ExtractionService::snapshotAraConfig(double sourceSampleRate)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto& runtime = ProcessF0Runtime::getInstance();
    F0ExtractionConfigSnapshot config;
    if (runtime.isReady())
        config = runtime.getF0Service()->snapshotExtractionConfig();
    else {
        config.modelDirectory = ModelPathResolver::getModelsDirectory();
        config.backend = AccelerationDetector::getInstance().getSelection();
    }
    auto model = ModelFactory::resolveF0ModelVersion(config.modelType, config.modelDirectory);
    return {std::move(model), std::move(config), sourceSampleRate};
}

AraF0Submission F0ExtractionService::submit(AraF0DocumentFlights& flights,
                                            const AraF0LocalKey& key,
                                            AraF0Pcm pcm,
                                            AraF0Receiver receiver)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (!flights.sink)
        return {AraF0Admission::Closing, {}};
    if (!receiver.key.isValid() || pcm.channel0.empty()
        || pcm.channel0.size() > static_cast<size_t>(std::numeric_limits<int>::max())
        || !(pcm.sourceSampleRate >= 1.0 && pcm.sourceSampleRate <= std::numeric_limits<int>::max())
        || pcm.sourceChannelCount <= 0)
        return {AraF0Admission::InvalidInput, {}};

    const auto local = std::find_if(flights.byLocalKey.begin(), flights.byLocalKey.end(),
        [&key](const auto& item) { return item.first == key; });
    if (local != flights.byLocalKey.end()) {
        const auto id = local->second;
        {
            std::lock_guard<std::mutex> lock(state_->entriesMutex_);
            const auto entry = state_->araEntries_.find(id);
            if (!state_->shutdownStarted_ && entry != state_->araEntries_.end()) {
                flights.byTaskId.at(id).insert_or_assign(receiver.wrapperGeneration, std::move(receiver));
                entry->second.hasReceivers = true;
                return {AraF0Admission::Attached, id};
            }
        }
        // A dispatcher rejection releases the service entry without touching the document.
        flights.take(id);
    }

    F0TaskId id;
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex_);
        if (state_->shutdownStarted_)
            return {AraF0Admission::Closing, {}};
        if (state_->queue_.size() >= state_->maxQueueSize_)
            return {AraF0Admission::QueueFull, {}};

        id = {state_->tokenCounter_++};
        jassert(id.value != 0);
        auto task = std::make_shared<const AraTask>(AraTask{id, std::move(pcm), key.config,
            std::make_shared<F0RunOwnerState>()});
        state_->araEntries_.emplace(id, AraEntry{task, flights.sink, {}, nullptr});
        state_->queue_.emplace_back(std::move(task));
    }
    flights.byLocalKey.emplace_back(key, id);
    flights.byTaskId[id].emplace(receiver.wrapperGeneration, std::move(receiver));
    state_->queueCv_.notify_all();
    return {AraF0Admission::Accepted, id};
}

bool F0ExtractionService::isActive(F0TaskId taskId) const
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex_);
    return !state_->shutdownStarted_ && state_->araEntries_.find(taskId) != state_->araEntries_.end();
}

void F0ExtractionService::abortTask(F0TaskId taskId, AraF0Abort reason)
{
    abortTasks({taskId}, reason);
}

void F0ExtractionService::abortTasks(const std::set<F0TaskId>& taskIds, AraF0Abort reason)
{
    if (taskIds.empty())
        return;

    std::vector<AraEntry> revoked;
    std::vector<AraGroups::node_type> discardedGroups;
    std::vector<std::unique_ptr<const AraF0Result>> discardedResults;
    bool promoted = false;
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex_);
        auto& entries = state_->araEntries_;
        auto& queue = state_->queue_;
        const auto close = [&](F0TaskId id) {
            auto& origin = revoked.emplace_back(std::move(entries.extract(id).mapped()));
            origin.task->owner->closed.store(true, std::memory_order_release);
        };
        // Mark the complete revoked scope before looking for a successor.
        for (auto id : taskIds)
            if (const auto entry = entries.find(id); entry != entries.end())
                entry->second.hasReceivers = false;

        for (auto id : taskIds) {
            const auto entry = entries.find(id);
            if (entry == entries.end())
                continue;
            if (!entry->second.group) {
                close(id);
                continue;
            }

            const auto groupIt = *entry->second.group;
            auto& group = groupIt->second;
            if (group.phase == AraGroupPhase::Delivering) {
                group.origins.erase(id);
                close(id);
                continue;
            }

            const auto next = std::find_if(group.origins.begin(), group.origins.end(), [&](F0TaskId candidate) {
                return entries.at(candidate).hasReceivers;
            });
            if (next == group.origins.end()) {
                for (auto origin : group.origins)
                    close(origin);
                discardedGroups.push_back(state_->araGroups_.extract(groupIt));
                continue;
            }

            // Removing the owner's last local receiver leaves its PCM lease serving the other origins.
            if (group.owner == id && reason == AraF0Abort::Orphaned && !state_->shutdownStarted_)
                continue;

            group.origins.erase(id);
            close(id);
            if (!(group.owner == id) || state_->shutdownStarted_)
                continue;

            group.owner = *next;
            discardedResults.push_back(std::move(group.result));
            if (group.phase == AraGroupPhase::Queued) {
                const auto slot = std::find_if(queue.begin(), queue.end(), [id](const Job& job) {
                    return std::get<AraJob>(job)->id == id;
                });
                *slot = entries.at(*next).task;
            } else {
                // Reuse the executing group's admission on handoff.
                group.phase = AraGroupPhase::Resuming;
            }
            promoted = true;
        }
        queue.erase(std::remove_if(queue.begin(), queue.end(), [&entries](const Job& job) {
            return entries.find(std::get<AraJob>(job)->id) == entries.end();
        }), queue.end());
    }

    // The closed state rejects a late lease. A registered lease is terminated outside the queue lock.
    static constexpr const char* reasons[] = {"SamplesDisabled", "SourceUndo", "SamplesChanged", "Orphaned"};
    for (const auto& origin : revoked) {
        if (auto* inference = origin.inference)
            inference->terminateActiveRun(origin.task->owner);
        AppLogger::logNoThrow("ARA-F0: task=" + juce::String(static_cast<juce::uint64>(origin.task->id.value))
            + " abort_requested reason=" + reasons[static_cast<size_t>(reason)]);
    }
    if (promoted)
        state_->queueCv_.notify_all();
}

void F0ExtractionService::revokeReceiver(AraF0DocumentFlights& flights, uint64_t wrapperGeneration)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto id = flights.taskForReceiver(wrapperGeneration);
    if (id.value == 0)
        return;
    auto& receivers = flights.byTaskId.at(id);
    receivers.erase(wrapperGeneration);
    if (receivers.empty()) {
        abortTask(id, AraF0Abort::Orphaned);
        // Keep the source lookup until an orphaned owner finishes serving its peers.
        if (!isActive(id))
            flights.take(id);
    }
}

template <typename Id>
void F0ExtractionService::postCompletion(const std::shared_ptr<SharedState>& state, Id id)
{
    ProcessRenderRuntime::postAsyncCallback([weak = std::weak_ptr<SharedState>(state), id](bool dispatched) {
        const auto retained = weak.lock();
        if (!retained)
            return;
        if (dispatched) {
            auto* manager = juce::MessageManager::getInstanceWithoutCreating();
            jassert(manager->isThisTheMessageThread());
            dispatched = !manager->hasStopMessageBeenSent();
        }
        settleCompletion(*retained, id, dispatched);
    });
}

F0ExtractionService::ProcessorEntries::iterator F0ExtractionService::findProcessorEntry(
    SharedState& state, ProcessorTaskId id)
{
    const auto it = state.processorEntries_.find(id.requestKey.contentKey);
    return it != state.processorEntries_.end() && it->second.token == id.token
        ? it : state.processorEntries_.end();
}

void F0ExtractionService::processorWorkerLoop(const std::shared_ptr<SharedState>& state)
{
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(state->entriesMutex_);
            state->queueCv_.wait(lock, [&] { return state->shutdownStarted_ || !state->queue_.empty(); });
            if (state->shutdownStarted_)
                return;
            task = std::move(std::get<Task>(state->queue_.front()));
            state->queue_.pop_front();
            if (findProcessorEntry(*state, task.id) == state->processorEntries_.end())
                continue;
        }
        runProcessorTask(state, std::move(task));
    }
}

void F0ExtractionService::runProcessorTask(const std::shared_ptr<SharedState>& state, Task task)
{
    Result result;
    try {
        result = task.execute(state->processorRunOwner_);
    } catch (const std::exception& e) {
        AppLogger::error("[F0ExtractionService] Task execution threw std::exception: " + juce::String(e.what()));
        result.errorMessage = "F0 extraction task threw std::exception: " + std::string(e.what());
    } catch (...) {
        AppLogger::error("[F0ExtractionService] Task execution threw unknown exception");
        result.errorMessage = "F0 extraction task threw an unknown exception";
    }
    {
        std::lock_guard<std::mutex> lock(state->entriesMutex_);
        const auto it = findProcessorEntry(*state, task.id);
        if (it == state->processorEntries_.end())
            return;
        it->second.result.emplace(std::move(result));
    }
    postCompletion(state, task.id);
}

void F0ExtractionService::settleCompletion(SharedState& state, ProcessorTaskId id, bool dispatcherAvailable)
{
    ProcessorEntries::node_type batch;
    {
        std::lock_guard<std::mutex> lock(state.entriesMutex_);
        const auto it = findProcessorEntry(state, id);
        if (it == state.processorEntries_.end())
            return;
        batch = state.processorEntries_.extract(it);
    }
    if (!dispatcherAvailable) {
        // The processor mailbox re-delivers the failed result on the message
        // thread so waiting content settles as Failed with its reason.
        if (state.dispatcherFailureMailbox_ && batch.mapped().dispatcherFailure) {
            state.dispatcherFailureMailbox_->enqueue(std::move(batch.mapped().dispatcherFailure),
                std::make_shared<Result>(std::move(*batch.mapped().result)));
            return;
        }
        AppLogger::logNoThrow("[F0ExtractionService] completion released: dispatcher unavailable task="
            + juce::String(static_cast<juce::uint64>(id.token)));
        return;
    }

    auto& entry = batch.mapped();
    try {
        entry.commit(std::move(*entry.result));
    } catch (const std::exception& e) {
        AppLogger::error("[F0ExtractionService] completion threw: " + juce::String(e.what()));
    } catch (...) {
        AppLogger::error("[F0ExtractionService] completion threw: unknown exception");
    }
}

void F0ExtractionService::prepareAraTasks(const std::shared_ptr<SharedState>& state)
{
    for (;;) {
        AraJob task;
        {
            std::unique_lock<std::mutex> lock(state->entriesMutex_);
            auto next = state->queue_.end();
            state->queueCv_.wait(lock, [&] {
                if (state->shutdownStarted_)
                    return true;
                // The ready owners form a prefix of the same admission queue.
                next = std::find_if(state->queue_.begin(), state->queue_.end(), [&](const Job& job) {
                    return !state->araEntries_.at(std::get<AraJob>(job)->id).group;
                });
                return next != state->queue_.end();
            });
            if (state->shutdownStarted_)
                return;
            task = std::get<AraJob>(*next);
        }

        auto& runtime = ProcessF0Runtime::getInstance();
        auto frozen = task->config;
        auto& config = frozen.extraction;
        std::string initializationError;
        const bool ready = runtime.isReady()
            || runtime.initialize(config.modelDirectory, config.modelType, &initializationError);
        const auto inference = ready ? runtime.getF0Service() : nullptr;
        if (task->owner->closed.load(std::memory_order_acquire))
            continue;
        if (ready && !config.backend.detected)
            config.backend = AccelerationDetector::getInstance().resolveAutomaticSelection();
        const auto& data = task->pcm.channel0;
        auto pcmHash = juce::SHA256(data.data(), data.size() * sizeof(float)).toHexString();

        {
            std::lock_guard<std::mutex> lock(state->entriesMutex_);
            const auto entry = state->araEntries_.find(task->id);
            if (state->shutdownStarted_ || entry == state->araEntries_.end())
                continue;
            auto [first, last] = state->araGroups_.equal_range(pcmHash);
            auto group = std::find_if(first, last, [&frozen](const auto& item) {
                return item.second.phase != AraGroupPhase::Delivering && item.second.config == frozen;
            });
            if (group != last) {
                entry->second.group = group;
                group->second.origins.insert(task->id);
                // Only the owner takes an execution slot. Followers retain value PCM for handoff.
                const auto queued = std::find_if(state->queue_.begin(), state->queue_.end(), [&task](const Job& job) {
                    return std::get<AraJob>(job) == task;
                });
                state->queue_.erase(queued);
                continue;
            }
            group = state->araGroups_.emplace(std::move(pcmHash), AraGroup{std::move(frozen), task->id,
                {task->id}, inference.get(), std::move(initializationError), {}});
            entry->second.group = group;
        }
        state->queueCv_.notify_all();
    }
}

void F0ExtractionService::executeAraTasks(const std::shared_ptr<SharedState>& state)
{
    for (;;) {
        AraJob task;
        AraF0ConfigTag frozen;
        F0InferenceService* inference;
        std::string initializationError;
        {
            std::unique_lock<std::mutex> lock(state->entriesMutex_);
            auto group = state->araGroups_.end();
            state->queueCv_.wait(lock, [&] {
                if (state->shutdownStarted_)
                    return true;
                group = std::find_if(state->araGroups_.begin(), state->araGroups_.end(), [](const auto& item) {
                    return item.second.phase == AraGroupPhase::Resuming;
                });
                if (group != state->araGroups_.end())
                    return true;
                if (state->queue_.empty())
                    return false;
                const auto& front = state->araEntries_.at(std::get<AraJob>(state->queue_.front())->id);
                if (!front.group)
                    return false;
                group = *front.group;
                return true;
            });
            if (state->shutdownStarted_)
                return;

            auto& batch = group->second;
            auto& origin = state->araEntries_.at(batch.owner);
            task = origin.task;
            if (batch.phase == AraGroupPhase::Queued) {
                jassert(std::get<AraJob>(state->queue_.front()) == task);
                state->queue_.pop_front();
            }
            batch.phase = AraGroupPhase::Running;
            origin.inference = inference = batch.inference;
            frozen = batch.config;
            initializationError = batch.initializationError;
        }
        runAraTask(state, *task, frozen, inference, std::move(initializationError));
    }
}

void F0ExtractionService::runAraTask(const std::shared_ptr<SharedState>& state, const AraTask& task,
                                     const AraF0ConfigTag& frozen, F0InferenceService* inference,
                                     std::string initializationError)
{
    auto result = std::make_unique<AraF0Result>();
    result->errorMessage = std::move(initializationError);
    if (inference != nullptr) {
        try {
            executeAraTask(*inference, task, frozen, *result);
        } catch (const std::exception& e) {
            result->errorMessage = e.what();
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->entriesMutex_);
        const auto entry = state->araEntries_.find(task.id);
        if (state->shutdownStarted_ || entry == state->araEntries_.end())
            return; // Revocation removes the old owner ID before a successor is queued.
        entry->second.inference = nullptr;
        auto& batch = (*entry->second.group)->second;
        batch.result = std::move(result);
        batch.phase = AraGroupPhase::ReadyToDeliver;
    }

    postCompletion(state, task.id);
}

F0ExtractionService::AraGroups::iterator F0ExtractionService::findAraBatch(SharedState& state, F0TaskId owner)
{
    const auto entry = state.araEntries_.find(owner);
    if (state.shutdownStarted_ || entry == state.araEntries_.end())
        return state.araGroups_.end();

    const auto group = *entry->second.group;
    jassert(group->second.owner == owner && group->second.phase == AraGroupPhase::ReadyToDeliver);
    return group;
}

F0ExtractionService::AraGroups::node_type F0ExtractionService::releaseAraBatch(
    SharedState& state, AraGroups::iterator group, std::vector<AraEntry>& released)
{
    released.reserve(group->second.origins.size());
    for (auto id : group->second.origins)
        released.push_back(std::move(state.araEntries_.extract(id).mapped()));
    return state.araGroups_.extract(group);
}

void F0ExtractionService::settleCompletion(SharedState& state, F0TaskId owner, bool dispatcherAvailable)
{
    using Sink = std::weak_ptr<AraF0DocumentFlights::CompletionSink>;
    std::map<Sink, std::vector<F0TaskId>, std::owner_less<Sink>> byDocument;
    AraGroups::iterator group;
    std::unique_ptr<const AraF0Result> result;
    std::vector<AraEntry> released;
    AraGroups::node_type releasedGroup;
    {
        std::lock_guard<std::mutex> lock(state.entriesMutex_);
        group = findAraBatch(state, owner);
        if (group == state.araGroups_.end())
            return;
        if (dispatcherAvailable) {
            auto& batch = group->second;
            for (auto id : batch.origins)
                byDocument[state.araEntries_.at(id).sink].push_back(id);
            // Close Digest admission at handoff. Local flights remain active until their sink takes them.
            batch.phase = AraGroupPhase::Delivering;
            result = std::move(batch.result);
        } else {
            releasedGroup = releaseAraBatch(state, group, released);
        }
    }

    if (!dispatcherAvailable) {
        AppLogger::logNoThrow("ARA-F0: completion released: dispatcher unavailable task="
            + juce::String(static_cast<juce::uint64>(owner.value)));
        return;
    }

    const juce::ScopeGuard finish{[&] {
        std::lock_guard<std::mutex> lock(state.entriesMutex_);
        releasedGroup = releaseAraBatch(state, group, released);
    }};
    for (const auto& [receiver, ids] : byDocument)
        if (const auto sink = receiver.lock())
            (*sink)(ids, *result);
}

void F0ExtractionService::executeAraTask(F0InferenceService& inference, const AraTask& task,
                                        const AraF0ConfigTag& frozen, AraF0Result& result)
{
    const auto& config = frozen.extraction;
    const auto& pcm = task.pcm;
    const auto& data = pcm.channel0;
    const auto logPrefix = "ARA-F0: task=" + juce::String(static_cast<juce::uint64>(task.id.value)) + " ";
    AppLogger::log(logPrefix + "inference_start worker="
        + juce::String::toHexString(reinterpret_cast<uintptr_t>(juce::Thread::getCurrentThreadId()))
        + " model=" + juce::String(static_cast<int>(config.modelType))
        + " backend=" + AccelerationDetector::backendName(config.backend.backend)
        + " confidence=" + juce::String(config.confidence)
        + " f0Min=" + juce::String(config.f0Min) + " f0Max=" + juce::String(config.f0Max));
    const auto sampleRate = static_cast<int>(pcm.sourceSampleRate);
    std::string actualBackend;
    auto extraction = inference.extractF0ForAnalysis(
        data.data(), data.size(), sampleRate, task.owner, config, frozen.model,
        result.hopSize, result.f0SampleRate, actualBackend);
    if (!extraction.ok()) {
        result.errorMessage = extraction.error().fullMessage();
        if (extraction.error().code == ErrorCode::OperationCancelled)
            AppLogger::log(logPrefix + "analysis_aborted reason=" + juce::String(result.errorMessage));
        return;
    }
    result.f0 = std::move(extraction).value();
    if (result.f0.empty()) {
        result.errorMessage = "F0 inference returned an empty result";
        return;
    }
    result.energy = computeFrameEnergy(data.data(), static_cast<int>(data.size()), sampleRate,
        result.f0, result.f0SampleRate, result.hopSize);
    AppLogger::log(logPrefix + "inference_finish backend=" + juce::String(actualBackend));
}

} // namespace OpenTune
