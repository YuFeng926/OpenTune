// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlOriginAction.h"

#include "Content/ContentKey.h"
#include "Utils/F0Timeline.h"
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"

#include <vector>

namespace OpenTune {

class OpenTuneAudioProcessor;

/**
 * Builds the undo entries of a content edit from snapshots, the way the piano roll builds them around a
 * commit of OpenTune's content commands, without the piano roll (PianoRollComponent stays untouched):
 *
 * 1. captureBefore() takes the notes and the correction segments of the content's current snapshot, as
 *    PianoRollComponent::captureBeforeUndoSnapshot() takes them from the piano roll's caches;
 * 2. the caller commits through the processor's command (for AUTO OpenTuneAudioProcessor::
 *    autoTuneContentRangeByContentKey(), what ContentEditCommands::autoTuneContentRange() calls);
 * 3. recordRange() keeps, for the affected frame range, the notes and segments before and after, cut to the
 *    range, in a PianoRollEditAction with the UI's description, as PianoRollComponent::recordUndoAction()
 *    does.
 *
 * Several commits of one call give several PianoRollEditActions; the caller wraps them in one
 * ControlOriginAction, so that one call is one undo step. The entries use the processor's own
 * ContentEditCommands, the instance the UI's piano roll uses, so undoing them in the UI behaves the same.
 * The mirrored upstream functions are listed in Source/Control/README.md (ContentEditTransaction).
 *
 * Threading: message thread only.
 */
class ContentEditTransaction
{
public:
    ContentEditTransaction(OpenTuneAudioProcessor& processor, ContentKey key);

    /** Captures the notes and correction segments before the next commit. False without a snapshot with a
        pitch curve (nothing can be committed then). */
    bool captureBefore();

    /** After a successful commit: the undo action of the range, from the captured state and the committed
        snapshot. False when nothing was captured or the content has no snapshot any more. */
    bool recordRange(const juce::String& description, F0FrameRange affectedRange);

    /** The recorded actions, oldest first, for a ControlOriginAction (aspect NotesAndPitch). */
    std::vector<ControlOriginAction::Inner> takeActions();

    /** Notes of the content that overlap [startSec, endSec), as PianoRollComponent::recordUndoAction() cuts
        them. Pure. */
    static std::vector<Note> notesInRange(const std::vector<Note>& notes, double startSec, double endSec);
    /** Correction segments cut to [startFrame, endFrameExclusive), as PianoRollComponent::recordUndoAction()
        cuts them (a segment that crosses an edge is split and its part inside kept). Pure. */
    static std::vector<PitchCorrectionSegment> segmentsInRange(const std::vector<PitchCorrectionSegment>& segments,
                                                               int startFrame, int endFrameExclusive);

private:
    OpenTuneAudioProcessor& processor_;
    ContentKey key_;
    std::vector<Note> beforeNotes_;
    std::vector<PitchCorrectionSegment> beforeSegments_;
    bool captured_ = false;
    std::vector<ControlOriginAction::Inner> actions_;
};

} // namespace OpenTune
