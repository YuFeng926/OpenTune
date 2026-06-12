#pragma once

#include "ContentKey.h"
#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/PitchShiftSettings.h"
#include "../DSP/ChromaKeyDetector.h"
#include "../DSP/ReferenceFeatures.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>

namespace OpenTune {

struct EditableContentSnapshot;

class ContentEditCommands
{
public:
    virtual ~ContentEditCommands() = default;

    virtual bool setNotes(ContentKey key,
                          std::vector<Note> notes) = 0;

    virtual bool commitNotesAndSegments(ContentKey key,
                                        std::vector<Note> notes,
                                        std::vector<CorrectedSegment> segments) = 0;

    virtual bool setCorrectedSegments(ContentKey key,
                                      std::vector<CorrectedSegment> segments) = 0;

    virtual bool setPitchCurve(ContentKey key,
                               std::shared_ptr<PitchCurve> curve) = 0;

    virtual bool setTimeGrid(ContentKey key,
                             std::shared_ptr<const TimeGridSnapshot> grid,
                             int64_t srcStartFrame,
                             int64_t srcEndFrame) = 0;

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

    virtual void enqueuePartialRender(ContentKey key,
                                      double relStartSeconds,
                                      double relEndSeconds) = 0;
};

} // namespace OpenTune
