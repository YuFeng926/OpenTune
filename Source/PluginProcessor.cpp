#include "PluginProcessor.h"
#include "Editor/EditorFactory.h"
#include "Host/HostIntegration.h"
#include "DSP/ResamplingManager.h"
#include "DSP/MelSpectrogram.h"
#include "Inference/InferenceManager.h"
#include "Utils/ModelPathResolver.h"
#include "Utils/AppLogger.h"
#include "Utils/TimeCoordinate.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>

namespace OpenTune {

// ============================================================================
// Export Helper Functions (Anonymous Namespace)
// ============================================================================
namespace {

constexpr double kExportSampleRateHz = 44100.0;
constexpr int kExportNumChannels = 1;
constexpr int kExportMasterNumChannels = 2;
constexpr int kExportBitsPerSample = static_cast<int>(sizeof(float) * 8);

juce::AudioBuffer<float> resampleForExportIfNeeded(const juce::AudioBuffer<float>& in, double sourceSampleRate)
{
    if (sourceSampleRate <= 0.0 || std::abs(sourceSampleRate - kExportSampleRateHz) < 0.01) {
        return in;
    }

    const double durationSeconds = TimeCoordinate::samplesToSeconds(in.getNumSamples(), sourceSampleRate);
    const double ratio = sourceSampleRate / kExportSampleRateHz;
    const int outSamples = std::max(
        1,
        static_cast<int>(TimeCoordinate::secondsToSamples(durationSeconds, kExportSampleRateHz)));

    juce::AudioBuffer<float> out(in.getNumChannels(), outSamples);
    out.clear();

    for (int ch = 0; ch < in.getNumChannels(); ++ch) {
        std::vector<float> paddedInput(static_cast<size_t>(in.getNumSamples() + 8), 0.0f);
        juce::FloatVectorOperations::copy(paddedInput.data(), in.getReadPointer(ch), in.getNumSamples());
        juce::LagrangeInterpolator interpolator;
        interpolator.reset();
        interpolator.process(ratio, paddedInput.data(), out.getWritePointer(ch), outSamples);
    }

    return out;
}

// 计算 fade 增益（淡入淡出曲线计算）
inline float computeFadeGain(int64_t sampleInClip, int64_t clipLen, int64_t fadeInSamples, int64_t fadeOutSamples) {
    float fade = 1.0f;
    if (fadeInSamples > 1 && sampleInClip < fadeInSamples) {
        fade *= static_cast<float>(sampleInClip) / static_cast<float>(fadeInSamples - 1);
    }
    if (fadeOutSamples > 1 && (clipLen - 1 - sampleInClip) < fadeOutSamples) {
        fade *= static_cast<float>(clipLen - 1 - sampleInClip) / static_cast<float>(fadeOutSamples - 1);
    }
    return fade;
}

// ==============================================================================
// F0 Gap Filling for Vocoder (Mel Frame Space)
// ==============================================================================
// 在渲染提交前填补 correctedF0 中的零值间隙：
//   1. 内部间隙：≤10帧用 log-domain 线性插值填补
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
    double f0FrameRate)
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
            snap->renderF0Range(queryStartFrame, queryEndFrame,
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
    if (n > 0 && f0[static_cast<size_t>(n - 1)] <= 0.0f) {
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
            snap->renderF0Range(queryStartFrame, queryEndFrame,
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

// 渲染单个 clip 为有效导出片段：dry 信号叠加 + rendered 信号覆盖 corrected 区域
// 输出：out[ch][clipStart + i] 混合写入
// 语义：corrected 区域：rendered 覆盖 dry（不叠加）
template <typename ClipType>
void renderClipForExport(
    const ClipType& clip,
    float trackGain,
    double currentSampleRate,
    int64_t clipStartInOutput,
    juce::AudioBuffer<float>& out,
    int64_t totalLen)
{
    const int64_t clipLen = clip.audioBuffer.getNumSamples();
    if (clipLen <= 0) return;
    if (clipStartInOutput >= totalLen) return;

    const float clipGain = clip.gain;
    const float baseGain = trackGain * clipGain;
    const int64_t fadeInSamples = (clip.fadeInDuration > 0.0) 
        ? TimeCoordinate::secondsToSamples(clip.fadeInDuration, currentSampleRate) : 0;
    const int64_t fadeOutSamples = (clip.fadeOutDuration > 0.0) 
        ? TimeCoordinate::secondsToSamples(clip.fadeOutDuration, currentSampleRate) : 0;

    // 第一步：写入 dry 信号（含 gain + fade）
    for (int ch = 0; ch < out.getNumChannels(); ++ch) {
        const float* src = clip.audioBuffer.getReadPointer(std::min(ch, clip.audioBuffer.getNumChannels() - 1));
        float* dst = out.getWritePointer(ch);
        for (int64_t i = 0; i < clipLen; ++i) {
            int64_t dstIndex = clipStartInOutput + i;
            if (dstIndex < 0 || dstIndex >= totalLen) continue;
            float fade = computeFadeGain(i, clipLen, fadeInSamples, fadeOutSamples);
            dst[static_cast<size_t>(dstIndex)] += src[static_cast<size_t>(i)] * baseGain * fade;
        }
    }

    // 第二步：若有 corrected 区域且缓存可读，用 rendered 覆盖该段
    if (!clip.pitchCurve || !clip.renderCache) return;
    if (!clip.pitchCurve->hasRenderableCorrectedF0()) return;

    auto snapshot = clip.pitchCurve->getSnapshot();
    const auto& segments = snapshot->getCorrectedSegments();
    const int hop = snapshot->getHopSize();
    const double f0Sr = snapshot->getSampleRate();
    if (segments.empty() || hop <= 0 || f0Sr <= 0.0) return;

    const auto frameToSample = [&](int frame) -> int64_t {
        const double frameSeconds = (static_cast<double>(frame) * static_cast<double>(hop)) / f0Sr;
        return TimeCoordinate::secondsToSamples(frameSeconds, currentSampleRate);
    };

    for (const auto& seg : segments) {
        int64_t relStart = frameToSample(seg.startFrame);
        int64_t relEnd = frameToSample(seg.endFrame);
        if (relStart < 0) relStart = 0;
        if (relEnd > clipLen) relEnd = clipLen;
        if (relEnd <= relStart) continue;

        const int num = static_cast<int>(relEnd - relStart);
        std::vector<float> tmp(static_cast<size_t>(num), 0.0f);
        const double relStartSeconds = TimeCoordinate::samplesToSeconds(relStart, currentSampleRate);
        const int readCount = clip.renderCache->readAtTimeForRate(
            tmp.data(), num, relStartSeconds, static_cast<int>(currentSampleRate), true);
        if (readCount < num) {
            continue;
        }

        // 用 rendered 覆盖该段（替换，不叠加）
        for (int ch = 0; ch < out.getNumChannels(); ++ch) {
            float* dst = out.getWritePointer(ch);
            for (int i = 0; i < num; ++i) {
                int64_t clipIndex = relStart + i;
                int64_t dstIndex = clipStartInOutput + clipIndex;
                if (dstIndex < 0 || dstIndex >= totalLen) continue;
                float fade = computeFadeGain(clipIndex, clipLen, fadeInSamples, fadeOutSamples);
                // 覆盖：先减去 dry，再加上 rendered
                const float* src = clip.audioBuffer.getReadPointer(std::min(ch, clip.audioBuffer.getNumChannels() - 1));
                float dryVal = src[static_cast<size_t>(clipIndex)] * baseGain * fade;
                float renderedVal = tmp[static_cast<size_t>(i)] * baseGain * fade;
                dst[static_cast<size_t>(dstIndex)] = dst[static_cast<size_t>(dstIndex)] - dryVal + renderedVal;
            }
        }
    }
}

} // anonymous namespace

template <typename ClipT>
static void computeClipSilentGaps(ClipT& clip)
{
    clip.silentGaps.clear();

    const int64_t clipLen = static_cast<int64_t>(clip.audioBuffer.getNumSamples());
    if (clipLen <= 0) {
        return;
    }

    // 使用秒数坐标，与设备采样率无关
    clip.silentGaps = SilentGapDetector::detectAllGapsAdaptive(clip.audioBuffer);
}


static juce::String encodeFloatVectorBase64(const std::vector<float>& v) {
    if (v.empty()) {
        return {};
    }
    return juce::Base64::toBase64(v.data(), v.size() * sizeof(float));
}

static bool decodeFloatVectorBase64(const juce::var& value, std::vector<float>& out) {
    out.clear();
    const juce::String s = value.toString();
    if (s.isEmpty()) {
        return true;
    }
    juce::MemoryBlock mb;
    juce::MemoryOutputStream mos(mb, false);
    if (!juce::Base64::convertFromBase64(mos, s)) { return false; }
    if (mb.getSize() % sizeof(float) != 0) {
        return false;
    }
    const size_t n = mb.getSize() / sizeof(float);
    out.resize(n);
    std::memcpy(out.data(), mb.getData(), n * sizeof(float));
    return true;
}


static bool hasReadyOriginalF0Curve(const std::shared_ptr<PitchCurve>& curve)
{
    if (!curve) {
        return false;
    }
    return !curve->getSnapshot()->getOriginalF0().empty();
}

OpenTuneAudioProcessor::TrackState::AudioClip::AudioClip(
    const OpenTuneAudioProcessor::TrackState::AudioClip& other)
    : clipId(other.clipId)
    , audioBuffer(other.audioBuffer)
    , drySignalBuffer_(other.drySignalBuffer_)
    , startSeconds(other.startSeconds)
    , gain(other.gain)
    , fadeInDuration(other.fadeInDuration)
    , fadeOutDuration(other.fadeOutDuration)
    , name(other.name)
    , colour(other.colour)
    , pitchCurve(other.pitchCurve)
    , originalF0State(other.originalF0State)
    , detectedKey(other.detectedKey)
    , renderCache(other.renderCache)
    , notes(other.notes)
    , silentGaps(other.silentGaps)
{
}

OpenTuneAudioProcessor::TrackState::AudioClip& OpenTuneAudioProcessor::TrackState::AudioClip::operator=(
    const OpenTuneAudioProcessor::TrackState::AudioClip& other)
{
    if (this == &other) {
        return *this;
    }

    clipId = other.clipId;
    audioBuffer = other.audioBuffer;
    drySignalBuffer_ = other.drySignalBuffer_;
    startSeconds = other.startSeconds;
    gain = other.gain;
    fadeInDuration = other.fadeInDuration;
    fadeOutDuration = other.fadeOutDuration;
    name = other.name;
    colour = other.colour;
    pitchCurve = other.pitchCurve;
    originalF0State = other.originalF0State;
    detectedKey = other.detectedKey;
    renderCache = other.renderCache;
    notes = other.notes;
    silentGaps = other.silentGaps;
    return *this;
}

OpenTuneAudioProcessor::TrackState::AudioClip::AudioClip(
    OpenTuneAudioProcessor::TrackState::AudioClip&& other) noexcept
    : clipId(other.clipId)
    , audioBuffer(std::move(other.audioBuffer))
    , drySignalBuffer_(std::move(other.drySignalBuffer_))
    , startSeconds(other.startSeconds)
    , gain(other.gain)
    , fadeInDuration(other.fadeInDuration)
    , fadeOutDuration(other.fadeOutDuration)
    , name(std::move(other.name))
    , colour(other.colour)
    , pitchCurve(std::move(other.pitchCurve))
    , originalF0State(other.originalF0State)
    , detectedKey(std::move(other.detectedKey))
    , renderCache(std::move(other.renderCache))
    , notes(std::move(other.notes))
    , silentGaps(std::move(other.silentGaps))
{
}

OpenTuneAudioProcessor::TrackState::AudioClip& OpenTuneAudioProcessor::TrackState::AudioClip::operator=(
    OpenTuneAudioProcessor::TrackState::AudioClip&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    clipId = other.clipId;
    audioBuffer = std::move(other.audioBuffer);
    drySignalBuffer_ = std::move(other.drySignalBuffer_);
    startSeconds = other.startSeconds;
    gain = other.gain;
    fadeInDuration = other.fadeInDuration;
    fadeOutDuration = other.fadeOutDuration;
    name = std::move(other.name);
    colour = other.colour;
    pitchCurve = std::move(other.pitchCurve);
    originalF0State = other.originalF0State;
    detectedKey = other.detectedKey;
    renderCache = std::move(other.renderCache);
    notes = std::move(other.notes);
    silentGaps = std::move(other.silentGaps);
    return *this;
}

OpenTuneAudioProcessor::OpenTuneAudioProcessor()
    : AudioProcessor(BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    AppLogger::initialize();
    AppLogger::log("OpenTuneAudioProcessor: ctor");

    editVersionParam_ = new juce::AudioParameterInt("editVersion", "EditVersion", 0, 100000, 0);
    addParameter(editVersionParam_);

    // Initialize tracks
    for (int i = 0; i < MAX_TRACKS; ++i) {
        tracks_[i].name = "Track " + juce::String(i + 1);
        tracks_[i].colour = juce::Colour::fromHSV(i * 0.3f, 0.6f, 0.8f, 1.0f);
    }

    hostIntegration_ = createHostIntegration();
    if (hostIntegration_) {
        hostIntegration_->configureInitialState(*this);
    }

    resamplingManager_ = std::make_unique<ResamplingManager>();
    renderingManager_ = std::make_unique<RenderingManager>();
    renderingManager_->initialize();
    resetPerfProbeCounters();

    chunkRenderWorkerRunning_ = true;
    chunkRenderWorkerThread_ = std::thread([this]() { chunkRenderWorkerLoop(); });
}

OpenTuneAudioProcessor::~OpenTuneAudioProcessor() {
    isPlaying_.store(false);

    {
        std::lock_guard<std::mutex> lock(chunkQueueMutex_);
        chunkRenderWorkerRunning_ = false;
        chunkQueueOrder_.clear();
        chunkPending_.clear();
    }
    chunkQueueCv_.notify_all();
    if (chunkRenderWorkerThread_.joinable()) {
        chunkRenderWorkerThread_.join();
    }

    if (renderingManager_) {
        renderingManager_->shutdown();
        renderingManager_.reset();
    }

    InferenceManager::getInstance().shutdown();

    AppLogger::shutdown();
}

bool OpenTuneAudioProcessor::ensureInferenceReady()
{
    if (inferenceReady_.load()) {
        return true;
    }

    std::scoped_lock lock(inferenceInitMutex_);

    if (inferenceReady_.load()) {
        return true;
    }

if (inferenceInitAttempted_.load()) {
        // 等待并发初始化完成后返回最终状态
        return inferenceReady_.load();
    }

    inferenceInitAttempted_.store(true);

    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    AppLogger::log("Models dir: " + juce::String(modelsDir));

    if (!ModelPathResolver::ensureOnnxRuntimeLoaded()) {
        AppLogger::log("ensureOnnxRuntimeLoaded failed");
        inferenceReady_.store(false);
        return false;
    }

    bool ok = false;
    try {
        ok = InferenceManager::getInstance().initialize(modelsDir);
    } catch (const std::exception& e) {
        AppLogger::log("InferenceManager initialize exception: " + juce::String(e.what()));
        ok = false;
    } catch (...) {
        AppLogger::log("InferenceManager initialize unknown exception");
        ok = false;
    }
    if (!ok) {
        AppLogger::log("InferenceManager initialize failed");
    }

    inferenceReady_.store(ok);
    return ok;
}

bool OpenTuneAudioProcessor::initializeInferenceIfNeeded()
{
    return ensureInferenceReady();
}

const juce::String OpenTuneAudioProcessor::getName() const {
    return JucePlugin_Name;
}

bool OpenTuneAudioProcessor::acceptsMidi() const {
    return false;
}

bool OpenTuneAudioProcessor::producesMidi() const {
    return false;
}

bool OpenTuneAudioProcessor::isMidiEffect() const {
    return false;
}

double OpenTuneAudioProcessor::getTailLengthSeconds() const {
    return 0.0;
}

int OpenTuneAudioProcessor::getNumPrograms() {
    return 1;
}

int OpenTuneAudioProcessor::getCurrentProgram() {
    return 0;
}

void OpenTuneAudioProcessor::setCurrentProgram(int index) {
    juce::ignoreUnused(index);
}

const juce::String OpenTuneAudioProcessor::getProgramName(int index) {
    if (index == 0) {
        return "Default";
    }
    return {};
}

void OpenTuneAudioProcessor::changeProgramName(int index, const juce::String& newName) {
    juce::ignoreUnused(index, newName);
}

void OpenTuneAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    const bool wasInitialized = (currentSampleRate_ > 0.0);
    const double oldSampleRate = currentSampleRate_;
    const bool sampleRateChanged = wasInitialized && (std::abs(oldSampleRate - sampleRate) > 1.0);
    
    AppLogger::log("prepareToPlay: sampleRate=" + juce::String(sampleRate, 2) +
                   " blockSize=" + juce::String(samplesPerBlock) +
                   " oldRate=" + juce::String(oldSampleRate, 0) +
                   " changed=" + (sampleRateChanged ? "true" : "false"));

    currentSampleRate_ = sampleRate;
    currentBlockSize_ = samplesPerBlock;
    
    // Calculate fade-out duration: 200ms = 0.2 seconds
    fadeOutTotalSamples_ = static_cast<int>(sampleRate * 0.2);
    
    if (sampleRateChanged && oldSampleRate > 0.0) {
        AppLogger::log("Sample rate changed: " + juce::String(oldSampleRate, 0) + 
                       " -> " + juce::String(sampleRate, 0) + 
                       ", resampling drySignalBuffer, clearing resampled cache");
        
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        for (auto& track : tracks_) {
            for (auto& clip : track.clips) {
                // Pre-resample dry signal to new device rate
                // audioBuffer stays at 44100Hz (fixed, never resampled on device rate change)
                resampleDrySignal(clip, sampleRate);
                
                // Clear only the resampled cache (not the 44.1kHz audio)
                // New renders will be resampled to the new sample rate
                if (clip.renderCache) {
                    clip.renderCache->clearResampledCache();
                }
                
                // silentGaps are in seconds, unchanged
            }
        }
    }

    doublePrecisionScratch_.setSize(std::max(1, getTotalNumOutputChannels()), std::max(1, currentBlockSize_), false, true, true);

#if JucePlugin_Enable_ARA
    prepareToPlayForARA(sampleRate,
                        samplesPerBlock,
                        getMainBusNumOutputChannels(),
                        getProcessingPrecision());
#endif
}

void OpenTuneAudioProcessor::releaseResources() {
    isPlaying_.store(false);

#if JucePlugin_Enable_ARA
    releaseResourcesForARA();
#endif
}

bool OpenTuneAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::stereo()) {
        return false;
    }

    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()) {
        return false;
    }

    return true;
}

