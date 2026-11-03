#pragma once


#include "CaptureSegment.h"
#include "../../Content/ContentKey.h"
#include "../../Content/EditableContentSnapshot.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace OpenTune::Capture {

/** Aggregated session state derived from segment states.
 *  Failed segments are intentionally ignored — they do not block next capture. */
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
 * Audio-thread-callable: replace a sub-range of buffer with rendered audio
 * from the given segment. Implementation:
 *   - clear [destStart, destStart+numSamples)
 *   - call contentRenderService->getPlaybackReadSource(segment content key, ...)
 *
 * readStartSample is the absolute relative sample offset into the segment's
 * rendered content (in targetSampleRate units). CaptureSession computes the
 * block/segment overlap and passes only the intersecting sub-range.
 */
using ReplaceWithRenderedFn = std::function<void(juce::AudioBuffer<float>& buffer,
                                                  int destStart,
                                                  int numSamples,
                                                  ContentKey segmentContentKey,
                                                  int64_t readStartSample,
                                                  double targetSampleRate)>;

/** Message-thread compaction sink: tell the processor to retire segment content. */
using RetireSegmentFn = std::function<void(ContentKey segmentContentKey)>;

/** Message-thread re-render trigger for an existing segment content (no new clip created).
 *  Used by CapturePersistence::deserialize to repopulate RenderCache after restore — the
 *  vocoder output is not in standard state, so it must be re-synthesized from the restored
 *  audio + pitchCurve. tick() promotes Processing → Edited via F0 state check. */
using RefreshSegmentFn = std::function<void(ContentKey segmentContentKey)>;

/** Publish PlaybackReadSource to ContentRenderService with segment.id as ContentKey. */
using PublishPlaybackSourceFn = std::function<void(const ContentKey& key,
                                                     std::shared_ptr<const EditableContentSnapshot> snapshot,
                                                     std::shared_ptr<const juce::AudioBuffer<float>> audio,
                                                     double sampleRate)>;

/** Enqueue a full-content render to rebuild render cache from restored owner truth.
 *  Called by CapturePersistence::deserialize immediately after CRS publish;
 *  does not re-run F0 analysis — uses the already-restored pitch curve. */
using RequestFullRenderFn = std::function<void(ContentKey)>;

/** Bundle of processor-side callbacks injected at CaptureSession construction. */
struct ProcessorBindings
{
    ReplaceWithRenderedFn replaceWithRendered;
    RetireSegmentFn retireSegment;
    RefreshSegmentFn refreshSegment;
    PublishPlaybackSourceFn publishPlaybackSource;
    RequestFullRenderFn requestFullRender;
};

/**
 * Capture session orchestrator.
 *
 * Created once per VST3 plugin instance. Recording is explicit: the plugin record
 * key starts a take, and the same key (or host transport stop) ends it.
 *
 * Thread model:
 *   - Audio thread calls processBlock() noexcept; never allocates, never locks.
 *   - Message thread calls everything else.
 *   - Communication via an atomically published SegmentsView. Audio readers are
 *     counted so retired views are destroyed only after the audio thread leaves.
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

    // ─── User actions (message thread) ─────────────────────────────────────
    /** Start a new take. Returns false if a Capturing or Pending segment already exists. */
    bool armNewCapture();

    /** End the active take (no-op if none Capturing). Finalize happens in tick(). */
    void stopCapture();

    // ─── Audio thread ──────────────────────────────────────────────────────
    /**
     * Process one audio block.
     *   - If any Edited segment covers hostAbsoluteSample → replace overlapping sub-range.
     *   - If a Capturing segment exists and isPlaying → write the block into its host window.
     *   - hostAbsoluteSample is the sole source of truth for audio positioning.
     *   - hostAbsoluteSample < 0 → early return (both replacement and capture skipped).
     */
    void processBlock(juce::AudioBuffer<float>& buffer,
                      int64_t hostAbsoluteSample,
                      double hostSampleRate,
                      bool isPlaying) noexcept;

    /** Periodic message-thread tick (~30 Hz from the processor timer). Ends takes on host
     *  stop or window exhaustion, finalizes Pending captures, promotes Processing -> Edited
     *  when usable OriginalF0 exists, and reclaims retired segments. */
    void tick();

    // ─── Notification injection (message thread) ───────────────────────────

    /** Called by render pipeline when CRS render cache is complete and ready for playback.
     *  Transitions segment from Processing to Edited state. */
    void onRenderComplete(ContentKey segmentContentKey);
    /** Called by the render pipeline when the current chunk fails. */
    void onRenderFailed(ContentKey segmentContentKey, const juce::String& reason);

    /** Commit F0 extraction result to segment content. When state is Failed,
     *  transitions segment lifecycle to Failed (content preserved, not deleted). */
    bool commitSegmentF0Result(ContentKey segmentContentKey,
                               std::shared_ptr<PitchCurve> pitchCurve,
                               OriginalF0State state,
                               std::string failureReason = {});
    bool applyAutoTuneGeneratedNotes(ContentKey segmentContentKey,
                                     std::vector<Note> notes,
                                     std::shared_ptr<PitchCurve> pitchCurve);
    bool applyNotes(ContentKey segmentContentKey, std::vector<Note> notes);
    // Volume envelope 编辑不触发神经渲染。
    bool applyVolumeEnvelope(ContentKey segmentContentKey, AutomationLane envelope);
    bool applyNotesAndPitchCurve(ContentKey segmentContentKey,
                                 std::vector<Note> notes,
                                 std::shared_ptr<PitchCurve> pitchCurve);
    bool applyPitchCurve(ContentKey segmentContentKey, std::shared_ptr<PitchCurve> pitchCurve);
    bool applyTimeGrid(ContentKey segmentContentKey, std::shared_ptr<const TimeGridSnapshot> grid);
    bool applyDetectedKey(ContentKey segmentContentKey, const DetectedKey& detectedKey);
    bool applyPitchShiftState(ContentKey segmentContentKey, const PitchShiftEditState& state);

    /** Combined external-edit return commit (message thread). Applies notes, pitch
     *  curve (via applyOriginalF0 when usable, else applyPitchCurve), pitch-shift
     *  state (settings + notes + curve correction segments) and volume envelope to
     *  one segment content in a single mutableMutex_ critical section. No publish,
     *  no partial state on early exit: the segment/content lookup happens before
     *  any mutation. Returns false if the key has no valid Capture segment/content. */
    bool applyExternalEdit(ContentKey segmentContentKey,
                           std::vector<Note> notes,
                           std::shared_ptr<PitchCurve> pitchCurve,
                           AutomationLane envelope,
                           PitchShiftSettings pitchShift);

    // ─── Query (message thread) ────────────────────────────────────────────
    SessionState getGlobalState() const noexcept;

    /** Find segment by ContentKey. Message thread only; the returned pointer's
     *  lifetime is owned by the session and is valid until the segment is removed
     *  from mutableSegments_. */
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
                                     std::shared_ptr<juce::AudioBuffer<float>> pcm,
                                     double sampleRate);

    /** Test only: inject a Processing segment with owner audio for lifecycle tests. */
    uint64_t testInjectProcessingSegment(double T_start,
                                         double durationSeconds,
                                         uint64_t segmentId,
                                         std::shared_ptr<juce::AudioBuffer<float>> pcm,
                                         double sampleRate);

    /** Test only: inject a fully-formed segment with explicit ContentState and state. */
    uint64_t testInjectSegment(OpenTune::ContentState state,
                               SegmentState segmentState,
                               double T_start,
                               double durationSeconds,
                               uint64_t segmentId,
                               double sampleRate);

    /** Test only: snapshot internal mutable segments (read-only, message thread). */
    const std::vector<std::shared_ptr<CaptureSegment>>& testSegments() const { return mutableSegments_; }

    /** Test only: lock-protected (state, writerActive) snapshot for concurrency tests. */
    std::vector<std::pair<SegmentState, bool>> testStateSnapshot() const;

    /** Test only: number of view publications since construction. */
    uint64_t testPublishedViewCount() const noexcept { return publishedViewCount_; }

    /** Test only: override the per-take span metadata capacity for the next armed
     *  takes (0 = production budget). Lets tests exercise span-budget exhaustion
     *  with a tiny capacity and observe the full reclaim lifecycle. */
    void testSetSpanCapacityOverride(int capacity) noexcept
    {
        testSpanCapacityOverride_.store(capacity, std::memory_order_release);
    }

