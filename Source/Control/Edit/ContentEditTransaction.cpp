// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContentEditTransaction.h"

#include "Content/EditableContentSnapshot.h"
#include "PluginProcessor.h"
#include "Utils/PianoRollEditAction.h"

#include <algorithm>

namespace OpenTune {

ContentEditTransaction::ContentEditTransaction(OpenTuneAudioProcessor& processor, ContentKey key)
    : processor_(processor)
    , key_(key)
{}

bool ContentEditTransaction::captureBefore()
{
    // PianoRollComponent::captureBeforeUndoSnapshot(): the committed notes and the correction segments of
    // the curve, here read from the content's snapshot (the piano roll's caches mirror it).
    const auto snapshot = processor_.getContentSnapshot(key_);
    captured_ = snapshot != nullptr && snapshot->pitchCurve != nullptr;
    if (!captured_)
        return false;
    beforeNotes_ = snapshot->notes;
    beforeSegments_ = snapshot->pitchCurve->getCorrectionSegments();
    return true;
}

bool ContentEditTransaction::recordRange(const juce::String& description, F0FrameRange affectedRange)
{
    if (!captured_)
        return false;
    captured_ = false;
    const auto snapshot = processor_.getContentSnapshot(key_);
    if (snapshot == nullptr || snapshot->pitchCurve == nullptr)
        return false;
    // PianoRollComponent::recordUndoAction(): the frame range in seconds on the content's analysis timeline,
    // then the notes and segments of that range before and after, in one PianoRollEditAction.
    const auto& curve = *snapshot->pitchCurve;
    const F0Timeline f0tl{curve.getHopSize(), curve.getSampleRate(), static_cast<int>(curve.size())};
    const double rangeStartSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.startFrame);
    const double rangeEndSec = f0tl.isEmpty() ? 0.0 : f0tl.timeAtFrame(affectedRange.endFrameExclusive);

    auto action = std::make_unique<PianoRollEditAction>(
        processor_.getContentCommands(), key_, description, notesInRange(beforeNotes_, rangeStartSec, rangeEndSec),
        notesInRange(snapshot->notes, rangeStartSec, rangeEndSec),
        segmentsInRange(beforeSegments_, affectedRange.startFrame, affectedRange.endFrameExclusive),
        segmentsInRange(curve.getCorrectionSegments(), affectedRange.startFrame, affectedRange.endFrameExclusive),
        ContentEditRangeFrames{affectedRange.startFrame, affectedRange.endFrameExclusive});
    ControlOriginAction::Inner inner;
    inner.action = std::move(action);
    inner.name = "PianoRollEditAction";
    inner.content = key_;
    inner.aspect = ControlContentAspect::NotesAndPitch;
    actions_.push_back(std::move(inner));
    beforeNotes_.clear();
    beforeSegments_.clear();
    return true;
}

std::vector<ControlOriginAction::Inner> ContentEditTransaction::takeActions()
{
    return std::move(actions_);
}

std::vector<Note> ContentEditTransaction::notesInRange(const std::vector<Note>& notes, double startSec, double endSec)
{
    std::vector<Note> result;
    for (const auto& note : notes)
        if (note.startTime < endSec && note.endTime > startSec)
            result.push_back(note);
    return result;
}

std::vector<PitchCorrectionSegment>
ContentEditTransaction::segmentsInRange(const std::vector<PitchCorrectionSegment>& segments, int startFrame,
                                        int endFrameExclusive)
{
    std::vector<PitchCorrectionSegment> result;
    for (const auto& segment : segments) {
        if (segment.endFrame <= startFrame || segment.startFrame >= endFrameExclusive)
            continue;
        // A segment that crosses an edge of the range keeps its part inside the range.
        const int clipStart = std::max(segment.startFrame, startFrame);
        const int clipEnd = std::min(segment.endFrame, endFrameExclusive);
        if (clipEnd <= clipStart)
            continue;
        const int startOffset = clipStart - segment.startFrame;
        const int clipLength = clipEnd - clipStart;
        if (startOffset >= 0 && clipLength > 0 && startOffset + clipLength <= static_cast<int>(segment.f0Data.size())) {
            PitchCorrectionSegment clipped = segment;
            clipped.startFrame = clipStart;
            clipped.endFrame = clipEnd;
            clipped.f0Data.assign(segment.f0Data.begin() + startOffset,
                                  segment.f0Data.begin() + startOffset + clipLength);
            result.push_back(std::move(clipped));
        }
    }
    return result;
}

} // namespace OpenTune
