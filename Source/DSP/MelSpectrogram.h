#pragma once

/**
 * Mel频谱计算模块
 * 
 * 提供对数Mel频谱图计算功能，用于AI模型推理的音频特征提取。
 * Mel频谱是人耳感知音频的常用表示方式。
 */

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
 * 
 * 包含计算Mel频谱所需的所有参数：
 * - sampleRate: 音频采样率
 * - nFft: FFT窗口大小
 * - winLength: 窗口长度
 * - hopLength: 跳跃长度（帧移）
 * - nMels: Mel滤波器组数量
 * - fMin/fMax: 频率范围
 * - logEps: 对数计算的最小值（防止log(0)）
 */
struct MelSpectrogramConfig
{
    int sampleRate = 44100;     // 采样率
    int nFft = 2048;            // FFT大小
    int winLength = 2048;       // 窗口长度
    int hopLength = 512;        // 帧移
    int nMels = 128;            // Mel滤波器组数量
    float fMin = 40.0f;         // 最低频率
    float fMax = 16000.0f;      // 最高频率
    float logEps = 1.0e-5f;     // 对数计算的最小值

    /**
     * 计算配置哈希值，用于检测配置变化
     */
    size_t hash() const noexcept
    {
        // 简单的FNV-1a哈希
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

    /**
     * 设置配置并初始化资源
     * @param cfg Mel频谱配置
     * @return 是否成功初始化
     */
    bool configure(const MelSpectrogramConfig& cfg);

    /**
     * 获取当前配置
     */
    const MelSpectrogramConfig& getConfig() const noexcept { return config_; }

    /**
     * 计算对数Mel频谱图
     * 
     * @param audio 输入音频数据
     * @param numSamples 音频样本数
     * @param numFrames 输出帧数
     * @param output 输出缓冲区（需预分配nMels * numFrames大小）
     * @return 是否成功
     */
    bool compute(const float* audio, int numSamples, int numFrames, float* output);

    /**
     * 计算对数Mel频谱图（返回vector版本）
     */
    std::vector<float> compute(const float* audio, int numSamples, int numFrames);

    /**
     * 强制重新初始化资源
     */
    void reset();

    /**
     * 检查是否已初始化
     */
    bool isInitialized() const noexcept { return initialized_; }

private:
    /**
     * 初始化FFT和窗口
     */
    bool initFftAndWindow();

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

/**
 * 计算对数Mel频谱图 (兼容旧API)
 * 
 * 注意: 此函数使用线程局部存储的处理器实例，适合多线程调用。
 * 如需更高性能，建议直接使用MelSpectrogramProcessor实例。
 * 
 * @param audio 输入音频数据
 * @param numSamples 音频样本数
 * @param numFrames 输出帧数
 * @param cfg Mel频谱配置
 * @return 对数Mel频谱数据（nMels x numFrames）
 */
std::vector<float> computeLogMelSpectrogram(const float* audio,
                                            int numSamples,
                                            int numFrames,
                                            const MelSpectrogramConfig& cfg);

} // namespace OpenTune