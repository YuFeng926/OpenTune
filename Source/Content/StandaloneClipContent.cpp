#include "StandaloneClipContent.h"
#include "ContentSnapshotProjection.h"
#include <algorithm>
#include <utility>

namespace OpenTune {

StandaloneClipContent::StandaloneClipContent(StandaloneClipId clipId)
    : clipId_(clipId)
{
}

ContentKey StandaloneClipContent::contentKey() const
{
    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = clipId_;
    return key;
}

std::shared_ptr<const EditableContentSnapshot> StandaloneClipContent::snapshotContent() const
{
    return std::make_shared<const EditableContentSnapshot>(makeContentSnapshot(content_));
}

// ── Lifecycle ───────────────────────────────────────────────

void StandaloneClipContent::retireContent(ContentKey key)
{
    if (key != contentKey())
        return;

    StandaloneRetiredContentRecord record;
    record.key = key;
    record.content = std::move(content_);
    lifecycle_ = ContentLifecycle::Retired;

    // 重置当前 content 为空状态
    content_ = ContentState{};

    retired_.push_back(std::move(record));
}

void StandaloneClipContent::reviveContent(ContentKey key)
{
    if (key != contentKey())
        return;

    for (auto it = retired_.begin(); it != retired_.end(); ++it) {
        if (it->key == key) {
            content_ = std::move(it->content);
            lifecycle_ = ContentLifecycle::Ready;
            bumpContentRevision();
            retired_.erase(it);
            return;
        }
    }
}

void StandaloneClipContent::releaseRetiredContent(ContentKey key)
{
    if (key != contentKey())
        return;

    retired_.erase(
        std::remove_if(retired_.begin(), retired_.end(),
            [&key](const StandaloneRetiredContentRecord& r) { return r.key == key; }),
        retired_.end());
}

bool StandaloneClipContent::isRetired() const
{
    return lifecycle_ == ContentLifecycle::Retired;
}

bool StandaloneClipContent::hasActiveContent() const
{
    return lifecycle_ != ContentLifecycle::Retired;
}

// ── Apply commands ──────────────────────────────────────────

void StandaloneClipContent::applyNotes(std::vector<Note> notes)
{
    content_.notes = std::move(notes);
    for (auto& note : content_.notes)
        note.outputGainDb = content_.volumeEnvelope.evalAt(note.startTime);
    content_.noteTopologyInitialized = true;
    ++content_.notesRevision;
    bumpContentRevision();
}

void StandaloneClipContent::applyVolumeEnvelope(AutomationLane envelope)
{
    content_.volumeEnvelope = std::move(envelope);
    for (auto& note : content_.notes)
        note.outputGainDb = content_.volumeEnvelope.evalAt(note.startTime);
    ++content_.notesRevision;
}

void StandaloneClipContent::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    content_.analysis.pitchCurve = std::move(curve);
    ++content_.pitchRevision;
    bumpContentRevision();
}

void StandaloneClipContent::applyOriginalF0(std::shared_ptr<PitchCurve> curve)
{
    // 一次性提交：curve + Ready 状态 + 单次 analysis/content revision。
    // 不调用 applyOriginalF0State，避免同一次提交双 bump。
    content_.analysis.pitchCurve = std::move(curve);
    content_.analysis.setOriginalF0State(OriginalF0State::Ready);
    ++content_.analysis.analysisRevision;
    ++content_.pitchRevision;
    bumpContentRevision();
}

void StandaloneClipContent::applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot)
{
    if (snapshot == nullptr)
        return;
    content_.timeGrid = std::move(snapshot);
    ++content_.timeGridRevision;
}

bool StandaloneClipContent::applyPitchShiftState(const PitchShiftEditState& state)
{
    if (content_.analysis.pitchCurve == nullptr)
        return false;

    content_.notes = state.notes;
    content_.analysis.pitchCurve->replaceCorrectionSegments(state.segments);
    content_.pitchShiftSettings = state.settings;
    ++content_.notesRevision;
    ++content_.pitchRevision;
    bumpContentRevision();
    return true;
}

void StandaloneClipContent::applyDetectedKey(const DetectedKey& key)
{
    content_.analysis.detectedKey = key;
}

void StandaloneClipContent::applyReferenceFeatures(const ReferenceFeatureSet& features)
{
    content_.analysis.referenceFeatures = features;
}

void StandaloneClipContent::applyOriginalF0State(OriginalF0State state)
{
    // 幂等 setter：状态未变则不推进 revision，与 Capture/ARA owner 语义一致。
    content_.analysis.setOriginalF0State(state);
}

void StandaloneClipContent::applyAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, double sampleRate)
{
    const double durationSeconds = static_cast<double>(buffer->getNumSamples()) / sampleRate;

    content_.audioBuffer = std::move(buffer);
    content_.sampleRate = sampleRate;
    ++content_.audioRevision;
    // 非正/非有限 duration 生成 identity 失败时保留已有 bootstrap identity，不写 nullptr。
    if (auto realGrid = TimeGridSnapshot::makeIdentity(durationSeconds))
    {
        content_.timeGrid = std::move(realGrid);
        ++content_.timeGridRevision;
    }
    bumpContentRevision();
}

// ── Private helpers ─────────────────────────────────────────

void StandaloneClipContent::bumpContentRevision()
{
    ++content_.contentRevision;
}

} // namespace OpenTune
