#include "Content/ContentEditCommands.h"
#include "Content/EditableContentSnapshot.h"
#include "Utils/PianoRollEditAction.h"
#include "Utils/PitchShiftEditAction.h"
#include "Utils/SelectedNoteParameterEdit.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace OpenTune;

bool sameNote(const Note& left, const Note& right)
{
    return left.startTime == right.startTime
        && left.endTime == right.endTime
        && left.pitch == right.pitch
        && left.originalPitch == right.originalPitch
        && left.retuneSpeed == right.retuneSpeed
        && left.vibratoDepth == right.vibratoDepth
        && left.vibratoRate == right.vibratoRate
        && left.dirty == right.dirty;
}

bool sameNotes(const std::vector<Note>& left, const std::vector<Note>& right)
{
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (!sameNote(left[i], right[i]))
            return false;
    }
    return true;
}

bool sameSegment(const PitchCorrectionSegment& left, const PitchCorrectionSegment& right)
{
    return left.startFrame == right.startFrame
        && left.endFrame == right.endFrame
        && left.f0Data == right.f0Data
        && left.baseF0Data == right.baseF0Data
        && left.parameterSnapshot.retuneSpeed == right.parameterSnapshot.retuneSpeed
        && left.parameterSnapshot.vibratoDepth == right.parameterSnapshot.vibratoDepth
        && left.parameterSnapshot.vibratoRate == right.parameterSnapshot.vibratoRate
        && left.parameterSnapshot.pitchDriftScale == right.parameterSnapshot.pitchDriftScale
        && left.source == right.source;
}

bool sameSegments(const std::vector<PitchCorrectionSegment>& left,
                  const std::vector<PitchCorrectionSegment>& right)
{
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (!sameSegment(left[i], right[i]))
            return false;
    }
    return true;
}

std::vector<Note> notesInRange(const std::vector<Note>& notes, double start, double end)
{
    std::vector<Note> result;
    for (const auto& note : notes) {
        if (note.startTime < end && note.endTime > start)
            result.push_back(note);
    }
    return result;
}

std::vector<PitchCorrectionSegment> segmentsInRange(
    const std::vector<PitchCorrectionSegment>& segments, int start, int end)
{
    std::vector<PitchCorrectionSegment> result;
    for (const auto& segment : segments) {
        if (segment.endFrame <= start || segment.startFrame >= end)
            continue;
        const int clippedStart = std::max(segment.startFrame, start);
        const int clippedEnd = std::min(segment.endFrame, end);
        const int offset = clippedStart - segment.startFrame;
        const int length = clippedEnd - clippedStart;
        if (offset < 0 || length <= 0 || offset + length > static_cast<int>(segment.f0Data.size()))
            continue;
        auto clipped = segment;
        clipped.startFrame = clippedStart;
        clipped.endFrame = clippedEnd;
        clipped.f0Data.assign(segment.f0Data.begin() + offset,
                              segment.f0Data.begin() + offset + length);
        result.push_back(std::move(clipped));
    }
    return result;
}

EditableContentSnapshot makeSnapshot()
{
    EditableContentSnapshot snapshot;
    Note selected;
    selected.startTime = 0.2;
    selected.endTime = 0.5;
    selected.pitch = 246.94f;
    selected.originalPitch = 220.0f;
    selected.retuneSpeed = 0.15f;
    selected.vibratoDepth = 0.0f;
    selected.vibratoRate = 7.5f;

    Note adjacent = selected;
    adjacent.startTime = 0.55;
    adjacent.endTime = 0.8;
    adjacent.pitch = 261.63f;
    adjacent.originalPitch = 220.0f;
    snapshot.notes = {selected, adjacent};
    snapshot.pitchShiftSettings = PitchShiftSettings{2, 0};

    std::vector<float> originalF0(200, 220.0f);
    std::vector<float> originalEnergy(200, 1.0f);
    PitchCorrectionSegment crossing(0, 10,
                                    {220.0f, 221.0f, 222.0f, 223.0f, 224.0f,
                                     225.0f, 226.0f, 227.0f, 228.0f, 229.0f},
                                    PitchCorrectionSegment::Source::HandDraw);
    crossing.baseF0Data = {210.0f, 211.0f, 212.0f, 213.0f, 214.0f,
                           215.0f, 216.0f, 217.0f, 218.0f, 219.0f};
    snapshot.pitchCurve = std::make_shared<const PitchCurveSnapshot>(
        std::move(originalF0), std::move(originalEnergy),
        std::vector<PitchCorrectionSegment>{std::move(crossing)}, 10, 1000.0);
    return snapshot;
}

