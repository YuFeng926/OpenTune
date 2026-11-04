#include "ProcessRenderRuntime.h"

#include "../DSP/AutoTunePeriodDetector.h"
#include "../DSP/AutoTunePitchShifter.h"
#include "../Services/HnsepAudio.h"
#include "../Editor/ConfirmDialogContent.h"
#include "../DSP/MelSpectrogram.h"
#include "../DSP/HybridAperiodicResidual.h"
#include "../DSP/NoteEqProcessor.h"
#include "../Inference/ChunkRenderStrategy.h"
#include "../Inference/VocoderDomain.h"
#include "../Render/RenderChunkPlanner.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include "../Utils/AppPreferences.h"
#include "../Utils/ChannelLayoutLogger.h"
#include "../Utils/ModelPathResolver.h"
#include "../Utils/LocalizationManager.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/F0GapFill.h"
#include "../Utils/TimeCoordinate.h"
#include "ProcessF0Runtime.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>

namespace OpenTune {

void ProcessRenderRuntime::notifyApplicationRenderFailure(ContentKey key,
                                                           uint64_t contentRevision,
                                                           uint64_t targetRevision,
                                                           const juce::String& reason,
                                                           std::shared_ptr<RenderCache> renderCache,
                                                           std::shared_ptr<CompletionGate> gate,
                                                           std::shared_ptr<std::atomic<bool>> reconfigurationNotificationClaim)
{
    const auto failureReason = reason;
    const bool posted = juce::MessageManager::callAsync(
        [key, contentRevision, targetRevision, failureReason,
         renderCache = std::move(renderCache), gate,
         reconfigurationNotificationClaim = std::move(reconfigurationNotificationClaim)]()
    {
        const auto displayFailure = [&]() {
            juce::ignoreUnused(targetRevision);
            if (renderCache != nullptr
                && !renderCache->markFailureNotificationForContentRevision(contentRevision))
                return;
            if (reconfigurationNotificationClaim != nullptr
                && reconfigurationNotificationClaim->exchange(true, std::memory_order_acq_rel))
                return;

            const auto displayedReason = failureReason.isNotEmpty()
                ? failureReason
                : juce::String("Render failure reason is unavailable from the reporting stage");
            const auto contentId = juce::String(static_cast<juce::int64>(key.objectId));
            ConfirmDialogContent::showDiagnostic(
                nullptr,
                LOC(kRenderFailure),
                Loc::format(LOC(kRenderFailureDetail), contentId,
                            getDomainKindName(key.domainKind), displayedReason),
                AppLogger::makeDiagnosticText("Render", displayedReason));
        };

        CompletionCallbackLease callbackLease(*gate);
        if (callbackLease)
            displayFailure();
    });
    if (!posted)
        AppLogger::error("[ProcessRenderRuntime] render failure notification dispatcher rejected");
}

namespace {

using PayloadFootprint = std::vector<std::pair<const void*, std::size_t>>;

template <typename T>
void addVectorPayload(const std::vector<T>& values,
                      std::unordered_map<const void*, std::size_t>& allocations)
{
    if (values.capacity() > 0)
        allocations.emplace(values.data(), values.capacity() * sizeof(T));
}

void addNotePayload(const std::vector<Note>& notes,
                    std::unordered_map<const void*, std::size_t>& allocations)
{
    addVectorPayload(notes, allocations);
    for (const auto& note : notes)
        if (note.eq)
            addVectorPayload(note.eq->filters, allocations);
}

void addPitchCurvePayload(const std::shared_ptr<const PitchCurveSnapshot>& curve,
                          std::unordered_map<const void*, std::size_t>& allocations)
{
    if (curve == nullptr)
        return;
    addVectorPayload(curve->getOriginalF0(), allocations);
    addVectorPayload(curve->getOriginalEnergy(), allocations);
    addVectorPayload(curve->getErasedRanges(), allocations);
    const auto& segments = curve->getCorrectionSegments();
    addVectorPayload(segments, allocations);
    for (const auto& segment : segments)
    {
        addVectorPayload(segment.f0Data, allocations);
        addVectorPayload(segment.baseF0Data, allocations);
    }
    if (curve->getVocoderMaskPayloadBytes() > 0)
        allocations.emplace(curve.get(), curve->getVocoderMaskPayloadBytes());
    // JUCE String and allocator/control-block metadata have no portable
    // payload-capacity API.
}

PayloadFootprint snapshotPayloadFootprint(
    const std::shared_ptr<const EditableContentSnapshot>& snapshot,
    const std::shared_ptr<const juce::AudioBuffer<float>>& jobAudio)
{
    std::unordered_map<const void*, std::size_t> allocations;
    const auto addAudio = [&allocations](
        const std::shared_ptr<const juce::AudioBuffer<float>>& audio) {
        if (audio != nullptr)
        {
            // JUCE allocates each channel for getNumSamples(); its allocation
            // capacity is not exposed. Count actual sample payload, not PCM cache data.
            const auto channelBytes = static_cast<std::size_t>(audio->getNumSamples())
                * sizeof(float);
            for (int channel = 0; channel < audio->getNumChannels(); ++channel)
                allocations.emplace(audio->getReadPointer(channel), channelBytes);
        }
    };
    addAudio(jobAudio);
    if (snapshot != nullptr)
    {
        addAudio(snapshot->audioBuffer);
        addNotePayload(snapshot->notes, allocations);
        addVectorPayload(snapshot->silentGaps, allocations);
        addVectorPayload(snapshot->volumeEnvelope.points(), allocations);
        addNotePayload(snapshot->referenceFeatures.pitch.notes, allocations);
        addVectorPayload(snapshot->referenceFeatures.timing.anchors, allocations);
        addPitchCurvePayload(snapshot->pitchCurve, allocations);
        if (snapshot->timeGrid != nullptr)
            addVectorPayload(snapshot->timeGrid->handles(), allocations);
        for (const auto& payload : {snapshot->hnsepHarmonicAudio, snapshot->hnsepResidualAudio})
            if (payload != nullptr)
                addVectorPayload(*payload, allocations);
        if (snapshot->originalF0FailureReason.capacity() > 0)
            allocations.emplace(snapshot.get(), snapshot->originalF0FailureReason.capacity() + 1);
    }

    PayloadFootprint footprint;
    footprint.reserve(allocations.size());
    for (const auto& allocation : allocations)
        footprint.emplace_back(allocation.first, allocation.second);
    return footprint;
}

// ==============================================================================
// Effective F0 Materialization
// ==============================================================================
// 将 contentSnap 的 forEachEffectiveF0Span 复制与 gain 应用封装为文件内函数，
// 消除 leading lookback、trailing lookahead、主 chunk 三处完全相同的逻辑。

std::vector<float> materializeEffectiveF0Range(
    const EditableContentSnapshot& contentSnap, int startFrame, int endFrame)
{
    const int rangeLength = std::max(0, endFrame - startFrame);
    std::vector<float> effectiveF0(static_cast<size_t>(rangeLength), 0.0f);
    contentSnap.forEachEffectiveF0Span(startFrame, endFrame,
        [&effectiveF0, startFrame](int frameIndex, const float* values, int spanLength, float gain)
        {
            const int offset = frameIndex - startFrame;
            for (int k = 0; k < spanLength; ++k)
                effectiveF0[static_cast<size_t>(offset + k)] = values[static_cast<size_t>(k)] * gain;
        });
    return effectiveF0;
}

struct ContentSampleRange
{
    int64_t startSample{0};
    int64_t endSampleExclusive{0};

