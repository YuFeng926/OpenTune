#include "PluginProcessor.h"
#include "Editor/EditorFactory.h"
#include "Host/HostIntegration.h"
#include "DSP/ResamplingManager.h"
#include "DSP/MelSpectrogram.h"
#include "Utils/ModelPathResolver.h"
#include "Utils/AppLogger.h"
#include "Utils/GpuDetector.h"
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
    int64_t clipStartInOutput,
    juce::AudioBuffer<float>& out,
    int64_t totalLen)
{
    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;
    
    const int64_t clipLen = clip.audioBuffer->getNumSamples();
    if (clipLen <= 0) return;
    if (clipStartInOutput >= totalLen) return;

    const float clipGain = clip.gain;
    const float baseGain = trackGain * clipGain;
    const int64_t fadeInSamples = (clip.fadeInDuration > 0.0) 
        ? TimeCoordinate::secondsToSamples(clip.fadeInDuration, kExportSr) : 0;
    const int64_t fadeOutSamples = (clip.fadeOutDuration > 0.0) 
        ? TimeCoordinate::secondsToSamples(clip.fadeOutDuration, kExportSr) : 0;

    for (int ch = 0; ch < out.getNumChannels(); ++ch) {
        const float* src = clip.audioBuffer->getReadPointer(std::min(ch, clip.audioBuffer->getNumChannels() - 1));
        float* dst = out.getWritePointer(ch);
        for (int64_t i = 0; i < clipLen; ++i) {
            int64_t dstIndex = clipStartInOutput + i;
            if (dstIndex < 0 || dstIndex >= totalLen) continue;
            float fade = computeFadeGain(i, clipLen, fadeInSamples, fadeOutSamples);
            dst[static_cast<size_t>(dstIndex)] += src[static_cast<size_t>(i)] * baseGain * fade;
        }
    }

    if (!clip.renderCache) return;

    const auto publishedChunks = clip.renderCache->getPublishedChunks();
    if (publishedChunks.empty()) return;

    const double clipDurationSeconds = TimeCoordinate::samplesToSeconds(clipLen, kExportSr);

    for (const auto& chunk : publishedChunks) {
        if (chunk.startSeconds >= clipDurationSeconds) continue;
        if (chunk.endSeconds <= 0.0) continue;
        if (!chunk.audio || chunk.audio->empty()) continue;

        const int64_t chunkStartSample = TimeCoordinate::secondsToSamples(chunk.startSeconds, kExportSr);
        const int64_t chunkEndSample = TimeCoordinate::secondsToSamples(chunk.endSeconds, kExportSr);
        
        const int64_t relStart = std::max<int64_t>(0, chunkStartSample);
        const int64_t relEnd = std::min<int64_t>(clipLen, chunkEndSample);
        if (relEnd <= relStart) continue;

        const int num = static_cast<int>(relEnd - relStart);
        const int64_t audioOffset = relStart - chunkStartSample;

        for (int ch = 0; ch < out.getNumChannels(); ++ch) {
            float* dst = out.getWritePointer(ch);
            const float* src = clip.audioBuffer->getReadPointer(std::min(ch, clip.audioBuffer->getNumChannels() - 1));
            
            for (int i = 0; i < num; ++i) {
                const int64_t clipIndex = relStart + i;
                const int64_t dstIndex = clipStartInOutput + clipIndex;
                if (dstIndex < 0 || dstIndex >= totalLen) continue;

                const size_t audioIdx = static_cast<size_t>(audioOffset + i);
                if (audioIdx >= chunk.audio->size()) break;

                float fade = computeFadeGain(clipIndex, clipLen, fadeInSamples, fadeOutSamples);
                float dryVal = src[static_cast<size_t>(clipIndex)] * baseGain * fade;
                float renderedVal = (*chunk.audio)[audioIdx] * baseGain * fade;
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

    if (!clip.audioBuffer) return;
    const int64_t clipLen = static_cast<int64_t>(clip.audioBuffer->getNumSamples());
    if (clipLen <= 0) {
        return;
    }

    // 使用秒数坐标，与设备采样率无关
    clip.silentGaps = SilentGapDetector::detectAllGapsAdaptive(*clip.audioBuffer);
}

static std::vector<double> buildChunkBoundariesFromSilentGaps(int64_t clipNumSamples,
                                                               const std::vector<SilentGap>& gaps)
{
    std::vector<double> boundaries;
    if (clipNumSamples <= 0) {
        return boundaries;
    }

    const double clipDurationSec = TimeCoordinate::samplesToSeconds(clipNumSamples, SilentGapDetector::kInternalSampleRate);
    boundaries.reserve(gaps.size() + 2);
    boundaries.push_back(0.0);
    for (const auto& gap : gaps) {
        boundaries.push_back((gap.startSeconds + gap.endSeconds) * 0.5);
    }
    boundaries.push_back(clipDurationSec);

    std::sort(boundaries.begin(), boundaries.end());
    auto last = std::unique(boundaries.begin(), boundaries.end());
    boundaries.erase(last, boundaries.end());
    return boundaries;
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

void OpenTuneAudioProcessor::copyClipToSnapshot(const TrackState::AudioClip& clip, OpenTune::ClipSnapshot& out)
{
    out.audioBuffer = clip.audioBuffer;
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
    out.silentGaps = clip.silentGaps;
}

void OpenTuneAudioProcessor::copySnapshotToClip(const OpenTune::ClipSnapshot& snap, TrackState::AudioClip& clip, uint64_t clipId)
{
    clip.clipId = clipId;
    clip.audioBuffer = snap.audioBuffer;
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
    clip.silentGaps = snap.silentGaps;
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
    GpuDetector::getInstance().detect();

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
    f0Service_ = std::make_unique<F0InferenceService>();
    vocoderDomain_ = std::make_unique<VocoderDomain>();
    resetPerfProbeCounters();

    chunkRenderWorkerRunning_ = true;
    chunkRenderWorkerThread_ = std::thread([this]() { chunkRenderWorkerLoop(); });
}

OpenTuneAudioProcessor::~OpenTuneAudioProcessor() {
    isPlaying_.store(false);

    {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        chunkRenderWorkerRunning_ = false;
    }
    schedulerCv_.notify_all();
    if (chunkRenderWorkerThread_.joinable()) {
        chunkRenderWorkerThread_.join();
    }

    if (vocoderDomain_) {
        vocoderDomain_->shutdown();
    }
    if (f0Service_) {
        f0Service_->shutdown();
    }

    AppLogger::shutdown();
}

bool OpenTuneAudioProcessor::ensureF0Ready()
{
    if (f0Ready_.load()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(f0InitMutex_);

    if (f0Ready_.load()) {
        return true;
    }

    if (f0InitAttempted_.load()) {
        return f0Ready_.load();
    }

    f0InitAttempted_.store(true);

    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    AppLogger::log("F0 models dir: " + juce::String(modelsDir));

    if (!ModelPathResolver::ensureOnnxRuntimeLoaded()) {
        AppLogger::log("ensureOnnxRuntimeLoaded failed");
        f0Ready_.store(false);
        return false;
    }

    bool ok = false;
    try {
        ok = f0Service_->initialize(modelsDir);
        if (!ok) {
            AppLogger::log("F0InferenceService initialize failed");
            f0Ready_.store(false);
            return false;
        }

        AppLogger::log("F0 inference service initialized successfully");
    } catch (const std::exception& e) {
        AppLogger::log("F0 initialize exception: " + juce::String(e.what()));
        ok = false;
    } catch (...) {
        AppLogger::log("F0 initialize unknown exception");
        ok = false;
    }

    f0Ready_.store(ok);
    return ok;
}

bool OpenTuneAudioProcessor::ensureVocoderReady()
{
    if (vocoderReady_.load()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(vocoderInitMutex_);

    if (vocoderReady_.load()) {
        return true;
    }

    if (vocoderInitAttempted_.load()) {
        return vocoderReady_.load();
    }

    vocoderInitAttempted_.store(true);

    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    AppLogger::log("Vocoder models dir: " + juce::String(modelsDir));

    if (!ModelPathResolver::ensureOnnxRuntimeLoaded()) {
        AppLogger::log("ensureOnnxRuntimeLoaded failed");
        vocoderReady_.store(false);
        return false;
    }

    bool ok = false;
    try {
        ok = vocoderDomain_->initialize(modelsDir);
        if (!ok) {
            AppLogger::log("VocoderDomain initialize failed");
            vocoderReady_.store(false);
            return false;
        }

        AppLogger::log("Vocoder inference service initialized successfully");
    } catch (const std::exception& e) {
        AppLogger::log("Vocoder initialize exception: " + juce::String(e.what()));
        ok = false;
    } catch (...) {
        AppLogger::log("Vocoder initialize unknown exception");
        ok = false;
    }

    vocoderReady_.store(ok);
    return ok;
}

bool OpenTuneAudioProcessor::initializeInferenceIfNeeded()
{
    return ensureF0Ready();
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

    const double deviceSampleRate = currentSampleRate_.load();
    const double blockDurationSeconds = static_cast<double>(numSamples) / deviceSampleRate;
    const double currentPosSeconds = positionAtomic_->load(std::memory_order_relaxed);
    const double blockEndSeconds = currentPosSeconds + blockDurationSeconds;
    const int64_t blockStartSample = TimeCoordinate::secondsToSamples(currentPosSeconds, deviceSampleRate);
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

            const int64_t clipStartSample = TimeCoordinate::secondsToSamples(clip.startSeconds, deviceSampleRate);
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
            const double clipDurationSeconds = TimeCoordinate::samplesToSeconds(dryLenSamples, deviceSampleRate);
            const double readStartSeconds = TimeCoordinate::samplesToSeconds(readStartSample, deviceSampleRate);
            const double readEndSeconds = TimeCoordinate::samplesToSeconds(readEndSample, deviceSampleRate);
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
                const int targetSampleRateInt = static_cast<int>(deviceSampleRate);
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
                const double dt = 1.0 / deviceSampleRate;
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
        const juce::ScopedReadLock rl(tracksLock_);
        int totalPending = 0;
        for (int t = 0; t < MAX_TRACKS; ++t) {
            for (const auto& clip : tracks_[t].clips) {
                if (clip.renderCache) {
                    totalPending += clip.renderCache->getPendingCount();
                }
            }
        }
        snapshot.renderQueueDepth = totalPending;
    }

    return snapshot;
}

RenderCache::ChunkStats OpenTuneAudioProcessor::getClipChunkStats(int trackId, int clipIndex) const
{
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        return {};
    }

    const juce::ScopedReadLock rl(tracksLock_);
    const auto& clips = tracks_[trackId].clips;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips.size())) {
        return {};
    }

    const auto& clip = clips[clipIndex];
    if (!clip.renderCache) {
        return {};
    }

    return clip.renderCache->getChunkStats();
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

std::shared_ptr<const juce::AudioBuffer<float>> OpenTuneAudioProcessor::getClipAudioBuffer(int trackId, int clipIndex) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].audioBuffer;
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
    int64_t totalSamples = originalClip.audioBuffer->getNumSamples();

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

    // Copy audio for new clip: 修改 = 生成新对象并替换引用
    int64_t newLength = totalSamples - splitPointInClip;
    const int numCh = originalClip.audioBuffer->getNumChannels();
    auto newRightBuffer = std::make_shared<juce::AudioBuffer<float>>(numCh, static_cast<int>(newLength));
    for (int ch = 0; ch < numCh; ++ch) {
        newRightBuffer->copyFrom(ch, 0, *originalClip.audioBuffer, ch, static_cast<int>(splitPointInClip), static_cast<int>(newLength));
    }
    newClip.audioBuffer = newRightBuffer;

    // Update original clip (Left part): 生成新 buffer 并替换引用，不原地修改
    auto newLeftBuffer = std::make_shared<juce::AudioBuffer<float>>(numCh, static_cast<int>(splitPointInClip));
    for (int ch = 0; ch < numCh; ++ch) {
        newLeftBuffer->copyFrom(ch, 0, *originalClip.audioBuffer, ch, 0, static_cast<int>(splitPointInClip));
    }
    originalClip.audioBuffer = newLeftBuffer;
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
    
    // 合并音频缓冲区: 修改 = 生成新对象并替换引用
    int originalSamples = originalClip.audioBuffer->getNumSamples();
    int newSamples = newClip.audioBuffer->getNumSamples();
    int channels = std::max(originalClip.audioBuffer->getNumChannels(), newClip.audioBuffer->getNumChannels());
    
    auto mergedBuffer = std::make_shared<juce::AudioBuffer<float>>(channels, originalSamples + newSamples);
    // Copy original clip audio
    for (int ch = 0; ch < originalClip.audioBuffer->getNumChannels(); ++ch) {
        mergedBuffer->copyFrom(ch, 0, *originalClip.audioBuffer, ch, 0, originalSamples);
    }
    // Append new clip audio
    for (int ch = 0; ch < newClip.audioBuffer->getNumChannels(); ++ch) {
        mergedBuffer->copyFrom(ch, originalSamples, *newClip.audioBuffer, ch, 0, newSamples);
    }
    originalClip.audioBuffer = mergedBuffer;

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

    copyClipToSnapshot(*it, out);
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
        copyClipToSnapshot(*it, *deletedOut);
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
    
    if (!snap.audioBuffer || snap.audioBuffer->getNumSamples() <= 0) {
        AppLogger::log("InsertClip rejected: empty audio buffer (zero samples) for clipId=" + 
                       (forcedClipId != 0 ? juce::String(static_cast<juce::int64>(forcedClipId)) : "new"));
        return false;
    }
    if (snap.audioBuffer->getNumChannels() <= 0) {
        AppLogger::log("InsertClip rejected: invalid channel count=" + juce::String(snap.audioBuffer->getNumChannels()));
        return false;
    }
    
    // UI线程修改 tracks_ 数据时使用写锁
    const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
    
    auto& clips = tracks_[trackId].clips;
    insertIndex = juce::jlimit(0, (int)clips.size(), insertIndex);

    TrackState::AudioClip clip;
    const uint64_t clipId = (forcedClipId != 0) ? forcedClipId : nextClipId_.fetch_add(1);
    copySnapshotToClip(snap, clip, clipId);

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

    const juce::ScopedReadLock tracksReadLock(tracksLock_);
    
    auto& track = tracks_[static_cast<size_t>(trackId)];
    if (clipIndex < 0 || clipIndex >= static_cast<int>(track.clips.size())) {
        lastExportError_ = "无效的Clip索引: " + juce::String(clipIndex);
        return false;
    }
    
    auto& clip = track.clips[static_cast<size_t>(clipIndex)];
    const int64_t clipLen = clip.audioBuffer->getNumSamples();
    if (clipLen <= 0) {
        lastExportError_ = "Clip音频长度为零";
        return false;
    }
    
    juce::AudioBuffer<float> out(kExportNumChannels, static_cast<int>(clipLen));
    out.clear();
    
    renderClipForExport(clip, track.volume, 0, out, clipLen);
    
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
        .withNumChannels(out.getNumChannels())
        .withBitsPerSample(kExportBitsPerSample);
    
    auto writer = wav.createWriterFor(outStream, options);
    if (!writer) {
        lastExportError_ = "无法创建WAV写入器";
        return false;
    }
    
    return writer->writeFromAudioSampleBuffer(out, 0, out.getNumSamples());
}

bool OpenTuneAudioProcessor::exportTrackAudio(int trackId, const juce::File& file) {
    lastExportError_.clear();
    
    if (trackId < 0 || trackId >= MAX_TRACKS) {
        lastExportError_ = "无效的轨道ID: " + juce::String(trackId);
        return false;
    }

    const juce::ScopedReadLock tracksReadLock(tracksLock_);
    
    auto& track = tracks_[static_cast<size_t>(trackId)];
    if (track.clips.empty()) {
        lastExportError_ = "轨道 " + juce::String(trackId + 1) + " 没有音频片段";
        return false;
    }

    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;
    int64_t totalLen = 0;
    for (const auto& clip : track.clips) {
        int64_t clipStart = TimeCoordinate::secondsToSamples(clip.startSeconds, kExportSr);
        int64_t clipEnd = clipStart + clip.audioBuffer->getNumSamples();
        totalLen = std::max(totalLen, clipEnd);
    }
    if (totalLen <= 0) {
        lastExportError_ = "音频总长度为零或无效";
        return false;
    }

    juce::AudioBuffer<float> out(kExportNumChannels, static_cast<int>(totalLen));
    out.clear();

    for (const auto& clip : track.clips) {
        const int64_t clipStart = TimeCoordinate::secondsToSamples(clip.startSeconds, kExportSr);
        renderClipForExport(clip, track.volume, clipStart, out, totalLen);
    }

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
        .withNumChannels(out.getNumChannels())
        .withBitsPerSample(kExportBitsPerSample);

    auto writer = wav.createWriterFor(outStream, options);
    if (!writer) return false;

    return writer->writeFromAudioSampleBuffer(out, 0, out.getNumSamples());
}

bool OpenTuneAudioProcessor::exportMasterMixAudio(const juce::File& file) {
    const juce::ScopedReadLock tracksReadLock(tracksLock_);

    constexpr double kExportSr = TimeCoordinate::kRenderSampleRate;
    int64_t totalLen = 0;
    for (int trackId = 0; trackId < static_cast<int>(tracks_.size()); ++trackId) {
        const auto& track = tracks_[static_cast<size_t>(trackId)];
        for (const auto& clip : track.clips) {
            int64_t clipStart = TimeCoordinate::secondsToSamples(clip.startSeconds, kExportSr);
            int64_t clipEnd = clipStart + clip.audioBuffer->getNumSamples();
            totalLen = std::max(totalLen, clipEnd);
        }
    }
    if (totalLen <= 0) return false;

    juce::AudioBuffer<float> mix(kExportMasterNumChannels, static_cast<int>(totalLen));
    mix.clear();

    bool anySolo = false;
    for (const auto& t : tracks_) {
        if (t.isSolo) {
            anySolo = true;
            break;
        }
    }

    for (int trackId = 0; trackId < static_cast<int>(tracks_.size()); ++trackId) {
        auto& track = tracks_[static_cast<size_t>(trackId)];
        
        if (anySolo) {
            if (!track.isSolo) continue;
        } else {
            if (track.isMuted) continue;
        }

        if (track.clips.empty()) continue;

        for (const auto& clip : track.clips) {
            const int64_t clipStart = TimeCoordinate::secondsToSamples(clip.startSeconds, kExportSr);
            renderClipForExport(clip, track.volume, clipStart, mix, totalLen);
        }
    }

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
        .withNumChannels(mix.getNumChannels())
        .withBitsPerSample(kExportBitsPerSample);

    auto writer = wav.createWriterFor(outStream, options);
    if (!writer) return false;

    return writer->writeFromAudioSampleBuffer(mix, 0, mix.getNumSamples());
}

void OpenTuneAudioProcessor::setPlaying(bool playing) {
    if (playing) {
        playStartPosition_.store(positionAtomic_->load(std::memory_order_relaxed));
        isFadingOut_.store(false);
        isPlaying_.store(true);
        useDrySignalFallback_.store(false);
        isBuffering_.store(false);
        AppLogger::log("Playback: start");
    } else {
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
    clip.audioBuffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(prepared.hostRateBuffer));  // Fixed 44100Hz
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
        if (it->audioBuffer->getNumSamples() <= 0 || it->audioBuffer->getNumChannels() <= 0) {
            return false;
        }
        clipAudio.makeCopyOf(*it->audioBuffer);
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

std::vector<Note>& OpenTuneAudioProcessor::getClipNotesRef(int trackId, int clipIndex) {
    static std::vector<Note> empty;
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        auto& clips = tracks_[trackId].clips;
        if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
            return clips[clipIndex].notes;
        }
    }
    return empty;
}

