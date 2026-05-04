#pragma once

namespace OpenTune {

enum class NoteNameMode
{
    ShowAll = 0,
    COnly = 1,
    Hide = 2
};

struct PianoRollVisualPreferences {
    NoteNameMode noteNameMode = NoteNameMode::COnly;
    bool showChunkBoundaries = false;
    bool showUnvoicedFrames = false;
};

} // namespace OpenTune