bool OpenTuneAudioProcessor::supportsDoublePrecisionProcessing() const {
    return true;
}

void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<double>& buffer,
                                          juce::MidiBuffer& midiMessages) {
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels <= 0 || numSamples <= 0) {
        return;
    }

    if (numChannels > doublePrecisionScratch_.getNumChannels() || numSamples > doublePrecisionScratch_.getNumSamples()) {
        buffer.clear();
        return;
    }

    for (int ch = 0; ch < numChannels; ++ch) {
        const double* src = buffer.getReadPointer(ch);
        float* dst = doublePrecisionScratch_.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i) {
            dst[i] = static_cast<float>(src[i]);
        }
    }

    juce::AudioBuffer<float> floatBuffer(doublePrecisionScratch_.getArrayOfWritePointers(), numChannels, numSamples);
    processBlock(floatBuffer, midiMessages);

    for (int ch = 0; ch < numChannels; ++ch) {
        const float* src = floatBuffer.getReadPointer(ch);
        double* dst = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i) {
            dst[i] = static_cast<double>(src[i]);
        }
    }
}

void OpenTuneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midiMessages) {
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;
    const double perfStartMs = juce::Time::getMillisecondCounterHiRes();
    const auto finalizePerf = [this, perfStartMs]() {
        const double durationMs = juce::Time::getMillisecondCounterHiRes() - perfStartMs;
        recordAudioCallbackDurationMs(durationMs);
    };
    
    const int totalNumInputChannels = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

#if JucePlugin_Enable_ARA
    if (isBoundToARA()) {
        if (auto* playHead = getPlayHead()) {
            const auto pos = playHead->getPosition().orFallback(juce::AudioPlayHead::PositionInfo{});
            if (auto s = pos.getTimeInSeconds()) {
                positionAtomic_->store(*s, std::memory_order_relaxed);
            }
            isPlaying_.store(pos.getIsPlaying());
            hostIsRecording_.store(pos.getIsRecording());
            hostIsLooping_.store(pos.getIsLooping());
            if (auto bpm = pos.getBpm()) {
                hostBpm_.store(*bpm);
            }
            if (auto ts = pos.getTimeSignature()) {
                hostTimeSigNum_.store(ts->numerator);
                hostTimeSigDenom_.store(ts->denominator);
            }
            if (auto ppq = pos.getPpqPosition()) {
                hostPpqPosition_.store(*ppq);
            }
            if (auto loop = pos.getLoopPoints()) {
                hostPpqLoopStart_.store(loop->ppqStart);
                hostPpqLoopEnd_.store(loop->ppqEnd);
            }
        }
    }

    if (processBlockForARA(buffer, isRealtime(), getPlayHead())) {
        finalizePerf();
        return;
    }
#endif

    if (hostIntegration_ && hostIntegration_->processIfApplicable(*this, buffer, totalNumInputChannels, totalNumOutputChannels, numSamples)) {
        finalizePerf();
        return;
    }

    // Clear output buffer
    for (int i = 0; i < totalNumOutputChannels; ++i) {
        buffer.clear(i, 0, numSamples);
    }

    // Handle fade-out state
    bool isFading = isFadingOut_.load();
    bool isPlaying = isPlaying_.load();
    
    if (!isPlaying && !isFading) {
        // Fully stopped - clear output and reset state
        for (auto& track : tracks_) {
            track.currentRMS.store(-100.0f);
        }
        isBuffering_.store(false);
        finalizePerf();
        return;
    }

    const double currentSampleRate = currentSampleRate_.load();
    const double blockDurationSeconds = static_cast<double>(numSamples) / currentSampleRate;
    const double currentPosSeconds = positionAtomic_->load(std::memory_order_relaxed);
    const double blockEndSeconds = currentPosSeconds + blockDurationSeconds;
    const int64_t blockStartSample = TimeCoordinate::secondsToSamples(currentPosSeconds, currentSampleRate);
    const int64_t blockEndSample = blockStartSample + static_cast<int64_t>(numSamples);
    
    const juce::ScopedReadLock tracksReadLock(tracksLock_);
    
    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        auto& track = tracks_[trackId];
        
        bool shouldPlay = true;
        if (anyTrackSoloed_) {
            if (!track.isSolo) shouldPlay = false;
        } else {
            if (track.isMuted) shouldPlay = false;
        }

        if (!shouldPlay) {
            track.currentRMS.store(-100.0f);
            continue;
        }

        float trackVolume = track.volume;
        double trackRmsSum = 0.0;
        int trackSampleCount = 0;
        
        juce::AudioBuffer<float> trackBuffer(totalNumOutputChannels, numSamples);
        trackBuffer.clear();

        bool trackHasOutput = false;

        int clipIndex = 0;
        for (auto& clip : track.clips) {
            const int64_t dryLenSamples = clip.drySignalBuffer_.getNumSamples();
            if (dryLenSamples <= 0) {
                ++clipIndex;
                continue;
            }

            const int64_t clipStartSample = TimeCoordinate::secondsToSamples(clip.startSeconds, currentSampleRate);
            const int64_t clipEndSample = clipStartSample + dryLenSamples;

            if (clipEndSample <= blockStartSample || clipStartSample >= blockEndSample) {
                ++clipIndex;
                continue;
            }

            const int64_t overlapStartSample = std::max(blockStartSample, clipStartSample);
            const int64_t overlapEndSample = std::min(blockEndSample, clipEndSample);
            const int64_t samplesToCopy64 = overlapEndSample - overlapStartSample;
            if (samplesToCopy64 <= 0) {
                ++clipIndex;
                continue;
            }

            const int64_t readStartSample = overlapStartSample - clipStartSample;
            const int64_t readEndSample = readStartSample + samplesToCopy64;
            const double clipDurationSeconds = TimeCoordinate::samplesToSeconds(dryLenSamples, currentSampleRate);
            const double readStartSeconds = TimeCoordinate::samplesToSeconds(readStartSample, currentSampleRate);
            const double readEndSeconds = TimeCoordinate::samplesToSeconds(readEndSample, currentSampleRate);
            const int offsetInBlock = static_cast<int>(overlapStartSample - blockStartSample);
            const int samplesToCopy = static_cast<int>(samplesToCopy64);

            float clipGain = clip.gain * trackVolume;
            const double fadeInSeconds = clip.fadeInDuration;
            const double fadeOutSeconds = clip.fadeOutDuration;

            double visibleStartSec = 0.0;
            double visibleEndSec = 0.0;
            bool inVisibleRange = false;
            std::shared_ptr<const PitchCurveSnapshot> snap;
            if (!useDrySignalFallback_.load() && clip.renderCache && clip.pitchCurve) {
                snap = clip.pitchCurve->getSnapshot();
                if (snap->hasRenderableCorrectedF0() &&
                    snap->getCorrectedVisibleTimeBounds(visibleStartSec, visibleEndSec)) {
                    inVisibleRange = (readStartSeconds < visibleEndSec && readEndSeconds > visibleStartSec);
                }
            }

            std::vector<float> renderedBlock;
            bool hasRenderedAudio = false;
            
            if (inVisibleRange && clip.renderCache) {
                const int targetSampleRateInt = static_cast<int>(currentSampleRate);
                const int numSamplesToRead = samplesToCopy;
                
                if (numSamplesToRead > 0) {
                    renderedBlock.resize(static_cast<size_t>(numSamplesToRead));
                    const double readStartSecondsInClip = readStartSeconds;
                    
                    const int readCount = clip.renderCache->readAtTimeForRate(
                        renderedBlock.data(), numSamplesToRead, readStartSecondsInClip, 
                        targetSampleRateInt, true);
                    
                    if (readCount > 0) {
                        if (readCount < numSamplesToRead) {
                            renderedBlock.resize(static_cast<size_t>(readCount));
                        }
                        hasRenderedAudio = !renderedBlock.empty();
                    } else {
                        renderedBlock.clear();
                    }
                }
            }

            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                const int numChannels = clip.drySignalBuffer_.getNumChannels();
                int sourceCh = (numChannels > 0) ? ch % numChannels : 0;
                const float* src = (dryLenSamples > 0) 
                    ? clip.drySignalBuffer_.getReadPointer(sourceCh) 
                    : nullptr;
                float* dst = trackBuffer.getWritePointer(ch, offsetInBlock);

                const int64_t firstSampleInClip = readStartSample;
                double timeInClip = readStartSeconds;
                const double dt = 1.0 / currentSampleRate;
                for (int s = 0; s < samplesToCopy; ++s) {
                    const int64_t sampleInClip = firstSampleInClip + static_cast<int64_t>(s);
                    float gain = clipGain;
                    
                    if (fadeInSeconds > 0.0 && timeInClip < fadeInSeconds) {
                        gain *= static_cast<float>(timeInClip / fadeInSeconds);
                    }
                    if (fadeOutSeconds > 0.0 && timeInClip >= clipDurationSeconds - fadeOutSeconds) {
                        gain *= static_cast<float>((clipDurationSeconds - timeInClip) / fadeOutSeconds);
                    }

                    float dry = (src != nullptr && sampleInClip < dryLenSamples) ? src[sampleInClip] : 0.0f;
                    float out = dry;

                    if (hasRenderedAudio && s < static_cast<int>(renderedBlock.size())) {
                        out = renderedBlock[s];
                    }

                    dst[s] += out * gain;
                    timeInClip += dt;
                }
            }

            trackHasOutput = true;
            ++clipIndex;
        }

        if (trackHasOutput) {
            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                const float* src = trackBuffer.getReadPointer(ch);
                float* dst = buffer.getWritePointer(ch);
                
                for (int s = 0; s < numSamples; ++s) {
                    dst[s] += src[s];
                    trackRmsSum += src[s] * src[s];
                }
            }
            trackSampleCount = numSamples * totalNumOutputChannels;
        }

        if (trackSampleCount > 0) {
            float rms = std::sqrt(static_cast<float>(trackRmsSum / trackSampleCount));
            float db = (rms > 1e-9f) ? 20.0f * std::log10(rms) : -100.0f;
            track.currentRMS.store(db);
        } else {
            track.currentRMS.store(-100.0f);
        }
    }

    if (isFadingOut_.load()) {
        int fadeCount = fadeOutSampleCount_.load();
        int fadeTotal = fadeOutTotalSamples_;
        
        for (int sample = 0; sample < numSamples; ++sample) {
            int currentSample = fadeCount + sample;
            float fadeGain = 1.0f;
            
            if (currentSample < fadeTotal) {
                fadeGain = 1.0f - static_cast<float>(currentSample) / static_cast<float>(fadeTotal);
            } else {
                fadeGain = 0.0f;
            }
            
            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                buffer.setSample(ch, sample, buffer.getSample(ch, sample) * fadeGain);
            }
        }
        
        fadeCount += numSamples;
        fadeOutSampleCount_.store(fadeCount);
        
        if (fadeCount >= fadeTotal) {
            isFadingOut_.store(false);
            isPlaying_.store(false);
            AppLogger::log("Playback: fade-out complete, stopped");
            
            for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
                for (int s = 0; s < numSamples; ++s) {
                    buffer.setSample(ch, s, 0.0f);
                }
            }
        }
    }

    positionAtomic_->store(blockEndSeconds, std::memory_order_relaxed);
    finalizePerf();
}

