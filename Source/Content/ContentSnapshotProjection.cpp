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
    snap.pitchCurve = state.analysis.pitchCurve != nullptr
        ? state.analysis.pitchCurve->getSnapshot()
        : nullptr;
    snap.timeGrid = state.timeGrid;
    if (snap.timeGrid == nullptr)
    {
        double durationSeconds = state.sourceWindow.durationSeconds();
        if (durationSeconds <= 0.0 && state.audioBuffer != nullptr && state.sampleRate > 0.0)
            durationSeconds = static_cast<double>(state.audioBuffer->getNumSamples()) / state.sampleRate;
        if (durationSeconds > 0.0)
            snap.timeGrid = TimeGridSnapshot::makeIdentity(durationSeconds);
    }
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
    if (snapshot.pitchCurve != nullptr)
    {
        auto pitchCurve = std::make_shared<PitchCurve>();
        pitchCurve->setHopSize(snapshot.pitchCurve->getHopSize());
        pitchCurve->setSampleRate(snapshot.pitchCurve->getSampleRate());
        pitchCurve->setOriginalF0(snapshot.pitchCurve->getOriginalF0());
        pitchCurve->setOriginalEnergy(snapshot.pitchCurve->getOriginalEnergy());
        pitchCurve->replaceCorrectionSegments(snapshot.pitchCurve->getCorrectionSegments());
        state.analysis.pitchCurve = std::move(pitchCurve);
    }
    state.analysis.setOriginalF0State(snapshot.originalF0State);
    state.analysis.detectedKey = snapshot.detectedKey;
    state.analysis.silentGaps = snapshot.silentGaps;
    state.analysis.referenceFeatures = snapshot.referenceFeatures;
    return state;
}

} // namespace OpenTune
