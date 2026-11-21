#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "../ARA/AraF0Types.h"
#include "../Utils/SilentGapDetector.h"
#include "../Content/ContentKey.h"

namespace OpenTune {

class F0InferenceService;
struct F0RunOwnerState;

struct F0RequestKey
{
    ContentKey contentKey;

    bool operator==(const F0RequestKey& o) const noexcept { return contentKey == o.contentKey; }
    bool operator!=(const F0RequestKey& o) const noexcept { return !(*this == o); }
};

class F0ExtractionService
{
public:
    struct Result
    {
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

    enum class SubmitResult : uint8_t
    {
        Accepted,
        AlreadyInProgress,
        QueueFull,
        InvalidTask,
        ShuttingDown
    };

    enum class WorkerTopology : uint8_t
    {
        General,
        AraPreparationExecution
    };

    F0ExtractionService(int workerCount, size_t maxQueueSize,
                        std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver,
                        WorkerTopology topology = WorkerTopology::General);
    ~F0ExtractionService();

    void shutdown();

    SubmitResult submit(F0RequestKey requestKey, ExecuteFn execute, CommitFn commit,
                        CommitFn dispatcherFailure = {},
                        uint64_t* acceptedRequestToken = nullptr);

    AraF0Submission submit(AraF0DocumentFlights& flights, const AraF0LocalKey& localKey,
                          AraF0Pcm pcm, AraF0Receiver receiver);
    bool isActive(F0TaskId taskId) const;
    void abortTask(F0TaskId taskId, AraF0Abort reason);
    void revokeReceiver(AraF0DocumentFlights& flights, uint64_t wrapperGeneration);
    void revokeTaskReceivers(AraF0DocumentFlights& flights,
                             const std::vector<F0TaskId>& taskIds);
    void detachDocument(AraF0DocumentFlights& flights);

    bool isActive(F0RequestKey requestKey) const;
    void cancel(F0RequestKey requestKey);

private:
    struct SharedState;
    std::shared_ptr<SharedState> state_;
    std::mutex shutdownMutex_;
    std::vector<std::thread> workers_;
    std::function<std::shared_ptr<F0InferenceService>()> f0ServiceResolver_;

    static void workerLoop(std::shared_ptr<SharedState> state);
    static void araPreparationLoop(std::shared_ptr<SharedState> state);
    static void araExecutionLoop(std::shared_ptr<SharedState> state);
    static void deliverAraGroup(std::shared_ptr<SharedState> state, F0TaskId groupId);
    static void releaseAraGroup(std::shared_ptr<SharedState> state, F0TaskId groupId);
};

using SharedAraF0Outcome = std::shared_ptr<const F0ExtractionService::Result>;

struct AraF0CompletionSink
{
    virtual ~AraF0CompletionSink() = default;
    virtual void complete(F0TaskId taskId, SharedAraF0Outcome outcome) = 0;
};

} // namespace OpenTune