    bool isValid() const noexcept
    {
        return endSampleExclusive > startSample;
    }
};

struct FrozenRenderBoundaries
{
    int64_t trueStartSample{0};
    int64_t trueEndSample{0};
    int64_t synthEndSample{0};
    int64_t publishSampleCount{0};
    int64_t synthSampleCount{0};
    int frameCount{0};
    int hopSize{0};
};

bool freezeRenderBoundaries(const ContentSampleRange& contentRange,
                            int64_t startSample,
                            int64_t endSampleExclusive,
                            int hopSize,
                            FrozenRenderBoundaries& out)
{
    out = FrozenRenderBoundaries{};

    if (!contentRange.isValid() || hopSize <= 0)
        return false;

    out.trueStartSample = juce::jlimit(contentRange.startSample, contentRange.endSampleExclusive, startSample);
    out.trueEndSample = juce::jlimit(out.trueStartSample, contentRange.endSampleExclusive, endSampleExclusive);
    out.publishSampleCount = out.trueEndSample - out.trueStartSample;
    if (out.publishSampleCount <= 0)
    {
        out = FrozenRenderBoundaries{};
        return false;
    }

    const bool isLastChunk = out.trueEndSample == contentRange.endSampleExclusive;
    if (!isLastChunk && (out.publishSampleCount % hopSize) != 0)
    {
        out = FrozenRenderBoundaries{};
        return false;
    }

    out.frameCount = juce::jmax(1, static_cast<int>((out.publishSampleCount + hopSize - 1) / hopSize));
    out.synthSampleCount = isLastChunk ? static_cast<int64_t>(out.frameCount) * hopSize
                                       : out.publishSampleCount;
    out.synthEndSample = out.trueStartSample + out.synthSampleCount;
    out.hopSize = hopSize;
    return true;
}

bool preparePublishedAudioFromSynthesis(const FrozenRenderBoundaries& boundaries,
                                        const std::vector<float>& synthesizedAudio,
                                        std::vector<float>& publishedAudio)
{
    publishedAudio.clear();

    if (boundaries.publishSampleCount <= 0 || boundaries.synthSampleCount <= 0)
        return false;

    if (synthesizedAudio.size() != static_cast<size_t>(boundaries.synthSampleCount))
        return false;

    publishedAudio.assign(synthesizedAudio.begin(),
                          synthesizedAudio.begin() + static_cast<size_t>(boundaries.publishSampleCount));
    return true;
}

void notifyChunkSettled(const ProcessRenderRuntime::CompletionContext& completion,
                        ContentKey key,
                        std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
                        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                        double audioSampleRate);

void notifyChunkSettled(const ProcessRenderRuntime::CompletionContext& completion,
                        const RenderJob& job)
{
    if (!completion.chunkSettled)
        return;
    notifyChunkSettled(completion, job.contentKey, job.contentSnapshot,
                       job.audioBuffer, job.audioSampleRate);
}

void notifyChunkSettled(const ProcessRenderRuntime::CompletionContext& completion,
                        ContentKey key,
                        std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
                        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                        double audioSampleRate)
{
    if (!completion.chunkSettled)
        return;
    const auto gate = completion.gate;
    auto callback = completion.chunkSettled;
    const bool posted = juce::MessageManager::callAsync(
        [gate, callback = std::move(callback), key,
         contentSnapshot = std::move(contentSnapshot), audioBuffer = std::move(audioBuffer), audioSampleRate]() mutable {
            ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
            if (!callbackLease)
                return;
            callback(key, std::move(contentSnapshot), std::move(audioBuffer), audioSampleRate);
        });
    if (!posted)
        AppLogger::error("[ProcessRenderRuntime] settled completion dispatcher rejected; hard lifecycle failure");
}

void notifyChunkFailed(const ProcessRenderRuntime::CompletionContext& completion,
                       ContentKey key,
                       uint64_t targetRevision,
                       const juce::String& reason,
                       bool notifyApplication = true)
{
    if (completion.gate != nullptr)
    {
        std::lock_guard<std::mutex> lock(completion.gate->mutex);
        if (completion.gate->closed)
            return;
    }
    // Application notification and owner completion each acquire the gate on
    // the message thread, so owner teardown suppresses both callbacks.
    if (notifyApplication && completion.applicationFailure)
        completion.applicationFailure(key, completion.contentRevision, targetRevision,
                                      reason, completion.renderCache);

    if (!completion.chunkFailed)
        return;
    const auto gate = completion.gate;
    auto callback = completion.chunkFailed;
    const auto failureReason = reason;
    const bool posted = juce::MessageManager::callAsync(
        [gate, callback = std::move(callback), key, failureReason]() mutable {
            ProcessRenderRuntime::CompletionCallbackLease callbackLease(*gate);
            if (!callbackLease)
                return;
            callback(key, failureReason);
        });
    if (!posted)
        AppLogger::error("[ProcessRenderRuntime] failed completion dispatcher rejected; hard lifecycle failure");
}

void failChunk(const ProcessRenderRuntime::CompletionContext& completion,
               RenderCache* cache,
               int64_t startSample,
               uint64_t revision,
               ContentKey key,
               const juce::String& reason)
{
    if (cache == nullptr)
    {
        notifyChunkFailed(completion, key, revision, reason);
        return;
    }
    if (cache->completeChunkRenderFailure(startSample, revision))
        notifyChunkFailed(completion, key, revision, reason);
}

// ==============================================================================
// Per-note EQ 唯一发布（契约 §4/§6/§7）
// ==============================================================================
// 所有 Stage1 最终 Note 音频结果（轻量、原始、Vocoder）在写入缓存前统一经
// 应用 per-note EQ，位于音量包络前（包络只在播放端
// applyAutomationGain 应用）。notes 只在本调用栈内读取，不复制、不存储。
// 每个 active Note 使用局部 NoteEqProcessor，prepare 按固定渲染率计算系数并从
// Note 起点重置状态，只处理该 Note 在完整 chunk 内的精确样本范围。
// RenderChunkPlanner 已保证 active-EQ Note 不跨 chunk；Stage1 音频固定 44.1kHz。
constexpr int kNoteEqBoundaryFadeSamples = 12;

// chunk 发布范围 [trueStartSample, trueEndSample) 是否与任何 active-EQ Note
// 相交（用于原始路径 Blank/失败条件中的 EQ 排除，契约 §6）。
// 秒域边界按 RenderCache::kSampleRate 换算，与接入音频的样本域一致。
bool chunkIntersectsActiveEqNote(const std::vector<Note>& notes,
                                 const FrozenRenderBoundaries& boundaries)
{
    for (const auto& note : notes)
    {
        if (!note.eq.has_value() || !note.eq->active)
            continue;

        const int64_t noteStartSample = TimeCoordinate::secondsToSamplesFloor(
            note.startTime, RenderCache::kSampleRate);
        if (noteStartSample >= boundaries.trueEndSample)
            break;  // notes 按起点升序

        const int64_t noteEndSample = TimeCoordinate::secondsToSamplesCeil(
            note.endTime, RenderCache::kSampleRate);
        if (noteEndSample > boundaries.trueStartSample)
            return true;
    }
    return false;
}

RenderCache::ChunkRenderResult publishChunkWithPerNoteEq(
    RenderCache& renderCache,
    const FrozenRenderBoundaries& boundaries,
    std::vector<float> audio,
    uint64_t revision,
    const std::vector<Note>& notes)
{
    for (const auto& note : notes)
    {
        if (!note.eq.has_value() || !note.eq->active)
            continue;

        const int64_t noteStartSample = TimeCoordinate::secondsToSamplesFloor(
            note.startTime, RenderCache::kSampleRate);
        if (noteStartSample >= boundaries.trueEndSample)
            break;  // notes 按起点升序

        const int64_t noteEndSample = TimeCoordinate::secondsToSamplesCeil(
            note.endTime, RenderCache::kSampleRate);
        const int64_t rangeStart = std::max(noteStartSample, boundaries.trueStartSample);
        const int64_t rangeEnd = std::min(noteEndSample, boundaries.trueEndSample);
        if (rangeEnd <= rangeStart)
            continue;

        // planner 保证 active-EQ Note 不跨 chunk，因此处理器仅在本 Note 内存在，
        // 不需要共享状态或跨 chunk 缓存。
        NoteEqProcessor processor;
        processor.prepare(RenderCache::kSampleRate, *note.eq);

        const size_t audioOffset = static_cast<size_t>(rangeStart - boundaries.trueStartSample);
        const int noteSampleCount = static_cast<int>(rangeEnd - rangeStart);
        const bool fadeIn = rangeStart == noteStartSample;
        const bool fadeOut = rangeEnd == noteEndSample;

        if (!fadeIn && !fadeOut)
        {
            float* channelData[1] = { audio.data() + audioOffset };
            juce::AudioBuffer<float> noteBuffer(channelData, 1, noteSampleCount);
            processor.process(noteBuffer);
            continue;
        }

        std::vector<float> filtered(static_cast<size_t>(noteSampleCount));
        std::copy_n(audio.data() + audioOffset, noteSampleCount, filtered.data());
        float* filteredData[1] = { filtered.data() };
        juce::AudioBuffer<float> filteredBuffer(filteredData, 1, noteSampleCount);
        processor.process(filteredBuffer);

        const int fadeInSamples = std::min(kNoteEqBoundaryFadeSamples, noteSampleCount);
        const int fadeOutSamples = std::min(kNoteEqBoundaryFadeSamples, noteSampleCount);
        for (int i = 0; i < noteSampleCount; ++i)
        {
            float wetMix = 1.0f;
            if (fadeIn)
            {
                const float weight = fadeInSamples > 1
                    ? static_cast<float>(i) / static_cast<float>(fadeInSamples - 1)
                    : 0.0f;
                wetMix = std::min(wetMix, weight);
            }
            if (fadeOut)
            {
                const int distanceFromEnd = noteSampleCount - 1 - i;
                const float weight = fadeOutSamples > 1
                    ? static_cast<float>(distanceFromEnd) / static_cast<float>(fadeOutSamples - 1)
                    : 0.0f;
                wetMix = std::min(wetMix, weight);
            }

            const float dry = audio[audioOffset + static_cast<size_t>(i)];
            const float wet = filtered[static_cast<size_t>(i)];
            audio[audioOffset + static_cast<size_t>(i)] = dry + (wet - dry) * wetMix;
        }
    }

    return renderCache.completeChunkRenderWithAudio(
        boundaries.trueStartSample, boundaries.trueEndSample,
        std::move(audio), revision);
}

} // namespace

ProcessRenderRuntime& ProcessRenderRuntime::getInstance()
{
    static ProcessRenderRuntime* instance = new ProcessRenderRuntime();
    return *instance;
}

void ProcessRenderRuntime::retainOwner()
{
    std::unique_lock<std::mutex> lifecycleLock(shutdownMutex_);
    if (ownerCount_ == 0 && shuttingDown_.load(std::memory_order_acquire))
    {
        // A new host instance may appear after the previous last instance was
        // destroyed.  Complete the already-requested one-shot shutdown before
        // reopening the process runtime; this prevents a new owner from
        // inheriting a joined worker and a permanently closed command queue.
        workerExitCv_.wait(lifecycleLock, [this]() {
            return workerExitDispatchAttempted_.load(std::memory_order_acquire);
        });
        const auto generation = shutdownGeneration_.load(std::memory_order_acquire);
        lifecycleLock.unlock();
        finishShutdown(generation);
        lifecycleLock.lock();

        {
            std::lock_guard<std::mutex> controlLock(controlMutex_);
            ensureVocoderQueued_ = false;
            deferredEnsureRetryPending_.store(false, std::memory_order_release);
            activeTransaction_.reset();
            controlQueue_.clear();
        }
        workerExitDispatchAttempted_.store(false, std::memory_order_release);
        workerExitDispatchPosted_.store(false, std::memory_order_release);
        shuttingDown_.store(false, std::memory_order_release);
        shutdownGeneration_.fetch_add(1, std::memory_order_acq_rel);
        controlWorker_ = std::thread([this]() { controlWorkerLoop(); });
    }
    ++ownerCount_;
}

void ProcessRenderRuntime::releaseOwner() noexcept
{
    std::unique_lock<std::mutex> lifecycleLock(shutdownMutex_);
    --ownerCount_;
    if (ownerCount_ == 0)
        shutdownLocked(lifecycleLock);
}

ProcessRenderRuntime::ProcessRenderRuntime()
{
    // 启动前从持久化配置读取 vocoder 模型权重，确保首次 EnsureVocoder 使用用户
    // 实际选择的权重（而非硬编码默认值）。
    const auto prefs = AppPreferences().getState().shared.vocoderModelWeight;
    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    currentVocoderModelWeight_ = prefs;

    // 持久化权重可能已被新版本停止打包；只回退到项目保证提供的内置默认权重。
    // 默认权重也缺失时保留原值，让后续 createVocoderDomain 报出真实错误。
    const auto resolvedPath = modelPathForWeight(modelsDir, prefs);
    if (!juce::File(resolvedPath).existsAsFile()) {
        AppLogger::warn("[ProcessRenderRuntime] Persisted vocoder weight not found: "
            + juce::String(prefs) + " (" + resolvedPath + ")");

        if (prefs != kDefaultVocoderWeight) {
            const auto defaultPath = modelPathForWeight(modelsDir, kDefaultVocoderWeight);
            if (juce::File(defaultPath).existsAsFile()) {
                AppLogger::warn("[ProcessRenderRuntime] Falling back to default weight: "
                    + juce::String(kDefaultVocoderWeight));
                currentVocoderModelWeight_ = kDefaultVocoderWeight;
            }
        }
    }

    controlWorker_ = std::thread([this]() { controlWorkerLoop(); });
}

void ProcessRenderRuntime::shutdownLocked(std::unique_lock<std::mutex>& shutdownLock)
{
    if (shuttingDown_.load(std::memory_order_acquire))
        return;
    shuttingDown_.store(true, std::memory_order_release);
    const auto generation = shutdownGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;

    std::shared_ptr<ControlTransaction> activeTransaction;
    std::vector<std::shared_ptr<ControlTransaction>> queuedTransactions;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        ControlCommand stop;
        stop.type = ControlCommand::Type::Stop;
        for (auto& command : controlQueue_)
        {
            if (command.transaction)
                queuedTransactions.push_back(std::move(command.transaction));
        }
        controlQueue_.clear();
        controlQueue_.push_back(std::move(stop));
        activeTransaction = activeTransaction_;
    }
    for (const auto& transaction : queuedTransactions)
    {
        transaction->closeRequested.store(true, std::memory_order_release);
        finishControlTransaction(transaction, "shutdown queued transaction");
    }
    // A command already taken by the worker may be waiting for a
    // message-thread ack.  Request its close; the control worker performs the
    // one pause/resume and CRS release itself after it has stopped using the
    // transaction.  shutdown() must not race that shared_ptr or resume CRS
    // while the worker is still inside the command.
    if (activeTransaction)
    {
        activeTransaction->closeRequested.store(true, std::memory_order_release);
        activeTransaction->cv.notify_all();
    }
    controlCv_.notify_all();

    // The worker posts finishShutdown() only after it has consumed Stop and
    // released the Domain.  Every caller waits until that dispatch attempt is
    // published.  A successful post is the join handoff, including for the
    // message thread; waiting for its completion there would deadlock.
    workerExitCv_.wait(shutdownLock, [this]() {
        return workerExitDispatchAttempted_.load(std::memory_order_acquire);
    });
    if (!workerExitDispatchPosted_.load(std::memory_order_acquire))
    {
        AppLogger::error("[ProcessRenderRuntime] worker-exit dispatcher rejected; synchronously joining");
        shutdownLock.unlock();
        finishShutdown(generation);
    }
}

void ProcessRenderRuntime::finishControlTransaction(
    const std::shared_ptr<ControlTransaction>& transaction,
    const char* reason) noexcept
{
    if (!claimControlTransaction(transaction))
        return;
    completeClaimedControlTransaction(transaction, reason);
}

void ProcessRenderRuntime::completeClaimedControlTransaction(
    const std::shared_ptr<ControlTransaction>& transaction,
    const char* reason) noexcept
{
    if (!transaction)
        return;
    if (transaction->crs
        && transaction->paused.exchange(false, std::memory_order_acq_rel))
        transaction->crs->resumeRenderWorker();
    transaction->crs.reset();
    transaction->acked.store(true, std::memory_order_release);
    AppLogger::info("[ProcessRenderRuntime] control transaction ack: " + juce::String(reason));
    transaction->cv.notify_all();
}

bool ProcessRenderRuntime::claimControlTransaction(
    const std::shared_ptr<ControlTransaction>& transaction) noexcept
{
    return transaction != nullptr
        && !transaction->finishing.exchange(true, std::memory_order_acq_rel);
}

void ProcessRenderRuntime::finishShutdown(uint64_t generation)
{
    std::lock_guard<std::mutex> lifecycleLock(shutdownMutex_);
    if (generation != shutdownGeneration_.load(std::memory_order_acquire))
        return;
    if (controlWorker_.joinable()
        && controlWorker_.get_id() == std::this_thread::get_id())
    {
        AppLogger::error("[ProcessRenderRuntime] control worker self-join rejected; hard failure");
        std::terminate();
    }
    if (controlWorker_.joinable())
        controlWorker_.join();
    std::vector<DeferredRetry> deferred;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        for (const auto& retry : deferredRetries_)
            releaseDeferredPayloadsLocked(retry);
        deferred = std::move(deferredRetries_);
        vocoderReconfiguring_ = false;
    }
    if (!deferred.empty())
    {
        AppLogger::error("[ProcessRenderRuntime] shutdown left deferred retries");
        std::terminate();
    }

    bool controlStateClear = false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        controlStateClear = controlQueue_.empty() && activeTransaction_ == nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (vocoderDomain_ != nullptr || domainSubmitInFlight_ != 0 || !controlStateClear)
        {
            AppLogger::error("[ProcessRenderRuntime] shutdown resource state did not converge");
            std::terminate();
        }
    }
}

std::string ProcessRenderRuntime::modelPathForWeight(const std::string& modelDir, const VocoderModelWeight& weight)
{
    // 权重标识即文件名：vocoder_weights/ 优先（用户放置的可切换权重），
    // 未命中时回退 models/ 根目录（内置权重）。
    const juce::File dirWeight = juce::File(modelDir)
        .getChildFile("vocoder_weights")
        .getChildFile(juce::String(weight));
    if (dirWeight.existsAsFile())
        return dirWeight.getFullPathName().toStdString();

    return modelDir + "/" + weight;
}

