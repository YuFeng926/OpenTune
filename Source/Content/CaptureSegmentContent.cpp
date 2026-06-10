#include "CaptureSegmentContent.h"
#include "EditableContentSnapshot.h"
#include <algorithm>
#include <cmath>

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
    RetiredContentRecord record;
    record.key = key;
    record.content.editable = std::move(editable_);
    record.content.contentRevision = editable_.contentRevision;
    retired_.push_back(std::move(record));
}

void CaptureSegmentContent::reviveContent(ContentKey key)
{
    if (key != contentKey()) return;
    for (auto it = retired_.begin(); it != retired_.end(); ++it) {
        if (it->key == key) {
            editable_ = std::move(it->content.editable);
            retired_.erase(it);
            return;
        }
    }
}

void CaptureSegmentContent::releaseRetiredContent(ContentKey key)
{
    if (key != contentKey()) return;
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
        [&key](const RetiredContentRecord& r) { return r.key == key; }),
        retired_.end());
}

void CaptureSegmentContent::applyAudioBuffer(const juce::AudioBuffer<float>* buffer, double sampleRate)
{
    if (buffer == nullptr || buffer->getNumSamples() == 0) {
        editable_.audioBuffer = nullptr;
        editable_.audioSampleRate = 0.0;
        return;
    }

    // Make a shared copy of the audio buffer
    auto bufferCopy = std::make_shared<juce::AudioBuffer<float>>();
    bufferCopy->makeCopyOf(*buffer);

    editable_.audioBuffer = bufferCopy;
    editable_.audioSampleRate = sampleRate;
    ++editable_.audioRevision;
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

void CaptureSegmentContent::applyPitchCurve(std::shared_ptr<PitchCurve> curve)
{
    pitchCurve_ = std::move(curve);
    ++editable_.pitchRevision;
    ++editable_.contentRevision;
}

} // namespace OpenTune
