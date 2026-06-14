#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "../Utils/LockFreeQueue.h"
#include "../Utils/SilentGapDetector.h"
#include "../Content/ContentKey.h"

namespace OpenTune {

struct F0RequestKey {
    ContentKey contentKey;
    int trackId{0};
    int placementIndex{-1};

    bool operator==(const F0RequestKey& o) const noexcept {
        return contentKey == o.contentKey
            && trackId == o.trackId
            && placementIndex == o.placementIndex;
    }
    bool operator!=(const F0RequestKey& o) const noexcept { return !(*this == o); }
};

} // namespace OpenTune

template <>
struct std::hash<OpenTune::F0RequestKey> {
    size_t operator()(const OpenTune::F0RequestKey& k) const noexcept {
        size_t h = static_cast<size_t>(k.contentKey.domainKind);
        h ^= static_cast<size_t>(k.contentKey.objectId * 1099511628211ULL);
        h ^= static_cast<size_t>(k.contentKey.sourceWindowDiscriminator * 1099511628211ULL);
        h ^= static_cast<size_t>(k.trackId) * 1099511628211ULL;
        h ^= static_cast<size_t>(k.placementIndex) * 1099511628211ULL;
        return h;
    }
};

namespace OpenTune {

class F0ExtractionService {
public:
    struct Result {
        bool success{false};
        int trackId{0};
        int placementIndexHint{-1};
        ContentKey contentKey;
        F0RequestKey requestKey;
        uint64_t requestToken{0};
        std::shared_ptr<const juce::AudioBuffer<float>> sourceAudioBuffer;
        int hopSize{0};
        int f0SampleRate{0};
        std::vector<float> f0;
        std::vector<float> energy;
        std::vector<SilentGap> silentGaps;
        const char* modelName{"Unknown"};
        std::string errorMessage;

        double audioDurationSeconds{0.0};
        double firstAudibleTimeSeconds{-1.0};
        double firstVoicedTimeSeconds{-1.0};
        int firstVoicedFrame{-1};
        int expectedInferenceFrameCount{0};
    };

    using ExecuteFn = std::function<Result()>;
    using CommitFn = std::function<void(Result&&)>;

    enum class SubmitResult : uint8_t {
        Accepted,
        AlreadyInProgress,
        QueueFull,
        InvalidTask
    };

    explicit F0ExtractionService(int workerCount = 1, size_t maxQueueSize = 64);
    ~F0ExtractionService();

    static F0RequestKey makeRequestKey(ContentKey contentKey, int trackId, int placementIndex);

    SubmitResult submit(F0RequestKey requestKey, ExecuteFn execute, CommitFn commit);

    bool isActive(F0RequestKey requestKey) const;
    void cancel(F0RequestKey requestKey);

private:
    struct Task {
        F0RequestKey requestKey;
        uint64_t token{0};
        ExecuteFn execute;
        CommitFn commit;
    };

    struct ActiveEntry {
        std::atomic<uint64_t> token{0};
    };

    void workerLoop();

    LockFreeQueue<Task> queue_;
    std::unordered_map<F0RequestKey, std::unique_ptr<ActiveEntry>> activeEntries_;
    mutable std::mutex entriesMutex_;
    std::vector<std::thread> workers_;
    size_t maxQueueSize_{64};
    std::atomic<uint64_t> tokenCounter_{1};
    std::atomic<bool> running_{true};
};

} // namespace OpenTune
