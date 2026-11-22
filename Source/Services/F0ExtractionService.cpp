#include "F0ExtractionService.h"
#include "../Inference/ModelFactory.h"
#include "../Runtime/ProcessF0Runtime.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include "../Utils/AppLogger.h"
#include "../Utils/ModelPathResolver.h"
#include "ImportedClipF0Extraction.h"
#if JucePlugin_Enable_ARA
#include "F0AnalysisCache.h"
#include "../DSP/ResamplingManager.h"
#include "../Inference/FCPEExtractor.h"
#include "../Utils/AppPreferences.h"
#endif
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
    startWorkers(workerCount);
}

F0ExtractionService::F0ExtractionService(int workerCount, size_t maxQueueSize)
    : state_(std::make_shared<SharedState>())
{
    state_->maxQueueSize_ = maxQueueSize;
    startWorkers(workerCount);
}

void F0ExtractionService::startWorkers(int workerCount)
{
    const int count = std::max(1, workerCount);
    workers_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        workers_.emplace_back([state = state_] { workerLoop(state); });
}

F0ExtractionService::~F0ExtractionService()
{
    shutdown();
}

void F0ExtractionService::shutdown()
{
    std::call_once(shutdownOnce_, [this] {
        auto& state = *state_;
        const auto& mailbox = state.dispatcherFailureMailbox_;
        if (mailbox)
            mailbox->cancelAndClear();
        for (const auto& worker : workers_) {
            if (worker.get_id() == std::this_thread::get_id()) {
                AppLogger::error("[F0ExtractionService] shutdown called from worker thread");
                jassertfalse;
                std::terminate();
            }
        }

        std::deque<Job> discardedTasks;
        decltype(state.activeEntries_) discardedProcessorEntries;
        std::vector<F0TaskId> araTasks;
        {
            std::lock_guard<std::mutex> lock(state.entriesMutex_);
            state.shutdownStarted_ = true;
            discardedTasks.swap(state.queue_);
            discardedProcessorEntries.swap(state.activeEntries_);
            if (mailbox)
                state.processorRunOwner_->closed.store(true, std::memory_order_release);
            for (const auto& entry : state.araEntries_)
                araTasks.push_back(entry.first);
        }

        for (auto id : araTasks)
            abortTask(id, AraF0Abort::Orphaned);
        if (f0ServiceResolver_) {
            if (auto svc = f0ServiceResolver_())
                svc->terminateActiveRun(state.processorRunOwner_);
        }

        state.queueCv_.notify_all();
        for (auto& worker : workers_)
            worker.join();
        workers_.clear();
        if (mailbox)
            mailbox->cancelAndClear();
    });
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

    for (auto& failure : failures) {
        try {
            failure.callback(std::move(*failure.result));
        }
        catch (const std::exception& e) {
            AppLogger::error("[F0ExtractionService] dispatcher-failure completion threw: "
                + juce::String(e.what()));
        }
        catch (...) {
            AppLogger::error("[F0ExtractionService] dispatcher-failure completion threw: unknown exception");
        }
    }
}

F0ExtractionService::SubmitResult F0ExtractionService::submit(F0RequestKey requestKey, ExecuteFn execute, CommitFn commit,
                                                              CommitFn dispatcherFailure,
                                                              uint64_t* acceptedRequestToken)
{
    if (acceptedRequestToken != nullptr)
        *acceptedRequestToken = 0;
    if (!requestKey.contentKey.isValid() || !execute || !commit) {
        return SubmitResult::InvalidTask;
    }

    // 单一临界区：检查 shutdown、查重、容量检查、分配 token、插入 active、enqueue。
    // 队列满直接 QueueFull，不创建 active 条目。与 shutdown 线性化。
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex_);
        if (state_->shutdownStarted_)
            return SubmitResult::ShuttingDown;

        if (state_->activeEntries_.find(requestKey.contentKey) != state_->activeEntries_.end()) {
            return SubmitResult::AlreadyInProgress;
        }

        if (state_->queue_.size() >= state_->maxQueueSize_) {
            return SubmitResult::QueueFull;
        }

        const uint64_t token = state_->tokenCounter_++;
        if (acceptedRequestToken != nullptr)
            *acceptedRequestToken = token;
        state_->activeEntries_.emplace(requestKey.contentKey, token);
        state_->queue_.emplace_back(Task{requestKey, token, std::move(execute), std::move(commit),
                                        std::move(dispatcherFailure)});
    }

    // 解锁后唤醒单个 worker。
    state_->queueCv_.notify_one();
    return SubmitResult::Accepted;
}

bool F0ExtractionService::isActive(F0RequestKey requestKey) const
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex_);
    return state_->activeEntries_.find(requestKey.contentKey) != state_->activeEntries_.end();
}

