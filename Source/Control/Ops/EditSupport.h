// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "WriteEnvelope.h"

#include "Utils/DetectedKey.h"
#include "Utils/Note.h"
#include "Utils/PitchShiftSettings.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenTune {

/**
 * What the methods that edit contents or apply undo steps share (content.setKey, content.autoTune, edit.*):
 * their params forms, the readiness gate, the expected-token checks, and the parts of their results that
 * describe a change of notes or an undo step. Message thread, apart from the params readers (connection
 * thread, no OpenTune state).
 */

// ---- Params ---------------------------------------------------------------------------------------

/** An optional revision token member (common.schema.json#/$defs/revisionToken): 1 to 128 characters of
    [A-Za-z0-9._:-]. */
bool controlReadRevisionToken(ControlParamReader& reader, std::string_view name, std::optional<std::string>& out);

/** A key as params give it (common.schema.json#/$defs/keyInput): {root 0..11, scale name}, closed. The result
    is a manual key, as choosing a key in the UI makes one (makeDetectedKeyFromUi in Utils/ScaleUiMapping.h). */
bool controlParseKeyInput(const JsonValue& value, DetectedKey& out);

/** The scale of a wire name (common.schema.json#/$defs/scaleName). */
std::optional<Scale> controlScaleFromName(std::string_view name);

// ---- Checks before a content write ---------------------------------------------------------------

/**
 * The readiness gate of PROTOCOL.md section 11 for a content write, from the content's snapshot: NOT_READY
 * with f0_not_ready (retryable) while the analysis was not requested or runs, f0_empty when it found no
 * voiced frame (its failure message says so, controlF0FailureCode(), or the analysis is ready without voiced
 * frames) and f0_failed when it failed otherwise. OpenTune generates the notes of an analysis in the same
 * message-thread step that marks it ready, so notes_generating never applies in this build.
 */
std::optional<Error> controlCheckContentReady(ControlOpContext& context, const ControlContentRef& content);

/** REVISION_CONFLICT(content_changed) when expectedRevision is given and differs from the content's
    revision, then REVISION_CONFLICT(key_changed) for expectedKeyRevision (PROTOCOL.md section 10 rule 3). */
std::optional<Error> controlCheckExpectedTokens(const ControlContentVersions& versions,
                                                const std::optional<std::string>& expectedRevision,
                                                const std::optional<std::string>& expectedKeyRevision);

// ---- Results ----------------------------------------------------------------------------------------

/** A note's entityRef {contentId, note: {index, atRevision}} (PROTOCOL.md section 9). */
JsonValue controlNoteRefJson(const std::string& contentId, std::size_t index, const std::string& revision);

/** The display label of a note at the instance's tuning (controlNoteLabel). */
JsonValue controlNoteLabelJson(std::size_t index, const Note& note);

/** sideEffects.reRenderQueued {contentIds, revert: null}: the contents whose render input (notes, pitch,
    audio and analysis, time grid, pitch shift) differs between the two captures, which OpenTune renders
    again. Nothing when there is none. */
void controlAddReRenderQueued(ControlWriteEnvelope& envelope, const ControlWorldVersions& before,
                              const ControlWorldVersions& after);

/** The reRenderQueued side effect that a commit to the content would report, for a prediction (dryRun): the
    same form controlAddReRenderQueued() gives it, {contentIds: [contentId], revert: null}. */
void controlAddPredictedReRenderQueued(ControlWriteEnvelope& envelope, const std::string& contentId);

/**
 * The changes of notes that the write does not move (content.editNotes, content.autoTune mode snapNotes): one entry
 * of entity note per note index whose reported values differ between before and after, with the fields
 * startContentSec and endContentSec (sec; listed only when OpenTune's commit trimmed a note that overlapped its
 * successor, which changes its times but not the note count), targetMidi (midi),
 * offsetCents (cents), retuneSpeed, vibratoDepth, vibratoRateHz (hz) and pitchDriftScale as content.getNotes
 * reports them, referenced at revision (PROTOCOL.md section 14.3). before and after hold the same notes, as many
 * and with their times; each list is counted in its own content.getNotes order (controlNoteIndexOrder), as the
 * stored order of after can differ from that of before (OpenTune's commit sorts the notes it stores). For a commit,
 * after is what controlReadCommittedNotes() read back, so that each "after" is what content.getNotes reads at
 * revision. At most maxListed entries; returns how many changed notes were left out.
 */
std::size_t controlListNoteChanges(ControlWriteEnvelope& envelope, const std::string& contentId,
                                   const std::vector<Note>& before, const std::vector<Note>& after,
                                   const std::string& revision, std::size_t maxListed);

/**
 * The members of a clip that differ between two captures of it (PROTOCOL.md section 14.3), named as project.get
 * names them: trackId, startTimelineSec, endTimelineSec, durationSec, clipInContentSec, gainDb, gainLinear, fadeInSec,
 * fadeOutSec. Each member is {before, after, unit}; an empty object when nothing differs. Shared by the effect of an
 * undo step and by arrangement.edit, so that both report a clip change alike. Pure.
 */
JsonValue controlPlacementChangeFields(const ControlPlacementRef& was, const ControlPlacementRef& now);

/** The notes of a content as a write committed them, and the content's revision then. */
struct ControlCommittedNotes
{
    std::vector<Note> notes;
    std::string revision;
};

/**
 * Reads the notes of a content back from the live content after a write committed to it: what the write result
 * reports, never the notes the write predicted, so that every "after" of its changes is what a read that follows
 * returns (PROTOCOL.md section 14.3), whatever OpenTune's commit did with the notes it was given (its range commit
 * stores them through normalizeStoredNotes(), which sorts them, trims overlaps and drops notes without duration).
 * Fails after the change (INTERNAL, `executed` true, so that the client reads the content again) when the content
 * cannot be read back, and when expectedCount is given and the content now holds another number of notes: the
 * changes of the write can then not be described note by note. Message thread.
 */
Result<ControlCommittedNotes> controlReadCommittedNotes(ControlOpContext& context, const ControlContentRef& content,
                                                        std::optional<std::size_t> expectedCount);

// ---- The effect of an undo or redo step --------------------------------------------------------------

/** The values from which the effect of an undo or redo step is described. */
struct ControlEditState
{
    struct Content
    {
        std::string contentId;
        DetectedKey key;
        PitchShiftSettings pitchShift;
    };
    struct Track
    {
        bool muted = false;
        bool solo = false;
        double volumeLinear = 1.0;
        std::uint32_t colour = 0;
        bool visible = false;
    };

    ControlWorldVersions versions;
    std::vector<Content> contents;
    std::vector<ControlPlacementRef> placements;
    std::vector<Track> tracks;
};

/** Captures the state in one message-thread step. */
ControlEditState controlCaptureEditState(ControlOpContext& context);

/**
 * Records in the envelope what explains the difference of two captures (PROTOCOL.md section 14.4 rule 6),
 * whatever undo entry caused it, also one the server cannot inspect: per content an entry of entity content
 * with the fields revision and gridRevision that changed, of entity key with root, scale and origin, of
 * entity pitchShift with semitones and cents; per placement an entry of entity placement with its changed
 * geometry, gain and fades; per track an entry of entity track; contents and placements that
 * appeared or disappeared in created and removed; the tokens of every changed content and placement; and
 * reRenderQueued.
 */
void controlDescribeEffect(ControlOpContext& context, const ControlEditState& before, const ControlEditState& after,
                           ControlWriteEnvelope& envelope);

} // namespace OpenTune
