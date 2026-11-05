// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Utils/PitchShiftSettings.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace OpenTune {

struct EditableContentSnapshot;

/**
 * Takes the steps of OpenTune's pitch shifts out of the notes and pitch counters from which the control API
 * derives the `revision` token, because a pitch shift must not change `revision` (PROTOCOL.md section 10
 * rule 2).
 *
 * OpenTune applies a pitch shift with applyPitchShiftState() (OpenTuneAudioProcessor::commitPitchShiftEdit(),
 * and the undo and redo of PitchShiftEditAction): it scales the notes and the correction segments and
 * advances the content's notesRevision and pitchRevision by one each, the counters that a note edit advances
 * as well. applyPitchShiftState() is the only change of a Standalone content that changes its pitch shift
 * settings, so a step shows as: the settings changed, and both counters advanced. The ledger observes each
 * content whenever the API computes its tokens (controlContentVersions()), counts such steps between two
 * observations, and returns the counters without them. It only reads: OpenTune's counters stay as they are.
 *
 * Exact when every pitch-shift step lies between two observations. That holds for the steps the API applies
 * itself (content.setPitchShift, edit.undo, edit.redo), because the write envelope observes every content
 * right before and right after a write. Between two observations at most one step is counted: when the user
 * shifts twice in the UI in between (or shifts and takes it back with Ctrl+Z), `revision` changes although
 * the notes did not, and a client that sends the old revision gets REVISION_CONFLICT and reads again. The
 * ledger never hides a change of the notes: a counted step always accounts for one increment of each counter
 * that a pitch shift made.
 *
 * Threading: message thread only. Owned by ControlHistory, which every operation context reaches.
 */
class ControlPitchShiftLedger
{
public:
    /** The notes and pitch counters of a content. */
    struct Counters
    {
        std::uint64_t notes = 0;
        std::uint64_t pitch = 0;
    };

    /**
     * Observes the content's snapshot and returns its counters without the pitch-shift steps counted up to
     * it. contentId names the content in the content epoch epoch; a new epoch forgets every content (opening
     * a project discards all contents). A counter lower than at the last observation means that the content
     * object was built anew under the same contentId (its counters start again): the content's entry starts
     * over from this snapshot, without the steps counted before, and its counters are returned as they are.
     */
    Counters observe(const std::string& contentId, std::uint64_t epoch, const EditableContentSnapshot& snapshot);

private:
    struct Entry
    {
        Counters last;
        PitchShiftSettings shift;
        /** The counters right after each counted step, in increasing order. */
        std::vector<Counters> steps;
    };

    std::optional<std::uint64_t> epoch_;
    std::map<std::string, Entry> entries_;
};

} // namespace OpenTune
