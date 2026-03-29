#pragma once

/**
 * 调式推断模块
 * 
 * 从F0数据推断音频的调式（大调/小调）和主音。
 * 使用Krumhansl-Schmuckler调式检测算法。
 */

#include <juce_core/juce_core.h>
#include <vector>
#include <array>

namespace OpenTune {

/**
 * Key - 音名枚举
 */
enum class Key {
    C = 0, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B
};

/**
 * Scale - 调式枚举
 */
enum class Scale {
    Major,      // 大调
    Minor,      // 小调
    Chromatic   // 半音阶
};

/**
 * DetectedKey - 检测到的调式信息
 */
struct DetectedKey {
    Key root = Key::C;              // 主音
    Scale scale = Scale::Major;     // 调式
    float confidence = 0.0f;        // 置信度

    static juce::String keyToString(Key key);
    static juce::String scaleToString(Scale scale);
};

/**
 * ScaleInference - 调式推断类
 * 
 * 通过分析F0数据分布，推断最可能的调式和主音。
 */
class ScaleInference {
public:
    ScaleInference();
    ~ScaleInference();

    /**
     * 处理F0数据（批量）
     * @param f0Frequencies F0频率数组
     */
    void processF0Data(const std::vector<float>& f0Frequencies);

    /**
     * 处理F0数据（批量，加权版）
     * @param f0Frequencies F0频率数组
     * @param confidences 每帧置信度(0-1)，可为空
     * @param energies 每帧能量，可为空
     */
    void processF0Data(const std::vector<float>& f0Frequencies,
                       const std::vector<float>& confidences,
                       const std::vector<float>& energies);
    
    /**
     * 重置检测状态
     */
    void reset();

    /**
     * 查找最佳匹配调式
     * @return 检测到的调式信息
     */
    DetectedKey findBestMatch() const;

    /**
     * 使用F0值更新检测（流式）
     * @param frequency F0频率
     */
    void updateWithNewF0(float frequency);
    
    /**
     * 获取当前检测结果
     */
    DetectedKey getCurrentDetection() const;

    /**
     * 设置投票持续时间
     * @param seconds 持续时间（秒）
     */
    void setVotingDuration(float seconds);
    
    /**
     * 更新检测（帧更新）
     * @param deltaTime 帧间隔时间
     */
    void update(float deltaTime);

private:
    /**
     * 将频率转换为音分数
     */
    float frequencyToCents(float frequency) const;
    
    /**
     * 将频率转换为音高bin
     */
    int frequencyToBin(float frequency) const;
    
    /**
     * 旋转调式模板
     */
    void rotateTemplate(const std::array<float, 12>& base, int semitones, std::array<float, 12>& result) const;
    
    /**
     * 计算匹配分数
     */
    float computeScore(const std::array<float, 12>& histogram, const std::array<float, 12>& templ) const;

    std::array<float, 12> histogram_;                   // 音高直方图
    std::array<std::array<float, 12>, 24> templates_;   // 调式模板（12大调 + 12小调）

    DetectedKey candidateKey_;      // 候选调式
    float candidateHoldTime_ = 0.0f; // 候选保持时间
    float votingDuration_ = 3.0f;    // 投票持续时间
    DetectedKey confirmedKey_;       // 确认的调式

    static const std::array<float, 12> majorProfile_;   // 大调模板
    static const std::array<float, 12> minorProfile_;   // 小调模板

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScaleInference)
};

} // namespace OpenTune
