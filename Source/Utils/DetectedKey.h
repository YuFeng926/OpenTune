#pragma once

#include <juce_core/juce_core.h>

namespace OpenTune {

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
 * Origin - 调式结果来源
 */
enum class Origin {
    Unset = 0,      // 未检测
    Automatic = 1,  // 自动检测
    Manual = 2      // 手动设置
};

/**
 * DetectedKey - 检测到的调式信息
 */
struct DetectedKey {
    Key root = Key::C;              // 主音
    Scale scale = Scale::Major;     // 调式
    float confidence = 0.0f;        // 置信度
    Origin origin = Origin::Unset;  // 结果来源

    static juce::String keyToString(Key key) {
        static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int idx = static_cast<int>(key);
        if (idx >= 0 && idx < 12)
            return names[idx];
        return "C";
    }

    static juce::String scaleToString(Scale scale) {
        static const char* names[] = {
            "Major", "Minor", "Chromatic", "Harmonic Minor",
            "Dorian", "Mixolydian", "Pentatonic Major", "Pentatonic Minor"
        };
        const int idx = static_cast<int>(scale);
        if (idx >= 0 && idx < static_cast<int>(sizeof(names) / sizeof(names[0])))
            return names[idx];
        return "Major";
    }

    // 旧持久化数据迁移：confidence==1 → Manual；0<confidence<1 → Automatic；其余全部 Unset
    static Origin originFromLegacyConfidence(float confidence) {
        if (confidence == 1.0f) return Origin::Manual;
        if (confidence > 0.0f && confidence < 1.0f) return Origin::Automatic;
        return Origin::Unset;
    }
};

} // namespace OpenTune
