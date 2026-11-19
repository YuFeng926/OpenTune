#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#endif

#include "NotePitchAudition.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>

#include "CDSPResampler.h"
#include "../DSP/PhaseVocoderPitchShifter.h"
#include "../Utils/TimeCoordinate.h"

namespace OpenTune {
namespace {

constexpr float kPreviewGain = 0.7f;
constexpr double kMinPreviewSeconds = 0.3;
constexpr double kFadeSeconds = 0.01;
constexpr int kResampleInputBlockSamples = 4096;

std::vector<float> resampleMono(const float* input, size_t inputLength,
                                int inputSR, int targetSR,
                                const std::atomic<bool>* stopRequested)
{
    if (input == nullptr || inputLength == 0 || inputSR <= 0 || targetSR <= 0)
        return {};
    const auto cancelled = [stopRequested]() {
        return stopRequested != nullptr
            && stopRequested->load(std::memory_order_relaxed);
    };
    if (cancelled())
        return {};
    if (inputSR == targetSR)
        return {input, input + inputLength};

    int outputLength = static_cast<int>(std::min<int64_t>(
        TimeCoordinate::sampleRateProject(static_cast<int64_t>(inputLength),
                                          static_cast<double>(inputSR),
                                          static_cast<double>(targetSR)),
        static_cast<int64_t>(std::numeric_limits<int>::max())));
    if (outputLength <= 0)
        outputLength = 1;

    r8b::CDSPResampler24 resampler(
        static_cast<double>(inputSR), static_cast<double>(targetSR),
        kResampleInputBlockSamples);
    std::vector<double> inputBlock(static_cast<size_t>(kResampleInputBlockSamples));
    std::vector<float> output(static_cast<size_t>(outputLength));
    size_t inputOffset = 0;
    int outputOffset = 0;

    while (outputOffset < outputLength)
    {
        if (cancelled())
            return {};

        const int inputCount = inputOffset < inputLength
            ? static_cast<int>(std::min<size_t>(
                inputBlock.size(), inputLength - inputOffset))
            : kResampleInputBlockSamples;
        if (inputOffset < inputLength)
        {
            for (int i = 0; i < inputCount; ++i)
                inputBlock[static_cast<size_t>(i)] =
                    static_cast<double>(input[inputOffset + static_cast<size_t>(i)]);
            inputOffset += static_cast<size_t>(inputCount);
        }
        else
        {
            std::fill(inputBlock.begin(), inputBlock.end(), 0.0);
        }

        double* resampled = nullptr;
        const int resampledCount = resampler.process(
            inputBlock.data(), inputCount, resampled);
        if (cancelled())
            return {};

        const int copyCount = std::min(outputLength - outputOffset, resampledCount);
        for (int i = 0; i < copyCount; ++i)
        {
            if ((i & 0xFFF) == 0 && cancelled())
                return {};
            output[static_cast<size_t>(outputOffset + i)] =
                static_cast<float>(resampled[i]);
        }
        outputOffset += copyCount;
    }

    return output;
}

} // namespace

NotePitchPreview renderNotePitchPreview(
    const juce::AudioBuffer<float>& source,
    double sourceSampleRate,
    double startSeconds,
    double endSeconds,
    float targetHz,
    double outputSampleRate,
    const float* originalF0,
    int numOriginalF0Frames,
    double originalF0FrameRate,
    const std::atomic<bool>* stopRequested)
{
    NotePitchPreview preview;
    const auto cancelled = [stopRequested]() {
        return stopRequested != nullptr
            && stopRequested->load(std::memory_order_relaxed);
    };

    if (cancelled())
        return preview;
    if (targetHz <= 0.0f)
        return preview;
    if (outputSampleRate <= 0.0 || sourceSampleRate <= 0.0)
        return preview;
    if (!(endSeconds > startSeconds) || startSeconds < 0.0)
        return preview;
    if (source.getNumSamples() <= 0)
        return preview;
    if (originalF0 == nullptr || numOriginalF0Frames <= 0
        || !(originalF0FrameRate > 0.0))
        return preview;

    const int64_t sourceNumSamples = source.getNumSamples();
    const double sourceDuration = TimeCoordinate::samplesToSeconds(
        sourceNumSamples, sourceSampleRate);

    // Preview length: max(note duration, 0.3s), no upper cap, clamped to source.
    double previewStart = std::max(0.0, startSeconds);
    double previewEnd = std::min(endSeconds, sourceDuration);
    previewEnd = std::max(previewEnd,
        std::min(previewStart + kMinPreviewSeconds, sourceDuration));
    if (!(previewEnd > previewStart))
        return preview;

    const int64_t startSample = TimeCoordinate::secondsToSamplesFloor(
        previewStart, sourceSampleRate);
    const int64_t endSample = std::min(
        TimeCoordinate::secondsToSamplesCeil(previewEnd, sourceSampleRate),
        sourceNumSamples);
    const int64_t outputCount = endSample - startSample;
    if (outputCount <= 0)
        return preview;

    // Target curve has the full source F0 length. Frames outside the preview
    // range copy originalF0 (neutral) so phase replay starts close to the
    // window; frames covering the actual start/end control the shifted range.
    std::vector<float> targetF0;
    targetF0.reserve(static_cast<size_t>(numOriginalF0Frames));
    for (int i = 0; i < numOriginalF0Frames; ++i)
    {
        if ((i & 0xFFF) == 0 && cancelled())
            return preview;
        targetF0.push_back(originalF0[static_cast<size_t>(i)]);
    }
    const double shiftStartSeconds = TimeCoordinate::samplesToSeconds(
        startSample, sourceSampleRate);
    const double shiftEndSeconds = TimeCoordinate::samplesToSeconds(
        endSample, sourceSampleRate);
    const int firstFrame = static_cast<int>(std::clamp<int64_t>(
        TimeCoordinate::secondsToSamplesFloor(shiftStartSeconds, originalF0FrameRate),
        0, numOriginalF0Frames));
    const int lastFrame = static_cast<int>(std::clamp<int64_t>(
        TimeCoordinate::secondsToSamplesCeil(shiftEndSeconds, originalF0FrameRate) + 1,
        firstFrame, numOriginalF0Frames));
    bool hasVoicedFrame = false;
    for (int i = firstFrame; i < lastFrame; ++i)
    {
        if (((i - firstFrame) & 0xFFF) == 0 && cancelled())
            return preview;
        if (originalF0[static_cast<size_t>(i)] > 0.0f)
        {
            targetF0[static_cast<size_t>(i)] = targetHz;
            hasVoicedFrame = true;
        }
        else
        {
            targetF0[static_cast<size_t>(i)] = 0.0f;
        }
    }
    if (!hasVoicedFrame)
        return preview; // Never play unshifted audio as a "preview".

    if (cancelled())
        return preview;

    PhaseVocoderPitchShifter shifter(sourceSampleRate);
    std::vector<float> shifted = shifter.render(
        source.getReadPointer(0),
        static_cast<int>(sourceNumSamples),
        static_cast<int>(startSample),
        static_cast<int>(outputCount),
        originalF0,
        targetF0.data(),
        numOriginalF0Frames,
        originalF0FrameRate,
        stopRequested);
    if (shifted.size() != static_cast<size_t>(outputCount))
        return preview;

    if (cancelled())
        return preview;

    // Resample only the returned preview to the output rate.
    std::vector<float> output = (outputSampleRate == sourceSampleRate)
        ? std::move(shifted)
        : resampleMono(shifted.data(), shifted.size(),
            static_cast<int>(sourceSampleRate),
            static_cast<int>(outputSampleRate), stopRequested);
    if (output.empty() || cancelled())
        return preview;

    // Fixed gain + 10 ms half-cosine fade in/out. If the snippet is too short
    // for two fades, use half the length per fade.
    const int numSamples = static_cast<int>(output.size());
    const int fadeSamples = static_cast<int>(
        TimeCoordinate::secondsToSamplesExact(kFadeSeconds, outputSampleRate));
    const int halfFade = numSamples / 2;
    const int fadeLen = std::max(1, std::min(fadeSamples, halfFade));
    for (int i = 0; i < numSamples; ++i) {
        if ((i & 0xFFF) == 0 && cancelled())
            return preview;
        float env = 1.0f;
        if (i < fadeLen) {
            const double t = static_cast<double>(i) / fadeLen; // 0..1
            env = static_cast<float>(0.5 - 0.5 * std::cos(t * juce::MathConstants<double>::pi));
        } else if (i >= numSamples - fadeLen) {
            const double t = static_cast<double>(numSamples - 1 - i) / fadeLen;
            env = static_cast<float>(0.5 - 0.5 * std::cos(t * juce::MathConstants<double>::pi));
        }
        output[static_cast<size_t>(i)] *= kPreviewGain * env;
    }

    preview.mono = std::move(output);
    preview.sampleRate = outputSampleRate;
    return preview;
}

// ============================================================================
// NotePitchAudition
// ============================================================================

NotePitchAudition::NotePitchAudition()
    : juce::Thread("NotePitchAudition")
{
}

NotePitchAudition::~NotePitchAudition()
{
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        stopRequested_.store(true, std::memory_order_release);
        const uint64_t generation =
            controlGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
        stopGeneration_.store(generation, std::memory_order_release);
        hasPending_ = false;
        pending_ = Request{};
    }
    signalThreadShouldExit();
    wakeup_.signal();

