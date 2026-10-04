#include "ProcessRenderRuntime.h"

#include "../DSP/AutoTunePeriodDetector.h"
#include "../DSP/AutoTunePitchShifter.h"
#include "../Editor/ConfirmDialogContent.h"
#include "../DSP/MelSpectrogram.h"
#include "../DSP/NoteEqProcessor.h"
#include "../Inference/ChunkRenderStrategy.h"
#include "../Inference/VocoderDomain.h"
#include "../Render/RenderChunkPlanner.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include "../Utils/AppPreferences.h"
#include "../Utils/ChannelLayoutLogger.h"
#include "../Utils/ModelPathResolver.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeCoordinate.h"
#include "ProcessF0Runtime.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace OpenTune {

namespace {

std::unordered_map<ContentKey, uint64_t>& notifiedRenderFailures()
{
    static std::unordered_map<ContentKey, uint64_t> failures;
    return failures;
}

} // namespace

void ProcessRenderRuntime::notifyApplicationRenderFailure(ContentKey key,
                                                           uint64_t revision,
                                                           const juce::String& reason)
{
    const auto failureReason = reason.isNotEmpty() ? reason : juce::String("Render failed");
    const bool posted = juce::MessageManager::callAsync([key, revision, failureReason]()
    {
        if (key.isValid())
        {
            auto& failures = notifiedRenderFailures();
            const auto [it, inserted] = failures.emplace(key, revision);
            if (!inserted && it->second == revision)
                return;
            it->second = revision;
        }

        ConfirmDialogContent::showDiagnostic(
            nullptr,
            "Render Failure",
            juce::String::fromUTF8(u8"渲染失败，可能回退干声。原因：") + failureReason,
            AppLogger::makeDiagnosticText("Render", failureReason));
    });
    if (!posted)
        AppLogger::error("[ProcessRenderRuntime] render failure notification dispatcher rejected");
}

namespace {

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

// ==============================================================================
// F0 Gap Filling for Vocoder (Mel Frame Space)
// ==============================================================================
// 对齐训练代码 interp_uv=True（utils/wav2F0.interp_f0）：
//   在 log2 频域对所有 unvoiced（f0==0）帧做 np.interp 等价线性插值：
//   - 内部间隙：log2 域左右 voiced 帧线性插值（无大小限制）
//   - 前导零帧：flat clamp 到第一个 voiced 帧的 log2(F0)
//   - 尾部零帧：flat clamp 到最后一个 voiced 帧的 log2(F0)
//   - 全零输入：保持全零
//
// 旧实现有两大不一致：(1) 内部间隙 ≤50 帧限制（训练无此限制）；
// (2) 前导/尾部通过 contentSnap 边界查询 + 几何均值延伸（训练只做 flat clamp）。
// 这两处偏差导致 vocoder 拿到的 F0 与训练不一致，产生相位震荡（低频砰砰声）。
void fillF0GapsForVocoder(std::vector<float>& f0)
{
    if (f0.empty()) return;

    const int n = static_cast<int>(f0.size());

    // 收集所有 voiced 帧的索引和 log2(F0) 值
    std::vector<int> voicedIdx;
    std::vector<float> voicedLogF0;
    voicedIdx.reserve(n);
    voicedLogF0.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        if (f0[static_cast<size_t>(i)] > 0.0f)
        {
            voicedIdx.push_back(i);
            voicedLogF0.push_back(std::log2(f0[static_cast<size_t>(i)]));
        }
    }

    // 全部 unvoiced → 保持全零（对齐 interp_f0 中 uv.all() 分支）
    if (voicedIdx.empty()) return;

