#include "ProcessRenderRuntime.h"

#include "../DSP/AutoTunePeriodDetector.h"
#include "../DSP/AutoTunePitchShifter.h"
#include "../DSP/MelSpectrogram.h"
#include "../DSP/NoteEqProcessor.h"
#include "../Inference/ChunkRenderStrategy.h"
#include "../Inference/VocoderDomain.h"
#include "../Render/RenderChunkPlanner.h"
#include "../Utils/AccelerationDetector.h"
#include "../Utils/AppLogger.h"
#include "../Utils/ChannelLayoutLogger.h"
#include "../Utils/ModelPathResolver.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeCoordinate.h"
#include "ProcessF0Runtime.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

// 临时关闭静息处边界淡化
constexpr int kSilentGapBoundaryFadeSamples = 0;

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
// 在渲染提交前填补 vocoderF0 的零值间隙：
//   1. 内部间隙：≤50帧用 log-domain 线性插值填充
//   2. 边界延伸：起点/终点若为零，向边界外查询并延伸填充
//      - 检测延伸方向是否有 voiced 段，有则延伸到该段起点为止
//
// 目的：消除 PC-NSF-HiFiGAN 在 F0 不连续处的相位震荡（低频砰砰声）
void fillF0GapsForVocoder(
    std::vector<float>& f0,
    const EditableContentSnapshot& contentSnap,
    double frameStartTimeSec,
    double frameEndTimeSec,
    double hopDuration,
    double f0FrameRate,
    bool allowTrailingExtension)
{
    if (f0.empty()) return;

    constexpr int maxGapFrames = 50;  // ~580ms at 86fps
    const int n = static_cast<int>(f0.size());

    // ---- Step 1: Fill internal gaps with log-domain interpolation ----
    {
        int i = 0;
        while (i < n) {
            // Find next voiced frame
            while (i < n && f0[static_cast<size_t>(i)] <= 0.0f) ++i;
            if (i >= n) break;

            // Find voiced segment end
            int segEnd = i;
            while (segEnd < n && f0[static_cast<size_t>(segEnd)] > 0.0f) ++segEnd;

            // Find next voiced segment after gap
            int gapStart = segEnd;
            while (gapStart < n && f0[static_cast<size_t>(gapStart)] <= 0.0f) ++gapStart;

            if (gapStart >= n) break;  // No more voiced segments

            int gapLen = gapStart - segEnd;
            if (gapLen > 0 && gapLen <= maxGapFrames) {
                // Fill gap with log-domain interpolation
                const float fStart = f0[static_cast<size_t>(segEnd - 1)];
                const float fEnd = f0[static_cast<size_t>(gapStart)];
                const float logStart = std::log2(std::max(fStart, 1e-6f));
                const float logEnd = std::log2(std::max(fEnd, 1e-6f));
                for (int j = 0; j < gapLen; ++j) {
                    float t = static_cast<float>(j + 1) / static_cast<float>(gapLen + 1);
                    f0[static_cast<size_t>(segEnd + j)] = std::pow(2.0f, logStart + (logEnd - logStart) * t);
                }
            }

            i = gapStart;
        }
    }

    // ---- Step 2: Extend leading zeros (f0[0] == 0) ----
    if (n > 0 && f0[0] <= 0.0f) {
        // Find first voiced frame in current chunk
        int firstVoicedIdx = 0;
        while (firstVoicedIdx < n && f0[static_cast<size_t>(firstVoicedIdx)] <= 0.0f) ++firstVoicedIdx;

        if (firstVoicedIdx < n) {
            const float firstVoicedF0 = f0[static_cast<size_t>(firstVoicedIdx)];

            const int lookbackF0Frames = 100;
            const int queryStartFrame = static_cast<int>(std::floor(frameStartTimeSec * f0FrameRate)) - lookbackF0Frames;
            const int queryEndFrame = static_cast<int>(std::floor(frameStartTimeSec * f0FrameRate));

            std::vector<float> prevF0 = materializeEffectiveF0Range(
                contentSnap, queryStartFrame, queryEndFrame);

            float extendF0 = 0.0f;
            for (int j = static_cast<int>(prevF0.size()) - 1; j >= 0; --j) {
                if (prevF0[static_cast<size_t>(j)] > 0.0f) {
                    extendF0 = prevF0[static_cast<size_t>(j)];
                    break;
                }
            }

            if (extendF0 > 0.0f) {
                const float fillF0 = (extendF0 > 0.0f && firstVoicedF0 > 0.0f)
                    ? std::sqrt(extendF0 * firstVoicedF0)
                    : (firstVoicedF0 > 0.0f ? firstVoicedF0 : extendF0);

                for (int j = 0; j < firstVoicedIdx; ++j) {
                    float t = static_cast<float>(j) / static_cast<float>(firstVoicedIdx + 1);
                    float logFill = std::log2(std::max(fillF0, 1e-6f));
                    float logFirst = std::log2(std::max(firstVoicedF0, 1e-6f));
                    f0[static_cast<size_t>(j)] = std::pow(2.0f, logFill + (logFirst - logFill) * t);
                }
            }
        }
    }

    // ---- Step 3: Extend trailing zeros (f0[n-1] == 0) ----
    if (allowTrailingExtension && n > 0 && f0[static_cast<size_t>(n - 1)] <= 0.0f) {
        int lastVoicedIdx = n - 1;
        while (lastVoicedIdx >= 0 && f0[static_cast<size_t>(lastVoicedIdx)] <= 0.0f) --lastVoicedIdx;

        if (lastVoicedIdx >= 0) {
            const float lastVoicedF0 = f0[static_cast<size_t>(lastVoicedIdx)];

            const int lookaheadF0Frames = 100;
            const int queryStartFrame = static_cast<int>(std::ceil(frameEndTimeSec * f0FrameRate));
            const int queryEndFrame = queryStartFrame + lookaheadF0Frames;

            std::vector<float> nextF0 = materializeEffectiveF0Range(
                contentSnap, queryStartFrame, queryEndFrame);

            float extendF0 = 0.0f;
            for (size_t j = 0; j < nextF0.size(); ++j) {
                if (nextF0[j] > 0.0f) {
                    extendF0 = nextF0[j];
                    break;
                }
            }

            if (extendF0 > 0.0f || lastVoicedF0 > 0.0f) {
                const float fillF0 = (extendF0 > 0.0f && lastVoicedF0 > 0.0f)
                    ? std::sqrt(extendF0 * lastVoicedF0)
                    : (lastVoicedF0 > 0.0f ? lastVoicedF0 : extendF0);

                const int trailingLen = n - lastVoicedIdx - 1;
                for (int j = 0; j < trailingLen; ++j) {
                    float t = static_cast<float>(j + 1) / static_cast<float>(trailingLen + 1);
                    float logLast = std::log2(std::max(lastVoicedF0, 1e-6f));
                    float logFill = std::log2(std::max(fillF0, 1e-6f));
                    f0[static_cast<size_t>(lastVoicedIdx + 1 + j)] = std::pow(2.0f, logLast + (logFill - logLast) * t);
                }
            }
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
                        ContentKey key)
{
    if (!completion.chunkSettled)
        return;
    std::lock_guard<std::mutex> lk(completion.gate->mutex);
    if (!completion.gate->closed)
        completion.chunkSettled(key); // 持锁调用：owner 析构必须先拿同一把锁置 closed，互斥保证无 UAF
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

    // 静息处边界淡化（诊断阶段临时关闭）：chunk 起点 fade-in，终点 fade-out。
    const int totalSamples = static_cast<int>(audio.size());
    const int fadeSamples = std::min(kSilentGapBoundaryFadeSamples, totalSamples / 2);
    if (fadeSamples > 1)
    {
        // Fade-in at chunk start
        for (int i = 0; i < fadeSamples; ++i)
        {
            const float weight = static_cast<float>(i) / static_cast<float>(fadeSamples - 1);
            audio[static_cast<size_t>(i)] *= weight;
        }
        // Fade-out at chunk end
        for (int i = 0; i < fadeSamples; ++i)
        {
            const int pos = totalSamples - 1 - i;
            const float weight = static_cast<float>(i) / static_cast<float>(fadeSamples - 1);
            audio[static_cast<size_t>(pos)] *= weight;
        }
    }

    return renderCache.completeChunkRenderWithAudio(
        boundaries.trueStartSample, boundaries.trueEndSample,
        std::move(audio), revision);
}

} // namespace

ProcessRenderRuntime& ProcessRenderRuntime::getInstance()
{
    // 进程寿命 heap singleton：不注册静态析构（DLL detach 持 loader lock，
    // 不得在静态析构中 join 工作线程）。VST3 构建中模块已被 pin
    // （Vst3ModulePin.cpp），domain 与 control worker 存活到进程退出；
    // vocoderDomain_ 只经 setVocoderModelWeight() / resetVocoder() /
    // resetInferenceBackend() 在 control worker 上显式重建。
    static auto* instance = new ProcessRenderRuntime;
    return *instance;
}

ProcessRenderRuntime::ProcessRenderRuntime()
{
    // 进程寿命 control worker：模型切换/后端重置的耗时 Session 销毁、按当前
    // 配置重建与 AccelerationDetector reset/detect 全部在此串行执行；UI 线程
    // 只投递命令并立即返回。单例永不析构，线程随进程退出回收，绝不在实例
    // 卸载路径 join。
    controlWorker_ = std::thread([this]() { controlWorkerLoop(); });
}

void ProcessRenderRuntime::attach()
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    ++clientCount_;
}