    waitForThreadToExit(-1);
    reclaimReturnedSlots();
}

void NotePitchAudition::request(PlaybackReadSource source,
                                double startSeconds,
                                double endSeconds,
                                float targetHz,
                                double outputSampleRate)
{
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        const uint64_t generation =
            controlGeneration_.load(std::memory_order_relaxed) + 1;
        stopRequested_.store(true, std::memory_order_release);
        controlGeneration_.store(generation, std::memory_order_release);
        pending_.source = std::move(source);
        pending_.startSeconds = startSeconds;
        pending_.endSeconds = endSeconds;
        pending_.targetHz = targetHz;
        pending_.outputSampleRate = outputSampleRate;
        pending_.targetRegion = nextTargetRegion_.load(std::memory_order_acquire);
        pending_.generation = generation;
        hasPending_ = true;
    }
    reclaimReturnedSlots();
    if (!isThreadRunning())
        startThread(juce::Thread::Priority::low);
    wakeup_.signal();
}

void NotePitchAudition::stop()
{
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        stopRequested_.store(true, std::memory_order_release);
        const uint64_t generation =
            controlGeneration_.load(std::memory_order_relaxed) + 1;
        controlGeneration_.store(generation, std::memory_order_release);
        stopGeneration_.store(generation, std::memory_order_release);
        hasPending_ = false;
        pending_ = Request{};
    }
    reclaimReturnedSlots();
    wakeup_.signal();
}

