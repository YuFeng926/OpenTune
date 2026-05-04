#include "MelSpectrogram.h"
#include "../Utils/Error.h"
#include "../Utils/AppLogger.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

namespace OpenTune {

static float hzToMel(float hz)
{
    const float fSp = 200.0f / 3.0f;
    const float minLogHz = 1000.0f;
    const float minLogMel = minLogHz / fSp;
    const float logStep = std::log(6.4f) / 27.0f;

    if (hz < minLogHz)
        return hz / fSp;

    return minLogMel + std::log(hz / minLogHz) / logStep;
}

static float melToHz(float mel)
{
    const float fSp = 200.0f / 3.0f;
    const float minLogHz = 1000.0f;
    const float minLogMel = minLogHz / fSp;
    const float logStep = std::log(6.4f) / 27.0f;

    if (mel < minLogMel)
        return fSp * mel;

    return minLogHz * std::exp(logStep * (mel - minLogMel));
}

static int reflectIndex(int i, int n)
{
    if (n <= 1) return 0;
    while (i < 0 || i >= n)
    {
        if (i < 0) i = -i;
        if (i >= n) i = 2 * n - i - 2;
    }
    return i;
}

static float dotProduct(const float* a, const float* b, size_t count)
{
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) sum += a[i] * b[i];
    return sum;
}

MelSpectrogramProcessor::MelSpectrogramProcessor() = default;

MelSpectrogramProcessor::~MelSpectrogramProcessor() = default;

Result<void> MelSpectrogramProcessor::configure(const MelSpectrogramConfig& cfg)
{
    const size_t newHash = cfg.hash();
    if (initialized_ && configHash_ == newHash)
        return Result<void>::success();

    const int fftOrder = (int) std::round(std::log2((double) cfg.nFft));
    if ((1 << fftOrder) != cfg.nFft)
        return Result<void>::failure(ErrorCode::MelFFTSizeInvalid,
            "FFT size must be power of 2, got: " + std::to_string(cfg.nFft));

    config_ = cfg;
    configHash_ = newHash;

    auto fftResult = initFftAndWindow();
    if (!fftResult)
        return fftResult;

    initMelFilterbank();

    initialized_ = true;
    return Result<void>::success();
}

Result<void> MelSpectrogramProcessor::initFftAndWindow()
{
    const int fftOrder = (int) std::round(std::log2((double) config_.nFft));
    
    fft_ = std::make_unique<juce::dsp::FFT>(fftOrder);
    window_ = std::make_unique<juce::dsp::WindowingFunction<float>>(
        (size_t) config_.winLength, 
        juce::dsp::WindowingFunction<float>::hann, 
        false
    );

    const int fftSize = config_.nFft * 2;
    fftBuffer_.resize((size_t) fftSize);

    const int nFftBins = config_.nFft / 2 + 1;
    magnitudeBuffer_.resize((size_t) nFftBins);

    return Result<void>::success();
}

void MelSpectrogramProcessor::initMelFilterbank()
{
    const int nFftBins = config_.nFft / 2 + 1;
    melFilterbank_.clear();
    melFilterbank_.resize((size_t) config_.nMels, std::vector<float>((size_t) nFftBins, 0.0f));

    const float melMin = hzToMel(config_.fMin);
    const float melMax = hzToMel(std::min(config_.fMax, 0.5f * (float) config_.sampleRate));

    std::vector<float> melPoints((size_t) config_.nMels + 2);
    std::vector<float> hzPoints((size_t) config_.nMels + 2);
    std::vector<int> binPoints((size_t) config_.nMels + 2);

    for (int i = 0; i < (int) melPoints.size(); ++i)
    {
        melPoints[(size_t) i] = melMin + (melMax - melMin) * ((float) i / (float) (config_.nMels + 1));
        hzPoints[(size_t) i] = melToHz(melPoints[(size_t) i]);
        const int bin = (int) std::floor((config_.nFft + 1) * hzPoints[(size_t) i] / (float) config_.sampleRate);
        binPoints[(size_t) i] = juce::jlimit(0, nFftBins - 1, bin);
    }

    for (int m = 0; m < config_.nMels; ++m)
    {
        const int left = binPoints[m];
        const int center = binPoints[m + 1];
        const int right = binPoints[m + 2];

        if (center == left || right == center)
            continue;

        for (int k = left; k < center; ++k)
            melFilterbank_[m][k] = (float) (k - left) / (float) (center - left);

        for (int k = center; k < right; ++k)
            melFilterbank_[m][k] = (float) (right - k) / (float) (right - center);

        const float hzLeft = hzPoints[(size_t) m];
        const float hzRight = hzPoints[(size_t) m + 2];
        const float enorm = (hzRight > hzLeft) ? (2.0f / (hzRight - hzLeft)) : 1.0f;
        
        juce::FloatVectorOperations::multiply(melFilterbank_[m].data(), enorm, nFftBins);
    }
}

