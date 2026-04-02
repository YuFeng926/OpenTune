#pragma once

#include <juce_graphics/juce_graphics.h>

namespace OpenTune {
namespace DarkBlueGrey {

    // 深蓝灰主题配色 - 现代、清爽、线条明快
    // 目标特征：
    // - 深色基底但不过黑，降低刺眼高对比
    // - 阴影更扩散、更柔和（以"空气感"塑造层级），避免厚重"黑边"
    // - 控件以细边框+轻渐变体现立体感，不走强拟物
    struct Colors
    {
        // 主强调色 - 柔和蓝色系
        static const juce::uint32 PrimaryBlue    = 0xFF60A5FA; // 柔和蓝（用于较大面积的强调）
        static const juce::uint32 Accent         = 0xFF3B82F6; // Electric Blue（更聚焦、更科技）
        static const juce::uint32 LightBlue      = 0xFF93C5FD; // 轻亮蓝（高光/hover）
        static const juce::uint32 DarkBlue       = 0xFF2563EB; // 深强调（按下/聚焦）

        // 背景层级（由深到浅）
        static const juce::uint32 BackgroundDark   = 0xFF1A212B;
        static const juce::uint32 BackgroundMedium = 0xFF222B36;
        static const juce::uint32 BackgroundLight  = 0xFF2B3643;

        // 渐变背景（非常克制）
        static const juce::uint32 GradientTop    = 0xFF273241;
        static const juce::uint32 GradientBottom = 0xFF171D27;

        // UI元素（边框与控件）
        static const juce::uint32 PanelBorder   = 0xFF3C4A5A;
        static const juce::uint32 ButtonNormal  = 0xFF2B3643;
        static const juce::uint32 ButtonHover   = 0xFF334253;
        static const juce::uint32 ButtonPressed = 0xFF1B2430;

        // 3D效果（用于少量控件细节）
        static const juce::uint32 BevelLight = 0xFF3E4D5E;
        static const juce::uint32 BevelDark  = 0xFF0B111B;
        static const juce::uint32 GlowColor  = 0xFF60A5FA;

        // 文字层级
        static const juce::uint32 TextPrimary   = 0xFFE6EDF5;
        static const juce::uint32 TextSecondary = 0xFFA8B4C3;
        static const juce::uint32 TextDisabled  = 0xFF6F7C8C;
        static const juce::uint32 TextHighlight = 0xFF60A5FA;

        // 钢琴卷帘（更暗一点，突出编辑内容）
        static const juce::uint32 RollBackground = 0xFF141A22;
        static const juce::uint32 LaneC          = 0xFF1D2631;
        static const juce::uint32 LaneOther      = 0xFF171F29;
        static const juce::uint32 GridLine       = 0xFF2B3643;

        // 音高曲线
        static const juce::uint32 OriginalF0   = 0xFFE53935; // 正红 (Material Red 600)
        static const juce::uint32 CorrectedF0  = 0xFF60A5FA; // 柔蓝
        static const juce::uint32 ShadowTrack  = 0x4060A5FA;

        // 音符块（以"边框强调"为主，填充克制）
        static const juce::uint32 NoteBlock        = 0xFF60A5FA;
        static const juce::uint32 NoteBlockBorder  = 0xFF3B82F6;
        static const juce::uint32 NoteBlockSelected = 0xFF93C5FD;
        static const juce::uint32 NoteBlockHover   = 0xFF93C5FD;

        // 播放头与时间线
        static const juce::uint32 Playhead       = 0xFFE6EDF5;
        static const juce::uint32 TimelineMarker = 0xFF60A5FA;
        static const juce::uint32 BeatMarker     = 0xFF3C4A5A;

        // 工具
        static const juce::uint32 ToolActive    = 0xFF60A5FA;
        static const juce::uint32 ToolInactive  = 0xFF3C4A5A;
        static const juce::uint32 ButtonInactive = 0xFF2B3643;

        // 状态
        static const juce::uint32 StatusProcessing  = 0xFFEAB308;
        static const juce::uint32 StatusReady       = 0xFF34D399;
        static const juce::uint32 StatusError       = 0xFFF87171;

        // 波形
        static const juce::uint32 WaveformFill    = 0x403B82F6;
        static const juce::uint32 WaveformOutline = 0xFF60A5FA;

        // 音阶
        static const juce::uint32 ScaleHighlight = 0x60FBBF24;

        // 旋钮
        static const juce::uint32 KnobBody      = 0xFF0F141B;
        static const juce::uint32 KnobIndicator = 0xFFE6EDF5;

        // VU表颜色 - 深色背景下更清晰（尽量克制饱和度）
        static const juce::uint32 VULow  = 0xFF2DD4BF; // 轻青绿
        static const juce::uint32 VUMid  = 0xFF60A5FA; // 柔和蓝
        static const juce::uint32 VUHigh = 0xFFFBBF24; // 暖黄
        static const juce::uint32 VUClip = 0xFFF87171; // 柔红

        // 时间显示颜色 - 深色底上的高可读
        static const juce::uint32 TimeActive   = 0xFFE6EDF5;
        static const juce::uint32 TimeInactive = 0xFF7E8A99;
    };

    struct Style
    {
        // 圆角设置 - Unified with BlueBreeze
        static constexpr float PanelRadius   = 16.0f;  // Unified with BlueBreeze
        static constexpr float ControlRadius = 10.0f;  // Unified with BlueBreeze
        static constexpr float FieldRadius   = 10.0f;  // Unified with BlueBreeze
        static constexpr float KnobRadius    = 999.0f; // 圆形旋钮

        // 线宽设置 - Unified with BlueBreeze
        static constexpr float StrokeThin         = 1.2f;  // 保持细描边但满足可见性下限
        static constexpr float StrokeThick        = 2.0f;  // Unified with BlueBreeze
        static constexpr float FocusRingThickness = 2.0f;  // 聚焦环厚度

        // 阴影设置 - Unified with BlueBreeze
        static constexpr float ShadowAlpha  = 0.25f; // Unified with BlueBreeze
        static constexpr int   ShadowRadius = 16;    // 维持层次感，避免阴影过薄
        static constexpr int   ShadowOffsetX = 0;    // Unified with BlueBreeze
        static constexpr int   ShadowOffsetY = 5;    // 保持轻微浮起感

        // 发光效果（深蓝灰主题不使用发光）
        static constexpr float GlowAlpha  = 0.0f;
        static constexpr float GlowRadius = 0.0f;

        // 3D Bevel 设置
        static constexpr float BevelWidth     = 1.0f;  // 轻微倒角（用于少量控件）
        static constexpr float BevelIntensity = 0.12f; // 更克制的浮雕强度

        // 动画设置
        static constexpr float AnimationDurationMs = 120.0f;
        static constexpr float HoverGlowIntensity  = 0.3f;

        // 全局圆角 - Unified with BlueBreeze
        static constexpr float CornerRadius = 16.0f; // Unified with BlueBreeze
    };

} // namespace DarkBlueGrey
} // namespace OpenTune
