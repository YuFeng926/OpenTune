#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <list>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "AudioModification.h"
#include "AudioSource.h"
#include "../Render/ContentRenderService.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include "../Services/F0ExtractionService.h"
#include "../Content/ContentKey.h"
#include "../Content/OriginalF0FailureEvent.h"
#include "../Utils/PlayHeadState.h"
#include "../Utils/HostTransportCoalescer.h"
#include "PlaybackRegion.h"

namespace OpenTune {

class OpenTunePlaybackRenderer;
class OpenTuneAudioProcessor;
class ResamplingManager;
struct RenderJob;
class OpenTuneDocumentController : public juce::ARADocumentControllerSpecialisation,
                                   private juce::Timer
{
public:
    struct PlaybackRegionProjection
    {
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        std::optional<juce::Colour> displayColour;
        double startInPlaybackTime{0.0};
        double startInModificationTime{0.0};
        double durationInPlaybackTime{0.0};
        double durationInModificationTime{0.0};

        ContentKey contentKey;

        double endInPlaybackTime() const noexcept { return startInPlaybackTime + durationInPlaybackTime; }
        bool isPlaybackRenderable() const noexcept;
    };

    OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                               const ARA::ARADocumentControllerHostInstance* instance);

    ~OpenTuneDocumentController() override;

    // Per ARA2 spec: ARA object persistence uses doStoreObjectsToStream/doRestoreObjectsFromStream,
    // NOT VST3 processor state. Legacy getContentSnapshot/restoreContentPayloadInto removed.

    const ContentRenderService* getContentRenderService() const noexcept;
    std::shared_ptr<ContentRenderService> getContentRenderServiceShared() const noexcept;
    // ARA mutation/render API — processor 通过这些 API 请求 ARA 渲染
    void requestModificationRender(
        ContentKey key,
        double startSeconds,
        double endSeconds,
        std::shared_ptr<const EditableContentSnapshot> snapshot);
    void requestFullModificationRender(
        ContentKey key,
        std::shared_ptr<const EditableContentSnapshot> snapshot = nullptr);
    void invalidateAllModificationCaches();
    // ============================================================
    // 编辑器只读内容访问器（通过 ContentKey 路由到 AudioModification + CRS）
    // ARA 模式下编辑器不经过 content owner，直接读 AudioModification.content
    // ============================================================

    /** 从 CRS 读取音频 buffer */
    std::shared_ptr<const juce::AudioBuffer<float>> readAudioBuffer(ContentKey key) const;

    /** 从 AudioModification.content.analysis 读取不可变 pitch curve 快照 */
    std::shared_ptr<const PitchCurveSnapshot> readPitchCurve(ContentKey key) const;

    /** 读取 OriginalF0 状态 */
    OriginalF0State readOriginalF0State(ContentKey key) const;
    std::string readOriginalF0FailureReason(ContentKey key) const;
    std::deque<OriginalF0FailureEvent> takeOriginalF0FailureEvents();

    /** 读取调性检测结果 */
    DetectedKey readDetectedKey(ContentKey key) const;

    /** 读取音高移调设置 */
    PitchShiftSettings readPitchShift(ContentKey key) const;

    /** Phase 4: 返回 EditableContentSnapshot — 唯一的跨域 snapshot 类型 */
    std::shared_ptr<const EditableContentSnapshot> readContentSnapshot(ContentKey key) const;

