#include "CaptureSegmentContent.h"
#include "EditableContentSnapshot.h"
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
    auto snap = std::make_shared<EditableContentSnapshot>();
    snap->notes = editable_.notes;
    snap->correctionSegments = editable_.correctionSegments;
    snap->pitchCurve = pitchCurve_;
    snap->timeGrid = editable_.timeGrid;
    snap->pitchShiftSettings = editable_.pitchShiftSettings;
    snap->originalF0State = editable_.originalF0State;
    snap->detectedKey = editable_.detectedKey;
    snap->audioBuffer = editable_.audioBuffer;
    snap->audioSampleRate = editable_.audioSampleRate;
    snap->audioRevision = editable_.audioRevision;
    snap->notesRevision = editable_.notesRevision;
    snap->pitchRevision = editable_.pitchRevision;
    snap->timeGridRevision = editable_.timeGridRevision;
    snap->pitchShiftRevision = editable_.pitchShiftRevision;
    snap->contentRevision = editable_.contentRevision;
    snap->referenceFeatures = editable_.referenceFeatures;
    return snap;
}

void CaptureSegmentContent::applyContentCommand(ContentCommand& cmd)
{
    (void)cmd;
    // Capture 段不支持编辑命令
}

void CaptureSegmentContent::retireContent(ContentKey key)
{
    if (key != contentKey()) return;
    // Capture uses lightweight EditableContentState, not full AudioModificationContentState
    CaptureRetiredRecord record;
    record.key = key;
    record.editable = std::move(editable_);
    record.contentRevision = editable_.contentRevision;
    captureRetired_.push_back(std::move(record));
}

void CaptureSegmentContent::reviveContent(ContentKey key)
{
    if (key != contentKey()) return;
    for (auto it = captureRetired_.begin(); it != captureRetired_.end(); ++it) {
        if (it->key == key) {
            editable_ = std::move(it->editable);
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

    editable_.audioBuffer = std::move(bufferCopy);
    editable_.audioSampleRate = sampleRate;
    ++editable_.audioRevision;

    editable_.timeGrid = TimeGridSnapshot::makeIdentity(durationSeconds);
    ++editable_.timeGridRevision;

    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyOriginalF0State(OriginalF0State state)
{
    if (editable_.originalF0State == state)
        return;
    editable_.originalF0State = state;
    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyDetectedKey(const DetectedKey& key)
{
    if (editable_.detectedKey.root == key.root
        && editable_.detectedKey.scale == key.scale
        && std::abs(editable_.detectedKey.confidence - key.confidence) <= 1.0e-6f)
        return;
    editable_.detectedKey = key;
    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyNotes(std::vector<Note> notes)
{
    editable_.notes = std::move(notes);
    ++editable_.notesRevision;
    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    pitchCurve_ = std::move(curve);
    ++editable_.pitchRevision;
    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyOriginalF0(std::shared_ptr<PitchCurve> curve)
{
    pitchCurve_ = std::move(curve);
    ++editable_.pitchRevision;
    // OriginalF0 只更新 UI revision，不触发音频渲染
}

void CaptureSegmentContent::applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot)
{
    editable_.timeGrid = std::move(snapshot);
    ++editable_.timeGridRevision;
    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyPitchShiftSettings(const PitchShiftSettings& settings)
{
    editable_.pitchShiftSettings = settings;
    ++editable_.pitchShiftRevision;
    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyReferenceFeatures(const ReferenceFeatureSet& features)
{
    editable_.referenceFeatures = features;
    ++editable_.contentRevision;
}

} // namespace OpenTune
