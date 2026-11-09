// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ContentEditTransaction.h"

#include "Content/ContentKey.h"
#include "Utils/F0Timeline.h"
#include "Utils/Note.h"
#include "Utils/NoteGeneratorTypes.h"
#include "Utils/PitchCurve.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace OpenTune {

struct EditableContentSnapshot;
class OpenTuneAudioProcessor;

/**
 * The correction segments that the piano roll rebuilds after an edit of notes, computed the way the piano roll
 * computes them but without it (PianoRollComponent and PianoRollToolHandler stay untouched), and committed with
 * OpenTune's content commands into a ContentEditTransaction. The mirrored upstream functions are listed in
 * Source/Control/README.md (Note edits).
 *
 * The fallback parameters of PitchCurve::applyCorrectionToRange() are NoteGeneratorParams{}: the piano roll passes
 * its knob positions there, which the API never uses (PROTOCOL.md section 21 rule 1). Every note brings its own
 * retune speed, vibrato and drift; the fallback values only end up in the segments' parameter snapshot.
 *
 * Threading: message thread only (the commit); the planning functions are pure.
 */

/** What one commit writes: the affected frame range, the notes after the edit (all of them; the commit keeps the
    ones of the range) and the correction segments of the range. */
struct ControlCorrectionCommit
{
    F0FrameRange affectedRange;
    std::vector<Note> notes;
    std::vector<PitchCorrectionSegment> segments;
};

/** The frame range from the earliest start to the latest end of the notes at the given positions
    (F0Timeline::rangeForTimes on the content's analysis timeline), as the piano roll derives the edit range of a
    note drag from the notes that changed (PianoRollToolHandler::noteDragEditRange()). Empty without notes. */
F0FrameRange controlNotesFrameRange(const PitchCurveSnapshot& curve, const std::vector<Note>& notes,
                                    const std::vector<std::size_t>& positions);

/**
 * The note-based correction of edited notes (PianoRollToolHandler.cpp buildNoteBasedCorrectionState() and
 * commitNoteBasedCorrection(), which the pitch move, the snap and the modulation and drift tools share;
 * PianoRollComponent::editParameter() for selected notes computes the same): a scratch copy of the content's
 * curve, PitchCurve::applyCorrectionToRange() over all notes in editRange with the content's pitch-shift ratio,
 * the affected range PitchCurve::expandNoteBasedCorrectionRange(), and the corrected segments that overlap it.
 * nullopt for an empty edit range or a content without a pitch curve, where the piano roll commits nothing.
 */
std::optional<ControlCorrectionCommit> controlNoteBasedCorrection(const EditableContentSnapshot& snapshot,
                                                                  const std::vector<Note>& notes,
                                                                  F0FrameRange editRange);

/** What SNAP would do (controlPlanAutoSnap). */
struct ControlAutoSnapPlan
{
    enum class Status {
        /** Nothing to do: no target moves and the targets' frames are fully corrected (AutoTuneApplyStatus
            NoChange). */
        NoChange,
        /** The edit range is empty (EmptyTargetRange). */
        EmptyRange,
        Commit
    };
    Status status = Status::NoChange;
    /** All notes, the targets snapped (also for NoChange). */
    std::vector<Note> notes;
    /** Status Commit: what to commit. */
    ControlCorrectionCommit commit;
};

/**
 * SNAP, PianoRollComponent::applyAutoSnapToAllNotes() (the AUTO button in the openDyne scheme) with an explicit
 * scale: each target note gets pitchOffset = scaleSnap.quantizeMidiToActiveScale(base) - base, base the MIDI
 * pitch of its base pitch (the offset is not part of the input); then the correction of the target notes is
 * rebuilt. targets are positions in snapshot.notes; nullopt is the AUTO button without a note selection (every
 * note, the new segments cover the whole affected range), a list is a selection (the new segments are cut to the
 * target notes' frames and the other frames of the affected range keep their segments). Pure.
 */
ControlAutoSnapPlan controlPlanAutoSnap(const EditableContentSnapshot& snapshot,
                                        const std::optional<std::vector<std::size_t>>& targets,
                                        const ScaleSnapConfig& scaleSnap);

/**
 * Commits through ContentEditCommands::commitNotesAndSegments() as PianoRollComponent::
 * commitEditedContentNotesAndSegments() does (the notes and the segments cut to the affected range first), with
 * the transaction's capture before and its PianoRollEditAction after (description as the UI's). False when
 * OpenTune committed nothing.
 */
bool controlCommitCorrection(OpenTuneAudioProcessor& processor, ContentEditTransaction& transaction,
                             const ContentKey& key, const ControlCorrectionCommit& commit,
                             const juce::String& description);

} // namespace OpenTune
