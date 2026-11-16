#pragma once


#include "CaptureRingBuffer.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "../../Content/ContentKey.h"
#include "../../Content/CaptureSegmentContent.h"
#include "../../Utils/ContentAnalysisState.h"

namespace OpenTune::Capture {

/**
 * Per-segment lifecycle state.
 *
 * Capturing  : audio thread is writing dry samples into the take's host window
 * Pending    : capture stopped, waiting for the message thread to finalize
 * Processing : submitted; F0/Vocoder rendering in progress
 * Edited     : rendered; eligible for replacement playback
 * Failed     : render or F0 analysis failed; content preserved, not blocking next capture
 */
enum class SegmentState : int
{
    Capturing = 0,
    Pending,
    Processing,
    Edited,
    Failed
};

/**
 * Pre-allocated metadata for one captured host range within a CaptureSegment.
 * Spans are appended in write order; host ranges may overlap. Finalize sorts
 * and merges them on the message thread. sampleCount only grows; it is
 * extended only when the host position continues from the previous span's tail.
 */
struct CapturedSpan
{
    int64_t hostStartSample = 0;  // absolute host sample at span start
    int32_t sampleCount = 0;      // number of samples written for this span
};

/**
 * One captured take, explicitly recorded into a host-addressed window
 * (anchorHostSample + capacitySamples, indexed with floorMod). Owned by
 * CaptureSession's mutableSegments_ and by the published audio-thread views.
 *
 * stateWord is a single atomic word: bits 0-3 hold the SegmentState, bit 4
 * holds the writer-active claim. The audio thread holds the writer bit while
 * it writes; the message thread does not finalize while the writer bit is set.
 * A segment object stays alive until the last published view that references
 * it is released.
 *
 * Audio thread writes (holding writerActive):
 *   - window PCM at host-addressed offsets
 *   - spans / numSpans, observedPeak
 *   - minHostSample, maxHostSampleExclusive, anchorHostSample, hasWritten
 *   - windowExhausted / spanBudgetExhausted latches
 *
 * Message thread writes everything else (state transitions, T_start/durationSeconds,
 * hostStartSample/hostSampleCount at finalize).
 *
 * Lifetime: a segment stays alive while any published view still references it;
 * reclaim happens after all audio readers leave the retired views.
 */
struct CaptureSegment
{
    /** Domain content key for this capture segment. Set at creation time. Serves as unique identity. */
    ContentKey contentKey;

    /** Strict creation order; equals contentKey.objectId. Newer = larger. */
    uint64_t creationOrder = 0;

    /** sampleRate at the time this segment was armed (window sizing + finalize math). */
    double captureSampleRate = 0.0;

    /** Number of channels captured. Snapshotted from CaptureSession::captureChannels_
     *  at arm time and immutable for the segment's lifetime. Always 1 or 2. */
    int captureChannels = 1;

    /** Host window capacity in samples (= 600 s × captureSampleRate, set at arm time). */
    int capacitySamples = 0;

    /** Latched when the user asks to stop while the audio thread holds the writer bit.
     *  tick() retries the Capturing -> Pending transition and clears it on success. */
    std::atomic<bool> stopRequested { false };

    /** Latched by the audio thread when a discontinuity needs a new span but the span
     *  metadata budget is spent. The whole unfinished take is discarded at finalize. */
    std::atomic<bool> spanBudgetExhausted { false };

    /** Latched by the audio thread when the take's host span would exceed capacitySamples.
     *  tick() ends the take and finalize publishes the content written so far. */
    std::atomic<bool> windowExhausted { false };

    /** Authoritative absolute host sample range. Set at finalize (message thread).
     *  Audio thread reads for playback replacement. */
    std::atomic<int64_t> hostStartSample { 0 };
    std::atomic<int64_t> hostSampleCount { 0 };

    /** UI/persistence presentation fields. Computed from hostStartSample/hostSampleCount
     *  at finalize time (message thread). NOT used by audio thread for positioning. */
    std::atomic<double> T_start { 0.0 };
    double durationSeconds = 0.0;

    /** Lifecycle state + writer-active flag packed into one atomic word.
     *  bits 0-3 = SegmentState, bit 4 = writerActive. Audio thread reads,
     *  message thread writes the state field; the writer bit is a CAS handshake. */
    std::atomic<uint32_t> stateWord { static_cast<uint32_t>(SegmentState::Capturing) };

