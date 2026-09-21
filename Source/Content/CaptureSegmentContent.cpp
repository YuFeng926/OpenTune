#include "CaptureSegmentContent.h"
#include "ContentSnapshotProjection.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace OpenTune {

CaptureSegmentContent::CaptureSegmentContent(uint64_t id)
    : id_(id)
{
}

ContentKey CaptureSegmentContent::contentKey() const
{
    ContentKey key;
    key.domainKind = DomainKind::RegularVST3Capture;
    key.objectId = id_;
    return key;
}

std::shared_ptr<const EditableContentSnapshot> CaptureSegmentContent::snapshotContent() const
{
    return std::make_shared<const EditableContentSnapshot>(makeContentSnapshot(content_));
}

void CaptureSegmentContent::retireContent(ContentKey key)
{
    if (key != contentKey()) return;
    CaptureRetiredRecord record;
    record.key = key;
    record.content = std::move(content_);
    captureRetired_.push_back(std::move(record));

    // 与 Standalone retire 对齐：active content 重置为默认 bootstrap identity/revision 1。
    content_ = ContentState{};
}

void CaptureSegmentContent::reviveContent(ContentKey key)
{
    if (key != contentKey()) return;
    for (auto it = captureRetired_.begin(); it != captureRetired_.end(); ++it) {
        if (it->key == key) {
            content_ = std::move(it->content);
            // 与 Standalone revive 对齐：恢复后推进一次 contentRevision。
            ++content_.contentRevision;
            captureRetired_.erase(it);
            return;
        }
    }
}

void CaptureSegmentContent::releaseRetiredContent(ContentKey key)
{
    if (key != contentKey()) return;
    captureRetired_.erase(std::remove_if(captureRetired_.begin(), captureRetired_.end(),
        [&key](const CaptureRetiredRecord& r) { return r.key == key; }),
        captureRetired_.end());
}

void CaptureSegmentContent::applyAudioBuffer(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    const double durationSeconds = static_cast<double>(buffer.getNumSamples()) / sampleRate;

    auto bufferCopy = std::make_shared<juce::AudioBuffer<float>>();
    bufferCopy->makeCopyOf(buffer);

    content_.audioBuffer = std::move(bufferCopy);
    content_.sampleRate = sampleRate;
    ++content_.audioRevision;

    // 非正/非有限 duration 生成 identity 失败时保留已有 bootstrap identity，不写 nullptr。
    if (auto realGrid = TimeGridSnapshot::makeIdentity(durationSeconds))
    {
        content_.timeGrid = std::move(realGrid);
        ++content_.timeGridRevision;
    }

    ++content_.contentRevision;
}

void CaptureSegmentContent::applyOriginalF0State(OriginalF0State state)
{
    if (!content_.analysis.setOriginalF0State(state))
        return;
}

void CaptureSegmentContent::applyDetectedKey(const DetectedKey& key)
{
    if (content_.analysis.detectedKey.root == key.root
        && content_.analysis.detectedKey.scale == key.scale
        && content_.analysis.detectedKey.origin == key.origin
        && std::abs(content_.analysis.detectedKey.confidence - key.confidence) <= 1.0e-6f)
        return;
    content_.analysis.detectedKey = key;
}

void CaptureSegmentContent::applyNotes(std::vector<Note> notes)
{
    content_.notes = std::move(notes);
    for (auto& note : content_.notes)
        note.outputGainDb = content_.volumeEnvelope.evalAt(note.startTime);
    content_.noteTopologyInitialized = true;
    ++content_.notesRevision;
    ++content_.contentRevision;
}

void CaptureSegmentContent::applyVolumeEnvelope(AutomationLane envelope)
{
    content_.volumeEnvelope = std::move(envelope);
    for (auto& note : content_.notes)
        note.outputGainDb = content_.volumeEnvelope.evalAt(note.startTime);
    ++content_.notesRevision;
    ++content_.outputGainRevision;
}

void CaptureSegmentContent::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    content_.analysis.pitchCurve = std::move(curve);
    ++content_.pitchRevision;
    ++content_.contentRevision;
}

void CaptureSegmentContent::applyOriginalF0(std::shared_ptr<PitchCurve> curve)
{
    // 一次性提交：curve + Ready 状态 + 单次 analysis/content revision。
    // 不调用 applyOriginalF0State，避免同一次提交双 bump。
    content_.analysis.pitchCurve = std::move(curve);
    content_.analysis.setOriginalF0State(OriginalF0State::Ready);
    ++content_.analysis.analysisRevision;
    ++content_.pitchRevision;
    ++content_.contentRevision;
}

void CaptureSegmentContent::applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot)
{
    if (snapshot == nullptr)
        return;
    content_.timeGrid = std::move(snapshot);
    ++content_.timeGridRevision;
}

bool CaptureSegmentContent::applyPitchShiftState(const PitchShiftEditState& state)
{
    if (content_.analysis.pitchCurve == nullptr)
        return false;

    content_.notes = state.notes;
    content_.analysis.pitchCurve->replaceCorrectionSegments(state.segments);
    content_.pitchShiftSettings = state.settings;
    ++content_.notesRevision;
    ++content_.pitchRevision;
    ++content_.pitchShiftRevision;
    ++content_.contentRevision;
    return true;
}

void CaptureSegmentContent::applyReferenceFeatures(const ReferenceFeatureSet& features)
{
    content_.analysis.referenceFeatures = features;
}

} // namespace OpenTune