    // 对每个 unvoiced 帧在 log2 域做线性插值
    // 等价于 np.interp(x_unvoiced, x_voiced, f0_log2_voiced)
    for (int i = 0; i < n; ++i)
    {
        if (f0[static_cast<size_t>(i)] > 0.0f) continue;

        auto it = std::lower_bound(voicedIdx.begin(), voicedIdx.end(), i);

        if (it == voicedIdx.begin())
        {
            // 前导零帧：flat clamp 到第一个 voiced F0
            f0[static_cast<size_t>(i)] = std::pow(2.0f, voicedLogF0.front());
        }
        else if (it == voicedIdx.end())
        {
            // 尾部零帧：flat clamp 到最后一个 voiced F0
            f0[static_cast<size_t>(i)] = std::pow(2.0f, voicedLogF0.back());
        }
        else
        {
            // 内部零帧：log2 域线性插值
            const int rightIdx = *it;
            const int leftIdx = *(it - 1);
            const float rightLogF0 = voicedLogF0[static_cast<size_t>(it - voicedIdx.begin())];
            const float leftLogF0 = voicedLogF0[static_cast<size_t>(it - voicedIdx.begin() - 1)];
            const float t = static_cast<float>(i - leftIdx) / static_cast<float>(rightIdx - leftIdx);
            f0[static_cast<size_t>(i)] = std::pow(2.0f, leftLogF0 + t * (rightLogF0 - leftLogF0));
        }
    }
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
                       uint64_t revision,
                       const juce::String& reason = {})
{
    // This notification intentionally does not use the completion gate. The
    // processor/editor callback may be rejected during teardown, but a
    // failure that has already been committed to RenderCache still needs to
    // reach the application UI while the message manager is alive.
    if (completion.applicationFailure)
        completion.applicationFailure(key, revision, reason);

    if (!completion.chunkFailed)
        return;
    const auto gate = completion.gate;
    auto callback = completion.chunkFailed;
    const auto failureReason = reason.isNotEmpty() ? reason : juce::String("Render failed");
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
               const juce::String& reason = {})
{
    if (cache != nullptr && cache->completeChunkRenderFailure(startSample, revision))
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

std::unique_ptr<VocoderDomain> ProcessRenderRuntime::createVocoderDomain(const VocoderModelWeight& weight)
{
    // 该函数始终在 vocoderMutex_ 外执行。ORT Env 初始化、模型加载、Session
    // 创建和失败清理都可能耗时，绝不能阻塞 UI 的 detach/isVocoderReady。
    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    if (!ProcessF0Runtime::getInstance().initialize(modelsDir))
        return nullptr;

    auto env = ProcessF0Runtime::getInstance().getOrtEnv();
    if (env == nullptr)
        return nullptr;

    auto domain = std::make_unique<VocoderDomain>(env);
    const auto modelPath = modelPathForWeight(modelsDir, weight);
    AppLogger::info("VocoderDomain: loading weight=" + juce::String(weight)
        + " modelPath=" + modelPath);
    if (!domain->initialize(modelPath))
        return nullptr;

    return domain;
}

ProcessRenderRuntime::ControlResult ProcessRenderRuntime::reconfigureVocoder(const ControlCommand& command)
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
        std::vector<DeferredRetry> failedRetries;
        {
            std::lock_guard<std::mutex> lock(vocoderMutex_);
            vocoderReconfiguring_ = false;
            failedRetries = std::move(deferredRetries_);
        }
        failDeferredRetries(std::move(failedRetries));
        return ControlResult::Failed;
    }

    if (command.type == ControlCommand::Type::ResetInferenceBackend)
    {
        auto& detector = AccelerationDetector::getInstance();
        detector.resetAndDetect(command.forceCpu);
    }

    // 严格先销毁旧 Session，再创建新 Session；新旧显存不并存。
    auto newDomain = createVocoderDomain(targetWeight);

    std::vector<DeferredRetry> readyRetries;
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
        readyRetries = std::move(deferredRetries_);
    }
    // Flush deferred retries outside vocoderMutex_: new domain is already
    // published (or creation failed and vocoderDomain_ remains null).
    // Directly check domain presence — never call acquireVocoderConfig here
    // which would trigger recursive domain creation.
    bool domainAvailable = false;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        domainAvailable = !shuttingDown_.load(std::memory_order_acquire)
            && vocoderDomain_ != nullptr;
    }
    for (auto& retry : readyRetries)
    {
        std::unique_lock<std::mutex> gateLock;
        if (retry.completion.gate != nullptr)
            gateLock = std::unique_lock<std::mutex>(retry.completion.gate->mutex);

        // Serialize the decision with owner close using gate -> runtime state.
        // The gate is released before requeue itself, while the runtime state
        // lock is retained through ContentRenderService/RenderWorker enqueue.
        // Owner close must acquire the same state lock after closing the gate,
        // so it cannot remove this item before that enqueue is complete.
        std::unique_lock<std::mutex> stateLock(vocoderMutex_);
        const bool gateClosed = retry.completion.gate != nullptr
            && retry.completion.gate->closed;
        domainAvailable = !shuttingDown_.load(std::memory_order_acquire)
            && vocoderDomain_ != nullptr;
        if (domainAvailable && !gateClosed)
        {
            bool requeued = false;
            if (gateLock.owns_lock())
                gateLock.unlock();
            if (auto crsShared = retry.crs.lock())
                requeued = crsShared->requeueRenderChunk(retry.job);
            if (requeued)
            {
                stateLock.unlock();
                continue;
            }
        }
        if (gateLock.owns_lock())
            gateLock.unlock();
        stateLock.unlock();

        if (retry.job.renderCache != nullptr
            && retry.job.renderCache->completeChunkRenderFailure(
                retry.job.startSample, retry.job.targetRevision))
            notifyChunkFailed(retry.completion, retry.job.contentKey,
                              retry.job.targetRevision, retry.reason);
    }
    return domainAvailable ? ControlResult::Changed : ControlResult::Failed;
}

