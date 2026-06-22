#pragma once

#include "PianoRollRenderer.h"
#include "Content/ContentKey.h"
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "Utils/TimeGrid.h"
#include "Utils/ContentTimelineProjection.h"
#include "UI/WaveformMipmap.h"
#include <vector>
#include <cstdint>
#include <memory>

namespace OpenTune {

struct PianoRollRenderSnapshot {
    ContentKey contentKey;
    double pixelsPerSecond = 100.0;
    int scrollOffsetPx = 0;
    int contentStartX = 60;
    float pixelsPerSemitone = 15.0f;
    float verticalScrollOffset = 0.0f;
    float maxMidi = 108.0f;
    double bpm = 120.0;

    std::vector<Note> notes;
    std::vector<float> originalF0;
    // correctedF0 removed: PitchCurveSnapshot stores corrected data as segments, not a flat vector.
    // Use pitchSnapshot->renderCorrectedOnlyRange() or correctedSegments for rendering.
    std::vector<CorrectedSegment> correctedSegments;
    std::shared_ptr<const PitchCurveSnapshot> pitchSnapshot;
    F0Timeline f0Timeline;
    WaveformLevelSnapshot waveformLevel;
    std::vector<double> chunkBoundaries;
    std::shared_ptr<const TimeGridSnapshot> timeGridSnapshot;

    ContentTimelineProjection activeProjection;

    bool showWaveform = true;
    bool showLanes = true;
    bool showOriginalF0 = true;
    bool showCorrectedF0 = true;
    bool showChunkBoundaries = false;
    bool showUnvoicedFrames = false;

    int scaleRootNote = 0;
    int scaleType = 1;
    NoteNameMode noteNameMode = NoteNameMode::COnly;

    uint64_t pitchEpoch = 0;
    uint64_t notesEpoch = 0;
    uint64_t waveformRevision = 0;
    uint64_t timeGridRevision = 0;
    uint64_t visualPrefsRevision = 0;
    uint64_t placementProjectionRevision = 0;

    ToolId currentTool = ToolId::Select;
    uint32_t themeId = 0;
};

} // namespace OpenTune
