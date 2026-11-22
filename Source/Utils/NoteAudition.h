#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace OpenTune {

/**
 * Note audition: when moving between notes with the arrows, plays that note's vocal
 * fragment, as in Melodyne. The host transport is not touched: the audio is
 * mixed into the plugin output just like the piano key audition.
 *
 * play/stop are called on the message thread, mixIntoBuffer on the audio thread.
 * PCM moves through three fixed slots; only the message thread constructs or
 * destroys it, while the audio thread transfers slot ownership atomically.
 */
class NoteAudition {
public:
    NoteAudition() = default;

    /// Play numSamples of source from startSample (replaces whatever is playing).
    void play(const juce::AudioBuffer<float>& source, double sourceSampleRate,
              int startSample, int numSamples);
    void stop();

    void mixIntoBuffer(juce::AudioBuffer<float>& output, int numSamples, double outputSampleRate);

    static constexpr double kMaxDurationSeconds = 4.0;

private:
    enum class SlotState : uint8_t { Free, Building, Pending, Playing, Returned };

    struct Clip {
        juce::AudioBuffer<float> mono;
        double sampleRate = 44100.0;
    };

    struct Slot {
        Clip clip;
        std::atomic<uint8_t> state{static_cast<uint8_t>(SlotState::Free)};
        uint64_t generation = 0;
    };

    int claimBuildSlot() noexcept;
    void returnSlot(int slotIndex) noexcept;
    void reclaimReturnedSlots() noexcept;

    static constexpr int kSlotCount = 3;
    std::array<Slot, kSlotCount> slots_{};
    std::atomic<uint64_t> controlGeneration_{0};
    std::atomic<uint64_t> stopGeneration_{0};
    uint64_t appliedStopGeneration_ = 0;

    // Audio thread only.
    int playingSlot_ = -1;
    double position_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteAudition)
};

static_assert(std::atomic<uint8_t>::is_always_lock_free,
              "NoteAudition slot state must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "NoteAudition generations must be lock-free");

} // namespace OpenTune
