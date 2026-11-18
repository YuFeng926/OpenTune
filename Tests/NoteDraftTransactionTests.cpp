#include "../Source/PluginProcessor.h"

#include <iostream>

namespace {

using namespace OpenTune;
using Status = OpenTuneAudioProcessor::NoteDraftCommitStatus;

Note makeNote(double start, double end, float pitch)
{
    Note note;
    note.startTime = start;
    note.endTime = end;
    note.pitch = pitch;
    note.originalPitch = pitch;
    return note;
}

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

bool runTransactions()
{
    OpenTuneAudioProcessor processor;
    processor.prepareToPlay(44100.0, 512);
    auto& repository = *processor.getStandaloneContentRepository();
    auto& history = processor.getUndoManager();
    const auto key = repository.createClip();
    const auto otherKey = repository.createClip();
    const std::vector<Note> original{
        makeNote(0.125, 0.375, 220.0f),
        makeNote(0.625, 0.875, 330.0f),
        makeNote(1.125, 1.375, 440.0f)
    };
    repository.findClip(key)->applyNotes(original);
    repository.findClip(otherKey)->applyNotes({makeNote(2.125, 2.375, 550.0f)});
    const auto initial = processor.getContentSnapshot(key);

    auto result = processor.commitNoteDraft(key, initial->notesRevision, original, "Unchanged draft");
    if (!expect(result.status == Status::NoChange && history.size() == 0
                    && result.contentSnapshot->notesRevision == initial->notesRevision,
                "NoChange must preserve revision and history"))
        return false;

    auto changed = original;
    changed[1].pitch = 660.0f;
    changed[1].endTime = 0.9375;
    result = processor.commitNoteDraft(key, initial->notesRevision, changed, "Change draft");
    if (!expect(result.status == Status::Applied && history.size() == 1
                    && result.contentSnapshot->notes[1].pitch == 660.0f
                    && result.contentSnapshot->notes[1].endTime == 0.9375
                    && result.contentSnapshot->notes.front().startTime == original.front().startTime
                    && result.contentSnapshot->notes.back().endTime == original.back().endTime,
                "Draft modification must commit one scoped transaction"))
        return false;

    const auto changedSnapshot = result.contentSnapshot;
    auto staleWorking = original;
    staleWorking[0].pitch = 880.0f;
    result = processor.commitNoteDraft(key, initial->notesRevision, staleWorking, "Stale draft");
    if (!expect(result.status == Status::StaleRevision && history.size() == 1
                    && result.contentSnapshot->notesRevision == changedSnapshot->notesRevision
                    && result.contentSnapshot->notes[0].pitch == original[0].pitch
                    && result.contentSnapshot->notes[1].pitch == 660.0f,
                "Stale draft must return current content and preserve committed notes"))
        return false;

    // A newer edit of a different content key changes the editing context.
    const auto otherBefore = processor.getContentSnapshot(otherKey);
    auto otherWorking = otherBefore->notes;
    otherWorking[0].pitch = 770.0f;
    result = processor.commitNoteDraft(otherKey, otherBefore->notesRevision, otherWorking, "Other content");
    if (!expect(result.status == Status::Applied && history.size() == 2,
                "Second content must have an independent transaction"))
        return false;
    history.undo();
    history.undo();
    if (!expect(processor.getContentSnapshot(key)->notes[1].pitch == original[1].pitch
                    && processor.getContentSnapshot(key)->notes[1].endTime == original[1].endTime
                    && processor.getContentSnapshot(otherKey)->notes[0].pitch == 550.0f,
                "Undo must retain each transaction's original content key"))
        return false;
    const auto undone = processor.getContentSnapshot(key);
    result = processor.commitNoteDraft(key, undone->notesRevision, undone->notes, "Unchanged after undo");
    if (!expect(result.status == Status::NoChange && history.size() == 2,
                "NoChange after undo must preserve the redo branch"))
        return false;
    history.redo();
    history.redo();
    if (!expect(processor.getContentSnapshot(key)->notes[1].pitch == 660.0f
                    && processor.getContentSnapshot(otherKey)->notes[0].pitch == 770.0f,
                "Redo must restore each original content independently"))
        return false;

    auto beforeInsert = processor.getContentSnapshot(key);
    auto inserted = beforeInsert->notes;
    inserted.insert(inserted.begin() + 1, makeNote(0.4375, 0.5625, 275.0f));
    result = processor.commitNoteDraft(key, beforeInsert->notesRevision, inserted, "Insert draft");
    if (!expect(result.status == Status::Applied && result.contentSnapshot->notes.size() == 4,
                "Draft insertion must add the requested note"))
        return false;
    history.undo();
    if (!expect(processor.getContentSnapshot(key)->notes.size() == 3,
                "Insertion undo must remove only the inserted note"))
        return false;
    history.redo();

    const auto beforeDelete = processor.getContentSnapshot(key);
    auto deleted = beforeDelete->notes;
    deleted.erase(deleted.begin() + 2);
    result = processor.commitNoteDraft(key, beforeDelete->notesRevision, deleted, "Delete draft");
    if (!expect(result.status == Status::Applied && result.contentSnapshot->notes.size() == 3
                    && result.contentSnapshot->notes.back().pitch == 440.0f,
                "Draft deletion must preserve notes outside the patch"))
        return false;
    history.undo();
    if (!expect(processor.getContentSnapshot(key)->notes.size() == 4
                    && processor.getContentSnapshot(key)->notes[2].pitch == 660.0f,
                "Deletion undo must restore the removed note"))
        return false;
    history.redo();
    return expect(processor.getContentSnapshot(key)->notes.size() == 3,
                  "Deletion redo must apply the same patch");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    return runTransactions() ? 0 : 1;
}
