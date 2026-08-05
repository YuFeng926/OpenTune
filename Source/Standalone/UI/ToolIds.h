#pragma once

namespace OpenTune {

enum class ToolId : int
{
    AutoTune = 0,
    Select = 1,
    DrawNote = 2,
    LineAnchor = 3,
    HandDraw = 4,
    // ⚡️ vocal-time-stretch §8.1 — Time tool for TimeGrid handle manipulation.
    // Mutually exclusive with all Note tools (per spec time-tool-interaction).
    TimeTool = 5,
    // OpenDyne 工具；F1 显示为 Main/Select 复用 Select，TimeTool 保持
    Pitch = 6,
    VolumeEnvelope = 7,
    Scissors = 8,
    // OpenDyne F2 sub-tools (Melodyne-style pitch editing)
    PitchModulation = 9,
    PitchDrift = 10
};

// Pitch Grid 全局开关：控制 Pitch 工具拖拽时的吸附行为（OpenDyne 模式）
enum class PitchGridMode : int {
    NoSnap = 0,      // 自由拖动，音分级精度
    Chromatic = 1,   // 吸附到最近半音
    KeyScale = 2     // 吸附到活动调式音阶（默认）
};

} // namespace OpenTune

