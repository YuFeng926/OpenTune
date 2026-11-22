#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

namespace juce { class ARAAudioSource; }

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
    F0ModelVersion model;
    F0ExtractionConfigSnapshot extraction;
    double sourceSampleRate{0.0};

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
    using CompletionSink = std::function<void(const std::vector<F0TaskId>&, const AraF0Result&)>;
    using Receivers = std::map<uint64_t, AraF0Receiver>;

    std::shared_ptr<CompletionSink> sink;
    std::vector<std::pair<AraF0LocalKey, F0TaskId>> byLocalKey;
    std::map<F0TaskId, Receivers> byTaskId;
    // Source-object index filled by the DC on submit. Revocation matches the
    // ARAAudioSource identity because the host may rebind a live source's
    // persistentId (didUpdateAudioSourceProperties); persistentId stays an
    // archive-mapping and logging value only.
    std::map<juce::ARAAudioSource*, std::set<F0TaskId>> bySource;

    F0TaskId taskForReceiver(uint64_t wrapperGeneration) const;
    Receivers take(F0TaskId taskId);
};

class F0ExtractionService {
public:
    struct Result {
        bool success{false};
        int hopSize{0};
        int f0SampleRate{0};
        std::vector<float> f0;
        std::vector<float> energy;
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
                        CommitFn dispatcherFailure = {});
    void cancel(F0RequestKey requestKey);

    static AraF0ConfigTag snapshotAraConfig(double sourceSampleRate);
    AraF0Submission submit(AraF0DocumentFlights&, const AraF0LocalKey&, AraF0Pcm, AraF0Receiver);
    bool isActive(F0TaskId taskId) const;
    void abortTask(F0TaskId taskId, AraF0Abort reason);
    void abortTasks(const std::set<F0TaskId>& taskIds, AraF0Abort reason);
    void revokeReceiver(AraF0DocumentFlights&, uint64_t wrapperGeneration);

    // Stop admission, revoke outstanding work and join the workers. The process
    // ARA queue calls this only at module shutdown.
    void shutdown();

private:
    friend class ProcessF0Runtime;
    friend struct F0ExtractionServiceTestAccess; // Test exes construct an isolated ARA face.
    explicit F0ExtractionService(size_t maxQueueSize);

    struct ProcessorTaskId {
        F0RequestKey requestKey;
        uint64_t token{0};
    };

    struct Task {
        ProcessorTaskId id;
        ExecuteFn execute;
    };

    struct ProcessorEntry {
        uint64_t token;
        CommitFn commit;
        CommitFn dispatcherFailure;
        std::optional<Result> result;
    };
    using ProcessorEntries = std::unordered_map<ContentKey, ProcessorEntry>;

    // Worker input is exclusively value PCM, frozen configuration and run identity.
    // It contains no DC, model, source, reader or receiver pointer/callback.
    struct AraTask {
        F0TaskId id;
        AraF0Pcm pcm;
        AraF0ConfigTag config;
        std::shared_ptr<F0RunOwnerState> owner;
    };
    using AraJob = std::shared_ptr<const AraTask>;
    using Job = std::variant<Task, AraJob>;

    enum class AraGroupPhase { Queued, Running, ReadyToDeliver, Delivering, Resuming };

    struct AraGroup {
        AraF0ConfigTag config;
        F0TaskId owner;
        std::set<F0TaskId> origins;
        F0InferenceService* inference; // Owned by the process runtime.
        std::string initializationError;
        std::unique_ptr<const AraF0Result> result;
        AraGroupPhase phase{AraGroupPhase::Queued};
    };
    // The PCM SHA-256 indexes candidates; the frozen config completes the Digest.
    using AraGroups = std::multimap<juce::String, AraGroup>;

    struct AraEntry {
        std::shared_ptr<const AraTask> task;
        std::weak_ptr<AraF0DocumentFlights::CompletionSink> sink; // Resolved only on the message thread.
        std::optional<AraGroups::iterator> group;
        F0InferenceService* inference{nullptr}; // The inference service has process lifetime.
        bool hasReceivers{true};
    };

    struct SharedState {
        std::deque<Job> queue_;
        size_t maxQueueSize_{0};
        std::condition_variable queueCv_;
        mutable std::mutex entriesMutex_;
        uint64_t tokenCounter_{1};
        bool shutdownStarted_{false};

        // Processor entries stay active until the completion is claimed or revoked.
        ProcessorEntries processorEntries_;
        std::shared_ptr<F0RunOwnerState> processorRunOwner_;
        AraGroups araGroups_;
        std::map<F0TaskId, AraEntry> araEntries_;

        // Processor-private failure mailbox: a dispatcher rejection re-delivers
        // the failed result on the message thread so waiting content settles as
        // Failed with its reason. The ARA queue allocates none and keeps the
        // value-side release.
        class DispatcherFailureMailbox final : public juce::AsyncUpdater {
        public:
            void enqueue(CommitFn callback, std::shared_ptr<Result> result);
            void cancelAndClear();
            void handleAsyncUpdate() override;

        private:
            std::mutex mutex_;
            std::deque<std::pair<CommitFn, std::shared_ptr<Result>>> queue_;
        };
        std::shared_ptr<DispatcherFailureMailbox> dispatcherFailureMailbox_;
    };

    std::shared_ptr<SharedState> state_;
    std::once_flag shutdownOnce_;
    std::vector<std::thread> workers_;
    std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver_;

    template <typename Id>
    static void postCompletion(const std::shared_ptr<SharedState>& state, Id id);

    static void processorWorkerLoop(const std::shared_ptr<SharedState>& state);
    static void runProcessorTask(const std::shared_ptr<SharedState>& state, Task task);
    static ProcessorEntries::iterator findProcessorEntry(SharedState& state, ProcessorTaskId id);
    static void settleCompletion(SharedState& state, ProcessorTaskId id, bool dispatcherAvailable);
    static void prepareAraTasks(const std::shared_ptr<SharedState>& state);
    static void executeAraTasks(const std::shared_ptr<SharedState>& state);
    static void runAraTask(const std::shared_ptr<SharedState>& state, const AraTask& task,
                           const AraF0ConfigTag& frozen, F0InferenceService* inference,
                           std::string initializationError);
    static void executeAraTask(F0InferenceService& inference, const AraTask& task,
                               const AraF0ConfigTag& frozen, AraF0Result& result);
    static AraGroups::iterator findAraBatch(SharedState& state, F0TaskId owner);
    static AraGroups::node_type releaseAraBatch(SharedState& state, AraGroups::iterator group,
                                               std::vector<AraEntry>& released);
    static void settleCompletion(SharedState& state, F0TaskId owner, bool dispatcherAvailable);
};

} // namespace OpenTune
