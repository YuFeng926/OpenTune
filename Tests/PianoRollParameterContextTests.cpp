#include "Content/EditableContentSnapshot.h"
#include "Standalone/UI/PianoRoll/PianoRollToolHandler.h"
#include "Utils/KeyShortcutConfig.h"
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace OpenTune;
using KeyShortcutConfig::ShortcutId;

Note makeNote(double start, double end, float retuneSpeed)
{
    Note note;
    note.startTime = start;
    note.endTime = end;
    note.pitch = 220.0f;
    note.originalPitch = 220.0f;
    note.retuneSpeed = retuneSpeed;
    return note;
}

struct Fixture
{
    InteractionState state;
    std::vector<Note> notes;
    std::shared_ptr<EditableContentSnapshot> snapshot = std::make_shared<EditableContentSnapshot>();
    KeyShortcutConfig::KeyShortcutSettings shortcuts = KeyShortcutConfig::KeyShortcutSettings::getDefault();
    uint64_t notesRevision = 0;
    bool accessibilityMode = false;
    AudioEditingScheme::Scheme scheme = AudioEditingScheme::Scheme::CorrectedF0Primary;

    void publishNotes()
    {
        snapshot->notes = notes;
        snapshot->notesRevision = notesRevision;
    }

    PianoRollToolHandler::Context makeContext()
    {
        PianoRollToolHandler::Context context;
        context.getState = [this]() -> InteractionState& { return state; };
        context.getCommittedNotes = [this]() -> const std::vector<Note>& { return notes; };
        context.getEditableContentSnapshot = [this]() -> std::shared_ptr<const EditableContentSnapshot> {
            return snapshot;
        };
        context.getNotesRevision = [this] { return notesRevision; };
        context.getF0Timeline = [] { return F0Timeline(10, 1000.0, 100); };
        context.getAudioEditingScheme = [this] { return scheme; };
        context.getShortcutSettings = [this]() -> const KeyShortcutConfig::KeyShortcutSettings& {
            return shortcuts;
        };
        context.accessibilityModeEnabled = [this] { return accessibilityMode; };
        return context;
    }
};

bool checkCtrlACommitsBeforeReadingAndProjectsAfterSelection()
{
    Fixture fixture;
    fixture.notes = { makeNote(0.1, 0.3, 0.2f), makeNote(0.4, 0.7, 0.4f) };
    fixture.notesRevision = 3;
    fixture.publishNotes();
    fixture.state.noteSelection.setSingle(0, 2);
    KeyShortcutConfig::setShortcutBinding(
        fixture.shortcuts, ShortcutId::SelectAll,
        KeyShortcutConfig::KeyBinding('A', juce::ModifierKeys::ctrlModifier));

    auto context = fixture.makeContext();
    std::vector<std::string> order;
    bool willSawOldSelection = false;
    bool didSawAllNotesSelected = false;
    float displayedRetuneSpeed = -1.0f;
    context.parameterTargetWillChange = [&] {
        order.emplace_back("will-change");
        willSawOldSelection = fixture.state.noteSelection.selectedIndices == std::vector<int>{ 0 };
        fixture.notes[0].retuneSpeed = 0.8f;
        ++fixture.notesRevision;
        fixture.publishNotes();
        order.emplace_back("pending-commit");
    };
    context.getCommittedNotes = [&]() -> const std::vector<Note>& {
        order.emplace_back("read-model");
        return fixture.notes;
    };
    context.getEditableContentSnapshot = [&]() -> std::shared_ptr<const EditableContentSnapshot> {
        order.emplace_back("read-snapshot");
        return fixture.snapshot;
    };
    context.parameterTargetDidChange = [&] {
        order.emplace_back("projection");
        didSawAllNotesSelected = fixture.state.noteSelection.selectedIndices == std::vector<int>{ 0, 1 };
        if (didSawAllNotesSelected)
            displayedRetuneSpeed = fixture.notes[0].retuneSpeed;
    };

    PianoRollToolHandler handler(std::move(context));
    const bool handled = handler.keyPressed(
        juce::KeyPress('A', juce::ModifierKeys::ctrlModifier, 'A'));

    return handled
        && willSawOldSelection
        && didSawAllNotesSelected
        && displayedRetuneSpeed == 0.8f
        && fixture.notesRevision == 4
        && order == std::vector<std::string>{
            "will-change", "pending-commit", "read-model", "read-snapshot", "projection" };
}

