#pragma once

/**
 * ScaleUiMapping - 调式与 UI scaleType (1..8) 的唯一映射入口
 * 
 * 消除 Standalone 和 VST3 editor 中的并行映射结构，
 * 收口到唯一共享转换函数。
 */

#include "../DSP/ChromaKeyDetector.h"

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
 * 从 UI 参数构造 DetectedKey
 */
inline DetectedKey makeDetectedKeyFromUi(int rootNote, int scaleType, float confidence = 1.0f) {
    DetectedKey key;
    key.root = static_cast<Key>(juce::jlimit(0, 11, rootNote));
    key.scale = uiScaleTypeToScale(scaleType);
    key.confidence = confidence;
    return key;
}

} // namespace OpenTune
