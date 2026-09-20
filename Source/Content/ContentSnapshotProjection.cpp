#include "ContentSnapshotProjection.h"

namespace OpenTune {

EditableContentSnapshot makeContentSnapshot(const ContentState& state)
{
    EditableContentSnapshot snap;
    snap.sourceWindow = state.sourceWindow;
    snap.audioBuffer = state.audioBuffer;
    // audioSampleRate 与 audioBuffer 绑定：无 PCM 的内容（ARA/Capture 未落音频）不报告采样率。
    snap.audioSampleRate = state.audioBuffer != nullptr ? state.sampleRate : 0.0;
    snap.audioRevision = state.audioRevision;
    snap.notes = state.notes;
    snap.pitchCurve = state.analysis.pitchCurve;
    snap.timeGrid = state.timeGrid;
    snap.pitchShiftSettings = state.pitchShiftSettings;
    snap.originalF0State = state.analysis.originalF0State;
    snap.detectedKey = state.analysis.detectedKey;
    snap.silentGaps = state.analysis.silentGaps;
    snap.referenceFeatures = state.analysis.referenceFeatures;
    snap.volumeEnvelope = state.volumeEnvelope;
    snap.notesRevision = state.notesRevision;
    snap.noteTopologyInitialized = state.noteTopologyInitialized;
    snap.pitchRevision = state.pitchRevision;
    snap.timeGridRevision = state.timeGridRevision;
    snap.pitchShiftRevision = state.pitchShiftRevision;
    snap.outputGainRevision = state.outputGainRevision;
    snap.contentRevision = state.contentRevision;
    return snap;
}

ContentState contentStateFromSnapshot(const EditableContentSnapshot& snapshot)
{
    ContentState state;
    state.sourceWindow = snapshot.sourceWindow;
    state.audioBuffer = snapshot.audioBuffer;
    state.sampleRate = snapshot.audioSampleRate;
    state.notes = snapshot.notes;
    state.timeGrid = snapshot.timeGrid;
    state.pitchShiftSettings = snapshot.pitchShiftSettings;
    state.volumeEnvelope = snapshot.volumeEnvelope;
    state.notesRevision = snapshot.notesRevision;
    state.noteTopologyInitialized = snapshot.noteTopologyInitialized;
    state.pitchRevision = snapshot.pitchRevision;
    state.timeGridRevision = snapshot.timeGridRevision;
    state.pitchShiftRevision = snapshot.pitchShiftRevision;
    state.outputGainRevision = snapshot.outputGainRevision;
    state.contentRevision = snapshot.contentRevision;
    state.audioRevision = snapshot.audioRevision;
    state.analysis.pitchCurve = snapshot.pitchCurve;
    state.analysis.setOriginalF0State(snapshot.originalF0State);
    state.analysis.detectedKey = snapshot.detectedKey;
    state.analysis.silentGaps = snapshot.silentGaps;
    state.analysis.referenceFeatures = snapshot.referenceFeatures;
    return state;
}

} // namespace OpenTune