private:
    /** Build a new immutable view from current mutableSegments_ and atomically
     *  exchange it into publishedSegments_. mutableMutex_ must NOT be held; the
     *  method locks it internally while constructing the new view. */
    void publishSegmentsView();

    /** Finalize a Pending capture: merge spans, read the host window, publish segments. */
    bool finalizePendingCapture(CaptureSegment& pending);

    /** Trim/split pre-existing segments overlapped by the new segment host ranges N.
     *  newKeys are the keys published by this finalize; they are never trimmed.
     *  Message thread only; called at the end of a successful finalizePendingCapture. */
    void trimOverlappedSegments(const std::vector<std::pair<int64_t, int64_t>>& newRanges,
                                const std::vector<ContentKey>& newKeys);

    /** Allocate the next monotonic id. */
    uint64_t nextId() noexcept;

    ProcessorBindings bindings_;

    // Configured at prepareToPlay
    double currentSampleRate_ = 0.0;
    int currentMaxBlockSize_ = 0;

    // Mutable owners (message thread).
    mutable std::mutex mutableMutex_;  // guards mutableSegments_, activeDisplaySegmentId_ + idCounter_
    std::vector<std::shared_ptr<CaptureSegment>> mutableSegments_;
    uint64_t activeDisplaySegmentId_ = 0;
    uint64_t idCounter_ = 0;

    // Published view for audio thread (lock-free read of raw pointers only).
    // The audio thread never touches owners or shared_ptr refcounts.
    std::atomic<const SegmentsView*> publishedSegments_ { nullptr };

    // Message-thread side of the publication lifecycle. currentView_ is the view
    // currently published (keeps its owners alive); retiredViews_ holds views that
    // were replaced but may still be read by the audio thread until readers hit 0.
    std::unique_ptr<const SegmentsView> currentView_;
    std::vector<std::unique_ptr<const SegmentsView>> retiredViews_;

    // Content keys removed from the published set but not yet retired with the
    // processor. Keys reappearing in a later view are dropped from this list.
    std::vector<ContentKey> pendingRetireKeys_;
    std::vector<ContentKey> publishedKeys_;

    mutable std::atomic<int> activeSegmentReaders_ { 0 };
    uint64_t publishedViewCount_ = 0;

    // Audio-thread scratch (allocated in prepareToPlay; resized only on message thread).
    juce::AudioBuffer<float> dryScratch_;

    // Single source of truth for the capture channel layout. Set by prepareToPlay
    // from the host's declared input bus, snapshotted into each CaptureSegment at
    // arm time. Always 1 or 2 — see channel-layout-policy spec.
    std::atomic<int> captureChannels_ { 1 };

    // Test-only span capacity override for armed takes (0 = production budget).
    std::atomic<int> testSpanCapacityOverride_ { 0 };
    // Last host transport playing flag published by the audio thread. tick() uses it
    // to end a take that holds content once the host stops.
    std::atomic<bool> lastHostPlaying_ { false };
};

}  // namespace OpenTune::Capture