    std::vector<PlaybackRegionProjection> getPlaybackRegionProjectionsFor(
        const std::vector<juce::ARAPlaybackRegion*>& playbackRegions) const;
    // 用户 Read 入口：只读取当前内容范围内的首个 PlaybackRegion。
    // archive 恢复且已有有效 F0 的内容由 readRestoredAudio 自动读取，无需此入口。
    int requestReadAudioForPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion,
                                          bool forceRefresh = false);
    // ARA SDK requires DocumentController operations on main thread.
    // This method executes synchronously to comply with ARA thread constraints.
    // Callers should display a loading overlay before calling if UI responsiveness is needed.
    void requestReadAudioForPlaybackRegionAsync(juce::ARAPlaybackRegion* playbackRegion,
                                                bool forceRefresh,
                                                std::function<void(int)> completionCallback);

    // Composite scope Read：去重后对 scope 内每个唯一 AudioModification 调一次
    // requestReadAudioForPlaybackRegion。返回 -1 表示任一失败，否则累计 refreshed。
    int requestReadAudioForPlaybackRegions(
        const std::vector<juce::ARAPlaybackRegion*>& playbackRegions, bool forceRefresh);
    void requestReadAudioForPlaybackRegionsAsync(
        const std::vector<juce::ARAPlaybackRegion*>& playbackRegions,
        bool forceRefresh,
        std::function<void(int)> completionCallback);

    // composite timeline 的轨道时间范围（min trackStart / max trackEnd）。
    // composite 不存在或 segments 为空时返回 nullopt。
    struct CompositeTrackRange { double trackStartSeconds{0.0}; double trackDurationSeconds{0.0}; };
    std::optional<CompositeTrackRange> getCompositeTrackRange(ContentKey compositeKey) const;
    void registerPlaybackRenderer(OpenTunePlaybackRenderer& renderer);
    void unregisterPlaybackRenderer(OpenTunePlaybackRenderer& renderer);

    // ============================================================
    // Track FX composite content — 整轨多 PlaybackRegion 合成的派生缓存。
    // Source of truth 仍是 PlaybackRegion/AudioModification；composite 只读。
    // composite key = {ARAComposite, instanceToken, 0}
    // ============================================================
    struct CompositeSegment
    {
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        ContentKey sourceContentKey;
        double trackStartSeconds{0.0};
        double trackEndSeconds{0.0};
        double sourceStartSeconds{0.0};
        double sourceEndSeconds{0.0};
    };

    struct CompositeEditMapping
    {
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        ContentKey sourceContentKey;
        double trackStartSeconds{0.0};
        double trackEndSeconds{0.0};
        double sourceStartSeconds{0.0};
        double sourceEndSeconds{0.0};
    };

    ContentKey registerCompositeContent(uint64_t instanceToken,
                                        const std::vector<juce::ARAPlaybackRegion*>& scopeRegions);
    void updateCompositeContent(ContentKey compositeKey,
                                const std::vector<juce::ARAPlaybackRegion*>& scopeRegions);
    void removeCompositeContent(ContentKey compositeKey);
    bool isCompositeContentKey(ContentKey key) const noexcept;
    std::vector<CompositeEditMapping> mapCompositeRange(ContentKey compositeKey,
                                                        double trackStartSeconds,
                                                        double trackEndSeconds) const;

    // 任一已注册 AudioModification 在 archive 恢复路径获得了有效内容
    // （restoredFromArchive）时为 true：编辑器首读不应 force-refresh。
    bool hasArchivedContentNeedingRestore() const;

    void didUpdateMusicalContextProperties(juce::ARAMusicalContext* musicalContext) override;
    void didUpdateRegionSequenceProperties(juce::ARARegionSequence* regionSequence) override;
    void willBeginEditing(juce::ARADocument* document) override;
    void didEndEditing(juce::ARADocument* document) override;

    void didUpdateAudioModificationProperties(juce::ARAAudioModification* audioModification) override;
    void willDestroyAudioModification(juce::ARAAudioModification* audioModification) override;

    void didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion* playbackRegion) override;
    void willDestroyPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion) override;
    void didAddPlaybackRegionToAudioModification(juce::ARAAudioModification* audioModification,
                                                 juce::ARAPlaybackRegion* playbackRegion) override;

    void didAddAudioSourceToDocument(juce::ARADocument* document, juce::ARAAudioSource* audioSource) override;
    void didUpdateAudioSourceProperties(juce::ARAAudioSource* audioSource) override;
    void doUpdateAudioSourceContent(juce::ARAAudioSource* audioSource,
                                    juce::ARAContentUpdateScopes scopeFlags) override;
    void willEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                            bool enable) override;
    void didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* audioSource,
                                           bool enable) override;
    void willRemovePlaybackRegionFromAudioModification(juce::ARAAudioModification* audioModification,
                                                       juce::ARAPlaybackRegion* playbackRegion) override;

    void willDestroyAudioSource(juce::ARAAudioSource* audioSource) override;

    bool requestSetPlaybackPosition(double timeInSeconds);
    // Committed seek. On Logic, a seek issued while the host is playing is
    // flushed as stop, then position, then start. Other hosts get a position
    // request only. Availability is returned immediately; the host call is deferred.
    bool requestCommitPlaybackPosition(double timeInSeconds);
    bool requestStartPlayback(double pendingSeekTime = -1.0);
    bool requestStopPlayback();
    bool requestTogglePlayback(bool fallbackObservedPlaying, double pendingSeekTime = -1.0);
    void observeHostPlaybackState(bool isPlaying) noexcept;
    // Publish host PositionInfo to document-shared PlayHeadState. Called from
    // the ARA playback renderer's processBlock. The CAS projection handles
    // contention with other writers; canonical atomics always win.
    void observeHostPlaybackPosition(
        const juce::Optional<juce::AudioPlayHead::PositionInfo>& positionInfo,
        double blockDurationSeconds) noexcept;
    // ARA2 official one-way HostPlaybackController requests for loop control.
    // Per ARA2 spec, host may ignore/delay/quantize; loop truth is observed
    // via companion PositionInfo in processBlock, never written here.
    bool requestEnableCycle(bool enabled);

