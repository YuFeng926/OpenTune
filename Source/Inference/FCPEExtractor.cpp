#include "FCPEExtractor.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/AppLogger.h"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace OpenTune {

FCPEExtractor::FCPEExtractor(
    std::unique_ptr<Ort::Session> session,
    std::shared_ptr<ResamplingManager> resampler)
    : session_(std::move(session))
    , resampler_(resampler)
    , forwardFFT_(static_cast<int>(std::log2(N_FFT)))
{
    memoryInfo_ = std::make_unique<Ort::MemoryInfo>(
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault)
    );
    initMelFilterbank();
    initHannWindow();
    initCentTable();
}

FCPEExtractor::~FCPEExtractor() = default;

// ============================================================
// Initialization — verbatim from PitchNet FCPEPitchDetector
// ============================================================

void FCPEExtractor::initMelFilterbank()
{
    const int numBins = N_FFT / 2 + 1;

    // Slaney mel scale (librosa default htk=False)
    // 线性 <1000Hz，对数 ≥1000Hz
    const float fSp = 200.0f / 3.0f;
    const float minLogMel = 1000.0f / fSp;
    const float logStep = std::log(6.4f) / 27.0f;

    auto hzToMel = [fSp, minLogMel, logStep](float hz) -> float {
        if (hz < 1000.0f)
            return hz / fSp;
        return minLogMel + std::log(hz / 1000.0f) / logStep;
    };
    auto melToHz = [fSp, minLogMel, logStep](float mel) -> float {
        if (mel < minLogMel)
            return fSp * mel;
        return 1000.0f * std::exp(logStep * (mel - minLogMel));
    };

    const float melMin = hzToMel(0.0f);
    const float melMax = hzToMel(8000.0f);

    std::vector<float> melPoints(N_MELS + 2);
    for (int i = 0; i <= N_MELS + 1; ++i)
        melPoints[i] = melMin + (melMax - melMin) * i / (N_MELS + 1);

    std::vector<float> hzPoints(N_MELS + 2);
    for (int i = 0; i <= N_MELS + 1; ++i)
        hzPoints[i] = melToHz(melPoints[i]);

    melFilterbank_.resize(N_MELS);
    for (int m = 0; m < N_MELS; ++m)
    {
        float fLow = hzPoints[m];
        float fCenter = hzPoints[m + 1];
        float fHigh = hzPoints[m + 2];
        float enorm = 2.0f / (fHigh - fLow);

        int firstBin = numBins;
        int lastBin = -1;
        for (int k = 0; k < numBins; ++k)
        {
            float freq = static_cast<float>(k) * SAMPLE_RATE / N_FFT;
            if (freq >= fLow && freq <= fHigh)
            {
                if (k < firstBin) firstBin = k;
                if (k > lastBin) lastBin = k;
            }
        }

        MelBand& band = melFilterbank_[m];
        if (lastBin < firstBin) { band.startBin = 0; band.endBin = 0; continue; }

        band.startBin = firstBin;
        band.endBin = lastBin + 1;
        band.weights.resize(band.endBin - band.startBin, 0.0f);

        for (int k = firstBin; k <= lastBin; ++k)
        {
            float freq = static_cast<float>(k) * SAMPLE_RATE / N_FFT;
            if (freq >= fLow && freq < fCenter)
                band.weights[k - firstBin] = enorm * (freq - fLow) / (fCenter - fLow);
            else if (freq >= fCenter && freq <= fHigh)
                band.weights[k - firstBin] = enorm * (fHigh - freq) / (fHigh - fCenter);
        }
    }
}

void FCPEExtractor::initHannWindow()
{
    hannWindow_.resize(WIN_SIZE);
    for (int i = 0; i < WIN_SIZE; ++i)
        hannWindow_[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / (WIN_SIZE - 1)));
}