void OpenTuneAudioProcessor::recordAudioCallbackDurationMs(double durationMs)
{
    if (durationMs < 0.0) {
        durationMs = 0.0;
    }
    const double cappedMs = std::min(20.0, durationMs);
    int bin = static_cast<int>(std::floor(cappedMs / PerfHistogramStepMs));
    bin = juce::jlimit(0, PerfHistogramBins - 1, bin);
    perfAudioDurationHistogram_[(size_t)bin].fetch_add(1, std::memory_order_relaxed);
    perfAudioCallbackCount_.fetch_add(1, std::memory_order_relaxed);
}

void OpenTuneAudioProcessor::recordCacheCheck(bool cacheHit)
{
    perfCacheChecks_.fetch_add(1, std::memory_order_relaxed);
    if (!cacheHit) {
        perfCacheMisses_.fetch_add(1, std::memory_order_relaxed);
    }
}

double OpenTuneAudioProcessor::computeAudioCallbackPercentileMs(double percentile) const
{
    const uint64_t total = perfAudioCallbackCount_.load(std::memory_order_relaxed);
    if (total == 0) {
        return 0.0;
    }

    const double clampedPercentile = juce::jlimit(0.0, 1.0, percentile);
    const uint64_t targetRank = static_cast<uint64_t>(std::ceil(clampedPercentile * static_cast<double>(total)));
    uint64_t cumulative = 0;

    for (int i = 0; i < PerfHistogramBins; ++i) {
        cumulative += static_cast<uint64_t>(perfAudioDurationHistogram_[(size_t)i].load(std::memory_order_relaxed));
        if (cumulative >= targetRank) {
            return static_cast<double>(i) * PerfHistogramStepMs;
        }
    }

    return static_cast<double>(PerfHistogramBins - 1) * PerfHistogramStepMs;
}

OpenTuneAudioProcessor::PerfProbeSnapshot OpenTuneAudioProcessor::getPerfProbeSnapshot() const
{
    PerfProbeSnapshot snapshot;
    snapshot.audioCallbackP99Ms = computeAudioCallbackPercentileMs(0.99);

    snapshot.cacheChecks = perfCacheChecks_.load(std::memory_order_relaxed);
    snapshot.cacheMisses = perfCacheMisses_.load(std::memory_order_relaxed);
    if (snapshot.cacheChecks > 0) {
        snapshot.cacheMissRate = static_cast<double>(snapshot.cacheMisses) / static_cast<double>(snapshot.cacheChecks);
    }

    {
        std::lock_guard<std::mutex> lock(chunkQueueMutex_);
        snapshot.renderQueueDepth = static_cast<int>(chunkQueueOrder_.size());
    }

    return snapshot;
}

