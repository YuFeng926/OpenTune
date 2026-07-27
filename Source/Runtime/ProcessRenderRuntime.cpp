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
#include <onnxruntime_cxx_api.h>

namespace OpenTune {

namespace {

// ==============================================================================
// F0 Gap Filling for Vocoder (Mel Frame Space)
// ==============================================================================
// 在渲染提交前填补 correctedF0 的零值间隙：
//   1. 内部间隙：≤50帧用 log-domain 线性插值填充
//   2. 边界延伸：起点/终点若为零，向边界外查询并延伸填充
//      - 检测延伸方向是否有 voiced 段，有则延伸到该段起点为止
//
// 目的：消除 PC-NSF-HiFiGAN 在 F0 不连续处的相位震荡（低频砰砰声）
void fillF0GapsForVocoder(
    std::vector<float>& f0,
    const std::shared_ptr<const PitchCurveSnapshot>& snap,
    double frameStartTimeSec,
    double frameEndTimeSec,
    double hopDuration,
    double f0FrameRate,
    bool allowTrailingExtension)
{
    if (f0.empty() || !snap) return;

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

            // Query PitchCurve for F0 before this chunk's start
            // We need to look backward from frameStartTimeSec
            const int lookbackF0Frames = 100;  // Look back up to 1 second (100 frames at 100fps)
            const int queryStartFrame = static_cast<int>(std::floor(frameStartTimeSec * f0FrameRate)) - lookbackF0Frames;
            const int queryEndFrame = static_cast<int>(std::floor(frameStartTimeSec * f0FrameRate));

            std::vector<float> prevF0(static_cast<size_t>(queryEndFrame - queryStartFrame), 0.0f);
            snap->renderFinalF0Range(queryStartFrame, queryEndFrame,
                [&prevF0, queryStartFrame](int frameIndex, const float* data, int length) {
                    if (!data || length <= 0) return;
                    const int offset = frameIndex - queryStartFrame;
                    if (offset < 0) return;
                    const int copyLen = std::min(length, static_cast<int>(prevF0.size()) - offset);
                    if (copyLen > 0) {
                        std::copy(data, data + copyLen, prevF0.begin() + offset);
                    }
                });

            // Find the nearest voiced F0 going backward
            float extendF0 = 0.0f;
            for (int j = static_cast<int>(prevF0.size()) - 1; j >= 0; --j) {
                if (prevF0[static_cast<size_t>(j)] > 0.0f) {
                    extendF0 = prevF0[static_cast<size_t>(j)];
                    break;
                }
            }

            // If we found a voiced F0 before, fill the leading zeros
            // But check if there's a voiced segment between extend point and firstVoicedIdx
            if (extendF0 > 0.0f) {
                // Use the closer F0 value (extendF0 or firstVoicedF0) for smoother transition
                const float fillF0 = (extendF0 > 0.0f && firstVoicedF0 > 0.0f)
                    ? std::sqrt(extendF0 * firstVoicedF0)  // Geometric mean
                    : (firstVoicedF0 > 0.0f ? firstVoicedF0 : extendF0);

                // Fill leading zeros with gradual transition
                for (int j = 0; j < firstVoicedIdx; ++j) {
                    float t = static_cast<float>(j) / static_cast<float>(firstVoicedIdx + 1);
                    // Linear interpolation in log domain
                    float logFill = std::log2(std::max(fillF0, 1e-6f));
                    float logFirst = std::log2(std::max(firstVoicedF0, 1e-6f));
                    f0[static_cast<size_t>(j)] = std::pow(2.0f, logFill + (logFirst - logFill) * t);
                }
            }
        }
    }

