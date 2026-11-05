// SPDX-License-Identifier: AGPL-3.0-or-later
// The workspace of the editor's side of ControlUiBridge: ui.setWorkspace (Control/Ops/WorkspaceOps.cpp) switches the
// window between the arrangement view and the piano roll as the two view buttons of the transport bar do. Only the
// view changes; the editor's viewToggled() moves the timeline camera, the visibility and the keyboard focus of the two
// views and runs the piano roll's heartbeat tick (the camera of the first F0 frame and the waveform cache), and never
// touches a content, a key, a note, a clip or a track. The one exception is upstream behaviour that is not corrected
// here and that the operation reports (Control/Ops/WorkspaceOps.cpp): the keyboard focus that leaves the transport
// bar's BPM field commits the field, which rewrites a tempo with more than one decimal (see settleViewSwitch()). The
// focus can also leave another field that commits when it loses the focus; that commit is a real change of the project,
// and settleViewSwitch() leaves the changed mark alone for it.
#include "ControlEditorBridge.h"

#include "PluginEditor.h"

namespace OpenTune {

namespace {

/** The transport bar's BPM field: the BpmValueField among the children of the transport bar, or null. The editor keeps
    the field private in the transport bar, which the bridge is no friend of, so it is found by its type. */
const juce::Component* findBpmField(const juce::Component& transportBar)
{
    for (const auto* child : transportBar.getChildren())
        if (dynamic_cast<const BpmValueField*>(child) != nullptr)
            return child;
    return nullptr;
}

} // namespace

ControlEditorBridge::ViewSwitchBaseline ControlEditorBridge::viewSwitchBaseline() const
{
    ViewSwitchBaseline baseline;
    baseline.dirty = editor_.projectSession_.isDirty();
    baseline.dirtyGeneration = editor_.projectSession_.getDirtyGeneration();
    baseline.bpm = editor_.processorRef_.getBpm();
    baseline.timeSignatureNumerator = editor_.processorRef_.getTimeSigNumerator();
    baseline.timeSignatureDenominator = editor_.processorRef_.getTimeSigDenominator();

    // juce::Component::getCurrentlyFocusedComponent(): the component that holds the keyboard focus now, null when none
    // does (a window in the background has none). The commit of the BPM field follows the switch when the field holds
    // the focus, and also when the view that is shown holds it or one of its children (a tool button of the piano roll,
    // say): the hidden view gives the focus to the editor, which hands it to the default component, the field, and the
    // focus that the new view then grabs leaves the field. After a switch of the control API the view holds the focus,
    // so this is the usual case on the real window. With the focus anywhere else the field is not involved.
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (focused != nullptr) {
        const auto* bpmField = findBpmField(editor_.transportBar_);
        const juce::Component* outgoingView = editor_.isWorkspaceView_
                                                  ? static_cast<const juce::Component*>(&editor_.arrangementView_)
                                                  : static_cast<const juce::Component*>(&editor_.pianoRoll_);
        baseline.focusLeadsToBpmCommit =
            focused == outgoingView || outgoingView->isParentOf(focused)
            || (bpmField != nullptr && (focused == bpmField || bpmField->isParentOf(focused)));
    }
    return baseline;
}

void ControlEditorBridge::settleViewSwitch(const ViewSwitchBaseline& baseline)
{
    // viewToggled() moves the keyboard focus, and the focus that the transport bar's BPM field loses then commits the
    // field (BpmValueField::commit -> TransportBarComponent::onBpmChanged -> OpenTuneAudioProcessorEditor::bpmChanged),
    // which sets the tempo the field shows (the processor's tempo with one decimal) and marks the project as changed
    // even when nothing was typed. A view switch leaves the project as it was (PROTOCOL.md section 15.2 rule 6), so the
    // mark is taken back when the project was unchanged before and the tempo and the time signature are what they were:
    // then the mark came from that commit, with nothing to save. A tempo or time signature that did change (a tempo
    // with more decimals that the field rewrote, or a value typed into the field and not committed yet) is a real
    // change and keeps the mark; the operation reports the rewritten tempo from the processor's tempo before and after.
    //
    // Only the commit of the BPM field is taken back. The focus that moves can also leave another field that commits
    // when it loses the focus, and that commit is a real change that leaves the tempo and the time signature as they
    // were, so the conditions above alone would take its mark back and a later project.open or app.quit would not see
    // the change. Two more conditions keep it: the focus before the switch has to be where the commit of the BPM field
    // follows it (viewSwitchBaseline()), and the mark has to be set exactly once during the switch, as one commit of
    // the field sets it (the dirty generation counts every markDirty()); the commit of another field adds a second one.
    // For anything else the mark stays. A mark that stays costs at most a project that is reported as changed without a
    // change in it; a mark taken back by mistake would lose a change without a word. (The numeric boxes of the panels,
    // the juce::Slider text boxes, are modal while they are edited, and the control API refuses the call with
    // BUSY(modal_open) before the bridge is reached; these conditions guard whatever else commits on losing the focus.)
    //
    // ProjectSession::clearDirty() does not take the dirty generation back: markDirty() counted it up for the commit,
    // and it stays. That matters only to a project save that is on its way and that clears the mark when its generation
    // is still the current one (saveProject() in the editor, the save job of project.save). None is: the operations
    // that switch the view answer BUSY(project_operation) while the editor's flag of a running project operation is set
    // (controlCheckBusy(), which both operations call before the editor is asked; the editor's own save sets the flag
    // from its start to the end of its worker's callback, the save job of the control API from its start to its end),
    // and the editor runs on the message thread, where no save can finish between the commit and this call.
    if (controlViewSwitchMarkIsBpmCommit(baseline, editor_.projectSession_.isDirty(),
                                         editor_.projectSession_.getDirtyGeneration(), editor_.processorRef_.getBpm(),
                                         editor_.processorRef_.getTimeSigNumerator(),
                                         editor_.processorRef_.getTimeSigDenominator()))
        editor_.projectSession_.clearDirty();
}

bool ControlEditorBridge::setWorkspace(bool arrangement)
{
    // The window shows the view already: what a click on the button of the view shown does (it only re-asserts the
    // toggle state of the button).
    if (editor_.isWorkspaceView_ == arrangement)
        return true;

    const auto baseline = viewSwitchBaseline();
    // TransportBarComponent::onTrackViewClicked() / onPianoViewClicked(): the button state, then the listener, which
    // is the editor. The editor's own Escape handler (escapeKeyPressed) makes the same two calls.
    editor_.transportBar_.setWorkspaceView(arrangement);
    editor_.viewToggled(arrangement);
    settleViewSwitch(baseline);
    return true;
}

} // namespace OpenTune