void FCPEExtractor::initCentTable()
{
    centTable_.resize(OUT_DIMS);
    float centMin = f0ToCent(F0_MIN_DEFAULT);
    float centMax = f0ToCent(F0_MAX_DEFAULT);
    for (int i = 0; i < OUT_DIMS; ++i)
        centTable_[i] = centMin + (centMax - centMin) * i / (OUT_DIMS - 1);
}

// ============================================================
// Preflight — simplified from RMVPEExtractor (duration gate only)
// ============================================================

FCPEExtractor::PreflightResult FCPEExtractor::preflightCheck(size_t audioLength, int sampleRate) const
{
    PreflightResult result;
    if (sampleRate <= 0 || audioLength == 0) {
        result.errorMessage = "[FCPE] Invalid input";
        return result;
    }
    result.audioDurationSec = static_cast<double>(audioLength) / sampleRate;
    if (result.audioDurationSec > kMaxAudioDurationSec) {
        result.errorMessage = "[FCPE] Duration gate exceeded";
        return result;
    }
    if (!session_) {
        result.errorMessage = "[FCPE] Session not initialized";
        return result;
    }
    result.success = true;
    return result;
}

// ============================================================
// Mel extraction — verbatim from PitchNet extractMel()
// ============================================================

std::vector<std::vector<float>> FCPEExtractor::extractMel(const std::vector<float>& audio)
{
    const int numBins = N_FFT / 2 + 1;
    int padLeft = (WIN_SIZE - HOP) / 2;
    int padRight = std::max((WIN_SIZE - HOP + 1) / 2,
                            WIN_SIZE - static_cast<int>(audio.size()) - padLeft);

    std::vector<float> paddedAudio;

    if (padRight < static_cast<int>(audio.size()))
    {
        paddedAudio.reserve(padLeft + audio.size() + padRight);
        for (int i = padLeft; i > 0; --i)
            paddedAudio.push_back(audio[std::min(i, static_cast<int>(audio.size()) - 1)]);
        paddedAudio.insert(paddedAudio.end(), audio.begin(), audio.end());
        int audioSize = static_cast<int>(audio.size());
        for (int i = 0; i < padRight; ++i)
        {
            int idx = audioSize - 2 - i;
            if (idx < 0) idx = 0;
            paddedAudio.push_back(audio[idx]);
        }
    }
    else
    {
        paddedAudio.resize(padLeft + audio.size() + padRight, 0.0f);
        std::copy(audio.begin(), audio.end(), paddedAudio.begin() + padLeft);
    }

    int numFrames = 1 + (static_cast<int>(paddedAudio.size()) - WIN_SIZE) / HOP;
    if (numFrames < 1) numFrames = 1;

    std::vector<std::vector<float>> mel(numFrames, std::vector<float>(N_MELS, 0.0f));

    std::vector<float> fftBuffer(N_FFT * 2, 0.0f);
    std::vector<float> mag(numBins, 0.0f);

    for (int frame = 0; frame < numFrames; ++frame)
    {
        int start = frame * HOP;

        std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
        for (int i = 0; i < WIN_SIZE && start + i < static_cast<int>(paddedAudio.size()); ++i)
            fftBuffer[i] = paddedAudio[start + i] * hannWindow_[i];

        forwardFFT_.performRealOnlyForwardTransform(fftBuffer.data());

        for (int k = 0; k < numBins; ++k)
        {
            float real = fftBuffer[k * 2];
            float imag = fftBuffer[k * 2 + 1];
            mag[k] = std::sqrt(real * real + imag * imag + 1e-9f);
        }

        for (int m = 0; m < N_MELS; ++m)
        {
            const auto& band = melFilterbank_[m];
            float sum = 0.0f;
            for (int k = band.startBin; k < band.endBin; ++k)
                sum += mag[k] * band.weights[k - band.startBin];
            mel[frame][m] = std::log(std::max(sum, CLIP_VAL));
        }
    }

    return mel;
}

// ============================================================
// Decode — verbatim from PitchNet decodeF0()
// ============================================================

