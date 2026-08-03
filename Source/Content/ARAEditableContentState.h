#pragma once
#include "../Utils/Note.h"
#include "../Utils/OutputGainEnvelope.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include <vector>
#include <memory>
#include <cstdint>

namespace OpenTune {

// ARA AudioModification editable state: user-editable modification data only.
// Per ARA2 spec: AudioModification does NOT own source PCM (comes from AudioSource).
// Analysis state (F0, detectedKey, referenceFeatures) belongs in AnalysisState, not here.
struct ARAEditableContentState
{
    // Musical content (user-editable modification data)
    std::vector<Note> notes;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    PitchShiftSettings pitchShiftSettings;
    SibilantGainEnvelope sibilantGainEnvelope;
    
    // Revisions
    uint64_t notesRevision{0};
    uint64_t pitchRevision{0};
    uint64_t timeGridRevision{0};
    uint64_t pitchShiftRevision{0};
    uint64_t outputGainRevision{0};
    uint64_t contentRevision{0};
};

} // namespace OpenTune
