#pragma once
#include "DomainContentOwner.h"
#include "ContentState.h"
#include "StandaloneRetiredContentRecord.h"
#include <memory>
#include <mutex>
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
    /// 快照发布边界。线程语义：提交路径（apply*）与快照读不取锁，无读写并发
    /// 保证（消息线程单线程所有权）；唯一并发保证是双冷读者的发布串行化
    /// （snapshotMutex_ 串行化惰性缓存写入），并发冷读者取得同一份已发布快照。
    std::shared_ptr<const EditableContentSnapshot> snapshotContent() const override;

    // ── Lifecycle ───────────────────────────────────────────
    void retireContent(ContentKey key) override;
    void reviveContent(ContentKey key) override;
    void releaseRetiredContent(ContentKey key) override;
    const StandaloneRetiredContentRecord* getRetiredContent(ContentKey key) const;

    /// 是否已退休。
    bool isRetired() const;

    /// 是否有活跃内容。
    bool hasActiveContent() const;

    // ── Apply commands（由 PluginProcessor coordinator 调用）─
    /// 新建 owner 的一次性完整内容提交（import/split/merge/copy/restore 构造 payload 后调用）。
    void initializeContent(ContentState payload);
    void applyNotes(std::vector<Note> notes, bool topologyInitialized = true);
    // 恢复持久化存储元数据（source window / native 来源 / canonical 起点）。
    void applyImportedSource(SourceWindow window, NativePcmSource nativeSource,
                             int64_t firstCanonicalSample);
    // 替换唯一音量包络，并同步 Note::outputGainDb 派生值。
    void applyVolumeEnvelope(AutomationLane envelope);
    void applyPitchCurve(std::shared_ptr<PitchCurve> curve);
    void applyOriginalF0(std::shared_ptr<PitchCurve> curve);
    void applyTimeGrid(std::shared_ptr<const TimeGridSnapshot> snapshot);
    bool applyPitchShiftState(const PitchShiftEditState& state);
    void applyDetectedKey(const DetectedKey& key);
    void applyReferenceFeatures(const ReferenceFeatureSet& features);
    void applyOriginalF0State(OriginalF0State state);
    void applyOriginalF0FailureReason(std::string reason);
    void applyPitchShiftSettings(PitchShiftSettings settings);
    void applySilentGaps(std::vector<SilentGap> gaps);
    void applyAudioBuffer(std::shared_ptr<const juce::AudioBuffer<float>> buffer, double sampleRate);

    // ── Content accessors ───────────────────────────────────
    const ContentState& content() const { return content_; }

private:
    void bumpContentRevision();
    /// 快照复用令牌：每个变更入口在与内容变更同一语句域内推进；
    /// 令牌不变时 snapshotContent 复用同一不可变快照对象（指针稳定）。
    void bumpSnapshotToken() noexcept { ++snapshotToken_; }

    StandaloneClipId clipId_;
    ContentState content_;
    ContentLifecycle lifecycle_{ContentLifecycle::Ready};
    std::vector<StandaloneRetiredContentRecord> retired_;

    uint64_t snapshotToken_{0};
    // 双冷读者发布串行化：惰性缓存写入在此互斥边界内完成。提交路径与快照读
    // 不取锁（消息线程单线程所有权），无读写并发保证。
    mutable std::mutex snapshotMutex_;
    mutable std::shared_ptr<const EditableContentSnapshot> cachedSnapshot_;
    mutable uint64_t cachedSnapshotToken_{0};
};

} // namespace OpenTune
