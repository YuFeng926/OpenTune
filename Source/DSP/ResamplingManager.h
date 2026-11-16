#pragma once

/**
 * 重采样管理器
 * 
 * 提供高质量的音频重采样功能，用于：
 * - 将宿主采样率转换为模型推理采样率
 * - 将推理结果转换回宿主采样率
 * - F0曲线对齐时的长度调整
 */

#include <juce_core/juce_core.h>
#include "../Utils/PlaybackSampleGeometry.h"
#include <vector>

namespace OpenTune {

/**
 * ResamplingManager - 重采样管理器类
 * 
 * 封装重采样算法，支持任意采样率之间的转换。
 */
class ResamplingManager {
public:
    ResamplingManager();
    ~ResamplingManager();

    /**
     * 为推理下采样音频
     * @param input 输入音频数据
     * @param inputLength 输入长度
     * @param inputSR 输入采样率
     * @param targetSR 目标采样率
     * @return 重采样后的音频数据
     */
    std::vector<float> downsampleForInference(
        const float* input, size_t inputLength,
        double inputSR, double targetSR
    );

    /**
     * 为宿主上采样音频
     * @param input 输入音频数据
     * @param inputLength 输入长度
     * @param inputSR 输入采样率
     * @param targetSR 目标采样率
     * @return 重采样后的音频数据
     */
    std::vector<float> upsampleForHost(
        const float* input, size_t inputLength,
        double inputSR, double targetSR
    );

    /**
     * 来源零点开始的整段转换。窗口准备使用 resampleRange 的全局网格。
     */
    std::vector<float> resampleExactLength(
        const float* input, size_t inputLength,
        double inputSR, double targetSR, int outputLength);

    /** Real input support and finite aligned startup for [outputBegin,outputEnd).
     * The returned addresses are on the original input sample grid. */
    PlaybackSampleGeometry::SampleSpan requiredInputRange(
        double inputSR, double targetSR, int64_t outputBegin, int64_t outputEnd);

    /** Convert the requested global output interval. input[0] is at
     * firstInputSample; only this immutable source extent contains real PCM.
     * Samples outside that extent use zero extension. Work is bounded by the
     * requested output and the actual filter support, independent of position. */
    std::vector<float> resampleRange(
        const float* input, size_t inputLength, double inputSR, double targetSR,
        int64_t firstInputSample, int64_t firstOutputSample, int outputLength);

private:
    std::vector<float> resample(
        const float* input, size_t inputLength,
        double inputSR, double targetSR
    );

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ResamplingManager)
};

} // namespace OpenTune