void ProcessRenderRuntime::detach()
{
    // 只递减客户端租约计数：vocoder domain 是进程寿命资源（VST3 模块 pin /
    // Standalone 进程寿命），最后一个客户端 detach 也不销毁它，后续实例直接
    // 复用已加载的 domain 与 generation，避免重建 ORT Session。显式重置只
    // 经由 setVocoderModelWeight() / resetVocoder() / resetInferenceBackend()。
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    --clientCount_;
}

std::string ProcessRenderRuntime::modelPathForWeight(const std::string& modelDir, VocoderModelWeight weight)
{
    switch (weight)
    {
        case VocoderModelWeight::Community: return modelDir + "/hifigan.onnx";
        case VocoderModelWeight::Coulin9V4: return modelDir + "/hifigan_coulin9.onnx";
    }

    return modelDir + "/hifigan.onnx";
}

std::unique_ptr<VocoderDomain> ProcessRenderRuntime::createVocoderDomain(VocoderModelWeight weight)
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
    if (!domain->initialize(modelPath))
        return nullptr;

    return domain;
}

void ProcessRenderRuntime::reconfigureVocoder(const ControlCommand& command)
{
    std::unique_ptr<VocoderDomain> retiredDomain;
    VocoderModelWeight targetWeight;

    {
        std::unique_lock<std::mutex> lock(vocoderMutex_);

        if (command.type == ControlCommand::Type::SetVocoderWeight
            && currentVocoderModelWeight_ == command.weight)
            return;

        if (command.type == ControlCommand::Type::SetVocoderWeight)
            currentVocoderModelWeight_ = command.weight;

        targetWeight = currentVocoderModelWeight_;
        vocoderReconfiguring_ = true;
        retiredDomain = std::move(vocoderDomain_); // O(1)：锁内只摘除所有权
        ++vocoderGeneration_;                      // 立即拒绝全部旧配置快照

        vocoderStateCv_.wait(lock, [this] { return !vocoderInitializing_; });
    }

    // 唯一可能无界的路径：只阻塞进程寿命 control worker，不持任何 runtime 锁。
    retiredDomain.reset();

    if (command.type == ControlCommand::Type::ResetInferenceBackend)
    {
        auto& detector = AccelerationDetector::getInstance();
        detector.reset();
        detector.detect(command.forceCpu);
    }

    // 严格先销毁旧 Session，再创建新 Session；新旧显存不并存。
    auto newDomain = createVocoderDomain(targetWeight);

    std::vector<DeferredRetry> readyRetries;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (currentVocoderModelWeight_ == targetWeight)
        {
            vocoderDomain_ = std::move(newDomain);
            if (vocoderDomain_ != nullptr)
                ++vocoderGeneration_; // 新 domain 获得独立代际
        }
        vocoderReconfiguring_ = false;
        readyRetries = std::move(deferredRetries_);
    }
    vocoderStateCv_.notify_all();

    // Flush deferred retries outside vocoderMutex_: new domain is already
    // published (or creation failed and vocoderDomain_ remains null).
    // Directly check domain presence — never call acquireVocoderConfig here
    // which would trigger recursive domain creation.
    const bool domainAvailable = [&]() {
        std::lock_guard<std::mutex> lk(vocoderMutex_);
        return vocoderDomain_ != nullptr;
    }();

    for (auto& retry : readyRetries)
    {
        auto crsShared = retry.crs.lock();
        if (!crsShared)
            continue;

        if (domainAvailable)
            crsShared->requeueRenderChunk(retry.job);
        else
            retry.job.renderCache->completeChunkRenderFailure(
                retry.job.startSeconds, retry.job.targetRevision);
    }
}

