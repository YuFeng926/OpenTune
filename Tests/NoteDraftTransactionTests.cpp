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

    auto changed = original;
    changed[1].pitch = 660.0f;
    changed[1].endTime = 0.9375;
    auto result = processor.commitNoteDraft(key, initial->notesRevision, changed, "Change draft");
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
    return expect(processor.getContentSnapshot(key)->notes[1].pitch == 660.0f
                      && processor.getContentSnapshot(otherKey)->notes[0].pitch == 770.0f,
                  "Redo must restore each original content independently");
}

bool runCorrectionDraftTransaction()
{
    OpenTuneAudioProcessor processor;
    processor.prepareToPlay(44100.0, 512);
    auto& repository = *processor.getStandaloneContentRepository();
    auto& history = processor.getUndoManager();
    const auto key = repository.createClip();
    auto* clip = repository.findClip(key);
    auto curve = std::make_shared<PitchCurve>();
    curve->setHopSize(256);
    curve->setSampleRate(44100.0);
    curve->setOriginalF0(std::vector<float>(128, 220.0f));
    clip->applyPitchCurve(std::move(curve));
    clip->applyNotes({makeNote(0.125, 0.375, 220.0f)});
    const auto baseline = processor.getContentSnapshot(key);
    auto working = baseline->notes;
    working[0].pitchOffset = 3.0f;

    auto external = baseline->notes;
    external[0].pitch = 330.0f;
    const auto newer = processor.commitNoteDraft(key, baseline->notesRevision, external, "External edit");
    const auto* expectedUndo = history.peekUndo();
    const NoteGeneratorParams params;
    const F0FrameRange range{0, 128};
    const auto stale = processor.commitNoteBasedCorrectionDraft(
        key, baseline->notesRevision, working, range, params, "Old pitch draft");
    if (!expect(stale.status == Status::StaleRevision
                    && stale.contentSnapshot->notes[0].pitch == 330.0f
                    && stale.contentSnapshot->pitchRevision == newer.contentSnapshot->pitchRevision
                    && history.peekUndo() == expectedUndo && history.size() == 1,
                "A full correction draft must preserve the newer edit and its history"))
        return false;

    history.undo();
    const auto undone = processor.getContentSnapshot(key);
    const auto* expectedRedo = history.peekRedo();
    const auto unchanged = processor.commitNoteBasedCorrectionDraft(
        key, undone->notesRevision, undone->notes, range, params, "Unchanged pitch draft");
    if (!expect(unchanged.status == Status::NoChange
                    && history.cursor() == 0 && history.peekRedo() == expectedRedo,
                "An unchanged correction draft must preserve redo"))
        return false;

    const auto applied = processor.commitNoteBasedCorrectionDraft(
        key, undone->notesRevision, working, range, params, "Pitch draft");
    if (!expect(applied.status == Status::Applied && history.size() == 1
                    && applied.contentSnapshot->notes[0].pitchOffset == 3.0f
                    && !applied.contentSnapshot->pitchCurve->getCorrectionSegments().empty(),
                "A current correction draft must commit notes and segments in one action"))
        return false;
    history.undo();
    const auto restored = processor.getContentSnapshot(key);
    if (!expect(restored->notes[0].pitchOffset == 0.0f
                    && restored->pitchCurve->getCorrectionSegments().empty(),
                "Correction undo must restore notes and segments together"))
        return false;
    history.redo();
    return expect(processor.getContentSnapshot(key)->notes[0].pitchOffset == 3.0f,
                  "Correction redo must restore the committed draft");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    return runTransactions() && runCorrectionDraftTransaction() ? 0 : 1;
}