void OpenTuneAudioProcessor::resetPerfProbeCounters()
{
    perfCacheChecks_.store(0, std::memory_order_relaxed);
    perfCacheMisses_.store(0, std::memory_order_relaxed);
    perfAudioCallbackCount_.store(0, std::memory_order_relaxed);
    for (auto& bin : perfAudioDurationHistogram_) {
        bin.store(0, std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* OpenTuneAudioProcessor::createEditor() {
    return createOpenTuneEditor(*this);
}

bool OpenTuneAudioProcessor::hasEditor() const {
    return true;
}

void OpenTuneAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    const juce::ScopedReadLock tracksReadLock(tracksLock_);

    juce::ValueTree state("OpenTuneState");
    state.setProperty("schemaVersion", 2, nullptr);
    state.setProperty("bpm", getBpm(), nullptr);
    state.setProperty("zoomLevel", zoomLevel_, nullptr);
    state.setProperty("trackHeight", trackHeight_, nullptr);

    juce::ValueTree tracksState("Tracks");
    for (int trackId = 0; trackId < MAX_TRACKS; ++trackId) {
        const auto& track = tracks_[trackId];
        juce::ValueTree trackState("Track");
        trackState.setProperty("trackId", trackId, nullptr);

        for (const auto& clip : track.clips) {
            if (!clip.pitchCurve) {
                continue;
            }

            juce::ValueTree clipState("Clip");
            clipState.setProperty("clipId", static_cast<juce::int64>(clip.clipId), nullptr);

            juce::ValueTree curveState("PitchCurve");
            auto snapshot = clip.pitchCurve->getSnapshot();
            curveState.setProperty("hopSize", snapshot->getHopSize(), nullptr);
            curveState.setProperty("f0SampleRate", snapshot->getSampleRate(), nullptr);
            curveState.setProperty("originalF0", encodeFloatVectorBase64(snapshot->getOriginalF0()), nullptr);
            curveState.setProperty("originalEnergy", encodeFloatVectorBase64(snapshot->getOriginalEnergy()), nullptr);

            const auto& segments = snapshot->getCorrectedSegments();
            for (const auto& seg : segments) {
                juce::ValueTree segState("Segment");
                segState.setProperty("start", seg.startFrame, nullptr);
                segState.setProperty("end", seg.endFrame, nullptr);
                segState.setProperty("source", static_cast<int>(seg.source), nullptr);
                segState.setProperty("retuneSpeed", seg.retuneSpeed, nullptr);
                segState.setProperty("vibratoDepth", seg.vibratoDepth, nullptr);
                segState.setProperty("vibratoRate", seg.vibratoRate, nullptr);
                segState.setProperty("f0", encodeFloatVectorBase64(seg.f0Data), nullptr);
                curveState.addChild(segState, -1, nullptr);
            }

            clipState.addChild(curveState, -1, nullptr);
            trackState.addChild(clipState, -1, nullptr);
        }

        tracksState.addChild(trackState, -1, nullptr);
    }
    state.addChild(tracksState, -1, nullptr);
    
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void OpenTuneAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName("OpenTuneState")) {
        juce::ValueTree state = juce::ValueTree::fromXml(*xml);
        setBpm(static_cast<double>(state.getProperty("bpm", 120.0)));
        zoomLevel_ = state.getProperty("zoomLevel", 1.0);
        trackHeight_ = state.getProperty("trackHeight", 120);

        const auto tracksState = state.getChildWithName("Tracks");
        if (tracksState.isValid()) {
            const juce::ScopedWriteLock tracksWriteLock(tracksLock_);

            for (auto trackState : tracksState) {
                if (!trackState.hasType("Track")) {
                    continue;
                }

                const int trackId = static_cast<int>(trackState.getProperty("trackId", -1));
                if (trackId < 0 || trackId >= MAX_TRACKS) {
                    continue;
                }

                auto& clips = tracks_[trackId].clips;
                for (auto clipState : trackState) {
                    if (!clipState.hasType("Clip")) {
                        continue;
                    }

                    const auto clipId = static_cast<uint64_t>(static_cast<juce::int64>(clipState.getProperty("clipId", 0)));
                    if (clipId == 0) {
                        continue;
                    }

                    auto it = std::find_if(clips.begin(), clips.end(), [clipId](const TrackState::AudioClip& c) {
                        return c.clipId == clipId;
                    });
                    if (it == clips.end()) {
                        continue;
                    }

                    auto curveState = clipState.getChildWithName("PitchCurve");
                    if (!curveState.isValid()) {
                        continue;
                    }

                    if (!it->pitchCurve) {
                        it->pitchCurve = std::make_shared<PitchCurve>();
                    }

                    std::vector<float> originalF0;
                    std::vector<float> originalEnergy;
                    decodeFloatVectorBase64(curveState.getProperty("originalF0"), originalF0);
                    decodeFloatVectorBase64(curveState.getProperty("originalEnergy"), originalEnergy);

                    it->pitchCurve->setHopSize(static_cast<int>(curveState.getProperty("hopSize", 0)));
                    it->pitchCurve->setSampleRate(static_cast<double>(curveState.getProperty("f0SampleRate", 0.0)));
                    it->pitchCurve->setOriginalF0(originalF0);
                    it->pitchCurve->setOriginalEnergy(originalEnergy);
                    it->pitchCurve->clearAllCorrections();

                    for (auto segState : curveState) {
                        if (!segState.hasType("Segment")) {
                            continue;
                        }

                        CorrectedSegment seg;
                        seg.startFrame = static_cast<int>(segState.getProperty("start", 0));
                        seg.endFrame = static_cast<int>(segState.getProperty("end", 0));
                        seg.source = static_cast<CorrectedSegment::Source>(static_cast<int>(segState.getProperty("source", 0)));
                        seg.retuneSpeed = static_cast<float>(static_cast<double>(segState.getProperty("retuneSpeed", 100.0)));
                        seg.vibratoDepth = static_cast<float>(static_cast<double>(segState.getProperty("vibratoDepth", 0.0)));
                        seg.vibratoRate = static_cast<float>(static_cast<double>(segState.getProperty("vibratoRate", 7.5)));
                        decodeFloatVectorBase64(segState.getProperty("f0"), seg.f0Data);
                        if (seg.startFrame < seg.endFrame && !seg.f0Data.empty()) {
                            it->pitchCurve->restoreCorrectedSegment(seg);
                        }
                    }

                    it->originalF0State = originalF0.empty() ? OriginalF0State::NotRequested : OriginalF0State::Ready;
                    if (it->renderCache) {
                        it->renderCache->clear();
                    }
                }
            }
        }
    }
}

void OpenTuneAudioProcessor::bumpEditVersion() {
    if (editVersionParam_ == nullptr) {
        return;
    }
    const int v = editVersionParam_->get();
    const int next = (v + 1) % 100001;
    const float norm = static_cast<float>(next) / 100000.0f;
    editVersionParam_->beginChangeGesture();
    editVersionParam_->setValueNotifyingHost(norm);
    editVersionParam_->endChangeGesture();
}

void OpenTuneAudioProcessor::showAudioSettingsDialog(juce::AudioProcessorEditor& editor)
{
    if (hostIntegration_) {
        hostIntegration_->audioSettingsRequested(editor);
        return;
    }
    juce::ignoreUnused(editor);
}

void OpenTuneAudioProcessor::setActiveTrack(int trackId) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        activeTrackId_ = trackId;
    }
}

void OpenTuneAudioProcessor::setTrackMuted(int trackId, bool muted) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        tracks_[trackId].isMuted = muted;
    }
}

bool OpenTuneAudioProcessor::isTrackMuted(int trackId) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        return tracks_[trackId].isMuted;
    }
    return false;
}

void OpenTuneAudioProcessor::setTrackSolo(int trackId, bool solo) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        tracks_[trackId].isSolo = solo;
        anyTrackSoloed_ = false;
        for (const auto& t : tracks_) {
            if (t.isSolo) {
                anyTrackSoloed_ = true;
                break;
            }
        }
    }
}

bool OpenTuneAudioProcessor::isTrackSolo(int trackId) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        return tracks_[trackId].isSolo;
    }
    return false;
}

void OpenTuneAudioProcessor::setTrackVolume(int trackId, float volume) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        tracks_[trackId].volume = volume;
    }
}

float OpenTuneAudioProcessor::getTrackVolume(int trackId) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        return tracks_[trackId].volume;
    }
    return 1.0f;
}

float OpenTuneAudioProcessor::getTrackRMS(int trackId) const {
    if (trackId >= 0 && trackId < MAX_TRACKS)
        return tracks_[trackId].currentRMS.load();
    return -100.0f;
}

void OpenTuneAudioProcessor::setTrackHeight(int height) {
    trackHeight_ = height;
}

int OpenTuneAudioProcessor::getNumClips(int trackId) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        return static_cast<int>(tracks_[trackId].clips.size());
    }
    return 0;
}

int OpenTuneAudioProcessor::getSelectedClip(int trackId) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        return tracks_[trackId].selectedClipIndex;
    }
    return -1;
}

void OpenTuneAudioProcessor::setSelectedClip(int trackId, int clipIndex) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        tracks_[trackId].selectedClipIndex = clipIndex;
    }
}

const juce::AudioBuffer<float>* OpenTuneAudioProcessor::getClipAudioBuffer(int trackId, int clipIndex) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return &clips[clipIndex].audioBuffer;
        }
    }
    return nullptr;
}

uint64_t OpenTuneAudioProcessor::getClipId(int trackId, int clipIndex) const
{
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].clipId;
        }
    }
    return 0;
}

int OpenTuneAudioProcessor::findClipIndexById(int trackId, uint64_t clipId) const
{
    if (clipId == 0) return -1;
    if (trackId < 0 || trackId >= MAX_TRACKS) return -1;
    const juce::ScopedReadLock tracksReadLock(tracksLock_);
    const auto& clips = tracks_[trackId].clips;
    for (int i = 0; i < (int)clips.size(); ++i) {
        if (clips[(size_t)i].clipId == clipId) {
            return i;
        }
    }
    return -1;
}

double OpenTuneAudioProcessor::getClipStartSeconds(int trackId, int clipIndex) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].startSeconds;
        }
    }
    return 0.0;
}

juce::String OpenTuneAudioProcessor::getClipName(int trackId, int clipIndex) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].name;
        }
    }
    return {};
}

float OpenTuneAudioProcessor::getClipGain(int trackId, int clipIndex) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].gain;
        }
    }
    return 1.0f;
}

void OpenTuneAudioProcessor::setClipStartSeconds(int trackId, int clipIndex, double startSeconds) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            clips[clipIndex].startSeconds = std::max(0.0, startSeconds);
        }
    }
}

void OpenTuneAudioProcessor::setClipGain(int trackId, int clipIndex, float gain) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            clips[clipIndex].gain = std::max(0.0f, gain);
        }
    }
}

bool OpenTuneAudioProcessor::splitClipAtSeconds(int trackId, int clipIndex, double splitSeconds) {
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        AppLogger::log("Split rejected: invalid trackId=" + juce::String(trackId));
        return false;
    }
    
    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    
    auto& clips = tracks_[trackId].clips;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips.size())) {
        AppLogger::log("Split rejected: invalid clipIndex=" + juce::String(clipIndex) + 
                       " for trackId=" + juce::String(trackId));
        return false;
    }

    auto& originalClip = clips[clipIndex];
    double clipStartSeconds = originalClip.startSeconds;
    double splitPointSeconds = splitSeconds - clipStartSeconds;
    
    // Convert to samples in stored audio (44.1kHz)
    constexpr double kStoredSampleRate = AudioConstants::StoredAudioSampleRate;
    int64_t splitPointInClip = static_cast<int64_t>(splitPointSeconds * kStoredSampleRate);
    int64_t totalSamples = originalClip.audioBuffer.getNumSamples();

    // Validate split point (allow some margin, e.g. 100ms)
    int64_t minLen = static_cast<int64_t>(0.1 * kStoredSampleRate);
    if (splitPointInClip < minLen || splitPointInClip > totalSamples - minLen) {
        AppLogger::log("Split rejected: split point out of valid range. splitPoint=" + 
                       juce::String(static_cast<double>(splitPointInClip / kStoredSampleRate), 3) + "s, clipLen=" + 
                       juce::String(static_cast<double>(totalSamples / kStoredSampleRate), 3) + "s");
        return false;
    }

    // Create new clip (Right part)
    TrackState::AudioClip newClip;
    newClip.clipId = nextClipId_.fetch_add(1);
    newClip.name = originalClip.name;
    newClip.colour = originalClip.colour;
    newClip.gain = originalClip.gain;
    newClip.startSeconds = splitSeconds;
    newClip.fadeInDuration = 0.25;
    newClip.fadeOutDuration = originalClip.fadeOutDuration; // Inherit fade out

    // Copy audio for new clip
    int64_t newLength = totalSamples - splitPointInClip;
    newClip.audioBuffer.setSize(originalClip.audioBuffer.getNumChannels(), static_cast<int>(newLength));
    for (int ch = 0; ch < originalClip.audioBuffer.getNumChannels(); ++ch) {
        newClip.audioBuffer.copyFrom(ch, 0, originalClip.audioBuffer, ch, static_cast<int>(splitPointInClip), static_cast<int>(newLength));
    }

    // Update original clip (Left part)
    // Resize audio buffer (this is destructive for the buffer, but we are splitting)
    originalClip.audioBuffer.setSize(originalClip.audioBuffer.getNumChannels(), static_cast<int>(splitPointInClip), true, true, true);
    originalClip.fadeOutDuration = 0.25;
    // originalClip.fadeInDuration remains unchanged

    // 重新计算两个 clip 的静息处和 chunk 边界（统一语义）
    computeClipSilentGaps(originalClip);
    computeClipSilentGaps(newClip);

    // Insert new clip
    clips.insert(clips.begin() + clipIndex + 1, std::move(newClip));
    
    // Update selection to the new clip (optional, but good UX)
    tracks_[trackId].selectedClipIndex = clipIndex + 1;
    
    return true;
}

void OpenTuneAudioProcessor::setClipGainById(int trackId, uint64_t clipId, float gain)
{
    if (trackId < 0 || trackId >= MAX_TRACKS || clipId == 0) {
        return;
    }

    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    auto& clips = tracks_[trackId].clips;
    for (auto& clip : clips) {
        if (clip.clipId == clipId) {
            clip.gain = std::max(0.0f, gain);
            return;
        }
    }
}

