#pragma once

namespace OpenTune {

enum class NoteNameMode
{
    ShowAll = 0,
    COnly = 1,
    Hide = 2
};

enum class ReferenceVisualization
{
    F0Curve = 0,
    Off = 1
};

struct PianoRollVisualPreferences {
    NoteNameMode noteNameMode = NoteNameMode::COnly;
    ReferenceVisualization referenceVisualization = ReferenceVisualization::F0Curve;
    bool showChunkBoundaries = false;
    bool showUnvoicedFrames = false;
};

} // namespace OpenTune
