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
        int inputSR, int targetSR
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
        int inputSR, int targetSR
    );



private:
    std::vector<float> resample(
        const float* input, size_t inputLength,
        int inputSR, int targetSR
    );

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ResamplingManager)
};

} // namespace OpenTune
