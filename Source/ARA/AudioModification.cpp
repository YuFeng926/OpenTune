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
    juce::ignoreUnused(source);
    if (content.has_value())
    {
        if (content->sourceWindow.sourcePersistentId != source.getIdentity().persistentId)
            return false;

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
    return true;
}

void AudioModification::revokeAnalysisReception() noexcept
{
    ++birthRevision;
    analysisContinuationPending = false;
    // Extracting is stored as a continuation in the archive.
    if (content && content->analysis.originalF0State == OriginalF0State::Extracting)
        content->analysis.setOriginalF0State(OriginalF0State::NotRequested);
}

void AudioModification::clearOriginalAnalysis()
{
    auto& analysis = content->analysis;
    analysis.clearOriginalAnalysis();
    ++analysis.analysisRevision;
    ++content->pitchRevision;
    ++content->contentRevision;
    originalF0InputStamp.reset();
    analysisContinuationPending = false;
    birthState = AudioModificationBirthState::WaitingForSource;
}

void AudioModification::registerNoteTopologyInitialization(bool notesPrimary) noexcept
{
    noteTopologyInitPending = notesPrimary && content.has_value() && !content->noteTopologyInitialized;
}

bool AudioModification::hasReadyOriginalF0() const
{
    return content.has_value()
        && content->analysis.originalF0State == OriginalF0State::Ready
        && content->analysis.pitchCurve != nullptr
        && content->analysis.pitchCurve->hasUsableOriginalF0();
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

    return std::make_shared<EditableContentSnapshot>(makeContentSnapshot(*content));
}

void AudioModification::applyNotes(const std::vector<Note>& notes)
{
    content->notes = notes;
    for (auto& note : content->notes)
        note.outputGainDb = content->volumeEnvelope.evalAt(note.startTime);
    content->noteTopologyInitialized = true;
    noteTopologyInitPending = false;
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

void AudioModification::applyOriginalF0FailureReason(std::string reason)
{
    content->analysis.originalF0FailureReason = std::move(reason);
}

} // namespace OpenTune
