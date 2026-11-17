#include "NoteAudition.h"

namespace OpenTune {

void NoteAudition::play(const juce::AudioBuffer<float>& source, double sourceSampleRate,
                        int startSample, int numSamples)
{
    const int sourceLength = source.getNumSamples();
    const int sourceChannels = source.getNumChannels();
    if (sourceSampleRate <= 0.0 || sourceChannels <= 0 || sourceLength <= 0)
        return;

    startSample = juce::jlimit(0, sourceLength, startSample);
    numSamples = juce::jlimit(0, sourceLength - startSample, numSamples);
    numSamples = juce::jmin(numSamples, static_cast<int>(kMaxDurationSeconds * sourceSampleRate));
    if (numSamples <= 0)
        return;

    const uint64_t generation = controlGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const int slotIndex = claimBuildSlot();
    jassert(slotIndex >= 0);
    if (slotIndex < 0)
        return;
    auto& slot = slots_[static_cast<size_t>(slotIndex)];
    auto& clip = slot.clip;

    // Prepare the clip up front: mono with 5 ms fades to avoid clicks.
    clip.sampleRate = sourceSampleRate;
    clip.mono.setSize(1, numSamples);
    clip.mono.clear();
    const float channelGain = 1.0f / static_cast<float>(sourceChannels);
    for (int ch = 0; ch < sourceChannels; ++ch)
        clip.mono.addFrom(0, 0, source, ch, startSample, numSamples, channelGain);

    const int fade = juce::jmin(numSamples / 2, static_cast<int>(0.005 * sourceSampleRate));
    if (fade > 0) {
        clip.mono.applyGainRamp(0, 0, fade, 0.0f, 1.0f);
        clip.mono.applyGainRamp(0, numSamples - fade, fade, 1.0f, 0.0f);
    }

    slot.generation = generation;

    for (auto& candidate : slots_) {
        uint8_t expected = static_cast<uint8_t>(SlotState::Pending);
        if (!candidate.state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Building),
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        candidate.clip = Clip{};
        candidate.state.store(static_cast<uint8_t>(SlotState::Free),
                              std::memory_order_release);
    }
    reclaimReturnedSlots();
    slot.state.store(static_cast<uint8_t>(SlotState::Pending), std::memory_order_release);
}

void NoteAudition::stop()
{
    const uint64_t generation = controlGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    stopGeneration_.store(generation, std::memory_order_release);

    for (auto& candidate : slots_) {
        uint8_t expected = static_cast<uint8_t>(SlotState::Pending);
        if (!candidate.state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Building),
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        candidate.clip = Clip{};
        candidate.state.store(static_cast<uint8_t>(SlotState::Free),
                              std::memory_order_release);
    }
    reclaimReturnedSlots();
}

int NoteAudition::claimBuildSlot() noexcept
{
    for (int i = 0; i < kSlotCount; ++i) {
        auto& state = slots_[static_cast<size_t>(i)].state;
        uint8_t expected = static_cast<uint8_t>(SlotState::Free);
        if (state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Building),
                std::memory_order_acq_rel, std::memory_order_acquire))
            return i;

        expected = static_cast<uint8_t>(SlotState::Returned);
        if (state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Building),
                std::memory_order_acq_rel, std::memory_order_acquire))
            return i;
    }

    for (int i = 0; i < kSlotCount; ++i) {
        auto& state = slots_[static_cast<size_t>(i)].state;
        uint8_t expected = static_cast<uint8_t>(SlotState::Pending);
        if (state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Building),
                std::memory_order_acq_rel, std::memory_order_acquire))
            return i;
    }

    return -1;
}

void NoteAudition::returnSlot(int slotIndex) noexcept
{
    slots_[static_cast<size_t>(slotIndex)].state.store(
        static_cast<uint8_t>(SlotState::Returned), std::memory_order_release);
}

void NoteAudition::reclaimReturnedSlots() noexcept
{
    for (auto& slot : slots_) {
        uint8_t expected = static_cast<uint8_t>(SlotState::Returned);
        if (!slot.state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Building),
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        slot.clip = Clip{};
        slot.state.store(static_cast<uint8_t>(SlotState::Free),
                         std::memory_order_release);
    }
}

void NoteAudition::mixIntoBuffer(juce::AudioBuffer<float>& output, int numSamples, double outputSampleRate)
{
    const uint64_t generation = controlGeneration_.load(std::memory_order_acquire);
    const uint64_t stopGeneration = stopGeneration_.load(std::memory_order_acquire);

    if (stopGeneration > appliedStopGeneration_ && stopGeneration == generation) {
        if (playingSlot_ >= 0)
            returnSlot(playingSlot_);
        playingSlot_ = -1;
        position_ = 0.0;
        appliedStopGeneration_ = stopGeneration;
    }

    int incomingSlot = -1;
    for (int i = 0; i < kSlotCount; ++i) {
        auto& slot = slots_[static_cast<size_t>(i)];
        uint8_t expected = static_cast<uint8_t>(SlotState::Pending);
        if (!slot.state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Playing),
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;

        if (slot.generation < generation) {
            returnSlot(i);
            continue;
        }
        if (slot.generation > generation) {
            slot.state.store(static_cast<uint8_t>(SlotState::Pending), std::memory_order_release);
            continue;
        }

        if (stopGeneration == generation && stopGeneration > appliedStopGeneration_) {
            returnSlot(i);
            continue;
        }
        if (incomingSlot < 0) {
            incomingSlot = i;
        } else {
            returnSlot(i);
        }
    }

    if (incomingSlot >= 0) {
        if (playingSlot_ >= 0)
            returnSlot(playingSlot_);
        playingSlot_ = incomingSlot;
        position_ = 0.0;
    }

    if (playingSlot_ < 0 || outputSampleRate <= 0.0)
        return;

    const auto& clip = slots_[static_cast<size_t>(playingSlot_)].clip;
    const int clipLength = clip.mono.getNumSamples();
    if (position_ >= static_cast<double>(clipLength - 1)) {
        returnSlot(playingSlot_);
        playingSlot_ = -1;
        position_ = 0.0;
        return;
    }

    const float* src = clip.mono.getReadPointer(0);
    const double step = clip.sampleRate / outputSampleRate;
    const int outChannels = output.getNumChannels();
    numSamples = juce::jmin(numSamples, output.getNumSamples());

    for (int i = 0; i < numSamples; ++i) {
        const int index = static_cast<int>(position_);
        if (index >= clipLength - 1)
            break;
        // Linear interpolation is good enough for audition when the sample rates differ.
        const float frac = static_cast<float>(position_ - static_cast<double>(index));
        const float sample = src[index] + (src[index + 1] - src[index]) * frac;
        for (int ch = 0; ch < outChannels; ++ch)
            output.addSample(ch, i, sample);
        position_ += step;
    }

    if (position_ >= static_cast<double>(clipLength - 1)) {
        returnSlot(playingSlot_);
        playingSlot_ = -1;
        position_ = 0.0;
    }
}

} // namespace OpenTune
