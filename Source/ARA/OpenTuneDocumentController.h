#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <map>
#include <optional>
#include <vector>
#include <functional>

#include "AudioModification.h"
#include "AudioSource.h"
#include "../Render/ContentRenderService.h"
#include "../Services/F0ExtractionService.h"
#include "../Content/ContentKey.h"
#include "PlaybackRegion.h"

namespace OpenTune {

class OpenTuneEditorView;
class OpenTunePlaybackRenderer;
class OpenTuneAudioProcessor;
class ResamplingManager;
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

        double endInPlaybackTime() const noexcept { return startInPlaybackTime + durationInPlaybackTime; }
        bool isRenderable() const noexcept;
    };

    OpenTuneDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                               const ARA::ARADocumentControllerHostInstance* instance);

    ~OpenTuneDocumentController() override;

    // Per ARA2 spec: ARA object persistence uses doStoreObjectsToStream/doRestoreObjectsFromStream,
    // NOT VST3 processor state. Legacy getContentSnapshot/restoreContentPayloadInto removed.

    const ContentRenderService* getContentRenderService() const noexcept;
    bool refreshPlaybackReadSource(ContentKey key);
    // ARA mutation/render API — processor 通过这些 API 请求 ARA 渲染
    void refreshModificationCRSMetadata(ContentKey key);
    void requestModificationRender(ContentKey key, double startSeconds, double endSeconds);
    void requestFullModificationRender(ContentKey key);
    void requestModificationStage2Rebuild(ContentKey key);
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

    /** 从 CRS renderCache 读取渲染统计 */
    RenderCache::ChunkStats readChunkStats(ContentKey key) const;

    /** 从 CRS renderCache 读取 chunk 边界 */
    bool readChunkBoundaries(ContentKey key, std::vector<double>& outSeconds) const;

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
    int refreshAllAudioModifications();
    // ARA SDK requires DocumentController operations on main thread.
    // This method executes synchronously to comply with ARA thread constraints.
    // Callers should display a loading overlay before calling if UI responsiveness is needed.
    void refreshAllAudioModificationsAsync(std::function<void(int)> completionCallback);
    void setAsyncWorkThreadPool(juce::ThreadPool* pool) noexcept { asyncWorkPool_ = pool; }
    void setEditorViewSelectionPlaybackRegions(std::vector<juce::ARAPlaybackRegion*> playbackRegions);
    void registerPlaybackRenderer(OpenTunePlaybackRenderer& renderer);
    void unregisterPlaybackRenderer(OpenTunePlaybackRenderer& renderer);

    void didUpdateMusicalContextProperties(juce::ARAMusicalContext* musicalContext) override;
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
    bool requestStartPlayback();
    bool requestStopPlayback();

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

    // 异步工作线程池（由 Processor 在 didBindToARA 时注入）
    juce::ThreadPool* asyncWorkPool_{nullptr};

    // 服务租约 token：DC 析构时置 false，后台 F0 work 持有 shared_ptr 可安全检查
    std::shared_ptr<std::atomic<bool>> asyncLeaseToken_;

    AudioSource* findAudioSource(juce::ARAAudioSource* audioSource);
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
    // ARA mutation API — Processor delegates ARA writes here
    bool applyNotesToModification(const ContentKey& key, std::vector<Note> notes);
    bool applyPitchCurveToModification(const ContentKey& key, std::shared_ptr<PitchCurve> curve);
    bool applyTimeGridToModification(const ContentKey& key, std::shared_ptr<const TimeGridSnapshot> grid);
    bool applyPitchShiftToModification(const ContentKey& key, const PitchShiftSettings& settings);
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
    bool birthContentForModification(AudioModification& modification);
    bool rebuildCRSFromSource(AudioModification& modification);
    void removeCRSArtifactsForModification(const AudioModification& modification);
    int rebuildCRSForSource(const AudioSource& source);
    void scheduleAsyncF0Extraction(ContentKey contentKey,
                                   std::vector<float> channel0Data,
                                   double sourceSampleRate);
    void installDocumentRenderExecution();
    void processDocumentRenderJob(RenderJob& job);
    std::shared_ptr<const EditableContentSnapshot> snapshotAudioModification(ContentKey key) const;
    void handleDocumentStage1ChunkPublished(ContentKey key, uint64_t publishedRevision);
    bool removePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneDocumentController)
};

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

} // namespace OpenTune
