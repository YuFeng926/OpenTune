#include "ProcessRenderRuntime.h"

#include "../DSP/AutoTunePitchShifter.h"
#include "../DSP/MelSpectrogram.h"
#include "../Inference/ChunkRenderStrategy.h"
#include "../Inference/VocoderDomain.h"
#include "../Utils/AppLogger.h"
#include "../Utils/ChannelLayoutLogger.h"
#include "../Utils/ModelPathResolver.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeCoordinate.h"
#include "ProcessF0Runtime.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {

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

} // namespace

ProcessRenderRuntime& ProcessRenderRuntime::getInstance()
{
    // 进程寿命 heap singleton：不注册静态析构（DLL detach 持 loader lock，
    // 不得在静态析构中 join 工作线程）。domain 由最后一个客户端 detach()
    // 在正常析构上下文销毁。
    static auto* instance = new ProcessRenderRuntime;
    return *instance;
}

void ProcessRenderRuntime::attach()
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    ++clientCount_;
}

void ProcessRenderRuntime::detach()
{
    // 与 attach 同一临界区线性化：仅当计数精确归零（最后一个客户端，正常析构
    // 上下文）时销毁 domain 并推进 generation，不释放锁后再 reset。
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    --clientCount_;
    if (clientCount_ == 0)
        resetVocoderLocked();
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

void ProcessRenderRuntime::resetVocoderLocked()
{
    vocoderDomain_.reset();   // VocoderDomain 析构已调用 shutdown，无需显式调用
    ++vocoderGeneration_;
}

bool ProcessRenderRuntime::setVocoderModelWeight(VocoderModelWeight weight)
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    if (currentVocoderModelWeight_ == weight)
        return false;

    currentVocoderModelWeight_ = weight;
    resetVocoderLocked();
    return true;
}

void ProcessRenderRuntime::resetVocoder()
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    resetVocoderLocked();
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
    std::lock_guard<std::mutex> lock(vocoderMutex_);

    if (vocoderDomain_ == nullptr)
    {
        const auto modelsDir = ModelPathResolver::getModelsDirectory();
        if (!ProcessF0Runtime::getInstance().initialize(modelsDir))
            return false;

        // Env 仅以局部 shared_ptr 传入 domain：生命周期由 domain 内部持有，
        // 不存成员，消除与 F0 runtime 的重叠所有权。
        auto env = ProcessF0Runtime::getInstance().getOrtEnv();
        if (env == nullptr)
            return false;

        auto domain = std::make_unique<VocoderDomain>(env);
        const auto modelPath = modelPathForWeight(modelsDir, currentVocoderModelWeight_);
        if (!domain->initialize(modelPath))
            return false;

        vocoderDomain_ = std::move(domain);
        ++vocoderGeneration_;
    }

    out.generation = vocoderGeneration_;
    out.hopSize = vocoderDomain_->getVocoderHopSize();
    out.melBins = vocoderDomain_->getMelBins();
    out.fMax = vocoderDomain_->getFMax();
    return true;
}