std::unique_ptr<VocoderDomain> ProcessRenderRuntime::createVocoderDomain(
    const VocoderModelWeight& weight, juce::String& errorMessage)
{
    // 该函数始终在 vocoderMutex_ 外执行。ORT Env 初始化、模型加载、Session
    // 创建和失败清理都可能耗时，绝不能阻塞 UI 的 detach/isVocoderReady。
    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    std::string f0Error;
    if (!ProcessF0Runtime::getInstance().initialize(modelsDir, &f0Error))
    {
        errorMessage = f0Error.empty()
            ? "F0 runtime initialization failed while creating the vocoder domain"
            : juce::String(f0Error);
        return nullptr;
    }

    auto env = ProcessF0Runtime::getInstance().getOrtEnv();
    if (env == nullptr)
    {
        errorMessage = "Vocoder domain could not acquire the ONNX Runtime environment";
        return nullptr;
    }

    auto domain = std::make_unique<VocoderDomain>(env);
    const auto modelPath = modelPathForWeight(modelsDir, weight);
    AppLogger::info("VocoderDomain: loading weight=" + juce::String(weight)
        + " modelPath=" + modelPath);
    std::string domainError;
    if (!domain->initialize(modelPath, &domainError))
    {
        errorMessage = domainError.empty()
            ? "Vocoder domain initialization failed without an error detail"
            : juce::String(domainError);
        return nullptr;
    }

    return domain;
}

ProcessRenderRuntime::ControlResult ProcessRenderRuntime::reconfigureVocoder(
    const ControlCommand& command,
    juce::String& errorMessage,
    std::vector<DeferredFailureNotification>& deferredFailureNotifications)
{
    std::unique_ptr<VocoderDomain> retiredDomain;
    VocoderModelWeight targetWeight;

    {
        std::unique_lock<std::mutex> lock(vocoderMutex_);

        if (shuttingDown_.load(std::memory_order_acquire))
            return ControlResult::Failed;

        if (command.type == ControlCommand::Type::SetVocoderWeight
            && currentVocoderModelWeight_ == command.weight)
            return ControlResult::Unchanged;

        if (command.type == ControlCommand::Type::EnsureVocoder
            && vocoderDomain_ != nullptr)
            return ControlResult::Unchanged;

        if (command.type == ControlCommand::Type::SetVocoderWeight)
            currentVocoderModelWeight_ = command.weight;

        targetWeight = currentVocoderModelWeight_;
        vocoderReconfiguring_ = true;
        retiredDomain = std::move(vocoderDomain_); // O(1)：锁内只摘除所有权
        ++vocoderGeneration_;                      // 立即拒绝全部旧配置快照

    }

    // 唯一可能无界的路径：只阻塞进程寿命 control worker，不持任何 runtime 锁。
    {
        std::unique_lock<std::mutex> lock(vocoderMutex_);
        domainSubmitCv_.wait(lock, [this] { return domainSubmitInFlight_ == 0; });
    }
    retiredDomain.reset();

    if (shuttingDown_.load(std::memory_order_acquire))
    {
        std::vector<DeferredRetry> releasedRetries;
        {
            std::lock_guard<std::mutex> lock(vocoderMutex_);
            vocoderReconfiguring_ = false;
            for (const auto& retry : deferredRetries_)
                releaseDeferredPayloadsLocked(retry);
            releasedRetries = std::move(deferredRetries_);
        }
        releasedRetries.clear();
        return ControlResult::Failed;
    }

    if (command.type == ControlCommand::Type::ResetInferenceBackend)
    {
        auto& detector = AccelerationDetector::getInstance();
        detector.resetAndDetect(command.forceCpu);
    }

    // 严格先销毁旧 Session，再创建新 Session；新旧显存不并存。
    auto newDomain = createVocoderDomain(targetWeight, errorMessage);

    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (!shuttingDown_.load(std::memory_order_acquire)
            && currentVocoderModelWeight_ == targetWeight)
        {
            vocoderDomain_ = std::move(newDomain);
            if (vocoderDomain_ != nullptr)
                ++vocoderGeneration_; // 新 domain 获得独立代际
        }
        vocoderReconfiguring_ = false;
    }
    bool domainAvailable = false;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        domainAvailable = !shuttingDown_.load(std::memory_order_acquire)
            && vocoderDomain_ != nullptr;
    }
    if (domainAvailable)
        resumeDeferredRetries();
    else if (!shuttingDown_.load(std::memory_order_acquire))
    {
        std::vector<DeferredRetry> failedRetries;
        {
            std::lock_guard<std::mutex> lock(vocoderMutex_);
            for (const auto& retry : deferredRetries_)
                releaseDeferredPayloadsLocked(retry);
            failedRetries = std::move(deferredRetries_);
        }
        for (const auto& retry : failedRetries)
        {
            const auto reason = errorMessage.isNotEmpty()
                ? errorMessage + "; deferred task: " + retry.reason
                : retry.reason;
            if (retry.job.renderCache != nullptr
                && retry.job.renderCache->completeChunkRenderFailure(
                    retry.job.startSample, retry.job.targetRevision))
            {
                notifyChunkFailed(retry.completion, retry.job.contentKey,
                                  retry.job.targetRevision, reason, false);
                deferredFailureNotifications.push_back({
                    retry.job.contentKey,
                    retry.completion.contentRevision,
                    retry.job.targetRevision,
                    reason,
                    retry.job.renderCache,
                    retry.completion.gate});
            }
        }
    }
    if (!domainAvailable && errorMessage.isEmpty())
        errorMessage = "Vocoder became unavailable during reconfiguration";
    return domainAvailable ? ControlResult::Changed : ControlResult::Failed;
}

bool ProcessRenderRuntime::postControlCommand(ControlCommand command)
{
    bool rejected = false;
    bool lifecycleRejected = false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (shuttingDown_.load(std::memory_order_acquire))
        {
            AppLogger::error("[ProcessRenderRuntime] rejected control command during shutdown; Failed");
            rejected = true;
            lifecycleRejected = true;
        }
        else
        {
            if (command.type != ControlCommand::Type::Stop
                && controlQueue_.size() >= ProcessRenderRuntime::kMaxControlQueueDepth)
            {
                AppLogger::error("[ProcessRenderRuntime] control queue full; Failed");
                rejected = true;
                if (command.type == ControlCommand::Type::EnsureVocoder)
                    deferredEnsureRetryPending_.store(true, std::memory_order_release);
            }
            else if (command.type == ControlCommand::Type::EnsureVocoder)
            {
                if (ensureVocoderQueued_)
                    return true;
                ensureVocoderQueued_ = true;
            }
            if (!rejected)
                controlQueue_.push_back(std::move(command));
        }
    }

    if (rejected)
    {
        auto transaction = std::move(command.transaction);
        auto completion = std::move(command.completion);
        auto completionGate = std::move(command.completionGate);
        auto finish = [transaction](const char* reason) mutable noexcept {
            finishControlTransaction(transaction, reason);
        };
        const bool posted = juce::MessageManager::callAsync(
            [completion = std::move(completion), completionGate, finish, transaction,
             lifecycleRejected]() mutable {
                if (!claimControlTransaction(transaction))
                    return;
                const auto invokeCompletion = [&]() {
                    try
                    {
                        if (completion)
                            completion(lifecycleRejected ? ControlResult::Unchanged : ControlResult::Failed,
                                lifecycleRejected
                                    ? juce::String{}
                                    : juce::String("Vocoder control queue is full"));
                    }
                    catch (...)
                    {
                        AppLogger::error("[ProcessRenderRuntime] rejected control completion threw");
                    }
                };
                if (completionGate != nullptr)
                {
                    CompletionCallbackLease callbackLease(*completionGate);
                    if (callbackLease)
                        invokeCompletion();
                }
                else
                {
                    invokeCompletion();
                }
                completeClaimedControlTransaction(transaction, "shutdown rejection");
            });
        if (!posted)
            finish("shutdown rejection dispatcher failure");
        return false;
    }
    controlCv_.notify_one();
    return true;
}

void ProcessRenderRuntime::controlWorkerLoop()
{
    while (true)
    {
        ControlCommand command;
        {
            std::unique_lock<std::mutex> lock(controlMutex_);
            controlCv_.wait(lock, [this]() { return !controlQueue_.empty(); });
            command = std::move(controlQueue_.front());
            controlQueue_.pop_front();
            if (command.type == ControlCommand::Type::EnsureVocoder)
                ensureVocoderQueued_ = false;
            activeTransaction_ = command.transaction;
        }
        if (command.type == ControlCommand::Type::Stop)
        {
            std::vector<DeferredRetry> deferred;
            std::unique_ptr<VocoderDomain> domain;
            {
                std::lock_guard<std::mutex> lock(vocoderMutex_);
                for (const auto& retry : deferredRetries_)
                    releaseDeferredPayloadsLocked(retry);
                deferred = std::move(deferredRetries_);
                domain = std::move(vocoderDomain_);
                vocoderReconfiguring_ = false;
            }

            // Stop uses the same submit-drain contract as reconfiguration.
            // The raw domain pointer in submitVocoderJob is only valid while
            // its in-flight counter is held.
            {
                std::unique_lock<std::mutex> lock(vocoderMutex_);
                domainSubmitCv_.wait(lock, [this]() {
                    return domainSubmitInFlight_ == 0;
                });
            }
            domain.reset();

            const auto generation = shutdownGeneration_.load(std::memory_order_acquire);
            const bool posted = juce::MessageManager::callAsync(
                [this, generation]() { finishShutdown(generation); });
            workerExitDispatchPosted_.store(posted, std::memory_order_release);
            workerExitDispatchAttempted_.store(true, std::memory_order_release);
            workerExitCv_.notify_all();
            if (!posted)
                AppLogger::error("[ProcessRenderRuntime] worker-exit dispatcher rejected; hard lifecycle failure");
            return;
        }

        ControlResult result = ControlResult::Failed;
        juce::String errorMessage;
        std::vector<DeferredFailureNotification> deferredFailureNotifications;
        try
        {
            if (command.transaction && command.transaction->crs)
            {
                command.transaction->crs->pauseRenderWorker();
                command.transaction->paused.store(true, std::memory_order_release);
            }
            result = reconfigureVocoder(command, errorMessage, deferredFailureNotifications);
            if (command.transaction && command.transaction->crs)
                command.transaction->crs->waitAsyncRenderJobs();
        }
        catch (const std::exception& e)
        {
            AppLogger::error("[ProcessRenderRuntime] control command failed: "
                + juce::String(e.what()));
            errorMessage = "Vocoder control command threw std::exception: " + juce::String(e.what());
            result = ControlResult::Failed;
        }
        catch (...)
        {
            AppLogger::error("[ProcessRenderRuntime] control command failed: unknown exception");
            errorMessage = "Vocoder control command threw an unknown exception";
            result = ControlResult::Failed;
        }

        if (command.type == ControlCommand::Type::EnsureVocoder)
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            ensureVocoderQueued_ = false;
        }
        if (command.type == ControlCommand::Type::EnsureVocoder)
            deferredEnsureRetryPending_.store(false, std::memory_order_release);
        else if (deferredEnsureRetryPending_.exchange(false, std::memory_order_acq_rel))
            postDeferredRetryEvent();

        auto transaction = std::move(command.transaction);
        auto completion = std::move(command.completion);
        auto completionGate = std::move(command.completionGate);
        const auto notifyDeferredFailures = [&]() {
            if (shuttingDown_.load(std::memory_order_acquire)
                || deferredFailureNotifications.empty())
                return;
            auto notificationClaim = std::make_shared<std::atomic<bool>>(false);
            for (auto& failure : deferredFailureNotifications)
                notifyApplicationRenderFailure(
                    failure.contentKey, failure.contentRevision, failure.targetRevision,
                    failure.reason, std::move(failure.renderCache), std::move(failure.gate),
                    notificationClaim);
        };
        const bool rootCompletionCanNotify = result == ControlResult::Failed
            && (command.type == ControlCommand::Type::SetVocoderWeight
                || command.type == ControlCommand::Type::ResetInferenceBackend)
            && static_cast<bool>(completion);
        auto rootCompletionDisplayed = std::make_shared<std::atomic<bool>>(false);
        if (!transaction && !completion)
        {
            notifyDeferredFailures();
            continue;
        }
        auto waitTransaction = transaction;
        const bool posted = juce::MessageManager::callAsync(
            [completion = std::move(completion), result, errorMessage, transaction,
             completionGate, rootCompletionCanNotify, rootCompletionDisplayed]() mutable {
                if (transaction && !claimControlTransaction(transaction))
                    return;
                const auto invokeCompletion = [&]() {
                    try
                    {
                        if (completion)
                        {
                            completion(result, errorMessage);
                            if (rootCompletionCanNotify)
                                rootCompletionDisplayed->store(true, std::memory_order_release);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        AppLogger::error("[ProcessRenderRuntime] control completion threw: "
                            + juce::String(e.what()));
                    }
                    catch (...)
                    {
                        AppLogger::error("[ProcessRenderRuntime] control completion threw unknown exception");
                    }
                };
                if (completionGate != nullptr)
                {
                    CompletionCallbackLease callbackLease(*completionGate);
                    if (callbackLease)
                        invokeCompletion();
                }
                else
                {
                    invokeCompletion();
                }
                completeClaimedControlTransaction(transaction, "message completion");
            });
        if (!posted)
        {
            AppLogger::error("[ProcessRenderRuntime] control completion dispatcher rejected; Failed close fallback");
            if (claimControlTransaction(transaction))
                completeClaimedControlTransaction(transaction, "dispatcher rejected / Failed");
        }
        else if (waitTransaction)
        {
            std::unique_lock<std::mutex> lock(waitTransaction->mutex);
            waitTransaction->cv.wait(lock, [this, &waitTransaction] {
                return waitTransaction->acked.load(std::memory_order_acquire)
                    || waitTransaction->closeRequested.load(std::memory_order_acquire);
            });
            const bool needsCloseFallback = !waitTransaction->acked.load(std::memory_order_acquire);
            lock.unlock();
            if (needsCloseFallback)
                finishControlTransaction(waitTransaction, "shutdown close fallback");
        }
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            if (activeTransaction_ == waitTransaction)
                activeTransaction_.reset();
        }
        const bool rootCompletionDisplayedSuccessfully = posted
            && rootCompletionDisplayed->load(std::memory_order_acquire);
        if (!posted || (waitTransaction != nullptr && !rootCompletionDisplayedSuccessfully))
            notifyDeferredFailures();
    }
}