void MelSpectrogramProcessor::resizeBuffers(int numSamples)
{
    if (numSamples != lastNumSamples_)
    {
        const int pad = config_.nFft / 2;
        paddedAudio_.resize((size_t) numSamples + (size_t) pad * 2);
        lastNumSamples_ = numSamples;
    }
}

Result<void> MelSpectrogramProcessor::compute(const float* audio, int numSamples, int numFrames, float* output)
{
    if (!initialized_)
        return Result<void>::failure(ErrorCode::MelNotConfigured, "MelSpectrogramProcessor not configured");
    if (audio == nullptr)
        return Result<void>::failure(ErrorCode::InvalidAudioInput, "Audio buffer is null");
    if (numSamples <= 0)
        return Result<void>::failure(ErrorCode::InvalidAudioLength, "Invalid sample count: " + std::to_string(numSamples));
    if (numFrames <= 0)
        return Result<void>::failure(ErrorCode::InvalidParameter, "Invalid frame count: " + std::to_string(numFrames));
    if (output == nullptr)
        return Result<void>::failure(ErrorCode::InvalidParameter, "Output buffer is null");

    resizeBuffers(numSamples);

    const int nFftBins = config_.nFft / 2 + 1;
    const int pad = config_.nFft / 2;

    for (int i = 0; i < (int) paddedAudio_.size(); ++i)
        paddedAudio_[(size_t) i] = audio[(size_t) reflectIndex(i - pad, numSamples)];

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int startSample = frame * config_.hopLength;
        
        juce::FloatVectorOperations::clear(fftBuffer_.data(), (int) fftBuffer_.size());

        for (int i = 0; i < config_.winLength; ++i)
        {
            const int idx = startSample + i;
            fftBuffer_[(size_t) i] = (idx < (int) paddedAudio_.size()) 
                ? paddedAudio_[(size_t) idx] 
                : 0.0f;
        }

        window_->multiplyWithWindowingTable(fftBuffer_.data(), (size_t) config_.winLength);

        fft_->performFrequencyOnlyForwardTransform(fftBuffer_.data());

        // 应用Mel滤波器组: 点积 → epsilon clamping → 向量化 log
        float melSums[128];
        const int nMelsActual = std::min(config_.nMels, 128);
        for (int m = 0; m < nMelsActual; ++m)
        {
            melSums[m] = dotProduct(fftBuffer_.data(), melFilterbank_[(size_t) m].data(), nFftBins);
            melSums[m] = std::max(config_.logEps, melSums[m]);
        }

        float logResults[128];
        for (int m = 0; m < nMelsActual; ++m) logResults[m] = std::log(melSums[m]);

        for (int m = 0; m < nMelsActual; ++m)
        {
            output[(size_t) m * (size_t) numFrames + (size_t) frame] = logResults[m];
        }
    }

    return Result<void>::success();
}

MelResult MelSpectrogramProcessor::compute(const float* audio, int numSamples, int numFrames)
{
    if (!initialized_)
        return MelResult::failure(ErrorCode::MelNotConfigured, "MelSpectrogramProcessor not configured");
    if (audio == nullptr)
        return MelResult::failure(ErrorCode::InvalidAudioInput, "Audio buffer is null");
    if (numSamples <= 0)
        return MelResult::failure(ErrorCode::InvalidAudioLength, "Invalid sample count: " + std::to_string(numSamples));
    if (numFrames <= 0)
        return MelResult::failure(ErrorCode::InvalidParameter, "Invalid frame count: " + std::to_string(numFrames));

    std::vector<float> mel((size_t) config_.nMels * (size_t) numFrames);
    
    auto result = compute(audio, numSamples, numFrames, mel.data());
    if (result)
        return MelResult::success(std::move(mel));
    
    return MelResult::failure(result.error());
}

MelResult computeLogMelSpectrogram(const float* audio,
                                   int numSamples,
                                   int numFrames,
                                   const MelSpectrogramConfig& cfg)
{
    if (audio == nullptr)
        return MelResult::failure(ErrorCode::InvalidAudioInput, "Audio buffer is null");
    if (numSamples <= 0)
        return MelResult::failure(ErrorCode::InvalidAudioLength, "Invalid sample count: " + std::to_string(numSamples));
    if (numFrames <= 0)
        return MelResult::failure(ErrorCode::InvalidParameter, "Invalid frame count: " + std::to_string(numFrames));

    thread_local MelSpectrogramProcessor processor;
    
    auto configResult = processor.configure(cfg);
    if (!configResult)
        return MelResult::failure(configResult.error());

    return processor.compute(audio, numSamples, numFrames);
}

} // namespace OpenTune