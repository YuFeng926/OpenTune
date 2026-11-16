#include "SelectedNoteParameterEdit.h"
#include "Content/EditableContentSnapshot.h"
#include <algorithm>
#include <utility>

namespace OpenTune {
namespace {

std::vector<Note> notesInRange(const std::vector<Note>& notes, double startSeconds, double endSeconds)
{
    std::vector<Note> result;
    for (const auto& note : notes) {
        if (note.startTime < endSeconds && note.endTime > startSeconds)
            result.push_back(note);
    }
    return result;
}

std::vector<PitchCorrectionSegment> segmentsInRange(
    const std::vector<PitchCorrectionSegment>& segments, int startFrame, int endFrame)
{
    std::vector<PitchCorrectionSegment> result;
    for (const auto& segment : segments) {
        if (segment.endFrame <= startFrame || segment.startFrame >= endFrame)
            continue;

        const int clippedStart = std::max(segment.startFrame, startFrame);
        const int clippedEnd = std::min(segment.endFrame, endFrame);
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

} // namespace

std::optional<SelectedNoteParameterPatch> planSelectedNoteParameterEdit(
    const ContentEditCommands& commands,
    ContentKey contentKey,
    const EditableContentSnapshot& snapshot,
    const std::vector<int>& selectedNoteIndices,
    uint64_t expectedNotesRevision,
    AudioEditingScheme::ParameterId parameterId,
    float value,
    float retuneSpeedDefault,
    float vibratoDepthDefault,
    float vibratoRateDefault)
{
    if (!contentKey.isValid() || snapshot.notesRevision != expectedNotesRevision
        || snapshot.pitchCurve == nullptr
        || parameterId == AudioEditingScheme::ParameterId::NoteSplit)
        return std::nullopt;

    const auto curveSnapshot = snapshot.pitchCurve;
    const F0Timeline timeline(curveSnapshot->getHopSize(), curveSnapshot->getSampleRate(),
                              static_cast<int>(curveSnapshot->size()));
    if (timeline.isEmpty())
        return std::nullopt;

    ContentNoteValueEditRequest request;
    for (const int noteIndex : selectedNoteIndices) {
        if (noteIndex >= 0 && noteIndex < static_cast<int>(snapshot.notes.size()))
            request.edits.push_back(ContentNoteValueEdit{
                ContentNoteValueEdit::Kind::Parameters,
                static_cast<std::size_t>(noteIndex)});
    }
    if (request.edits.empty())
        return std::nullopt;

    switch (parameterId) {
        case AudioEditingScheme::ParameterId::RetuneSpeed:
            for (auto& edit : request.edits) edit.retuneSpeed = value;
            break;
        case AudioEditingScheme::ParameterId::VibratoDepth:
            for (auto& edit : request.edits) edit.vibratoDepth = value;
            break;
        case AudioEditingScheme::ParameterId::VibratoRate:
            for (auto& edit : request.edits) edit.vibratoRate = value;
            break;
        case AudioEditingScheme::ParameterId::NoteSplit: return std::nullopt;
    }
    auto valuePlan = commands.planNoteValueEdit(snapshot.notes, request);
    SelectedNoteParameterPatch patch;
    patch.contentKey = contentKey;
    patch.changed = !valuePlan.changedPositions.empty();
    if (!patch.changed)
        return patch;

    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    for (const auto position : valuePlan.changedPositions) {
        dirtyStartTime = std::min(dirtyStartTime, valuePlan.notes[position].startTime);
        dirtyEndTime = std::max(dirtyEndTime, valuePlan.notes[position].endTime);
    }
    const auto editRange = timeline.rangeForTimes(dirtyStartTime, dirtyEndTime);
    if (editRange.isEmpty())
        return patch;

    auto derivedCurve = PitchCurve::fromSnapshot(curveSnapshot);
    const auto derivedSnapshot = derivedCurve->applyCorrectionToRange(
        valuePlan.notes,
        editRange.startFrame,
        editRange.endFrameExclusive,
        static_cast<float>(snapshot.pitchShiftSettings.getPitchRatio()),
        retuneSpeedDefault,
        vibratoDepthDefault,
        vibratoRateDefault);

    patch.affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
        editRange.startFrame, editRange.endFrameExclusive, timeline.endFrameExclusive());
    const double affectedStartSeconds = timeline.timeAtFrame(patch.affectedRange.startFrame);
    const double affectedEndSeconds = timeline.timeAtFrame(patch.affectedRange.endFrameExclusive);
    patch.beforeNotes = notesInRange(snapshot.notes, affectedStartSeconds, affectedEndSeconds);
    patch.afterNotes = notesInRange(valuePlan.notes, affectedStartSeconds, affectedEndSeconds);
    patch.beforeSegments = segmentsInRange(
        curveSnapshot->getCorrectionSegments(),
        patch.affectedRange.startFrame,
        patch.affectedRange.endFrameExclusive);
    patch.afterSegments = segmentsInRange(
        derivedSnapshot->getCorrectionSegments(),
        patch.affectedRange.startFrame,
        patch.affectedRange.endFrameExclusive);
    return patch;
}

} // namespace OpenTune