bool OpenTuneAudioProcessor::mergeSplitClips(int trackId, uint64_t originalClipId, uint64_t newClipId, int targetClipIndex)
{
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        AppLogger::log("Merge rejected: invalid trackId=" + juce::String(trackId));
        return false;
    }
    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    auto& clips = tracks_[trackId].clips;
    
    int originalIndex = -1;
    int newIndex = -1;
    
    // 查找两个 clip 的索引
    for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
        if (clips[i].clipId == originalClipId) originalIndex = i;
        if (clips[i].clipId == newClipId) newIndex = i;
    }
    
    if (originalIndex < 0 || newIndex < 0) {
        AppLogger::log("Merge rejected: clip not found. originalClipId=" + 
                       juce::String(static_cast<juce::int64>(originalClipId)) + 
                       " newClipId=" + juce::String(static_cast<juce::int64>(newClipId)));
        return false;
    }
    
    auto& originalClip = clips[originalIndex];
    auto& newClip = clips[newIndex];
    
    // 合并音频缓冲区
    int originalSamples = originalClip.audioBuffer.getNumSamples();
    int newSamples = newClip.audioBuffer.getNumSamples();
    int channels = std::max(originalClip.audioBuffer.getNumChannels(), newClip.audioBuffer.getNumChannels());
    
    // 确保原始 clip 有足够的通道
    if (originalClip.audioBuffer.getNumChannels() < channels) {
        originalClip.audioBuffer.setSize(channels, originalSamples, true, true, false);
    }

    // 追加新 clip 的音频数据
    originalClip.audioBuffer.setSize(channels, originalSamples + newSamples, true, true, false);
    for (int ch = 0; ch < newClip.audioBuffer.getNumChannels(); ++ch) {
        originalClip.audioBuffer.copyFrom(ch, originalSamples, newClip.audioBuffer, ch, 0, newSamples);
    }

    // 恢复 fadeout 设置
    originalClip.fadeOutDuration = newClip.fadeOutDuration;
    
    // 重新计算静息处和 chunk 边界（统一语义）
    computeClipSilentGaps(originalClip);
    
    // 删除新 clip
    clips.erase(clips.begin() + newIndex);
    
    // 更新选择
    if (targetClipIndex >= 0 && targetClipIndex < static_cast<int>(clips.size())) {
        tracks_[trackId].selectedClipIndex = targetClipIndex;
    }
    
    return true;
}

bool OpenTuneAudioProcessor::deleteClip(int trackId, int clipIndex) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
// UI线程修改 tracks_ 数据时使用写锁
    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        
        auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            clips.erase(clips.begin() + clipIndex);
            if (tracks_[trackId].selectedClipIndex >= static_cast<int>(clips.size())) {
                tracks_[trackId].selectedClipIndex = std::max(0, static_cast<int>(clips.size()) - 1);
            }
            return true;
        }
    }
    return false;
}

bool OpenTuneAudioProcessor::getClipSnapshot(int trackId, uint64_t clipId, ClipSnapshot& out) const
{
    if (trackId < 0 || trackId >= MAX_TRACKS || clipId == 0) return false;
    const juce::ScopedReadLock tracksReadLock(tracksLock_);
    const auto& clips = tracks_[trackId].clips;
    auto it = std::find_if(clips.begin(), clips.end(), [clipId](const TrackState::AudioClip& c) {
        return c.clipId == clipId;
    });
    if (it == clips.end()) return false;

    const auto& clip = *it;
    out.audioBuffer.makeCopyOf(clip.audioBuffer);
    out.startSeconds = clip.startSeconds;
    out.gain = clip.gain;
    out.fadeInDuration = clip.fadeInDuration;
    out.fadeOutDuration = clip.fadeOutDuration;
    out.name = clip.name;
    out.colour = clip.colour;
    out.pitchCurve = clip.pitchCurve;
    out.originalF0State = clip.originalF0State;
    out.detectedKey = clip.detectedKey;
    out.renderCache = clip.renderCache;
    return true;
}

double OpenTuneAudioProcessor::getClipStartSecondsById(int trackId, uint64_t clipId) const
{
    if (trackId < 0 || trackId >= MAX_TRACKS || clipId == 0) return 0.0;
    const juce::ScopedReadLock tracksReadLock(tracksLock_);
    const auto& clips = tracks_[trackId].clips;
    auto it = std::find_if(clips.begin(), clips.end(), [clipId](const TrackState::AudioClip& c) {
        return c.clipId == clipId;
    });
    return (it != clips.end()) ? it->startSeconds : 0.0;
}

bool OpenTuneAudioProcessor::setClipStartSecondsById(int trackId, uint64_t clipId, double startSeconds)
{
    if (trackId < 0 || trackId >= MAX_TRACKS || clipId == 0) return false;
    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    auto& clips = tracks_[trackId].clips;
    auto it = std::find_if(clips.begin(), clips.end(), [clipId](const TrackState::AudioClip& c) {
        return c.clipId == clipId;
    });
    if (it == clips.end()) return false;
    it->startSeconds = std::max(0.0, startSeconds);
    return true;
}

bool OpenTuneAudioProcessor::deleteClipById(int trackId, uint64_t clipId, ClipSnapshot* deletedOut, int* deletedIndexOut)
{
    if (trackId < 0 || trackId >= MAX_TRACKS || clipId == 0) return false;

    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    auto& clips = tracks_[trackId].clips;
    auto it = std::find_if(clips.begin(), clips.end(), [clipId](const TrackState::AudioClip& c) {
        return c.clipId == clipId;
    });
    if (it == clips.end()) return false;

    const int clipIndex = static_cast<int>(std::distance(clips.begin(), it));
    if (deletedOut != nullptr) {
        deletedOut->audioBuffer.makeCopyOf(it->audioBuffer);
        deletedOut->startSeconds = it->startSeconds;
        deletedOut->gain = it->gain;
        deletedOut->fadeInDuration = it->fadeInDuration;
        deletedOut->fadeOutDuration = it->fadeOutDuration;
        deletedOut->name = it->name;
        deletedOut->colour = it->colour;
        deletedOut->pitchCurve = it->pitchCurve;
        deletedOut->originalF0State = it->originalF0State;
        deletedOut->detectedKey = it->detectedKey;
        deletedOut->renderCache = it->renderCache;
    }

    if (deletedIndexOut != nullptr) {
        *deletedIndexOut = clipIndex;
    }

    clips.erase(clips.begin() + clipIndex);
    if (tracks_[trackId].selectedClipIndex >= static_cast<int>(clips.size())) {
        tracks_[trackId].selectedClipIndex = std::max(0, static_cast<int>(clips.size()) - 1);
    }
    return true;
}

bool OpenTuneAudioProcessor::insertClipSnapshot(int trackId, int insertIndex, const ClipSnapshot& snap, uint64_t forcedClipId)
{
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        AppLogger::log("InsertClip rejected: invalid trackId=" + juce::String(trackId));
        return false;
    }
    
    if (snap.audioBuffer.getNumSamples() <= 0) {
        AppLogger::log("InsertClip rejected: empty audio buffer (zero samples) for clipId=" + 
                       (forcedClipId != 0 ? juce::String(static_cast<juce::int64>(forcedClipId)) : "new"));
        return false;
    }
    if (snap.audioBuffer.getNumChannels() <= 0) {
        AppLogger::log("InsertClip rejected: invalid channel count=" + juce::String(snap.audioBuffer.getNumChannels()));
        return false;
    }
    
    // UI线程修改 tracks_ 数据时使用写锁
    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    
    auto& clips = tracks_[trackId].clips;
    insertIndex = juce::jlimit(0, (int)clips.size(), insertIndex);

    TrackState::AudioClip clip;
    clip.clipId = (forcedClipId != 0) ? forcedClipId : nextClipId_.fetch_add(1);
    clip.audioBuffer.makeCopyOf(snap.audioBuffer);
    clip.startSeconds = snap.startSeconds;
    clip.gain = snap.gain;
    clip.fadeInDuration = snap.fadeInDuration;
    clip.fadeOutDuration = snap.fadeOutDuration;
    clip.name = snap.name;
    clip.colour = snap.colour;
    clip.pitchCurve = snap.pitchCurve;
    clip.originalF0State = snap.originalF0State;
    clip.detectedKey = snap.detectedKey;
    clip.renderCache = snap.renderCache;

    // 计算静息处和 chunk 边界（统一语义）
    computeClipSilentGaps(clip);

    clips.insert(clips.begin() + insertIndex, std::move(clip));
    if (tracks_[trackId].selectedClipIndex >= insertIndex) {
        tracks_[trackId].selectedClipIndex += 1;
    }
    return true;
}

bool OpenTuneAudioProcessor::hasTrackAudio(int trackId) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        return !tracks_[trackId].clips.empty();
    }
    return false;
}

// 导出单个Clip的音频
bool OpenTuneAudioProcessor::exportClipAudio(int trackId, int clipIndex, const juce::File& file) {
    lastExportError_.clear();
    
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        lastExportError_ = "无效的轨道ID: " + juce::String(trackId);
        return false;
    }
    
    if (currentSampleRate_ <= 0.0) {
        lastExportError_ = "采样率未初始化";
        return false;
    }

    const juce::ScopedReadLock tracksReadLock(tracksLock_);
    
    auto& track = tracks_[static_cast<size_t>(trackId)];
    if (clipIndex < 0 || clipIndex >= static_cast<int>(track.clips.size())) {
        lastExportError_ = "无效的Clip索引: " + juce::String(clipIndex);
        return false;
    }
    
    auto& clip = track.clips[static_cast<size_t>(clipIndex)];
    const int64_t clipLen = clip.audioBuffer.getNumSamples();
    if (clipLen <= 0) {
        lastExportError_ = "Clip音频长度为零";
        return false;
    }
    
    // 创建输出缓冲区
    juce::AudioBuffer<float> out(kExportNumChannels, static_cast<int>(clipLen));
    out.clear();
    
    // 使用统一 helper 渲染（rendered 优先 -> dry 回退）
    renderClipForExport(clip, track.volume, currentSampleRate_, 0, out, clipLen);
    auto exportBuffer = resampleForExportIfNeeded(out, currentSampleRate_);
    
    // 写入文件
    auto outFile = file;
    if (!outFile.hasFileExtension(".wav")) {
        outFile = outFile.withFileExtension(".wav");
    }
    outFile.deleteFile();
    
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream(outFile.createOutputStream());
    if (!stream) {
        lastExportError_ = "无法创建输出文件";
        return false;
    }
    
    std::unique_ptr<juce::OutputStream> outStream(stream.release());
    
    auto options = juce::AudioFormatWriterOptions{}
        .withSampleRate(kExportSampleRateHz)
        .withNumChannels(exportBuffer.getNumChannels())
        .withBitsPerSample(kExportBitsPerSample);
    
    auto writer = wav.createWriterFor(outStream, options);
    if (!writer) {
        lastExportError_ = "无法创建WAV写入器";
        return false;
    }
    
    return writer->writeFromAudioSampleBuffer(exportBuffer, 0, exportBuffer.getNumSamples());
}

bool OpenTuneAudioProcessor::exportTrackAudio(int trackId, const juce::File& file) {
    lastExportError_.clear();
    
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        lastExportError_ = "无效的轨道ID: " + juce::String(trackId);
        return false;
    }
    if (currentSampleRate_ <= 0.0) {
        lastExportError_ = "采样率未初始化";
        return false;
    }

    const juce::ScopedReadLock tracksReadLock(tracksLock_);
    
    auto& track = tracks_[static_cast<size_t>(trackId)];
    if (track.clips.empty()) {
        lastExportError_ = "轨道 " + juce::String(trackId + 1) + " 没有音频片段";
        return false;
    }

    // 计算总长度
    int64_t totalLen = 0;
    for (const auto& clip : track.clips) {
        int64_t clipStart = TimeCoordinate::secondsToSamples(clip.startSeconds, currentSampleRate_);
        int64_t clipEnd = clipStart + clip.audioBuffer.getNumSamples();
        totalLen = std::max(totalLen, clipEnd);
    }
    if (totalLen <= 0) {
        lastExportError_ = "音频总长度为零或无效";
        return false;
    }

    juce::AudioBuffer<float> out(kExportNumChannels, static_cast<int>(totalLen));
    out.clear();

    // 遍历所有 clip，使用统一 helper 渲染
    for (const auto& clip : track.clips) {
        const int64_t clipStart = TimeCoordinate::secondsToSamples(clip.startSeconds, currentSampleRate_);
        renderClipForExport(clip, track.volume, currentSampleRate_, clipStart, out, totalLen);
    }

    auto exportBuffer = resampleForExportIfNeeded(out, currentSampleRate_);

    auto outFile = file;
    if (!outFile.hasFileExtension(".wav")) {
        outFile = outFile.withFileExtension(".wav");
    }
    outFile.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream(outFile.createOutputStream());
    if (!stream) return false;

    std::unique_ptr<juce::OutputStream> outStream(stream.release());

    auto options = juce::AudioFormatWriterOptions{}
        .withSampleRate(kExportSampleRateHz)
        .withNumChannels(exportBuffer.getNumChannels())
        .withBitsPerSample(kExportBitsPerSample);

    auto writer = wav.createWriterFor(outStream, options);
    if (!writer) return false;

    return writer->writeFromAudioSampleBuffer(exportBuffer, 0, exportBuffer.getNumSamples());
}

