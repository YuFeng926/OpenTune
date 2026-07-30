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

class PitchShiftEditAction;
struct EditableContentSnapshot;

using ContentCommitSnapshot = std::shared_ptr<const EditableContentSnapshot>;

struct ContentEditRangeFrames {
    int startFrame{0};
    int endFrameExclusive{0};
};

struct ContentEditRangeSeconds {
    double startSeconds{0.0};
    double endSeconds{0.0};
};

struct ContentNoteRangePatch {
    ContentEditRangeSeconds affectedRange;
    std::vector<Note> afterNotesInRange;
};

class ContentEditCommands
{
public:
    virtual ~ContentEditCommands() = default;

    virtual bool replaceContentNotesForFullMutation(ContentKey key,
                                                     std::vector<Note> notes) = 0;

    virtual ContentCommitSnapshot commitNotePatch(ContentKey key,
                                                   ContentNoteRangePatch patch) = 0;

    virtual ContentCommitSnapshot commitNotesAndSegments(
        ContentKey key,
        std::vector<Note> notes,
        std::vector<PitchCorrectionSegment> segments,
        ContentEditRangeFrames affectedRange) = 0;

    virtual bool setPitchCurve(ContentKey key,
                               std::shared_ptr<PitchCurve> curve,
                               ContentEditRangeFrames affectedRange) = 0;

    virtual bool setTimeGrid(ContentKey key,
                             std::shared_ptr<const TimeGridSnapshot> grid) = 0;

    virtual bool setDetectedKey(ContentKey key,
                                const DetectedKey& detectedKey) = 0;

    virtual bool applyPitchShiftState(ContentKey key,
                                      const PitchShiftEditState& state) = 0;

    virtual std::unique_ptr<PitchShiftEditAction> commitPitchShiftEdit(
        ContentKey key,
        const PitchShiftSettings& newSettings) = 0;

    virtual bool commitAutoTuneGeneratedNotes(ContentKey key,
                                               std::vector<Note> generatedNotes,
                                               int startFrame,
                                               int endFrameExclusive,
                                               float retuneSpeed,
                                               float vibratoDepth,
                                               float vibratoRate) = 0;
};

} // namespace OpenTune
