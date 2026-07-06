// ============================================================================
// OpenTune PianoRoll Behavior Tests
// ============================================================================
// Real behavior tests for PianoRoll edit undo boundaries.
// These tests execute production PianoRollEditAction + UndoManager code.
// ============================================================================

#include "../Source/Utils/PianoRollEditAction.h"
#include "../Source/Utils/UndoManager.h"
#include "../Source/Content/EditableContentSnapshot.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace OpenTune;

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        ++failures;
        std::cout << "[FAIL] " << message << "\n";
    }
}

Note makeNote(double startSeconds, double endSeconds, float pitchHz)
{
    Note note;
    note.startTime = startSeconds;
    note.endTime = endSeconds;
    note.pitch = pitchHz;
    note.originalPitch = pitchHz;
    return note;
}

bool sameNotes(const std::vector<Note>& a, const std::vector<Note>& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].startTime != b[i].startTime
            || a[i].endTime != b[i].endTime
            || a[i].pitch != b[i].pitch
            || a[i].originalPitch != b[i].originalPitch) {
            return false;
        }
    }

    return true;
}

struct CommitCall
{
    ContentKey key;
    std::vector<Note> notes;
    std::vector<PitchCorrectionSegment> segments;
    ContentEditRangeFrames range;
};

class RecordingContentEditCommands final : public ContentEditCommands
{
public:
    std::vector<CommitCall> commits;
    int unexpectedCalls = 0;

    bool replaceContentNotesForFullMutation(ContentKey, std::vector<Note>) override
    {
        ++unexpectedCalls;
        return false;
    }

    bool commitNotePatch(ContentKey, ContentNoteRangePatch) override
    {
        ++unexpectedCalls;
        return false;
    }

    ContentCommitSnapshot commitNotesAndSegments(ContentKey key,
                                                 std::vector<Note> notes,
                                                 std::vector<PitchCorrectionSegment> segments,
                                                 ContentEditRangeFrames affectedRange) override
    {
        commits.push_back({key, notes, segments, affectedRange});

        auto snapshot = std::make_shared<EditableContentSnapshot>();
        snapshot->notes = std::move(notes);
        snapshot->correctionSegments = std::move(segments);
        snapshot->notesRevision = static_cast<uint64_t>(commits.size());
        snapshot->contentRevision = snapshot->notesRevision;
        return snapshot;
    }

    bool setPitchCurve(ContentKey, std::shared_ptr<PitchCurve>, ContentEditRangeFrames) override
    {
        ++unexpectedCalls;
        return false;
    }

    bool setTimeGrid(ContentKey, std::shared_ptr<const TimeGridSnapshot>) override
    {
        ++unexpectedCalls;
        return false;
    }

    bool setDetectedKey(ContentKey, const DetectedKey&) override
    {
        ++unexpectedCalls;
        return false;
    }

    bool setPitchShiftSettings(ContentKey, const PitchShiftSettings&) override
    {
        ++unexpectedCalls;
        return false;
    }

    bool commitAutoTuneGeneratedNotes(ContentKey,
                                      std::vector<Note>,
                                      int,
                                      int,
                                      float,
                                      float,
                                      float) override
    {
        ++unexpectedCalls;
        return false;
    }
};

bool pianoRollEditActionUndoRedoCommitsRangeSnapshots()
{
    auto commands = std::make_shared<RecordingContentEditCommands>();

    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 42;

    const std::vector<Note> beforeNotes = {
        makeNote(0.25, 0.75, 220.0f)
    };
    const std::vector<Note> afterNotes = {
        makeNote(0.25, 1.00, 246.94165f)
    };
    const std::vector<PitchCorrectionSegment> beforeSegments = {
        PitchCorrectionSegment(10, 20, {220.0f, 221.0f}, PitchCorrectionSegment::Source::NoteBased)
    };
    const std::vector<PitchCorrectionSegment> afterSegments = {
        PitchCorrectionSegment(10, 24, {246.0f, 247.0f}, PitchCorrectionSegment::Source::NoteBased)
    };
    const ContentEditRangeFrames range {10, 24};

    UndoManager undoManager;
    undoManager.addAction(std::make_unique<PianoRollEditAction>(
        commands,
        key,
        "edit note",
        beforeNotes,
        afterNotes,
        beforeSegments,
        afterSegments,
        range));

    expect(undoManager.canUndo(), "UndoManager must own the PianoRoll edit action");
    expect(!undoManager.canRedo(), "UndoManager must not expose redo before undo");

    undoManager.undo();
    expect(commands->commits.size() == 1, "undo must commit once");
    if (commands->commits.size() >= 1) {
        const auto& undoCommit = commands->commits[0];
        expect(undoCommit.key == key, "undo must use the original ContentKey");
        expect(sameNotes(undoCommit.notes, beforeNotes), "undo must commit before notes");
        expect(undoCommit.segments.size() == beforeSegments.size(), "undo must commit before segments");
        expect(undoCommit.range.startFrame == range.startFrame, "undo must preserve range start");
        expect(undoCommit.range.endFrameExclusive == range.endFrameExclusive, "undo must preserve range end");
    }

    expect(undoManager.canRedo(), "UndoManager must expose redo after undo");
    undoManager.redo();
    expect(commands->commits.size() == 2, "redo must commit once");
    if (commands->commits.size() >= 2) {
        const auto& redoCommit = commands->commits[1];
        expect(redoCommit.key == key, "redo must use the original ContentKey");
        expect(sameNotes(redoCommit.notes, afterNotes), "redo must commit after notes");
        expect(redoCommit.segments.size() == afterSegments.size(), "redo must commit after segments");
        expect(redoCommit.range.startFrame == range.startFrame, "redo must preserve range start");
        expect(redoCommit.range.endFrameExclusive == range.endFrameExclusive, "redo must preserve range end");
    }

    expect(commands->unexpectedCalls == 0, "PianoRollEditAction must only call commitNotesAndSegments");
    return failures == 0;
}

} // namespace

int main()
{
    std::cout << "=== OpenTune PianoRoll Behavior Tests ===\n\n";

    pianoRollEditActionUndoRedoCommitsRangeSnapshots();

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL PIANOROLL BEHAVIOR TESTS PASSED\n";
        return 0;
    }

    std::cout << failures << " PIANOROLL BEHAVIOR TEST(S) FAILED\n";
    return 1;
}
