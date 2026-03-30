#include "MelSpectrogram.h"
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

bool MelSpectrogramProcessor::configure(const MelSpectrogramConfig& cfg)
{
    // 检查配置是否变化
    const size_t newHash = cfg.hash();
    if (initialized_ && configHash_ == newHash)
        return true; // 配置相同，无需重新初始化

    // 验证FFT大小是2的幂
    const int fftOrder = (int) std::round(std::log2((double) cfg.nFft));
    if ((1 << fftOrder) != cfg.nFft)
        return false;

    config_ = cfg;
    configHash_ = newHash;

    // 初始化FFT和窗口
    if (!initFftAndWindow())
        return false;

    // 初始化Mel滤波器组
    initMelFilterbank();

    initialized_ = true;
    return true;
}

bool MelSpectrogramProcessor::initFftAndWindow()
{
    const int fftOrder = (int) std::round(std::log2((double) config_.nFft));
    
    fft_ = std::make_unique<juce::dsp::FFT>(fftOrder);
    window_ = std::make_unique<juce::dsp::WindowingFunction<float>>(
        (size_t) config_.winLength, 
        juce::dsp::WindowingFunction<float>::hann, 
        false
    );

    // 预分配FFT缓冲区
    const int fftSize = config_.nFft * 2; // 实部+虚部交错
    fftBuffer_.resize((size_t) fftSize);

    // 预分配幅度谱缓冲区
    const int nFftBins = config_.nFft / 2 + 1;
    magnitudeBuffer_.resize((size_t) nFftBins);

    return fft_ != nullptr && window_ != nullptr;
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

bool MelSpectrogramProcessor::compute(const float* audio, int numSamples, int numFrames, float* output)
{
    if (!initialized_ || audio == nullptr || numSamples <= 0 || numFrames <= 0 || output == nullptr)
        return false;

    // 调整缓冲区大小
    resizeBuffers(numSamples);

    const int nFftBins = config_.nFft / 2 + 1;
    const int pad = config_.nFft / 2;

    // 填充音频（反射填充）
    for (int i = 0; i < (int) paddedAudio_.size(); ++i)
        paddedAudio_[(size_t) i] = audio[(size_t) reflectIndex(i - pad, numSamples)];

    // 获取 SIMD 加速器实例
    auto& simd = SimdAccelerator::getInstance();

    // 逐帧处理
    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int startSample = frame * config_.hopLength;
        
        // 清空FFT缓冲区
        juce::FloatVectorOperations::clear(fftBuffer_.data(), (int) fftBuffer_.size());

        // 复制窗口数据
        for (int i = 0; i < config_.winLength; ++i)
        {
            const int idx = startSample + i;
            fftBuffer_[(size_t) i] = (idx < (int) paddedAudio_.size()) 
                ? paddedAudio_[(size_t) idx] 
                : 0.0f;
        }

        // 应用窗口函数
        window_->multiplyWithWindowingTable(fftBuffer_.data(), (size_t) config_.winLength);

        // 执行FFT（仅频率输出）
        fft_->performFrequencyOnlyForwardTransform(fftBuffer_.data());

        // 应用Mel滤波器组
        for (int m = 0; m < config_.nMels; ++m)
        {
            // SIMD点积计算
            float sum = simd.dotProduct(fftBuffer_.data(), melFilterbank_[(size_t) m].data(), nFftBins);

            // 对数变换
            const float logVal = std::log(std::max(config_.logEps, sum));
            output[(size_t) m * (size_t) numFrames + (size_t) frame] = logVal;
        }
    }

    return true;
}

std::vector<float> MelSpectrogramProcessor::compute(const float* audio, int numSamples, int numFrames)
{
    if (!initialized_ || audio == nullptr || numSamples <= 0 || numFrames <= 0)
        return {};

    std::vector<float> mel((size_t) config_.nMels * (size_t) numFrames);
    
    if (compute(audio, numSamples, numFrames, mel.data()))
        return mel;
    
    return {};
}

void MelSpectrogramProcessor::reset()
{
    fft_.reset();
    window_.reset();
    melFilterbank_.clear();
    paddedAudio_.clear();
    fftBuffer_.clear();
    magnitudeBuffer_.clear();
    initialized_ = false;
    configHash_ = 0;
    lastNumSamples_ = 0;
}

std::vector<float> computeLogMelSpectrogram(const float* audio,
                                            int numSamples,
                                            int numFrames,
                                            const MelSpectrogramConfig& cfg)
{
    if (audio == nullptr || numSamples <= 0 || numFrames <= 0)
        return {};

    // 线程局部存储处理器实例，避免重复创建
    thread_local MelSpectrogramProcessor processor;
    
    // 配置（如果配置相同会跳过重新初始化）
    if (!processor.configure(cfg))
        return {};

    return processor.compute(audio, numSamples, numFrames);
}

} // namespace OpenTune