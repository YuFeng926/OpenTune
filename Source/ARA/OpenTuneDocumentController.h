#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <vector>
#include <functional>

#include "AudioModification.h"
#include "AudioSource.h"
#include "../Render/ContentRenderService.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include "../Services/F0ExtractionService.h"
#include "../Content/ContentKey.h"
#include "../Utils/PlayHeadState.h"
#include "PlaybackRegion.h"

namespace OpenTune {

class OpenTuneEditorView;
class OpenTunePlaybackRenderer;
class OpenTuneAudioProcessor;
class ResamplingManager;
struct RenderJob;
class OpenTuneDocumentController : public juce::ARADocumentControllerSpecialisation
{
public:
    struct PlaybackRegionProjection
    {
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        juce::String audioModificationPersistentId;
        SourceWindow contentWindow;
        uint64_t contentRevision{0};
        uint64_t placementRevision{0};
        std::optional<juce::Colour> displayColour;
        double startInPlaybackTime{0.0};
        double startInModificationTime{0.0};
        double durationInPlaybackTime{0.0};
        double durationInModificationTime{0.0};
        double contentDurationSeconds{0.0};
        double sampleRate{44100.0};
        int numChannels{0};
        bool timestretchEnabled{false};
        bool timestretchReflectingTempo{false};
        bool contentBasedFadeAtHead{false};
        bool contentBasedFadeAtTail{false};

        ContentKey contentKey;
        // renderer-only gate: true only when AudioModification::isRenderable()
        // (CRS playback source已建立)。UI projection 不再以它阻断。
        bool playbackSourceReady{false};

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
    void refreshModificationCRSMetadata(ContentKey key);
    void requestModificationRender(ContentKey key, double startSeconds, double endSeconds);
    void requestFullModificationRender(ContentKey key);
    void invalidateAllModificationCaches();
    // ============================================================
    // 编辑器只读内容访问器（通过 ContentKey 路由到 AudioModification + CRS）
    // ARA 模式下编辑器不经过 content owner，直接读 AudioModification.content
    // ============================================================

    /** 从 CRS 读取音频 buffer */
    std::shared_ptr<const juce::AudioBuffer<float>> readAudioBuffer(ContentKey key) const;

    /** 从 AudioModification.content.analysis 读取 pitch curve */
    std::shared_ptr<PitchCurve> readPitchCurve(ContentKey key) const;

    /** 读取 OriginalF0 状态 */
    OriginalF0State readOriginalF0State(ContentKey key) const;

    /** 读取调性检测结果 */
    DetectedKey readDetectedKey(ContentKey key) const;

    /** 读取音符 */
    std::vector<Note> readNotes(ContentKey key) const;

    /** 读取音符版本号 */
    uint64_t readNotesRevision(ContentKey key) const;

    /** 读取时间网格 */
    std::shared_ptr<const TimeGridSnapshot> readTimeGrid(ContentKey key) const;

    /** 读取时间网格版本号 */
    uint64_t readTimeGridRevision(ContentKey key) const;

    /** 读取音高移调设置 */
    PitchShiftSettings readPitchShift(ContentKey key) const;

    /** 读取内容版本号 */
    uint64_t readContentRevision(ContentKey key) const;

    /** 读取材质化时长 */
    double readContentDuration(ContentKey key) const;

    /** 是否有内容 */
    bool hasContent(ContentKey key) const;

    /** Phase 4: 返回 EditableContentSnapshot — 唯一的跨域 snapshot 类型 */
    std::shared_ptr<const EditableContentSnapshot> readContentSnapshot(ContentKey key) const;

