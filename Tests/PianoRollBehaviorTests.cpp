#include "../Source/Utils/PianoRollEditAction.h"
#include "../Source/Utils/UndoManager.h"
#include "../Source/Standalone/UI/PianoRoll/InteractionState.h"
#include "../Source/Content/EditableContentSnapshot.h"
#include "../Source/Content/CaptureSegmentContent.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef OPENTUNE_SOURCE_DIR
#error "OPENTUNE_SOURCE_DIR must be defined by CMake"
#endif

using namespace OpenTune;

namespace {

int failures = 0;

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        ++failures;
        std::cout << "[FAIL] " << message << "\n";
    }
}

std::filesystem::path sourcePath(std::string_view relative)
{
    return std::filesystem::path(OPENTUNE_SOURCE_DIR) / std::filesystem::path(relative);
}

std::string readText(std::string_view relative)
{
    const auto path = sourcePath(relative);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool contains(std::string_view text, std::string_view token)
{
    return text.find(token) != std::string_view::npos;
}

std::string extractBracedBlock(std::string_view text, size_t markerPos)
{
    if (markerPos == std::string_view::npos)
        return {};

    const size_t bracePos = text.find('{', markerPos);
    if (bracePos == std::string_view::npos)
        return {};

    int depth = 0;
    for (size_t i = bracePos; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0)
                return std::string(text.substr(markerPos, i - markerPos + 1));
        }
    }
    return {};
}

std::string extractBlockByMarker(std::string_view text, std::string_view marker)
{
    return extractBracedBlock(text, text.find(marker));
}

void expectTokens(std::string_view blockName,
                  std::string_view text,
                  std::initializer_list<std::string_view> tokens)
{
    for (const auto token : tokens)
        expect(contains(text, token), std::string(blockName) + " missing token: " + std::string(token));
}

void expectNoTokens(std::string_view blockName,
                    std::string_view text,
                    std::initializer_list<std::string_view> tokens)
{
    for (const auto token : tokens)
        expect(!contains(text, token), std::string(blockName) + " contains forbidden token: " + std::string(token));
}

Note makeNote(double startSeconds, double endSeconds, float pitchHz, float pitchOffset = 0.0f)
{
    Note note;
    note.startTime = startSeconds;
    note.endTime = endSeconds;
    note.pitch = pitchHz;
    note.originalPitch = pitchHz;
    note.pitchOffset = pitchOffset;
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
            || a[i].originalPitch != b[i].originalPitch
            || a[i].pitchOffset != b[i].pitchOffset) {
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

    ContentCommitSnapshot commitNotePatch(ContentKey, ContentNoteRangePatch) override
    {
        ++unexpectedCalls;
        return {};
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

void selectAllFeedbackPathCoversEveryNote()
{
    NoteSelectionState selection;
    selection.selectAll(3);

    expect((selection.selectedIndices == std::vector<int>{0, 1, 2}),
           "selectAll must select every note index");
    expect(selection.anchorIndex == 2,
           "selectAll must set anchor to the last selected note");

    selection.trimToNoteCount(2);
    expect((selection.selectedIndices == std::vector<int>{0, 1}),
           "selection trimming must keep selected indices inside the note range");
    expect(selection.anchorIndex == 1,
           "selection trimming must keep anchor inside the note range");

    const auto handler = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto selectAllBlock = extractBlockByMarker(
        handler, "if (KeyShortcutConfig::matchesShortcut(shortcutSettings, KeyShortcutConfig::ShortcutId::SelectAll");

    expectTokens("SelectAll shortcut path",
                 selectAllBlock,
                 {"committedNotes(ctx_)", "selectAllNotes(notes);", "updateF0SelectionFromNotes(committed)", "invalidateSelectionFeedback"});
    expectNoTokens("SelectAll shortcut path",
                   selectAllBlock,
                   {"invalidateLiveNotes", "invalidateInteractionPreview", "prepareVisibleContentTiles", "beginNoteDraft", "commitNoteDraft"});
}

void dragPreviewUsesWorkingNotesAndLiveInvalidation()
{
    NoteInteractionDraft draft;
    draft.active = true;
    draft.baselineNotes = {makeNote(0.0, 1.0, 220.0f)};
    draft.workingNotes = draft.baselineNotes;
    draft.workingNotes[0].pitchOffset = 2.0f;
    draft.workingNotes[0].dirty = true;

    expect(draft.baselineNotes[0].pitchOffset == 0.0f,
           "draft baseline must preserve the committed note pitch offset");
    expect(draft.workingNotes[0].pitchOffset == 2.0f,
           "draft working notes must carry the live dragged pitch offset");

    const auto handler = readText("Source/Standalone/UI/PianoRoll/PianoRollToolHandler.cpp");
    const auto dragBlock = extractBlockByMarker(
        handler, "if (ctx_.getState().noteDrag.draggedNoteIndex >= 0)");

    expectTokens("note drag path",
                 dragBlock,
                 {"ctx_.beginNoteDraft()", "auto& notes = workingDraftNotes(ctx_)", "resetDraftNotesToBaseline(ctx_)", "pitchOffset = snappedOffset", "invalidateLiveNotes"});
    expectNoTokens("note drag path",
                   dragBlock,
                   {"invalidateSelectionFeedback", "prepareVisibleContentTiles", "renderer_->drawNotes", "contentCache_"});
}

void pianoRollEditActionUndoRedoCommitsRangeSnapshots()
{
    auto commands = std::make_shared<RecordingContentEditCommands>();

    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 42;

    const std::vector<Note> beforeNotes = {
        makeNote(0.25, 0.75, 220.0f)
    };
    const std::vector<Note> afterNotes = {
        makeNote(0.25, 1.00, 246.94165f, 1.0f)
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
}

void captureSegmentContentAudioBufferBirthsIdentityTimeGrid()
{
    CaptureSegmentContent content(1);

    juce::AudioBuffer<float> buffer(2, 480);
    buffer.clear();
    const double sampleRate = 48000.0;

    content.applyAudioBuffer(buffer, sampleRate);

    const auto snap = content.snapshotContent();

    expect(snap->audioBuffer != nullptr, "audioBuffer must not be null after applyAudioBuffer");
    expect(snap->timeGrid != nullptr, "timeGrid must not be null after applyAudioBuffer");
    expect(snap->timeGrid->isIdentity(), "timeGrid must be identity after applyAudioBuffer");

    const double expectedDuration = static_cast<double>(buffer.getNumSamples()) / sampleRate;
    expect(snap->timeGrid->totalDurationSeconds() == expectedDuration,
           "timeGrid totalDurationSeconds must match buffer duration");
    expect(snap->timeGridRevision == 1, "timeGridRevision must be 1 after first applyAudioBuffer");
}

} // namespace

int main()
{
    std::cout << "=== OpenTune PianoRoll Behavior Tests ===\n\n";

    try {
        selectAllFeedbackPathCoversEveryNote();
        dragPreviewUsesWorkingNotesAndLiveInvalidation();
        pianoRollEditActionUndoRedoCommitsRangeSnapshots();
        captureSegmentContentAudioBufferBirthsIdentityTimeGrid();
    } catch (const std::exception& e) {
        ++failures;
        std::cout << "[FAIL] uncaught exception: " << e.what() << "\n";
    }

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL PIANOROLL BEHAVIOR TESTS PASSED\n";
        return 0;
    }

    std::cout << failures << " PIANOROLL BEHAVIOR TEST(S) FAILED\n";
    return 1;
}
