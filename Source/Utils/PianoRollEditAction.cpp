#include "PianoRollEditAction.h"
#include "Content/ContentEditCommands.h"
#include <limits>

namespace OpenTune {

PianoRollEditAction::PianoRollEditAction(std::shared_ptr<ContentEditCommands> commands,
                                         ContentKey key,
                                         juce::String description,
                                         std::vector<Note> beforeNotesInRange,
                                         std::vector<Note> afterNotesInRange,
                                         std::vector<PitchCorrectionSegment> beforeSegments,
                                         std::vector<PitchCorrectionSegment> afterSegments,
                                         ContentEditRangeFrames affectedRange,
                                         std::vector<float> beforeOriginalF0InRange,
                                         std::vector<float> afterOriginalF0InRange,
                                         std::vector<F0FrameRange> beforeErasedRanges,
                                         std::vector<F0FrameRange> afterErasedRanges)
    : commands_(commands)
    , contentKey_(key)
    , description_(std::move(description))
    , beforeNotes_(std::move(beforeNotesInRange))
    , afterNotes_(std::move(afterNotesInRange))
    , beforeSegments_(std::move(beforeSegments))
    , afterSegments_(std::move(afterSegments))
    , beforeOriginalF0_(std::move(beforeOriginalF0InRange))
    , afterOriginalF0_(std::move(afterOriginalF0InRange))
    , beforeErasedRanges_(std::move(beforeErasedRanges))
    , afterErasedRanges_(std::move(afterErasedRanges))
    , affectedRange_(affectedRange)
{
    jassert(affectedRange_.startFrame >= 0);
    jassert(affectedRange_.endFrameExclusive >= affectedRange_.startFrame);
}

void PianoRollEditAction::undo()
{
    if (!beforeOriginalF0_.empty() || hasErasedRangePatch_)
        commands_->commitNotesAndSegmentsWithOriginalF0(contentKey_, beforeNotes_, beforeSegments_, affectedRange_, beforeOriginalF0_, beforeErasedRanges_);
    else
        commands_->commitNotesAndSegments(contentKey_, beforeNotes_, beforeSegments_, affectedRange_);
}

void PianoRollEditAction::redo()
{
    if (!afterOriginalF0_.empty() || hasErasedRangePatch_)
        commands_->commitNotesAndSegmentsWithOriginalF0(contentKey_, afterNotes_, afterSegments_, affectedRange_, afterOriginalF0_, afterErasedRanges_);
    else
        commands_->commitNotesAndSegments(contentKey_, afterNotes_, afterSegments_, affectedRange_);
}

} // namespace OpenTune
