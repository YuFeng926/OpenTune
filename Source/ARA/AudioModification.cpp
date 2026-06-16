#include "AudioModification.h"
#include <algorithm>

namespace OpenTune {

void AudioModification::updateIdentity(juce::ARAAudioModification* modification)
{
    audioModification = modification;
    persistentId = modification != nullptr
        ? juce::String(modification->getPersistentID())
        : juce::String();
}

void AudioModification::attachSource(const AudioSource& source)
{
    // Per ARA2 spec: AudioSource binding is part of content.sourceWindow,
    // single source of truth for source identity.
    const auto& shape = source.getShape();
    content.sourceWindow = SourceWindow{0, source.getIdentity().persistentId, 0.0, shape.durationSeconds()};
    cachedSourceShape_ = shape;  // 缓存 shape
}

void AudioModification::resetContent() noexcept
{
    content = AudioModificationContentState{};
    birthState = AudioModificationBirthState::Empty;
}

bool AudioModification::isRenderable() const noexcept
{
    return content.lifecycle == ContentLifecycle::Ready
        && content.sourceWindow.isValid();
}

ContentKey AudioModification::contentKey() const noexcept
{
    return contentIdentity;
}

std::shared_ptr<const EditableContentSnapshot> AudioModification::snapshotContent() const
{
    auto snap = std::make_shared<EditableContentSnapshot>();
    snap->sourceWindow = content.sourceWindow;

    // ARA2: 从缓存的 AudioSource shape 提供只读元数据
    snap->sourceSampleRate = cachedSourceShape_.sourceSampleRate;
    snap->sourceChannelCount = cachedSourceShape_.numChannels;
    snap->sourceSampleCount = cachedSourceShape_.numSamples;

    // 保持 audioSampleRate 与 audioBuffer 绑定（ARA 下为 nullptr/0.0）
    snap->audioBuffer = nullptr;  // ARA 不拥有 PCM
    snap->audioSampleRate = 0.0;  // 与 audioBuffer 一致

    // modification-scoped state
    snap->notes = content.editable.notes;
    snap->correctedSegments = content.editable.correctedSegments;
    snap->pitchCurve = content.analysis.pitchCurve;
    snap->timeGrid = content.editable.timeGrid;
    snap->pitchShiftSettings = content.editable.pitchShiftSettings;
    snap->originalF0State = content.analysis.originalF0State;
    snap->detectedKey = content.analysis.detectedKey;
    snap->silentGaps = content.analysis.silentGaps;
    snap->referenceFeatures = content.analysis.referenceFeatures;
    snap->notesRevision = content.editable.notesRevision;
    snap->pitchRevision = content.editable.pitchRevision;
    snap->timeGridRevision = content.editable.timeGridRevision;
    snap->pitchShiftRevision = content.editable.pitchShiftRevision;
    snap->contentRevision = content.contentRevision;
    return snap;
}

void AudioModification::applyNotes(const std::vector<Note>& notes)
{
    content.editable.notes = notes;
    ++content.editable.notesRevision;
    ++content.editable.contentRevision;
    ++content.contentRevision;
}

void AudioModification::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    content.analysis.pitchCurve = std::move(curve);
    ++content.editable.pitchRevision;
    ++content.editable.contentRevision;
    ++content.contentRevision;
}

void AudioModification::applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> grid)
{
    content.editable.timeGrid = std::move(grid);
    ++content.editable.timeGridRevision;
    ++content.editable.contentRevision;
    ++content.contentRevision;
}

void AudioModification::applyPitchShift(const PitchShiftSettings& settings)
{
    content.editable.pitchShiftSettings = settings;
    ++content.editable.pitchShiftRevision;
    ++content.editable.contentRevision;
    ++content.contentRevision;
}

void AudioModification::applyDetectedKey(const DetectedKey& key)
{
    content.analysis.detectedKey = key;
    ++content.contentRevision;
}

void AudioModification::applyF0Analysis(std::shared_ptr<PitchCurve> curve)
{
    content.analysis.pitchCurve = std::move(curve);
    content.analysis.originalF0State = OriginalF0State::Ready;
    content.analysis.f0Lifecycle = AnalysisLifecycle::Ready;
    ++content.analysis.analysisRevision;
    ++content.contentRevision;
}

void AudioModification::applyReferenceFeatures(const ReferenceFeatureSet& features)
{
    content.analysis.referenceFeatures = features;
    ++content.contentRevision;
}

void AudioModification::applyOriginalF0State(OriginalF0State state)
{
    if (content.analysis.originalF0State == state)
        return;
    content.analysis.originalF0State = state;
    ++content.contentRevision;
}

} // namespace OpenTune