bool NotePitchAudition::isActive() const noexcept
{
    for (const auto& voice : voices_) {
        if (voice.slotIndex >= 0 && (voice.gain > 0.0f || voice.targetGain > 0.0f))
            return true;
    }
    return false;
}

bool NotePitchAudition::isContentActive(const ContentKey& key) const noexcept
{
    for (const auto& voice : voices_) {
        if (voice.slotIndex >= 0
                && slots_[static_cast<size_t>(voice.slotIndex)].snippet.contentKey == key
                && (voice.gain > 0.0f || voice.targetGain > 0.0f))
            return true;
    }
    return false;
}

void NotePitchAudition::setTargetRegion(const void* region) noexcept
{
    nextTargetRegion_.store(region, std::memory_order_release);
}

void NotePitchAudition::clearActiveTargetRegion() noexcept
{
    activeTargetRegion_.store(nullptr, std::memory_order_release);
}

const void* NotePitchAudition::activeTargetRegion() const noexcept
{
    return activeTargetRegion_.load(std::memory_order_acquire);
}

int NotePitchAudition::claimBuildSlot() noexcept
{
    for (int i = 0; i < kNumSlots; ++i) {
        auto& slot = slots_[static_cast<size_t>(i)];
        for (const SlotState available : {SlotState::Free, SlotState::Returned}) {
            uint8_t expected = static_cast<uint8_t>(available);
            if (!slot.state.compare_exchange_strong(
                    expected, static_cast<uint8_t>(SlotState::Building),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            slot.snippet = Snippet{};
            return i;
        }
    }

    for (int i = 0; i < kNumSlots; ++i) {
        auto& slot = slots_[static_cast<size_t>(i)];
        uint8_t expected = static_cast<uint8_t>(SlotState::Pending);
        if (!slot.state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Building),
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        slot.snippet = Snippet{};
        return i;
    }
    return -1;
}

void NotePitchAudition::discardPendingSlots() noexcept
{
    for (auto& slot : slots_) {
        uint8_t expected = static_cast<uint8_t>(SlotState::Pending);
        if (!slot.state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Building),
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        slot.snippet = Snippet{};
        slot.state.store(static_cast<uint8_t>(SlotState::Free),
                         std::memory_order_release);
    }
}

void NotePitchAudition::returnSlot(int slotIndex) noexcept
{
    slots_[static_cast<size_t>(slotIndex)].state.store(
        static_cast<uint8_t>(SlotState::Returned), std::memory_order_release);
}

void NotePitchAudition::releaseVoice(Voice& voice) noexcept
{
    if (voice.slotIndex >= 0)
        returnSlot(voice.slotIndex);
    voice = Voice{};
}

void NotePitchAudition::processRejectedResult()
{
    const uint64_t rejected = rejectedGeneration_.exchange(0, std::memory_order_acq_rel);
    if (rejected <= workerRejectedGeneration_)
        return;

    workerRejectedGeneration_ = rejected;
    std::lock_guard<std::mutex> lock(requestMutex_);
    if (hasPending_ && pending_.generation == rejected) {
        hasPending_ = false;
        pending_ = Request{};
    }
}

void NotePitchAudition::reclaimReturnedSlots() noexcept
{
    for (auto& slot : slots_) {
        uint8_t expected = static_cast<uint8_t>(SlotState::Returned);
        if (!slot.state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::Building),
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        slot.snippet = Snippet{};
        slot.state.store(static_cast<uint8_t>(SlotState::Free),
                         std::memory_order_release);
    }
}

void NotePitchAudition::run()
{
    Request request;
    bool hasRequest = false;

    while (!threadShouldExit()) {
        processRejectedResult();
        reclaimReturnedSlots();

        if (!hasRequest) {
            std::lock_guard<std::mutex> lock(requestMutex_);
            if (hasPending_) {
                request = std::move(pending_);
                pending_ = Request{};
                hasPending_ = false;
                if (request.generation == controlGeneration_.load(std::memory_order_acquire)) {
                    stopRequested_.store(false, std::memory_order_release);
                    hasRequest = true;
                }
            }
        }

        if (!hasRequest) {
            wakeup_.wait();
            continue;
        }

        if (threadShouldExit())
            break;

        if (request.generation != controlGeneration_.load(std::memory_order_acquire)) {
            request = Request{};
            hasRequest = false;
            continue;
        }

        const int slotIndex = claimBuildSlot();
        if (slotIndex < 0) {
            if (threadShouldExit()
                    || request.generation != controlGeneration_.load(std::memory_order_acquire)
                    || stopRequested_.load(std::memory_order_acquire)) {
                request = Request{};
                hasRequest = false;
                continue;
            }
            std::this_thread::yield();
            continue;
        }

        const auto& source = request.source;
        NotePitchPreview preview;
        if (source.hasAudio()) {
            // The immutable snapshot's complete originalF0 track shares the
            // source/content time origin; no slicing loses frame phase.
            const float* originalF0 = nullptr;
            int numOriginalF0Frames = 0;
            double originalF0FrameRate = 0.0;
            if (source.contentSnapshot != nullptr
                    && source.contentSnapshot->pitchCurve != nullptr) {
                const auto& curve = *source.contentSnapshot->pitchCurve;
                if (curve.getHopSize() > 0 && curve.getSampleRate() > 0.0) {
                    const double frameRate =
                        curve.getSampleRate() / static_cast<double>(curve.getHopSize());
                    const auto& track = curve.getOriginalF0();
                    if (frameRate > 0.0 && !track.empty()) {
                        originalF0 = track.data();
                        numOriginalF0Frames = static_cast<int>(track.size());
                        originalF0FrameRate = frameRate;
                    }
                }
            }

            // 音高试听 DSP 以内容本地请求时间读取 canonical 编辑输入。
            preview = renderNotePitchPreview(
                *source.canonicalBuffer,
                source.canonicalSampleRate,
                request.startSeconds,
                request.endSeconds,
                request.targetHz,
                request.outputSampleRate,
                originalF0, numOriginalF0Frames, originalF0FrameRate,
                &stopRequested_);
        }

        auto& slot = slots_[static_cast<size_t>(slotIndex)];
        const bool stale = request.generation
            != controlGeneration_.load(std::memory_order_acquire);
        if (stale || stopRequested_.load(std::memory_order_acquire)
                || preview.mono.empty() || preview.sampleRate <= 0.0) {
            slot.snippet = Snippet{};
            slot.state.store(static_cast<uint8_t>(SlotState::Free), std::memory_order_release);
            request = Request{};
            hasRequest = false;
            continue;
        }

        discardPendingSlots();
        if (request.generation != controlGeneration_.load(std::memory_order_acquire)
                || stopRequested_.load(std::memory_order_acquire)) {
            slot.snippet = Snippet{};
            slot.state.store(static_cast<uint8_t>(SlotState::Free), std::memory_order_release);
            request = Request{};
            hasRequest = false;
            continue;
        }

        slot.snippet = Snippet{std::move(preview.mono), preview.sampleRate,
                               source.contentKey, request.targetRegion};
        slot.generation = request.generation;
        slot.state.store(static_cast<uint8_t>(SlotState::Pending), std::memory_order_release);
        request = Request{};
        hasRequest = false;
    }
}

void NotePitchAudition::mixIntoBuffer(juce::AudioBuffer<float>& output,
                                      int numSamples,
                                      double outputSampleRate) noexcept
{
    if (numSamples <= 0 || outputSampleRate <= 0.0)
        return;

    const uint64_t generation = controlGeneration_.load(std::memory_order_acquire);
    const uint64_t stopGeneration = stopGeneration_.load(std::memory_order_acquire);
    if (stopGeneration > appliedStopGeneration_) {
        for (auto& voice : voices_) {
            if (voice.slotIndex >= 0 && voice.generation < stopGeneration)
                voice.targetGain = 0.0f;
        }
        appliedStopGeneration_ = stopGeneration;
    }

    int incomingSlot = -1;
    for (int i = 0; i < kNumSlots; ++i) {
        auto& slot = slots_[static_cast<size_t>(i)];
        uint8_t expected = static_cast<uint8_t>(SlotState::Pending);
        if (!slot.state.compare_exchange_strong(
                expected, static_cast<uint8_t>(SlotState::AudioOwned),
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
        if (slot.snippet.sampleRate != outputSampleRate) {
            const uint64_t previous = rejectedGeneration_.load(std::memory_order_relaxed);
            if (slot.generation > previous)
                rejectedGeneration_.store(slot.generation, std::memory_order_release);
            returnSlot(i);
            continue;
        }
        if (incomingSlot < 0)
            incomingSlot = i;
        else
            returnSlot(i);
    }

    if (incomingSlot >= 0) {
        Voice* incoming = nullptr;
        for (auto& voice : voices_) {
            if (voice.slotIndex < 0 || voice.gain <= 0.0f) {
                incoming = &voice;
                break;
            }
        }
        if (incoming == nullptr)
            incoming = voices_[0].gain <= voices_[1].gain ? &voices_[0] : &voices_[1];

        Voice& previous = incoming == &voices_[0] ? voices_[1] : voices_[0];
        if (previous.slotIndex >= 0)
            previous.targetGain = 0.0f;
        releaseVoice(*incoming);
        incoming->slotIndex = incomingSlot;
        incoming->generation = slots_[static_cast<size_t>(incomingSlot)].generation;
        incoming->position = 0;
        incoming->gain = 1.0f;
        incoming->targetGain = 1.0f;
        activeTargetRegion_.store(
            slots_[static_cast<size_t>(incomingSlot)].snippet.targetRegion,
            std::memory_order_release);
    }

    const double fadeRate =
        1.0 / std::max(1.0, kVoiceFadeSeconds * outputSampleRate);

    for (auto& voice : voices_) {
        if (voice.slotIndex < 0)
            continue;
        const auto& snippet = slots_[static_cast<size_t>(voice.slotIndex)].snippet;
        if (snippet.sampleRate != outputSampleRate) {
            releaseVoice(voice);
            continue;
        }
        if (voice.gain <= 0.0f) {
            if (voice.targetGain == 0.0f)
                releaseVoice(voice);
            continue;
        }
        const auto& mono = snippet.mono;
        const int totalSamples = static_cast<int>(mono.size());

        for (int i = 0; i < numSamples; ++i) {
            if (voice.position >= totalSamples)
                break;
            const int index = voice.position;
            const float sample = mono[static_cast<size_t>(index)];
            ++voice.position;

            // Per-sample linear gain ramp toward the target.
            if (voice.gain < voice.targetGain)
                voice.gain = std::min(voice.gain + static_cast<float>(fadeRate),
                                      voice.targetGain);
            else if (voice.gain > voice.targetGain)
                voice.gain = std::max(voice.gain - static_cast<float>(fadeRate),
                                      voice.targetGain);

            const float value = sample * voice.gain;
            if (value != 0.0f) {
                for (int ch = 0; ch < output.getNumChannels(); ++ch)
                    output.addSample(ch, i, value);
            }
        }

        if (voice.position >= totalSamples
                || (voice.gain <= 0.0f && voice.targetGain == 0.0f))
            releaseVoice(voice);
    }

    if (!isActive())
        activeTargetRegion_.store(nullptr, std::memory_order_release);
}

} // namespace OpenTune
