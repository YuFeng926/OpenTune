#pragma once
#include "DomainContentOwner.h"
#include "ContentState.h"
#include "StandaloneRetiredContentRecord.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

using StandaloneClipId = uint64_t;

enum class ContentLifecycle
{
    Ready,
    Retired
};

/// Standalone clip 域内容所有者。持有完整 ContentState。
/// 不拥有 RenderCache/TimeStretchCache/Stretcher/Worker/PlaybackSourcePublisher。
class StandaloneClipContent : public DomainContentOwner
{
public:
    explicit StandaloneClipContent(StandaloneClipId clipId);
    ~StandaloneClipContent() override = default;

    // ── DomainContentOwner 接口 ─────────────────────────────
    ContentKey contentKey() const override;
    std::shared_ptr<const EditableContentSnapshot> snapshotContent() const override;

    // ── Lifecycle ───────────────────────────────────────────
    void retireContent(ContentKey key) override;
    void reviveContent(ContentKey key) override;
    void releaseRetiredContent(ContentKey key) override;

    /// 是否已退休。
    bool isRetired() const;

    /// 是否有活跃内容。
    bool hasActiveContent() const;

    // ── Apply commands（由 PluginProcessor coordinator 调用）─
    void applyNotes(std::vector<Note> notes);
    // 替换唯一音量包络，并同步 Note::outputGainDb 派生值。
    void applyVolumeEnvelope(AutomationLane envelope);
    void applyPitchCurve(std::shared_ptr<PitchCurve> curve);
    void applyOriginalF0(std::shared_ptr<PitchCurve> curve);
    void applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot);
    bool applyPitchShiftState(const PitchShiftEditState& state);
    void applyDetectedKey(const DetectedKey& key);
    void applyReferenceFeatures(const ReferenceFeatureSet& features);
    void applyOriginalF0State(OriginalF0State state);
    void applyAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, double sampleRate);

    // ── Content accessors ───────────────────────────────────
    ContentState& content() { return content_; }
    const ContentState& content() const { return content_; }

private:
    void bumpContentRevision();

    StandaloneClipId clipId_;
    ContentState content_;
    ContentLifecycle lifecycle_{ContentLifecycle::Ready};
    std::vector<StandaloneRetiredContentRecord> retired_;
};

} // namespace OpenTune
