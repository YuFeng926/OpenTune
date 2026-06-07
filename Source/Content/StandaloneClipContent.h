#pragma once
#include "DomainContentOwner.h"
#include "EditableContentState.h"
#include "RetiredContentRecord.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeGrid.h"
#include "../Utils/SourceWindow.h"
#include <memory>
#include <vector>

namespace OpenTune {

/// Standalone clip 域内容所有者。每个 placement 对应一个实例。
class StandaloneClipContent : public DomainContentOwner
{
public:
    explicit StandaloneClipContent(uint64_t id);
    ~StandaloneClipContent() override = default;

    ContentKey contentKey() const override;
    std::shared_ptr<const EditableContentSnapshot> snapshotContent() const override;
    void applyContentCommand(class ContentCommand& cmd) override;

    // 生命周期管理 — 非 ARA 模式下保留空实现
    void retireContent(ContentKey key) override;
    void reviveContent(ContentKey key) override;
    void releaseRetiredContent(ContentKey key) override;

    // 可编辑状态访问
    EditableContentState& editable() { return editable_; }
    const EditableContentState& editable() const { return editable_; }

    // 分析/渲染源数据（Standalone 模式下由 import/capture 填充）
    void setPitchCurve(std::shared_ptr<PitchCurve> curve) { pitchCurve_ = std::move(curve); }
    std::shared_ptr<PitchCurve> pitchCurve() const { return pitchCurve_; }

    void setAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, double sampleRate)
    {
        audioBuffer_ = std::move(buffer);
        audioSampleRate_ = sampleRate;
    }
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer() const { return audioBuffer_; }
    double audioSampleRate() const { return audioSampleRate_; }

    void setDetectedKey(const DetectedKey& key) { detectedKey_ = key; }
    DetectedKey detectedKey() const { return detectedKey_; }

    void setOriginalF0State(OriginalF0State state) { originalF0State_ = state; }
    OriginalF0State originalF0State() const { return originalF0State_; }

private:
    uint64_t id_;
    EditableContentState editable_;
    std::vector<RetiredContentRecord> retired_;
    std::shared_ptr<PitchCurve> pitchCurve_;
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer_;
    double audioSampleRate_ = 44100.0;
    DetectedKey detectedKey_;
    OriginalF0State originalF0State_{OriginalF0State::NotRequested};
};

} // namespace OpenTune