    // ---- Step 3: Extend trailing zeros (f0[n-1] == 0) ----
    if (allowTrailingExtension && n > 0 && f0[static_cast<size_t>(n - 1)] <= 0.0f) {
        // Find last voiced frame in current chunk
        int lastVoicedIdx = n - 1;
        while (lastVoicedIdx >= 0 && f0[static_cast<size_t>(lastVoicedIdx)] <= 0.0f) --lastVoicedIdx;

        if (lastVoicedIdx >= 0) {
            const float lastVoicedF0 = f0[static_cast<size_t>(lastVoicedIdx)];

            // Query PitchCurve for F0 after this chunk's end
            const int lookaheadF0Frames = 100;  // Look ahead up to 1 second
            const int queryStartFrame = static_cast<int>(std::ceil(frameEndTimeSec * f0FrameRate));
            const int queryEndFrame = queryStartFrame + lookaheadF0Frames;

            std::vector<float> nextF0(static_cast<size_t>(queryEndFrame - queryStartFrame), 0.0f);
            snap->renderFinalF0Range(queryStartFrame, queryEndFrame,
                [&nextF0, queryStartFrame](int frameIndex, const float* data, int length) {
                    if (!data || length <= 0) return;
                    const int offset = frameIndex - queryStartFrame;
                    if (offset < 0) return;
                    const int copyLen = std::min(length, static_cast<int>(nextF0.size()) - offset);
                    if (copyLen > 0) {
                        std::copy(data, data + copyLen, nextF0.begin() + offset);
                    }
                });

            // Find the nearest voiced F0 going forward
            float extendF0 = 0.0f;
            for (size_t j = 0; j < nextF0.size(); ++j) {
                if (nextF0[j] > 0.0f) {
                    extendF0 = nextF0[j];
                    break;
                }
            }

            // If we found a voiced F0 after, fill the trailing zeros
            if (extendF0 > 0.0f || lastVoicedF0 > 0.0f) {
                const float fillF0 = (extendF0 > 0.0f && lastVoicedF0 > 0.0f)
                    ? std::sqrt(extendF0 * lastVoicedF0)
                    : (lastVoicedF0 > 0.0f ? lastVoicedF0 : extendF0);

                // Fill trailing zeros with gradual transition
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

bool completionIsAlive(const ProcessRenderRuntime::CompletionContext& completion)
{
    return completion.alive == nullptr || completion.alive->load(std::memory_order_acquire);
}

void notifyChunkPublished(const ProcessRenderRuntime::CompletionContext& completion,
                          ContentKey key,
                          uint64_t revision)
{
    if (completionIsAlive(completion) && completion.chunkPublished)
        completion.chunkPublished(key, revision);
}

} // namespace

ProcessRenderRuntime& ProcessRenderRuntime::getInstance()
{
    static ProcessRenderRuntime instance;
    return instance;
}

ProcessRenderRuntime::~ProcessRenderRuntime()
{
    resetVocoder();
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

bool ProcessRenderRuntime::ensureVocoderReady()
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);

    if (vocoderDomain_ != nullptr)
        return true;

    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    if (!ProcessF0Runtime::getInstance().initialize(modelsDir))
        return false;

    ortEnv_ = ProcessF0Runtime::getInstance().getOrtEnv();
    if (ortEnv_ == nullptr)
        return false;

    auto domain = std::make_unique<VocoderDomain>(ortEnv_);
    const auto modelPath = modelPathForWeight(modelsDir, currentVocoderModelWeight_);
    if (!domain->initialize(modelPath))
        return false;

    vocoderDomain_ = std::move(domain);
    return true;
}

bool ProcessRenderRuntime::setVocoderModelWeight(VocoderModelWeight weight)
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    if (currentVocoderModelWeight_ == weight)
        return false;

    currentVocoderModelWeight_ = weight;
    if (vocoderDomain_ != nullptr)
    {
        vocoderDomain_->shutdown();
        vocoderDomain_.reset();
    }
    return true;
}

void ProcessRenderRuntime::resetVocoder()
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    if (vocoderDomain_ != nullptr)
    {
        vocoderDomain_->shutdown();
        vocoderDomain_.reset();
    }
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
    std::vector<float> sourceF0;
    std::vector<float> correctedF0;

    const double relChunkStartSec = job.startSeconds;
    auto coreJob = std::move(job);
    FrozenRenderBoundaries boundaries;
    int numFrames = 0;
    bool clipFound = false;
    bool boundariesFrozen = false;