void ProcessRenderRuntime::postControlCommand(ControlCommand command)
{
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        controlQueue_.push_back(std::move(command));
    }
    controlCv_.notify_one();
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
        }

        reconfigureVocoder(command);

        if (command.completion)
            juce::MessageManager::callAsync(std::move(command.completion));
    }
}

void ProcessRenderRuntime::setVocoderModelWeight(VocoderModelWeight weight, std::function<void()> completion)
{
    // UI 线程只投递命令并立即返回；Session 销毁与重建由 control worker 串行执行。
    ControlCommand command;
    command.type = ControlCommand::Type::SetVocoderWeight;
    command.weight = weight;
    command.completion = std::move(completion);
    postControlCommand(std::move(command));
}

void ProcessRenderRuntime::resetVocoder(std::function<void()> completion)
{
    ControlCommand command;
    command.type = ControlCommand::Type::ResetVocoder;
    command.completion = std::move(completion);
    postControlCommand(std::move(command));
}

void ProcessRenderRuntime::resetInferenceBackend(bool forceCpu, std::function<void()> completion)
{
    ControlCommand command;
    command.type = ControlCommand::Type::ResetInferenceBackend;
    command.forceCpu = forceCpu;
    command.completion = std::move(completion);
    postControlCommand(std::move(command));
}