void ProcessRenderRuntime::setVocoderModelWeight(const VocoderModelWeight& weight,
                                                 std::shared_ptr<ContentRenderService> crs,
                                                 ControlCompletion completion,
                                                 std::shared_ptr<CompletionGate> completionGate)
{
    // UI 线程只投递命令并立即返回；Session 销毁与重建由 control worker 串行执行。
    ControlCommand command;
    command.type = ControlCommand::Type::SetVocoderWeight;
    command.weight = weight;
    command.transaction = std::make_shared<ControlTransaction>();
    command.transaction->crs = std::move(crs);
    command.completion = std::move(completion);
    command.completionGate = std::move(completionGate);
    postControlCommand(std::move(command));
}

void ProcessRenderRuntime::resetVocoder(std::shared_ptr<ContentRenderService> crs,
                                        ControlCompletion completion)
{
    ControlCommand command;
    command.type = ControlCommand::Type::ResetVocoder;
    command.transaction = std::make_shared<ControlTransaction>();
    command.transaction->crs = std::move(crs);
    command.completion = std::move(completion);
    postControlCommand(std::move(command));
}

void ProcessRenderRuntime::resetInferenceBackend(bool forceCpu,
                                                 std::shared_ptr<ContentRenderService> crs,
                                                 ControlCompletion completion,
                                                 std::shared_ptr<CompletionGate> completionGate)
{
    ControlCommand command;
    command.type = ControlCommand::Type::ResetInferenceBackend;
    command.forceCpu = forceCpu;
    command.transaction = std::make_shared<ControlTransaction>();
    command.transaction->crs = std::move(crs);
    command.completion = std::move(completion);
    command.completionGate = std::move(completionGate);
    postControlCommand(std::move(command));
}

void ProcessRenderRuntime::detachDeferredJobs(
    void* owner, const std::shared_ptr<CompletionGate>& gate)
{
    std::vector<DeferredRetry> detached;
    std::unique_lock<std::mutex> gateLock;
    if (gate != nullptr)
        gateLock = std::unique_lock<std::mutex>(gate->mutex);

    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        auto it = std::remove_if(deferredRetries_.begin(), deferredRetries_.end(),
            [this, owner, &detached](DeferredRetry& retry) {
                if (retry.ownerIdentity != owner)
                    return false;
                releaseDeferredPayloadsLocked(retry);
                detached.push_back(std::move(retry));
                return true;
            });
        deferredRetries_.erase(it, deferredRetries_.end());
    }

    if (gateLock.owns_lock())
        gateLock.unlock();
    const bool removed = !detached.empty();
    detached.clear();
    if (removed)
        postDeferredRetryEvent();
}

ProcessRenderRuntime::VocoderSubmitResult ProcessRenderRuntime::submitVocoderJob(
    VocoderDomain::Job job, uint64_t expectedGeneration, juce::String& rejectionReason)
{
    VocoderDomain* domain = nullptr;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (vocoderDomain_ == nullptr || vocoderGeneration_ != expectedGeneration) {
            rejectionReason = "Vocoder submission rejected because the domain generation changed (expected="
                + juce::String(static_cast<juce::int64>(expectedGeneration))
                + ", actual=" + juce::String(static_cast<juce::int64>(vocoderGeneration_))
                + ", domainAvailable=" + juce::String(vocoderDomain_ != nullptr ? "true" : "false") + ")";
            return VocoderSubmitResult::Retryable;
        }
        ++domainSubmitInFlight_;
        domain = vocoderDomain_.get();
    }

    const size_t retainedBytes = job.hybridPayload != nullptr
        ? job.hybridPayload->retainedBytes() : 0;
    if (!VocoderRenderScheduler::isJobPayloadWithinLimit(
            job.f0.capacity(), job.uv.capacity(), job.conditioning.capacity(), retainedBytes))
    {
        {
            std::lock_guard<std::mutex> lock(vocoderMutex_);
            --domainSubmitInFlight_;
        }
        domainSubmitCv_.notify_all();
        return VocoderSubmitResult::PayloadTooLarge;
    }

    bool submitted = false;
    std::string submitError;
    try
    {
        submitted = domain->submit(std::move(job), &submitError);
    }
    catch (const std::exception& e)
    {
        submitError = "Vocoder domain submission threw std::exception: " + std::string(e.what());
    }
    catch (...)
    {
        submitError = "Vocoder domain submission threw an unknown exception";
    }

    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        --domainSubmitInFlight_;
    }
    domainSubmitCv_.notify_all();
    if (!submitted)
        rejectionReason = submitError.empty()
            ? "Vocoder domain rejected the job without an error detail"
            : juce::String(submitError);
    return submitted ? VocoderSubmitResult::Submitted : VocoderSubmitResult::Retryable;
}

bool ProcessRenderRuntime::acquireVocoderConfig(VocoderConfig& out)
{
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (vocoderDomain_ != nullptr)
        {
            out.generation = vocoderGeneration_;
            out.modelWeight = currentVocoderModelWeight_;
            out.conditioningBins = vocoderDomain_->getConditioningBins();
            out.conditioningType = vocoderDomain_->getConditioningType();
            out.fMax = vocoderDomain_->getFMax();
            out.melFilterbank = vocoderDomain_->getMelFilterbankSpec();
            out.melLogEps = vocoderDomain_->getMelLogEps();
            out.needsHighFrequencyOriginal = vocoderDomain_->needsHighFrequencyOriginal();
            return true;
        }

        if (vocoderReconfiguring_)
            return false;
    }

    ControlCommand command;
    command.type = ControlCommand::Type::EnsureVocoder;
    postControlCommand(std::move(command));
    return false;
}

bool ProcessRenderRuntime::isVocoderReady() const noexcept
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return vocoderDomain_ != nullptr;
}

VocoderModelWeight ProcessRenderRuntime::getVocoderModelWeight() const
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return currentVocoderModelWeight_;
}

bool ProcessRenderRuntime::isVocoderReconfiguring() const noexcept
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return vocoderReconfiguring_;
}

std::size_t ProcessRenderRuntime::controlQueueDepth() const noexcept
{
    std::lock_guard<std::mutex> lock(controlMutex_);
    return controlQueue_.size();
}

std::size_t ProcessRenderRuntime::deferredRetryCount() const noexcept
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return deferredRetries_.size();
}

bool ProcessRenderRuntime::hasDeferredRetriesForService(
    const ContentRenderService* service) const
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return std::any_of(deferredRetries_.begin(), deferredRetries_.end(),
        [service](const DeferredRetry& retry) {
            return retry.serviceIdentity == service;
        });
}

bool ProcessRenderRuntime::flushDeferredRetriesForService(
    const ContentRenderService* service)
{
    if (!hasDeferredRetriesForService(service))
        return true;
    return resumeDeferredRetries(service);
}

std::size_t ProcessRenderRuntime::deferredRetrySnapshotBytes() const
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return deferredSnapshotBytes_;
}

bool ProcessRenderRuntime::deferredSnapshotFitsLocked(
    const DeferredRetry& retry, std::size_t budgetBytes) const
{
    const auto limit = std::min(budgetBytes, kDeferredSnapshotBudgetBytes);
    if (deferredSnapshotBytes_ > limit)
        return false;
    std::size_t projected = deferredSnapshotBytes_;
    for (const auto& allocation : retry.payloads)
    {
        if (deferredPayloads_.find(allocation.identity) != deferredPayloads_.end())
            continue;
        if (allocation.bytes > limit - projected)
            return false;
        projected += allocation.bytes;
    }
    return true;
}

void ProcessRenderRuntime::retainDeferredPayloadsLocked(const DeferredRetry& retry)
{
    for (const auto& allocation : retry.payloads)
    {
        auto it = deferredPayloads_.try_emplace(
            allocation.identity, DeferredPayloadReference{allocation.bytes, 0}).first;
        if (it->second.references == 0)
            deferredSnapshotBytes_ += it->second.bytes;
        ++it->second.references;
    }
}

void ProcessRenderRuntime::releaseDeferredPayloadsLocked(const DeferredRetry& retry)
{
    for (const auto& allocation : retry.payloads)
    {
        const auto it = deferredPayloads_.find(allocation.identity);
        jassert(it != deferredPayloads_.end() && it->second.references > 0);
        if (--it->second.references == 0)
        {
            deferredSnapshotBytes_ -= it->second.bytes;
            deferredPayloads_.erase(it);
        }
    }
}

void ProcessRenderRuntime::postDeferredRetryEvent()
{
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (deferredRetries_.empty())
            return;
    }
    if (deferredRetryEventPosted_.exchange(true, std::memory_order_acq_rel))
        return;
    const bool posted = juce::MessageManager::callAsync([this]() {
        deferredRetryEventPosted_.store(false, std::memory_order_release);
        resumeDeferredRetries();
    });
    if (!posted)
        deferredRetryEventPosted_.store(false, std::memory_order_release);
}

void ProcessRenderRuntime::renderQueueCapacityAvailable()
{
    postDeferredRetryEvent();
}

void ProcessRenderRuntime::pruneDeferredJobsForCache(
    const std::shared_ptr<RenderCache>& cache)
{
    if (cache == nullptr)
        return;
    std::vector<DeferredRetry> released;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        auto it = std::remove_if(deferredRetries_.begin(), deferredRetries_.end(),
            [this, &released, cache](DeferredRetry& retry) {
                if (retry.job.renderCache != cache)
                    return false;
                releaseDeferredPayloadsLocked(retry);
                released.push_back(std::move(retry));
                return true;
            });
        deferredRetries_.erase(it, deferredRetries_.end());
    }
    const bool removed = !released.empty();
    released.clear();
    if (removed)
        postDeferredRetryEvent();
}

void ProcessRenderRuntime::pruneStaleDeferredJobsForCache(
    const std::shared_ptr<RenderCache>& cache)
{
    if (cache == nullptr)
        return;
    std::vector<DeferredRetry> released;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        auto it = std::remove_if(deferredRetries_.begin(), deferredRetries_.end(),
            [this, &released, &cache](DeferredRetry& retry) {
                if (retry.job.renderCache != cache)
                    return false;
                RenderCache::RunningChunkSnapshot running;
                if (cache->getRunningChunk(retry.job.startSample, running)
                    && running.startSample == retry.job.startSample
                    && running.endSampleExclusive == retry.job.endSampleExclusive
                    && running.runningRevision == retry.job.targetRevision)
                    return false;
                releaseDeferredPayloadsLocked(retry);
                released.push_back(std::move(retry));
                return true;
            });
        deferredRetries_.erase(it, deferredRetries_.end());
    }
    const bool removed = !released.empty();
    released.clear();
    if (removed)
        postDeferredRetryEvent();
}

void ProcessRenderRuntime::pruneDeferredJobsForService(const ContentRenderService* service)
{
    std::vector<DeferredRetry> released;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        auto it = std::remove_if(deferredRetries_.begin(), deferredRetries_.end(),
            [this, &released, service](DeferredRetry& retry) {
                if (retry.serviceIdentity != service)
                    return false;
                releaseDeferredPayloadsLocked(retry);
                released.push_back(std::move(retry));
                return true;
            });
        deferredRetries_.erase(it, deferredRetries_.end());
    }
    const bool removed = !released.empty();
    released.clear();
    if (removed)
        postDeferredRetryEvent();
}