void F0ExtractionService::cancel(F0RequestKey requestKey)
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex_);
    state_->activeEntries_.erase(requestKey.contentKey);
}

bool AraF0ConfigTag::operator==(const AraF0ConfigTag& rhs) const noexcept
{
    return selectionRevision == rhs.selectionRevision
        && absoluteModelPath == rhs.absoluteModelPath
        && modelBytes == rhs.modelBytes
        && modelWriteTime == rhs.modelWriteTime
        && allowCachedAnalysis == rhs.allowCachedAnalysis;
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
    jassert(!entry.empty());
    const auto key = std::find_if(byLocalKey.begin(), byLocalKey.end(),
        [taskId](const auto& item) { return item.second == taskId; });
    jassert(key != byLocalKey.end());
    byLocalKey.erase(key);
    return std::move(entry.mapped());
}

AraF0ConfigTag F0ExtractionService::snapshotAraConfig(bool allowCachedAnalysis)
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
    const auto path = ModelFactory::getModelPath(config.modelType, config.modelDirectory);
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(path);
    return {config.selectionRevision + config.backend.revision,
            file.getFullPathName().toStdString(), file.getSize(), file.getLastModificationTime(),
            std::move(config), allowCachedAnalysis};
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
        if (isActive(id)) {
            flights.byTaskId.at(id).insert_or_assign(receiver.wrapperGeneration, std::move(receiver));
            return {AraF0Admission::Attached, id};
        }
        // A dispatcher rejection releases the service entry without touching the document.
        flights.take(id);
    }

    auto owner = std::make_shared<F0RunOwnerState>();
    AraTask task{{}, std::move(pcm), key.config, owner};
    F0TaskId id;
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex_);
        if (state_->shutdownStarted_)
            return {AraF0Admission::Closing, {}};
        if (state_->queue_.size() >= state_->maxQueueSize_)
            return {AraF0Admission::QueueFull, {}};

        id = {state_->tokenCounter_++};
        jassert(id.value != 0);
        task.id = id;
        state_->araEntries_.emplace(id, AraEntry{owner, flights.sink, nullptr, {}});
        state_->queue_.emplace_back(std::move(task));
    }
    flights.byLocalKey.emplace_back(key, id);
    flights.byTaskId[id].emplace(receiver.wrapperGeneration, std::move(receiver));
    state_->queueCv_.notify_one();
    return {AraF0Admission::Accepted, id};
}

bool F0ExtractionService::isActive(F0TaskId taskId) const
{
    std::lock_guard<std::mutex> lock(state_->entriesMutex_);
    return !state_->shutdownStarted_ && state_->araEntries_.find(taskId) != state_->araEntries_.end();
}

void F0ExtractionService::abortTask(F0TaskId taskId, AraF0Abort reason)
{
    decltype(state_->araEntries_)::node_type entry;
    std::optional<AraTask> discarded;
    {
        std::lock_guard<std::mutex> lock(state_->entriesMutex_);
        entry = state_->araEntries_.extract(taskId);
        if (entry.empty())
            return;
        entry.mapped().owner->closed.store(true, std::memory_order_release);
        const auto queued = std::find_if(state_->queue_.begin(), state_->queue_.end(),
            [taskId](const Job& job) {
                const auto* task = std::get_if<AraTask>(&job);
                return task != nullptr && task->id == taskId;
            });
        if (queued != state_->queue_.end()) {
            discarded.emplace(std::move(std::get<AraTask>(*queued)));
            state_->queue_.erase(queued);
        }
    }

    // Registration publishes the exact service for this origin. Cold startup
    // has no registration yet, so a revoke never waits on runtime initialization.
    if (auto* inference = entry.mapped().inference)
        inference->terminateActiveRun(entry.mapped().owner);
    static constexpr const char* reasons[] = {"SamplesDisabled", "SourceUndo", "SamplesChanged", "Orphaned"};
    AppLogger::logNoThrow("ARA-F0: task=" + juce::String(static_cast<juce::uint64>(taskId.value))
        + " abort_requested reason=" + reasons[static_cast<size_t>(reason)]);
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
        flights.take(id);
        abortTask(id, AraF0Abort::Orphaned);
    }
}

void F0ExtractionService::workerLoop(std::shared_ptr<SharedState> state)
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(state->entriesMutex_);
            state->queueCv_.wait(lock, [&] { return state->shutdownStarted_ || !state->queue_.empty(); });
            if (state->shutdownStarted_)
                return;
            job = std::move(state->queue_.front());
            state->queue_.pop_front();
            if (auto* processorTask = std::get_if<Task>(&job)) {
                const auto it = state->activeEntries_.find(processorTask->requestKey.contentKey);
                if (it == state->activeEntries_.end() || it->second != processorTask->token)
                    continue;
            }
        }
        if (auto* processorTask = std::get_if<Task>(&job))
            runProcessorTask(*state, std::move(*processorTask));
        else
            runAraTask(state, std::move(std::get<AraTask>(job)));
    }
}