bool ProcessRenderRuntime::submitVocoderJob(VocoderDomain::Job job, uint64_t expectedGeneration)
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    if (vocoderDomain_ == nullptr)
        return false;
    if (vocoderGeneration_ != expectedGeneration)
        return false;   // domain 已重建：job 配置过期，拒绝提交
    return vocoderDomain_->submit(std::move(job));
}

bool ProcessRenderRuntime::acquireVocoderConfig(VocoderConfig& out)
{
    VocoderModelWeight targetWeight;
    uint64_t creationGeneration = 0;

    {
        std::unique_lock<std::mutex> lock(vocoderMutex_);
        if (vocoderDomain_ != nullptr)
        {
            out.generation = vocoderGeneration_;
            out.melBins = vocoderDomain_->getMelBins();
            out.fMax = vocoderDomain_->getFMax();
            return true;
        }

        if (vocoderInitializing_)
        {
            vocoderStateCv_.wait(lock, [this] {
                return !vocoderInitializing_ || vocoderReconfiguring_;
            });

            if (vocoderDomain_ != nullptr)
            {
                out.generation = vocoderGeneration_;
                out.melBins = vocoderDomain_->getMelBins();
                out.fMax = vocoderDomain_->getFMax();
                return true;
            }
        }

        if (vocoderReconfiguring_)
            return false;

        vocoderInitializing_ = true;
        targetWeight = currentVocoderModelWeight_;
        creationGeneration = vocoderGeneration_;
    }

    // 首次模型加载在 RenderWorker 上锁外执行；UI 查询仍可取得短锁并立即返回。
    auto newDomain = createVocoderDomain(targetWeight);

    bool mayPublish = false;
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        mayPublish = !vocoderReconfiguring_
            && vocoderInitializing_
            && vocoderGeneration_ == creationGeneration
            && currentVocoderModelWeight_ == targetWeight
            && vocoderDomain_ == nullptr;

        if (mayPublish && newDomain != nullptr)
        {
            vocoderDomain_ = std::move(newDomain);
            ++vocoderGeneration_;
            out.generation = vocoderGeneration_;
            out.melBins = vocoderDomain_->getMelBins();
            out.fMax = vocoderDomain_->getFMax();
            published = true;
        }
    }

    // 发布失败的新 domain 也必须在锁外销毁。保持 initializing=true 直到其清理
    // 完成，防止 control worker 在旧初始化对象仍存在时创建第二个 Session。
    newDomain.reset();
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        vocoderInitializing_ = false;
    }
    vocoderStateCv_.notify_all();
    return published;
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