bool OpenTuneAudioProcessor::exportMasterMixAudio(const juce::File& file) {
    if (currentSampleRate_ <= 0.0) return false;

    const juce::ScopedReadLock tracksReadLock(tracksLock_);

    // 计算总长度
    int64_t totalLen = 0;
    for (int trackId = 0; trackId < static_cast<int>(tracks_.size()); ++trackId) {
        const auto& track = tracks_[static_cast<size_t>(trackId)];
        for (const auto& clip : track.clips) {
            int64_t clipStart = TimeCoordinate::secondsToSamples(clip.startSeconds, currentSampleRate_);
            int64_t clipEnd = clipStart + clip.audioBuffer.getNumSamples();
            totalLen = std::max(totalLen, clipEnd);
        }
    }
    if (totalLen <= 0) return false;

    juce::AudioBuffer<float> mix(kExportMasterNumChannels, static_cast<int>(totalLen));
    mix.clear();

    // 应用 solo/mute 规则
    bool anySolo = false;
    for (const auto& t : tracks_) {
        if (t.isSolo) {
            anySolo = true;
            break;
        }
    }

    // 遍历所有轨道，使用统一 helper 渲染
    for (int trackId = 0; trackId < static_cast<int>(tracks_.size()); ++trackId) {
        auto& track = tracks_[static_cast<size_t>(trackId)];
        
        // Solo/Mute 逻辑
        if (anySolo) {
            if (!track.isSolo) continue;
        } else {
            if (track.isMuted) continue;
        }

        if (track.clips.empty()) continue;

        for (const auto& clip : track.clips) {
            const int64_t clipStart = TimeCoordinate::secondsToSamples(clip.startSeconds, currentSampleRate_);
            renderClipForExport(clip, track.volume, currentSampleRate_, clipStart, mix, totalLen);
        }
    }

    auto exportBuffer = resampleForExportIfNeeded(mix, currentSampleRate_);

    auto outFile = file;
    if (!outFile.hasFileExtension(".wav")) {
        outFile = outFile.withFileExtension(".wav");
    }
    outFile.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream(outFile.createOutputStream());
    if (!stream) return false;

    std::unique_ptr<juce::OutputStream> outStream(stream.release());

    auto options = juce::AudioFormatWriterOptions{}
        .withSampleRate(kExportSampleRateHz)
        .withNumChannels(exportBuffer.getNumChannels())
        .withBitsPerSample(kExportBitsPerSample);

    auto writer = wav.createWriterFor(outStream, options);
    if (!writer) return false;

    return writer->writeFromAudioSampleBuffer(exportBuffer, 0, exportBuffer.getNumSamples());
}

void OpenTuneAudioProcessor::setPlaying(bool playing) {
    if (playing) {
        // Start playing: cancel any pending fade-out
        isFadingOut_.store(false);
        isPlaying_.store(true);
        useDrySignalFallback_.store(false);
        isBuffering_.store(false);
        AppLogger::log("Playback: start");
    } else {
        // Stop/pause: start fade-out instead of immediate stop
        if (isPlaying_.load()) {
            isFadingOut_.store(true);
            fadeOutSampleCount_.store(0);
            AppLogger::log("Playback: fade-out started");
        }
    }
}

void OpenTuneAudioProcessor::setLoopEnabled(bool enabled) {
    loopEnabled_.store(enabled);
}

void OpenTuneAudioProcessor::setPosition(double seconds) {
    positionAtomic_->store(seconds, std::memory_order_relaxed);
}

double OpenTuneAudioProcessor::getPosition() const {
    return positionAtomic_->load(std::memory_order_relaxed);
}

void OpenTuneAudioProcessor::setBpm(double bpm) {
    bpm_ = bpm;

#if !JucePlugin_Build_Standalone
    hostBpm_.store(bpm);
#endif
}

void OpenTuneAudioProcessor::setZoomLevel(double zoom) {
    zoomLevel_ = zoom;
}

// ============================================================================
// Two-phase Import Implementation
// ============================================================================

bool OpenTuneAudioProcessor::prepareImportClip(int trackId,
                                                juce::AudioBuffer<float>&& inBuffer,
                                                double inSampleRate,
                                                const juce::String& clipName,
                                                PreparedImportClip& out)
{
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        AppLogger::log("Import rejected: invalid trackId=" + juce::String(trackId));
        return false;
    }
    if (inBuffer.getNumSamples() <= 0) {
        AppLogger::log("Import rejected: empty audio buffer (zero samples) for '" + clipName + "'");
        return false;
    }
    if (inBuffer.getNumChannels() <= 0) {
        AppLogger::log("Import rejected: invalid channel count=" + juce::String(inBuffer.getNumChannels()) + " for '" + clipName + "'");
        return false;
    }
    if (inSampleRate <= 0.0) {
        AppLogger::log("Import rejected: invalid sample rate=" + juce::String(inSampleRate, 2) + " for '" + clipName + "'");
        return false;
    }
    
    PerfTimer perfTimer("prepareImportClip");
    
    out.trackId = trackId;
    out.clipName = clipName;

    // 重采样到固定 44100Hz（用于存储和渲染）
    const double targetSampleRate = TimeCoordinate::kRenderSampleRate;
    if (std::abs(inSampleRate - targetSampleRate) > 1.0) {
        const int numChannels = inBuffer.getNumChannels();
        const int originalLen = inBuffer.getNumSamples();
        const double sourceDurationSeconds = TimeCoordinate::samplesToSeconds(originalLen, inSampleRate);
        const int newLen = juce::jmax(
            1,
            static_cast<int>(TimeCoordinate::secondsToSamples(sourceDurationSeconds, targetSampleRate)));
        
        out.hostRateBuffer.setSize(numChannels, newLen);
        
        for (int ch = 0; ch < numChannels; ++ch) {
            auto resampledData = resamplingManager_->upsampleForHost(
                inBuffer.getReadPointer(ch),
                originalLen,
                static_cast<int>(inSampleRate),
                static_cast<int>(targetSampleRate)
            );
            const int toCopy = juce::jmin(newLen, static_cast<int>(resampledData.size()));
            out.hostRateBuffer.copyFrom(ch, 0, resampledData.data(), toCopy);
        }
    } else {
        out.hostRateBuffer = std::move(inBuffer);
    }
    
    // 后处理数据延后到波形完全可见后再异步计算
    out.silentGaps.clear();
    
    return true;
}

bool OpenTuneAudioProcessor::commitPreparedImportClip(PreparedImportClip&& prepared)
{
    if (prepared.trackId < 0 || prepared.trackId >= MAX_TRACKS) return false;
    
    PerfTimer perfTimer("commitPreparedImportClip");
    
    // 写锁内只做轻量对象挂载
    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    
    TrackState::AudioClip clip;
    clip.clipId = nextClipId_.fetch_add(1);
    clip.audioBuffer = std::move(prepared.hostRateBuffer);  // Fixed 44100Hz
    clip.startSeconds = 0.0;
    clip.gain = 1.0f;
    clip.name = prepared.clipName;
    clip.colour = juce::Colour::fromHSV(prepared.trackId * 0.3f, 0.6f, 0.8f, 1.0f);
    clip.originalF0State = OriginalF0State::NotRequested;
    clip.silentGaps = std::move(prepared.silentGaps);
    clip.renderCache = std::make_shared<RenderCache>();
    
    // Initialize drySignalBuffer_ to current device rate
    const double deviceSampleRate = currentSampleRate_.load(std::memory_order_relaxed);
    resampleDrySignal(clip, deviceSampleRate);
    
    tracks_[prepared.trackId].clips.push_back(std::move(clip));
    return true;
}

bool OpenTuneAudioProcessor::prepareDeferredClipPostProcess(int trackId,
                                                            uint64_t clipId,
                                                            OpenTuneAudioProcessor::PreparedClipPostProcess& out) const
{
    if (trackId < 0 || trackId >= MAX_TRACKS || clipId == 0) {
        return false;
    }

    juce::AudioBuffer<float> clipAudio;
    {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        auto it = std::find_if(clips.begin(), clips.end(), [clipId](const TrackState::AudioClip& c) {
            return c.clipId == clipId;
        });
        if (it == clips.end()) {
            return false;
        }
        if (it->audioBuffer.getNumSamples() <= 0 || it->audioBuffer.getNumChannels() <= 0) {
            return false;
        }
        clipAudio.makeCopyOf(it->audioBuffer);
    }

    out = OpenTuneAudioProcessor::PreparedClipPostProcess{};
    out.trackId = trackId;
    out.clipId = clipId;

    {
        PerfTimer perfSilent("deferredImportPostProcess_silentGapDetection");
        out.silentGaps = SilentGapDetector::detectAllGapsAdaptive(clipAudio);
    }

    return true;
}

bool OpenTuneAudioProcessor::commitDeferredClipPostProcess(int trackId,
                                                           uint64_t clipId,
                                                           OpenTuneAudioProcessor::PreparedClipPostProcess&& prepared)
{
    if (trackId < 0 || trackId >= MAX_TRACKS || clipId == 0) {
        return false;
    }

    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    auto& clips = tracks_[trackId].clips;
    auto it = std::find_if(clips.begin(), clips.end(), [clipId](const TrackState::AudioClip& c) {
        return c.clipId == clipId;
    });
    if (it == clips.end()) {
        return false;
    }

    it->silentGaps = std::move(prepared.silentGaps);
    return true;
}

// ============================================================================
// Clip Movement
// ============================================================================

bool OpenTuneAudioProcessor::moveClipToTrack(int sourceTrackId, int targetTrackId, uint64_t clipId, double newStartSeconds)
{
    // 验证参数
    if (sourceTrackId < 0 || sourceTrackId >= MAX_TRACKS || targetTrackId < 0 || targetTrackId >= MAX_TRACKS)
        return false;
    if (sourceTrackId == targetTrackId)
        return false;
    
    // UI线程修改 tracks_ 数据时使用写锁
    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    
    auto& sourceClips = tracks_[sourceTrackId].clips;
    auto& targetClips = tracks_[targetTrackId].clips;
    
    // 查找要移动的 clip
    auto it = std::find_if(sourceClips.begin(), sourceClips.end(),
        [clipId](const TrackState::AudioClip& c) { return c.clipId == clipId; });
    
    if (it == sourceClips.end())
        return false;
    
    // 移动 clip 到目标轨道
    TrackState::AudioClip movedClip = std::move(*it);
    movedClip.startSeconds = std::max(0.0, newStartSeconds);
    // 更新颜色为目标轨道的颜色
    movedClip.colour = juce::Colour::fromHSV(targetTrackId * 0.3f, 0.6f, 0.8f, 1.0f);
    sourceClips.erase(it);
    targetClips.push_back(std::move(movedClip));
    
    // 更新选中状态
    tracks_[targetTrackId].selectedClipIndex = static_cast<int>(targetClips.size()) - 1;
    
    return true;
}

std::shared_ptr<PitchCurve> OpenTuneAudioProcessor::getClipPitchCurve(int trackId, int clipIndex) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].pitchCurve;
        }
    }
    return nullptr;
}

void OpenTuneAudioProcessor::setClipPitchCurve(int trackId, int clipIndex, std::shared_ptr<PitchCurve> curve) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            clips[clipIndex].pitchCurve = curve;
            clips[clipIndex].originalF0State = hasReadyOriginalF0Curve(curve)
                ? OriginalF0State::Ready
                : OriginalF0State::NotRequested;
        }
    }
}

OriginalF0State OpenTuneAudioProcessor::getClipOriginalF0State(int trackId, int clipIndex) const
{
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].originalF0State;
        }
    }
    return OriginalF0State::NotRequested;
}

