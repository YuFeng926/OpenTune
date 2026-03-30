#include "UndoAction.h"
#include "../PluginProcessor.h"

namespace OpenTune {

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
    OpenTuneAudioProcessor::ClipSnapshot snap;
    snap.audioBuffer = snapshot_.audioBuffer;
    snap.startSeconds = snapshot_.startSeconds;
    snap.gain = snapshot_.gain;
    snap.fadeInDuration = snapshot_.fadeInDuration;
    snap.fadeOutDuration = snapshot_.fadeOutDuration;
    snap.name = snapshot_.name;
    snap.colour = snapshot_.colour;
    snap.pitchCurve = snapshot_.pitchCurve;
    snap.originalF0State = static_cast<OriginalF0State>(snapshot_.originalF0State);
    snap.detectedKey.root = static_cast<Key>(snapshot_.detectedRootNote);
    snap.detectedKey.scale = static_cast<Scale>(snapshot_.detectedScaleType);
    snap.renderCache = snapshot_.renderCache;

    if (!processor_.insertClipSnapshot(trackId_, deletedIndex_, snap, clipId_)) {
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

    snapshot_.audioBuffer = fullSnap.audioBuffer;
    snapshot_.startSeconds = fullSnap.startSeconds;
    snapshot_.gain = fullSnap.gain;
    snapshot_.fadeInDuration = fullSnap.fadeInDuration;
    snapshot_.fadeOutDuration = fullSnap.fadeOutDuration;
    snapshot_.name = fullSnap.name;
    snapshot_.colour = fullSnap.colour;
    snapshot_.pitchCurve = fullSnap.pitchCurve;
    snapshot_.originalF0State = static_cast<int>(fullSnap.originalF0State);
    snapshot_.detectedRootNote = static_cast<int>(fullSnap.detectedKey.root);
    snapshot_.detectedScaleType = static_cast<int>(fullSnap.detectedKey.scale);
    snapshot_.renderCache = fullSnap.renderCache;
    hasSnapshot_ = true;

    processor_.deleteClipById(trackId_, clipId_, nullptr, nullptr);
}

void ClipCreateAction::redo()
{
    if (!hasSnapshot_) return;

    OpenTuneAudioProcessor::ClipSnapshot snap;
    snap.audioBuffer = snapshot_.audioBuffer;
    snap.startSeconds = snapshot_.startSeconds;
    snap.gain = snapshot_.gain;
    snap.fadeInDuration = snapshot_.fadeInDuration;
    snap.fadeOutDuration = snapshot_.fadeOutDuration;
    snap.name = snapshot_.name;
    snap.colour = snapshot_.colour;
    snap.pitchCurve = snapshot_.pitchCurve;
    snap.originalF0State = static_cast<OriginalF0State>(snapshot_.originalF0State);
    snap.detectedKey.root = static_cast<Key>(snapshot_.detectedRootNote);
    snap.detectedKey.scale = static_cast<Scale>(snapshot_.detectedScaleType);
    snap.renderCache = snapshot_.renderCache;

    processor_.insertClipSnapshot(trackId_, -1, snap, clipId_);
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
