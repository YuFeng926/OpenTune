#pragma once

namespace OpenTune {

enum class NoteNameMode
{
    ShowAll = 0,
    COnly = 1,
    Hide = 2
};

enum class PianoGridStyle {
    PianoLanes = 0,    // 钢琴键槽网格（黑白键上色，F0标准音在两线之间）
    EqualSpacing = 1   // 等距网格（均匀无上色，F0标准音在线上）
};

struct PianoRollVisualPreferences {
    NoteNameMode noteNameMode = NoteNameMode::COnly;
    bool showUnvoicedFrames = false;
};

} // namespace OpenTune