bool ProcessRenderRuntime::resumeDeferredRetries(
    const ContentRenderService* serviceIdentity)
{
    struct DeferredCandidate {
        std::weak_ptr<ContentRenderService> crs;
        RenderJob job;
        CompletionContext completion;
        const ContentRenderService* serviceIdentity{nullptr};
        uint64_t audioRevision{0};
        int audioSamples{0};
        int audioChannels{0};
    };
    std::vector<DeferredCandidate> snapshot;
    bool domainAvailable = false;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (shuttingDown_.load(std::memory_order_acquire) || vocoderReconfiguring_)
            return false;
        if (deferredRetries_.empty())
            return false;
        domainAvailable = vocoderDomain_ != nullptr;
        if (domainAvailable)
        {
            snapshot.reserve(deferredRetries_.size());
            for (const auto& retry : deferredRetries_)
            {
                if (serviceIdentity != nullptr && retry.serviceIdentity != serviceIdentity)
                    continue;
                DeferredCandidate candidate;
                candidate.crs = retry.crs;
                candidate.job.kind = retry.job.kind;
                candidate.job.contentKey = retry.job.contentKey;
                candidate.job.renderCache = retry.job.renderCache;
                candidate.job.audioSampleRate = retry.job.audioSampleRate;
                candidate.job.startSample = retry.job.startSample;
                candidate.job.endSampleExclusive = retry.job.endSampleExclusive;
                candidate.job.queuedChunkStartSample = retry.job.queuedChunkStartSample;
                candidate.job.contentRevision = retry.job.contentRevision;
                candidate.job.targetRevision = retry.job.targetRevision;
                candidate.completion = retry.completion;
                candidate.serviceIdentity = retry.serviceIdentity;
                candidate.audioRevision = retry.audioRevision;
                candidate.audioSamples = retry.audioSamples;
                candidate.audioChannels = retry.audioChannels;
                snapshot.push_back(std::move(candidate));
            }
        }
    }
    if (!domainAvailable)
    {
        ControlCommand command;
        command.type = ControlCommand::Type::EnsureVocoder;
        postControlCommand(std::move(command));
        return false;
    }

    bool removedAny = false;
    bool admittedAny = false;
    for (const auto& candidate : snapshot)
    {
        auto crs = candidate.crs.lock();
        std::vector<DeferredRetry> released;
        std::vector<RenderJob> readyJobs;
        std::unique_lock<std::mutex> gateLock;
        if (candidate.completion.gate != nullptr)
            gateLock = std::unique_lock<std::mutex>(candidate.completion.gate->mutex);
        const bool gateClosed = candidate.completion.gate != nullptr
            && candidate.completion.gate->closed;
        auto job = candidate.job;

        {
            std::unique_lock<std::mutex> stateLock(vocoderMutex_);
            if (shuttingDown_.load(std::memory_order_acquire)
                || vocoderReconfiguring_ || vocoderDomain_ == nullptr)
                continue;

            const auto removeMatching = [&]() {
                auto it = std::find_if(deferredRetries_.begin(), deferredRetries_.end(),
                    [&candidate](const DeferredRetry& current) {
                        return current.serviceIdentity == candidate.serviceIdentity
                            && current.job.renderCache == candidate.job.renderCache
                            && current.job.startSample == candidate.job.startSample
                            && current.job.targetRevision == candidate.job.targetRevision;
                    });
                if (it != deferredRetries_.end()) {
                    releaseDeferredPayloadsLocked(*it);
                    released.push_back(std::move(*it));
                    deferredRetries_.erase(it);
                    removedAny = true;
                }
            };

            if (gateClosed || crs == nullptr)
            {
                removeMatching();
            }
            else
            {
                auto it = std::find_if(deferredRetries_.begin(), deferredRetries_.end(),
                    [&candidate](const DeferredRetry& current) {
                        return current.serviceIdentity == candidate.serviceIdentity
                            && current.job.renderCache == candidate.job.renderCache
                            && current.job.startSample == candidate.job.startSample
                            && current.job.targetRevision == candidate.job.targetRevision;
                    });
                if (it != deferredRetries_.end())
                {
                    const auto sourceStatus = crs->restoreDeferredRenderJob(
                        job, candidate.audioRevision, candidate.audioSamples, candidate.audioChannels);
                    if (sourceStatus == ContentRenderService::DeferredSourceStatus::Stale)
                    {
                        removeMatching();
                    }
                    else if (sourceStatus == ContentRenderService::DeferredSourceStatus::Ready)
                    {
                        it->completion.contentRevision = job.contentRevision;

                        DeferredRetry restoredPayloads;
                        for (const auto& allocation : snapshotPayloadFootprint(
                                 job.contentSnapshot, job.audioBuffer))
                            restoredPayloads.payloads.push_back(
                                {allocation.first, allocation.second});
                        if (deferredSnapshotFitsLocked(
                                restoredPayloads, kDeferredSnapshotBudgetBytes))
                        {
                            retainDeferredPayloadsLocked(restoredPayloads);
                            readyJobs.push_back(job);
                            const auto results = crs->requeueRenderChunks(readyJobs);
                            const auto result = results.front();
                            admittedAny = result == RenderWorker::RequeueResult::Requeued
                                || admittedAny;
                            if (result != RenderWorker::RequeueResult::QueueFull
                                && result != RenderWorker::RequeueResult::LeaseUnavailable)
                                removeMatching();
                            releaseDeferredPayloadsLocked(restoredPayloads);
                        }
                    }
                }
            }
        }
        if (gateLock.owns_lock())
            gateLock.unlock();
        released.clear();
        readyJobs.clear();
    }
    snapshot.clear();
    if (removedAny)
        postDeferredRetryEvent();
    return removedAny || admittedAny;
}

bool ProcessRenderRuntime::hasActiveTransaction() const noexcept
{
    std::lock_guard<std::mutex> lock(controlMutex_);
    return activeTransaction_ != nullptr;
}

int ProcessRenderRuntime::domainSubmitInFlight() const noexcept
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return domainSubmitInFlight_;
}

uint64_t ProcessRenderRuntime::vocoderGeneration() const noexcept
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return vocoderGeneration_;
}

int ProcessRenderRuntime::ownerCount() const noexcept
{
    std::lock_guard<std::mutex> lock(shutdownMutex_);
    return ownerCount_;
}

bool ProcessRenderRuntime::isControlWorkerJoinable() const noexcept
{
    std::lock_guard<std::mutex> lock(shutdownMutex_);
    return controlWorker_.joinable();
}

void ProcessRenderRuntime::deferOrRequeue(
    std::shared_ptr<ContentRenderService> crs,
    RenderJob job,
    CompletionContext completion,
    juce::String reason,
    std::size_t snapshotBudgetBytes)
{
    if (crs == nullptr || job.renderCache == nullptr)
        return;

    const auto audioRevision = job.contentSnapshot != nullptr
        ? job.contentSnapshot->audioRevision : 0;
    const int audioSamples = job.audioBuffer != nullptr
        ? job.audioBuffer->getNumSamples()
        : (job.contentSnapshot != nullptr && job.contentSnapshot->audioBuffer != nullptr
            ? job.contentSnapshot->audioBuffer->getNumSamples() : 0);
    const int audioChannels = job.audioBuffer != nullptr
        ? job.audioBuffer->getNumChannels()
        : (job.contentSnapshot != nullptr && job.contentSnapshot->audioBuffer != nullptr
            ? job.contentSnapshot->audioBuffer->getNumChannels() : 0);
    auto sourceStatus = crs->restoreDeferredRenderJob(
        job, audioRevision, audioSamples, audioChannels);
    if (sourceStatus == ContentRenderService::DeferredSourceStatus::Stale)
        return;

    crs->setDeferredRetryCapacityCallback([this]() { renderQueueCapacityAvailable(); });
    std::unique_lock<std::mutex> gateLock;
    if (completion.gate != nullptr)
    {
        gateLock = std::unique_lock<std::mutex>(completion.gate->mutex);
        if (completion.gate->closed)
            return;
    }

    DeferredRetry retry;
    retry.crs = crs;
    retry.job = std::move(job);
    retry.completion = std::move(completion);
    retry.ownerIdentity = crs.get();
    retry.serviceIdentity = crs.get();
    retry.audioRevision = audioRevision;
    retry.audioSamples = audioSamples;
    retry.audioChannels = audioChannels;
    retry.reason = std::move(reason);
    if (sourceStatus == ContentRenderService::DeferredSourceStatus::Ready)
        retry.completion.contentRevision = retry.job.contentRevision;
    for (const auto& allocation : snapshotPayloadFootprint(
             retry.job.contentSnapshot, retry.job.audioBuffer))
        retry.payloads.push_back({allocation.first, allocation.second});

    std::vector<DeferredRetry> released;
    std::shared_ptr<const EditableContentSnapshot> droppedSnapshot;
    std::shared_ptr<const juce::AudioBuffer<float>> droppedAudio;
    bool shouldEnsureVocoder = false;
    bool shouldPostRetry = false;
    bool releasedDeferredEntry = false;
    {
        std::unique_lock<std::mutex> lock(vocoderMutex_);
        if (shuttingDown_.load(std::memory_order_acquire))
            return;
        if (!crs->isCurrentRenderCache(retry.job.contentKey, retry.job.renderCache))
            return;

        RenderCache::RunningChunkSnapshot running;
        if (!retry.job.renderCache->getRunningChunk(retry.job.startSample, running)
            || running.endSampleExclusive != retry.job.endSampleExclusive
            || running.runningRevision != retry.job.targetRevision)
            return;

        const bool domainAvailable = vocoderDomain_ != nullptr && !vocoderReconfiguring_;
        if (domainAvailable && sourceStatus == ContentRenderService::DeferredSourceStatus::Ready)
        {
            if (gateLock.owns_lock())
                gateLock.unlock();
            const auto result = crs->requeueRenderChunk(retry.job);
            if (result == RenderWorker::RequeueResult::Requeued)
            {
                const auto previous = std::find_if(deferredRetries_.begin(), deferredRetries_.end(),
                    [&retry](const DeferredRetry& current) {
                        return current.job.renderCache == retry.job.renderCache
                            && current.job.startSample == retry.job.startSample;
                    });
                if (previous != deferredRetries_.end())
                {
                    releaseDeferredPayloadsLocked(*previous);
                    released.push_back(std::move(*previous));
                    deferredRetries_.erase(previous);
                    releasedDeferredEntry = true;
                }
                lock.unlock();
                released.clear();
                if (releasedDeferredEntry)
                    postDeferredRetryEvent();
                return;
            }
            if (result != RenderWorker::RequeueResult::QueueFull
                && result != RenderWorker::RequeueResult::LeaseUnavailable)
            {
                const auto stale = std::find_if(deferredRetries_.begin(), deferredRetries_.end(),
                    [&retry](const DeferredRetry& current) {
                        return current.job.renderCache == retry.job.renderCache
                            && current.job.startSample == retry.job.startSample
                            && current.job.targetRevision == retry.job.targetRevision;
                    });
                if (stale != deferredRetries_.end())
                {
                    releaseDeferredPayloadsLocked(*stale);
                    released.push_back(std::move(*stale));
                    deferredRetries_.erase(stale);
                    releasedDeferredEntry = true;
                }
                lock.unlock();
                released.clear();
                if (releasedDeferredEntry)
                    postDeferredRetryEvent();
                return;
            }
        }

        const auto existing = std::find_if(deferredRetries_.begin(), deferredRetries_.end(),
            [&retry](const DeferredRetry& current) {
                return current.job.renderCache == retry.job.renderCache
                    && current.job.startSample == retry.job.startSample;
            });
        if (existing != deferredRetries_.end())
        {
            releaseDeferredPayloadsLocked(*existing);
            released.push_back(std::move(*existing));
            deferredRetries_.erase(existing);
        }

        if (!deferredSnapshotFitsLocked(retry, snapshotBudgetBytes))
        {
            droppedSnapshot = std::move(retry.job.contentSnapshot);
            droppedAudio = std::move(retry.job.audioBuffer);
            retry.payloads.clear();
        }
        else
            retainDeferredPayloadsLocked(retry);
        deferredRetries_.push_back(std::move(retry));
        shouldEnsureVocoder = vocoderDomain_ == nullptr && !vocoderReconfiguring_;
        shouldPostRetry = !vocoderReconfiguring_ && !shouldEnsureVocoder
            && sourceStatus == ContentRenderService::DeferredSourceStatus::Ready;
    }
    if (gateLock.owns_lock())
        gateLock.unlock();
    released.clear();
    droppedSnapshot.reset();
    droppedAudio.reset();
    if (shouldEnsureVocoder)
    {
        ControlCommand command;
        command.type = ControlCommand::Type::EnsureVocoder;
        postControlCommand(std::move(command));
    }
    else if (shouldPostRetry)
        postDeferredRetryEvent();
}