    if (coreJob.audioBuffer != nullptr)
    {
        const int audioNumSamples = coreJob.audioBuffer->getNumSamples();
        const int audioNumChannels = coreJob.audioBuffer->getNumChannels();
        int workerHopSize = 512;

        if (auto* domain = getVocoderDomain())
        {
            const int currentHopSize = domain->getVocoderHopSize();
            if (currentHopSize > 0)
                workerHopSize = currentHopSize;
        }

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
    if (!snap->hasFinalF0Data())
    {
        coreJob.renderCache->markChunkAsBlank(relChunkStartSec, coreJob.targetRevision);
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

    sourceF0.assign(static_cast<size_t>(numF0Frames), 0.0f);
    snap->renderFinalF0Range(f0StartFrame, f0EndFrame,
        [&sourceF0, f0StartFrame](int frameIndex, const float* data, int length)
        {
            if (!data || length <= 0)
                return;
            const int offset = frameIndex - f0StartFrame;
            if (offset < 0)
                return;
            const int copyLen = std::min(length, static_cast<int>(sourceF0.size()) - offset);
            if (copyLen > 0)
                std::copy(data, data + copyLen, sourceF0.begin() + offset);
        });

    if (!contentSnap->pitchShiftSettings.isIdentity())
    {
        const float pitchRatio = static_cast<float>(contentSnap->pitchShiftSettings.getPitchRatio());
        for (auto& f0Val : sourceF0)
            if (f0Val > 0.0f)
                f0Val *= pitchRatio;
    }

    bool hasValidF0 = false;
    for (float f : sourceF0)
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
        return;
    }

    if (lightPitchEnabled)
    {
        const auto& originalF0Full = snap->getOriginalF0();
        const int originalF0Size = static_cast<int>(originalF0Full.size());
        if (f0StartFrame >= 0 && f0StartFrame < originalF0Size)
        {
            const bool needsVocoder = chunkNeedsVocoder(
                sourceF0.data(), numF0Frames, originalF0Full, f0StartFrame);
            if (!needsVocoder)
            {
                AutoTunePitchShifter autoTuneShifter(RenderCache::kSampleRate);
                const int safeNumF0Frames = std::min(numF0Frames, originalF0Size - f0StartFrame);
                auto shiftedAudio = autoTuneShifter.shiftChunk(
                    monoAudio.data(),
                    static_cast<int>(boundaries.publishSampleCount),
                    originalF0Full.data() + f0StartFrame,
                    sourceF0.data(),
                    safeNumF0Frames,
                    f0FrameRate);

                if (static_cast<int64_t>(shiftedAudio.size()) != boundaries.publishSampleCount)
                    shiftedAudio.resize(static_cast<size_t>(boundaries.publishSampleCount), 0.0f);

                const uint64_t objectId = coreJob.contentKey.objectId;
                const auto result = coreJob.renderCache->completeChunkRenderWithAudio(
                    boundaries.trueStartSample, boundaries.trueEndSample,
                    std::move(shiftedAudio), coreJob.targetRevision);

                if (result == RenderCache::ChunkRenderResult::InvalidInput)
                {
                    // 输入无效：调用方 bug，走 failure 收口
                    coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
                }
                else if (result == RenderCache::ChunkRenderResult::Published && objectId != 0)
                {
                    crs->getTimeStretchCache().invalidate(coreJob.contentKey);
                    notifyChunkPublished(completion, coreJob.contentKey, coreJob.targetRevision);
                }
                // Stale: 无需处理，chunk 已被新编辑重新调度

                AppLogger::debug("RenderWorker: AutoTune pitch-shift chunk objId="
                    + juce::String(static_cast<juce::int64>(objectId))
                    + " start=" + juce::String(relChunkStartSec, 3));
                return;
            }
        }
    }

    if (!ensureVocoderReady())
    {
        AppLogger::log("RenderWorker: ensureVocoderReady FAILED");
        coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
        return;
    }

    auto* domain = getVocoderDomain();
    if (domain == nullptr)
    {
        coreJob.renderCache->completeChunkRenderFailure(relChunkStartSec, coreJob.targetRevision);
        return;
    }

    MelSpectrogramConfig melConfig;
    melConfig.sampleRate = static_cast<int>(RenderCache::kSampleRate);
    melConfig.nMels = domain->getMelBins();
    melConfig.fMax = domain->getFMax();

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

    correctedF0.assign(static_cast<size_t>(actualFrames), 0.0f);
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
        const float f0_0 = sourceF0[static_cast<size_t>(srcIdx0)];
        const float f0_1 = sourceF0[static_cast<size_t>(srcIdx1)];
        if (f0_0 > 0.0f && f0_1 > 0.0f)
            correctedF0[static_cast<size_t>(i)] =
                static_cast<float>(std::exp(std::log(f0_0) * (1.0 - frac) + std::log(f0_1) * frac));
        else if (f0_0 > 0.0f)
            correctedF0[static_cast<size_t>(i)] = f0_0;
        else if (f0_1 > 0.0f)
            correctedF0[static_cast<size_t>(i)] = f0_1;
    }

    const bool allowTrailingExtension = !(boundaries.synthSampleCount > boundaries.publishSampleCount);
    fillF0GapsForVocoder(correctedF0,
                         snap,
                         trueStartSeconds,
                         trueEndSeconds,
                         hopDuration,
                         f0FrameRate,
                         allowTrailingExtension);

    VocoderDomain::Job vocoderJob;
    vocoderJob.chunkKey = (coreJob.contentKey.objectId << 32)
        | static_cast<uint64_t>(static_cast<uint32_t>(coreJob.startSample));
    vocoderJob.f0 = std::move(correctedF0);
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
            if (chunkObjId != 0)
            {
                crs->getTimeStretchCache().invalidate(captureContentKey);
                notifyChunkPublished(completion, captureContentKey, targetRevision);
            }
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
    if (!domain->submit(std::move(vocoderJob)))
    {
        renderCache->completeChunkRenderFailure(jobStartSeconds, targetRevision);
        crs->completeAsyncRenderJob();
    }
}

} // namespace OpenTune
