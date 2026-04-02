#include "UndoAction.h"
#include "../PluginProcessor.h"
#include <cmath>

namespace OpenTune {

namespace {

bool nearlyEqualFloat(float a, float b, float epsilon)
{
    if (std::abs(a - b) < epsilon) return true;
    if (std::abs(a) < epsilon && std::abs(b) < epsilon) return true;
    return false;
}

}

std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot>
CorrectedSegmentsChangeAction::captureSegments(const std::shared_ptr<PitchCurve>& curve)
{
    if (!curve) {
        return {};
    }

    auto snapshot = curve->getSnapshot();
    const auto& segments = snapshot->getCorrectedSegments();
    std::vector<SegmentSnapshot> captured;
    captured.reserve(segments.size());
    for (const auto& seg : segments) {
        captured.emplace_back(seg);
    }
    return captured;
}

bool CorrectedSegmentsChangeAction::snapshotsEquivalent(const std::vector<SegmentSnapshot>& a,
                                                        const std::vector<SegmentSnapshot>& b,
                                                        float epsilon)
{
    if (a.size() != b.size()) return false;

    for (size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.startFrame != y.startFrame) return false;
        if (x.endFrame != y.endFrame) return false;
        if (x.source != y.source) return false;
        if (!nearlyEqualFloat(x.retuneSpeed, y.retuneSpeed, epsilon)) return false;
        if (!nearlyEqualFloat(x.vibratoDepth, y.vibratoDepth, epsilon)) return false;
        if (!nearlyEqualFloat(x.vibratoRate, y.vibratoRate, epsilon)) return false;
        if (x.f0Data.size() != y.f0Data.size()) return false;
        for (size_t j = 0; j < x.f0Data.size(); ++j) {
            if (!nearlyEqualFloat(x.f0Data[j], y.f0Data[j], epsilon)) return false;
        }
    }

    return true;
}

std::function<void(const std::vector<CorrectedSegmentsChangeAction::SegmentSnapshot>&)>
CorrectedSegmentsChangeAction::makeCurveApplier(const std::shared_ptr<PitchCurve>& curve)
{
    return [curve](const std::vector<SegmentSnapshot>& snapshots) {
        if (!curve) {
            return;
        }

        std::vector<CorrectedSegment> restored;
        restored.reserve(snapshots.size());
        for (const auto& snap : snapshots) {
            CorrectedSegment seg;
            seg.startFrame = snap.startFrame;
            seg.endFrame = snap.endFrame;
            seg.f0Data = snap.f0Data;
            seg.source = snap.source;
            seg.retuneSpeed = snap.retuneSpeed;
            seg.vibratoDepth = snap.vibratoDepth;
            seg.vibratoRate = snap.vibratoRate;
            restored.push_back(std::move(seg));
        }
        curve->replaceCorrectedSegments(restored);
    };
}

std::unique_ptr<CorrectedSegmentsChangeAction>
CorrectedSegmentsChangeAction::createForCurve(
    uint64_t clipId,
    const std::vector<SegmentSnapshot>& oldSegments,
    const std::vector<SegmentSnapshot>& newSegments,
    const std::shared_ptr<PitchCurve>& curve,
    int affectedStartFrame,
    int affectedEndFrame,
    const juce::String& description)
{
    ChangeInfo info;
    info.affectedStartFrame = affectedStartFrame;
    info.affectedEndFrame = affectedEndFrame;
    info.description = description;

    return std::make_unique<CorrectedSegmentsChangeAction>(
        clipId,
        oldSegments,
        newSegments,
        makeCurveApplier(curve),
        info);
}

// === NotesChangeAction 实现 ===

void NotesChangeAction::undo()
{
    processor_.setClipNotesById(trackId_, clipId_, oldNotes_);
}

void NotesChangeAction::redo()
{
    processor_.setClipNotesById(trackId_, clipId_, newNotes_);
}

// === ClipSplitAction 实现 ===