void F0ExtractionService::runProcessorTask(SharedState& state, Task task)
{
    Result result;
    try {
        result = task.execute(state.processorRunOwner_);
    } catch (const std::exception& e) {
        AppLogger::error("[F0ExtractionService] Task execution threw std::exception: " + juce::String(e.what()));
        result.errorMessage = "F0 extraction task threw std::exception: " + std::string(e.what());
    } catch (...) {
        AppLogger::error("[F0ExtractionService] Task execution threw unknown exception");
        result.errorMessage = "F0 extraction task threw an unknown exception";
    }
    result.requestKey = task.requestKey;
    result.requestToken = task.token;
    {
        std::lock_guard<std::mutex> lock(state.entriesMutex_);
        auto it = state.activeEntries_.find(task.requestKey.contentKey);
        if (state.shutdownStarted_ || it == state.activeEntries_.end() || it->second != task.token)
            return;
        state.activeEntries_.erase(it);
    }
    auto resultForDispatch = std::make_shared<Result>(std::move(result));
    const bool posted = juce::MessageManager::callAsync([commit = std::move(task.commit), resultForDispatch]() mutable {
        try {
            commit(std::move(*resultForDispatch));
        } catch (const std::exception& e) {
            AppLogger::error("[F0ExtractionService] completion threw: " + juce::String(e.what()));
        } catch (...) {
            AppLogger::error("[F0ExtractionService] completion threw: unknown exception");
        }
    });
    if (!posted) {
        if (task.dispatcherFailure)
            state.dispatcherFailureMailbox_->enqueue(std::move(task.dispatcherFailure), std::move(resultForDispatch));
        else
            AppLogger::error("[F0ExtractionService] completion dispatcher rejected; hard lifecycle failure");
    }
}

void F0ExtractionService::runAraTask(const std::shared_ptr<SharedState>& state, AraTask task)
{
    auto result = std::make_shared<AraF0Result>();
    try {
        executeAraTask(state, task, *result);
    } catch (const std::exception& e) {
        result->errorMessage = e.what();
    }
    {
        std::lock_guard<std::mutex> lock(state->entriesMutex_);
        const auto entry = state->araEntries_.find(task.id);
        if (state->shutdownStarted_ || entry == state->araEntries_.end())
            return;
        entry->second.inference = nullptr;
        entry->second.result = std::move(result);
    }
    // The worker hands off only a weak, non-model service state and an ID. The
    // weak document sink is resolved exclusively in message-thread delivery.
    if (!ProcessRenderRuntime::postAsyncCallback([weak = std::weak_ptr<SharedState>(state), id = task.id] {
            deliverAraResult(weak, id);
        })) {
        decltype(state->araEntries_)::node_type released;
        {
            std::lock_guard<std::mutex> lock(state->entriesMutex_);
            released = state->araEntries_.extract(task.id);
        }
        AppLogger::logNoThrow("ARA-F0: completion dispatcher rejected task="
            + juce::String(static_cast<juce::uint64>(task.id.value)));
    }
}

void F0ExtractionService::deliverAraResult(const std::weak_ptr<SharedState>& weak, F0TaskId id)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto state = weak.lock();
    if (!state)
        return;
    decltype(state->araEntries_)::node_type entry;
    {
        std::lock_guard<std::mutex> lock(state->entriesMutex_);
        if (state->shutdownStarted_)
            return;
        entry = state->araEntries_.extract(id);
    }
    if (!entry.empty())
        if (auto sink = entry.mapped().sink.lock())
            (*sink)(id, *entry.mapped().result);
}

