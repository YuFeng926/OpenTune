#pragma once

#include "../Utils/Error.h"
#include <vector>
#include <memory>
#include <cstddef>

namespace juce {
namespace dsp {
    class FFT;
    template<typename T> class WindowingFunction;
}
}

namespace OpenTune {

/**
 * MelSpectrogramConfig - Mel频谱计算配置
 */
struct MelSpectrogramConfig
{
    int sampleRate = 44100;
    int nFft = 2048;
    int winLength = 2048;
    int hopLength = 512;
    int nMels = 128;
    float fMin = 40.0f;
    float fMax = 16000.0f;
    float logEps = 1.0e-5f;

    size_t hash() const noexcept
    {
        size_t h = 14695981039346656037ULL;
        auto combine = [&h](auto v) {
            h ^= static_cast<size_t>(v);
            h *= 1099511628211ULL;
        };
        combine(sampleRate);
        combine(nFft);
        combine(winLength);
        combine(hopLength);
        combine(nMels);
        combine(static_cast<int>(fMin * 1000));
        combine(static_cast<int>(fMax * 1000));
        return h;
    }
};

using MelResult = Result<std::vector<float>>;

/**
 * MelSpectrogramProcessor - Mel频谱计算处理器
 * 
 * 持有可复用的计算资源:
 * - FFT对象 (JUCE DSP)
 * - 窗口函数 (JUCE DSP)
 * - Mel滤波器组
 * - 工作缓冲区
 * 
 * 当配置变化时自动重新初始化。每个线程应持有独立实例。
 */
class MelSpectrogramProcessor
{
public:
    MelSpectrogramProcessor();
    ~MelSpectrogramProcessor();

    Result<void> configure(const MelSpectrogramConfig& cfg);

    const MelSpectrogramConfig& getConfig() const noexcept { return config_; }

    Result<void> compute(const float* audio, int numSamples, int numFrames, float* output);

    MelResult compute(const float* audio, int numSamples, int numFrames);

    bool isInitialized() const noexcept { return initialized_; }

private:
    Result<void> initFftAndWindow();

    /**
     * 初始化Mel滤波器组
     */
    void initMelFilterbank();

    /**
     * 调整工作缓冲区大小
     */
    void resizeBuffers(int numSamples);

private:
    MelSpectrogramConfig config_;
    size_t configHash_ = 0;
    bool initialized_ = false;

    // JUCE DSP对象 (使用unique_ptr因为不可拷贝/无默认构造)
    std::unique_ptr<juce::dsp::FFT> fft_;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window_;

    // Mel滤波器组 [nMels][nFftBins]
    std::vector<std::vector<float>> melFilterbank_;

    // 工作缓冲区
    std::vector<float> paddedAudio_;    // 填充后的音频
    std::vector<float> fftBuffer_;      // FFT缓冲区 (实部+虚部交错)
    std::vector<float> magnitudeBuffer_; // 幅度谱缓冲区

    int lastNumSamples_ = 0;  // 用于缓冲区复用判断
};

MelResult computeLogMelSpectrogram(const float* audio,
                                   int numSamples,
                                   int numFrames,
                                   const MelSpectrogramConfig& cfg);

} // namespace OpenTune