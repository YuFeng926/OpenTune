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
    snap->pitchCurve = pitchCurve_;
    snap->timeGrid = editable_.timeGrid;
    snap->pitchShiftSettings = editable_.pitchShiftSettings;
    snap->originalF0State = editable_.originalF0State;
    snap->detectedKey = editable_.detectedKey;
    snap->audioBuffer = editable_.audioBuffer;
    snap->audioSampleRate = editable_.audioSampleRate;
    snap->audioRevision = editable_.audioRevision;
    snap->notesRevision = editable_.notesRevision;
    snap->noteTopologyInitialized = editable_.noteTopologyInitialized;
    snap->pitchRevision = editable_.pitchRevision;
    snap->timeGridRevision = editable_.timeGridRevision;
    snap->pitchShiftRevision = editable_.pitchShiftRevision;
    snap->contentRevision = editable_.contentRevision;
    snap->referenceFeatures = editable_.referenceFeatures;
    snap->volumeEnvelope = editable_.volumeEnvelope;
    snap->outputGainRevision = editable_.outputGainRevision;
    return snap;
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
        && editable_.detectedKey.origin == key.origin
        && std::abs(editable_.detectedKey.confidence - key.confidence) <= 1.0e-6f)
        return;
    editable_.detectedKey = key;
    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyNotes(std::vector<Note> notes)
{
    editable_.notes = std::move(notes);
    for (auto& note : editable_.notes)
        note.outputGainDb = editable_.volumeEnvelope.evalAt(note.startTime);
    editable_.noteTopologyInitialized = true;
    ++editable_.notesRevision;
    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyVolumeEnvelope(AutomationLane envelope)
{
    editable_.volumeEnvelope = std::move(envelope);
    for (auto& note : editable_.notes)
        note.outputGainDb = editable_.volumeEnvelope.evalAt(note.startTime);
    ++editable_.notesRevision;
    ++editable_.outputGainRevision;
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
    ++editable_.contentRevision;
}

void CaptureSegmentContent::applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot)
{
    editable_.timeGrid = std::move(snapshot);
    ++editable_.timeGridRevision;
    ++editable_.contentRevision;
}

bool CaptureSegmentContent::applyPitchShiftState(const PitchShiftEditState& state)
{
    if (pitchCurve_ == nullptr)
        return false;

    editable_.notes = state.notes;
    pitchCurve_->replaceCorrectionSegments(state.segments);
    editable_.pitchShiftSettings = state.settings;
    ++editable_.notesRevision;
    ++editable_.pitchRevision;
    ++editable_.pitchShiftRevision;
    ++editable_.contentRevision;
    return true;
}

void CaptureSegmentContent::applyReferenceFeatures(const ReferenceFeatureSet& features)
{
    editable_.referenceFeatures = features;
}

} // namespace OpenTune