    std::vector<PlaybackRegionProjection> getPlaybackRegionProjections() const;
    std::vector<PlaybackRegionProjection> getPlaybackRegionProjectionsFor(
        const std::vector<juce::ARAPlaybackRegion*>& playbackRegions) const;
    std::vector<PlaybackRegionProjection> getEditorSelectionPlaybackRegionProjections() const;
    std::optional<PlaybackRegionProjection> getFocusedEditorPlaybackRegionProjection() const;
    // 用户 Read 入口：只读取当前 focused PlaybackRegion 对应的内容。
    // archive 恢复且已有有效 F0 的内容由 readRestoredAudio 自动读取，无需此入口。
    int requestReadAudioForPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);
    // ARA SDK requires DocumentController operations on main thread.
    // This method executes synchronously to comply with ARA thread constraints.
    // Callers should display a loading overlay before calling if UI responsiveness is needed.
    void requestReadAudioForPlaybackRegionAsync(juce::ARAPlaybackRegion* playbackRegion,
                                                std::function<void(int)> completionCallback);
    void setEditorViewSelectionPlaybackRegions(std::vector<juce::ARAPlaybackRegion*> playbackRegions);
    void registerPlaybackRenderer(OpenTunePlaybackRenderer& renderer);
    void unregisterPlaybackRenderer(OpenTunePlaybackRenderer& renderer);

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
    bool requestStartPlayback(double pendingSeekTime = -1.0);
    bool requestStopPlayback();
    bool requestTogglePlayback(bool fallbackObservedPlaying, double pendingSeekTime = -1.0);
    void observeHostPlaybackState(bool isPlaying) noexcept;
    // Publish host PositionInfo to document-shared PlayHeadState. Called from
    // processBlock of any ARA role that receives a host PositionInfo. The CAS
    // projection handles multi-writer contention; canonical atomics always win.
    void observeHostPlaybackPosition(
        const juce::Optional<juce::AudioPlayHead::PositionInfo>& positionInfo,
        double blockDurationSeconds) noexcept;
    // ARA2 official one-way HostPlaybackController requests for loop control.
    // Per ARA2 spec, host may ignore/delay/quantize; loop truth is observed
    // via companion PositionInfo in processBlock, never written here.
    bool requestEnableCycle(bool enabled);
    bool requestSetCycleRange(double startTime, double duration);

protected:
    bool doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                    const juce::ARARestoreObjectsFilter* filter) override;
    bool doStoreObjectsToStream(juce::ARAOutputStream& output,
                                const juce::ARAStoreObjectsFilter* filter) override;

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() override;
    juce::ARAEditorView* doCreateEditorView() override;

private:
    std::vector<AudioSource> audioSources_;
    std::vector<AudioModification> audioModifications_;
    std::vector<PlaybackRegion> playbackRegions_;
    std::vector<juce::ARAPlaybackRegion*> editorSelectionPlaybackRegions_;
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

    // 服务租约 token：DC 析构时置 false，后台 F0 work 持有 shared_ptr 可安全检查
    std::shared_ptr<std::atomic<bool>> asyncLeaseToken_;

    // Document-level shared PlayHeadState: all ARA roles within this document
    // share one canonical transport truth. Any processor's processBlock writes;
    // UI (PluginEditor) reads via getSharedPlayHeadState().
    PlayHeadState sharedPlayHeadState_;

    // Stage1 → Stage2 异步完成回调 gate，跟随 DC 析构关闭。
    std::shared_ptr<ProcessRenderRuntime::CompletionGate> completionGate_;
    void handleStage1ChunkSettled(ContentKey key);

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
    /** Shared PlayHeadState for all ARA roles bound to this document. Any
     *  processor whose processBlock is called writes here; UI binds to this. */
    const PlayHeadState& getSharedPlayHeadState() const noexcept { return sharedPlayHeadState_; }

    // ARA mutation API — Processor delegates ARA writes here
    bool applyNotesToModification(const ContentKey& key, std::vector<Note> notes);
    // Volume envelope 编辑不触发神经渲染。
    bool applyVolumeEnvelopeToModification(const ContentKey& key, AutomationLane envelope);
    // 无渲染 republish：从最新 owner snapshot 原子发布播放源。
    // 不 enqueue render、不失效 RenderCache、不重建 TimeStretchCache。
    void republishPlaybackSourceForModification(ContentKey key);
    bool applyPitchCurveToModification(const ContentKey& key, std::shared_ptr<PitchCurve> curve);
    bool applyOriginalF0ToModification(const ContentKey& key, std::shared_ptr<PitchCurve> curve);
    bool applyTimeGridToModification(const ContentKey& key, std::shared_ptr<const TimeGridSnapshot> grid);
    bool applyPitchShiftStateToModification(const ContentKey& key, const PitchShiftEditState& state);
    bool applyDetectedKeyToModification(const ContentKey& key, const DetectedKey& detectedKey);
    bool applyReferenceFeaturesToModification(const ContentKey& key, const ReferenceFeatureSet& features);
    bool applyOriginalF0StateToModification(const ContentKey& key, const OriginalF0State& state);

private:
    PlaybackRegion* findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);
    const PlaybackRegion* findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion) const;
    PlaybackRegion& ensurePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);
    PlaybackRegionProjection makeProjection(const PlaybackRegion& region) const;
    std::vector<PlaybackRegionProjection> buildProjections() const;
    std::vector<OpenTunePlaybackRenderer*> publishModelChange();
    static void refreshRegisteredRenderers(const std::vector<OpenTunePlaybackRenderer*>& renderers);
    void reconcileEditorSelectionPlaybackRegions();
    bool publishPlaybackReadSourceForModification(
        AudioModification& modification,
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer);
    void readRestoredAudio(const AudioSource* enabledSource);
    bool birthContentForModification(AudioModification& modification);
    void removeCRSArtifactsForModification(const AudioModification& modification);
    bool scheduleAsyncF0Extraction(ContentKey contentKey,
                                   std::vector<float> channel0Data,
                                   double sourceSampleRate,
                                   juce::ARAAudioModification* hostModification);
    std::shared_ptr<const EditableContentSnapshot> snapshotAudioModification(ContentKey key) const;
    void installDocumentRenderExecution();
    void processDocumentRenderJob(RenderJob& job);
    bool removePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneDocumentController)
};

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

} // namespace OpenTune
