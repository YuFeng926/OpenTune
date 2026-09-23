#include "AudioModification.h"
#include "../Content/ContentSnapshotProjection.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace OpenTune {

void AudioModification::updateIdentity(juce::ARAAudioModification* modification)
{
    audioModification = modification;
    persistentId = modification != nullptr
        ? juce::String(modification->getPersistentID())
        : juce::String();
}

bool AudioModification::attachSource(const AudioSource& source)
{
    if (content.has_value())
    {
        if (content->sourceWindow.sourcePersistentId != source.getIdentity().persistentId)
            return false;

        cachedSourceShape_ = source.getShape();
        return true;
    }

    // 端点 0.0 / numSamples / sourceSampleRate 本身落在 source sample 网格上，
    // 无需再经 helper 对齐。
    const SourceWindow newWindow{
        0,
        source.getIdentity().persistentId,
        0.0,
        source.getShape().durationSeconds()
    };

    const auto grid = TimeGridSnapshot::makeIdentity(newWindow.durationSeconds());
    if (grid == nullptr)
        return false;

    ContentState state;
    state.sourceWindow = newWindow;
    state.timeGrid = std::move(grid);
    state.timeGridRevision = 1;
    content.emplace(std::move(state));
    birthState = AudioModificationBirthState::WaitingForSource;
    cachedSourceShape_ = source.getShape();
    return true;
}

void AudioModification::resetContent() noexcept
{
    ++birthRevision;
    content.reset();
    originalF0InputStamp.reset();
    birthState = AudioModificationBirthState::Empty;
}

void AudioModification::invalidateDerivedContent() noexcept
{
    if (!content.has_value())
        return;

    ++birthRevision;
    content->analysis = AnalysisState{};
    originalF0InputStamp.reset();
    birthState = AudioModificationBirthState::WaitingForSource;
}

bool AudioModification::isRenderable() const noexcept
{
    if (!content.has_value())
        return false;
    if (birthState != AudioModificationBirthState::Ready)
        return false;
    return content->sourceWindow.isValid();
}

ContentKey AudioModification::contentKey() const noexcept
{
    return contentIdentity;
}

std::shared_ptr<const EditableContentSnapshot> AudioModification::snapshotContent() const
{
    if (!content.has_value())
        return nullptr;

    auto snap = std::make_shared<EditableContentSnapshot>(makeContentSnapshot(*content));

    // ARA2: 从缓存的 AudioSource shape 提供只读元数据
    snap->sourceSampleRate = cachedSourceShape_.sourceSampleRate;
    snap->sourceChannelCount = cachedSourceShape_.numChannels;
    snap->sourceSampleCount = cachedSourceShape_.numSamples;
    return snap;
}

void AudioModification::applyNotes(const std::vector<Note>& notes)
{
    content->notes = notes;
    for (auto& note : content->notes)
        note.outputGainDb = content->volumeEnvelope.evalAt(note.startTime);
    content->noteTopologyInitialized = true;
    ++content->notesRevision;
    ++content->contentRevision;
}

void AudioModification::applyVolumeEnvelope(const AutomationLane& envelope)
{
    content->volumeEnvelope = envelope;
    for (auto& note : content->notes)
        note.outputGainDb = content->volumeEnvelope.evalAt(note.startTime);
    ++content->notesRevision;
}

void AudioModification::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    content->analysis.pitchCurve = std::move(curve);
    ++content->pitchRevision;
    ++content->contentRevision;
}

bool AudioModification::applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> grid)
{
    if (!content.has_value())
        return false;

    const auto& sourceWindow = content->sourceWindow;
    if (sourceWindow.sourcePersistentId.isEmpty() || !sourceWindow.isValid())
        return false;

    if (grid == nullptr)
        return false;

    const double gridDuration = grid->totalDurationSeconds();
    const double sourceDuration = sourceWindow.durationSeconds();

    if (!grid->isIdentity())
        return false;

    if (std::abs(gridDuration - sourceDuration) > 1e-6)
        return false;

    content->timeGrid = std::move(grid);
    ++content->timeGridRevision;

    return true;
}

bool AudioModification::applyPitchShiftState(const PitchShiftEditState& state)
{
    if (!content.has_value() || content->analysis.pitchCurve == nullptr)
        return false;

    content->notes = state.notes;
    content->analysis.pitchCurve->replaceCorrectionSegments(state.segments);
    content->pitchShiftSettings = state.settings;
    ++content->notesRevision;
    ++content->pitchRevision;
    ++content->contentRevision;
    return true;
}

void AudioModification::applyDetectedKey(const DetectedKey& key)
{
    content->analysis.detectedKey = key;
}

void AudioModification::applyOriginalF0(std::shared_ptr<PitchCurve> curve)
{
    // 一次性提交：curve + Ready 状态 + 单次 analysis/content revision。
    // 不调用 applyOriginalF0State，避免同一次提交双 bump。
    content->analysis.pitchCurve = std::move(curve);
    content->analysis.setOriginalF0State(OriginalF0State::Ready);
    ++content->analysis.analysisRevision;
    ++content->pitchRevision;
    ++content->contentRevision;
}

void AudioModification::submitSilentGaps(std::vector<SilentGap> gaps)
{
    content->analysis.silentGaps = std::move(gaps);
    ++content->analysis.analysisRevision;
    ++content->contentRevision;
}

void AudioModification::applyReferenceFeatures(const ReferenceFeatureSet& features)
{
    content->analysis.referenceFeatures = features;
}

void AudioModification::applyOriginalF0State(OriginalF0State state)
{
    content->analysis.setOriginalF0State(state);
}

} // namespace OpenTune