protected:
    bool doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                    const juce::ARARestoreObjectsFilter* filter) override;
    bool doStoreObjectsToStream(juce::ARAOutputStream& output,
                                const juce::ARAStoreObjectsFilter* filter) override;

    juce::ARAAudioModification* doCreateAudioModification(
        juce::ARAAudioSource* audioSource,
        ARA::ARAAudioModificationHostRef hostRef,
        const juce::ARAAudioModification* optionalModificationToClone) override;
    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() override;
    juce::ARAEditorView* doCreateEditorView() override;

private:
    void requestModificationRenderSamples(ContentKey key,
                                          int64_t startSample,
                                          int64_t endSampleExclusive,
                                          std::shared_ptr<const EditableContentSnapshot> snapshot);

    std::vector<AudioSource> audioSources_;
    std::list<AudioModification> audioModifications_;
    std::vector<PlaybackRegion> playbackRegions_;
    std::vector<OpenTunePlaybackRenderer*> playbackRenderers_;
    std::map<uint64_t, juce::String> araPersistentIdsByObjectId_;
    std::map<juce::String, uint64_t> araObjectIdsByPersistentId_;

    std::shared_ptr<ContentRenderService> contentRenderService_;
    std::shared_ptr<ResamplingManager> resamplingManager_;
    std::unique_ptr<F0ExtractionService> contentF0ExtractionService_;

    // Shared across every ARA role bound to this document. One versioned,
    // CAS-protected word prevents an old PositionInfo observation from clearing
    // a newer UI request, including an ABA sequence of rapid toggles.
    //
    // Bit layout (low four bits are flags; the upper bits are a change version):
    //   bit 0 kObservedValid   — false until first observeHostPlaybackState sets it
    //   bit 1 kObservedPlaying — last confirmed host playing state
    //   bit 2 kRequestPending  — a start/stop request is in flight
    //   bit 3 kTargetPlaying   — desired playing state when pending resolves
    //   bits 4..63             — incremented on every successful state change
    //
    // Invariant: observeHostPlaybackState makes one CAS attempt per host
    // observation. If another request/observation wins the race, this snapshot
    // is discarded; the next processBlock supplies a fresh observation.
    static constexpr std::uint64_t kObservedValid   = 1ull << 0;
    static constexpr std::uint64_t kObservedPlaying = 1ull << 1;
    static constexpr std::uint64_t kRequestPending  = 1ull << 2;
    static constexpr std::uint64_t kTargetPlaying   = 1ull << 3;
    static constexpr std::uint64_t kStateVersionIncrement = 1ull << 4;
    std::atomic<std::uint64_t> playbackCommandState_{0};
    void markPlaybackRequest(bool shouldPlay) noexcept;
    bool logicHostIsPlaying() const noexcept;

    struct HostTransportRelayState;
    std::shared_ptr<HostTransportRelayState> transportRelay_;
    void enqueueHostTransport(const std::function<void(HostTransportCoalescer&)>& mutate);
    void scheduleHostTransportFlush();
    void performHostTransportBatch(const HostTransportBatch& batch);
    bool hostPlaybackTransportAvailable();

    // DC/ARA model ownership、F0 completion 和 DC destruction 均遵守消息线程契约。
    // gate 只由消息线程访问；排队 completion 持有 shared_ptr，先检查 closed 再访问 DC。
    struct F0CompletionRecord
    {
        F0ExtractionService::Result result;
        ContentKey key;
        uint64_t wrapperGeneration{0};
        juce::String persistentId;
        uint64_t birthRevision{0};
        OriginalF0InputStamp stamp;
    };
    struct CompletionGate
    {
        std::mutex mutex;
        bool closed{false};
        std::deque<F0CompletionRecord> pending;
    };
    std::shared_ptr<CompletionGate> completionGate_{std::make_shared<CompletionGate>()};
    std::deque<OriginalF0FailureEvent> pendingF0FailureEvents_;
    std::shared_ptr<ProcessRenderRuntime::CompletionGate> renderCompletionGate_{
        std::make_shared<ProcessRenderRuntime::CompletionGate>()};
    uint64_t nextWrapperGeneration_{1};

    // Document-level shared PlayHeadState: all ARA roles within this document
    // share one canonical transport truth. Any processor's processBlock writes;
    // UI (PluginEditor) reads via getSharedPlayHeadState().
    PlayHeadState sharedPlayHeadState_;

    AudioSource* findAudioSource(juce::ARAAudioSource* audioSource);
    AudioSource* findAudioSource(const juce::String& persistentId);
    const AudioSource* findAudioSource(const juce::String& persistentId) const;
    AudioSource& ensureAudioSource(juce::ARAAudioSource* audioSource);
    AudioModification* findAudioModification(const juce::String& persistentId);
    const AudioModification* findAudioModification(const juce::String& persistentId) const;
    AudioModification* findAudioModification(juce::ARAAudioModification* audioModification);
    AudioModification& ensureAudioModification(juce::ARAAudioModification* audioModification);
    ContentKey bindAudioModificationIdentity(AudioModification& modification);
    ContentKey makeAudioModificationContentKey(const juce::String& persistentId);
    const juce::String* findPersistentIdForAudioModificationKey(ContentKey key) const;

