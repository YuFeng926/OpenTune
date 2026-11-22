#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include "../Content/ContentKey.h"
#include "../Content/OriginalF0InputStamp.h"
#include "../Inference/F0InferenceService.h"
#include "../Utils/SilentGapDetector.h"

namespace OpenTune {

class ProcessF0Runtime;

// Processor-side request identity.
struct F0RequestKey {
    ContentKey contentKey;

    bool operator==(const F0RequestKey& o) const noexcept { return contentKey == o.contentKey; }
    bool operator!=(const F0RequestKey& o) const noexcept { return !(*this == o); }
};

struct F0TaskId {
    uint64_t value{0};

    bool operator==(F0TaskId rhs) const noexcept { return value == rhs.value; }
    bool operator<(F0TaskId rhs) const noexcept { return value < rhs.value; }
};

struct AraF0ConfigTag {
    uint64_t selectionRevision{0};
    std::string absoluteModelPath;
    int64_t modelBytes{0};
    juce::Time modelWriteTime;
    F0ExtractionConfigSnapshot extraction;
    bool allowCachedAnalysis{false}; // Archive continuation requires a fresh analysis until P3 removes the disk cache.

    bool operator==(const AraF0ConfigTag& rhs) const noexcept;
};

struct AraF0LocalKey {
    OriginalF0InputStamp input;
    AraF0ConfigTag config;

    bool operator==(const AraF0LocalKey& rhs) const noexcept
    {
        // Identical absolute endpoints project to the identical native PCM span.
        return input.sourceStartSeconds == rhs.input.sourceStartSeconds
            && input.sourceEndSeconds == rhs.input.sourceEndSeconds
            && input.sourceSampleRate == rhs.input.sourceSampleRate
            && input == rhs.input && config == rhs.config;
    }
};

struct AraF0Target {
    ContentKey key;
    uint64_t wrapperGeneration{0};
    uint64_t birthRevision{0};
    juce::String modificationPersistentId;
    OriginalF0InputStamp stamp;
};
using AraF0Receiver = AraF0Target;

struct AraF0Pcm {
    std::vector<float> channel0;
    double sourceSampleRate{0.0};
    int sourceChannelCount{0};
};

enum class AraF0Admission { Accepted, Attached, QueueFull, InvalidInput, Closing };
enum class AraF0Abort { SamplesDisabled, SourceUndo, SamplesChanged, Orphaned };

struct AraF0Submission {
    AraF0Admission admission;
    F0TaskId taskId;
};

struct AraF0Result {
    int hopSize{0};
    int f0SampleRate{0};
    std::vector<float> f0;
    std::vector<float> energy;
    std::string errorMessage;
};

// Both indexes and the sink belong to one DC and are accessed only on the message thread.
struct AraF0DocumentFlights {
    using CompletionSink = std::function<void(F0TaskId, const AraF0Result&)>;
    using Receivers = std::map<uint64_t, AraF0Receiver>;

    std::shared_ptr<CompletionSink> sink;
    std::vector<std::pair<AraF0LocalKey, F0TaskId>> byLocalKey;
    std::map<F0TaskId, Receivers> byTaskId;

    F0TaskId taskForReceiver(uint64_t wrapperGeneration) const;
    Receivers take(F0TaskId taskId);
};

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
        std::string errorMessage;

        double audioDurationSeconds{0.0};
        double firstAudibleTimeSeconds{-1.0};
        double firstVoicedTimeSeconds{-1.0};
        int firstVoicedFrame{-1};
        int expectedInferenceFrameCount{0};
    };

    using ExecuteFn = std::function<Result(const std::shared_ptr<F0RunOwnerState>&)>;
    using CommitFn = std::function<void(Result&&)>;

    enum class SubmitResult : uint8_t {
        Accepted,
        AlreadyInProgress,
        QueueFull,
        InvalidTask,
        ShuttingDown
    };

    F0ExtractionService(int workerCount, size_t maxQueueSize,
                        std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver);
    ~F0ExtractionService();

    SubmitResult submit(F0RequestKey requestKey, ExecuteFn execute, CommitFn commit,
                        CommitFn dispatcherFailure = {},
                        uint64_t* acceptedRequestToken = nullptr);
    bool isActive(F0RequestKey requestKey) const;
    void cancel(F0RequestKey requestKey);

    static AraF0ConfigTag snapshotAraConfig(bool allowCachedAnalysis);
    AraF0Submission submit(AraF0DocumentFlights&, const AraF0LocalKey&, AraF0Pcm, AraF0Receiver);
    bool isActive(F0TaskId taskId) const;
    void abortTask(F0TaskId taskId, AraF0Abort reason);
    void revokeReceiver(AraF0DocumentFlights&, uint64_t wrapperGeneration);

    // Stop admission, revoke outstanding work and join the workers. The process
    // ARA queue calls this only at module shutdown.
    void shutdown();

private:
    friend class ProcessF0Runtime;
    F0ExtractionService(int workerCount, size_t maxQueueSize);
    void startWorkers(int workerCount);

    struct Task {
        F0RequestKey requestKey;
        uint64_t token{0};
        ExecuteFn execute;
        CommitFn commit;
        CommitFn dispatcherFailure;
    };

    // Worker input is exclusively value PCM, frozen configuration and run identity.
    // It contains no DC, model, source, reader or receiver pointer/callback.
    struct AraTask {
        F0TaskId id;
        AraF0Pcm pcm;
        AraF0ConfigTag config;
        std::shared_ptr<F0RunOwnerState> owner;
    };
    using Job = std::variant<Task, AraTask>;

    struct AraEntry {
        std::shared_ptr<F0RunOwnerState> owner;
        std::weak_ptr<AraF0DocumentFlights::CompletionSink> sink; // Resolved only on the message thread.
        F0InferenceService* inference{nullptr}; // Published at registration; the inference service has process lifetime.
        std::shared_ptr<const AraF0Result> result;
    };

    struct SharedState {
        using Commit = F0ExtractionService::CommitFn;
        using ResultType = F0ExtractionService::Result;

        struct DispatcherFailure {
            Commit callback;
            std::shared_ptr<ResultType> result;
        };

        class DispatcherFailureMailbox final : public juce::AsyncUpdater {
        public:
            void enqueue(Commit callback, std::shared_ptr<ResultType> result);
            void cancelAndClear();
            void handleAsyncUpdate() override;

        private:
            std::mutex mutex_;
            std::deque<DispatcherFailure> queue_;
        };

        std::deque<Job> queue_;
        size_t maxQueueSize_{0};
        std::condition_variable queueCv_;
        mutable std::mutex entriesMutex_;
        uint64_t tokenCounter_{1};
        bool shutdownStarted_{false};

        // Allocated only for processor services.
        std::unordered_map<ContentKey, uint64_t> activeEntries_;
        std::shared_ptr<F0RunOwnerState> processorRunOwner_;
        std::shared_ptr<DispatcherFailureMailbox> dispatcherFailureMailbox_;
        std::map<F0TaskId, AraEntry> araEntries_;
    };

    std::shared_ptr<SharedState> state_;
    std::once_flag shutdownOnce_;
    std::vector<std::thread> workers_;
    std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver_;

    static void workerLoop(std::shared_ptr<SharedState> state);
    static void runProcessorTask(SharedState& state, Task task);
    static void runAraTask(const std::shared_ptr<SharedState>& state, AraTask task);
    static void executeAraTask(const std::shared_ptr<SharedState>& state, AraTask& task, AraF0Result& result);
    static void deliverAraResult(const std::weak_ptr<SharedState>& state, F0TaskId id);
};

} // namespace OpenTune
