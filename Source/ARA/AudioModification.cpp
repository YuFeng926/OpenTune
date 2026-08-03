#include "AudioModification.h"
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

std::optional<AudioModificationContentState> AudioModificationContentState::makeBorn(const SourceWindow& window)
{
    const auto grid = TimeGridSnapshot::makeIdentity(window.durationSeconds());
    if (!grid)
        return std::nullopt;

    AudioModificationContentState state;
    state.sourceWindow = window;
    state.editable.timeGrid = grid;
    state.editable.timeGridRevision = 1;
    state.contentRevision = 0;
    return state;
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

    const SourceWindow newWindow{
        0,
        source.getIdentity().persistentId,
        0.0,
        source.getShape().durationSeconds()
    };

    const auto bornOpt = AudioModificationContentState::makeBorn(newWindow);
    if (!bornOpt)
        return false;

    content.emplace(std::move(*bornOpt));
    birthState = AudioModificationBirthState::WaitingForSource;
    cachedSourceShape_ = source.getShape();
    return true;
}

void AudioModification::resetContent() noexcept
{
    ++birthRevision;
    content.reset();
    birthState = AudioModificationBirthState::Empty;
}

void AudioModification::invalidateDerivedContent() noexcept
{
    if (!content.has_value())
        return;

    ++birthRevision;
    content->analysis = AnalysisState{};
    birthState = AudioModificationBirthState::WaitingForSource;
}

bool AudioModification::isRenderable() const noexcept
{
    if (!content.has_value())
        return false;
    if (birthState != AudioModificationBirthState::Ready)
        return false;
    if (!content->sourceWindow.isValid())
        return false;
    if (!content->editable.timeGrid)
        return false;
    return true;
}

ContentKey AudioModification::contentKey() const noexcept
{
    return contentIdentity;
}

std::shared_ptr<const EditableContentSnapshot> AudioModification::snapshotContent() const
{
    if (!content.has_value())
        return nullptr;

    auto snap = std::make_shared<EditableContentSnapshot>();
    snap->sourceWindow = content->sourceWindow;

    // ARA2: 从缓存的 AudioSource shape 提供只读元数据
    snap->sourceSampleRate = cachedSourceShape_.sourceSampleRate;
    snap->sourceChannelCount = cachedSourceShape_.numChannels;
    snap->sourceSampleCount = cachedSourceShape_.numSamples;

    // 保持 audioSampleRate 与 audioBuffer 绑定（ARA 下为 nullptr/0.0）
    snap->audioBuffer = nullptr;  // ARA 不拥有 PCM
    snap->audioSampleRate = 0.0;  // 与 audioBuffer 一致

    // modification-scoped state
    snap->notes = content->editable.notes;
    snap->pitchCurve = content->analysis.pitchCurve;
    snap->timeGrid = content->editable.timeGrid;
    snap->pitchShiftSettings = content->editable.pitchShiftSettings;
    snap->originalF0State = content->analysis.originalF0State;
    snap->detectedKey = content->analysis.detectedKey;
    snap->silentGaps = content->analysis.silentGaps;
    snap->referenceFeatures = content->analysis.referenceFeatures;
    snap->sibilantGainEnvelope = content->editable.sibilantGainEnvelope;
    snap->notesRevision = content->editable.notesRevision;
    snap->pitchRevision = content->editable.pitchRevision;
    snap->timeGridRevision = content->editable.timeGridRevision;
    snap->pitchShiftRevision = content->editable.pitchShiftRevision;
    snap->outputGainRevision = content->editable.outputGainRevision;
    snap->contentRevision = content->contentRevision;
    return snap;
}

void AudioModification::applyNotes(const std::vector<Note>& notes)
{
    content->editable.notes = notes;
    ++content->editable.notesRevision;
    ++content->editable.contentRevision;
    ++content->contentRevision;
}

void AudioModification::applyNotesWithOutputGain(const std::vector<Note>& notes)
{
    applyNotes(notes);
    ++content->editable.outputGainRevision;
}

void AudioModification::applySibilantGainEnvelope(const SibilantGainEnvelope& envelope)
{
    content->editable.sibilantGainEnvelope = envelope;
    ++content->editable.outputGainRevision;
    ++content->editable.contentRevision;
    ++content->contentRevision;
}

void AudioModification::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    content->analysis.pitchCurve = std::move(curve);
    ++content->editable.pitchRevision;
    ++content->editable.contentRevision;
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

    if (std::abs(gridDuration - sourceDuration) > 1e-6)
        return false;

    content->editable.timeGrid = std::move(grid);
    ++content->editable.timeGridRevision;
    ++content->editable.contentRevision;
    ++content->contentRevision;

    return true;
}

bool AudioModification::applyPitchShiftState(const PitchShiftEditState& state)
{
    if (!content.has_value() || content->analysis.pitchCurve == nullptr)
        return false;

    content->editable.notes = state.notes;
    content->analysis.pitchCurve->replaceCorrectionSegments(state.segments);
    content->editable.pitchShiftSettings = state.settings;
    ++content->editable.notesRevision;
    ++content->editable.pitchRevision;
    ++content->editable.pitchShiftRevision;
    ++content->editable.contentRevision;
    ++content->contentRevision;
    return true;
}

void AudioModification::applyDetectedKey(const DetectedKey& key)
{
    content->analysis.detectedKey = key;
    ++content->contentRevision;
}

void AudioModification::applyOriginalF0(std::shared_ptr<PitchCurve> curve)
{
    content->analysis.pitchCurve = std::move(curve);
    content->analysis.f0Lifecycle = AnalysisLifecycle::Ready;
    applyOriginalF0State(OriginalF0State::Ready);
    ++content->analysis.analysisRevision;
    ++content->editable.pitchRevision;
    // OriginalF0 只更新分析数据和 UI revision，不触发音频渲染
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
    if (content->analysis.originalF0State == state)
        return;
    content->analysis.originalF0State = state;
    ++content->contentRevision;
}

} // namespace OpenTune
