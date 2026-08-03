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
    // Phase E adds the enum value + key shortcut + InteractionState mode flag;
    // Phase F adds the full ToolHandler / Renderer wiring + UI affordances.
    TimeTool = 5,
    // OpenDyne 工具；F1 显示为 Main/Select 复用 Select，TimeTool 保持
    Pitch = 6,
    VolumeEnvelope = 7,
    Scissors = 8
};

} // namespace OpenTune

