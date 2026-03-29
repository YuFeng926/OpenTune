#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <map>
#include <unordered_map>
#include <atomic>
#include "Utils/TimeCoordinate.h"

struct RenderCacheTestAccessor;

namespace OpenTune {

class RenderCache {
public:
    static constexpr size_t kDefaultGlobalCacheLimitBytes = static_cast<size_t>(1536) * 1024 * 1024;
    static constexpr double kSampleRate = TimeCoordinate::kRenderSampleRate;

    struct GlobalMemoryStats {
        size_t cacheLimitBytes{0};
        size_t cacheCurrentBytes{0};
        size_t cachePeakBytes{0};
    };

    struct Chunk {
        double startSeconds{0.0};
        double endSeconds{0.0};
        std::vector<float> audio;
        std::unordered_map<int, std::vector<float>> resampledAudio;
        uint64_t desiredRevision{0};
        uint64_t publishedRevision{0};
    };

    RenderCache();
    ~RenderCache();

    uint64_t bumpChunkRevision(double startSeconds, double endSeconds);

    bool addChunk(double startSeconds, double endSeconds, std::vector<float>&& audio, uint64_t targetRevision);

    bool addResampledChunk(double startSeconds, double endSeconds, int targetSampleRate, 
                          std::vector<float>&& resampledAudio, uint64_t targetRevision);

    int readAtTimeForRate(float* dest, int numSamples, double timeSeconds, 
                          int targetSampleRate, bool nonBlocking = false);

    bool isRevisionPublished(double startSeconds, double endSeconds, uint64_t revision) const;

    void clearRange(double startSeconds, double endSeconds);

    void clearResampledCache();

    void clear();

    size_t getTotalMemoryUsage() const;
    void setMemoryLimit(size_t bytes);

    static GlobalMemoryStats getGlobalMemoryStats();

private:
    friend struct RenderCacheTestAccessor;
    mutable juce::SpinLock lock_;
    std::map<double, Chunk> chunks_;
    size_t totalMemoryUsage_ = 0;

    static std::atomic<size_t>& globalCacheLimitBytes();
    static std::atomic<size_t>& globalCacheCurrentBytes();
    static std::atomic<size_t>& globalCachePeakBytes();
};

} // namespace OpenTune