bool checkAccessibilitySplitPlansFromCommittedParameters()
{
    Fixture fixture;
    fixture.notes = { makeNote(0.2, 0.8, 0.2f) };
    fixture.notesRevision = 10;
    fixture.publishNotes();
    fixture.snapshot->pitchCurve = std::make_shared<const PitchCurveSnapshot>(
        std::vector<float>(100, 220.0f), std::vector<float>(100, 1.0f),
        std::vector<PitchCorrectionSegment>{}, 10, 1000.0);
    fixture.state.noteSelection.setSingle(0, 1);
    fixture.accessibilityMode = true;
    fixture.scheme = AudioEditingScheme::Scheme::NotesPrimary;

    auto context = fixture.makeContext();
    bool willSawOriginalSelectionAndValue = false;
    bool pendingCommitAdvancedRevision = false;
    bool scissorsReceivedCommittedValue = false;
    bool didProjectedSplitNote = false;
    float displayedRetuneSpeed = -1.0f;
    context.parameterTargetWillChange = [&] {
        willSawOriginalSelectionAndValue = fixture.state.noteSelection.selectedIndices == std::vector<int>{ 0 }
            && fixture.notes[0].retuneSpeed == 0.2f;
        fixture.notes[0].retuneSpeed = 0.8f;
        ++fixture.notesRevision;
        fixture.publishNotes();
        pendingCommitAdvancedRevision = fixture.notesRevision == 11;
    };
    context.commitScissorsNotes = [&](const std::vector<Note>& afterNotes, juce::String) {
        scissorsReceivedCommittedValue = fixture.notesRevision == 11
            && afterNotes.size() == 2
            && afterNotes[0].retuneSpeed == 0.8f
            && afterNotes[1].retuneSpeed == 0.8f;
        fixture.notes = afterNotes;
        ++fixture.notesRevision;
        fixture.publishNotes();
        return true;
    };
    context.parameterTargetDidChange = [&] {
        didProjectedSplitNote = fixture.state.noteSelection.selectedIndices == std::vector<int>{ 0 }
            && fixture.notes.size() == 2;
        if (didProjectedSplitNote)
            displayedRetuneSpeed = fixture.notes[0].retuneSpeed;
    };

    PianoRollToolHandler handler(std::move(context));
    const bool handled = handler.keyPressed(juce::KeyPress('X', {}, 'x'));

    return handled
        && willSawOriginalSelectionAndValue
        && pendingCommitAdvancedRevision
        && scissorsReceivedCommittedValue
        && didProjectedSplitNote
        && displayedRetuneSpeed == 0.8f
        && fixture.notesRevision == 12;
}

bool checkAccessibilityDeleteProjectsTheNoteReusingIndexZero()
{
    Fixture fixture;
    fixture.notes = { makeNote(0.1, 0.3, 0.2f), makeNote(0.4, 0.7, 0.73f) };
    fixture.notesRevision = 20;
    fixture.publishNotes();
    fixture.state.noteSelection.setSingle(0, 2);
    fixture.accessibilityMode = true;
    fixture.scheme = AudioEditingScheme::Scheme::NotesPrimary;

    auto context = fixture.makeContext();
    bool deleteReceivedSelection = false;
    bool didSeeReusedIndex = false;
    float displayedRetuneSpeed = -1.0f;
    context.deleteSelectedPianoRollNotes = [&](const std::vector<int>& selectedIndices,
                                                uint64_t expectedRevision,
                                                juce::String) {
        deleteReceivedSelection = selectedIndices == std::vector<int>{ 0 }
            && expectedRevision == 20
            && fixture.notes[0].retuneSpeed == 0.2f;
        SelectedNoteDeleteResult result;
        result.status = SelectedNoteDeleteStatus::Applied;
        result.beforeNotes = fixture.notes;
        fixture.notes.erase(fixture.notes.begin());
        ++fixture.notesRevision;
        fixture.publishNotes();
        result.contentSnapshot = fixture.snapshot;
        return result;
    };
    context.parameterTargetDidChange = [&] {
        didSeeReusedIndex = fixture.state.noteSelection.selectedIndices == std::vector<int>{ 0 }
            && fixture.snapshot->notes.size() == 1
            && fixture.snapshot->notes[0].retuneSpeed == 0.73f;
        if (didSeeReusedIndex)
            displayedRetuneSpeed = fixture.snapshot->notes[0].retuneSpeed;
    };

    PianoRollToolHandler handler(std::move(context));
    const bool handled = handler.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey, {}, 0));

    return handled
        && deleteReceivedSelection
        && didSeeReusedIndex
        && fixture.notesRevision == 21
        && fixture.notes.size() == 1
        && fixture.notes[0].retuneSpeed == 0.73f
        && displayedRetuneSpeed == 0.73f;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI guiInitialiser;
    juce::ignoreUnused(guiInitialiser);
    if (!checkCtrlACommitsBeforeReadingAndProjectsAfterSelection()) {
        std::cerr << "Ctrl+A parameter-context ordering failed\n";
        return 1;
    }
    if (!checkAccessibilitySplitPlansFromCommittedParameters()) {
        std::cerr << "Accessibility split parameter-context failed\n";
        return 1;
    }
    if (!checkAccessibilityDeleteProjectsTheNoteReusingIndexZero()) {
        std::cerr << "Accessibility delete parameter projection failed\n";
        return 1;
    }
    return 0;
}