std::vector<float> FCPEExtractor::decodeF0(const float* latent, int numFrames, float threshold)
{
    std::vector<float> f0(numFrames, 0.0f);
    for (int t = 0; t < numFrames; ++t)
    {
        const float* frame = latent + static_cast<size_t>(t) * OUT_DIMS;
        int maxIdx = 0;
        float maxVal = frame[0];
        for (int i = 1; i < OUT_DIMS; ++i)
        {
            if (frame[i] > maxVal) { maxVal = frame[i]; maxIdx = i; }
        }
        if (maxVal <= threshold) { f0[t] = 0.0f; continue; }

        int localStart = std::max(0, maxIdx - 4);
        int localEnd = std::min(OUT_DIMS - 1, maxIdx + 4);
        float weightedSum = 0.0f;
        float weightSum = 0.0f;
        for (int i = localStart; i <= localEnd; ++i)
        {
            weightedSum += centTable_[i] * frame[i];
            weightSum += frame[i];
        }
        f0[t] = (weightSum > 1e-9f) ? centToF0(weightedSum / weightSum) : 0.0f;
    }
    return f0;
}

// ============================================================
// interp_uv — 对齐官方 batch_interp_with_replacement_detach
// 对 unvoiced 帧 (f0==0) 用相邻 voiced 帧线性插值填充
// ============================================================

void FCPEExtractor::interpF0Gaps(std::vector<float>& f0)
{
    if (f0.empty()) return;

    // 收集 voiced 帧索引和 f0 值
    std::vector<int> voicedIdx;
    std::vector<float> voicedF0;
    for (int i = 0; i < static_cast<int>(f0.size()); ++i)
    {
        if (f0[i] > 0.0f)
        {
            voicedIdx.push_back(i);
            voicedF0.push_back(f0[i]);
        }
    }

    if (voicedIdx.empty()) return; // 全部 unvoiced，无法插值

    // 对每个 unvoiced 帧做线性插值（Hz 空间，与官方一致）
    for (int i = 0; i < static_cast<int>(f0.size()); ++i)
    {
        if (f0[i] > 0.0f) continue;

        // 找第一个 voiced 索引 >= i
        auto it = std::lower_bound(voicedIdx.begin(), voicedIdx.end(), i);

        if (it == voicedIdx.begin())
        {
            // i 在第一个 voiced 帧之前 → clamp 到第一个 voiced f0
            f0[i] = voicedF0.front();
        }
        else if (it == voicedIdx.end())
        {
            // i 在最后一个 voiced 帧之后 → clamp 到最后一个 voiced f0
            f0[i] = voicedF0.back();
        }
        else
        {
            // i 在两个 voiced 帧之间 → 线性插值
            int rightIdx = *it;
            int leftIdx = *(it - 1);
            float rightF0 = voicedF0[it - voicedIdx.begin()];
            float leftF0 = voicedF0[it - voicedIdx.begin() - 1];

            float t = static_cast<float>(i - leftIdx) / static_cast<float>(rightIdx - leftIdx);
            f0[i] = leftF0 + t * (rightF0 - leftF0);
        }
    }
}

// ============================================================
// Main extraction pipeline
// ============================================================