void ProcessRenderRuntime::deferOrRequeue(
    std::shared_ptr<ContentRenderService> crs, RenderJob job)
{
    bool shouldDefer = false;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        shouldDefer = vocoderReconfiguring_;
        if (shouldDefer)
            deferredRetries_.push_back({crs, std::move(job)});
    }
    if (!shouldDefer)
        crs->requeueRenderChunk(std::move(job));
}

void ProcessRenderRuntime::processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                                                 RenderJob& job,
                                                 std::shared_ptr<const EditableContentSnapshot> contentSnap,
                                                 bool lightPitchEnabled,
                                                 CompletionContext completion)
{
    if (crs == nullptr || job.renderCache == nullptr || !contentSnap)
    {
        if (job.renderCache != nullptr)
            job.renderCache->completeChunkRenderFailure(job.startSeconds, job.targetRevision);
        return;
    }

    PlaybackReadSource readSource;
    if (!crs->getPlaybackReadSource(job.contentKey, readSource) || !readSource.hasAudio())
    {
        job.renderCache->completeChunkRenderFailure(job.startSeconds, job.targetRevision);
        return;
    }
    // 执行读取点成对刷新：canonical 音频与其样本率永远来自同一份最新
    // PlaybackReadSource 快照，绝不跨快照混用。
    job.audioBuffer = readSource.audioBuffer;
    job.audioSampleRate = readSource.audioSampleRate;

    auto pitchCurve = contentSnap->pitchCurve;

    std::vector<float> monoAudio;
    std::vector<float> effectiveF0;
    std::vector<float> vocoderF0;

    const double relChunkStartSec = job.startSeconds;
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
        if (result == RenderCache::ChunkRenderResult::InvalidInput)
            coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
        else if (result == RenderCache::ChunkRenderResult::Published)
            notifyChunkSettled(completion, coreJob.contentKey);
    };

    if (!clipFound || monoAudio.empty() || numFrames <= 0 || !boundariesFrozen)
    {
        // 无原始音频可发布：保持既有失败语义
        coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
        return;
    }

    if (!pitchCurve)
    {
        if (!intersectsActiveEqNote)
        {
            coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
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

    auto snap = pitchCurve->getSnapshot();
    if (!snap->hasOriginalF0Data())
    {
        if (!intersectsActiveEqNote)
        {
            coreJob.renderCache->markChunkAsBlank(relChunkStartSec, coreJob.targetRevision);
            notifyChunkSettled(completion, coreJob.contentKey);
        }
        else
        {
            publishRawWithEq();
        }
        return;
    }

    const int f0HopSize = snap->getHopSize();
    const double f0SampleRate = snap->getSampleRate();
    if (f0HopSize <= 0 || f0SampleRate <= 0.0)
    {
        coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
        return;
    }

    const double f0FrameRate = f0SampleRate / static_cast<double>(f0HopSize);
    const int f0StartFrame = static_cast<int>(std::floor(trueStartSeconds * f0FrameRate));
    const int f0EndFrame = static_cast<int>(std::ceil(trueEndSeconds * f0FrameRate)) + 1;
    const int numF0Frames = std::max(1, f0EndFrame - f0StartFrame);
    const double firstSampleFramePhase = trueStartSeconds * f0FrameRate - static_cast<double>(f0StartFrame);

    // Blank 判定：仅当全局移调为恒等（effectiveF0 与 originalF0 一致，无差异可
    // 合成）且该 chunk 帧范围内无任何 correction segment 时，才 Blank 回退原始
    // 音频缓存播放。非恒等全局移调即使无 correction 也必须进入 vocoder 全量渲染，
    // 否则移调不生效。带 active EQ 的 Note 不得走 Blank：改为发布原始音频并应用 EQ。
    if (contentSnap->pitchShiftSettings.isIdentity() && !snap->hasCorrectionInRange(f0StartFrame, f0EndFrame))
    {
        if (!intersectsActiveEqNote)
        {
            coreJob.renderCache->markChunkAsBlank(relChunkStartSec, coreJob.targetRevision);
            notifyChunkSettled(completion, coreJob.contentKey);
        }
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
        {
            coreJob.renderCache->markChunkAsBlank(relChunkStartSec, coreJob.targetRevision);
            notifyChunkSettled(completion, coreJob.contentKey);
        }
        else
        {
            publishRawWithEq();
        }
        return;
    }

    if (lightPitchEnabled && contentSnap->pitchShiftSettings.isIdentity())
    {
        const auto& originalF0Full = snap->getOriginalF0();
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
                const int detectorNumSamples =
                    static_cast<int>(boundaries.publishSampleCount);
                // UV/V is decided by the waveform detector itself.  RMVPE's
                // originalF0 remains the editable source/target track, but it
                // must not gate the AutoTune detector's periodicity analysis.
                const std::vector<AutoTunePeriodDetector::DetectedPeriod>
                    detectorFrames = AutoTunePeriodDetector::analyze(
                        shifterLookbehind, shifterLookbehindSamples,
                        monoAudio.data(), detectorNumSamples,
                        RenderCache::kSampleRate);

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
                    detectorNumSamples);

                const uint64_t objectId = coreJob.contentKey.objectId;
                const auto result = publishChunkWithPerNoteEq(
                    *coreJob.renderCache, boundaries, std::move(shiftedAudio),
                    coreJob.targetRevision, contentSnap->notes);

                if (result == RenderCache::ChunkRenderResult::InvalidInput)
                {
                    coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
                }
                else if (result == RenderCache::ChunkRenderResult::Published)
                {
                    notifyChunkSettled(completion, coreJob.contentKey);
                }

                AppLogger::debug("RenderWorker: AutoTune pitch-shift chunk objId="
                    + juce::String(static_cast<juce::int64>(objectId))
                    + " start=" + juce::String(relChunkStartSec, 3));
                return;
            }
        }
    }

    // 真实 mel/vocoder 分支前唯一一次锁内获取 domain 配置（generation/melBins/fMax）：
    // raw 四分支与 light 路径已在上方早退，绝不触发模型加载。melBins/fMax 与提交
    // 校验的 generation 同属一个 domain，不存在跨域混用。
    if (!acquireVocoderConfig(vocoderCfg))
    {
        if (isVocoderReconfiguring())
        {
            RenderJob requeueJob;
            requeueJob.kind = RenderJob::Kind::Stage1Render;
            requeueJob.contentKey = coreJob.contentKey;
            requeueJob.renderCache = coreJob.renderCache;
            requeueJob.startSeconds = relChunkStartSec;
            requeueJob.targetRevision = coreJob.targetRevision;
            deferOrRequeue(crs, std::move(requeueJob));
            return;
        }
        AppLogger::log("RenderWorker: acquireVocoderConfig FAILED");
        coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
        return;
    }

    const int melBins = vocoderCfg.melBins;
    const float fMax = vocoderCfg.fMax;
    if (melBins <= 0)
    {
        coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
        return;
    }

    MelSpectrogramConfig melConfig;
    melConfig.sampleRate = static_cast<int>(RenderCache::kSampleRate);
    melConfig.nMels = melBins;
    melConfig.fMax = fMax;

    auto melResult = computeLogMelSpectrogram(monoAudio.data(),
                                              static_cast<int>(monoAudio.size()),
                                              numFrames,
                                              melConfig);
    if (!melResult.ok() || melResult.value().empty())
    {
        coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
        return;
    }

    auto mel = std::move(melResult).value();
    const int actualFrames = static_cast<int>(mel.size() / melConfig.nMels);

    vocoderF0.assign(static_cast<size_t>(actualFrames), 0.0f);
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
        const float f0_0 = effectiveF0[static_cast<size_t>(srcIdx0)];
        const float f0_1 = effectiveF0[static_cast<size_t>(srcIdx1)];
        if (f0_0 > 0.0f && f0_1 > 0.0f)
            vocoderF0[static_cast<size_t>(i)] =
                static_cast<float>(std::exp(std::log(f0_0) * (1.0 - frac) + std::log(f0_1) * frac));
        else if (f0_0 > 0.0f)
            vocoderF0[static_cast<size_t>(i)] = f0_0;
        else if (f0_1 > 0.0f)
            vocoderF0[static_cast<size_t>(i)] = f0_1;
    }

    const bool allowTrailingExtension = !(boundaries.synthSampleCount > boundaries.publishSampleCount);
    fillF0GapsForVocoder(vocoderF0,
                         *contentSnap,
                         trueStartSeconds,
                         trueEndSeconds,
                         hopDuration,
                         f0FrameRate,
                         allowTrailingExtension);

    VocoderDomain::Job vocoderJob;
    vocoderJob.f0 = std::move(vocoderF0);
    vocoderJob.mel = std::move(mel);

    auto renderCache = coreJob.renderCache;
    auto targetRevision = coreJob.targetRevision;
    const ContentKey captureContentKey = coreJob.contentKey;
    const uint64_t chunkObjId = captureContentKey.objectId;
    const double jobStartSeconds = TimeCoordinate::samplesToSeconds(boundaries.trueStartSample,
                                                                    TimeCoordinate::kRenderSampleRate);
    const FrozenRenderBoundaries frozenBoundaries = boundaries;

    vocoderJob.onComplete = [this, crs,
                             renderCache,
                             targetRevision,
                             captureContentKey,
                             chunkObjId,
                             jobStartSeconds,
                             frozenBoundaries,
                             contentSnap,   // 现有 shared_ptr：不复制 notes/EqSettings
                             completion = std::move(completion)](
                                 VocoderRenderScheduler::JobResult result,
                                 const juce::String& error,
                                 const std::vector<float>& audio)
    {
        struct AsyncRenderCompletion final
        {
            ContentRenderService& service;
            ~AsyncRenderCompletion() { service.completeAsyncRenderJob(); }
        } asyncCompletion{*crs};

        const auto& boundaries = frozenBoundaries;

        if (result == VocoderRenderScheduler::JobResult::Succeeded)
        {
            std::vector<float> publishedAudio;
            if (!preparePublishedAudioFromSynthesis(boundaries, audio, publishedAudio))
            {
                AppLogger::error("ChunkRender: synthesis length mismatch for RenderCache publish objId="
                    + juce::String(static_cast<juce::int64>(chunkObjId)));
                renderCache->completeChunkRenderFailure(jobStartSeconds, targetRevision);
                return;
            }

            const auto publishResult = publishChunkWithPerNoteEq(
                *renderCache, boundaries, std::move(publishedAudio), targetRevision,
                contentSnap->notes);

            if (publishResult == RenderCache::ChunkRenderResult::InvalidInput)
            {
                renderCache->completeChunkRenderFailure(jobStartSeconds, targetRevision);
                return;
            }

            if (publishResult == RenderCache::ChunkRenderResult::Stale)
            {
                return;
            }

            notifyChunkSettled(completion, captureContentKey);
        }
        else if (result == VocoderRenderScheduler::JobResult::Cancelled)
        {
            // Cancelled (scheduler shutdown, superseded, queue overflow): requeue or defer
            RenderJob requeueJob;
            requeueJob.kind = RenderJob::Kind::Stage1Render;
            requeueJob.contentKey = captureContentKey;
            requeueJob.renderCache = renderCache;
            requeueJob.startSeconds = jobStartSeconds;
            requeueJob.targetRevision = targetRevision;
            deferOrRequeue(crs, std::move(requeueJob));
        }
        else
        {
            // Failed: real inference error
            AppLogger::error("ChunkRender: vocoder failed objId="
                + juce::String(static_cast<juce::int64>(chunkObjId))
                + " error=" + error);
            renderCache->completeChunkRenderFailure(jobStartSeconds, targetRevision);
        }
    };

    crs->beginAsyncRenderJob();
    if (!submitVocoderJob(std::move(vocoderJob), vocoderCfg.generation))
    {
        // stale generation or reconfiguring: deferOrRequeue handles both paths
        deferOrRequeue(crs, std::move(coreJob));
        crs->completeAsyncRenderJob();
        return;
    }
}

} // namespace OpenTune