private:
    // Internal lookup — mutation API callers should use applyXxxToModification() instead
    AudioModification* findAudioModificationByContentKey(const ContentKey& key);
    const AudioModification* findAudioModificationByContentKey(const ContentKey& key) const;

public:
    /** Shared PlayHeadState for all ARA roles bound to this document. The
     *  playback renderer publishes host transport here; UI binds to this. */
    const PlayHeadState& getSharedPlayHeadState() const noexcept { return sharedPlayHeadState_; }

    // ARA mutation API — Processor delegates ARA writes here
    bool applyNotesToModification(const ContentKey& key, std::vector<Note> notes);
    // Volume envelope 编辑不触发神经渲染。
    bool applyVolumeEnvelopeToModification(const ContentKey& key, AutomationLane envelope);
    // 无渲染 republish：从最新 owner snapshot 原子发布播放源。
    // 不 enqueue render、不失效 RenderCache。
    void republishPlaybackSourceForModification(ContentKey key);
    bool applyPitchCurveToModification(const ContentKey& key, std::shared_ptr<PitchCurve> curve);
    bool applyNotesAndPitchCurveToModification(const ContentKey& key,
                                               std::vector<Note> notes,
                                               std::shared_ptr<PitchCurve> curve);
    bool applyOriginalF0ToModification(const ContentKey& key, std::shared_ptr<PitchCurve> curve);
    bool applyTimeGridToModification(const ContentKey& key, std::shared_ptr<const TimeGridSnapshot> grid);
    bool applyPitchShiftStateToModification(const ContentKey& key, const PitchShiftEditState& state);
    bool applyDetectedKeyToModification(const ContentKey& key, const DetectedKey& detectedKey);
    bool applyReferenceFeaturesToModification(const ContentKey& key, const ReferenceFeatureSet& features);
    bool applyOriginalF0StateToModification(const ContentKey& key, const OriginalF0State& state);
    bool applyOriginalF0FailureReasonToModification(const ContentKey& key, std::string reason);