void OpenTuneAudioProcessor::setClipOriginalF0State(int trackId, int clipIndex, OriginalF0State state)
{
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            clips[clipIndex].originalF0State = state;
        }
    }
}

bool OpenTuneAudioProcessor::setClipOriginalF0StateById(int trackId, uint64_t clipId, OriginalF0State state)
{
    if (trackId < 0 || trackId >= MAX_TRACKS || clipId == 0) {
        return false;
    }

    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    auto& clips = tracks_[trackId].clips;
    auto it = std::find_if(clips.begin(), clips.end(), [clipId](const TrackState::AudioClip& c) {
        return c.clipId == clipId;
    });
    if (it == clips.end()) {
        return false;
    }
    it->originalF0State = state;
    return true;
}

DetectedKey OpenTuneAudioProcessor::getClipDetectedKey(int trackId, int clipIndex) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].detectedKey;
        }
    }
    return DetectedKey{};
}

void OpenTuneAudioProcessor::setClipDetectedKey(int trackId, int clipIndex, const DetectedKey& key) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            clips[clipIndex].detectedKey = key;
        }
    }
}

std::vector<Note> OpenTuneAudioProcessor::getClipNotes(int trackId, int clipIndex) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].notes;
        }
    }
    return {};
}

void OpenTuneAudioProcessor::setClipNotes(int trackId, int clipIndex, const std::vector<Note>& notes) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            clips[clipIndex].notes = notes;
        }
    }
}

const std::vector<SilentGap>& OpenTuneAudioProcessor::getClipSilentGaps(int trackId, int clipIndex) const {
    static const std::vector<SilentGap> empty;
    static thread_local std::vector<SilentGap> copy;
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            copy = clips[clipIndex].silentGaps;
            return copy;
        }
    }
    return empty;
}

int OpenTuneAudioProcessor::getClipChunkCountInRange(int trackId, int clipIndex, double relStartSeconds, double relEndSeconds) const
{
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        return 0;
    }

    const juce::ScopedReadLock tracksReadLock(tracksLock_);
    const auto& clips = tracks_[trackId].clips;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips.size())) {
        return 0;
    }

    const auto& clip = clips[(size_t)clipIndex];
    const int64_t clipLen = clip.audioBuffer.getNumSamples();
    if (clipLen <= 0) {
        return 0;
    }

    const double relStart = std::min(relStartSeconds, relEndSeconds);
    const double relEnd = std::max(relStartSeconds, relEndSeconds);

    const double clipDurationSec = static_cast<double>(clipLen) / SilentGapDetector::kInternalSampleRate;
    const auto& gaps = clip.silentGaps;

    // Build chunk boundaries from gaps (use gap midpoints)
    std::vector<double> boundaries;
    boundaries.reserve(gaps.size() + 2);
    boundaries.push_back(0.0);
    for (const auto& gap : gaps) {
        double mid = (gap.startSeconds + gap.endSeconds) / 2.0;
        boundaries.push_back(mid);
    }
    boundaries.push_back(clipDurationSec);
    std::sort(boundaries.begin(), boundaries.end());
    auto last = std::unique(boundaries.begin(), boundaries.end());
    boundaries.erase(last, boundaries.end());

    int count = 0;
    for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
        const double cStart = boundaries[i];
        const double cEnd = boundaries[i + 1];
        if (cEnd > relStart && cStart < relEnd) {
            ++count;
        }
    }
    return count;
}

SilentGapDetector::DetectionConfig OpenTuneAudioProcessor::getSilentGapDetectionConfig() const {
    return SilentGapDetector::getConfig();
}

void OpenTuneAudioProcessor::setSilentGapDetectionConfig(const SilentGapDetector::DetectionConfig& config) {
    SilentGapDetector::setConfig(config);
}

void OpenTuneAudioProcessor::resampleDrySignal(TrackState::AudioClip& clip, double deviceSampleRate)
{
    // audioBuffer is fixed at 44100Hz (kRenderSampleRate)
    // drySignalBuffer_ is pre-resampled to device rate for playback
    constexpr double kStoredAudioSampleRate = TimeCoordinate::kRenderSampleRate; // 44100.0
    
    if (clip.audioBuffer.getNumSamples() <= 0) {
        clip.drySignalBuffer_.setSize(0, 0);
        return;
    }
    
    // If device rate matches stored rate, just copy
    if (std::abs(kStoredAudioSampleRate - deviceSampleRate) < 1.0) {
        clip.drySignalBuffer_.makeCopyOf(clip.audioBuffer);
        return;
    }
    
    // Resample from 44100Hz to device rate
    const int numChannels = clip.audioBuffer.getNumChannels();
    const int srcSamples = clip.audioBuffer.getNumSamples();
    const double sourceDurationSeconds = TimeCoordinate::samplesToSeconds(srcSamples, kStoredAudioSampleRate);
    const int newLen = juce::jmax(
        1,
        static_cast<int>(TimeCoordinate::secondsToSamples(sourceDurationSeconds, deviceSampleRate)));
    
    clip.drySignalBuffer_.setSize(numChannels, newLen, false, true, true);
    
    for (int ch = 0; ch < numChannels; ++ch) {
        auto resampled = resamplingManager_->upsampleForHost(
            clip.audioBuffer.getReadPointer(ch),
            srcSamples,
            static_cast<int>(kStoredAudioSampleRate),
            static_cast<int>(deviceSampleRate)
        );
        const int toCopy = juce::jmin(newLen, static_cast<int>(resampled.size()));
        clip.drySignalBuffer_.copyFrom(ch, 0, resampled.data(), toCopy);
    }
}

void OpenTuneAudioProcessor::enqueuePartialRender(int trackId,
                                                  int clipIndex,
                                                  double relStartSeconds,
                                                  double relEndSeconds)
{
    AppLogger::log("RenderTrace: enqueuePartialRender called track=" + juce::String(trackId)
        + " clip=" + juce::String(clipIndex)
        + " range=[" + juce::String(relStartSeconds, 3) + "," + juce::String(relEndSeconds, 3) + "]");

    if (trackId < 0 || trackId >= MAX_TRACKS || clipIndex < 0) {
        AppLogger::log("RenderTrace: enqueuePartialRender early-return invalid track/clip");
        return;
    }

    if (relEndSeconds <= relStartSeconds) {
        AppLogger::log("RenderTrace: enqueuePartialRender early-return invalid range");
        return;
    }

    uint64_t clipId = 0;
    std::shared_ptr<RenderCache> renderCache;
    std::vector<double> chunkBoundaries;
    {
        const juce::ScopedReadLock rl(tracksLock_);
        const auto& clips = tracks_[(size_t)trackId].clips;
        if (clipIndex >= static_cast<int>(clips.size())) {
            return;
        }

        const auto& clip = clips[(size_t)clipIndex];
        clipId = clip.clipId;
        renderCache = clip.renderCache;
        
        const double clipDurationSec = static_cast<double>(clip.audioBuffer.getNumSamples()) / SilentGapDetector::kInternalSampleRate;
        const auto& gaps = clip.silentGaps;
        chunkBoundaries.reserve(gaps.size() + 2);
        chunkBoundaries.push_back(0.0);
        for (const auto& gap : gaps) {
            double mid = (gap.startSeconds + gap.endSeconds) / 2.0;
            chunkBoundaries.push_back(mid);
        }
        chunkBoundaries.push_back(clipDurationSec);
        std::sort(chunkBoundaries.begin(), chunkBoundaries.end());
        auto last = std::unique(chunkBoundaries.begin(), chunkBoundaries.end());
        chunkBoundaries.erase(last, chunkBoundaries.end());
    }

    if (!renderCache) {
        return;
    }

    const uint64_t txnId = nextTxnId_.fetch_add(1);
    currentTxn_.reset();
    currentTxn_.id = txnId;
    currentTxn_.trackId = trackId;
    currentTxn_.clipId = clipId;
    
    AppLogger::log("AUTO_TXN_START id=" + juce::String(static_cast<juce::int64>(txnId))
        + " track=" + juce::String(trackId) + " clipId=" + juce::String(static_cast<juce::int64>(clipId)));

    if (chunkBoundaries.size() < 2) {
        ChunkRenderTask chunkTask;
        chunkTask.key.trackId = trackId;
        chunkTask.key.clipId = clipId;
        chunkTask.key.chunkIndex = 0;
        chunkTask.relStartSeconds = relStartSeconds;
        chunkTask.relEndSeconds = relEndSeconds;
        chunkTask.targetRevision = renderCache->bumpChunkRevision(chunkTask.relStartSeconds, chunkTask.relEndSeconds);
        chunkTask.txnId = txnId;

        if (chunkTask.targetRevision != 0 && enqueueChunkTask(chunkTask.key, chunkTask)) {
            currentTxn_.planned.fetch_add(1);
        } else {
            currentTxn_.errors.fetch_add(1);
            AppLogger::log("AUTO_TXN_FINISH id=" + juce::String(static_cast<juce::int64>(txnId))
                + " chunk=0 outcome=Error converged=" + juce::String(currentTxn_.converged.load())
                + "/" + juce::String(currentTxn_.planned.load())
                + " errors=" + juce::String(currentTxn_.errors.load()));
        }
    } else {
        for (size_t i = 0; i + 1 < chunkBoundaries.size(); ++i) {
            const double chunkStartSec = chunkBoundaries[i];
            const double chunkEndSec = chunkBoundaries[i + 1];

            const double overlapStart = std::max(relStartSeconds, chunkStartSec);
            const double overlapEnd = std::min(relEndSeconds, chunkEndSec);

            if (overlapEnd > overlapStart) {
                ChunkRenderTask chunkTask;
                chunkTask.key.trackId = trackId;
                chunkTask.key.clipId = clipId;
                chunkTask.key.chunkIndex = static_cast<int64_t>(i);
                chunkTask.relStartSeconds = chunkStartSec;
                chunkTask.relEndSeconds = chunkEndSec;
                chunkTask.targetRevision = renderCache->bumpChunkRevision(chunkTask.relStartSeconds, chunkTask.relEndSeconds);
                chunkTask.txnId = txnId;

                if (chunkTask.targetRevision != 0 && enqueueChunkTask(chunkTask.key, chunkTask)) {
                    currentTxn_.planned.fetch_add(1);
                } else {
                    currentTxn_.errors.fetch_add(1);
                    AppLogger::log("AUTO_TXN_FINISH id=" + juce::String(static_cast<juce::int64>(txnId))
                        + " chunk=" + juce::String(static_cast<juce::int64>(i)) + " outcome=Error"
                        + " converged=" + juce::String(currentTxn_.converged.load())
                        + "/" + juce::String(currentTxn_.planned.load())
                        + " errors=" + juce::String(currentTxn_.errors.load()));
                }
            }
        }
    }

    chunkQueueCv_.notify_one();
}

bool OpenTuneAudioProcessor::enqueueChunkTask(const ChunkTaskKey& key, const ChunkRenderTask& task)
{
    std::lock_guard<std::mutex> lock(chunkQueueMutex_);

    auto it = chunkPending_.find(key);
    if (it != chunkPending_.end()) {
        const auto orderIt = std::find(chunkQueueOrder_.begin(), chunkQueueOrder_.end(), key);
        if (orderIt != chunkQueueOrder_.end()) {
            chunkQueueOrder_.erase(orderIt);
        }
        chunkPending_.erase(it);
    }

    if (chunkQueueOrder_.size() >= kMaxChunkQueueSize) {
        AppLogger::log("RenderTrace: enqueueChunkTask queue full, rejecting");
        return false;
    }

    chunkQueueOrder_.push_front(key);
    chunkPending_.emplace(key, task);

    AppLogger::log("RenderTrace: enqueueChunkTask enqueued track=" + juce::String(key.trackId)
        + " clipId=" + juce::String(static_cast<juce::int64>(key.clipId))
        + " chunkIndex=" + juce::String(static_cast<juce::int64>(key.chunkIndex))
        + " queueSize=" + juce::String(static_cast<int>(chunkQueueOrder_.size())));

    return true;
}

