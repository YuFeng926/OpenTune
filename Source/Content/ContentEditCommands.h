#pragma once

#include "ContentKey.h"
#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../DSP/ChromaKeyDetector.h"
#include "../DSP/ReferenceFeatures.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

struct EditableContentSnapshot;

// ── Hard-cut render mutation contract ────────────────────────────────────────
//
// Two distinct sinks separate local edits (where the caller knows the precise
// affected frame range) from whole-content rebuilds (where the entire content
// must be re-rendered). The hard cut lives in the sink dispatch, not in the
// individual mutation methods: local edits never trigger a whole-content
// rebuild, full rebuilds never try to be clever with ranges.

struct ContentEditRangeFrames {
    int startFrame{0};
    int endFrameExclusive{0};
};

enum class FullRenderReason : uint8_t {
    Import,
    SourceReplacement,
    ProjectRestore,
    CaptureRestore,
    GlobalPitchShift,
    GlobalTimeGrid,
    ModelOrSettingsWholeContentRerender
};

class ContentEditCommands
{
public:
    virtual ~ContentEditCommands() = default;

    virtual bool setNotes(ContentKey key,
                          std::vector<Note> notes) = 0;

    virtual bool commitNotesAndSegments(ContentKey key,
                                        std::vector<Note> notes,
                                        std::vector<CorrectedSegment> segments,
                                        ContentEditRangeFrames affectedRange) = 0;

    virtual bool setCorrectedSegments(ContentKey key,
                                      std::vector<CorrectedSegment> segments) = 0;

    virtual bool setPitchCurve(ContentKey key,
                               std::shared_ptr<PitchCurve> curve,
                               ContentEditRangeFrames affectedRange) = 0;

    virtual bool setTimeGrid(ContentKey key,
                             std::shared_ptr<const TimeGridSnapshot> grid) = 0;

    virtual bool setDetectedKey(ContentKey key,
                                const DetectedKey& detectedKey) = 0;

    virtual bool setPitchShiftSettings(ContentKey key,
                                       const PitchShiftSettings& settings) = 0;

    virtual bool commitAutoTuneGeneratedNotes(ContentKey key,
                                              std::vector<Note> generatedNotes,
                                              int startFrame,
                                              int endFrameExclusive,
                                              float retuneSpeed,
                                              float vibratoDepth,
                                              float vibratoRate,
                                              double audioSampleRate) = 0;
};

} // namespace OpenTune