bool ProcessRenderRuntime::postControlCommand(ControlCommand command)
{
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (shuttingDown_.load(std::memory_order_acquire))
        {
            AppLogger::error("[ProcessRenderRuntime] rejected control command during shutdown; Failed");
            rejected = true;
        }
        else
        {
            if (command.type != ControlCommand::Type::Stop
                && controlQueue_.size() >= ProcessRenderRuntime::kMaxControlQueueDepth)
            {
                AppLogger::error("[ProcessRenderRuntime] control queue full; Failed");
                rejected = true;
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
        auto finish = [transaction](const char* reason) mutable noexcept {
            finishControlTransaction(transaction, reason);
        };
        const bool posted = juce::MessageManager::callAsync(
            [completion = std::move(completion), finish, transaction]() mutable {
                if (!claimControlTransaction(transaction))
                    return;
                try
                {
                    if (completion)
                        completion(ControlResult::Failed);
                }
                catch (...)
                {
                    AppLogger::error("[ProcessRenderRuntime] rejected control completion threw");
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
            for (auto& retry : deferred)
            {
                if (retry.job.renderCache != nullptr
                    && retry.job.renderCache->completeChunkRenderFailure(
                        retry.job.startSample, retry.job.targetRevision))
                    notifyChunkFailed(retry.completion, retry.job.contentKey,
                                      retry.job.targetRevision, retry.reason);
            }
            domain.reset();

            // GAME inference is process-scoped and not cancellable.  Join its
            // worker here, before publishing worker-exit, so the message
            // thread only joins an already fully quiescent control worker.
            ProcessF0Runtime::getInstance().shutdown();

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
        try
        {
            if (command.transaction && command.transaction->crs)
            {
                command.transaction->crs->pauseRenderWorker();
                command.transaction->paused.store(true, std::memory_order_release);
            }
            result = reconfigureVocoder(command);
            if (command.transaction && command.transaction->crs)
                command.transaction->crs->waitAsyncRenderJobs();
        }
        catch (const std::exception& e)
        {
            AppLogger::error("[ProcessRenderRuntime] control command failed: "
                + juce::String(e.what()));
            result = ControlResult::Failed;
        }
        catch (...)
        {
            AppLogger::error("[ProcessRenderRuntime] control command failed: unknown exception");
            result = ControlResult::Failed;
        }

        if (command.type == ControlCommand::Type::EnsureVocoder)
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            ensureVocoderQueued_ = false;
        }

        auto transaction = std::move(command.transaction);
        auto completion = std::move(command.completion);
        if (!transaction && !completion)
            continue;
        auto waitTransaction = transaction;
        const bool posted = juce::MessageManager::callAsync(
            [completion = std::move(completion), result, transaction]() mutable {
                if (transaction && !claimControlTransaction(transaction))
                    return;
                try
                {
                    if (completion)
                        completion(result);
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
    }
}

void ProcessRenderRuntime::setVocoderModelWeight(const VocoderModelWeight& weight,
                                                 std::shared_ptr<ContentRenderService> crs,
                                                 std::function<void(ControlResult)> completion)
{
    // UI 线程只投递命令并立即返回；Session 销毁与重建由 control worker 串行执行。
    ControlCommand command;
    command.type = ControlCommand::Type::SetVocoderWeight;
    command.weight = weight;
    command.transaction = std::make_shared<ControlTransaction>();
    command.transaction->crs = std::move(crs);
    command.completion = std::move(completion);
    postControlCommand(std::move(command));
}

void ProcessRenderRuntime::resetVocoder(std::shared_ptr<ContentRenderService> crs,
                                        std::function<void(ControlResult)> completion)
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
                                                 std::function<void(ControlResult)> completion)
{
    ControlCommand command;
    command.type = ControlCommand::Type::ResetInferenceBackend;
    command.forceCpu = forceCpu;
    command.transaction = std::make_shared<ControlTransaction>();
    command.transaction->crs = std::move(crs);
    command.completion = std::move(completion);
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
            [owner, &detached](DeferredRetry& retry) {
                if (retry.ownerIdentity != owner)
                    return false;
                detached.push_back(std::move(retry));
                return true;
            });
        deferredRetries_.erase(it, deferredRetries_.end());
    }

    if (gateLock.owns_lock())
        gateLock.unlock();
    failDeferredRetries(std::move(detached));
}

ProcessRenderRuntime::VocoderSubmitResult ProcessRenderRuntime::submitVocoderJob(
    VocoderDomain::Job job, uint64_t expectedGeneration)
{
    VocoderDomain* domain = nullptr;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (vocoderDomain_ == nullptr || vocoderGeneration_ != expectedGeneration)
            return VocoderSubmitResult::Retryable;
        ++domainSubmitInFlight_;
        domain = vocoderDomain_.get();
    }

    if (!VocoderRenderScheduler::isJobPayloadWithinLimit(
            job.f0.capacity(), job.uv.capacity(), job.conditioning.capacity()))
    {
        {
            std::lock_guard<std::mutex> lock(vocoderMutex_);
            --domainSubmitInFlight_;
        }
        domainSubmitCv_.notify_all();
        return VocoderSubmitResult::PayloadTooLarge;
    }

    bool submitted = false;
    try
    {
        submitted = domain->submit(std::move(job));
    }
    catch (...)
    {
        submitted = false;
    }

    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        --domainSubmitInFlight_;
    }
    domainSubmitCv_.notify_all();
    return submitted ? VocoderSubmitResult::Submitted : VocoderSubmitResult::Retryable;
}

bool ProcessRenderRuntime::acquireVocoderConfig(VocoderConfig& out)
{
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (vocoderDomain_ != nullptr)
        {
            out.generation = vocoderGeneration_;
            out.conditioningBins = vocoderDomain_->getConditioningBins();
            out.conditioningType = vocoderDomain_->getConditioningType();
            out.fMax = vocoderDomain_->getFMax();
            out.melFilterbank = vocoderDomain_->getMelFilterbankSpec();
            out.melLogEps = vocoderDomain_->getMelLogEps();
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

void ProcessRenderRuntime::failDeferredRetries(std::vector<DeferredRetry> retries)
{
    for (auto& retry : retries)
    {
        if (retry.job.renderCache != nullptr
            && retry.job.renderCache->completeChunkRenderFailure(
                retry.job.startSample, retry.job.targetRevision))
            notifyChunkFailed(retry.completion, retry.job.contentKey,
                              retry.job.targetRevision, retry.reason);
    }
}

bool ProcessRenderRuntime::isVocoderReady() const noexcept
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return vocoderDomain_ != nullptr;
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
    juce::String reason)
{
    const auto failureCache = job.renderCache;
    const auto failureContentKey = job.contentKey;
    const auto failureStartSample = job.startSample;
    const auto failureRevision = job.targetRevision;
    bool shouldDefer = false;
    bool deferredCapacityRejected = false;
    bool shuttingDown = false;
    bool gateClosed = false;
    std::unique_lock<std::mutex> gateLock;
    if (completion.gate != nullptr)
    {
        gateLock = std::unique_lock<std::mutex>(completion.gate->mutex);
        gateClosed = completion.gate->closed;
    }
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        shuttingDown = shuttingDown_.load(std::memory_order_acquire);
        shouldDefer = !gateClosed && !shuttingDown
            && (vocoderReconfiguring_ || vocoderDomain_ == nullptr);
        if (shouldDefer && deferredRetries_.size() < ProcessRenderRuntime::kMaxDeferredRetryDepth)
            deferredRetries_.push_back(
                {crs, std::move(job), std::move(completion), crs.get(), std::move(reason)});
        else if (shouldDefer)
        {
            shouldDefer = false;
            deferredCapacityRejected = true;
        }
    }
    if (gateLock.owns_lock())
        gateLock.unlock();
    if (gateClosed)
    {
        if (job.renderCache != nullptr
            && job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision))
            notifyChunkFailed(completion, job.contentKey, job.targetRevision, reason);
        return;
    }
    if (shuttingDown)
    {
        if (job.renderCache != nullptr
            && job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision))
            notifyChunkFailed(completion, job.contentKey, job.targetRevision, reason);
        return;
    }
    if (shouldDefer)
    {
        ControlCommand command;
        command.type = ControlCommand::Type::EnsureVocoder;
        if (!postControlCommand(std::move(command)))
        {
            DeferredRetry failed;
            {
                std::lock_guard<std::mutex> lock(vocoderMutex_);
                const auto failedRetry = std::find_if(
                    deferredRetries_.begin(), deferredRetries_.end(),
                    [failureCache, failureContentKey, failureStartSample, failureRevision](
                        const DeferredRetry& retry) {
                        return retry.job.renderCache == failureCache
                            && retry.job.contentKey == failureContentKey
                            && retry.job.startSample == failureStartSample
                            && retry.job.targetRevision == failureRevision;
                    });
                if (failedRetry != deferredRetries_.end())
                {
                    failed = std::move(*failedRetry);
                    deferredRetries_.erase(failedRetry);
                }
            }
            if (failed.job.renderCache != nullptr
                && failed.job.renderCache->completeChunkRenderFailure(
                    failed.job.startSample, failed.job.targetRevision))
                notifyChunkFailed(failed.completion, failed.job.contentKey,
                                  failed.job.targetRevision, failed.reason);
        }
    }
    else if (deferredCapacityRejected)
    {
        if (job.renderCache != nullptr
            && job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision))
            notifyChunkFailed(completion, job.contentKey, job.targetRevision, reason);
    }
    else if (!crs->requeueRenderChunk(job)
        && job.renderCache != nullptr
        && job.renderCache->completeChunkRenderFailure(job.startSample, job.targetRevision))
        notifyChunkFailed(completion, job.contentKey, job.targetRevision, reason);
}

