#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <optional>
#include <vector>
#include <functional>

#include "AudioModification.h"
#include "AudioSource.h"
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
        uint64_t materializationId{0};
        uint64_t materializationRevision{0};
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
    };

    void attachProcessorServices(ProcessorServices services);
    void detachProcessorServices(const OpenTuneAudioProcessor* owner);

    void runContentReclaimSweep();
    void scheduleContentReclaim();
    void getContentSnapshot(juce::XmlElement& dest) const;
    void restoreContentPayloadInto(const juce::XmlElement& src);

    MaterializationStore* getMaterializationStore() const noexcept;
    SourceStore* getSourceStore() const noexcept;

    std::vector<PlaybackRegionProjection> getPlaybackRegionProjections() const;
    std::vector<PlaybackRegionProjection> getPlaybackRegionProjectionsFor(
        const std::vector<juce::ARAPlaybackRegion*>& playbackRegions) const;
    std::vector<PlaybackRegionProjection> getEditorSelectionPlaybackRegionProjections() const;
    std::optional<PlaybackRegionProjection> getFocusedEditorPlaybackRegionProjection() const;
    bool referencesMaterialization(uint64_t materializationId) const;
    int refreshAllAudioModifications();
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
    struct RestoredMaterializationBinding
    {
        juce::String audioModificationPersistentId;
        SourceWindow sourceWindow;
        uint64_t sourceId{0};
        uint64_t materializationId{0};
        uint64_t materializationRevision{0};
        double materializationDurationSeconds{0.0};
    };

    std::vector<AudioSource> audioSources_;
    std::vector<AudioModification> audioModifications_;
    std::vector<PlaybackRegion> playbackRegions_;
    std::vector<juce::ARAPlaybackRegion*> editorSelectionPlaybackRegions_;
    std::vector<OpenTunePlaybackRenderer*> playbackRenderers_;
    std::vector<RestoredMaterializationBinding> pendingRestoredBindings_;

    std::shared_ptr<MaterializationStore> materializationStore_;
    std::shared_ptr<SourceStore> sourceStore_;
    std::shared_ptr<ResamplingManager> resamplingManager_;
    std::shared_ptr<F0InferenceService> f0Service_;
    std::function<void(std::function<void()>&&)> scheduleAsyncWork_;
    std::function<void()> onReclaimNeeded_;
    const OpenTuneAudioProcessor* serviceOwner_ = nullptr;

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
    void applyRestoredBinding(AudioModification& modification,
                              const RestoredMaterializationBinding& binding) noexcept;
    bool applyPendingRestoredBinding(AudioModification& modification);
    void rememberPendingRestoredBinding(RestoredMaterializationBinding binding);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTuneDocumentController)
};

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

} // namespace OpenTune