bool checkPurePlanForParameter(AudioEditingScheme::ParameterId id, float value,
                               float retuneDefault, float depthDefault, float rateDefault)
{
    const auto snapshot = makeSnapshot();
    const ContentKey key{DomainKind::StandaloneClip, 77, 0};
    const std::vector<int> selectedIndices{0};
    const auto patch = planSelectedNoteParameterEdit(
        key, snapshot, selectedIndices, snapshot.notesRevision, id, value,
        retuneDefault, depthDefault, rateDefault);
    if (!patch || !patch->changed || patch->contentKey != key)
        return false;

    auto legacyAfterNotes = snapshot.notes;
    auto& selected = legacyAfterNotes[0];
    selected.dirty = true;
    switch (id) {
        case AudioEditingScheme::ParameterId::RetuneSpeed: selected.retuneSpeed = value; break;
        case AudioEditingScheme::ParameterId::VibratoDepth: selected.vibratoDepth = value; break;
        case AudioEditingScheme::ParameterId::VibratoRate: selected.vibratoRate = value; break;
        case AudioEditingScheme::ParameterId::NoteSplit: return false;
    }

    const auto curveSnapshot = snapshot.pitchCurve;
    const F0Timeline timeline(curveSnapshot->getHopSize(), curveSnapshot->getSampleRate(),
                              static_cast<int>(curveSnapshot->size()));
    const auto editRange = timeline.rangeForTimes(selected.startTime, selected.endTime);
    auto legacyCurve = PitchCurve::fromSnapshot(curveSnapshot);
    const auto legacyAfterCurve = legacyCurve->applyCorrectionToRange(
        legacyAfterNotes,
        editRange.startFrame,
        editRange.endFrameExclusive,
        static_cast<float>(snapshot.pitchShiftSettings.getPitchRatio()),
        retuneDefault,
        depthDefault,
        rateDefault);
    const auto affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
        editRange.startFrame, editRange.endFrameExclusive, timeline.endFrameExclusive());
    const double affectedStart = timeline.timeAtFrame(affectedRange.startFrame);
    const double affectedEnd = timeline.timeAtFrame(affectedRange.endFrameExclusive);
    if (patch->beforeSegments.size() != 1
        || patch->beforeSegments.front().startFrame != affectedRange.startFrame
        || patch->beforeSegments.front().endFrame != 10
        || patch->beforeSegments.front().f0Data != std::vector<float>({224.0f, 225.0f, 226.0f,
                                                                        227.0f, 228.0f, 229.0f})
        || patch->beforeSegments.front().baseF0Data != snapshot.pitchCurve->getCorrectionSegments().front().baseF0Data)
        return false;

    return patch->affectedRange.startFrame == affectedRange.startFrame
        && patch->affectedRange.endFrameExclusive == affectedRange.endFrameExclusive
        && sameNotes(patch->beforeNotes, notesInRange(snapshot.notes, affectedStart, affectedEnd))
        && sameNotes(patch->afterNotes, notesInRange(legacyAfterNotes, affectedStart, affectedEnd))
        && sameSegments(patch->beforeSegments,
                        segmentsInRange(curveSnapshot->getCorrectionSegments(),
                                        affectedRange.startFrame, affectedRange.endFrameExclusive))
        && sameSegments(patch->afterSegments,
                        segmentsInRange(legacyAfterCurve->getCorrectionSegments(),
                                        affectedRange.startFrame, affectedRange.endFrameExclusive));
}

bool checkStaleNotesRevisionRejected()
{
    const auto snapshot = makeSnapshot();
    const ContentKey key{DomainKind::StandaloneClip, 77, 0};
    const auto patch = planSelectedNoteParameterEdit(
        key, snapshot, {0}, snapshot.notesRevision + 1,
        AudioEditingScheme::ParameterId::RetuneSpeed, 0.8f, 0.15f, 0.0f, 7.5f);
    return !patch;
}

