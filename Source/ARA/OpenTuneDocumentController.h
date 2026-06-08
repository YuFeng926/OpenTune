#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <optional>
#include <vector>
#include <functional>

#include "AudioModification.h"
#include "AudioSource.h"
#include "../MaterializationStore.h"
#include "../Render/ContentRenderService.h"
#include "../Content/ContentKey.h"
#include "PlaybackRegion.h"

namespace OpenTune {

class OpenTuneEditorView;
class OpenTunePlaybackRenderer;
class OpenTuneAudioProcessor;
class F0InferenceService;
class ResamplingManager;
class MaterializationStore;
class SourceStore;

class OpenTuneDocumentController : public juce::ARADocumentControllerSpecialisation
{
public:
    struct PlaybackRegionProjection
    {
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        juce::String audioModificationPersistentId;
        SourceWindow contentWindow;
        uint64_t sourceId{0};
        uint64_t contentRevision{0};
        uint64_t placementRevision{0};
        double startInPlaybackTime{0.0};
        double startInModificationTime{0.0};
        double durationInPlaybackTime{0.0};
        double durationInModificationTime{0.0};
        double materializationDurationSeconds{0.0};
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

    struct ProcessorServices
    {
        const OpenTuneAudioProcessor* owner = nullptr;
        std::shared_ptr<F0InferenceService> f0Service;
        std::function<void(std::function<void()>&&)> scheduleAsyncWork;
        std::function<void()> requestReclaimSweep;
        ContentRenderService* contentRenderService{nullptr};
    };

    void attachProcessorServices(ProcessorServices services);
    void detachProcessorServices(const OpenTuneAudioProcessor* owner);

    void runContentReclaimSweep();
    void scheduleContentReclaim();
    void getContentSnapshot(juce::XmlElement& dest) const;
    void restoreContentPayloadInto(const juce::XmlElement& src);

    ContentRenderService* getContentRenderService() const noexcept;
    SourceStore* getSourceStore() const noexcept;

    // ============================================================
    // 编辑器只读内容访问器（通过 ContentKey 路由到 AudioModification + CRS）
    // ARA 模式下编辑器不经过 MaterializationStore，直接读 AudioModification.content
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

    /** 读取音符快照 */
    MaterializationStore::MaterializationNotesSnapshot readNotesSnapshot(ContentKey key) const;

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
    double readMaterializationDuration(ContentKey key) const;

    /** 读取 sourceId */
    uint64_t readSourceId(ContentKey key) const;

    /** 是否有内容 */
    bool hasContent(ContentKey key) const;

    /** 组合快照（兼容旧 MaterializationSnapshot 接口） */
    MaterializationStore::MaterializationSnapshot readSnapshot(ContentKey key) const;

    std::vector<PlaybackRegionProjection> getPlaybackRegionProjections() const;
    std::vector<PlaybackRegionProjection> getPlaybackRegionProjectionsFor(
        const std::vector<juce::ARAPlaybackRegion*>& playbackRegions) const;
    std::vector<PlaybackRegionProjection> getEditorSelectionPlaybackRegionProjections() const;
    std::optional<PlaybackRegionProjection> getFocusedEditorPlaybackRegionProjection() const;
    int refreshAllAudioModifications();
    void setEditorViewSelectionPlaybackRegions(std::vector<juce::ARAPlaybackRegion*> playbackRegions);
    void registerPlaybackRenderer(OpenTunePlaybackRenderer& renderer);
    void unregisterPlaybackRenderer(OpenTunePlaybackRenderer& renderer);

    // 退休内容池查询（undo/revive 入口）
    const std::vector<RetiredContentRecord>& getRetiredContents() const noexcept { return retiredContents_; }
    bool reviveRetiredContentByKey(const ContentKey& key, AudioModification& target);
    void releaseRetiredContent(const ContentKey& key);

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
    std::vector<RetiredContentRecord> retiredContents_;

    // 保留 MaterializationStore 作为向后兼容（短期），不再做内容路由
    std::shared_ptr<MaterializationStore> materializationStore_;
    std::shared_ptr<SourceStore> sourceStore_;
    ContentRenderService* contentRenderService_{nullptr};
    std::shared_ptr<ResamplingManager> resamplingManager_;
    std::shared_ptr<F0InferenceService> f0Service_;
    std::function<void(std::function<void()>&&)> scheduleAsyncWork_;
    std::function<void()> onReclaimNeeded_;
    const OpenTuneAudioProcessor* serviceOwner_ = nullptr;

    // 服务租约 token：detach 时置 false，后台 F0 work 持有 shared_ptr 可安全检查
    std::shared_ptr<std::atomic<bool>> asyncLeaseToken_;

    struct ReclaimAsyncUpdater : juce::AsyncUpdater
    {
        using Callback = std::function<void()>;
        explicit ReclaimAsyncUpdater(Callback cb) : callback(std::move(cb)) {}
        void handleAsyncUpdate() override { if (callback) callback(); }
        Callback callback;
    };

    mutable ReclaimAsyncUpdater reclaimAsyncUpdater_;

    AudioSource* findAudioSource(juce::ARAAudioSource* audioSource);
    const AudioSource* findAudioSource(const juce::String& persistentId) const;
    AudioSource& ensureAudioSource(juce::ARAAudioSource* audioSource);
    AudioModification* findAudioModification(const juce::String& persistentId);
    const AudioModification* findAudioModification(const juce::String& persistentId) const;
    AudioModification* findAudioModification(juce::ARAAudioModification* audioModification);
    AudioModification* findAudioModificationByContentKey(const ContentKey& key);
    const AudioModification* findAudioModificationByContentKey(const ContentKey& key) const;
    AudioModification& ensureAudioModification(juce::ARAAudioModification* audioModification);
    PlaybackRegion* findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);
    const PlaybackRegion* findPlaybackRegion(juce::ARAPlaybackRegion* playbackRegion) const;
    PlaybackRegion& ensurePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);
    PlaybackRegionProjection makeProjection(const PlaybackRegion& region) const;
    std::vector<PlaybackRegionProjection> buildProjections() const;
    std::vector<OpenTunePlaybackRenderer*> publishModelChange();
    static void refreshRegisteredRenderers(const std::vector<OpenTunePlaybackRenderer*>& renderers);
    void reconcileEditorSelectionPlaybackRegions();
    bool birthMaterializationForModification(AudioModification& modification);
    void scheduleAsyncF0Extraction(uint64_t materializationId,
                                   std::vector<float> channel0Data,
                                   double sourceSampleRate);
    bool removePlaybackRegion(juce::ARAPlaybackRegion* playbackRegion);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneDocumentController)
};

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

} // namespace OpenTune
