#include "NotePitchAudition.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>

#include "../DSP/PhaseVocoderPitchShifter.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/TimeCoordinate.h"

namespace OpenTune {
namespace {

constexpr float kPreviewGain = 0.7f;
constexpr double kMinPreviewSeconds = 0.3;
constexpr double kFadeSeconds = 0.01;

std::vector<float> resampleMono(const float* input, size_t inputLength,
                                int inputSR, int targetSR)
{
    if (input == nullptr || inputLength == 0 || inputSR <= 0 || targetSR <= 0)
        return {};
    if (inputSR == targetSR)
        return {input, input + inputLength};
    ResamplingManager resampler;
    if (targetSR < inputSR)
        return resampler.downsampleForInference(input, inputLength, inputSR, targetSR);
    return resampler.upsampleForHost(input, inputLength, inputSR, targetSR);
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
    double originalF0FrameRate)
{
    NotePitchPreview preview;
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
    std::vector<float> targetF0(originalF0, originalF0 + numOriginalF0Frames);
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

    PhaseVocoderPitchShifter shifter(sourceSampleRate);
    std::vector<float> shifted = shifter.render(
        source.getReadPointer(0),
        static_cast<int>(sourceNumSamples),
        static_cast<int>(startSample),
        static_cast<int>(outputCount),
        originalF0,
        targetF0.data(),
        numOriginalF0Frames,
        originalF0FrameRate);
    if (shifted.size() != static_cast<size_t>(outputCount))
        return preview;

    // Resample only the returned preview to the output rate.
    std::vector<float> output = (outputSampleRate == sourceSampleRate)
        ? std::move(shifted)
        : resampleMono(shifted.data(), shifted.size(),
            static_cast<int>(sourceSampleRate),
            static_cast<int>(outputSampleRate));
    if (output.empty())
        return preview;

    // Fixed gain + 10 ms half-cosine fade in/out. If the snippet is too short
    // for two fades, use half the length per fade.
    const int numSamples = static_cast<int>(output.size());
    const int fadeSamples = static_cast<int>(
        TimeCoordinate::secondsToSamplesExact(kFadeSeconds, outputSampleRate));
    const int halfFade = numSamples / 2;
    const int fadeLen = std::max(1, std::min(fadeSamples, halfFade));
    for (int i = 0; i < numSamples; ++i) {
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
    stopRequested_.store(true, std::memory_order_release);
    hasPending_ = false;
    wakeup_.signal();
    stopThread(2000);
}

void NotePitchAudition::request(PlaybackReadSource source,
                                double startSeconds,
                                double endSeconds,
                                float targetHz,
                                double outputSampleRate)
{
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        pending_.source = std::move(source);
        pending_.startSeconds = startSeconds;
        pending_.endSeconds = endSeconds;
        pending_.targetHz = targetHz;
        pending_.outputSampleRate = outputSampleRate;
        hasPending_ = true;
    }
    stopRequested_.store(false, std::memory_order_release);
    if (!isThreadRunning())
        startThread(juce::Thread::Priority::low);
    wakeup_.signal();
}

void NotePitchAudition::stop()
{
    stopRequested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        hasPending_ = false;
        pending_ = Request{};
    }
    fadeOutAll_.store(true, std::memory_order_release);
    wakeup_.signal();
}

void NotePitchAudition::pruneRetired()
{
    retired_.erase(
        std::remove_if(retired_.begin(), retired_.end(),
            [](const std::shared_ptr<const Snippet>& s) {
                return !s || s.use_count() == 1;
            }),
        retired_.end());
}

void NotePitchAudition::run()
{
    while (!threadShouldExit()) {
        wakeup_.wait();

        if (threadShouldExit())
            break;

        Request request;
        bool valid = false;
        {
            std::lock_guard<std::mutex> lock(requestMutex_);
            valid = hasPending_;
            if (valid) {
                request = std::move(pending_);
                hasPending_ = false;
                pending_ = Request{};
            }
        }

        if (stopRequested_.load(std::memory_order_acquire) || !valid) {
            pruneRetired();
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

            preview = renderNotePitchPreview(
                *source.audioBuffer,
                source.audioSampleRate,
                request.startSeconds,
                request.endSeconds,
                request.targetHz,
                request.outputSampleRate,
                originalF0, numOriginalF0Frames, originalF0FrameRate);
        }

        // Drop the result if stop was requested or a newer pending request
        // exists (single pending slot identity).
        bool superseded = false;
        {
            std::lock_guard<std::mutex> lock(requestMutex_);
            superseded = hasPending_;
        }
        if (stopRequested_.load(std::memory_order_acquire) || superseded
                || preview.mono.empty() || preview.sampleRate <= 0.0) {
            pruneRetired();
            continue;
        }

        // Publish: retire the previously published snippet (producer keeps a
        // reference so the audio thread never frees the last one), then store
        // the new snippet atomically.
        {
            auto newSnippet = std::make_shared<const Snippet>(
                Snippet{std::move(preview.mono), preview.sampleRate});
            auto previous = std::atomic_load(&published_);
            if (previous)
                retired_.push_back(std::move(previous));
            std::atomic_store(&published_,
                std::static_pointer_cast<const Snippet>(newSnippet));
        }
        pruneRetired();
    }
}

void NotePitchAudition::mixIntoBuffer(juce::AudioBuffer<float>& output,
                                      int numSamples,
                                      double outputSampleRate) noexcept
{
    if (numSamples <= 0 || outputSampleRate <= 0.0)
        return;

    // Pick up a newly published snippet (or an ignored rate mismatch). A
    // snippet counts as new only if this player has not already consumed it,
    // which also prevents retriggering after stop() fades the voices out.
    const auto published = std::atomic_load(&published_);
    if (published && published != seenSnippet_) {
        seenSnippet_ = published;
        if (published->sampleRate != outputSampleRate) {
            // prepare/rate changes stop the audition: ignore this snippet.
            std::atomic_store(&published_, std::shared_ptr<const Snippet>());
        } else {
            // Start a new voice and fade the previous voice out over 10 ms.
            // Prefer the inactive/zero-gain voice as the incoming slot so the
            // currently audible voice keeps sounding during rapid retriggers.
            Voice* incoming = nullptr;
            for (auto& voice : voices_) {
                if (!voice.snippet || voice.gain <= 0.0f) {
                    incoming = &voice;
                    break;
                }
            }
            if (incoming == nullptr) {
                incoming = (voices_[0].gain <= voices_[1].gain)
                    ? &voices_[0] : &voices_[1];
            }
            Voice& previous = (incoming == &voices_[0]) ? voices_[1] : voices_[0];
            previous.targetGain = 0.0f;
            incoming->snippet = published;
            incoming->position = 0.0;
            incoming->gain = 1.0f;
            incoming->targetGain = 1.0f;
            fadeOutAll_.store(false, std::memory_order_release);
        }
    }

    // Stop request applies to this block: ramp voices down immediately instead
    // of mixing one full-gain block first.
    if (fadeOutAll_.load(std::memory_order_acquire)) {
        for (auto& voice : voices_) {
            if (voice.snippet)
                voice.targetGain = 0.0f;
        }
    }

    const double fadeRate =
        1.0 / std::max(1.0, kVoiceFadeSeconds * outputSampleRate);

    for (auto& voice : voices_) {
        if (!voice.snippet || voice.gain <= 0.0f)
            continue;
        const auto& mono = voice.snippet->mono;
        const int totalSamples = static_cast<int>(mono.size());
        const double snippetRate = voice.snippet->sampleRate;

        for (int i = 0; i < numSamples; ++i) {
            if (voice.position >= totalSamples)
                break;
            // Advance the voice position on its own snippet rate grid.
            const int index = static_cast<int>(voice.position);
            if (index >= totalSamples)
                break;
            const float sample = mono[static_cast<size_t>(index)];
            voice.position += snippetRate / outputSampleRate;

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

        if (voice.position >= totalSamples) {
            voice.snippet.reset();
            voice.position = 0.0;
            voice.gain = 0.0f;
            voice.targetGain = 0.0f;
        }
    }

    if (fadeOutAll_.load(std::memory_order_acquire)) {
        for (auto& voice : voices_) {
            if (voice.snippet)
                voice.targetGain = 0.0f;
        }
        const bool allSilent = std::all_of(std::begin(voices_), std::end(voices_),
            [](const Voice& v) { return !v.snippet || v.gain <= 0.0f; });
        if (allSilent) {
            for (auto& voice : voices_) {
                voice.snippet.reset();
                voice.position = 0.0;
                voice.gain = 0.0f;
                voice.targetGain = 0.0f;
            }
            fadeOutAll_.store(false, std::memory_order_release);
        }
    }
}

} // namespace OpenTune