    static constexpr uint32_t kStateMask = 0xFu;
    static constexpr uint32_t kWriterBit = 0x10u;

    SegmentState getState() const noexcept
    {
        return static_cast<SegmentState>(stateWord.load(std::memory_order_acquire) & kStateMask);
    }

    /** Message-thread state transition. Preserves the writer bit via CAS loop. */
    void setState(SegmentState s) noexcept
    {
        uint32_t cur = stateWord.load(std::memory_order_acquire);
        for (;;) {
            const uint32_t desired = (cur & ~kStateMask) | static_cast<uint32_t>(s);
            if (stateWord.compare_exchange_weak(cur, desired,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire))
                return;
        }
    }

    bool isWriterActive() const noexcept
    {
        return (stateWord.load(std::memory_order_seq_cst) & kWriterBit) != 0;
    }

    /** Audio thread: claim exclusive write access, only while Capturing and no writer. */
    bool tryAcquireWriter() noexcept
    {
        uint32_t cur = static_cast<uint32_t>(SegmentState::Capturing);
        return stateWord.compare_exchange_strong(cur, cur | kWriterBit, std::memory_order_seq_cst);
    }

    /** Audio thread: release the write claim. */
    void releaseWriter() noexcept
    {
        stateWord.fetch_and(~kWriterBit, std::memory_order_seq_cst);
    }

    /** Message thread: Capturing → Pending. Fails while a writer holds the bit. */
    bool tryStopCapturing() noexcept
    {
        uint32_t cur = static_cast<uint32_t>(SegmentState::Capturing);
        return stateWord.compare_exchange_strong(cur, static_cast<uint32_t>(SegmentState::Pending),
                                                 std::memory_order_seq_cst);
    }

    /** Number of populated span entries in spans[]. Audio thread writes (monotonically
     *  increasing), message thread reads at finalize. Bounded by spans.size(). */
    std::atomic<int> numSpans { 0 };

    /** Pre-allocated span metadata array. Capacity is bounded by a fixed per-take
     *  budget; the audio thread never grows it. */
    std::vector<CapturedSpan> spans;

    /** Audio-thread single-writer window extent. Message thread reads after the
     *  writer released its claim. */
    int64_t minHostSample = 0;
    int64_t maxHostSampleExclusive = 0;
    int64_t anchorHostSample = 0;  // first successfully written host sample
    bool hasWritten = false;

    /** Message-thread only: 上次 tick 观察到的 F0 可用状态。tick() 仅在跃迁
     *  （无可用 F0 → 可用 F0，含首次观察即可用）时提交一次 requestFullRender，
     *  render gate 以 owner 实际可用曲线数据为准，观察基线归一化为 Ready。
     *  避免渲染窗口内（渲染耗时 > 33ms，大于 30Hz tick 间隔）重复提交取消并重启
     *  Running chunk。与插件 UI 侧 lastObservedOriginalF0States_ 同为
     *  状态记录+跃迁检测模式；初始 NotRequested 保证首次观察即可用不丢失。 */
    OriginalF0State lastObservedF0State = OriginalF0State::NotRequested;

    /** Diagnostic: peak absolute sample value seen during capture. Audio thread writes,
     *  message thread reads in finalize log. Helps distinguish "host sends silence"
     *  from "window write path broken". */
    std::atomic<float> observedPeak { 0.0f };

    /** Host-addressed PCM window: audio writes, message reads at finalize. */
    CaptureRingBuffer window;

    /** Content owner for this segment. */
    std::unique_ptr<OpenTune::CaptureSegmentContent> content;

    /** Window offset of an absolute host sample inside this take's window. */
    int windowOffset(int64_t hostSample) const noexcept
    {
        const int64_t capacity = static_cast<int64_t>(capacitySamples);
        int64_t offset = (hostSample - anchorHostSample) % capacity;
        if (offset < 0)
            offset += capacity;
        return static_cast<int>(offset);
    }

    /** Test if host_t is in [T_start, T_start + duration). UI/display path only. */
    bool containsTime(double host_t) const noexcept
    {
        const double start = T_start.load(std::memory_order_acquire);
        return host_t >= start && host_t < start + durationSeconds;
    }
};

}  // namespace OpenTune::Capture
