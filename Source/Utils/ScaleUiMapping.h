#pragma once

/**
 * ScaleUiMapping - 调式与 UI scaleType (1..8) 的唯一映射入口
 * 
 * 消除 Standalone 和 VST3 editor 中的并行映射结构，
 * 收口到唯一共享转换函数。
 */

#include "DetectedKey.h"
#include "NoteGeneratorTypes.h"
#include <array>
#include <optional>

namespace OpenTune {

/**
 * Scale 枚举 → UI scaleType (1..8)
 */
inline int scaleToUiScaleType(Scale scale) {
    switch (scale) {
        case Scale::Major:           return 1;
        case Scale::Minor:           return 2;
        case Scale::Chromatic:       return 3;
        case Scale::HarmonicMinor:   return 4;
        case Scale::Dorian:          return 5;
        case Scale::Mixolydian:      return 6;
        case Scale::PentatonicMajor: return 7;
        case Scale::PentatonicMinor: return 8;
    }
    return 1; // unreachable, but compiler warning suppression
}

/**
 * UI scaleType (1..8) → Scale 枚举
 */
inline Scale uiScaleTypeToScale(int scaleType) {
    switch (scaleType) {
        case 1: return Scale::Major;
        case 2: return Scale::Minor;
        case 3: return Scale::Chromatic;
        case 4: return Scale::HarmonicMinor;
        case 5: return Scale::Dorian;
        case 6: return Scale::Mixolydian;
        case 7: return Scale::PentatonicMajor;
        case 8: return Scale::PentatonicMinor;
    }
    return Scale::Major; // unreachable
}

/**
 * UI scaleType (1..8) → ScaleMode 枚举
 */
inline ScaleMode uiScaleTypeToScaleMode(int scaleType) {
    switch (scaleType) {
        case 1: return ScaleMode::Major;
        case 2: return ScaleMode::Minor;
        case 3: return ScaleMode::Chromatic;
        case 4: return ScaleMode::HarmonicMinor;
        case 5: return ScaleMode::Dorian;
        case 6: return ScaleMode::Mixolydian;
        case 7: return ScaleMode::PentatonicMajor;
        case 8: return ScaleMode::PentatonicMinor;
        default: return ScaleMode::Major;
    }
}

/**
 * 从 UI 参数构造 DetectedKey（手动设置入口）
 */
inline DetectedKey makeDetectedKeyFromUi(int rootNote, int scaleType) {
    DetectedKey key;
    key.root = static_cast<Key>(juce::jlimit(0, 11, rootNote));
    key.scale = uiScaleTypeToScale(scaleType);
    key.confidence = 1.0f;
    key.origin = Origin::Manual;
    return key;
}

/**
 * UI scaleType (1..8) + rootNote → ScaleSnapConfig（AUTO/Pitch 工具唯一映射入口）。
 * Chromatic（scaleType==3）返回 nullopt，表示不做音阶吸附。
 */
inline std::optional<ScaleSnapConfig> makeScaleSnapConfigFromUi(int rootNote, int scaleType) {
    if (scaleType == 3) // chromatic：无音阶吸附
        return std::nullopt;
    ScaleSnapConfig snapCfg;
    snapCfg.root = juce::jlimit(0, 11, rootNote);
    snapCfg.mode = uiScaleTypeToScaleMode(scaleType);
    return snapCfg;
}

/**
 * DetectedKey → ScaleSnapConfig（F0 完成链整段 AUTO 唯一映射入口）。
 * Chromatic 或未知 scale 返回 nullopt，表示不做音阶吸附。
 */
inline std::optional<ScaleSnapConfig> makeScaleSnapConfig(const DetectedKey& key) {
    return makeScaleSnapConfigFromUi(static_cast<int>(key.root), scaleToUiScaleType(key.scale));
}

/**
 * 音阶 pitch-class mask：返回 12 元素 bool 数组，true 表示该 pitch class 在调内。
 * Chromatic (scaleType==3) 返回全 true（所有半音都在调内）。
 * 中央 lane 与左轴共享此 helper，消除 TimelineLayerComposer / PianoRollRenderer 重复。
 *
 * 视觉策略注意：Chromatic 的 mask 全 true，classifyPitchRow 收到 isPitchInScale=true
 * 后统一返回 WhiteKey（亮）。ScaleAssist 的调内/调外直接映射到 WhiteKey/BlackKey，
 * 不再拥有独立绘制路径。
 */
inline std::array<bool, 12> buildInScalePitchClasses(int scaleType, int rootNote) noexcept {
    return buildPitchClassMask(uiScaleTypeToScaleMode(scaleType), rootNote);
}

} // namespace OpenTune
