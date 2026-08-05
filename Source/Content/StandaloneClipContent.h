#pragma once
#include "DomainContentOwner.h"
#include "ContentPayloadState.h"
#include "StandaloneRetiredContentRecord.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace OpenTune {

using StandaloneClipId = uint64_t;

/// Standalone clip 域内容所有者。持有完整 ContentPayloadState。
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

    /// 是否已退休
    bool isRetired() const;

    /// 是否有活跃内容（lifecycle != Retired && content_ 非空）
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

    // ── Payload accessors ──────────────────────────────────
    ContentPayloadState& payload() { return content_; }
    const ContentPayloadState& payload() const { return content_; }

    // ── Retired content management ─────────────────────────
    std::vector<StandaloneRetiredContentRecord>& retiredRecords() { return retired_; }
    const std::vector<StandaloneRetiredContentRecord>& retiredRecords() const { return retired_; }

private:
    void bumpContentRevision();

    StandaloneClipId clipId_;
    ContentPayloadState content_;
    std::vector<StandaloneRetiredContentRecord> retired_;
};

} // namespace OpenTune
