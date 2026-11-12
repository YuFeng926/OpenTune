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
class NotePitchAudition;
class NoteAudition;
struct RenderJob;

/**
 * Document-shared audition routing published for the audio callbacks.
 * The message thread publishes sources and the note-pitch mixer; processBlock
 * only loads atomics and claims the single keyboard mixer with a CAS.
 */
struct AuditionRoute
{
    std::atomic<NotePitchAudition*> notePitchAudition{nullptr};
    std::atomic<NoteAudition*> keyboardAudition{nullptr};
    std::atomic<juce::ARAEditorRenderer*> notePitchMixer{nullptr};
    std::atomic<juce::ARAEditorRenderer*> keyboardMixer{nullptr};
};

class OpenTuneDocumentController : public juce::ARADocumentControllerSpecialisation
{
public:
    struct PlaybackRegionProjection
    {
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        std::optional<juce::Colour> displayColour;
        double startInPlaybackTime{0.0};
        double endInPlaybackTime{0.0};
        double startInModificationTime{0.0};
        double endInModificationTime{0.0};
        double durationInPlaybackTime{0.0};
        double durationInModificationTime{0.0};

        ContentKey contentKey;

        bool isPlaybackRenderable() const noexcept;
    };

    OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                               const ARA::ARADocumentControllerHostInstance* instance);

    ~OpenTuneDocumentController() override;

    // Per ARA2 spec: ARA object persistence uses doStoreObjectsToStream/doRestoreObjectsFromStream,
    // NOT VST3 processor state. Legacy getContentSnapshot/restoreContentPayloadInto removed.

    // Content archive version. Version 9 stores the source window's exclusive
    // end directly; earlier versions stored duration and are decoded only
    // through those earlier version numbers.
    static constexpr int kContentArchiveVersion = 9;
    static constexpr int kContentArchiveVersionExplicitSourceEnd = 9;

    // Source-window archive codec shared by store/restore and contract tests.
    static void writeSourceWindowToArchive(const SourceWindow& window, juce::XmlElement& element);
    static std::optional<SourceWindow> readSourceWindowFromArchive(
        const juce::XmlElement& element,
        const juce::ARARestoreObjectsFilter* filter,
        int archiveVersion);

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
    int requestReadAudioForPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);
    // ARA SDK requires DocumentController operations on main thread.
    // This method executes synchronously to comply with ARA thread constraints.
    // Callers should display a loading overlay before calling if UI responsiveness is needed.
    void requestReadAudioForPlaybackRegionAsync(juce::ARAPlaybackRegion* playbackRegion,
                                                std::function<void(int)> completionCallback);
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

    // True when the host is Logic and document-shared transport reports playing.
    bool logicHostIsPlaying() const noexcept;

    struct HostTransportRelayState;
    std::shared_ptr<HostTransportRelayState> transportRelay_;
    void enqueueHostTransport(const std::function<void(HostTransportCoalescer&)>& mutate);
    void scheduleHostTransportFlush();
    void performHostTransportBatch(const HostTransportBatch& batch);
    bool hostSeekAvailable();

    // F0 completion 无锁提交：completion 由 F0ExtractionService 投递到消息线程
    // 执行，捕获共享 sink；析构时在消息线程把指针置空，回调看到空指针直接退出。
    struct F0CompletionRecord
    {
        F0ExtractionService::Result result;
        ContentKey key;
        uint64_t wrapperGeneration{0};
        juce::String persistentId;
        uint64_t birthRevision{0};
        OriginalF0InputStamp stamp;
    };
    struct CompletionSink
    {
        OpenTuneDocumentController* controller{nullptr};
    };
    std::shared_ptr<CompletionSink> completionSink_{std::make_shared<CompletionSink>()};
    std::deque<OriginalF0FailureEvent> pendingF0FailureEvents_;
    std::shared_ptr<ProcessRenderRuntime::CompletionGate> renderCompletionGate_{
        std::make_shared<ProcessRenderRuntime::CompletionGate>()};
    uint64_t nextWrapperGeneration_{1};

    // Document-level shared PlayHeadState: all ARA roles within this document
    // share one canonical transport truth. Realtime editor renderers publish;
    // playback renderers may be ahead and do not. UI reads via
    // getSharedPlayHeadState().
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
    // 文档级音符移调试听：UI 实例投递请求，拥有当前编辑 region 的 editor renderer 混音。
    NotePitchAudition& getNotePitchAudition() noexcept;
    bool selectNotePitchAuditionRendererForRegion(juce::ARAPlaybackRegion* region) noexcept;

    // Keyboard note audition. The editor view owns it, but as an item FX in REAPER that
    // instance is a playback renderer and gets no audio blocks while stopped, so the
    // realtime editor renderer of the document mixes it instead.
    void setKeyboardAuditionSource(std::shared_ptr<NoteAudition> source);

    // Message-thread render terminal notification used by the editor for post-edit audition.
    using RenderTerminalCallback = std::function<void(ContentKey, bool)>;
    void setRenderTerminalCallback(RenderTerminalCallback callback);

    /** Audio callbacks read this stable route; no controller API is touched there. */
    AuditionRoute* getAuditionRoute() noexcept { return &auditionRoute_; }

    /** Shared PlayHeadState for all ARA roles bound to this document. The
     *  realtime editor renderer publishes host transport here; UI binds to this. */
    const PlayHeadState& getSharedPlayHeadState() const noexcept { return sharedPlayHeadState_; }
    /** Message thread: stable address handed to the audio callback at bind. */
    PlayHeadState& getSharedPlayHeadStateForAudio() noexcept { return sharedPlayHeadState_; }

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
        NativePcmSource nativeSource,
        std::shared_ptr<const juce::AudioBuffer<float>> canonicalBuffer,
        int64_t firstCanonicalSample);
    void readRestoredAudio(const AudioSource* enabledSource);
    bool birthContentForModification(AudioModification& modification);
    // Reuses a full native PCM already published for the same source identity and
    // sample content generation. Returns null when a fresh read is required.
    std::shared_ptr<const juce::AudioBuffer<float>> findReusableNativePcm(
        const AudioModification& modification,
        const juce::String& sourcePersistentId,
        double sourceSampleRate,
        uint64_t sampleGeneration,
        int64_t sourceSampleCount) const;
    void removeCRSArtifactsForModification(const AudioModification& modification);
    bool scheduleAsyncF0Extraction(ContentKey contentKey,
                                   std::vector<float> channel0Data,
                                   double sourceSampleRate,
                                   uint64_t wrapperGeneration,
                                   juce::String persistentId,
                                   OriginalF0InputStamp stamp);
    void commitF0Completion(F0CompletionRecord record);
    static OriginalF0InputStamp makeF0InputStamp(const AudioSource& source, const AudioModification& modification);
    std::shared_ptr<const EditableContentSnapshot> snapshotAudioModification(ContentKey key) const;
    void installDocumentRenderExecution();
    struct RenderTerminalRelay;
    static void processDocumentRenderJob(std::shared_ptr<ContentRenderService> contentRenderService,
                                         RenderJob& job,
                                         std::shared_ptr<ProcessRenderRuntime::CompletionGate> completionGate,
                                         std::shared_ptr<RenderTerminalRelay> renderTerminalRelay);
    bool removePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);

    std::unique_ptr<NotePitchAudition> notePitchAudition_;
    AuditionRoute auditionRoute_;
    std::vector<std::shared_ptr<NoteAudition>> keyboardAuditionSources_;

    // Render terminal notifications are built by the render worker and invoked on
    // the message thread; the relay keeps the callback lifetime independent of
    // the worker's completion copy.
    struct RenderTerminalRelay
    {
        std::mutex mutex;
        RenderTerminalCallback callback;
        // Success notifications are claimed once per key+render generation: a
        // multi-chunk render delivers one settled callback per chunk and every
        // queued callback observes the same fully settled cache.
        std::map<ContentKey, uint64_t> lastCompletedRenderGeneration;
        void set(RenderTerminalCallback next);
        void invoke(ContentKey key, bool completed);
        void invokeCompleted(ContentKey key, uint64_t renderGeneration);
    };
    std::shared_ptr<RenderTerminalRelay> renderTerminalRelay_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneDocumentController)
};

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

} // namespace OpenTune