void ProcessRenderRuntime::processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                                                  RenderJob& job,
                                                  bool lightPitchEnabled,
                                                  CompletionContext completion)
{
    completion.contentRevision = job.contentRevision;
    completion.renderCache = job.renderCache;
    const auto failureCache = job.renderCache;
    const auto failureStartSample = job.startSample;
    const auto failureRevision = job.targetRevision;
    const auto failureContentKey = job.contentKey;
    const auto failFrozen = [&](const juce::String& reason) {
        failChunk(completion, failureCache.get(), failureStartSample,
                  failureRevision, failureContentKey, reason);
    };

    if (shuttingDown_.load(std::memory_order_acquire))
    {
        if (failureCache != nullptr)
            failureCache->completeChunkRenderFailure(failureStartSample, failureRevision);
        return;
    }

    std::shared_ptr<RenderWorker::AsyncState> asyncCounter;
    bool asyncSubmitted = false;
    try
    {
    if (crs == nullptr || job.renderCache == nullptr || !job.contentSnapshot || !job.audioBuffer)
    {
        failFrozen("Render precondition failed: "
            + juce::String(crs == nullptr ? "ContentRenderService is unavailable; " : "")
            + juce::String(job.renderCache == nullptr ? "RenderCache is unavailable; " : "")
            + juce::String(!job.contentSnapshot ? "content snapshot is unavailable; " : "")
            + juce::String(!job.audioBuffer ? "audio buffer is unavailable" : ""));
        return;
    }

    const auto contentSnap = job.contentSnapshot;

    auto pitchCurve = contentSnap->pitchCurve;

    std::vector<float> monoAudio;
    std::vector<float> effectiveF0;
    std::vector<float> vocoderF0;
    std::vector<float> vocoderUv;

    const double relChunkStartSec = TimeCoordinate::samplesToSeconds(
        job.startSample, RenderCache::kSampleRate);
    auto coreJob = std::move(job);
    FrozenRenderBoundaries boundaries;
    int numFrames = 0;
    bool clipFound = false;
    bool boundariesFrozen = false;
    VocoderConfig vocoderCfg;

    if (coreJob.audioBuffer != nullptr)
    {
        // 边界冻结使用唯一渲染 hop（RenderChunkPlanner::kRenderHopSize），与
        // Vocoder domain 配置无关；vocoder 配置延后到真实 mel/vocoder 分支前获取。
        const int audioNumSamples = coreJob.audioBuffer->getNumSamples();
        const int audioNumChannels = coreJob.audioBuffer->getNumChannels();

        ContentSampleRange contentRange{0, audioNumSamples};
        if (freezeRenderBoundaries(contentRange,
                                   coreJob.startSample,
                                   coreJob.endSampleExclusive,
                                   RenderChunkPlanner::kRenderHopSize,
                                   boundaries))
        {
            boundariesFrozen = true;
            if (audioNumChannels > 0)
            {
                numFrames = boundaries.frameCount;
                monoAudio.resize(static_cast<size_t>(boundaries.synthSampleCount), 0.0f);
                const float* ch0 = coreJob.audioBuffer->getReadPointer(0);
                for (int64_t i = 0; i < boundaries.publishSampleCount; ++i)
                    monoAudio[static_cast<size_t>(i)] = ch0[static_cast<int>(boundaries.trueStartSample + i)];
                ChannelLayoutLog::logChunkRender(
                    static_cast<juce::int64>(coreJob.contentKey.objectId),
                    audioNumChannels);
                clipFound = true;
            }
        }
    }
    if (!boundariesFrozen)
    {
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.contentKey,
                  "Render chunk boundaries could not be frozen (startSample="
                    + juce::String(coreJob.startSample)
                    + ", endSampleExclusive=" + juce::String(coreJob.endSampleExclusive)
                    + ", audioSamples=" + juce::String(coreJob.audioBuffer->getNumSamples())
                    + ", hopSize=" + juce::String(RenderChunkPlanner::kRenderHopSize) + ")");
        return;
    }
    if (coreJob.audioBuffer->getNumChannels() <= 0)
    {
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.contentKey,
                  "Render audio buffer has no channels (channels=0)");
        return;
    }

    // 契约 §6：边界冻结后立即计算当前 chunk 是否与 active-EQ Note 相交。
    // 带 active EQ 的 Note 不得走 Blank：原 Blank/失败条件中若相交则发布原始
    // monoAudio 的 publishSampleCount 部分并应用 EQ。
    const bool intersectsActiveEqNote = chunkIntersectsActiveEqNote(
        contentSnap->notes, boundaries);

    // 原始路径发布：monoAudio 的 publishSampleCount 部分 + per-note EQ（契约 §6）
    const auto publishRawWithEq = [&]() {
        std::vector<float> rawAudio(
            monoAudio.begin(),
            monoAudio.begin() + static_cast<size_t>(boundaries.publishSampleCount));
        const auto rawAudioSamples = rawAudio.size();
        const auto result = publishChunkWithPerNoteEq(
            *coreJob.renderCache, boundaries, std::move(rawAudio),
            coreJob.targetRevision, contentSnap->notes);
        if (result == RenderCache::ChunkRenderResult::InvalidInput
            || result == RenderCache::ChunkRenderResult::MemoryLimitExceeded) {
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.contentKey,
                      result == RenderCache::ChunkRenderResult::InvalidInput
                          ? "RenderCache rejected raw audio as invalid input (startSample="
                              + juce::String(boundaries.trueStartSample) + ", endSampleExclusive="
                              + juce::String(boundaries.trueEndSample) + ", expectedSamples="
                              + juce::String(boundaries.publishSampleCount) + ", actualSamples="
                              + juce::String(static_cast<juce::int64>(rawAudioSamples))
                              + ", revision=" + juce::String(static_cast<juce::int64>(coreJob.targetRevision)) + ")"
                          : "RenderCache could not publish raw audio because its memory limit was exceeded (samples="
                              + juce::String(static_cast<juce::int64>(rawAudioSamples)) + ")");
        } else if (result == RenderCache::ChunkRenderResult::Published)
            notifyChunkSettled(completion, coreJob);
    };
    const auto settleBlankChunk = [&]() {
        const auto result = coreJob.renderCache->markChunkAsBlank(
            coreJob.startSample, coreJob.targetRevision);
        if (result == RenderCache::ChunkRenderResult::Published)
            notifyChunkSettled(completion, coreJob);
        else if (result == RenderCache::ChunkRenderResult::MemoryLimitExceeded)
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.contentKey,
                      "RenderCache could not publish the blank-settled state because its memory limit was exceeded");
    };

    if (!clipFound || monoAudio.empty() || numFrames <= 0)
    {
        // 无原始音频可发布：保持既有失败语义
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.contentKey,
                  "Render audio chunk could not be prepared for publication (clipFound="
                    + juce::String(clipFound ? "true" : "false")
                    + ", monoSamples=" + juce::String(static_cast<juce::int64>(monoAudio.size()))
                    + ", frames=" + juce::String(numFrames) + ")");
        return;
    }

    if (pitchCurve == nullptr)
    {
        if (intersectsActiveEqNote)
            publishRawWithEq();
        else
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.contentKey,
                      "Render input has no pitch curve and the chunk contains no active-EQ Note");
        return;
    }

    const double trueStartSeconds = TimeCoordinate::samplesToSeconds(boundaries.trueStartSample,
                                                                     TimeCoordinate::kRenderSampleRate);
    const double trueEndSeconds = TimeCoordinate::samplesToSeconds(boundaries.trueEndSample,
                                                                   TimeCoordinate::kRenderSampleRate);
    const double hopDuration = static_cast<double>(boundaries.hopSize) / RenderCache::kSampleRate;

    if (!pitchCurve->hasOriginalF0Data())
    {
        if (!intersectsActiveEqNote)
            settleBlankChunk();
        else
        {
            publishRawWithEq();
        }
        return;
    }

    const int f0HopSize = pitchCurve->getHopSize();
    const double f0SampleRate = pitchCurve->getSampleRate();
    if (f0HopSize <= 0 || !std::isfinite(f0SampleRate) || f0SampleRate <= 0.0)
    {
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.contentKey,
                  "OriginalF0 coordinates are invalid (hopSize=" + juce::String(f0HopSize)
                    + ", sampleRate=" + juce::String(f0SampleRate, 6) + ")");
        return;
    }

    const double f0FrameRate = f0SampleRate / static_cast<double>(f0HopSize);
    const F0Timeline f0Timeline(f0HopSize,
                                f0SampleRate,
                                static_cast<int>(pitchCurve->getOriginalF0().size()));
    const double f0TimelineEndSeconds = f0Timeline.timeAtFrame(
        static_cast<int>(pitchCurve->getOriginalF0().size()));
    if (trueEndSeconds <= 0.0 || trueStartSeconds >= f0TimelineEndSeconds)
    {
        if (!intersectsActiveEqNote)
            settleBlankChunk();
        else
        {
            publishRawWithEq();
        }
        return;
    }
    const auto contentF0Range = f0Timeline.rangeForTimes(trueStartSeconds, trueEndSeconds);
    const int f0StartFrame = contentF0Range.startFrame;
    // One extra frame is operational interpolation context. It is not part of
    // the chunk's absolute content range or published output length.
    const int f0EndFrame = std::min(
        static_cast<int>(pitchCurve->getOriginalF0().size()),
        contentF0Range.endFrameExclusive + 1);
    const int numF0Frames = std::max(1, f0EndFrame - f0StartFrame);
    const double firstSampleFramePhase = trueStartSeconds * f0FrameRate - static_cast<double>(f0StartFrame);

    // Blank 判定：仅当全局移调为恒等（effectiveF0 与 originalF0 一致，无差异可
    // 合成）且该 chunk 帧范围内无任何 correction segment 时，才 Blank 回退原始
    // 音频缓存播放。非恒等全局移调即使无 correction 也必须进入 vocoder 全量渲染，
    // 否则移调不生效。带 active EQ 的 Note 不得走 Blank：改为发布原始音频并应用 EQ。
    if (contentSnap->pitchShiftSettings.isIdentity() && !pitchCurve->hasCorrectionInRange(f0StartFrame, f0EndFrame))
    {
        if (!intersectsActiveEqNote)
            settleBlankChunk();
        else
        {
            publishRawWithEq();
        }
        return;
    }

    effectiveF0 = materializeEffectiveF0Range(*contentSnap, f0StartFrame, f0EndFrame);

    bool hasValidF0 = false;
    for (float f : effectiveF0)
    {
        if (f > 0.0f)
        {
            hasValidF0 = true;
            break;
        }
    }

    if (!hasValidF0)
    {
        if (!intersectsActiveEqNote)
            settleBlankChunk();
        else
        {
            publishRawWithEq();
        }
        return;
    }

    if (lightPitchEnabled && contentSnap->pitchShiftSettings.isIdentity())
    {
        const auto& originalF0Full = pitchCurve->getOriginalF0();
        const int originalF0Size = static_cast<int>(originalF0Full.size());
        if (f0StartFrame >= 0 && f0StartFrame < originalF0Size)
        {
            const bool needsVocoder = chunkNeedsVocoder(
                effectiveF0.data(), numF0Frames, originalF0Full, f0StartFrame);
            if (!needsVocoder)
            {
                AutoTunePitchShifter autoTuneShifter(RenderCache::kSampleRate);
                const int safeNumF0Frames = std::min(numF0Frames, originalF0Size - f0StartFrame);

                // Supply the source context needed by the pitch resampler:
                // preceding waveform history for cycle jumps and five future
                // samples for zero-latency interpolation.
                float shifterLookahead[AutoTunePitchShifter::kLookaheadSamples] = {};
                int shifterLookaheadSamples = 0;
                const float* shifterLookbehind = nullptr;
                int shifterLookbehindSamples = 0;
                {
                    const int64_t clipNumSamples = coreJob.audioBuffer->getNumSamples();
                    const int64_t tailStart = boundaries.trueStartSample + boundaries.publishSampleCount;
                    const float* clipCh0 = coreJob.audioBuffer->getReadPointer(0);
                    const int64_t avail = std::clamp<int64_t>(
                        clipNumSamples - tailStart, 0, AutoTunePitchShifter::kLookaheadSamples);
                    shifterLookaheadSamples = static_cast<int>(avail);
                    for (int64_t i = 0; i < avail; ++i)
                        shifterLookahead[i] = clipCh0[tailStart + i];

                    // 周期检测器需要完整 coarse window 及其因果 FIR 历史，
                    // 其固定上下文长度由 detector 自身声明。
                    const int lookbehindCapacity =
                        AutoTunePeriodDetector::kRequiredLookbehindSamples
                        + AutoTunePitchShifter::kLookaheadSamples + 2;
                    const int64_t lookbehindStart = std::max<int64_t>(
                        0, boundaries.trueStartSample - lookbehindCapacity);
                    shifterLookbehind = clipCh0 + lookbehindStart;
                    shifterLookbehindSamples = static_cast<int>(
                        boundaries.trueStartSample - lookbehindStart);
                }

                // Chunk-local 双阶段周期检测：downsampled lag 粗搜 + full-rate
                // V=E-2H 局部精搜，逐样本输出 period/valid 影子源交给 shifter。
                // FCPE F0 hint provides an octave prior for coarse acquisition.
                const int detectorNumSamples =
                    static_cast<int>(boundaries.publishSampleCount);
                // UV/V is decided by the waveform detector itself.  RMVPE's
                // originalF0 remains the editable source/target track, but it
                // must not gate the AutoTune detector's periodicity analysis.
                const float* f0HintPtr = originalF0Full.data() + f0StartFrame;
                const std::vector<AutoTunePeriodDetector::DetectedPeriod>
                    detectorFrames = AutoTunePeriodDetector::analyze(
                        shifterLookbehind, shifterLookbehindSamples,
                        monoAudio.data(), detectorNumSamples,
                        RenderCache::kSampleRate,
                        f0HintPtr, safeNumF0Frames, f0FrameRate);

                // snapMode：所有音符 retuneSpeed 均为 1.0 时，DSP 跳过 EMA 直接锁定
                bool snapMode = true;
                for (const auto& n : contentSnap->notes) {
                    if (n.retuneSpeed < 1.0f) { snapMode = false; break; }
                }

                auto shiftedAudio = autoTuneShifter.shiftChunk(
                    monoAudio.data(),
                    static_cast<int>(boundaries.publishSampleCount),
                    originalF0Full.data() + f0StartFrame,
                    effectiveF0.data(),
                    safeNumF0Frames,
                    f0FrameRate,
                    firstSampleFramePhase,
                    shifterLookahead,
                    shifterLookaheadSamples,
                    shifterLookbehind,
                    shifterLookbehindSamples,
                    detectorFrames.data(),
                    detectorNumSamples,
                    snapMode);
                const auto shiftedAudioSamples = shiftedAudio.size();

                const uint64_t objectId = coreJob.contentKey.objectId;
                const auto result = publishChunkWithPerNoteEq(
                    *coreJob.renderCache, boundaries, std::move(shiftedAudio),
                    coreJob.targetRevision, contentSnap->notes);

                if (result == RenderCache::ChunkRenderResult::InvalidInput
                    || result == RenderCache::ChunkRenderResult::MemoryLimitExceeded)
                {
                    failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                              coreJob.targetRevision, coreJob.contentKey,
                              result == RenderCache::ChunkRenderResult::InvalidInput
                                  ? "RenderCache rejected AutoTune audio as invalid input (startSample="
                                      + juce::String(boundaries.trueStartSample) + ", endSampleExclusive="
                                      + juce::String(boundaries.trueEndSample) + ", expectedSamples="
                                      + juce::String(boundaries.publishSampleCount) + ", actualSamples="
                                      + juce::String(static_cast<juce::int64>(shiftedAudioSamples))
                                      + ", revision=" + juce::String(static_cast<juce::int64>(coreJob.targetRevision)) + ")"
                                  : "RenderCache could not publish AutoTune audio because its memory limit was exceeded (samples="
                                      + juce::String(static_cast<juce::int64>(shiftedAudioSamples)) + ")");
                }
                else if (result == RenderCache::ChunkRenderResult::Published)
                {
                    notifyChunkSettled(completion, coreJob);
                }

                AppLogger::debug("RenderWorker: AutoTune pitch-shift chunk objId="
                    + juce::String(static_cast<juce::int64>(objectId))
                    + " start=" + juce::String(relChunkStartSec, 3));
                return;
            }
        }
    }

    // 真实条件谱/vocoder 分支前唯一一次锁内获取 domain 配置
    // （generation/conditioningBins/conditioningType/fMax）：raw 四分支与 light
    // 路径已在上方早退，绝不触发模型加载。配置与提交校验的 generation 同属一个
    // domain，不存在跨域混用。
    if (!acquireVocoderConfig(vocoderCfg))
    {
        RenderJob requeueJob;
        requeueJob.kind = RenderJob::Kind::Stage1Render;
        requeueJob.contentKey = coreJob.contentKey;
        requeueJob.renderCache = coreJob.renderCache;
        requeueJob.contentSnapshot = contentSnap;
        requeueJob.contentRevision = coreJob.contentRevision;
        requeueJob.audioBuffer = coreJob.audioBuffer;
        requeueJob.audioSampleRate = coreJob.audioSampleRate;
        requeueJob.startSample = coreJob.startSample;
        requeueJob.endSampleExclusive = coreJob.endSampleExclusive;
        requeueJob.queuedChunkStartSample = coreJob.startSample;
        requeueJob.targetRevision = coreJob.targetRevision;
        deferOrRequeue(crs, std::move(requeueJob), completion,
            "Vocoder configuration is unavailable because the domain is initializing or being reconfigured");
        return;
    }

    const int conditioningBins = vocoderCfg.conditioningBins;
    const float fMax = vocoderCfg.fMax;
    if (conditioningBins <= 0)
    {
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.contentKey,
                  "Vocoder conditioning bin count is invalid (bins="
                    + juce::String(conditioningBins) + ")");
        return;
    }

    MelSpectrogramConfig conditioningConfig;
    conditioningConfig.sampleRate = static_cast<int>(RenderCache::kSampleRate);
    conditioningConfig.nMels = conditioningBins;
    conditioningConfig.fMax = fMax;
    conditioningConfig.melFilterbank = vocoderCfg.melFilterbank;
    conditioningConfig.logEps = vocoderCfg.melLogEps;

    const bool linearSpec = vocoderCfg.conditioningType == VocoderConditioningType::LogLinearSpec;
    std::vector<float> conditioning;
    int actualFrames = 0;
    std::shared_ptr<const HybridRenderPayload> hybridPayload;
    const bool usesHnsepDdspGenerator =
        coreJob.contentKey.domainKind == DomainKind::StandaloneClip
        && vocoderCfg.modelWeight == kVocoderWeightDdspHarmonicGenerator;
    if (vocoderCfg.needsHighFrequencyOriginal || usesHnsepDdspGenerator)
    {
        if (linearSpec)
        {
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.contentKey,
                      "High-frequency original rendering requires a LogMel vocoder");
            return;
        }

        // Hybrid model, current-training-config estimate: ~14 mel-frame convolutional
        // receptive-field radius (conv_pre + five upsampling ResBlock1 stages).
        constexpr int hybridContextFrames = 15;
        const auto& originalF0 = pitchCurve->getOriginalF0();
        auto sourceF0Track = originalF0;
        OpenTune::fillF0GapsForVocoder(sourceF0Track);
        auto effectiveTrack = materializeEffectiveF0Range(
            *contentSnap, 0, static_cast<int>(originalF0.size()));
        OpenTune::fillF0GapsForVocoder(effectiveTrack, pitchCurve->getErasedRanges());

        const int64_t clipSamples = coreJob.audioBuffer->getNumSamples();
        const int64_t clipFrames = (clipSamples + RenderChunkPlanner::kRenderHopSize - 1)
            / RenderChunkPlanner::kRenderHopSize;
        const int64_t contextStartFrame = std::max<int64_t>(0,
            boundaries.trueStartSample / RenderChunkPlanner::kRenderHopSize - hybridContextFrames);
        const int64_t contextEndFrame = std::min<int64_t>(clipFrames,
            (boundaries.trueEndSample + RenderChunkPlanner::kRenderHopSize - 1)
                / RenderChunkPlanner::kRenderHopSize + hybridContextFrames);
        const int contextFrames = static_cast<int>(contextEndFrame - contextStartFrame);
        const auto& erasedRanges = pitchCurve->getErasedRanges();
        const auto isErased = [&erasedRanges](int frame) {
            return std::any_of(erasedRanges.begin(), erasedRanges.end(),
                [frame](const F0FrameRange& range) {
                    return frame >= range.startFrame && frame < range.endFrameExclusive;
                });
        };
        const auto interpolateTrack = [](const std::vector<float>& track, double position) {
            if (position < 0.0)
                return 0.0f;
            const int first = static_cast<int>(position);
            if (first >= static_cast<int>(track.size()))
                return 0.0f;
            const int second = std::min(first + 1, static_cast<int>(track.size()) - 1);
            const float value0 = track[static_cast<size_t>(first)];
            const float value1 = track[static_cast<size_t>(second)];
            if (value0 > 0.0f && value1 > 0.0f)
            {
                const double fraction = position - first;
                return static_cast<float>(std::exp(
                    std::log(value0) * (1.0 - fraction) + std::log(value1) * fraction));
            }
            return value0 > 0.0f ? value0 : value1;
        };

        const float* sourceAudio = coreJob.audioBuffer->getReadPointer(0);
        const int64_t contextStartSample = contextStartFrame * RenderChunkPlanner::kRenderHopSize;

        std::vector<float> sourceF0(static_cast<size_t>(contextFrames), 0.0f);
        std::vector<uint8_t> voicedFrames(static_cast<size_t>(contextFrames), 0);
        vocoderF0.assign(static_cast<size_t>(contextFrames), 0.0f);
        vocoderUv.assign(static_cast<size_t>(contextFrames), 0.0f);
        for (int i = 0; i < contextFrames; ++i)
        {
            const int64_t globalMelFrame = contextStartFrame + i;
            const double framePosition = static_cast<double>(globalMelFrame)
                * RenderChunkPlanner::kRenderHopSize / RenderCache::kSampleRate * f0FrameRate;
            if (framePosition < 0.0)
                continue;
            const int first = static_cast<int>(framePosition);
            if (first >= static_cast<int>(originalF0.size()))
                continue;
            const int second = std::min(first + 1, static_cast<int>(originalF0.size()) - 1);
            const int nearest = static_cast<int>(std::nearbyint(framePosition));
            const bool voiced = nearest >= 0 && nearest < static_cast<int>(originalF0.size())
                && originalF0[static_cast<size_t>(nearest)] > 0.0f
                && !isErased(nearest);
            voicedFrames[static_cast<size_t>(i)] = voiced ? 1 : 0;
            vocoderUv[static_cast<size_t>(i)] = voiced ? 1.0f : 0.0f;
            sourceF0[static_cast<size_t>(i)] = interpolateTrack(sourceF0Track, framePosition);
            const bool erasedFirst = isErased(first);
            const bool erasedSecond = isErased(second);
            if (erasedFirst || erasedSecond)
                continue;

            vocoderF0[static_cast<size_t>(i)] = interpolateTrack(effectiveTrack, framePosition);
        }

        const size_t contextSamples = static_cast<size_t>(contextFrames)
            * RenderChunkPlanner::kRenderHopSize;
        std::vector<float> cleanAudio;
        auto payload = std::make_shared<HybridRenderPayload>();
        if (usesHnsepDdspGenerator)
        {
            if (contentSnap->hnsepHarmonicAudio == nullptr
                || contentSnap->hnsepResidualAudio == nullptr
                || contentSnap->hnsepHarmonicAudio->size() != static_cast<size_t>(clipSamples)
                || contentSnap->hnsepResidualAudio->size() != static_cast<size_t>(clipSamples))
            {
                failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                          coreJob.targetRevision, coreJob.contentKey,
                          "HNSEP harmonic/residual audio is unavailable or not sample-aligned; "
                          "refresh content with ddsp_harmonic_generator_opentune.onnx selected");
                return;
            }

            if (!sliceHnsepSamples(*contentSnap->hnsepHarmonicAudio,
                                   contextStartSample, contextSamples, cleanAudio)
                || !sliceHnsepSamples(*contentSnap->hnsepResidualAudio,
                                      contextStartSample, contextSamples, payload->residual))
            {
                failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                          coreJob.targetRevision, coreJob.contentKey,
                          "HNSEP chunk could not be sliced on the dry-audio sample timeline");
                return;
            }

            payload->sourceAudio.assign(contextSamples, 0.0f);
            for (size_t i = 0; i < contextSamples; ++i)
            {
                const int64_t globalSample = contextStartSample + static_cast<int64_t>(i);
                if (globalSample >= 0 && globalSample < clipSamples)
                    payload->sourceAudio[i] = sourceAudio[globalSample];
            }
        }
        else
        {
            auto extracted = extractAperiodicHigh(
                sourceAudio, static_cast<size_t>(clipSamples), sourceF0, voicedFrames,
                static_cast<size_t>(contextStartFrame), static_cast<size_t>(clipFrames));
            cleanAudio.resize(extracted.alignedAudio.size());
            for (size_t i = 0; i < cleanAudio.size(); ++i)
                cleanAudio[i] = extracted.alignedAudio[i] - extracted.residual[i];
            payload->sourceAudio = std::move(extracted.alignedAudio);
            payload->residual = std::move(extracted.residual);
        }
        auto cleanConditioning = computeLogMelSpectrogram(
            cleanAudio.data(), static_cast<int>(cleanAudio.size()), contextFrames,
            conditioningConfig);
        if (!cleanConditioning.ok() || cleanConditioning.value().empty())
        {
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.contentKey,
                      !cleanConditioning.ok()
                          ? juce::String(cleanConditioning.error().fullMessage())
                          : juce::String("Clean mel conditioning calculation returned an empty result"));
            return;
        }
        conditioning = std::move(cleanConditioning).value();
        actualFrames = static_cast<int>(conditioning.size() / conditioningConfig.nMels);

        payload->voicedSamples.resize(payload->sourceAudio.size(), 0);
        for (size_t i = 0; i < payload->voicedSamples.size(); ++i)
        {
            const int64_t globalSample = contextStartSample + static_cast<int64_t>(i);
            const double framePosition = static_cast<double>(globalSample)
                / RenderCache::kSampleRate * f0FrameRate;
            const int nearest = static_cast<int>(std::nearbyint(framePosition));
            const bool voiced = globalSample < clipSamples
                && nearest >= 0 && nearest < static_cast<int>(originalF0.size())
                && originalF0[static_cast<size_t>(nearest)] > 0.0f
                && !isErased(nearest);
            payload->voicedSamples[i] = voiced ? 1 : 0;
        }
        payload->globalSampleOffset = contextStartSample;
        payload->synthesisOffsetSamples = static_cast<size_t>(
            boundaries.trueStartSample - contextStartSample);
        payload->publishSampleCount = static_cast<size_t>(boundaries.publishSampleCount);
        hybridPayload = std::move(payload);
    }
    else
    {
        auto conditioningResult = linearSpec
            ? computeLogLinearSpectrogram(monoAudio.data(),
                                          static_cast<int>(monoAudio.size()),
                                          numFrames,
                                          conditioningConfig)
            : computeLogMelSpectrogram(monoAudio.data(),
                                       static_cast<int>(monoAudio.size()),
                                       numFrames,
                                       conditioningConfig);
        if (!conditioningResult.ok() || conditioningResult.value().empty())
        {
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.contentKey,
                      !conditioningResult.ok()
                          ? juce::String(conditioningResult.error().fullMessage())
                          : juce::String("Mel conditioning calculation returned an empty result"));
            return;
        }

        conditioning = std::move(conditioningResult).value();
        actualFrames = static_cast<int>(conditioning.size() / conditioningConfig.nMels);

        // Gap filling remains the ordinary vocoder's complete-timeline conditioning rule.
        const auto& originalF0 = pitchCurve->getOriginalF0();
        auto vocoderSourceF0 = materializeEffectiveF0Range(
            *contentSnap, 0, static_cast<int>(originalF0.size()));
        const auto preFillF0 = vocoderSourceF0;
        OpenTune::fillF0GapsForVocoder(vocoderSourceF0, pitchCurve->getErasedRanges());

        vocoderF0.assign(static_cast<size_t>(actualFrames), 0.0f);
        vocoderUv.assign(static_cast<size_t>(actualFrames), 0.0f);
        for (int i = 0; i < actualFrames; ++i)
        {
            const double melTimeSec = trueStartSeconds + i * hopDuration;
            const double srcPos = melTimeSec * f0FrameRate - static_cast<double>(f0StartFrame);
            if (srcPos < 0.0)
                continue;
            const int srcIdx0 = static_cast<int>(srcPos);
            if (srcIdx0 >= numF0Frames)
                continue;
            const int srcIdx1 = std::min(srcIdx0 + 1, numF0Frames - 1);
            const double frac = srcPos - static_cast<double>(srcIdx0);
            const int globalIdx0 = f0StartFrame + srcIdx0;
            const int globalIdx1 = f0StartFrame + srcIdx1;
            const auto isErased = [&](int frame) {
                return std::any_of(pitchCurve->getErasedRanges().begin(), pitchCurve->getErasedRanges().end(),
                    [frame](const F0FrameRange& range) { return frame >= range.startFrame && frame < range.endFrameExclusive; });
            };
            const bool erased0 = isErased(globalIdx0);
            const bool erased1 = isErased(globalIdx1);
            const float f0_0 = erased0 ? 0.0f : (globalIdx0 >= 0
                && globalIdx0 < static_cast<int>(vocoderSourceF0.size())
                ? vocoderSourceF0[static_cast<size_t>(globalIdx0)] : 0.0f);
            const float f0_1 = erased1 ? 0.0f : (globalIdx1 >= 0
                && globalIdx1 < static_cast<int>(vocoderSourceF0.size())
                ? vocoderSourceF0[static_cast<size_t>(globalIdx1)] : 0.0f);
            if (erased0 || erased1)
                vocoderF0[static_cast<size_t>(i)] = 0.0f;
            else if (f0_0 > 0.0f && f0_1 > 0.0f)
                vocoderF0[static_cast<size_t>(i)] =
                    static_cast<float>(std::exp(std::log(f0_0) * (1.0 - frac) + std::log(f0_1) * frac));
            else if (f0_0 > 0.0f)
                vocoderF0[static_cast<size_t>(i)] = f0_0;
            else if (f0_1 > 0.0f)
                vocoderF0[static_cast<size_t>(i)] = f0_1;

            const bool voiced0 = globalIdx0 >= 0
                && globalIdx0 < static_cast<int>(preFillF0.size())
                && preFillF0[static_cast<size_t>(globalIdx0)] > 0.0f;
            const bool voiced1 = globalIdx1 >= 0
                && globalIdx1 < static_cast<int>(preFillF0.size())
                && preFillF0[static_cast<size_t>(globalIdx1)] > 0.0f;
            vocoderUv[static_cast<size_t>(i)] = (erased0 || erased1) ? 0.0f : ((voiced0 || voiced1) ? 1.0f : 0.0f);
        }
    }

    VocoderDomain::Job vocoderJob;
    vocoderJob.f0 = std::move(vocoderF0);
    vocoderJob.uv = std::move(vocoderUv);
    vocoderJob.conditioning = std::move(conditioning);
    vocoderJob.hybridPayload = hybridPayload;

    auto renderCache = coreJob.renderCache;
    auto targetRevision = coreJob.targetRevision;
    auto requeueAudioBuffer = coreJob.audioBuffer;
    const double requeueAudioSampleRate = coreJob.audioSampleRate;
    const ContentKey captureContentKey = coreJob.contentKey;
    const uint64_t chunkObjId = captureContentKey.objectId;
    const int64_t jobStartSample = boundaries.trueStartSample;
    const FrozenRenderBoundaries frozenBoundaries = boundaries;

    asyncCounter = crs->beginAsyncRenderJob();
    if (asyncCounter == nullptr)
    {
        failFrozen("AsyncRenderJob registration failed: RenderWorker rejected the job because its async execution is closed");
        return;
    }
    vocoderJob.onComplete = [this, weakCrs = std::weak_ptr<ContentRenderService>(crs),
                              asyncCounter, renderCache, targetRevision,
                             requeueAudioBuffer, requeueAudioSampleRate,
                              captureContentKey, chunkObjId,
                              jobStartSample, frozenBoundaries, contentSnap,
                              hybridPayload,
                              completion](
                                 VocoderRenderScheduler::JobResult result,
                                 const juce::String& error,
                                 const std::vector<float>& audio)
    {
        struct AsyncRenderCompletion final
        {
            std::shared_ptr<RenderWorker::AsyncState> state;
            ~AsyncRenderCompletion() noexcept { RenderWorker::completeAsyncJob(state); }
        } asyncCompletion{asyncCounter};

        const auto& boundaries = frozenBoundaries;

        if (RenderWorker::isAsyncJobClosed(asyncCounter))
        {
            if (renderCache != nullptr)
                renderCache->completeChunkRenderFailure(jobStartSample, targetRevision);
            return;
        }

        if (result == VocoderRenderScheduler::JobResult::Succeeded)
        {
            std::vector<float> publishedAudio;
            const bool prepared = hybridPayload != nullptr
                ? ((publishedAudio = mixHybridRender(audio, *hybridPayload)),
                   publishedAudio.size() == static_cast<size_t>(boundaries.publishSampleCount))
                : preparePublishedAudioFromSynthesis(boundaries, audio, publishedAudio);
            if (!prepared)
            {
                AppLogger::error("ChunkRender: synthesis length mismatch for RenderCache publish objId="
                    + juce::String(static_cast<juce::int64>(chunkObjId)));
                failChunk(completion, renderCache.get(), jobStartSample,
                          targetRevision, captureContentKey,
                          (hybridPayload != nullptr
                            ? juce::String("Hybrid vocoder synthesis context or crop length mismatch (expectedSamples=")
                            : juce::String("Vocoder synthesis output length mismatch (expected="))
                            + juce::String(hybridPayload != nullptr
                                ? boundaries.publishSampleCount : boundaries.synthSampleCount)
                            + ", actual=" + juce::String(static_cast<juce::int64>(audio.size())) + ")");
                return;
            }

            const auto publishResult = publishChunkWithPerNoteEq(
                *renderCache, boundaries, std::move(publishedAudio), targetRevision,
                contentSnap->notes);

            if (publishResult == RenderCache::ChunkRenderResult::InvalidInput
                || publishResult == RenderCache::ChunkRenderResult::MemoryLimitExceeded)
            {
                failChunk(completion, renderCache.get(), jobStartSample,
                          targetRevision, captureContentKey,
                          publishResult == RenderCache::ChunkRenderResult::InvalidInput
                              ? "RenderCache rejected synthesized audio as invalid input (startSample="
                                  + juce::String(boundaries.trueStartSample) + ", endSampleExclusive="
                                  + juce::String(boundaries.trueEndSample) + ", expectedSamples="
                                  + juce::String(boundaries.publishSampleCount) + ", actualSamples="
                                  + juce::String(static_cast<juce::int64>(boundaries.publishSampleCount))
                                  + ", revision=" + juce::String(static_cast<juce::int64>(targetRevision)) + ")"
                              : "RenderCache could not publish synthesized audio because its memory limit was exceeded (samples="
                                  + juce::String(boundaries.publishSampleCount) + ")");
                return;
            }

            if (publishResult == RenderCache::ChunkRenderResult::Stale)
            {
                return;
            }

            notifyChunkSettled(completion,
                               captureContentKey,
                               contentSnap,
                               requeueAudioBuffer,
                               requeueAudioSampleRate);
        }
        else if (result == VocoderRenderScheduler::JobResult::Cancelled)
        {
            AppLogger::warn("ChunkRender: vocoder inference cancelled objId="
                + juce::String(static_cast<juce::int64>(chunkObjId))
                + " reason=" + error);
            RenderJob requeueJob;
            requeueJob.kind = RenderJob::Kind::Stage1Render;
            requeueJob.contentKey = captureContentKey;
            requeueJob.renderCache = renderCache;
            requeueJob.contentSnapshot = contentSnap;
            requeueJob.contentRevision = completion.contentRevision;
            requeueJob.audioBuffer = requeueAudioBuffer;
            requeueJob.audioSampleRate = requeueAudioSampleRate;
            requeueJob.startSample = jobStartSample;
            requeueJob.endSampleExclusive = frozenBoundaries.trueEndSample;
            requeueJob.queuedChunkStartSample = jobStartSample;
            requeueJob.targetRevision = targetRevision;
            if (RenderWorker::isAsyncJobClosed(asyncCounter))
            {
                if (renderCache != nullptr)
                    renderCache->completeChunkRenderFailure(jobStartSample, targetRevision);
            }
            else if (auto crs = weakCrs.lock())
                deferOrRequeue(std::move(crs), std::move(requeueJob), completion,
                    "Vocoder inference cancellation could not be requeued: "
                        + (error.isNotEmpty() ? error : juce::String("scheduler supplied no cancellation reason")));
            else if (renderCache != nullptr)
                renderCache->completeChunkRenderFailure(jobStartSample, targetRevision);
        }
        else
        {
            // Failed: real inference error
            const auto inferenceError = error.isNotEmpty()
                ? error
                : juce::String("Vocoder inference failed without an error detail");
            AppLogger::error("ChunkRender: vocoder failed objId="
                + juce::String(static_cast<juce::int64>(chunkObjId))
                + " error=" + inferenceError);
            failChunk(completion, renderCache.get(), jobStartSample,
                      targetRevision, captureContentKey, inferenceError);
        }
    };

    const auto f0Capacity = vocoderJob.f0.capacity();
    const auto uvCapacity = vocoderJob.uv.capacity();
    const auto conditioningCapacity = vocoderJob.conditioning.capacity();
    const size_t hybridPayloadBytes = vocoderJob.hybridPayload != nullptr
        ? vocoderJob.hybridPayload->retainedBytes() : 0;
    juce::String submissionError;
    const auto submitResult = submitVocoderJob(
        std::move(vocoderJob), vocoderCfg.generation, submissionError);
    if (submitResult != VocoderSubmitResult::Submitted)
    {
        if (submitResult == VocoderSubmitResult::PayloadTooLarge)
            failFrozen("Vocoder submission payload exceeded the scheduler memory limit (F0="
                + juce::String(static_cast<juce::int64>(f0Capacity)) + ", UV="
                + juce::String(static_cast<juce::int64>(uvCapacity)) + ", conditioning="
                + juce::String(static_cast<juce::int64>(conditioningCapacity)) + " floats, hybrid="
                + juce::String(static_cast<juce::int64>(hybridPayloadBytes)) + " bytes)");
        else
            deferOrRequeue(crs, std::move(coreJob), completion,
                submissionError.isNotEmpty()
                    ? submissionError
                    : juce::String("Vocoder job submission was rejected without an error detail"));
        RenderWorker::completeAsyncJob(asyncCounter);
        return;
    }
    asyncSubmitted = true;
    }
    catch (const std::exception& e)
    {
        if (asyncCounter != nullptr && !asyncSubmitted)
            RenderWorker::completeAsyncJob(asyncCounter);
        failFrozen("Render processing threw std::exception: " + juce::String(e.what()));
    }
    catch (...)
    {
        if (asyncCounter != nullptr && !asyncSubmitted)
            RenderWorker::completeAsyncJob(asyncCounter);
        failFrozen("Render processing threw an unknown exception");
    }
}

} // namespace OpenTune