private:
    PlaybackRegion* findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);
    const PlaybackRegion* findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion) const;
    PlaybackRegion& ensurePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);
    PlaybackRegionProjection makeProjection(const PlaybackRegion& region) const;
    std::vector<OpenTunePlaybackRenderer*> publishModelChange();
    static void refreshRegisteredRenderers(const std::vector<OpenTunePlaybackRenderer*>& renderers);
    bool publishPlaybackReadSourceForModification(
        AudioModification& modification,
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer);
    void readRestoredAudio(const AudioSource* enabledSource);
    bool birthContentForModification(AudioModification& modification);
    void removeCRSArtifactsForModification(const AudioModification& modification);
    bool scheduleAsyncF0Extraction(ContentKey contentKey,
                                   std::vector<float> channel0Data,
                                   double sourceSampleRate,
                                   uint64_t wrapperGeneration,
                                   juce::String persistentId,
                                   OriginalF0InputStamp stamp);
    void timerCallback() override;
    void commitF0Completion(F0CompletionRecord record);
    static OriginalF0InputStamp makeF0InputStamp(const AudioSource& source, const AudioModification& modification);
    std::shared_ptr<const EditableContentSnapshot> snapshotAudioModification(ContentKey key) const;
    void installDocumentRenderExecution();
    static void processDocumentRenderJob(std::shared_ptr<ContentRenderService> contentRenderService,
                                         RenderJob& job,
                                         std::shared_ptr<ProcessRenderRuntime::CompletionGate> completionGate);
    bool removePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);

    // Track FX composite 派生缓存（消息线程访问；读路径 const，重建用 mutable）
    struct CompositeContent
    {
        ContentKey key;
        uint64_t instanceToken{0};
        // 当前 composite scope 的 PlaybackRegion 指针列表（update 时写入）。
        // dirty rebuild 按此重新投影 segments，保证 region 几何变化后用新几何。
        std::vector<juce::ARAPlaybackRegion*> scopeRegions;
        std::vector<CompositeSegment> segments;
        std::shared_ptr<const EditableContentSnapshot> snapshot;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        bool dirty{true};
        uint64_t generation{0};
    };
    mutable std::unordered_map<uint64_t, CompositeContent> compositeContents_;
    uint64_t nextCompositeInstanceToken_{1};
    CompositeContent* findCompositeContent(ContentKey key);
    const CompositeContent* findCompositeContent(ContentKey key) const;
    void rebuildCompositeContent(CompositeContent& content) const;
    void ensureCompositeUpToDate(const CompositeContent& content) const;
    void markAllCompositeContentsDirty();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneDocumentController)
};

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

} // namespace OpenTune