int OpenTuneAudioProcessor::getClipIndexById(int trackId, uint64_t clipId) const {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedReadLock tracksReadLock(tracksLock_);
        const auto& clips = tracks_[trackId].clips;
        for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
            if (clips[i].clipId == clipId) {
                return i;
            }
        }
    }
    return -1;
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

bool OpenTuneAudioProcessor::setClipNotesById(int trackId, uint64_t clipId, const std::vector<Note>& notes) {
    if (trackId >= 0 && trackId < MAX_TRACKS) {
        const juce::ScopedWriteLock tracksWriteLock(tracksLock_);
        auto& clips = tracks_[trackId].clips;
        for (auto& clip : clips) {
            if (clip.clipId == clipId) {
                clip.notes = notes;
                return true;
            }
        }
    }
    return false;
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
    
    if (clip.audioBuffer->getNumSamples() <= 0) {
        clip.drySignalBuffer_.setSize(0, 0);
        return;
    }
    
    // If device rate matches stored rate, just copy
        if (std::abs(kStoredAudioSampleRate - deviceSampleRate) < 1.0) {
        clip.drySignalBuffer_.makeCopyOf(*clip.audioBuffer);
        return;
    }
    
    // Resample from 44100Hz to device rate
    const int numChannels = clip.audioBuffer->getNumChannels();
    const int srcSamples = clip.audioBuffer->getNumSamples();
    const double sourceDurationSeconds = TimeCoordinate::samplesToSeconds(srcSamples, kStoredAudioSampleRate);
    const int newLen = juce::jmax(
        1,
        static_cast<int>(TimeCoordinate::secondsToSamples(sourceDurationSeconds, deviceSampleRate)));
    
    clip.drySignalBuffer_.setSize(numChannels, newLen, false, true, true);
    
    for (int ch = 0; ch < numChannels; ++ch) {
        auto resampled = resamplingManager_->upsampleForHost(
            clip.audioBuffer->getReadPointer(ch),
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

    std::shared_ptr<RenderCache> renderCache;
    std::vector<double> chunkBoundaries;
    {
        const juce::ScopedReadLock rl(tracksLock_);
        const auto& clips = tracks_[(size_t)trackId].clips;
        if (clipIndex >= static_cast<int>(clips.size())) {
            return;
        }

        const auto& clip = clips[(size_t)clipIndex];
        renderCache = clip.renderCache;

        chunkBoundaries = buildChunkBoundariesFromSilentGaps(
            clip.audioBuffer->getNumSamples(),
            clip.silentGaps);
    }

    if (!renderCache) {
        return;
    }

    // 对每个受影响的 Chunk 请求渲染
    // - 若 Chunk 处于 Idle：状态变为 Pending，加入 pendingChunks_ 集合
    // - 若 Chunk 处于 Pending/Running：只更新 desiredRevision（版本去重）
    int requestedCount = 0;
    if (chunkBoundaries.size() < 2) {
        renderCache->requestRenderPending(relStartSeconds, relEndSeconds);
        requestedCount = 1;
    } else {
        for (size_t i = 0; i + 1 < chunkBoundaries.size(); ++i) {
            const double chunkStartSec = chunkBoundaries[i];
            const double chunkEndSec = chunkBoundaries[i + 1];

            const double overlapStart = std::max(relStartSeconds, chunkStartSec);
            const double overlapEnd = std::min(relEndSeconds, chunkEndSec);

            if (overlapEnd > overlapStart) {
                renderCache->requestRenderPending(chunkStartSec, chunkEndSec);
                ++requestedCount;
            }
        }
    }

    AppLogger::log("RenderTrace: enqueuePartialRender requested=" + juce::String(requestedCount)
        + " pendingTotal=" + juce::String(renderCache->getPendingCount()));

    // 唤醒 Worker
    schedulerCv_.notify_one();
}

bool OpenTuneAudioProcessor::hasPendingRenderJobs() const
{
    const juce::ScopedReadLock rl(tracksLock_);
    for (int t = 0; t < MAX_TRACKS; ++t) {
        for (const auto& clip : tracks_[t].clips) {
            if (clip.renderCache && clip.renderCache->getPendingCount() > 0) {
                return true;
            }
        }
    }
    return false;
}

void OpenTuneAudioProcessor::chunkRenderWorkerLoop()
{
    AppLogger::log("RenderTrace: chunkRenderWorkerLoop started");

    constexpr double kHopDuration = 512.0 / RenderCache::kSampleRate;

    struct WorkerRenderJob {
        std::shared_ptr<RenderCache> cache;
        int trackId{0};
        uint64_t clipId{0};
        double startSeconds{0.0};
        double endSeconds{0.0};
        uint64_t targetRevision{0};
    };

    while (true) {
        // 1. 找 Pending Chunk
        std::shared_ptr<WorkerRenderJob> job;

        {
            std::unique_lock<std::mutex> lock(schedulerMutex_);
            schedulerCv_.wait(lock, [this]() {
                return !chunkRenderWorkerRunning_.load(std::memory_order_acquire)
                    || hasPendingRenderJobs();
            });

            if (!chunkRenderWorkerRunning_) {
                AppLogger::log("RenderTrace: chunkRenderWorkerLoop exiting");
                return;
            }

            // 遍历所有 Track/Clip，找 Pending Chunk
            const juce::ScopedReadLock rl(tracksLock_);
            for (int t = 0; t < MAX_TRACKS; ++t) {
                for (auto& clip : tracks_[t].clips) {
                    if (!clip.renderCache) continue;

                    RenderCache::PendingJob pendingJob;
                    if (clip.renderCache->getNextPendingJob(pendingJob)) {
                        job = std::make_shared<WorkerRenderJob>();
                        job->trackId = t;
                        job->clipId = clip.clipId;
                        job->startSeconds = pendingJob.startSeconds;
                        job->endSeconds = pendingJob.endSeconds;
                        job->targetRevision = pendingJob.targetRevision;
                        job->cache = clip.renderCache;

                        AppLogger::log("RenderTrace: chunkRenderWorkerLoop pulled track=" + juce::String(t)
                            + " clipId=" + juce::String(static_cast<juce::int64>(clip.clipId))
                            + " start=" + juce::String(pendingJob.startSeconds, 3)
                            + " revision=" + juce::String(static_cast<juce::int64>(pendingJob.targetRevision)));
                        break;
                    }
                }
                if (job) break;
            }

        }

        if (!job) {
            // 无 Pending 任务，等待唤醒
            continue;
        }

        const double relChunkStartSec = job->startSeconds;
        const double relChunkEndSec = job->endSeconds;
        const double lengthSeconds = relChunkEndSec - relChunkStartSec;

        // 2. 准备渲染数据（读取 Clip 中的音频和 PitchCurve）
        std::shared_ptr<PitchCurve> pitchCurve;
        std::vector<float> monoAudio;
        int numFrames = 0;
        int64_t targetSamples = 0;
        bool clipFound = false;

        {
            const juce::ScopedReadLock rl(tracksLock_);
            if (job->trackId >= 0 && job->trackId < MAX_TRACKS) {
                const auto& clips = tracks_[(size_t)job->trackId].clips;
                for (const auto& clip : clips) {
                    if (clip.clipId == job->clipId) {
                        pitchCurve = clip.pitchCurve;

                        if (lengthSeconds <= 0.0) {
                            break;
                        }

                        const int audioNumSamples = clip.audioBuffer->getNumSamples();
                        const int audioNumChannels = clip.audioBuffer->getNumChannels();

                        const int64_t startSample = TimeCoordinate::secondsToSamples(relChunkStartSec, RenderCache::kSampleRate);
                        const int64_t endSample = TimeCoordinate::secondsToSamples(relChunkEndSec, RenderCache::kSampleRate);
                        const int64_t clampedStart = std::max<int64_t>(0, startSample);
                        const int64_t clampedEnd = std::min<int64_t>(audioNumSamples, endSample);
                        const int64_t audioLen = clampedEnd - clampedStart;

                        if (audioLen > 0 && audioNumChannels > 0) {
                            numFrames = static_cast<int>((audioLen + 512 - 1) / 512);
                            if (numFrames < 1) numFrames = 1;

                            monoAudio.resize(static_cast<size_t>(audioLen));
                            targetSamples = audioLen;
                            for (int64_t i = 0; i < audioLen; ++i) {
                                float sum = 0.0f;
                                for (int ch = 0; ch < audioNumChannels; ++ch) {
                                    const float* chData = clip.audioBuffer->getReadPointer(ch);
                                    sum += chData[static_cast<int>(clampedStart + i)];
                                }
                                monoAudio[static_cast<size_t>(i)] = sum / static_cast<float>(audioNumChannels);
                            }
                            clipFound = true;
                        }
                        break;
                    }
                }
            }
        }

        if (!clipFound || !pitchCurve || monoAudio.empty() || numFrames <= 0 || targetSamples <= 0) {
            AppLogger::log("RenderTrace: TerminalFailure: missing_audio_data"
                " clipFound=" + juce::String(clipFound ? 1 : 0)
                + " pitchCurve=" + juce::String(pitchCurve ? 1 : 0)
                + " monoAudio.empty=" + juce::String(monoAudio.empty() ? 1 : 0)
                + " numFrames=" + juce::String(numFrames)
                + " targetSamples=" + juce::String(static_cast<juce::int64>(targetSamples))
                + " start=" + juce::String(relChunkStartSec, 3)
                + " revision=" + juce::String(static_cast<juce::int64>(job->targetRevision)));

            job->cache->completeChunkRender(relChunkStartSec, job->targetRevision,
                RenderCache::CompletionResult::TerminalFailure);
            schedulerCv_.notify_one();
            continue;
        }

        auto snap = pitchCurve->getSnapshot();
        if (!snap->hasRenderableCorrectedF0()) {
            AppLogger::log("RenderTrace: Blank: no_renderable_corrected_F0"
                " start=" + juce::String(relChunkStartSec, 3)
                + " revision=" + juce::String(static_cast<juce::int64>(job->targetRevision)));
            job->cache->markChunkAsBlank(relChunkStartSec);
            schedulerCv_.notify_one();
            continue;
        }

        const int f0HopSize = snap->getHopSize();
        const double f0SampleRate = snap->getSampleRate();
        if (f0HopSize <= 0 || f0SampleRate <= 0.0) {
            AppLogger::log("RenderTrace: TerminalFailure: invalid_f0_timebase"
                " hop=" + juce::String(f0HopSize)
                + " sampleRate=" + juce::String(f0SampleRate, 3)
                + " start=" + juce::String(relChunkStartSec, 3)
                + " revision=" + juce::String(static_cast<juce::int64>(job->targetRevision)));
            job->cache->completeChunkRender(relChunkStartSec, job->targetRevision,
                RenderCache::CompletionResult::TerminalFailure);
            schedulerCv_.notify_one();
            continue;
        }

        const double f0FrameRate = f0SampleRate / static_cast<double>(f0HopSize);

        // 3. 构造 F0 数据
        const int f0StartFrame = static_cast<int>(std::floor(relChunkStartSec * f0FrameRate));
        const int f0EndFrame = static_cast<int>(std::ceil(relChunkEndSec * f0FrameRate)) + 1;
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
            AppLogger::log("RenderTrace: Blank: no_valid_F0"
                " start=" + juce::String(relChunkStartSec, 3)
                + " revision=" + juce::String(static_cast<juce::int64>(job->targetRevision)));
            job->cache->markChunkAsBlank(relChunkStartSec);
            schedulerCv_.notify_one();
            continue;
        }

        if (!ensureVocoderReady()) {
            AppLogger::log("RenderTrace: TerminalFailure: vocoder_not_ready"
                " start=" + juce::String(relChunkStartSec, 3)
                + " revision=" + juce::String(static_cast<juce::int64>(job->targetRevision)));
            job->cache->completeChunkRender(relChunkStartSec, job->targetRevision,
                RenderCache::CompletionResult::TerminalFailure);
            schedulerCv_.notify_one();
            continue;
        }

        // 4. 构造 Mel Spectrogram
        MelSpectrogramConfig melConfig;
        melConfig.sampleRate = static_cast<int>(RenderCache::kSampleRate);
        melConfig.nMels = vocoderDomain_->getMelBins();

        auto melResult = computeLogMelSpectrogram(monoAudio.data(), static_cast<int>(monoAudio.size()), numFrames, melConfig);
        if (!melResult.ok() || melResult.value().empty()) {
            AppLogger::log("RenderTrace: TerminalFailure: mel_computation_failed"
                " mel.ok=" + juce::String(melResult.ok() ? 1 : 0)
                + " mel.empty=" + juce::String(!melResult.ok() ? -1 : (melResult.value().empty() ? 1 : 0))
                + " start=" + juce::String(relChunkStartSec, 3)
                + " revision=" + juce::String(static_cast<juce::int64>(job->targetRevision)));
            job->cache->completeChunkRender(relChunkStartSec, job->targetRevision,
                RenderCache::CompletionResult::TerminalFailure);
            schedulerCv_.notify_one();
            continue;
        }

        auto mel = std::move(melResult).value();
        const int actualFrames = static_cast<int>(mel.size() / melConfig.nMels);

        // 5. F0-to-Mel 插值
        std::vector<float> correctedF0(static_cast<size_t>(actualFrames), 0.0f);
        for (int i = 0; i < actualFrames; ++i) {
            const double melTimeSec = relChunkStartSec + i * kHopDuration;
            const double srcPos = melTimeSec * f0FrameRate - static_cast<double>(f0StartFrame);
            if (srcPos < 0.0) continue;

            const int srcIdx0 = static_cast<int>(srcPos);
            if (srcIdx0 >= numF0Frames) continue;
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

        fillF0GapsForVocoder(correctedF0, snap, relChunkStartSec, relChunkEndSec, kHopDuration, f0FrameRate);

        // 6. 提交执行
        VocoderDomain::Job vocoderJob;
        vocoderJob.f0 = std::move(correctedF0);
        vocoderJob.energy.resize(vocoderJob.f0.size(), 1.0f);
        vocoderJob.mel = std::move(mel);

        auto renderCache = job->cache;
        auto targetRevision = job->targetRevision;
        double jobStartSeconds = job->startSeconds;

        vocoderJob.onComplete = [this, renderCache, targetRevision, jobStartSeconds](bool success, const juce::String& error, const std::vector<float>& audio) {
            if (success && !audio.empty()) {
                const double chunkEndSeconds = jobStartSeconds + static_cast<double>(audio.size()) / RenderCache::kSampleRate;
                std::vector<float> renderedAudio(audio.begin(), audio.end());
                const bool baseChunkStored = renderCache->addChunk(
                    jobStartSeconds,
                    chunkEndSeconds,
                    std::move(renderedAudio),
                    targetRevision);

                const int renderSampleRate = static_cast<int>(RenderCache::kSampleRate);
                const int deviceSampleRate = static_cast<int>(std::lround(currentSampleRate_.load(std::memory_order_relaxed)));
                if (baseChunkStored && resamplingManager_ && deviceSampleRate > 0 && deviceSampleRate != renderSampleRate) {
                    auto resampledAudio = resamplingManager_->upsampleForHost(
                        audio.data(),
                        audio.size(),
                        renderSampleRate,
                        deviceSampleRate);
                    if (!resampledAudio.empty()) {
                        const bool resampledStored = renderCache->addResampledChunk(
                            jobStartSeconds,
                            chunkEndSeconds,
                            deviceSampleRate,
                            std::move(resampledAudio),
                            targetRevision);
                        if (!resampledStored) {
                            AppLogger::log("RenderTrace: addResampledChunk skipped start="
                                + juce::String(jobStartSeconds, 3)
                                + " revision=" + juce::String(static_cast<juce::int64>(targetRevision))
                                + " sampleRate=" + juce::String(deviceSampleRate));
                        }
                    }
                }

                renderCache->completeChunkRender(jobStartSeconds, targetRevision, RenderCache::CompletionResult::Succeeded);
                AppLogger::log("RenderTrace: chunkRenderWorkerLoop complete start="
                    + juce::String(jobStartSeconds, 3)
                    + " revision=" + juce::String(static_cast<juce::int64>(targetRevision)));
            } else {
                renderCache->completeChunkRender(jobStartSeconds, targetRevision, RenderCache::CompletionResult::TerminalFailure);
                AppLogger::log("RenderTrace: chunkRenderWorkerLoop failed start="
                    + juce::String(jobStartSeconds, 3)
                    + " error=" + error);
            }
            schedulerCv_.notify_one();
        };

        vocoderDomain_->submit(std::move(vocoderJob));
        AppLogger::log("RenderTrace: chunkRenderWorkerLoop submitted start=" + juce::String(relChunkStartSec, 3));
    }
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
