/**
 * TimeStretchCache — clip-wide single-entry cache for Stage 2 time-stretch output.
 *
 * Canonical audio stored at 44.1kHz as shared_ptr<const vector<float>> (immutable).
 * Prepared audio stored at target playback rate (shared_ptr<const vector<float>>).
 * Audio thread reads via direct integer-sample slice from prepared audio only.
 * Canonical slice provided for Stage2/export non-realtime reading.
 *
 * Writer side uses std::mutex; Entry is shared_ptr<const Entry> — published once, never mutated.
 * Audio thread borrows readerMap_ through RealtimeSnapshot::Read.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "../Content/ContentKey.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/TimeCoordinate.h"
#include "../Utils/RealtimeSnapshot.h"

namespace OpenTune {

class TimeStretchCache {
public:
    struct Entry {
        // Canonical Stage 2 output on the TimeGrid local output grid —
        // immutable after publish. firstCanonicalOutputSample is the explicit
        // output-grid index of canonicalAudio[0].
        std::shared_ptr<const std::vector<float>> canonicalAudio;
        uint64_t contentRevision = 0;
        uint64_t timeGridRevision = 0;
        double sampleRate = TimeCoordinate::kRenderSampleRate;
        int64_t firstCanonicalOutputSample = 0;
        bool published = false;

        // Prepared at target playback rate — immutable after publish.
        // firstPreparedOutputSample is the explicit playback-grid index of
        // preparedAudio[0]; when target rate == canonical rate the prepared
        // audio aliases the canonical audio.
        std::shared_ptr<const std::vector<float>> preparedAudio;
        double preparedSampleRate = 0.0;
        int64_t firstPreparedOutputSample = 0;
    };

    TimeStretchCache();
    ~TimeStretchCache();

    void store(ContentKey key,
               std::vector<float> audio,
               uint64_t contentRevision,
               uint64_t timeGridRevision,
               double sampleRate,
               uint32_t buildGeneration,
               int64_t firstCanonicalOutputSample = 0);

    uint32_t beginBuild(ContentKey key);

    int sliceForOutputRange(ContentKey key,
                            uint64_t contentRevision,
                            uint64_t timeGridRevision,
                            int64_t readStartSample,
                            juce::AudioBuffer<float>& destination,
                            int destinationStartSample,
                            int numSamples,
                            double targetSampleRate) const;

    /** Read the canonical Stage2 output for offline output-rate playback. */
    int sliceCanonicalForOutputRange(ContentKey key,
                                     uint64_t contentRevision,
                                     uint64_t timeGridRevision,
                                     int64_t readStartSample,
                                     juce::AudioBuffer<float>& destination,
                                     int destinationStartSample,
                                     int numSamples) const;

    /** 将所有已 publish 的 canonical entry 重采样到目标率（writer mutex 内完成）。 */
    void prepareForPlaybackSampleRate(double targetSr);

    void invalidate(ContentKey key);
    void clear();
    void collectPlaybackSnapshots();

private:
    mutable std::mutex mutex_;
    std::map<ContentKey, std::shared_ptr<const Entry>> entries_;

    using ReaderMap = std::map<ContentKey, std::shared_ptr<const Entry>>;
    mutable RealtimeSnapshot<ReaderMap> readerMap_;

    std::map<ContentKey, uint32_t> buildGenerationByKey_;
    uint32_t nextBuildGeneration_{0};

    // Prepared state: serialized by writer mutex_ only.
    double targetSampleRate_{TimeCoordinate::kRenderSampleRate};
    ResamplingManager preparedResampler_;

    // Range SRC from the complete Stage2 output. mutex_ must be held.
    void prepareEntry(Entry& entry, double targetSr);
    void publishReaderMapLocked();
};

} // namespace OpenTune
