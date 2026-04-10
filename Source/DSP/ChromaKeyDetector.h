#pragma once

/**
 * ChromaKeyDetector - 基于 Chroma/HPCP 特征的调式检测器
 *
 * 使用 STFT → Chroma 向量 → Pearson 相关系数模板匹配的方案，
 * 直接从音频 PCM 数据检测调式，天然支持单音和复音素材。
 *
 * 本文件同时定义了 Key、Scale、DetectedKey 等调式相关基础类型。
 */

#include <juce_core/juce_core.h>
#include <memory>
#include <array>
#include <vector>

namespace juce {
namespace dsp {
    class FFT;
    template<typename T> class WindowingFunction;
}
}

namespace OpenTune {

// ============================================================================
// 调式相关基础类型
// ============================================================================

/**
 * Key - 音名枚举
 */
enum class Key {
    C = 0, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B
};

/**
 * Scale - 调式枚举
 *
 * 注意：新增枚举值必须追加在末尾，保持 Major=0, Minor=1, Chromatic=2 不变，
 * 确保与已序列化的 DetectedKey 数据向后兼容。
 */
enum class Scale {
    Major = 0,          // 大调
    Minor = 1,          // 自然小调
    Chromatic = 2,      // 半音阶
    HarmonicMinor = 3,  // 和声小调
    Dorian = 4,         // 多利亚调式
    Mixolydian = 5,     // 混合利底亚调式
    PentatonicMajor = 6,// 大调五声音阶
    PentatonicMinor = 7 // 小调五声音阶
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

// ============================================================================
// ChromaKeyDetector
// ============================================================================

class ChromaKeyDetector {
public:
    ChromaKeyDetector();
    ~ChromaKeyDetector();

    /**
     * @brief 从音频 PCM 数据检测调式
     *
     * @param audio   音频样本数据（mono float）
     * @param numSamples 样本数量
     * @param sampleRate 采样率（Hz）
     * @return DetectedKey 检测到的调式信息
     *
     * 线程安全：每个线程应持有独立实例。
     * 不抛出异常。输入无效时返回默认 {C, Major, 0.0}。
     */
    DetectedKey detect(const float* audio, int numSamples, int sampleRate);

private:
    // ============================================================================
    // Chroma Extraction
    // ============================================================================

    /** 从音频计算全局 12 维 Chroma 向量 */
    void computeChroma(const float* audio, int numSamples, int sampleRate);

    // ============================================================================
    // Key Profile Matching
    // ============================================================================

    /** 用 Pearson 相关系数评分所有 key 模板，返回最佳匹配 */
    DetectedKey matchBestKey() const;

    /** 计算 Pearson 相关系数 */
    static float pearsonCorrelation(const std::array<float, 12>& a,
                                    const std::array<float, 12>& b);

    /** 将 profile 旋转到指定 root */
    static void rotateProfile(const std::array<float, 12>& base,
                              int semitones,
                              std::array<float, 12>& result);

    // ============================================================================
    // FFT Resources
    // ============================================================================

    static constexpr int kFftOrder = 12;           // 2^12 = 4096
    static constexpr int kFftSize = 4096;
    static constexpr int kHopSize = 2048;
    static constexpr float kMinFreqHz = 50.0f;
    static constexpr float kMaxFreqHz = 5000.0f;
    static constexpr double kMaxAnalysisDurationSec = 300.0;  // 5 min
    static constexpr double kTruncateAnalysisSec = 30.0;      // 截取首尾各 30 秒

    std::unique_ptr<juce::dsp::FFT> fft_;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window_;
    std::vector<float> fftBuffer_;
    std::array<float, 12> chroma_;

    // ============================================================================
    // Key Profiles
    // ============================================================================

    // Krumhansl-Schmuckler (1990) profiles
    static const std::array<float, 12> kKSMajorProfile;
    static const std::array<float, 12> kKSMinorProfile;

    // Temperley (2001) profiles
    static const std::array<float, 12> kTemperleyMajorProfile;
    static const std::array<float, 12> kTemperleyMinorProfile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChromaKeyDetector)
};

} // namespace OpenTune