void ClipSplitAction::undo()
{
    processor_.mergeSplitClips(trackId_, originalClipId_, result_.newClipId, originalClipIndex_);
    processor_.setSelectedClip(trackId_, originalClipIndex_);
}

void ClipSplitAction::redo()
{
    processor_.splitClipAtSeconds(trackId_, originalClipIndex_, result_.splitSeconds);
    processor_.setSelectedClip(trackId_, newClipIndex_);
}

// === ClipGainChangeAction 实现 ===

void ClipGainChangeAction::undo()
{
    processor_.setClipGainById(trackId_, clipId_, oldGain_);
}

void ClipGainChangeAction::redo()
{
    processor_.setClipGainById(trackId_, clipId_, newGain_);
}

// === ClipMoveAction 实现 ===

void ClipMoveAction::undo()
{
    processor_.setClipStartSecondsById(trackId_, clipId_, oldStartSeconds_);
}

void ClipMoveAction::redo()
{
    processor_.setClipStartSecondsById(trackId_, clipId_, newStartSeconds_);
}

// === ClipDeleteAction 实现 ===

void ClipDeleteAction::undo()
{
    if (!processor_.insertClipSnapshot(trackId_, deletedIndex_, snapshot_, clipId_)) {
        return;
    }
    int restoredIndex = processor_.findClipIndexById(trackId_, clipId_);
    if (restoredIndex >= 0) {
        processor_.setSelectedClip(trackId_, restoredIndex);
    }
}

void ClipDeleteAction::redo()
{
    processor_.deleteClipById(trackId_, clipId_, nullptr, nullptr);
}

// === ClipCreateAction 实现 ===

void ClipCreateAction::undo()
{
    OpenTuneAudioProcessor::ClipSnapshot fullSnap;
    if (!processor_.getClipSnapshot(trackId_, clipId_, fullSnap)) return;

    snapshot_ = std::move(fullSnap);
    hasSnapshot_ = true;

    processor_.deleteClipById(trackId_, clipId_, nullptr, nullptr);
}

void ClipCreateAction::redo()
{
    if (!hasSnapshot_) return;

    processor_.insertClipSnapshot(trackId_, -1, snapshot_, clipId_);
}

// === TrackMuteAction 实现 ===

void TrackMuteAction::undo()
{
    processor_.setTrackMuted(trackId_, oldMuted_);
    if (uiUpdater_) uiUpdater_(trackId_, oldMuted_);
}

void TrackMuteAction::redo()
{
    processor_.setTrackMuted(trackId_, newMuted_);
    if (uiUpdater_) uiUpdater_(trackId_, newMuted_);
}

// === TrackSoloAction 实现 ===

void TrackSoloAction::undo()
{
    processor_.setTrackSolo(trackId_, oldSolo_);
    if (uiUpdater_) uiUpdater_(trackId_, oldSolo_);
}

void TrackSoloAction::redo()
{
    processor_.setTrackSolo(trackId_, newSolo_);
    if (uiUpdater_) uiUpdater_(trackId_, newSolo_);
}

// === TrackVolumeAction 实现 ===

void TrackVolumeAction::undo()
{
    processor_.setTrackVolume(trackId_, oldVolume_);
    if (uiUpdater_) uiUpdater_(trackId_, oldVolume_);
}

void TrackVolumeAction::redo()
{
    processor_.setTrackVolume(trackId_, newVolume_);
    if (uiUpdater_) uiUpdater_(trackId_, newVolume_);
}

// === ClipCrossTrackMoveAction 实现 ===

void ClipCrossTrackMoveAction::undo()
{
    // 从目标轨道移回源轨道
    processor_.moveClipToTrack(targetTrackId_, sourceTrackId_, clipId_, oldStartSeconds_);
}

void ClipCrossTrackMoveAction::redo()
{
    // 从源轨道移到目标轨道
    processor_.moveClipToTrack(sourceTrackId_, targetTrackId_, clipId_, newStartSeconds_);
}

} // namespace OpenTune
