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
                                         std::vector<ContentEditRangeFrames> affectedRanges,
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
    , affectedRanges_(std::move(affectedRanges))
{
    jassert(!affectedRanges_.empty());
    for (const auto& range : affectedRanges_) {
        jassert(range.startFrame >= 0);
        jassert(range.endFrameExclusive >= range.startFrame);
    }
    jassert((beforeOriginalF0_.empty() && afterOriginalF0_.empty() && !hasErasedRangePatch_)
            || affectedRanges_.size() == 1);
}

void PianoRollEditAction::undo()
{
    if (!beforeOriginalF0_.empty() || hasErasedRangePatch_)
        commands_->commitNotesAndSegmentsWithOriginalF0(contentKey_, beforeNotes_, beforeSegments_, affectedRanges_.front(), beforeOriginalF0_, beforeErasedRanges_);
    else
        commands_->commitNotesAndSegmentsInRanges(contentKey_, beforeNotes_, beforeSegments_, affectedRanges_);
}

void PianoRollEditAction::redo()
{
    if (!afterOriginalF0_.empty() || hasErasedRangePatch_)
        commands_->commitNotesAndSegmentsWithOriginalF0(contentKey_, afterNotes_, afterSegments_, affectedRanges_.front(), afterOriginalF0_, afterErasedRanges_);
    else
        commands_->commitNotesAndSegmentsInRanges(contentKey_, afterNotes_, afterSegments_, affectedRanges_);
}

} // namespace OpenTune
