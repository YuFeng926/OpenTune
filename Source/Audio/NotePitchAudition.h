#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include "../Render/PlaybackReadSource.h"

namespace OpenTune {

/// Rendered audition snippet: pitch-shifted mono preview at a fixed sample rate.
struct NotePitchPreview {
    std::vector<float> mono;
    double sampleRate = 0.0;
};

/// Render one output-side note pitch audition snippet (no cache writes, no disk).
///
/// Reads channel 0 of @p source between @p startSeconds and @p endSeconds
/// (source/content seconds at @p sourceSampleRate). @p originalF0 with
/// @p numOriginalF0Frames at @p originalF0FrameRate is the complete source F0
/// track; its time origin is zero, matching @p source. The preview range is
/// shifted to a constant @p targetHz by PhaseVocoderPitchShifter on the native
/// source rate, while all frames outside the range keep their original F0 as
/// neutral phase context. The result is resampled to @p outputSampleRate.
/// The returned preview is at least 0.3 s long (or the note duration, whichever
/// is longer) and clamped to the source buffer duration. Returns an empty
/// preview on invalid input or when every frame in the preview range is
/// unvoiced.
NotePitchPreview renderNotePitchPreview(
    const juce::AudioBuffer<float>& source,
    double sourceSampleRate,
    double startSeconds,
    double endSeconds,
    float targetHz,
    double outputSampleRate,
    const float* originalF0,
    int numOriginalF0Frames,
    double originalF0FrameRate);

/**
 * NotePitchAudition - output-side, non-persistent note transposition audition.
 *
 * A background thread renders audition snippets from an immutable
 * PlaybackReadSource snapshot (copied at request time; no
 * ContentRenderService/DocumentController pointers reach the worker, no
 * RenderCache/TimeStretchCache writes, no disk I/O). One pending request slot:
 * latest wins, no queue. The thread starts lazily on the first request; the
 * destructor signals stop and joins.
 *
 * Threading: request()/stop() are called from the message thread (or any
 * producer thread); mixIntoBuffer() is called from the audio thread. Published
 * snippets cross threads via C++17 std::atomic_load/std::atomic_store free
 * functions on std::shared_ptr.
 * Rate changes are handled externally by calling stop().
 */
class NotePitchAudition : private juce::Thread
{
public:
    NotePitchAudition();
    ~NotePitchAudition() override;

    /// Request an audition render (producer thread). Latest request wins.
    void request(PlaybackReadSource source,
                 double startSeconds,
                 double endSeconds,
                 float targetHz,
                 double outputSampleRate);

    /// Stop auditioning: drop pending work and fade all voices out.
    void stop();

    /// Audio thread: mix audition voices into all channels of @p output.
    void mixIntoBuffer(juce::AudioBuffer<float>& output,
                       int numSamples,
                       double outputSampleRate) noexcept;

private:
    /// Published snippet payload (audio thread reads it lock-free via
    /// shared_ptr; the producer prunes retired copies so the audio thread
    /// never frees the last reference).
    struct Snippet {
        std::vector<float> mono;
        double sampleRate = 0.0;
    };

    struct Request {
        PlaybackReadSource source;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        float targetHz = 0.0f;
        double outputSampleRate = 0.0;
    };

    void run() override;

    void pruneRetired();

    juce::WaitableEvent wakeup_;
    std::mutex requestMutex_;
    Request pending_;
    bool hasPending_ = false;

    /// Set by stop(); the producer drops results when it observes it.
    std::atomic<bool> stopRequested_{false};

    // Audio-thread state -----------------------------------------------------
    struct Voice {
        std::shared_ptr<const Snippet> snippet;
        double position = 0.0;
        float gain = 0.0f;
        float targetGain = 0.0f;
    };

    static constexpr int kNumVoices = 2;
    static constexpr double kVoiceFadeSeconds = 0.01;

    /// Producer copies previously published snippets into retired_ so the
    /// audio thread never frees the last reference.
    std::vector<std::shared_ptr<const Snippet>> retired_;
    std::atomic<bool> fadeOutAll_{false};
    /// C++17 std::atomic_load/std::atomic_store free functions on shared_ptr.
    /// Audio thread reads via std::atomic_load; the producer publishes via
    /// std::atomic_store.
    std::shared_ptr<const Snippet> published_;
    /// Audio-thread-only copy of the last published snippet this player has
    /// consumed (started or explicitly rejected). Released only on the audio
    /// thread.
    std::shared_ptr<const Snippet> seenSnippet_;
    Voice voices_[kNumVoices]{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NotePitchAudition)
};

} // namespace OpenTune
