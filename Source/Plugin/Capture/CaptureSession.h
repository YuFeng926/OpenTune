#pragma once


#include "CaptureSegment.h"

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
    uint64_t materializationId = 0;
};

/**
 * Submit a captured take to the existing render pipeline.
 *
 * Implementation MUST:
 *   - call processor.prepareImport(std::move(buffer), sampleRate, name, prepared)
 *   - call processor.commitPreparedImportAsMaterialization(std::move(prepared))
 *   - call processor.requestMaterializationRefresh({materializationId})
 *
 * Returns the materializationId (non-zero on success, 0 on failure).
 */
using SubmitForRenderFn = std::function<uint64_t(std::shared_ptr<juce::AudioBuffer<float>> pcm,
                                                 double sampleRate,
                                                 juce::String displayName)>;

/**
 * Audio-thread-callable: replace [destStart, destStart+numSamples) of buffer with rendered audio
 * from the given materialization. Implementation MUST:
 *   - clear destination range
 *   - call processor.readPlaybackAudio({materializationId, readStartSeconds, targetSampleRate}, ...)
 *     with the per-materialization CrossoverMixer so LR4 path is preserved.
 *
 * Captures a small (one-pointer) lambda; no heap allocation expected when called.
 */
using ReplaceWithRenderedFn = std::function<void(juce::AudioBuffer<float>& buffer,
                                                  int destStart,
                                                  int numSamples,
                                                  uint64_t materializationId,
                                                  double readStartSeconds,
                                                  double targetSampleRate)>;

/** Message-thread poll: returns true when the materialization is ready for playback. */
using IsRenderReadyFn = std::function<bool(uint64_t materializationId)>;

/** Message-thread poll: returns true when render pipeline has marked the materialization as failed
 *  (e.g. F0 extraction returned empty/unvoiced). Capture session uses this to drop dead segments
 *  so the user can record again rather than getting stuck in Processing. */
using IsRenderFailedFn = std::function<bool(uint64_t materializationId)>;

/** Message-thread compaction sink: tell the processor to retire a materialization. */
using RetireMaterializationFn = std::function<void(uint64_t materializationId)>;

/** Notification sink for UI: a segment has reached Edited state. */
using ActiveSegmentChangedFn = std::function<void(uint64_t materializationId)>;

/** Message-thread orphan check: returns true when MaterializationStore knows about a mat id.
 *  Used by CapturePersistence::deserialize to skip segments referencing missing mats. */
using ContainsMaterializationFn = std::function<bool(uint64_t materializationId)>;

/** Message-thread re-render trigger for an existing materialization (no new mat created).
 *  Used by CapturePersistence::deserialize to repopulate RenderCache after restore — the
 *  vocoder output is not in standard state, so it must be re-synthesized from the restored
 *  audio + pitchCurve. tick() promotes Processing → Edited via isRenderReady. */
using RefreshMaterializationFn = std::function<void(uint64_t materializationId)>;

/** Bundle of processor-side callbacks injected at CaptureSession construction. */
struct ProcessorBindings
{
    SubmitForRenderFn submitForRender;
    ReplaceWithRenderedFn replaceWithRendered;
    IsRenderReadyFn isRenderReady;
    IsRenderFailedFn isRenderFailed;
    RetireMaterializationFn retireMaterialization;
    ContainsMaterializationFn containsMaterialization;
    RefreshMaterializationFn refreshMaterialization;
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
     *   - If a Capturing segment exists and isPlaying → write dry copy to its fifo.
     */
    void processBlock(juce::AudioBuffer<float>& buffer,
                      double hostTimeSeconds,
                      double hostSampleRate,
                      bool isPlaying) noexcept;

    /** Periodic message-thread tick (~30 Hz from PluginEditor timer). Promotes Pending → Processing
     *  when audio thread has signaled stopRequested, and detects Processing → Edited via callback. */
    void tick();

    // ─── Notification injection (message thread) ───────────────────────────
    void setActiveSegmentChangedCallback(ActiveSegmentChangedFn fn);

    /** Called by render pipeline when a segment's materialization is ready. */
    void onSegmentRenderingComplete(uint64_t segmentId, uint64_t materializationId);

    // ─── Query (any thread) ────────────────────────────────────────────────
    SessionState getGlobalState() const noexcept;
    double getCurrentlyCapturedSeconds() const noexcept;
    size_t getTotalCapturedBytes() const noexcept;

    /** GUI snapshot (message thread). */
    std::vector<SegmentInfo> listSegments() const;

    // ─── Persistence (message thread) ──────────────────────────────────────
    juce::MemoryBlock serialize() const;
    bool deserialize(const juce::MemoryBlock& block);

    // ─── Test hooks ────────────────────────────────────────────────────────
    /** Test only: inject a fully-formed Edited segment for unit tests. */
    uint64_t testInjectEditedSegment(double T_start,
                                     double durationSeconds,
                                     uint64_t materializationId,
                                     std::shared_ptr<juce::AudioBuffer<float>> pcm);

    /** Test only: snapshot internal mutable segments (read-only). */
    const std::vector<std::unique_ptr<CaptureSegment>>& testSegments() const { return mutableSegments_; }

    /** Test only: apply the per-sample numerical-safety pass (NaN/Inf → 0) used in
     *  the audio thread. Lets unit tests verify numeric defense in isolation. */
    static void applyNumericGuardForTest(juce::AudioBuffer<float>& buffer) noexcept;

private:
    /** Build a new immutable view from current mutableSegments_ and atomic-store it. */
    void publishSegmentsView();

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
    mutable std::mutex mutableMutex_;  // guards mutableSegments_ + nextId
    std::vector<std::unique_ptr<CaptureSegment>> mutableSegments_;
    uint64_t idCounter_ = 0;

    // Published view for audio thread (lock-free read).
    // C++17 idiom matching project style (PitchCurve.cpp): plain shared_ptr field accessed
    // via free std::atomic_load/store. C++20's std::atomic<std::shared_ptr<T>> would be
    // cleaner but the project targets C++17.
    std::shared_ptr<const SegmentsView> publishedSegments_;

    // Reclaim sweep: segments removed from mutable but still possibly visible to audio thread.
    std::vector<std::unique_ptr<CaptureSegment>> pendingReclaim_;
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

