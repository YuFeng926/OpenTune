#pragma once

#include "../Content/ContentKey.h"
#include "../Render/PlaybackReadSource.h"
#include "../Utils/PlaybackSampleGeometry.h"
#include "../Utils/RealtimeSnapshot.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace OpenTune {

// JUCE ARA playback semantics: realtime calls render only while the host is
// playing; offline/non-realtime calls may still render while stopped.
inline bool shouldRenderPlaybackBlock(juce::AudioProcessor::Realtime realtime,
                                      bool isPlaying) noexcept
{
    return realtime != juce::AudioProcessor::Realtime::yes || isPlaying;
}

#if JucePlugin_Enable_ARA
class OpenTuneDocumentController;
class ContentRenderService;
class NotePitchAudition;
struct OpenTunePlaybackRendererTestAccess;
class OpenTunePlaybackRenderer : public juce::ARAPlaybackRenderer,
                                 private juce::AsyncUpdater
{
public:
    OpenTunePlaybackRenderer(ARA::PlugIn::DocumentController* araDc,
                             OpenTuneDocumentController* docController);
    using juce::ARAPlaybackRenderer::ARAPlaybackRenderer;

    // Published projection of one playback region. The semantic time fields are
    // immutable snapshots of the authoritative owner values; the placement is
    // the fixed integer mapping derived from them at the plan's target rate.
    struct PlaybackRegionRenderItem
    {
        juce::ARAPlaybackRegion* playbackRegion{nullptr};
        ContentKey contentKey;
        double startInPlaybackTime{0.0};
        double endInPlaybackTime{0.0};
        double startInModificationTime{0.0};
        double endInModificationTime{0.0};
        // M of the fixed mapping: identity uses the global source time of the
        // region start; non-identity uses the TimeGrid output-domain anchor.
        double placementAnchorSeconds{0.0};
        // Content-local output time at the region's playback start, used as the
        // automation request anchor. Traversal adds integer sample spacing.
        double semanticAnchorSeconds{0.0};
        // Slices with a real predecessor share its fixed D. Predecessors are
        // resolved by source provenance and exact shared boundaries, not by
        // adjacency in the sorted plan.
        uint32_t continuityGroup{0};
        PlaybackSampleGeometry::Placement placement;
        PlaybackReadSource source;
    };

    struct RenderPlan
    {
        double targetSampleRate{0.0};
        // False while CRS source views still belong to another sample rate.
        // The plan carries the region topology but must not render.
        bool sourceViewsPrepared{true};
        std::vector<juce::ARAPlaybackRegion*> playbackRegions;
        std::vector<PlaybackRegionRenderItem> items;
    };

    ~OpenTunePlaybackRenderer() override;

    // Owner-driven detach: clears documentController_ for matching owner and publishes empty render plan.
    // Called by OpenTuneDocumentController before clearing playbackRenderers_ in destructor.
    void detachDocumentController(OpenTuneDocumentController& owner);

    void refreshRenderPlanFromDocument();
    void collectPlaybackSnapshots();

    // Message-thread pure projection: orders items by playback position,
    // resolves each item's actual predecessor, assigns continuity groups, and
    // derives the fixed mapping at targetSampleRate. Exposed for contract tests.
    static void assignContinuityAndPlacements(
        std::vector<PlaybackRegionRenderItem>& items,
        double targetSampleRate);

    void prepareToPlay(double sampleRate,
                       int maximumSamplesPerBlock,
                       int numChannels,
                       juce::AudioProcessor::ProcessingPrecision precision,
                       AlwaysNonRealtime alwaysNonRealtime) override;

    void releaseResources() override;

    bool processBlock(juce::AudioBuffer<float>& buffer,
                      juce::AudioProcessor::Realtime realtime,
                      const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;

protected:
    void didAddPlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;
    void willRemovePlaybackRegion(ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;

private:
    friend struct OpenTunePlaybackRendererTestAccess;

    static constexpr uint32_t lifecycleClosingBit_ = uint32_t{1} << 31;
    static constexpr uint32_t lifecycleReaderMask_ = lifecycleClosingBit_ - 1;
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "ARA playback lifecycle gate must be lock-free");
    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<const NotePitchAudition*>::is_always_lock_free);

    class LifecycleReadGuard
    {
    public:
        explicit LifecycleReadGuard(OpenTunePlaybackRenderer& renderer) noexcept;
        ~LifecycleReadGuard();

        LifecycleReadGuard(const LifecycleReadGuard&) = delete;
        LifecycleReadGuard& operator=(const LifecycleReadGuard&) = delete;
        explicit operator bool() const noexcept { return renderer_ != nullptr; }

    private:
        OpenTunePlaybackRenderer* renderer_;
    };

    bool enterLifecycle() noexcept;
    void leaveLifecycle() noexcept;
    void closeLifecycleAndWait();
    void finishPendingRenderPlanPublication();
    void handleAsyncUpdate() override;

    std::atomic<double> hostSampleRate_{0.0};
    std::atomic<double> pendingRenderPlanSampleRate_{0.0};
    std::atomic<uint32_t> lifecycleGate_{0};
    int numChannels_ = 2;
    int maximumSamplesPerBlock_ = 512;
    juce::AudioBuffer<float> playbackScratch_;

    OpenTuneDocumentController* documentController_ = nullptr;
    std::shared_ptr<ContentRenderService> contentRenderService_;
    std::atomic<const NotePitchAudition*> notePitchAudition_{nullptr};

    RealtimeSnapshot<RenderPlan> currentPlan_;

    // Message thread: reads DC projections and source views into an immutable plan.
    std::shared_ptr<RenderPlan> buildRenderPlan(
        std::vector<juce::ARAPlaybackRegion*> playbackRegions,
        double targetSampleRate) const;
    void publishRenderPlanFor(std::vector<juce::ARAPlaybackRegion*> playbackRegions,
                              double targetSampleRate);
    void clearPlaybackSnapshotsAndWait();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenTunePlaybackRenderer)
};
#endif // JucePlugin_Enable_ARA

}
