#pragma once

#include "../Render/ContentRenderService.h"
#include "../DSP/ResamplingManager.h"
#include "PlaybackSampleGeometry.h"
#include "TimeCoordinate.h"
#include <algorithm>
#include <cmath>

namespace OpenTune {

/** Prepared addresses use the source grid for identity playback and the
 * TimeGrid output grid for Stage2. outputStartSeconds is the owner's explicit
 * content-local output-time anchor for the first requested sample. */
struct PlaybackReadRequest {
    PlaybackReadSource source;
    int64_t readStartSample{0};
    double targetSampleRate{0.0};
    int numSamples{0};
    double outputStartSeconds{0.0};

    PlaybackReadRequest() = default;
    PlaybackReadRequest(PlaybackReadSource src, int64_t startSample, double rate,
                        int samples, double outputTime)
        : source(std::move(src)), readStartSample(startSample), targetSampleRate(rate),
          numSamples(samples), outputStartSeconds(outputTime) {}
};

/** Stage2 input uses content-local canonical indices, matching Stage1 chunks. */
struct CanonicalReadRequest {
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    std::shared_ptr<RenderCache> renderCache;
    int64_t readStartSample{0};
    int numSamples{0};

    CanonicalReadRequest() = default;
    CanonicalReadRequest(std::shared_ptr<const juce::AudioBuffer<float>> buffer,
                         std::shared_ptr<RenderCache> cache, int64_t startSample, int samples)
        : audioBuffer(std::move(buffer)), renderCache(std::move(cache)),
          readStartSample(startSample), numSamples(samples) {}
};

inline void applyAutomationGain(juce::AudioBuffer<float>& destination,
                                int destinationStartSample, int numSamples,
                                const EditableContentSnapshot& snapshot,
                                double outputStartSeconds, double targetSampleRate)
{
    if (snapshot.volumeEnvelope.empty())
        return;
    constexpr float kDbToLinear = 0.11512925465f;
    for (int s = 0; s < numSamples; ++s) {
        const double outputSeconds = outputStartSeconds + static_cast<double>(s) / targetSampleRate;
        const double sourceSeconds = snapshot.timeGrid->tauInverse(outputSeconds);
        const float gain = std::exp(snapshot.volumeEnvelope.evalAt(sourceSeconds) * kDbToLinear);
        for (int ch = 0; ch < destination.getNumChannels(); ++ch)
            destination.getWritePointer(ch, destinationStartSample)[s] *= gain;
    }
}

namespace PlaybackReaderDetail {

inline int writableSamples(const juce::AudioBuffer<float>& destination, int start, int count)
{
    if (start < 0 || start >= destination.getNumSamples() || destination.getNumChannels() == 0)
        return 0;
    return std::max(0, std::min(count, destination.getNumSamples() - start));
}

/** Return the written extent, including a clipped leading zero prefix. */
inline int copyPcm(const juce::AudioBuffer<float>& source, int64_t firstSourceSample,
                   int64_t readStartSample, int count,
                   juce::AudioBuffer<float>& destination, int destinationStartSample)
{
    using namespace PlaybackSampleGeometry;
    const auto span = intersect({readStartSample, readStartSample + count},
                               {firstSourceSample, firstSourceSample + source.getNumSamples()});
    if (span.empty())
        return 0;
    const int leading = static_cast<int>(span.begin - readStartSample);
    const int copied = static_cast<int>(span.end - span.begin);
    destination.clear(destinationStartSample, count);
    for (int ch = 0; ch < destination.getNumChannels(); ++ch)
        destination.copyFrom(ch, destinationStartSample + leading,
            source, ch % source.getNumChannels(),
            static_cast<int>(span.begin - firstSourceSample), copied);
    return leading + copied;
}

} // namespace PlaybackReaderDetail

/** Realtime direct copy plus edited overlay and automation. The returned extent
 * includes the leading zero prefix so a negative read preserves its valid suffix. */
inline int readPlaybackAudio(const PlaybackReadRequest& request,
                             juce::AudioBuffer<float>& destination, int destinationStartSample)
{
    const auto& snapshot = request.source.contentSnapshot;
    const int count = PlaybackReaderDetail::writableSamples(destination, destinationStartSample, request.numSamples);
    if (count == 0 || request.targetSampleRate <= 0.0 || !request.source.hasAudio() || !snapshot)
        return 0;

    int wrote = 0;
    if (!snapshot->timeGrid->isIdentity()) {
        if (!request.source.timeStretchCache)
            return 0;
        wrote = request.source.timeStretchCache->sliceForOutputRange(
            request.source.contentKey, snapshot->contentRevision, snapshot->timeGridRevision,
            request.readStartSample, destination, destinationStartSample, count, request.targetSampleRate);
    } else {
        const auto& dry = request.source.preparedDry;
        if (!dry.buffer || dry.sampleRate != request.targetSampleRate)
            return 0;
        wrote = PlaybackReaderDetail::copyPcm(*dry.buffer, dry.firstPreparedSample,
            request.readStartSample, count, destination, destinationStartSample);
        if (wrote > 0 && request.source.renderCache)
            request.source.renderCache->overlayPreparedAudio(
                destination, destinationStartSample, wrote, request.readStartSample,
                request.targetSampleRate, snapshot->contentRevision);
    }
    if (wrote > 0)
        applyAutomationGain(destination, destinationStartSample, wrote, *snapshot,
                            request.outputStartSeconds, request.targetSampleRate);
    return wrote;
}

/** Stage2 reads the canonical input plus Stage1 edits in content-local indices. */
inline int readSourceAudioWithStage1Overlay(const CanonicalReadRequest& request,
                                            juce::AudioBuffer<float>& destination, int destinationStartSample)
{
    const int count = PlaybackReaderDetail::writableSamples(destination, destinationStartSample, request.numSamples);
    if (count == 0 || !request.audioBuffer)
        return 0;
    const int wrote = PlaybackReaderDetail::copyPcm(*request.audioBuffer, 0,
        request.readStartSample, count, destination, destinationStartSample);
    if (wrote > 0 && request.renderCache)
        request.renderCache->overlayCanonicalAudio(destination, destinationStartSample, wrote, request.readStartSample);
    return wrote;
}

inline int readCanonicalAudio(const CanonicalReadRequest& request,
                              juce::AudioBuffer<float>& destination, int destinationStartSample)
{
    return readSourceAudioWithStage1Overlay(request, destination, destinationStartSample);
}

/** Offline export reads native PCM on the export grid and applies canonical edits.
 * Preparation is confined to this requested range and leaves realtime state intact. */
inline int readExportPlaybackAudio(const PlaybackReadRequest& request,
                                   juce::AudioBuffer<float>& destination, int destinationStartSample)
{
    const auto& snapshot = request.source.contentSnapshot;
    const int count = PlaybackReaderDetail::writableSamples(destination, destinationStartSample, request.numSamples);
    if (count == 0 || request.targetSampleRate != TimeCoordinate::kRenderSampleRate
        || !snapshot || !request.source.hasAudio())
        return 0;

    int wrote = 0;
    if (!snapshot->timeGrid->isIdentity()) {
        if (!request.source.timeStretchCache)
            return 0;
        wrote = request.source.timeStretchCache->sliceCanonicalForOutputRange(
            request.source.contentKey, snapshot->contentRevision, snapshot->timeGridRevision,
            request.readStartSample, destination, destinationStartSample, count);
    } else {
        const auto& native = request.source.nativeSource;
        if (native.sampleRate == request.targetSampleRate) {
            wrote = PlaybackReaderDetail::copyPcm(*native.buffer, native.firstSourceSample,
                request.readStartSample, count, destination, destinationStartSample);
        } else {
            ResamplingManager resampler;
            for (int ch = 0; ch < destination.getNumChannels(); ++ch) {
                const auto pcm = resampler.resampleRange(
                    native.buffer->getReadPointer(ch % native.buffer->getNumChannels()),
                    native.buffer->getNumSamples(), native.sampleRate, request.targetSampleRate,
                    native.firstSourceSample, request.readStartSample, count);
                destination.copyFrom(ch, destinationStartSample, pcm.data(), count);
            }
            wrote = count;
        }
        if (wrote > 0 && request.source.renderCache)
            request.source.renderCache->overlayCanonicalAudio(
                destination, destinationStartSample, wrote,
                request.readStartSample - request.source.firstCanonicalSample);
    }
    if (wrote > 0)
        applyAutomationGain(destination, destinationStartSample, wrote, *snapshot,
                            request.outputStartSeconds, request.targetSampleRate);
    return wrote;
}

} // namespace OpenTune
