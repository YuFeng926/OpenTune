#pragma once


#include "CaptureSegment.h"
#include "../../Content/ContentKey.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace OpenTune::Capture {

/** Aggregated session state derived from segment states. */
enum class SessionState : int
{
    Idle = 0,         // no Capturing / Processing segment exists (Edited segments may exist)
    HasCapturing,     // some segment is in Capturing state
    HasProcessing     // some segment is in Processing state (no Capturing)
};

/** Snapshot of one segment for GUI consumption (returned by listSegments). */
struct SegmentInfo
{
    uint64_t id = 0;
    double T_start = 0.0;
    double durationSeconds = 0.0;
    SegmentState state = SegmentState::Capturing;
    ContentKey contentKey;
};

/**
 * Audio-thread-callable: replace [destStart, destStart+numSamples) of buffer with rendered audio
 * from the given segment. Implementation:
 *   - clear destination range
 *   - call contentRenderService->getPlaybackReadSource(ContentKey{RegularVST3Capture, segmentId, 0}, ...)
 *
 * Uses segment ContentKey directly.
 * Captures a small (one-pointer) lambda; no heap allocation expected when called.
 */
using ReplaceWithRenderedFn = std::function<void(juce::AudioBuffer<float>& buffer,
                                                  int destStart,
                                                  int numSamples,
                                                  ContentKey segmentContentKey,
                                                  double readStartSeconds,
                                                  double targetSampleRate)>;

/** Message-thread compaction sink: tell the processor to retire segment content. */
using RetireSegmentFn = std::function<void(ContentKey segmentContentKey)>;

/** Notification sink for UI: a segment has reached Edited state. */
using ActiveSegmentChangedFn = std::function<void(ContentKey segmentContentKey)>;

/** Message-thread re-render trigger for an existing segment content (no new clip created).
 *  Used by CapturePersistence::deserialize to repopulate RenderCache after restore — the
 *  vocoder output is not in standard state, so it must be re-synthesized from the restored
 *  audio + pitchCurve. tick() promotes Processing → Edited via F0 state check. */
using RefreshSegmentFn = std::function<void(ContentKey segmentContentKey)>;

/** Publish PlaybackReadSource to ContentRenderService with segment.id as ContentKey. */
using PublishPlaybackSourceFn = std::function<void(const ContentKey& key,
                                                     std::shared_ptr<const juce::AudioBuffer<float>> audio,
                                                     double sampleRate)>;

/** Enqueue a partial render to rebuild render cache from restored owner truth.
 *  Called by CapturePersistence::deserialize immediately after CRS publish;
 *  does not re-run F0 analysis — uses the already-restored pitch curve. */
using EnqueuePartialRenderFn = std::function<void(ContentKey, double startSeconds, double endSeconds)>;

/** Bundle of processor-side callbacks injected at CaptureSession construction. */
struct ProcessorBindings
{
    ReplaceWithRenderedFn replaceWithRendered;
    RetireSegmentFn retireSegment;
    RefreshSegmentFn refreshSegment;
    PublishPlaybackSourceFn publishPlaybackSource;
    EnqueuePartialRenderFn enqueuePartialRender;
};

/**
 * Capture session orchestrator.
 *
 * Created once per VST3 plugin instance. Implements Melodyne Transfer-style
 * non-ARA capture workflow: dry pass-through by default, record on demand,
 * replay rendered audio within recorded segments' time ranges.
 *
 * Thread model:
 *   - Audio thread calls processBlock() noexcept; never allocates, never locks.
 *   - Message thread calls everything else.
 *   - Communication via std::atomic<shared_ptr<const SegmentsView>> publish-subscribe.
 */
class CapturePersistence;  // friend for serialize/deserialize internal access

class CaptureSession
{
public:
    explicit CaptureSession(ProcessorBindings bindings);
    ~CaptureSession();

    friend class CapturePersistence;

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    // ─── Lifecycle (message thread) ────────────────────────────────────────
    /**
     * Configure the session for the host's current bus layout.
     * @param sampleRate         Host audio sample rate.
     * @param maxBlockSize       Maximum samples per processBlock.
     * @param hostInputChannels  Declared host input bus channel count
     *                           (`AudioProcessor::getMainBusNumInputChannels()`).
     *                           Clamped internally to {1, 2}; this is the SOLE
     *                           determinant of capture layout per
     *                           channel-layout-policy spec.
     */
    void prepareToPlay(double sampleRate, int maxBlockSize, int hostInputChannels);
    void releaseResources();

    /** Audio-thread / GUI-thread read of the current capture channel count. */
    int getCaptureChannels() const noexcept { return captureChannels_.load(std::memory_order_acquire); }

    // ─── User actions (message thread) ─────────────────────────────────────
    /** Start a new capture. Returns false if a Capturing or Processing segment already exists. */
    bool armNewCapture();

    /** Stop the active capture (no-op if none Capturing). */
    void stopCapture();

    // ─── Audio thread ──────────────────────────────────────────────────────
    /**
     * Process one audio block. Behavior depends on current segments:
     *   - If any Edited segment covers host_t  → buffer.clear() + write rendered audio.
     *   - Else                                  → leave buffer untouched (dry pass-through).
     *   - If a Capturing segment exists and host_t advances continuously → write dry copy to its fifo.
     */
    void processBlock(juce::AudioBuffer<float>& buffer,
                      double hostTimeSeconds,
                      double hostSampleRate,
                      bool isPlaying) noexcept;

