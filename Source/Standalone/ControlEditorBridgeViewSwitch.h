// SPDX-License-Identifier: AGPL-3.0-or-later
// The rule that decides whether the changed mark that is set after a view switch is taken back (see
// ControlEditorBridgeWorkspace.cpp). It reads plain values and no component, so that a test can run it without a
// window.
#pragma once

#include <cstdint>

namespace OpenTune {

/**
 * What a view switch must leave as it was, read before the switch (ControlEditorBridge::viewSwitchBaseline()): the
 * project's changed mark and the number of times it was set, the tempo and the time signature of the transport bar, and
 * where the keyboard focus was.
 */
struct ControlViewSwitchBaseline
{
    bool dirty = false;
    /** ProjectSession::getDirtyGeneration(): it counts every markDirty(), and clearDirty() does not take it back. */
    std::uint64_t dirtyGeneration = 0;
    double bpm = 0.0;
    int timeSignatureNumerator = 0;
    int timeSignatureDenominator = 0;

    /** The keyboard focus was where the commit of the transport bar's BPM field can follow the switch: in the field, or
        in the view that the switch hides (a hidden view gives the focus to the editor, which hands it to the default
        component, the field, and the focus that the new view then grabs leaves the field). False for anything else: a
        field that commits when it loses the focus (the focus moves out of it at the grab of the new view, and its
        commit is a real change), and no focus at all.
        The view case rests on an observation of the real window, not on a guarantee: the focus that leaves a hidden
        view always passes the BPM field, so a field inside that view that commits on focus loss comes with a commit of
        the BPM field (the mark is set twice and stays). Today the only such field in the Standalone UI is the BPM
        field itself (the parameter panel's number boxes edit modally, and a write meanwhile answers BUSY(modal_open)).
        A field added inside a view that commits on focus loss and hands the focus elsewhere than to the editor would
        set the mark once, and this rule would take it back: narrow the view case for such a field. */
    bool focusLeadsToBpmCommit = false;
};

/**
 * After a view switch: whether the project's changed mark came only from the commit of the transport bar's BPM field,
 * so that the mark is to be taken back. True when all of these hold: the focus was where that commit can follow it
 * (focusLeadsToBpmCommit); the project was unchanged before and is changed now; the mark was set exactly once, which is
 * what one commit of the field does (OpenTuneAudioProcessorEditor::bpmChanged marks once), so that no other field's
 * commit came with it; and the tempo and the time signature are what they were (the field commits the tempo it shows,
 * so a commit that changed nothing leaves them as they were).
 *
 * False keeps the mark, and a mark that stays is never a loss: the worst case is a project that is reported as changed
 * and asks to be saved, not a change that nothing reports. Pure; any thread.
 */
inline bool controlViewSwitchMarkIsBpmCommit(const ControlViewSwitchBaseline& baseline, bool dirtyNow,
                                             std::uint64_t dirtyGenerationNow, double bpmNow,
                                             int timeSignatureNumeratorNow, int timeSignatureDenominatorNow)
{
    return baseline.focusLeadsToBpmCommit && !baseline.dirty && dirtyNow
           && dirtyGenerationNow == baseline.dirtyGeneration + 1 && bpmNow == baseline.bpm
           && timeSignatureNumeratorNow == baseline.timeSignatureNumerator
           && timeSignatureDenominatorNow == baseline.timeSignatureDenominator;
}

} // namespace OpenTune
