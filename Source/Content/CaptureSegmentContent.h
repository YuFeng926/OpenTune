#pragma once
#include "DomainContentOwner.h"
#include "EditableContentState.h"
#include "../Utils/PitchCurve.h"
#include <memory>
#include <vector>

namespace OpenTune {

/// Regular VST3 Capture 段的内容所有者。轻量级实现，不涉及全套编辑功能。
class CaptureSegmentContent : public DomainContentOwner
{
public:
    explicit CaptureSegmentContent(uint64_t id);
    ~CaptureSegmentContent() override = default;

    ContentKey contentKey() const override;
    std::shared_ptr<const EditableContentSnapshot> snapshotContent() const override;

    // 生命周期管理
    void retireContent(ContentKey key) override;
    void reviveContent(ContentKey key) override;
    void releaseRetiredContent(ContentKey key) override;

    EditableContentState& editable() { return editable_; }
    const EditableContentState& editable() const { return editable_; }

    // Apply methods for Capture segment content
    void applyAudioBuffer(const juce::AudioBuffer<float>& buffer, double sampleRate);
    void applyOriginalF0State(OriginalF0State state);
    void applyDetectedKey(const DetectedKey& key);
    void applyNotes(std::vector<Note> notes);
    // 替换唯一音量包络，并同步 Note::outputGainDb 派生值。
    void applyVolumeEnvelope(AutomationLane envelope);
    void applyPitchCurve(std::shared_ptr<PitchCurve> curve);
    void applyOriginalF0(std::shared_ptr<PitchCurve> curve);
    void applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot);
    bool applyPitchShiftState(const PitchShiftEditState& state);
    void applyReferenceFeatures(const ReferenceFeatureSet& features);
    std::shared_ptr<PitchCurve> pitchCurve() const { return pitchCurve_; }

    struct CaptureRetiredRecord {
        ContentKey key;
        EditableContentState editable;
        uint64_t contentRevision;
    };

private:
    uint64_t id_;
    EditableContentState editable_;
    std::shared_ptr<PitchCurve> pitchCurve_;
    std::vector<CaptureRetiredRecord> captureRetired_;
};

} // namespace OpenTune
