/**
 * TimeStretchCache — clip-wide single-entry cache for Stage 2 time-stretch output.
 *
 * Canonical audio stored at 44.1kHz as shared_ptr<const vector<float>> (immutable).
 * Prepared audio stored at target playback rate (shared_ptr<const vector<float>>).
 * Audio thread reads via direct integer-sample slice from prepared audio only.
 * Canonical slice provided for Stage2/export non-realtime reading.
 *
 * Writer side uses std::mutex; Entry is shared_ptr<const Entry> — published once, never mutated.
 * Audio thread atomic_load(readerMap_) — lock-free read path.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "../Content/ContentKey.h"
#include "../DSP/ResamplingManager.h"
#include "../Utils/TimeCoordinate.h"

namespace OpenTune {

class TimeStretchCache {
public:
    struct Entry {
        // Canonical (44.1kHz) Stage 2 output — immutable after publish
        std::shared_ptr<const std::vector<float>> canonicalAudio;
        uint64_t pitchRevision = 0;
        uint64_t pitchShiftRevision = 0;
        uint64_t timeGridRevision = 0;
        double sampleRate = 44100.0;
        bool published = false;

        // Prepared at target playback rate — immutable after publish.
        // When target rate == canonical rate, preparedAudio aliases canonicalAudio.
        std::shared_ptr<const std::vector<float>> preparedAudio;
        double preparedSampleRate = 0.0;
    };

    TimeStretchCache();
    ~TimeStretchCache();

    void store(ContentKey key,
               std::vector<float> audio,
               uint64_t pitchRevision,
               uint64_t pitchShiftRevision,
               uint64_t timeGridRevision,
               double sampleRate,
               uint32_t buildGeneration);

    uint32_t beginBuild(ContentKey key);

    int sliceForOutputRange(ContentKey key,
                            uint64_t pitchRevision,
                            uint64_t pitchShiftRevision,
                            uint64_t timeGridRevision,
                            int64_t readStartSample,
                            juce::AudioBuffer<float>& destination,
                            int destinationStartSample,
                            int numSamples,
                            int targetSampleRate) const;

    int sliceCanonicalForOutputRange(ContentKey key,
                                      uint64_t pitchRevision,
                                      uint64_t pitchShiftRevision,
                                      uint64_t timeGridRevision,
                                      int64_t readStartSample,
                                      juce::AudioBuffer<float>& destination,
                                      int destinationStartSample,
                                      int numSamples) const;

    /** 将所有已 publish 的 canonical entry 重采样到目标率（writer mutex 内完成）。 */
    void prepareForPlaybackSampleRate(double targetSr);

    void invalidate(ContentKey key);
    void clear();

private:
    mutable std::mutex mutex_;
    std::map<ContentKey, std::shared_ptr<const Entry>> entries_;

    // Atomic snapshot for lock-free readers. Published after each mutex-protected write.
    mutable std::shared_ptr<const std::map<ContentKey, std::shared_ptr<const Entry>>> readerMap_;

    mutable std::vector<std::shared_ptr<const std::map<ContentKey, std::shared_ptr<const Entry>>>> retiredSnapshots_;

    mutable std::map<ContentKey, uint32_t> invalidationGen_;

    // Prepared state: serialized by writer mutex_ only.
    double targetSampleRate_{TimeCoordinate::kRenderSampleRate};
    ResamplingManager preparedResampler_;

    static size_t entryTotalBytes(const Entry& e);
};

} // namespace OpenTune
