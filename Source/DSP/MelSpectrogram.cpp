#include "MelSpectrogram.h"
#include "../Utils/Error.h"
#include "../Utils/AppLogger.h"
#include "../Utils/SimdAccelerator.h"
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

    // Periodic Hann window matching torch.hann_window default (periodic=True):
    //   w[n] = 0.5 * (1 - cos(2*pi*n / N)),  n = 0..N-1
    // NOT the symmetric variant 2*pi*n/(N-1) used by JUCE's WindowingFunction::hann.
    hannWindow_.resize((size_t) config_.winLength);
    const float twoPiOverN = 2.0f * 3.14159265358979323846f / (float) config_.winLength;
    for (int n = 0; n < config_.winLength; ++n)
        hannWindow_[(size_t) n] = 0.5f * (1.0f - std::cos(twoPiOverN * (float) n));

    const int fftSize = config_.nFft * 2;
    fftBuffer_.resize((size_t) fftSize);

    const int nFftBins = config_.nFft / 2 + 1;
    magnitudeBuffer_.resize((size_t) nFftBins);

    return Result<void>::success();
}

void MelSpectrogramProcessor::initMelFilterbank()
{
    // librosa.filters.mel(htk=False, norm='slaney') reference algorithm:
    //   1. Mel-spaced anchor frequencies in Hz (n_mels + 2 points), Slaney scale
    //   2. Sample triangular filters at exact FFT bin frequencies (NOT integer-snap)
    //   3. Slaney normalization: weights /= (mel_f[m+2] - mel_f[m]) / 2
    // Differs from old HTK-style integer-bin snap which lost precision in low
    // frequencies where mel spacing is dense.

    const int nFftBins = config_.nFft / 2 + 1;
    melFilterbank_.clear();
    melFilterbank_.resize((size_t) config_.nMels, std::vector<float>((size_t) nFftBins, 0.0f));

    const float melMin = hzToMel(config_.fMin);
    const float melMax = hzToMel(std::min(config_.fMax, 0.5f * (float) config_.sampleRate));

    // mel_f: n_mels + 2 mel-spaced anchor frequencies in Hz
    std::vector<float> melF((size_t) config_.nMels + 2);
    for (int i = 0; i < (int) melF.size(); ++i)
    {
        const float mel = melMin + (melMax - melMin) * ((float) i / (float) (config_.nMels + 1));
        melF[(size_t) i] = melToHz(mel);
    }

    // FFT bin frequencies (matches np.fft.rfftfreq(n_fft, d=1/sr))
    std::vector<float> fftFreqs((size_t) nFftBins);
    const float binHz = (float) config_.sampleRate / (float) config_.nFft;
    for (int k = 0; k < nFftBins; ++k)
        fftFreqs[(size_t) k] = (float) k * binHz;

    // Triangle for filter m: peak at mel_f[m+1], shoulders at mel_f[m] and mel_f[m+2].
    // Sampled at every fftFreq (fractional value, NOT snapped to integer bin).
    for (int m = 0; m < config_.nMels; ++m)
    {
        const float left   = melF[(size_t) m];
        const float center = melF[(size_t) (m + 1)];
        const float right  = melF[(size_t) (m + 2)];

        const float leftSpan  = center - left;   // both > 0 by mel_f construction
        const float rightSpan = right - center;
        if (leftSpan <= 0.0f || rightSpan <= 0.0f)
            continue;

        // Slaney norm: total triangle area = 1 / ((right - left) / 2)
        const float enorm = 2.0f / (right - left);

        for (int k = 0; k < nFftBins; ++k)
        {
            const float f = fftFreqs[(size_t) k];
            const float lower = (f - left)  / leftSpan;   // ramps up 0 → 1 on [left, center]
            const float upper = (right - f) / rightSpan;  // ramps down 1 → 0 on [center, right]
            const float w = std::min(lower, upper);
            if (w > 0.0f)
                melFilterbank_[(size_t) m][(size_t) k] = w * enorm;
        }
    }
}

void MelSpectrogramProcessor::resizeBuffers(int numSamples)
{
    if (numSamples != lastNumSamples_)
    {
        // Match training-time asymmetric padding (wav2mel.py:64-72):
        //   padLeft  = (win_size - hop_length) // 2     = 768
        //   padRight = (win_size - hop_length + 1) // 2 = 769
        // Python uses integer division with +1 on the right, producing 1 more
        // sample on the right.  Reproduce exactly to avoid per-frame misalignment.
        padLeft_  = (config_.winLength - config_.hopLength) / 2;
        padRight_ = (config_.winLength - config_.hopLength + 1) / 2;
        paddedAudio_.resize((size_t) numSamples + (size_t) padLeft_ + (size_t) padRight_);
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

    for (int i = 0; i < (int) paddedAudio_.size(); ++i)
        paddedAudio_[(size_t) i] = audio[(size_t) reflectIndex(i - padLeft_, numSamples)];

    std::vector<float> melSums((size_t) config_.nMels);
    std::vector<float> logResults((size_t) config_.nMels);

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int startSample = frame * config_.hopLength;

        juce::FloatVectorOperations::clear(fftBuffer_.data(), (int) fftBuffer_.size());

        // Copy + apply periodic Hann window in one pass.
        for (int i = 0; i < config_.winLength; ++i)
        {
            const int idx = startSample + i;
            const float sample = (idx < (int) paddedAudio_.size())
                ? paddedAudio_[(size_t) idx]
                : 0.0f;
            fftBuffer_[(size_t) i] = sample * hannWindow_[(size_t) i];
        }

        fft_->performFrequencyOnlyForwardTransform(fftBuffer_.data());

        // 应用Mel滤波器组: 点积 → epsilon clamping → 向量化 log
        const auto& simd = SimdAccelerator::getInstance();
        const int nMels = config_.nMels;
        for (int m = 0; m < nMels; ++m)
        {
            melSums[(size_t) m] = simd.dotProduct(fftBuffer_.data(), melFilterbank_[(size_t) m].data(), nFftBins);
            melSums[(size_t) m] = std::max(config_.logEps, melSums[(size_t) m]);
        }

        simd.vectorLog(logResults.data(), melSums.data(), static_cast<size_t>(nMels));

        for (int m = 0; m < nMels; ++m)
        {
            output[(size_t) m * (size_t) numFrames + (size_t) frame] = logResults[(size_t) m];
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