void F0ExtractionService::executeAraTask(const std::shared_ptr<SharedState>& state,
                                        AraTask& task, AraF0Result& result)
{
    auto& runtime = ProcessF0Runtime::getInstance();
    auto& config = task.config.extraction;
    if (!runtime.isReady() && !runtime.initialize(config.modelDirectory, config.modelType, &result.errorMessage))
        return;

    auto inference = runtime.getF0Service();
    {
        std::lock_guard<std::mutex> lock(state->entriesMutex_);
        const auto entry = state->araEntries_.find(task.id);
        if (state->shutdownStarted_ || entry == state->araEntries_.end())
            return;
        entry->second.inference = inference.get();
    }

    // Auto detection on cold startup resolves the frozen automatic policy here.
    if (!config.backend.detected)
        config.backend = AccelerationDetector::getInstance().resolveAutomaticSelection();
    const juce::File model(task.config.absoluteModelPath);
    if (model.getSize() != task.config.modelBytes || model.getLastModificationTime() != task.config.modelWriteTime) {
        result.errorMessage = "F0 model changed after task submission";
        return;
    }
    const auto& pcm = task.pcm;
    const auto& data = pcm.channel0;
    const std::string backend = AccelerationDetector::backendName(config.backend.backend);
    const auto logPrefix = "ARA-F0: task=" + juce::String(static_cast<juce::uint64>(task.id.value)) + " ";

#if JucePlugin_Enable_ARA
    // Disk cache policy and key are unchanged in P2. Hashing and I/O stay on the worker.
    std::optional<F0AnalysisCache::KeyConfig> cacheConfig;
    F0AnalysisCache::Digest pcmDigest{};
    F0AnalysisCache::Digest cacheKey{};
    if (task.config.allowCachedAnalysis && AppPreferences::readAraF0AnalysisCacheEnabledFromDisk()) {
        if (const auto weights = ModelFactory::getModelWeightDigest(task.config.absoluteModelPath)) {
            auto& key = cacheConfig.emplace();
            key.schemaVersion = F0AnalysisCache::kSchemaVersion;
            key.algorithmVersion = FCPEExtractor::kCacheAlgorithmVersion;
            key.resamplingAlgorithmVersion = ResamplingManager::kCacheAlgorithmVersion;
            key.f0AlgorithmVersion = FCPEExtractor::kCacheAlgorithmVersion;
            key.energyAlgorithmVersion = kFrameEnergyAlgorithmVersion;
            key.modelType = static_cast<int>(config.modelType);
            key.modelWeightDigest = *weights;
            key.confidence = config.confidence;
            key.f0Min = config.f0Min;
            key.f0Max = config.f0Max;
            key.backend = backend;
            key.sourceSampleRate = pcm.sourceSampleRate;
            key.sourceChannelCount = pcm.sourceChannelCount;
            key.channelPolicyVersion = F0AnalysisCache::kChannelZeroPolicyVersion;
            key.windowSampleCount = data.size();
            pcmDigest = F0AnalysisCache::digestPcm(data.data(), data.size());
            cacheKey = F0AnalysisCache::buildKey(key, pcmDigest);
            if (auto cached = F0AnalysisCache::tryLoad(F0AnalysisCache::cacheDirectory(), cacheKey)) {
                F0AnalysisCache::touch(F0AnalysisCache::cacheDirectory(), cacheKey);
                result.hopSize = cached->hopSize;
                result.f0SampleRate = static_cast<int>(cached->f0SampleRate);
                result.f0 = std::move(cached->originalF0);
                result.energy = std::move(cached->energy);
                AppLogger::log(logPrefix + "cache_hit key=" + juce::String(F0AnalysisCache::keyToHex(cacheKey).substr(0, 16)));
                return;
            }
        }
    }
#endif

    AppLogger::log(logPrefix + "inference_start worker="
        + juce::String::toHexString(reinterpret_cast<uintptr_t>(juce::Thread::getCurrentThreadId()))
        + " model=" + juce::String(static_cast<int>(config.modelType)) + " backend=" + juce::String(backend)
        + " confidence=" + juce::String(config.confidence)
        + " f0Min=" + juce::String(config.f0Min) + " f0Max=" + juce::String(config.f0Max));
    const auto sampleRate = static_cast<int>(pcm.sourceSampleRate);
    std::string actualBackend;
    auto extraction = inference->extractF0ForAnalysis(
        data.data(), data.size(), sampleRate, task.owner, config, task.config.absoluteModelPath,
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

#if JucePlugin_Enable_ARA
    if (cacheConfig && !task.owner->closed.load(std::memory_order_acquire)
        && AppPreferences::readAraF0AnalysisCacheEnabledFromDisk()) {
        const auto weights = ModelFactory::getModelWeightDigest(task.config.absoluteModelPath);
        if (weights && *weights == cacheConfig->modelWeightDigest) {
            if (cacheConfig->backend != actualBackend) {
                cacheConfig->backend = actualBackend;
                cacheKey = F0AnalysisCache::buildKey(*cacheConfig, pcmDigest);
            }
            F0AnalysisCache::Record record;
            record.hopSize = result.hopSize;
            record.f0SampleRate = static_cast<double>(result.f0SampleRate);
            record.originalF0 = result.f0;
            record.energy = result.energy;
            F0AnalysisCache::publish(F0AnalysisCache::cacheDirectory(), cacheKey, record);
        } else {
            AppLogger::log(logPrefix + "cache_publish_skipped reason=model_digest_changed");
        }
    }
#endif
}

} // namespace OpenTune