void ProcessRenderRuntime::processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                                                  RenderJob& job,
                                                  bool lightPitchEnabled,
                                                  CompletionContext completion)
{
    const auto failureCache = job.renderCache;
    const auto failureStartSample = job.startSample;
    const auto failureRevision = job.targetRevision;
    const auto failureContentKey = job.contentKey;
    const auto failFrozen = [&]() {
        failChunk(completion, failureCache.get(), failureStartSample,
                  failureRevision, failureContentKey);
    };

    if (shuttingDown_.load(std::memory_order_acquire))
    {
        failFrozen();
        return;
    }

    std::shared_ptr<RenderWorker::AsyncState> asyncCounter;
    bool asyncSubmitted = false;
    try
    {
    if (crs == nullptr || job.renderCache == nullptr || !job.contentSnapshot || !job.audioBuffer)
    {
        failFrozen();
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
        const auto result = publishChunkWithPerNoteEq(
            *coreJob.renderCache, boundaries, std::move(rawAudio),
            coreJob.targetRevision, contentSnap->notes);
        if (result == RenderCache::ChunkRenderResult::InvalidInput
            || result == RenderCache::ChunkRenderResult::MemoryLimitExceeded) {
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.contentKey);
        } else if (result == RenderCache::ChunkRenderResult::Published)
            notifyChunkSettled(completion, coreJob);
    };
    const auto settleBlankChunk = [&]() {
        if (coreJob.renderCache->markChunkAsBlank(coreJob.startSample, coreJob.targetRevision))
            notifyChunkSettled(completion, coreJob);
        else
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.contentKey);
    };

    if (!clipFound || monoAudio.empty() || numFrames <= 0 || !boundariesFrozen)
    {
        // 无原始音频可发布：保持既有失败语义
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.contentKey);
        return;
    }

    if (!pitchCurve)
    {
        if (!intersectsActiveEqNote)
        {
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.contentKey);
            return;
        }
        publishRawWithEq();
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
    if (f0HopSize <= 0 || f0SampleRate <= 0.0)
    {
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.contentKey);
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

                const uint64_t objectId = coreJob.contentKey.objectId;
                const auto result = publishChunkWithPerNoteEq(
                    *coreJob.renderCache, boundaries, std::move(shiftedAudio),
                    coreJob.targetRevision, contentSnap->notes);

                if (result == RenderCache::ChunkRenderResult::InvalidInput
                    || result == RenderCache::ChunkRenderResult::MemoryLimitExceeded)
                {
                    failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                              coreJob.targetRevision, coreJob.contentKey);
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
        requeueJob.audioBuffer = coreJob.audioBuffer;
        requeueJob.audioSampleRate = coreJob.audioSampleRate;
        requeueJob.startSample = coreJob.startSample;
        requeueJob.endSampleExclusive = coreJob.endSampleExclusive;
        requeueJob.queuedChunkStartSample = coreJob.startSample;
        requeueJob.targetRevision = coreJob.targetRevision;
        deferOrRequeue(crs, std::move(requeueJob), completion);
        return;
    }

    const int conditioningBins = vocoderCfg.conditioningBins;
    const float fMax = vocoderCfg.fMax;
    if (conditioningBins <= 0)
    {
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.contentKey);
        return;
    }

    MelSpectrogramConfig conditioningConfig;
    conditioningConfig.sampleRate = static_cast<int>(RenderCache::kSampleRate);
    conditioningConfig.nMels = conditioningBins;
    conditioningConfig.fMax = fMax;
    conditioningConfig.melFilterbank = vocoderCfg.melFilterbank;
    conditioningConfig.logEps = vocoderCfg.melLogEps;

    const bool linearSpec = vocoderCfg.conditioningType == VocoderConditioningType::LogLinearSpec;
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
                  coreJob.targetRevision, coreJob.contentKey);
        return;
    }

    auto conditioning = std::move(conditioningResult).value();
    const int actualFrames = static_cast<int>(conditioning.size() / conditioningConfig.nMels);

    // Training applies interp_uv=True to the complete F0 timeline before the
    // vocoder sees it.  Interpolating only this render chunk misses voiced
    // frames on the other side of an all-unvoiced chunk.
    const auto& originalF0 = pitchCurve->getOriginalF0();
    auto vocoderSourceF0 = materializeEffectiveF0Range(
        *contentSnap, 0, static_cast<int>(originalF0.size()));
    // UV 必须在 gap-fill 之前采样：fillF0GapsForVocoder 按训练 interp_uv=True
    // 语义填满所有 unvoiced 帧，填完后无法再区分浊音/清音。线性谱声码器需要
    // 显式 UV 才能在清音段抑制谐波源。
    const auto preFillF0 = vocoderSourceF0;
    fillF0GapsForVocoder(vocoderSourceF0);

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
        const float f0_0 = globalIdx0 >= 0
            && globalIdx0 < static_cast<int>(vocoderSourceF0.size())
            ? vocoderSourceF0[static_cast<size_t>(globalIdx0)] : 0.0f;
        const float f0_1 = globalIdx1 >= 0
            && globalIdx1 < static_cast<int>(vocoderSourceF0.size())
            ? vocoderSourceF0[static_cast<size_t>(globalIdx1)] : 0.0f;
        if (f0_0 > 0.0f && f0_1 > 0.0f)
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
        vocoderUv[static_cast<size_t>(i)] = (voiced0 || voiced1) ? 1.0f : 0.0f;
    }

    VocoderDomain::Job vocoderJob;
    vocoderJob.f0 = std::move(vocoderF0);
    vocoderJob.uv = std::move(vocoderUv);
    vocoderJob.conditioning = std::move(conditioning);

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
        failFrozen();
        return;
    }
    vocoderJob.onComplete = [this, weakCrs = std::weak_ptr<ContentRenderService>(crs),
                              asyncCounter, renderCache, targetRevision,
                             requeueAudioBuffer, requeueAudioSampleRate,
                             captureContentKey, chunkObjId,
                             jobStartSample, frozenBoundaries, contentSnap,
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
            failChunk(completion, renderCache.get(), jobStartSample,
                      targetRevision, captureContentKey,
                      result == VocoderRenderScheduler::JobResult::Succeeded
                          ? juce::String("Render cancelled because its owner detached")
                          : error);
            return;
        }

        if (result == VocoderRenderScheduler::JobResult::Succeeded)
        {
            std::vector<float> publishedAudio;
            if (!preparePublishedAudioFromSynthesis(boundaries, audio, publishedAudio))
            {
                AppLogger::error("ChunkRender: synthesis length mismatch for RenderCache publish objId="
                    + juce::String(static_cast<juce::int64>(chunkObjId)));
                failChunk(completion, renderCache.get(), jobStartSample,
                          targetRevision, captureContentKey);
                return;
            }

            const auto publishResult = publishChunkWithPerNoteEq(
                *renderCache, boundaries, std::move(publishedAudio), targetRevision,
                contentSnap->notes);

            if (publishResult == RenderCache::ChunkRenderResult::InvalidInput
                || publishResult == RenderCache::ChunkRenderResult::MemoryLimitExceeded)
            {
                failChunk(completion, renderCache.get(), jobStartSample,
                          targetRevision, captureContentKey);
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
            requeueJob.audioBuffer = requeueAudioBuffer;
            requeueJob.audioSampleRate = requeueAudioSampleRate;
            requeueJob.startSample = jobStartSample;
            requeueJob.endSampleExclusive = frozenBoundaries.trueEndSample;
            requeueJob.queuedChunkStartSample = jobStartSample;
            requeueJob.targetRevision = targetRevision;
            if (RenderWorker::isAsyncJobClosed(asyncCounter))
            {
                failChunk(completion, renderCache.get(), jobStartSample,
                          targetRevision, captureContentKey, error);
            }
            else if (auto crs = weakCrs.lock())
                deferOrRequeue(std::move(crs), std::move(requeueJob), completion, error);
            else
                failChunk(completion, renderCache.get(), jobStartSample,
                          targetRevision, captureContentKey, error);
        }
        else
        {
            // Failed: real inference error
            AppLogger::error("ChunkRender: vocoder failed objId="
                + juce::String(static_cast<juce::int64>(chunkObjId))
                + " error=" + error);
            failChunk(completion, renderCache.get(), jobStartSample,
                      targetRevision, captureContentKey, error);
        }
    };

    const auto submitResult = submitVocoderJob(std::move(vocoderJob), vocoderCfg.generation);
    if (submitResult != VocoderSubmitResult::Submitted)
    {
        if (submitResult == VocoderSubmitResult::PayloadTooLarge)
            failFrozen();
        else
            deferOrRequeue(crs, std::move(coreJob), completion);
        RenderWorker::completeAsyncJob(asyncCounter);
        return;
    }
    asyncSubmitted = true;
    }
    catch (...)
    {
        if (asyncCounter != nullptr && !asyncSubmitted)
            RenderWorker::completeAsyncJob(asyncCounter);
        failFrozen();
    }
}

} // namespace OpenTune
