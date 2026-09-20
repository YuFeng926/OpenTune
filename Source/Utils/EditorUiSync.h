#pragma once

#include <cmath>
#include <cstdint>

namespace OpenTune {

// Pure UI decisions shared by the Standalone and Plugin editors.
// No component access, no state, no timer ordering.

// ---------------------------------------------------------------------------
// UI zoom
// ---------------------------------------------------------------------------

struct EditorUiZoomDecision {
    bool changed = false;
    int appliedPercent = 100;
    int minWidth = 0;
    int minHeight = 0;
};

// Compares the processor zoom (single source of truth) against the zoom this
// editor has already applied. On change it derives the new min size with the
// existing policy: scale = static_cast<float>(percent) / 100.0f, then
// ceil(base * scale). Callers keep their own max/window/resized/repaint steps.
inline EditorUiZoomDecision resolveEditorUiZoomDecision(int processorZoomPercent,
                                                        int appliedZoomPercent,
                                                        int baseMinWidth,
                                                        int baseMinHeight) noexcept
{
    EditorUiZoomDecision decision;
    decision.appliedPercent = appliedZoomPercent;

    if (processorZoomPercent == appliedZoomPercent)
        return decision;

    decision.changed = true;
    decision.appliedPercent = processorZoomPercent;

    const float uiZoomScale = static_cast<float>(processorZoomPercent) / 100.0f;
    decision.minWidth = static_cast<int>(std::ceil(baseMinWidth * uiZoomScale));
    decision.minHeight = static_cast<int>(std::ceil(baseMinHeight * uiZoomScale));
    return decision;
}

// ---------------------------------------------------------------------------
// Content revision pulse
// ---------------------------------------------------------------------------

struct ContentRevisionPulse {
    bool notesChanged = false;
    bool timeGridChanged = false;
    bool pitchChanged = false;

    uint64_t nextNotesRevision = 0;
    uint64_t nextTimeGridRevision = 0;
    uint64_t nextPitchRevision = 0;
};

// Pure compare of the three content revisions against the last seen values.
// Returns the changed flags plus the next baseline values.
inline ContentRevisionPulse resolveContentRevisionPulse(uint64_t currentNotesRevision,
                                                        uint64_t currentTimeGridRevision,
                                                        uint64_t currentPitchRevision,
                                                        uint64_t lastNotesRevision,
                                                        uint64_t lastTimeGridRevision,
                                                        uint64_t lastPitchRevision) noexcept
{
    ContentRevisionPulse pulse;
    pulse.nextNotesRevision = currentNotesRevision;
    pulse.nextTimeGridRevision = currentTimeGridRevision;
    pulse.nextPitchRevision = currentPitchRevision;
    pulse.notesChanged = currentNotesRevision != lastNotesRevision;
    pulse.timeGridChanged = currentTimeGridRevision != lastTimeGridRevision;
    pulse.pitchChanged = currentPitchRevision != lastPitchRevision;
    return pulse;
}

} // namespace OpenTune