std::vector<float> FCPEExtractor::extractF0(
    const float* audio,
    size_t length,
    int sampleRate,
    Ort::RunOptions& runOptions,
    std::function<void(float)> progressCallback,
    std::function<void(const std::vector<float>&, int)> partialCallback)
{
    if (!audio || length == 0)
        throw std::invalid_argument("[FCPE] audio is null or empty");
    if (sampleRate <= 0)
        throw std::invalid_argument("[FCPE] sampleRate must be positive");
    if (!session_)
        throw std::logic_error("[FCPE] session not initialized");
    if (!resampler_)
        throw std::logic_error("[FCPE] resampler not initialized");

    PreflightResult preflight = preflightCheck(length, sampleRate);
    if (!preflight.success)
        throw std::runtime_error(preflight.errorMessage);

    if (progressCallback) progressCallback(0.1f);

    std::vector<float> audio16k;
    if (sampleRate != SAMPLE_RATE)
        audio16k = resampler_->downsampleForInference(audio, length, sampleRate, SAMPLE_RATE);
    else
        audio16k.assign(audio, audio + length);

    if (audio16k.empty())
        throw std::runtime_error("[FCPE] Resampling failed");

    // ---- 前处理：对齐 RMVPEExtractor ----

    // 1. 高通滤波：50Hz Butterworth 8阶（48dB/oct）
    auto coefficients = juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod(
        50.0f, static_cast<float>(SAMPLE_RATE), 8);
    float* channelData = audio16k.data();
    juce::dsp::AudioBlock<float> block(&channelData, 1, audio16k.size());
    juce::dsp::ProcessContextReplacing<float> context(block);
    for (auto& coefs : coefficients)
    {
        juce::dsp::IIR::Filter<float> filter;
        filter.coefficients = coefs;
        filter.reset();
        filter.process(context);
    }

    // 2. 噪声门：-50dBFS（|sample| < 0.00316 → 0）
    constexpr float kNoiseGateThreshold = 0.00316227766f;
    for (auto& sample : audio16k)
        if (std::abs(sample) < kNoiseGateThreshold)
            sample = 0.0f;

    if (progressCallback) progressCallback(0.3f);

    auto mel = extractMel(audio16k);
    if (mel.empty())
        throw std::runtime_error("[FCPE] Mel extraction produced no frames");

    if (progressCallback) progressCallback(0.5f);

    int numFrames = static_cast<int>(mel.size());
    std::vector<float> melFlat(static_cast<size_t>(numFrames) * N_MELS);
    for (int t = 0; t < numFrames; ++t)
        for (int m = 0; m < N_MELS; ++m)
            melFlat[static_cast<size_t>(t) * N_MELS + m] = mel[t][m];

    std::vector<int64_t> inputShape = {1, numFrames, N_MELS};
    auto inputTensor = Ort::Value::CreateTensor<float>(
        *memoryInfo_, melFlat.data(), melFlat.size(),
        inputShape.data(), inputShape.size());

    const char* inputNames[] = {"mel"};
    const char* outputNames[] = {"latent"};
    std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(std::move(inputTensor));

    if (progressCallback) progressCallback(0.6f);

    auto outputTensors = session_->Run(
        runOptions, inputNames, inputTensors.data(), 1, outputNames, 1);

    if (progressCallback) progressCallback(0.8f);

    float* outputData = outputTensors[0].GetTensorMutableData<float>();
    const int outFrames = static_cast<int>(
        outputTensors[0].GetTensorTypeAndShapeInfo().GetElementCount() / OUT_DIMS);

    auto f0 = decodeF0(outputData, outFrames, confidenceThreshold_);

    // ---- 后处理：对齐官方 torchfcpe infer() ----

    // 1. f0Min 过滤：低于 f0Min 的帧置零（官方: uv = (f0 < f0_min); f0 *= (1-uv)）
    if (f0Min_ > 0.0f)
    {
        for (auto& v : f0)
            if (v > 0.0f && v < f0Min_) v = 0.0f;
    }

    // 2. f0Max 钳位：超过 f0Max 的帧截断（官方: f0[f0 > f0_max] = f0_max）
    if (f0Max_ > 0.0f)
    {
        for (auto& v : f0)
            if (v > f0Max_) v = f0Max_;
    }

    // 3. interp_uv：对 unvoiced 帧用相邻 voiced 帧线性插值（官方默认关闭）
    if (interpUV_)
        interpF0Gaps(f0);

    if (partialCallback && !f0.empty())
        partialCallback(f0, 0);

    if (progressCallback) progressCallback(1.0f);

    AppLogger::debug("[FCPEExtractor] Extracted F0 frames: " + juce::String((int)f0.size()));

    return f0;
}

} // namespace OpenTune
