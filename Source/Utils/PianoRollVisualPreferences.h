#pragma once

namespace OpenTune {

enum class NoteNameMode
{
    ShowAll = 0,
    COnly = 1,
    Hide = 2
};

enum class PianoGridStyle {
    PianoLanes = 0,    // 键槽几何：F0 标准音在两线之间
    EqualSpacing = 1   // 等距网格（纯几何：每行严格等距；F0标准音在线上）
};

// 纯逻辑视觉角色：只有亮/暗两种 palette，对应既有白键/黑键绘制材质。
// 重要：visual role 仅代表 palette，不决定几何。
enum class PitchRowVisualRole {
    WhiteKey,      // 亮色 palette：物理白键或调内
    BlackKey       // 暗色 palette：物理黑键或调外
};
// 背景纵向偏移（屏幕 y 向下为正）：仅由 gridStyle 决定。
// EqualSpacing: +0.5 * pixelsPerSemitone，让标准音高（lane 中央）落在网格线上；
// PianoLanes: 0，lane 填充与横向分隔线保持原位。
inline float laneBackgroundYOffset(PianoGridStyle gridStyle, float pixelsPerSemitone) noexcept {
    return (gridStyle == PianoGridStyle::EqualSpacing) ? (pixelsPerSemitone * 0.5f) : 0.0f;
}

// 左侧钢琴键盘可见性：由键盘开关控制，TimeTool 下始终隐藏。
inline bool shouldShowPianoKeys(bool showPianoKeyboard, bool isTimeView) noexcept {
    return showPianoKeyboard && !isTimeView;
}

inline bool isPhysicalBlackKey(int midiNote) noexcept {
    const int pitchClass = ((midiNote % 12) + 12) % 12;
    return pitchClass == 1 || pitchClass == 3 || pitchClass == 6
        || pitchClass == 8 || pitchClass == 10;
}

inline PitchRowVisualRole classifyPitchRow(bool showPianoKeyboard, bool scaleAssistEnabled,
                                           int midiNote, bool isPitchInScale) noexcept {
    if (!showPianoKeyboard && !scaleAssistEnabled)
        return PitchRowVisualRole::WhiteKey;
    if (!scaleAssistEnabled) {
        return isPhysicalBlackKey(midiNote) ? PitchRowVisualRole::BlackKey : PitchRowVisualRole::WhiteKey;
    }
    return isPitchInScale ? PitchRowVisualRole::WhiteKey : PitchRowVisualRole::BlackKey;
}

struct PianoRollVisualPreferences {
    NoteNameMode noteNameMode = NoteNameMode::COnly;
    bool showUnvoicedFrames = false;
    bool showPianoKeyboard = true;
    bool scaleAssistEnabled = false;
    float backgroundBrightness = 1.0f; // 0.0=纯黑, 1.0=当前默认, 2.0=高亮
};

} // namespace OpenTune