void OpenTuneAudioProcessor::finishChunk(const ChunkRenderTask& task, std::shared_ptr<RenderCache> renderCache, bool isError) {
    if (currentTxn_.id != task.txnId) {
        return;
    }

    if (renderCache && renderCache->isRevisionPublished(task.relStartSeconds, task.relEndSeconds, task.targetRevision)) {
        currentTxn_.converged.fetch_add(1);
    }

    if (isError) {
        currentTxn_.errors.fetch_add(1);
    }
}

void OpenTuneAudioProcessor::chunkRenderWorkerLoop()
{
    AppLogger::log("RenderTrace: chunkRenderWorkerLoop started");
    while (true) {
        ChunkRenderTask task;
        ChunkTaskKey key{};
        {
            std::unique_lock<std::mutex> lock(chunkQueueMutex_);
            chunkQueueCv_.wait(lock, [this]() {
                return !chunkRenderWorkerRunning_ || !chunkQueueOrder_.empty();
            });

            if (!chunkRenderWorkerRunning_ && chunkQueueOrder_.empty()) {
                AppLogger::log("RenderTrace: chunkRenderWorkerLoop exiting");
                return;
            }

            key = chunkQueueOrder_.front();
            chunkQueueOrder_.pop_front();
            auto it = chunkPending_.find(key);
            if (it == chunkPending_.end()) {
                continue;
            }

            task = it->second;
            chunkPending_.erase(it);
        }

        const uint64_t txnId = task.txnId;

        AppLogger::log("RenderTrace: chunkRenderWorkerLoop processing task track=" + juce::String(key.trackId)
            + " clipId=" + juce::String(static_cast<juce::int64>(key.clipId))
            + " chunkIndex=" + juce::String(static_cast<juce::int64>(key.chunkIndex))
            + " txnId=" + juce::String(static_cast<juce::int64>(txnId)));

        constexpr double kHopDuration = 512.0 / RenderCache::kSampleRate;
        constexpr double kF0FrameRate = 100.0;
        
        double relChunkStartSec = task.relStartSeconds;
        double relChunkEndSec = task.relEndSeconds;
        double lengthSeconds = relChunkEndSec - relChunkStartSec;
        
        std::shared_ptr<RenderCache> renderCache;
        std::shared_ptr<PitchCurve> pitchCurve;
        std::vector<float> monoAudio;
        int numFrames = 0;
        int64_t targetSamples = 0;
        double frameStartTimeSec = 0.0;
        double frameEndTimeSec = 0.0;
        bool clipFound = false;
    {
        const juce::ScopedReadLock rl(tracksLock_);
        if (task.key.trackId >= 0 && task.key.trackId < MAX_TRACKS) {
            const auto& clips = tracks_[(size_t)task.key.trackId].clips;
            for (const auto& clip : clips) {
                if (clip.clipId == task.key.clipId) {
                    pitchCurve = clip.pitchCurve;
                    renderCache = clip.renderCache;
                    clipFound = true;
                        
                        if (lengthSeconds <= 0.0) {
                            break;
                        }
                        
                        const int audioNumSamples = clip.audioBuffer.getNumSamples();
                        const int audioNumChannels = clip.audioBuffer.getNumChannels();
                        
                        frameStartTimeSec = relChunkStartSec;
                        frameEndTimeSec = relChunkEndSec;
                        
                        const int64_t startSample = TimeCoordinate::secondsToSamples(frameStartTimeSec, RenderCache::kSampleRate);
                        const int64_t endSample = TimeCoordinate::secondsToSamples(frameEndTimeSec, RenderCache::kSampleRate);
                        const int64_t clampedStart = std::max<int64_t>(0, startSample);
                        const int64_t clampedEnd = std::min<int64_t>(audioNumSamples, endSample);
                        const int64_t audioLen = clampedEnd - clampedStart;
                        
                        if (audioLen <= 0 || audioNumChannels <= 0) {
                            break;
                        }

                        numFrames = (audioLen + 512 - 1) / 512;  // ceiling: matches computeLogMelSpectrogram output
                        if (numFrames < 1) {
                            numFrames = 1;
                        }
                        
                        monoAudio.resize(static_cast<size_t>(audioLen));
                        targetSamples = audioLen;
                        for (int64_t i = 0; i < audioLen; ++i) {
                            float sum = 0.0f;
                            for (int ch = 0; ch < audioNumChannels; ++ch) {
                                const float* chData = clip.audioBuffer.getReadPointer(ch);
                                sum += chData[static_cast<int>(clampedStart + i)];
                            }
                            monoAudio[static_cast<size_t>(i)] = sum / static_cast<float>(audioNumChannels);
                        }
                        break;
                    }
                }
            }
        }

        if (!clipFound) {
            AppLogger::log("RenderTrace: chunkRenderWorkerLoop skip - clip not found track="
                + juce::String(task.key.trackId) + " clipId=" + juce::String(static_cast<juce::int64>(task.key.clipId)));
            finishChunk(task, renderCache, true);
            continue;
        }

        if (!pitchCurve || !renderCache || monoAudio.empty() || numFrames <= 0 || targetSamples <= 0) {
            AppLogger::log("RenderTrace: chunkRenderWorkerLoop skip - missing data pitchCurve="
                + juce::String(pitchCurve ? "1" : "0") + " renderCache=" + juce::String(renderCache ? "1" : "0")
                + " monoAudio=" + juce::String(monoAudio.empty() ? "0" : "1")
                + " numFrames=" + juce::String(numFrames)
                + " targetSamples=" + juce::String(static_cast<juce::int64>(targetSamples)));
            finishChunk(task, renderCache, true);
            continue;
        }

        auto snap = pitchCurve->getSnapshot();
        if (!snap->hasRenderableCorrectedF0()) {
            AppLogger::log("RenderTrace: chunkRenderWorkerLoop skip - no renderable corrected F0");
            finishChunk(task, renderCache, true);
            continue;
        }

        const int f0StartFrame = static_cast<int>(std::floor(frameStartTimeSec * kF0FrameRate));
        const int f0EndFrame = static_cast<int>(std::ceil(frameEndTimeSec * kF0FrameRate)) + 1;
        const int numF0Frames = std::max(1, f0EndFrame - f0StartFrame);

        std::vector<float> sourceF0(static_cast<size_t>(numF0Frames), 0.0f);

        snap->renderF0Range(f0StartFrame, f0EndFrame,
            [&sourceF0, f0StartFrame](int frameIndex, const float* data, int length) {
                if (!data || length <= 0) return;
                const int offset = frameIndex - f0StartFrame;
                if (offset < 0) return;
                const int copyLen = std::min(length, static_cast<int>(sourceF0.size()) - offset);
                if (copyLen > 0) {
                    std::copy(data, data + copyLen, sourceF0.begin() + offset);
                }
            });

        bool hasValidF0 = false;
        for (float f : sourceF0) {
            if (f > 0.0f) {
                hasValidF0 = true;
                break;
            }
        }

        if (!hasValidF0) {
            AppLogger::log("RenderTrace: chunkRenderWorkerLoop skip - no valid F0");
            finishChunk(task, renderCache, true);
            continue;
        }

        MelSpectrogramConfig melConfig;
        melConfig.sampleRate = static_cast<int>(RenderCache::kSampleRate);

        auto mel = computeLogMelSpectrogram(monoAudio.data(), static_cast<int>(monoAudio.size()), numFrames, melConfig);
        if (mel.empty()) {
            AppLogger::log("RenderTrace: chunkRenderWorkerLoop skip - mel empty");
            finishChunk(task, renderCache, true);
            continue;
        }

        const int actualFrames = static_cast<int>(mel.size() / 128);

        // F0-to-Mel interpolation: maps from F0 time space (100 Hz) to Mel frame space.
        // melTimeSec = frameStartTimeSec + i * kHopDuration
        // srcPos     = melTimeSec * kF0FrameRate - f0StartFrame
        // numFrames (audioLen/512) is irrelevant here; actualFrames is authoritative.
        std::vector<float> correctedF0(static_cast<size_t>(actualFrames), 0.0f);
        for (int i = 0; i < actualFrames; ++i) {
            const double melTimeSec = frameStartTimeSec + i * kHopDuration;
            const double srcPos = melTimeSec * kF0FrameRate - static_cast<double>(f0StartFrame);
            if (srcPos < 0.0) {
                continue;
            }

            const int srcIdx0 = static_cast<int>(srcPos);
            if (srcIdx0 >= numF0Frames) {
                continue;
            }
            const int srcIdx1 = std::min(srcIdx0 + 1, numF0Frames - 1);
            const double frac = srcPos - static_cast<double>(srcIdx0);

            const float f0_0 = sourceF0[static_cast<size_t>(srcIdx0)];
            const float f0_1 = sourceF0[static_cast<size_t>(srcIdx1)];

            if (f0_0 > 0.0f && f0_1 > 0.0f) {
                correctedF0[static_cast<size_t>(i)] = static_cast<float>(std::exp(std::log(f0_0) * (1.0 - frac) + std::log(f0_1) * frac));
            } else if (f0_0 > 0.0f) {
                correctedF0[static_cast<size_t>(i)] = f0_0;
            } else if (f0_1 > 0.0f) {
                correctedF0[static_cast<size_t>(i)] = f0_1;
            }
        }

        // Fill F0 gaps for vocoder: internal gaps + boundary extension
        fillF0GapsForVocoder(correctedF0, snap, frameStartTimeSec, frameEndTimeSec, kHopDuration, kF0FrameRate);

        auto inputs = std::make_shared<RenderingManager::ChunkInputs>();
        inputs->startSeconds = relChunkStartSec;
        inputs->lengthSeconds = lengthSeconds;
        inputs->endSeconds = relChunkEndSec;
        inputs->targetSamples = targetSamples;
        inputs->numFrames = actualFrames;
        inputs->mel = std::move(mel);
        inputs->energy.resize(correctedF0.size(), 1.0f);
        inputs->targetRevision = task.targetRevision;
        inputs->targetSampleRate = static_cast<int>(currentSampleRate_.load());

        renderingManager_->submit(inputs, std::move(correctedF0), renderCache, 
            [this, task, renderCache, chunkIdx = key.chunkIndex](bool isError) {
                finishChunk(task, renderCache, isError);
                AppLogger::log("AUTO_TXN_FINISH id=" + juce::String(static_cast<juce::int64>(task.txnId))
                    + " chunk=" + juce::String(static_cast<juce::int64>(chunkIdx))
                    + " outcome=" + (isError ? "Error" : "Success")
                    + " converged=" + juce::String(currentTxn_.converged.load())
                    + "/" + juce::String(currentTxn_.planned.load())
                    + " errors=" + juce::String(currentTxn_.errors.load()));
            });
    }
}

void OpenTuneAudioProcessor::clearClipCacheRange(int trackId, int clipIndex, double relStartSeconds, double relEndSeconds) {
    // 清除指定范围的渲染缓存
    // relStartSeconds 和 relEndSeconds 是相对于 clip 起点的时间位置
    if (trackId < 0 || trackId >= MAX_TRACKS) return;
    if (relEndSeconds <= relStartSeconds) return;

    std::shared_ptr<RenderCache> renderCache;
    {
        const juce::ScopedReadLock rl(tracksLock_);
        const auto& clips = tracks_[(size_t)trackId].clips;
        if (clipIndex < 0 || clipIndex >= static_cast<int>(clips.size())) return;

        const auto& clip = clips[(size_t)clipIndex];
        renderCache = clip.renderCache;
    }

    if (!renderCache) return;
    renderCache->clearRange(relStartSeconds, relEndSeconds);
}

void OpenTuneAudioProcessor::performUndo() {
    if (globalUndoManager_.canUndo()) {
        DBG("Global Undo: " + globalUndoManager_.getUndoDescription());
        globalUndoManager_.undo();
    }
}

void OpenTuneAudioProcessor::performRedo() {
    if (globalUndoManager_.canRedo()) {
        DBG("Global Redo: " + globalUndoManager_.getRedoDescription());
        globalUndoManager_.redo();
    }
}

} // namespace OpenTune

// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OpenTune::OpenTuneAudioProcessor();
}