    /** Periodic message-thread tick (~30 Hz from PluginEditor timer). Promotes Pending -> Processing
     *  after capture drain, and Processing -> Edited when CaptureSegmentContent F0 state is Ready. */
    void tick();

    // ─── Notification injection (message thread) ───────────────────────────
    void setActiveSegmentChangedCallback(ActiveSegmentChangedFn fn);

    /** Called by render pipeline when a segment's content is ready. */
    void onSegmentRenderingComplete(ContentKey segmentContentKey);

    /** Commit F0 extraction result to segment content. Does not promote lifecycle. */
    bool commitSegmentF0Result(ContentKey segmentContentKey,
                               std::shared_ptr<PitchCurve> pitchCurve,
                               OriginalF0State state,
                               const DetectedKey& detectedKey);

    // ─── Query (any thread) ────────────────────────────────────────────────
    SessionState getGlobalState() const noexcept;
    double getCurrentlyCapturedSeconds() const noexcept;
    size_t getTotalCapturedBytes() const noexcept;

    /** Find segment by ContentKey. Returns nullptr if not found. Thread-safe. */
    CaptureSegment* findSegmentByContentKey(const ContentKey& key) const;

    /** GUI snapshot (message thread). */
    std::vector<SegmentInfo> listSegments() const;
    std::vector<SegmentInfo> listEditedSegments() const;
    bool resolveDisplaySegment(double hostTimeSeconds, SegmentInfo& out) const;

    // ─── Persistence (message thread) ──────────────────────────────────────
    juce::MemoryBlock serialize() const;
    bool deserialize(const juce::MemoryBlock& block);

    // ─── Test hooks ────────────────────────────────────────────────────────
    /** Test only: inject a fully-formed Edited segment for unit tests. */
    uint64_t testInjectEditedSegment(double T_start,
                                     double durationSeconds,
                                     uint64_t segmentId,
                                     std::shared_ptr<juce::AudioBuffer<float>> pcm);

    /** Test only: inject a Processing segment with owner audio for lifecycle tests. */
    uint64_t testInjectProcessingSegment(double T_start,
                                         double durationSeconds,
                                         uint64_t segmentId,
                                         std::shared_ptr<juce::AudioBuffer<float>> pcm,
                                         double sampleRate);

    /** Test only: snapshot internal mutable segments (read-only). */
    const std::vector<std::unique_ptr<CaptureSegment>>& testSegments() const { return mutableSegments_; }

    /** Test only: apply the per-sample numerical-safety pass (NaN/Inf → 0) used in
     *  the audio thread. Lets unit tests verify numeric defense in isolation. */
    static void applyNumericGuardForTest(juce::AudioBuffer<float>& buffer) noexcept;

private:
    /** Build a new immutable view from current mutableSegments_ and atomic-store it. */
    void publishSegmentsView();

    /** Park a removed segment until old published views have aged out. mutableMutex_ must be held. */
    void queueForReclaimLocked(std::unique_ptr<CaptureSegment> segment);

    /** Drain a Pending capture after the audio-thread grace window and submit it for rendering. */
    bool finalizePendingCapture(CaptureSegment& pending);

    /** Compaction: remove older segments fully covered by 'newlyEdited'. */
    void runCompaction(const CaptureSegment& newlyEdited);

    /** Allocate the next monotonic id. */
    uint64_t nextId() noexcept;

    ProcessorBindings bindings_;
    ActiveSegmentChangedFn activeSegmentChanged_;

    // Configured at prepareToPlay
    double currentSampleRate_ = 44100.0;
    int currentMaxBlockSize_ = 0;

    // Mutable owners (message thread).
    mutable std::mutex mutableMutex_;  // guards mutableSegments_, activeDisplaySegmentId_ + idCounter_
    std::vector<std::unique_ptr<CaptureSegment>> mutableSegments_;
    uint64_t activeDisplaySegmentId_ = 0;
    uint64_t idCounter_ = 0;

    // Published view for audio thread (lock-free read).
    // C++17 idiom matching project style (PitchCurve.cpp): plain shared_ptr field accessed
    // via free std::atomic_load/store. C++20's std::atomic<std::shared_ptr<T>> would be
    // cleaner but the project targets C++17.
    std::shared_ptr<const SegmentsView> publishedSegments_;

    struct ReclaimEntry
    {
        std::unique_ptr<CaptureSegment> segment;
        int queuedTick = 0;
    };

    // Reclaim sweep: segments removed from mutable but still possibly visible to audio thread.
    std::vector<ReclaimEntry> pendingReclaim_;
    std::atomic<int> tickCounter_ { 0 };

    // Audio-thread scratch (allocated in prepareToPlay; resized only on message thread).
    juce::AudioBuffer<float> dryScratch_;

    // Audio-thread only: tracks the previous block's host time. Used to detect "transport
    // is running" by observing host_t advance, even when the host reports isPlaying=false
    // (FL Studio / some Reaper configs do that). Sentinel <0 means "no prior block seen".
    double lastSeenHostTime_ { -1.0 };

    // Single source of truth for the capture channel layout. Set by prepareToPlay
    // from the host's declared input bus, snapshotted into each CaptureSegment at
    // arm time. Always 1 or 2 — see channel-layout-policy spec.
    std::atomic<int> captureChannels_ { 1 };
};

}  // namespace OpenTune::Capture
