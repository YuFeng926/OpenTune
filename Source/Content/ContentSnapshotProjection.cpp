#include "ContentSnapshotProjection.h"
#include <cmath>

namespace OpenTune {

namespace {

std::shared_ptr<const TimeGridSnapshot> normalizeTimeGrid(
    std::shared_ptr<const TimeGridSnapshot> grid,
    const SourceWindow& sourceWindow,
    const std::shared_ptr<const juce::AudioBuffer<float>>& audioBuffer,
    double sampleRate)
{
    const double contentDuration = sourceWindow.isValid()
        ? sourceWindow.durationSeconds()
        : (audioBuffer != nullptr && sampleRate > 0.0
            ? static_cast<double>(audioBuffer->getNumSamples()) / sampleRate
            : 0.0);

    if (grid != nullptr && !grid->empty()) {
        if (contentDuration <= 0.0
            || std::abs(grid->totalDurationSeconds() - contentDuration) <= 1.0e-6)
            return grid;

        if (auto realGrid = TimeGridSnapshot::makeIdentity(contentDuration))
            return realGrid;
        return grid;
    }

    if (contentDuration > 0.0) {
        if (auto realGrid = TimeGridSnapshot::makeIdentity(contentDuration))
            return realGrid;
    }
    return TimeGridSnapshot::bootstrapIdentity();
}

} // namespace

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
    snap.timeGrid = normalizeTimeGrid(
        state.timeGrid, state.sourceWindow, state.audioBuffer, state.sampleRate);
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
    state.timeGrid = normalizeTimeGrid(
        snapshot.timeGrid, snapshot.sourceWindow, snapshot.audioBuffer, snapshot.audioSampleRate);
    state.pitchShiftSettings = snapshot.pitchShiftSettings;
    state.volumeEnvelope = snapshot.volumeEnvelope;
    state.notesRevision = snapshot.notesRevision;
    state.noteTopologyInitialized = snapshot.noteTopologyInitialized;
    state.pitchRevision = snapshot.pitchRevision;
    state.timeGridRevision = snapshot.timeGridRevision;
    // contentRevision 不随投影复制：反向投影构造的是新 ContentState（新内容身份），
    // 运行时 revision 从默认 1 开始，这不是运行时 revision restore。
    state.audioRevision = snapshot.audioRevision;
    state.analysis.pitchCurve = PitchCurve::fromSnapshot(snapshot.pitchCurve);
    state.analysis.setOriginalF0State(snapshot.originalF0State);
    state.analysis.detectedKey = snapshot.detectedKey;
    state.analysis.silentGaps = snapshot.silentGaps;
    state.analysis.referenceFeatures = snapshot.referenceFeatures;
    return state;
}

} // namespace OpenTune