bool ProcessRenderRuntime::isVocoderReady() const noexcept
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return vocoderDomain_ != nullptr;
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
    job.audioBuffer = readSource.audioBuffer;

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
        // Vocoder 配置在首次使用前就绪，并一次锁内获取 generation/hop/melBins/fMax：
        // boundaries 所用 hop 与提交校验的 generation 必须同属一个 domain。
        if (!acquireVocoderConfig(vocoderCfg))
        {
            AppLogger::log("RenderWorker: acquireVocoderConfig FAILED");
            coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
            return;
        }

        const int audioNumSamples = coreJob.audioBuffer->getNumSamples();
        const int audioNumChannels = coreJob.audioBuffer->getNumChannels();
        int workerHopSize = 512;
        if (vocoderCfg.hopSize > 0)
            workerHopSize = vocoderCfg.hopSize;

        ContentSampleRange contentRange{0, audioNumSamples};
        if (freezeRenderBoundaries(contentRange,
                                   coreJob.startSample,
                                   coreJob.endSampleExclusive,
                                   workerHopSize,
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

    if (!clipFound || !pitchCurve || monoAudio.empty() || numFrames <= 0 || !boundariesFrozen)
    {
        coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
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
        coreJob.renderCache->markChunkAsBlank(relChunkStartSec, coreJob.targetRevision);
        notifyChunkSettled(completion, coreJob.contentKey);
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
    // 否则移调不生效。
    if (contentSnap->pitchShiftSettings.isIdentity() && !snap->hasCorrectionInRange(f0StartFrame, f0EndFrame))
    {
        coreJob.renderCache->markChunkAsBlank(relChunkStartSec, coreJob.targetRevision);
        notifyChunkSettled(completion, coreJob.contentKey);
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
        coreJob.renderCache->markChunkAsBlank(relChunkStartSec, coreJob.targetRevision);
        notifyChunkSettled(completion, coreJob.contentKey);
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
                auto shiftedAudio = autoTuneShifter.shiftChunk(
                    monoAudio.data(),
                    static_cast<int>(boundaries.publishSampleCount),
                    originalF0Full.data() + f0StartFrame,
                    effectiveF0.data(),
                    safeNumF0Frames,
                    f0FrameRate,
                    firstSampleFramePhase);

                if (static_cast<int64_t>(shiftedAudio.size()) != boundaries.publishSampleCount)
                    shiftedAudio.resize(static_cast<size_t>(boundaries.publishSampleCount), 0.0f);

                const uint64_t objectId = coreJob.contentKey.objectId;
                const auto result = coreJob.renderCache->completeChunkRenderWithAudio(
                    boundaries.trueStartSample, boundaries.trueEndSample,
                    std::move(shiftedAudio), coreJob.targetRevision);

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

    // 配置快照（generation/hop/melBins/fMax）已在本函数开头随 acquireVocoderConfig()
    // 一次锁内获取（vocoderCfg），此处 melBins/fMax 与提交校验的 generation
    // 同属一个 domain，不存在跨域混用。
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
    vocoderJob.chunkKey = (coreJob.contentKey.objectId << 32)
        | static_cast<uint64_t>(static_cast<uint32_t>(coreJob.startSample));
    vocoderJob.f0 = std::move(vocoderF0);
    vocoderJob.mel = std::move(mel);

    auto renderCache = coreJob.renderCache;
    auto targetRevision = coreJob.targetRevision;
    const ContentKey captureContentKey = coreJob.contentKey;
    const uint64_t chunkObjId = captureContentKey.objectId;
    const double jobStartSeconds = TimeCoordinate::samplesToSeconds(boundaries.trueStartSample,
                                                                    TimeCoordinate::kRenderSampleRate);
    const FrozenRenderBoundaries frozenBoundaries = boundaries;

    vocoderJob.onComplete = [crs,
                             renderCache,
                             targetRevision,
                             captureContentKey,
                             chunkObjId,
                             jobStartSeconds,
                             frozenBoundaries,
                             completion = std::move(completion)](
                                 bool success,
                                 const juce::String& error,
                                 const std::vector<float>& audio)
    {
        struct AsyncRenderCompletion final
        {
            ContentRenderService& service;
            ~AsyncRenderCompletion() { service.completeAsyncRenderJob(); }
        } asyncCompletion{*crs};

        const auto& boundaries = frozenBoundaries;

        if (success)
        {
            std::vector<float> publishedAudio;
            if (!preparePublishedAudioFromSynthesis(boundaries, audio, publishedAudio))
            {
                AppLogger::error("ChunkRender: synthesis length mismatch for RenderCache publish objId="
                    + juce::String(static_cast<juce::int64>(chunkObjId)));
                renderCache->completeChunkRenderFailure(jobStartSeconds, targetRevision);
                return;
            }

            const auto result = renderCache->completeChunkRenderWithAudio(
                boundaries.trueStartSample, boundaries.trueEndSample,
                std::move(publishedAudio), targetRevision);

            if (result == RenderCache::ChunkRenderResult::InvalidInput)
            {
                // 输入无效：调用方 bug，走 failure 收口
                renderCache->completeChunkRenderFailure(jobStartSeconds, targetRevision);
                return;
            }

            if (result == RenderCache::ChunkRenderResult::Stale)
            {
                // stale completion：chunk 已被新编辑重新调度，无需再调用 completeChunkRenderFailure
                return;
            }

            // Published
            notifyChunkSettled(completion, captureContentKey);
        }
        else
        {
            AppLogger::error("ChunkRender: vocoder failed objId="
                + juce::String(static_cast<juce::int64>(chunkObjId))
                + " error=" + error);
            renderCache->completeChunkRenderFailure(jobStartSeconds, targetRevision);
        }
    };

    crs->beginAsyncRenderJob();
    if (!submitVocoderJob(std::move(vocoderJob), vocoderCfg.generation))
    {
        // stale generation：快照后、提交前另一实例 reset/重建了 domain。
        // 原样重排队 coreJob（下一轮以新 domain 配置重算），不标记失败
        // （chunk 保持 Pending，仅归还异步计数）。
        crs->enqueueRender(std::move(coreJob));
        crs->completeAsyncRenderJob();
        return;
    }
}

} // namespace OpenTune
