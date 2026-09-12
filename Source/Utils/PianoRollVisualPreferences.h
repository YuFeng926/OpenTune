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

// 钢琴卷帘 lane 视觉模式：两模式互斥，统一使用亮/暗两种键槽视觉。
// PianoKeys: 按物理黑白键分类——WhiteKey=亮, BlackKey=暗。
// ScaleAssist: 按调内/调外分类——调内→WhiteKey(亮), 调外→BlackKey(暗)。
//   中央 lane 与左侧键盘均复用既有白键/黑键绘制实现，无独立渲染路径。
//   Chromatic 所有半音都在调内，全部映射为 WhiteKey(亮)。
enum class PitchLaneVisualMode {
    ScaleAssist = 0,  // 音阶辅助模式（未勾选）
    PianoKeys = 1     // 钢琴键槽模式（勾选，默认）
};

// 纯逻辑视觉角色：只有亮/暗两种 palette，对应既有白键/黑键绘制材质。
// 重要：visual role 仅代表 palette，不决定几何。
enum class PitchRowVisualRole {
    WhiteKey,      // 亮色 palette（PianoKeys: 物理白键；ScaleAssist: 调内）
    BlackKey       // 暗色 palette（PianoKeys: 物理黑键；ScaleAssist: 调外）
};

// 纯逻辑、可测试的视觉角色分类。
// 两种模式都只返回 WhiteKey 或 BlackKey。
// PianoKeys: 按物理黑白键分类，忽略 isPitchInScale。
// ScaleAssist: 按调内/调外分类，isPitchInScale=true→WhiteKey, false→BlackKey。
// Chromatic 调用方传 isPitchInScale=true，因此全部映射为 WhiteKey。
//
// 注意：classifyPitchRow 返回的是视觉角色（visualRole），仅用于选择颜色/亮度 palette。
// drawPianoKeys 的左侧键盘几何（full-width white rect / black-key extension / short
// black-key body）永远由物理 MIDI pitch class 决定，与 visualRole 无关。
inline PitchRowVisualRole classifyPitchRow(PitchLaneVisualMode mode, int midiNote,
                                            bool isPitchInScale) noexcept {
    if (mode == PitchLaneVisualMode::PianoKeys) {
        const int noteInOctave = ((midiNote % 12) + 12) % 12;
        const bool isBlack = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
                              noteInOctave == 8 || noteInOctave == 10);
        return isBlack ? PitchRowVisualRole::BlackKey : PitchRowVisualRole::WhiteKey;
    }
    // ScaleAssist: 调内→WhiteKey(亮), 调外→BlackKey(暗)
    return isPitchInScale ? PitchRowVisualRole::WhiteKey : PitchRowVisualRole::BlackKey;
}

struct PianoRollVisualPreferences {
    NoteNameMode noteNameMode = NoteNameMode::COnly;
    bool showUnvoicedFrames = false;
    float backgroundBrightness = 1.0f; // 0.0=纯黑, 1.0=当前默认, 2.0=高亮
};

} // namespace OpenTune