class RecordingCommands final : public ContentEditCommands
{
public:
    ContentKey lastKey;
    std::vector<Note> lastNotes;
    std::vector<PitchCorrectionSegment> lastSegments;
    ContentEditRangeFrames lastRange;
    int commits = 0;

    bool replaceContentNotesForFullMutation(ContentKey, std::vector<Note>) override { return false; }
    ContentCommitSnapshot commitNoteTopologyPatch(ContentKey, ContentNoteRangePatch) override { return {}; }
    ContentCommitSnapshot commitNotesAndSegments(
        ContentKey key, std::vector<Note> notes, std::vector<PitchCorrectionSegment> segments,
        ContentEditRangeFrames range) override
    {
        lastKey = key;
        lastNotes = std::move(notes);
        lastSegments = std::move(segments);
        lastRange = range;
        ++commits;
        return {};
    }
    ContentCommitSnapshot commitNotesAndSegmentsWithOriginalF0(
        ContentKey, std::vector<Note>, std::vector<PitchCorrectionSegment>, ContentEditRangeFrames,
        std::vector<float>, std::optional<std::vector<F0FrameRange>>) override { return {}; }
    bool setTimeGrid(ContentKey, std::shared_ptr<const TimeGridSnapshot>) override { return false; }
    bool setDetectedKey(ContentKey, const DetectedKey&) override { return false; }
    bool applyPitchShiftState(ContentKey, const PitchShiftEditState&) override { return false; }
    std::unique_ptr<PitchShiftEditAction> commitPitchShiftEdit(
        ContentKey, const PitchShiftSettings&) override { return {}; }
    bool autoTuneContentRange(ContentKey, int, int, const NoteGeneratorParams&,
                              const std::optional<ScaleSnapConfig>&) override { return false; }
    bool generateNotesOnly(ContentKey, const NoteGeneratorParams&) override { return false; }
    bool ensureTimeToolAnchorSeed(ContentKey) override { return false; }
    ContentCommitSnapshot commitVolumeEnvelope(ContentKey, AutomationLane) override { return {}; }
    void republishPlaybackSource(ContentKey) override {}
};

bool checkEditActionUsesBoundContentKey()
{
    const ContentKey key{DomainKind::StandaloneClip, 91, 4};
    auto commands = std::make_shared<RecordingCommands>();
    Note before;
    before.startTime = 0.2;
    before.endTime = 0.4;
    before.retuneSpeed = 0.15f;
    Note after = before;
    after.retuneSpeed = 0.8f;
    PitchCorrectionSegment beforeSegment(12, 14, {220.0f, 221.0f},
                                         PitchCorrectionSegment::Source::NoteBased);
    PitchCorrectionSegment afterSegment(12, 14, {240.0f, 241.0f},
                                        PitchCorrectionSegment::Source::NoteBased);

    PianoRollEditAction action(
        commands, key, "Edit note", {before}, {after}, {beforeSegment}, {afterSegment}, {12, 14});
    action.undo();
    if (commands->commits != 1 || commands->lastKey != key
        || !sameNotes(commands->lastNotes, {before})
        || !sameSegments(commands->lastSegments, {beforeSegment}))
        return false;

    action.redo();
    return commands->commits == 2 && commands->lastKey == key
        && sameNotes(commands->lastNotes, {after})
        && sameSegments(commands->lastSegments, {afterSegment});
}

} // namespace

int main()
{
    if (!checkPurePlanForParameter(AudioEditingScheme::ParameterId::RetuneSpeed,
                                   0.8f, 0.22f, 0.0f, 7.5f)
        || !checkPurePlanForParameter(AudioEditingScheme::ParameterId::VibratoDepth,
                                      35.0f, 0.15f, 0.0f, 7.5f)
        || !checkPurePlanForParameter(AudioEditingScheme::ParameterId::VibratoRate,
                                      9.0f, 0.15f, 0.0f, 7.5f)
        || !checkStaleNotesRevisionRejected()
        || !checkEditActionUsesBoundContentKey()) {
        std::cerr << "Selected-note parameter edit regression failed\n";
        return 1;
    }
    return 0;
}
