#include "../Source/Render/ContentRenderService.h"
#include "../Source/Inference/TimeStretchCache.h"
#include "../Source/Inference/VocoderInferenceService.h"
#include "../Source/Inference/VocoderInterface.h"
#include "../Source/Inference/VocoderRenderScheduler.h"
#include "../Source/Inference/VocoderSidecarMetadata.h"
#include "../Source/Runtime/ProcessRenderRuntime.h"
#include "LifecycleTestTrace.h"
#include "LifecycleWatchdog.h"
#include "LifecycleMessagePump.h"
#include "../Source/Utils/TimeCoordinate.h"
#include <juce_events/juce_events.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace OpenTune
{
struct ProcessRenderRuntimeTestAccess
{
    static std::thread::id controlWorkerIdentity(ProcessRenderRuntime& runtime)
    {
        std::lock_guard<std::mutex> lock(runtime.shutdownMutex_);
        return runtime.controlWorker_.get_id();
    }

    /** The transaction the control worker is currently settling, or null once it published its ack. */
    static std::shared_ptr<ProcessRenderRuntime::ControlTransaction> activeControlTransaction(
        ProcessRenderRuntime& runtime)
    {
        std::lock_guard<std::mutex> lock(runtime.controlMutex_);
        return runtime.activeTransaction_;
    }

    /** A runtime instance that is independent of the process singleton. */
    static std::shared_ptr<ProcessRenderRuntime> createRuntime()
    {
        return std::shared_ptr<ProcessRenderRuntime>(new ProcessRenderRuntime(), &destroyRuntime);
    }

    static bool postAsyncDelivery(ProcessRenderRuntime& runtime, std::function<void()> delivery)
    {
        return runtime.postAsyncDelivery(std::move(delivery));
    }

private:
    static void destroyRuntime(ProcessRenderRuntime* runtime)
    {
        delete runtime;
    }
};
}

namespace
{
class DefaultVocoderCapability final : public OpenTune::VocoderInterface
{
public:
    std::vector<float> synthesize(const std::vector<float>&,
                                  const std::vector<float>&,
                                  const float*,
                                  size_t,
                                  Ort::RunOptions&) override
    {
        return {};
    }

    float getFMax() const override { return 16000.0f; }
    void setMelFMax(float) override {}
};

bool checkRenderContract(bool value, const char* reason)
{
    if (! value)
        std::fprintf(stderr, "{\"event\":\"render_cache_contract\",\"result\":\"failure\",\"reason\":\"%s\"}\n", reason);
    return value;
}

bool checkLifecycle(bool value, const char* reason)
{
    if (! value)
        std::fprintf(stderr, "{\"event\":\"lifecycle_contract\",\"result\":\"failure\",\"reason\":\"%s\"}\n", reason);
    return value;
}

/**
 * Sends accepted control commands to the shared runtime and records their worker-owned outcomes.
 *
 * The first command is posted while the caller still owns the runtime. The two follow-up commands
 * are posted after the caller released its last owner: the first one closes its completion gate
 * before it is enqueued, the second one is the open-gate barrier. After the barrier's message
 * completion returned, the settlement check reads the worker-owned transaction ack. No timer or
 * state polling is involved.
 */
class AcceptedCommandProbe
{
public:
    AcceptedCommandProbe(OpenTune::ProcessRenderRuntime& runtime,
                         std::shared_ptr<OpenTune::ContentRenderService> sourceOwner,
                         std::string weight)
        : runtime_(runtime),
          sourceOwner_(std::move(sourceOwner)),
          weight_(std::move(weight)),
          closedGate_(std::make_shared<OpenTune::ProcessRenderRuntime::CompletionGate>())
    {
    }

    void postFirstCommand()
    {
        runtime_.setVocoderModelWeight(
            weight_, nullptr,
            [this](OpenTune::ProcessRenderRuntime::ControlResult result, juce::String) {
                firstResult_.store(static_cast<int>(result), std::memory_order_release);
                firstCalls_.fetch_add(1, std::memory_order_acq_rel);
            },
            std::make_shared<OpenTune::ProcessRenderRuntime::CompletionGate>());
    }

    void postSubsequentCommands()
    {
        zeroOwnerObserved_ = runtime_.ownerCount() == 0;
        workerJoinableAtZeroOwner_ = runtime_.isControlWorkerJoinable();
        workerIdentityAtZeroOwner_ =
            OpenTune::ProcessRenderRuntimeTestAccess::controlWorkerIdentity(runtime_);

        closedGate_->closeAndWait();
        runtime_.setVocoderModelWeight(
            weight_, sourceOwner_,
            [this](OpenTune::ProcessRenderRuntime::ControlResult result, juce::String) {
                juce::ignoreUnused(result);
                closedCalls_.fetch_add(1, std::memory_order_acq_rel);
            },
            closedGate_);
        runtime_.setVocoderModelWeight(
            weight_, nullptr,
            [this](OpenTune::ProcessRenderRuntime::ControlResult result, juce::String) {
                barrierResult_.store(static_cast<int>(result), std::memory_order_release);
                barrierCalls_.fetch_add(1, std::memory_order_acq_rel);
                postSettlementCheck();
            },
            std::make_shared<OpenTune::ProcessRenderRuntime::CompletionGate>());
    }

    int firstCalls() const noexcept { return firstCalls_.load(std::memory_order_acquire); }
    int closedCalls() const noexcept { return closedCalls_.load(std::memory_order_acquire); }
    int barrierCalls() const noexcept { return barrierCalls_.load(std::memory_order_acquire); }
    int firstResult() const noexcept { return firstResult_.load(std::memory_order_acquire); }
    int barrierResult() const noexcept { return barrierResult_.load(std::memory_order_acquire); }
    bool settled() const noexcept { return settled_.load(std::memory_order_acquire); }
    bool zeroOwnerObserved() const noexcept { return zeroOwnerObserved_; }
    bool workerJoinableAtZeroOwner() const noexcept { return workerJoinableAtZeroOwner_; }
    std::thread::id workerIdentityAtZeroOwner() const noexcept { return workerIdentityAtZeroOwner_; }
    void releaseSourceOwner() { sourceOwner_.reset(); }

private:
    void postSettlementCheck()
    {
        juce::MessageManager::callAsync([this] {
            auto transaction = OpenTune::ProcessRenderRuntimeTestAccess::activeControlTransaction(runtime_);
            bool settled = true;
            if (transaction != nullptr)
            {
                std::unique_lock<std::mutex> lock(transaction->mutex);
                transaction->cv.wait(lock, [&transaction] {
                    return transaction->acked.load(std::memory_order_acquire);
                });
                settled = transaction->acked.load(std::memory_order_acquire);
            }
            settled_.store(settled, std::memory_order_release);
        });
    }

    OpenTune::ProcessRenderRuntime& runtime_;
    std::shared_ptr<OpenTune::ContentRenderService> sourceOwner_;
    std::string weight_;
    std::shared_ptr<OpenTune::ProcessRenderRuntime::CompletionGate> closedGate_;
    std::atomic<int> firstCalls_{0};
    std::atomic<int> closedCalls_{0};
    std::atomic<int> barrierCalls_{0};
    std::atomic<int> firstResult_{static_cast<int>(OpenTune::ProcessRenderRuntime::ControlResult::Failed)};
    std::atomic<int> barrierResult_{static_cast<int>(OpenTune::ProcessRenderRuntime::ControlResult::Failed)};
    std::atomic<bool> settled_{false};
    bool zeroOwnerObserved_{false};
    bool workerJoinableAtZeroOwner_{false};
    std::thread::id workerIdentityAtZeroOwner_;
};

bool runRenderCacheCompletionContract()
{
    OpenTune::RenderCache cache;
    const std::vector<float> audio(256, 1.0f);

    cache.reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
    auto jobs = cache.getPendingJobs();
    if (! checkRenderContract(jobs.size() == 1 && cache.claimPendingJob(jobs.front()), "success_claim_failed"))
        return false;
    if (! checkRenderContract(cache.completeChunkRenderWithAudio(0, 256, std::vector<float>(audio),
                                                                  jobs.front().targetRevision)
                                  == OpenTune::RenderCache::ChunkRenderResult::Published,
                              "success_completion_failed")
        || ! checkRenderContract(cache.getChunkStats().idle == 1, "success_state_not_idle"))
        return false;

    cache.reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 2);
    jobs = cache.getPendingJobs();
    if (! checkRenderContract(jobs.size() == 1 && cache.claimPendingJob(jobs.front()), "old_revision_claim_failed"))
        return false;
    if (! checkRenderContract(cache.completeChunkRenderWithAudio(0, 256, std::vector<float>(audio),
                                                                  jobs.front().targetRevision - 1)
                                  == OpenTune::RenderCache::ChunkRenderResult::Stale,
                              "old_revision_accepted")
        || ! checkRenderContract(cache.getChunkStats().running == 1, "old_revision_changed_state")
        || ! checkRenderContract(cache.completeChunkRenderWithAudio(0, 256, std::vector<float>(audio),
                                                                     jobs.front().targetRevision)
                                  == OpenTune::RenderCache::ChunkRenderResult::Published,
                                 "current_revision_completion_failed")
        || ! checkRenderContract(cache.getChunkStats().idle == 1, "current_revision_state_not_idle"))
        return false;

    cache.reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 3);
    jobs = cache.getPendingJobs();
    if (! checkRenderContract(jobs.size() == 1 && cache.claimPendingJob(jobs.front()), "failure_claim_failed"))
        return false;
    const auto failureRevision = jobs.front().targetRevision;
    if (! checkRenderContract(cache.completeChunkRenderFailure(0, failureRevision), "first_failure_not_accepted")
        || ! checkRenderContract(! cache.completeChunkRenderFailure(0, failureRevision), "repeated_failure_accepted")
        || ! checkRenderContract(cache.getChunkStats().failed == 1, "failure_state_not_failed"))
        return false;

    cache.reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 4);
    jobs = cache.getPendingJobs();
    if (! checkRenderContract(jobs.size() == 1 && cache.claimPendingJob(jobs.front()), "blank_claim_failed"))
        return false;
    const auto blankRevision = jobs.front().targetRevision;
    if (! checkRenderContract(
            cache.markChunkAsBlank(0, blankRevision) == OpenTune::RenderCache::ChunkRenderResult::Published,
            "first_blank_not_accepted")
        || ! checkRenderContract(
            cache.markChunkAsBlank(0, blankRevision) == OpenTune::RenderCache::ChunkRenderResult::Stale,
            "repeated_blank_accepted")
        || ! checkRenderContract(cache.getChunkStats().blank == 1, "blank_state_not_blank"))
        return false;

    return checkRenderContract(cache.isCanonicalSettled(), "completion_contract_not_settled");
}

bool runRenderGenerationContract()
{
    OpenTune::RenderCache cache;
    const std::vector<OpenTune::RenderCache::PlannedChunk> chunks{
        {0, 256}, {256, 512}
    };
    const std::vector<float> audio(256, 1.0f);

    const auto completePending = [&cache, &audio]() {
        const auto jobs = cache.getPendingJobs();
        for (const auto& job : jobs)
        {
            if (!cache.claimPendingJob(job)
                || cache.completeChunkRenderWithAudio(
                       job.startSample, job.endSampleExclusive,
                       std::vector<float>(audio), job.targetRevision)
                    != OpenTune::RenderCache::ChunkRenderResult::Published)
                return false;
        }
        return true;
    };

    cache.reconcileFullPlanAndRequest(chunks, 0, 512, 1);
    const auto initialJobs = cache.getPendingJobs();
    if (!checkRenderContract(initialJobs.size() == 2 && completePending(),
                             "generation_initial_render_failed"))
        return false;

    uint64_t chunkARevision = 0;
    for (uint64_t contentRevision = 2; contentRevision <= 7; ++contentRevision)
    {
        cache.reconcileFullPlanAndRequest(chunks, 0, 256, contentRevision);
        const auto jobs = cache.getPendingJobs();
        if (!checkRenderContract(jobs.size() == 1 && jobs.front().startSample == 0,
                                 "generation_chunk_a_job_missing"))
            return false;
        chunkARevision = jobs.front().targetRevision;
        if (!completePending())
            return false;
    }

    const auto generationBeforeChunkBEdit = cache.getStateSnapshot().renderGeneration;
    cache.reconcileFullPlanAndRequest(chunks, 256, 512, 8);
    const auto chunkBJobs = cache.getPendingJobs();
    if (!checkRenderContract(chunkBJobs.size() == 1
                                 && chunkBJobs.front().startSample == 256
                                 && chunkBJobs.front().targetRevision < chunkARevision,
                             "generation_chunk_local_revision_setup_failed"))
        return false;
    if (!checkRenderContract(!cache.getStateSnapshot().canonicalSettled,
                             "generation_chunk_b_not_pending"))
        return false;
    if (!completePending())
        return false;

    const auto settledState = cache.getStateSnapshot();
    if (!checkRenderContract(settledState.canonicalSettled
                                 && settledState.renderGeneration != generationBeforeChunkBEdit,
                             "generation_chunk_b_success_did_not_advance_cycle"))
        return false;

    cache.reconcileFullPlanAndRequest(chunks, 256, 512, 8);
    return checkRenderContract(
        cache.getStateSnapshot().renderGeneration == settledState.renderGeneration,
        "generation_advanced_for_noop_reconcile");
}

bool runHighFrequencyMetadataContract()
{
    DefaultVocoderCapability defaultVocoder;
    juce::StringArray absent;
    absent.add("mel_fmax: 16000");
    juce::StringArray disabled;
    disabled.add("needs_high_frequency_original: false");
    juce::StringArray enabled;
    enabled.add("needs_high_frequency_original: true");
    juce::StringArray nested;
    nested.add("model:");
    nested.add("  needs_high_frequency_original: true");

    return checkRenderContract(
        !defaultVocoder.needsHighFrequencyOriginal()
            && !OpenTune::sidecarNeedsHighFrequencyOriginal(absent)
            && !OpenTune::sidecarNeedsHighFrequencyOriginal(disabled)
            && OpenTune::sidecarNeedsHighFrequencyOriginal(enabled)
            && !OpenTune::sidecarNeedsHighFrequencyOriginal(nested),
        "high_frequency_original_metadata_contract_regressed");
}

bool runVocoderErrorContract()
{
    const std::string ortTermination =
        "Exiting due to terminate flag being set to true.";
    const auto cancelledError = OpenTune::VocoderInferenceService::mapInferenceException(
        ortTermination);
    if (!checkRenderContract(cancelledError.code == OpenTune::ErrorCode::OperationCancelled,
                             "ort_termination_not_cancelled")
        || !checkRenderContract(cancelledError.message == ortTermination,
                                "ort_termination_reason_lost")
        || !checkRenderContract(
            OpenTune::VocoderRenderScheduler::classifyInferenceError(cancelledError).result
                == OpenTune::VocoderRenderScheduler::JobResult::Cancelled,
            "cancelled_error_classification_failed")
        || !checkRenderContract(
            OpenTune::VocoderRenderScheduler::classifyInferenceError(cancelledError).reason
                == juce::String(ortTermination),
            "cancelled_error_reason_not_preserved"))
        return false;

    const auto failedError = OpenTune::VocoderInferenceService::mapInferenceException(
        "DirectML device execution failed");
    if (!checkRenderContract(failedError.code == OpenTune::ErrorCode::ModelInferenceFailed,
                             "ordinary_error_classified_cancelled")
        || !checkRenderContract(failedError.message == "DirectML device execution failed",
                                "ordinary_error_reason_lost")
        || !checkRenderContract(
            OpenTune::VocoderRenderScheduler::classifyInferenceError(failedError).result
                == OpenTune::VocoderRenderScheduler::JobResult::Failed,
            "ordinary_error_classification_failed")
        || !checkRenderContract(
            OpenTune::VocoderRenderScheduler::classifyInferenceError(failedError).reason
                == juce::String("DirectML device execution failed"),
            "ordinary_error_not_preserved_by_scheduler"))
        return false;
    return true;
}

bool runTimeStretchCacheInvalidationContract()
{
    const size_t baseline = OpenTune::RenderCache::globalCacheCurrentBytes().load(
        std::memory_order_acquire);
    OpenTune::TimeStretchCache cache;
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 924, 0};
    const uint32_t oldGeneration = cache.beginBuild(key);
    cache.invalidate(key);
    cache.store(key, std::vector<float>(256, 1.0f), 1, 1, 44100.0, oldGeneration);

    juce::AudioBuffer<float> destination(1, 256);
    destination.clear();
    const int readSamples = cache.sliceCanonicalForOutputRange(
        key, 1, 1, 0, destination, 0, 256);
    return checkRenderContract(
        readSamples == 0
            && OpenTune::RenderCache::globalCacheCurrentBytes().load(std::memory_order_acquire)
                == baseline,
        "invalidated_time_stretch_build_was_published");
}
}

bool runRenderCacheCapacityContract()
{
    auto& limit = OpenTune::RenderCache::renderCachePcmLimitBytes();
    const size_t oldLimit = limit.load(std::memory_order_acquire);
    const size_t baseline = OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire);
    constexpr size_t expectedDefaultLimit = static_cast<size_t>(6) * 1024 * 1024 * 1024;
    bool passed = checkRenderContract(
        OpenTune::RenderCache::kDefaultRenderCachePcmLimitBytes == expectedDefaultLimit
            && oldLimit == expectedDefaultLimit,
        "default_pcm_budget_not_6_gib");

    const auto completeChunk = [](OpenTune::RenderCache& cache,
                                  int64_t startSample,
                                  int64_t endSampleExclusive,
                                  uint64_t contentRevision,
                                  float sample) {
        cache.reconcileFullPlanAndRequest(
            {{startSample, endSampleExclusive}}, startSample, endSampleExclusive, contentRevision);
        const auto jobs = cache.getPendingJobs();
        if (jobs.size() != 1 || !cache.claimPendingJob(jobs.front()))
            return false;
        return cache.completeChunkRenderWithAudio(
                   startSample,
                   endSampleExclusive,
                   std::vector<float>(static_cast<size_t>(endSampleExclusive - startSample), sample),
                   jobs.front().targetRevision)
            == OpenTune::RenderCache::ChunkRenderResult::Published;
    };

    {
        limit.store(baseline + 1024, std::memory_order_release);
        OpenTune::RenderCache cache;
        const std::vector<OpenTune::RenderCache::PlannedChunk> firstPlan{{0, 256}};
        cache.reconcileFullPlanAndRequest(firstPlan, 0, 256, 1);
        const auto firstJob = cache.getPendingJobs();
        passed = passed && firstJob.size() == 1 && cache.claimPendingJob(firstJob.front());
        if (passed)
        {
            std::vector<float> firstAudio(256, 1.0f);
            passed = passed && cache.completeChunkRenderWithAudio(
                                    0, 256, std::move(firstAudio), firstJob.front().targetRevision)
                == OpenTune::RenderCache::ChunkRenderResult::Published;
        }

        const std::vector<OpenTune::RenderCache::PlannedChunk> expandedPlan{{0, 256}, {256, 512}};
        cache.reconcileFullPlanAndRequest(expandedPlan, 0, 512, 1);
        const auto secondJob = cache.getPendingJobs();
        passed = passed && secondJob.size() == 1 && cache.claimPendingJob(secondJob.front());
        if (passed)
        {
            std::vector<float> secondAudio(256, 2.0f);
            passed = passed && cache.completeChunkRenderWithAudio(
                                    256, 512, std::move(secondAudio), secondJob.front().targetRevision)
                == OpenTune::RenderCache::ChunkRenderResult::MemoryLimitExceeded;
            passed = passed && cache.completeChunkRenderFailure(
                secondJob.front().startSample, secondJob.front().targetRevision);
            const auto stats = cache.getChunkStats();
            passed = passed && stats.idle == 1 && stats.failed == 1;
        }
        passed = passed
            && OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire)
                == baseline + 1024;
    }

    passed = passed
        && OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire) == baseline;

    {
        limit.store(baseline + 8192, std::memory_order_release);
        OpenTune::RenderCache cache;
        cache.reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
        const auto job = cache.getPendingJobs();
        passed = passed && job.size() == 1 && cache.claimPendingJob(job.front());
        if (passed)
        {
            std::vector<float> audio(256, 1.0f);
            passed = passed && cache.completeChunkRenderWithAudio(
                                    0, 256, std::move(audio), job.front().targetRevision)
                == OpenTune::RenderCache::ChunkRenderResult::Published;
            const bool prepared = cache.prepareForPlaybackSampleRate(48000.0);
            const size_t preparedBytes =
                OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire);
            passed = passed && prepared && preparedBytes > baseline + 1024;

            limit.store(preparedBytes, std::memory_order_release);
            const bool rejected = !cache.prepareForPlaybackSampleRate(32000.0);
            juce::AudioBuffer<float> destination(1, 256);
            destination.clear();
            cache.overlayPreparedAudio(destination, 0, 256, 0, 48000, 1);
            passed = passed
                && OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire)
                    == preparedBytes
                && rejected
                && destination.getSample(0, 0) != 0.0f;

            limit.store(preparedBytes + 1024, std::memory_order_release);
            cache.reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 2);
            const auto rebuiltJob = cache.getPendingJobs();
            passed = passed && rebuiltJob.size() == 1 && cache.claimPendingJob(rebuiltJob.front());
            if (passed)
            {
                std::vector<float> rebuiltAudio(256, 2.0f);
                passed = passed && cache.completeChunkRenderWithAudio(
                                        0, 256, std::move(rebuiltAudio), rebuiltJob.front().targetRevision)
                    == OpenTune::RenderCache::ChunkRenderResult::MemoryLimitExceeded;
                const auto stats = cache.getChunkStats();
                passed = passed && stats.running == 1
                    && cache.completeChunkRenderFailure(0, rebuiltJob.front().targetRevision)
                    && cache.getChunkStats().failed == 1;
            }
        }
    }

    passed = passed
        && OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire) == baseline;

    {
        limit.store(baseline + 32768, std::memory_order_release);
        OpenTune::RenderCacheRegistry registry;
        const OpenTune::ContentKey removedKey{OpenTune::DomainKind::StandaloneClip, 920, 0};
        auto removedCache = registry.getOrCreate(removedKey);
        passed = checkRenderContract(
            removedCache != nullptr && completeChunk(*removedCache, 0, 256, 1, 1.0f),
            "registry_remove_pcm_setup_failed") && passed;
        passed = checkRenderContract(
            OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire)
                == baseline + 256 * sizeof(float),
            "registry_remove_pcm_not_tracked") && passed;
        registry.remove(removedKey);
        passed = checkRenderContract(
            registry.get(removedKey) == nullptr
                && OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire)
                    == baseline,
            "registry_remove_did_not_clear_pcm") && passed;

        const OpenTune::ContentKey firstClearKey{OpenTune::DomainKind::StandaloneClip, 921, 0};
        const OpenTune::ContentKey secondClearKey{OpenTune::DomainKind::StandaloneClip, 922, 0};
        auto firstCache = registry.getOrCreate(firstClearKey);
        auto secondCache = registry.getOrCreate(secondClearKey);
        passed = checkRenderContract(
            firstCache != nullptr && secondCache != nullptr
                && completeChunk(*firstCache, 0, 256, 1, 1.0f)
                && completeChunk(*secondCache, 0, 256, 1, 2.0f),
            "registry_clear_pcm_setup_failed") && passed;
        registry.clear();
        passed = checkRenderContract(
            registry.get(firstClearKey) == nullptr && registry.get(secondClearKey) == nullptr
                && OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire)
                    == baseline,
            "registry_clear_did_not_clear_pcm") && passed;

        const OpenTune::ContentKey staleKey{OpenTune::DomainKind::StandaloneClip, 923, 0};
        auto staleCache = registry.getOrCreate(staleKey);
        staleCache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
        const auto staleJobs = staleCache->getPendingJobs();
        const bool staleClaimed = staleJobs.size() == 1
            && staleCache->claimPendingJob(staleJobs.front());
        registry.remove(staleKey);
        passed = checkRenderContract(
            staleClaimed
                && staleCache->completeChunkRenderWithAudio(
                       0, 256, std::vector<float>(256, 1.0f), staleJobs.front().targetRevision)
                    == OpenTune::RenderCache::ChunkRenderResult::Stale,
            "cleared_cache_accepted_claimed_completion") && passed;
    }
    passed = checkRenderContract(
        OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire) == baseline,
        "registry_cleanup_counter_not_at_baseline") && passed;

    {
        limit.store(baseline + 65536, std::memory_order_release);
        OpenTune::RenderCache cache;
        const std::vector<OpenTune::RenderCache::PlannedChunk> plan{{0, 256}, {256, 512}};
        cache.reconcileFullPlanAndRequest(plan, 0, 512, 1);
        auto jobs = cache.getPendingJobs();
        bool ready = jobs.size() == 2;
        for (const auto& job : jobs)
        {
            ready = ready && cache.claimPendingJob(job);
            if (ready)
            {
                const float sample = job.startSample == 0 ? 1.0f : 2.0f;
                ready = cache.completeChunkRenderWithAudio(
                    job.startSample,
                    job.endSampleExclusive,
                    std::vector<float>(static_cast<size_t>(job.endSampleExclusive - job.startSample), sample),
                    job.targetRevision)
                    == OpenTune::RenderCache::ChunkRenderResult::Published;
            }
        }
        ready = ready && cache.prepareForPlaybackSampleRate(48000.0);
        passed = checkRenderContract(ready, "local_edit_prepared_setup_failed") && passed;

        if (ready)
        {
            const size_t bytesBeforeEdit =
                OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire);
            using OpenTune::PlaybackSampleGeometry::projectToSample;
            const int64_t preparedFirstEnd = projectToSample(
                static_cast<double>(256) * 48000.0 / OpenTune::RenderCache::kSampleRate);
            const int64_t preparedPlanEnd = projectToSample(
                static_cast<double>(512) * 48000.0 / OpenTune::RenderCache::kSampleRate);
            const int64_t preparedChunkStart = preparedFirstEnd;
            // Both adjacent canonical chunks intersect the other chunk's real
            // FIR support, so the local edit invalidates and rebuilds both
            // prepared chunks. Their output lengths are Q(globalEnd) spans.
            const size_t changedChunkPcmBytes =
                static_cast<size_t>(preparedFirstEnd) * sizeof(float);
            const size_t neighborChunkPcmBytes =
                static_cast<size_t>(preparedPlanEnd - preparedFirstEnd) * sizeof(float);
            const size_t expectedCurrentBytes = bytesBeforeEdit + 256 * sizeof(float)
                + changedChunkPcmBytes + neighborChunkPcmBytes;
            limit.store(expectedCurrentBytes, std::memory_order_release);

            cache.reconcileFullPlanAndRequest(plan, 0, 256, 2);
            jobs = cache.getPendingJobs();
            bool edited = jobs.size() == 1 && jobs.front().startSample == 0
                && cache.claimPendingJob(jobs.front());
            if (edited)
            {
                edited = cache.completeChunkRenderWithAudio(
                    0, 256, std::vector<float>(256, 3.0f), jobs.front().targetRevision)
                    == OpenTune::RenderCache::ChunkRenderResult::Published;
            }
            passed = checkRenderContract(
                edited, "local_edit_neighbor_prepared_rebuild_exceeded_budget") && passed;

            if (edited)
            {
                juce::AudioBuffer<float> unchangedChunk(1, 64);
                unchangedChunk.clear();
                cache.overlayPreparedAudio(
                    unchangedChunk, 0, 64, preparedChunkStart + 32, 48000, 2);
                passed = checkRenderContract(
                    unchangedChunk.getSample(0, 0) != 0.0f,
                    "neighbor_prepared_chunk_rejected_current_revision") && passed;
                passed = checkRenderContract(
                    OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire)
                        == expectedCurrentBytes,
                    "local_edit_neighbor_prepared_pcm_not_accounted") && passed;
            }
        }
        cache.clear();
    }

    passed = checkRenderContract(
        OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire) == baseline,
        "local_edit_cleanup_counter_not_at_baseline") && passed;
    limit.store(oldLimit, std::memory_order_release);
    return passed;
}

bool runRenderWorkerRequeueContract()
{
    auto service = std::make_shared<OpenTune::ContentRenderService>();
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 902, 0};
    auto cache = service->getOrCreateRenderCache(key);
    cache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
    const auto pending = cache->getPendingJobs();
    if (!checkRenderContract(pending.size() == 1 && cache->claimPendingJob(pending.front()),
                             "requeue_initial_claim_failed"))
        return false;

    std::atomic<int> claimed{0};
    int owner = 0;
    service->attachExecutionLease({&owner, [&claimed](OpenTune::RenderJob&) {
        ++claimed;
    }});

    OpenTune::RenderJob retry;
    retry.kind = OpenTune::RenderJob::Kind::Stage1Render;
    retry.contentKey = key;
    retry.renderCache = cache;
    retry.startSample = 0;
    retry.endSampleExclusive = 256;
    retry.queuedChunkStartSample = 0;
    retry.targetRevision = pending.front().targetRevision;
    if (!checkRenderContract(
            service->requeueRenderChunk(retry) == OpenTune::RenderWorker::RequeueResult::Requeued,
            "requeue_submit_failed"))
        return false;

    service->drainRenderWorker();
    const bool passed = checkRenderContract(claimed.load() == 1,
                                            "requeue_worker_not_woken")
        && checkRenderContract(cache->completeChunkRenderFailure(0, retry.targetRevision),
                               "requeue_failure_convergence_failed");
    service->detachExecutionLease(&owner);

    const OpenTune::ContentKey detachedKey{OpenTune::DomainKind::StandaloneClip, 903, 0};
    auto detachedCache = service->getOrCreateRenderCache(detachedKey);
    detachedCache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
    const auto detachedPending = detachedCache->getPendingJobs();
    if (!checkRenderContract(detachedPending.size() == 1
                                 && detachedCache->claimPendingJob(detachedPending.front()),
                             "detached_requeue_claim_failed"))
        return false;
    retry.contentKey = detachedKey;
    retry.renderCache = detachedCache;
    retry.targetRevision = detachedPending.front().targetRevision;
    if (!checkRenderContract(
                                 service->requeueRenderChunk(retry)
                                     != OpenTune::RenderWorker::RequeueResult::Requeued,
                             "detached_requeue_was_accepted")
        || !checkRenderContract(detachedCache->completeChunkRenderFailure(
                                    retry.startSample, retry.targetRevision),
                                "detached_requeue_failure_not_settleable"))
        return false;

    auto detachedSnapshot = std::make_shared<OpenTune::EditableContentSnapshot>();
    detachedSnapshot->contentRevision = 2;
    auto detachedAudio = std::make_shared<juce::AudioBuffer<float>>(1, 256);
    detachedAudio->clear();
    service->enqueueRender(OpenTune::RenderJob{
        OpenTune::RenderJob::Kind::Stage1Render, key, detachedSnapshot, cache,
        detachedAudio, 44100.0, 0, detachedAudio->getNumSamples(), -1, 2});
    const auto detachedStats = cache->getChunkStats();
    return passed
        && checkRenderContract(detachedStats.pending == 0 && detachedStats.failed == 1,
                               "enqueue_after_detach_not_failed");
}

bool runRenderWorkerStage1QueueCapacityContract()
{
    OpenTune::RenderWorker worker;
    int owner = 0;
    worker.attachExecutionLease({&owner, [](OpenTune::RenderJob& job) {
        const auto sampleCount = static_cast<size_t>(job.endSampleExclusive - job.startSample);
        job.renderCache->completeChunkRenderWithAudio(
            job.startSample,
            job.endSampleExclusive,
            std::vector<float>(sampleCount, 1.0f),
            job.targetRevision);
    }});
    worker.pause();

    constexpr std::size_t kQueuedJobs = 101;
    std::vector<std::shared_ptr<OpenTune::RenderCache>> caches;
    caches.reserve(kQueuedJobs);

    bool passed = OpenTune::RenderWorker::kMaxQueueDepth == 1000;
    for (std::size_t i = 0; i < kQueuedJobs && passed; ++i)
    {
        auto cache = std::make_shared<OpenTune::RenderCache>();
        cache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
        const auto pending = cache->getPendingJobs();
        if (!checkRenderContract(pending.size() == 1,
                                 "stage1_capacity_pending_setup_failed"))
            return false;

        OpenTune::RenderJob job;
        job.kind = OpenTune::RenderJob::Kind::Stage1Render;
        job.renderCache = cache;
        job.startSample = pending.front().startSample;
        job.endSampleExclusive = pending.front().endSampleExclusive;
        job.queuedChunkStartSample = pending.front().startSample;
        job.targetRevision = pending.front().targetRevision;
        passed = worker.enqueue(std::move(job));
        caches.push_back(std::move(cache));
    }

    passed = passed && worker.queueDepth() == kQueuedJobs;
    for (const auto& cache : caches)
    {
        const auto stats = cache->getChunkStats();
        passed = passed && stats.pending == 1 && stats.failed == 0;
    }

    worker.resume();
    worker.drain();
    for (const auto& cache : caches)
        passed = passed && cache->isCanonicalSettled();

    worker.detachExecutionLease(&owner);
    return checkRenderContract(passed, "stage1_queue_capacity_regressed");
}

/**
 * Final shutdown returns only after every accepted async delivery has run on
 * the message thread. The shutdown call below executes on the JUCE message
 * thread while no dispatch loop is running, so the three queued deliveries
 * remain in the message manager and must be dispatched by shutdownAndJoin
 * itself before it returns.
 */
bool runAsyncDeliveryShutdownContract()
{
    auto runtime = OpenTune::ProcessRenderRuntimeTestAccess::createRuntime();
    std::atomic<int> delivered{0};
    std::atomic<int> deliveredAtShutdownReturn{-1};
    std::atomic<bool> rejectedAfterShutdown{false};

    for (int i = 0; i < 3; ++i)
    {
        const bool accepted = OpenTune::ProcessRenderRuntimeTestAccess::postAsyncDelivery(
            *runtime, [&delivered] {
                delivered.fetch_add(1, std::memory_order_acq_rel);
            });
        if (!checkLifecycle(accepted, "async_delivery_not_accepted"))
        {
            runtime->shutdownAndJoin();
            return false;
        }
    }

    runtime->shutdownAndJoin();
    deliveredAtShutdownReturn.store(delivered.load(std::memory_order_acquire),
                                    std::memory_order_release);
    rejectedAfterShutdown.store(
        !OpenTune::ProcessRenderRuntimeTestAccess::postAsyncDelivery(*runtime, [] {}),
        std::memory_order_release);

    const auto deliveredAtReturn = deliveredAtShutdownReturn.load(std::memory_order_acquire);
    const bool passed = checkLifecycle(
            deliveredAtReturn == 3,
            "async_deliveries_not_drained_before_shutdown_return")
        && checkLifecycle(delivered.load(std::memory_order_acquire) == 3,
                          "async_deliveries_lost")
        && checkLifecycle(rejectedAfterShutdown.load(std::memory_order_acquire),
                          "async_delivery_accepted_after_shutdown");
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "async_delivery_shutdown_contract", {
        {"delivered", OpenTuneTest::jsonNumber(delivered.load(std::memory_order_acquire))},
        {"deliveredAtShutdownReturn", OpenTuneTest::jsonNumber(deliveredAtReturn)},
        {"lateDeliveryRejected", rejectedAfterShutdown.load(std::memory_order_acquire) ? "true" : "false"}
    });
    return passed;
}

int runChild(int argc, char** argv)
{
    juce::ignoreUnused(argc, argv);
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "process_begin", {
        {"watchdogMs", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleWatchdogApprovalTimeoutMs)},
        {"queueLimit", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleQueueApprovalLimit)}
    });
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    constexpr std::size_t payloadFloats =
        OpenTune::VocoderRenderScheduler::kMaxJobPayloadBytes / sizeof(float);
    if (!OpenTune::VocoderRenderScheduler::isJobPayloadWithinLimit(payloadFloats, 0, 0)
        || OpenTune::VocoderRenderScheduler::isJobPayloadWithinLimit(payloadFloats + 1, 0, 0)
        || !OpenTune::VocoderRenderScheduler::isJobPayloadWithinLimit(0, 0, 0,
            OpenTune::VocoderRenderScheduler::kMaxJobPayloadBytes)
        || OpenTune::VocoderRenderScheduler::isJobPayloadWithinLimit(0, 0, 0,
            OpenTune::VocoderRenderScheduler::kMaxJobPayloadBytes + 1))
    {
        std::fprintf(stderr, "{\"event\":\"vocoder_payload_capacity\",\"result\":\"failure\"}\n");
        return 1;
    }
    if (!runHighFrequencyMetadataContract())
        return 1;
    if (!runRenderCacheCapacityContract())
    {
        std::fprintf(stderr, "{\"event\":\"render_cache_capacity\",\"result\":\"failure\"}\n");
        return 1;
    }
    if (!runTimeStretchCacheInvalidationContract())
        return 1;
    if (! runRenderCacheCompletionContract())
        return 1;
    if (!runRenderGenerationContract())
        return 1;
    if (!runVocoderErrorContract())
        return 1;
    if (!runRenderWorkerRequeueContract())
        return 1;
    if (!runRenderWorkerStage1QueueCapacityContract())
        return 1;
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "render_cache_capacity_passed", {
        {"renderCacheBytes", OpenTuneTest::jsonNumber(static_cast<long long>(
            OpenTune::RenderCache::renderCacheCurrentBytes()))}
    });
    auto service = std::make_shared<OpenTune::ContentRenderService>();
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 901, 0};
    auto cache = service->getOrCreateRenderCache(key);
    const auto snapshot = std::make_shared<OpenTune::EditableContentSnapshot>();
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 256);
    audio->clear();

    auto& runtime = OpenTune::ProcessRenderRuntime::getInstance();
    runtime.retainOwner();
    const auto completionGate = std::make_shared<OpenTune::ProcessRenderRuntime::CompletionGate>();
    std::atomic<int> settled{0};
    std::atomic<int> failed{0};
    std::atomic<int> applicationFailures{0};
    OpenTune::ContentKey failedContentKey;
    juce::String failureReason;
    const auto weakService = std::weak_ptr<OpenTune::ContentRenderService>(service);
    int owner = 0;
    service->attachExecutionLease({&owner, [weakService, &runtime, completionGate, &settled,
                                            &failed, &applicationFailures, &failedContentKey,
                                            &failureReason](OpenTune::RenderJob& job) {
        OpenTuneTest::trace("RenderRuntimeLifecycleTests", "job_claim", {
            {"startSample", OpenTuneTest::jsonNumber(job.startSample)},
            {"endSampleExclusive", OpenTuneTest::jsonNumber(job.endSampleExclusive)},
            {"targetRevision", OpenTuneTest::jsonNumber(static_cast<long long>(job.targetRevision))}
        });
        auto strongService = weakService.lock();
        if (!strongService)
            return;
        OpenTune::ProcessRenderRuntime::CompletionContext completion;
        completion.gate = completionGate;
        completion.applicationFailure = [&applicationFailures](OpenTune::ContentKey,
                                                                uint64_t,
                                                                uint64_t,
                                                                const juce::String&,
                                                                std::shared_ptr<OpenTune::RenderCache>) {
            applicationFailures.fetch_add(1, std::memory_order_relaxed);
        };
        completion.chunkSettled = [&settled](OpenTune::ContentKey,
                                               std::shared_ptr<const OpenTune::EditableContentSnapshot>,
                                               std::shared_ptr<const juce::AudioBuffer<float>>,
                                               double) {
            settled.fetch_add(1, std::memory_order_relaxed);
            OpenTuneTest::trace("RenderRuntimeLifecycleTests", "message_completion_settled");
        };
        completion.chunkFailed = [&failed, &failedContentKey, &failureReason](OpenTune::ContentKey key,
                                                            const juce::String& reason) {
            failed.fetch_add(1, std::memory_order_relaxed);
            failedContentKey = key;
            failureReason = reason;
            OpenTuneTest::trace("RenderRuntimeLifecycleTests", "message_completion_failed");
        };
        runtime.processChunkRenderJob(std::move(strongService), job, false, std::move(completion));
        OpenTuneTest::trace("RenderRuntimeLifecycleTests", "job_completion_posted");
    }});
    service->enqueueRender(OpenTune::RenderJob{
        OpenTune::RenderJob::Kind::Stage1Render, key, snapshot, cache, audio, 44100.0,
        0, audio->getNumSamples(), -1, snapshot->contentRevision});
    const auto pending = cache->getPendingJobs();
    if (pending.size() != 1)
    {
        std::fprintf(stderr, "{\"event\":\"lifecycle_boundary\",\"result\":\"failure\"}\n");
        return 1;
    }

    service->drainRenderWorker();
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "render_drain_complete", {
        {"pendingChunks", OpenTuneTest::jsonNumber(static_cast<long long>(cache->getPendingJobs().size()))},
        {"canonicalSettled", cache->isCanonicalSettled() ? "true" : "false"},
        {"renderCacheBytes", OpenTuneTest::jsonNumber(static_cast<long long>(OpenTune::RenderCache::renderCacheCurrentBytes()))}
    });
    service->detachExecutionLease(&owner);
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "lease_detached");

    if (!runAsyncDeliveryShutdownContract())
        return 1;

    const auto commandWeight = runtime.getVocoderModelWeight();
    auto pauseTarget = std::make_shared<OpenTune::ContentRenderService>();
    std::atomic<int> pauseTargetJobs{0};
    int pauseTargetLeaseOwner = 0;
    pauseTarget->attachExecutionLease({&pauseTargetLeaseOwner, [&pauseTargetJobs](OpenTune::RenderJob& job) {
        pauseTargetJobs.fetch_add(1, std::memory_order_relaxed);
        if (job.renderCache != nullptr)
        {
            const auto sampleCount = static_cast<std::size_t>(job.endSampleExclusive - job.startSample);
            job.renderCache->completeChunkRenderWithAudio(
                job.startSample, job.endSampleExclusive,
                std::vector<float>(sampleCount, 1.0f), job.targetRevision);
        }
    }});

    AcceptedCommandProbe probe(runtime, pauseTarget, commandWeight);
    probe.postFirstCommand();
    runtime.releaseOwner();
    probe.postSubsequentCommands();
    bool lifecyclePassed = true;
    const bool messagesDispatched = OpenTuneTest::dispatchMessagesUntil(
        [&probe] { return probe.settled(); },
        static_cast<int>(OpenTuneTest::lifecycleWatchdogApprovalTimeoutMs - 500));
    lifecyclePassed = checkLifecycle(messagesDispatched, "control_completion_not_dispatched") && lifecyclePassed;

    const auto workerIdentityAfterCommands =
        OpenTune::ProcessRenderRuntimeTestAccess::controlWorkerIdentity(runtime);
    const auto unchanged = static_cast<int>(OpenTune::ProcessRenderRuntime::ControlResult::Unchanged);
    lifecyclePassed = checkLifecycle(probe.firstCalls() == 1,
                                     "accepted_command_before_zero_owner_not_settled") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(probe.firstResult() == unchanged,
                                     "accepted_command_before_zero_owner_changed_backend") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(probe.zeroOwnerObserved(),
                                     "owner_count_not_zero_after_release") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(probe.workerJoinableAtZeroOwner(),
                                     "control_worker_joined_at_zero_owners") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(probe.closedCalls() == 0,
                                     "closed_owner_completion_called") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(probe.barrierCalls() == 1,
                                     "post_zero_owner_command_not_settled_once") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(probe.barrierResult() == unchanged,
                                     "post_zero_owner_command_changed_backend") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(probe.settled(),
                                     "post_zero_owner_transaction_not_acked") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(runtime.isControlWorkerJoinable(),
                                     "control_worker_not_joinable_after_zero_owner_commands") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(workerIdentityAfterCommands != std::thread::id{}
                                         && probe.workerIdentityAtZeroOwner() != std::thread::id{}
                                         && workerIdentityAfterCommands == probe.workerIdentityAtZeroOwner(),
                                     "control_worker_identity_changed") && lifecyclePassed;
    lifecyclePassed = checkLifecycle(runtime.controlQueueDepth() == 0,
                                     "control_queue_not_drained") && lifecyclePassed;

    const OpenTune::ContentKey pauseKey{OpenTune::DomainKind::StandaloneClip, 905, 0};
    auto pauseCache = pauseTarget->getOrCreateRenderCache(pauseKey);
    auto pauseSnapshot = std::make_shared<OpenTune::EditableContentSnapshot>();
    auto pauseAudio = std::make_shared<juce::AudioBuffer<float>>(1, 256);
    pauseAudio->clear();
    pauseTarget->enqueueRender(OpenTune::RenderJob{
        OpenTune::RenderJob::Kind::Stage1Render, pauseKey, pauseSnapshot, pauseCache,
        pauseAudio, 44100.0, 0, pauseAudio->getNumSamples(), -1, 1});
    pauseTarget->drainRenderWorker();
    lifecyclePassed = checkLifecycle(pauseTargetJobs.load(std::memory_order_acquire) == 1,
                                     "accepted_transaction_left_source_owner_paused") && lifecyclePassed;
    pauseTarget->detachExecutionLease(&pauseTargetLeaseOwner);
    probe.releaseSourceOwner();
    pauseTarget.reset();

    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "zero_owner_runtime_persisted", {
        {"ownerCount", OpenTuneTest::jsonNumber(runtime.ownerCount())},
        {"controlWorkerJoinable", runtime.isControlWorkerJoinable() ? "true" : "false"},
        {"firstCommandCompletions", OpenTuneTest::jsonNumber(probe.firstCalls())},
        {"closedOwnerCallbacks", OpenTuneTest::jsonNumber(probe.closedCalls())},
        {"barrierCompletions", OpenTuneTest::jsonNumber(probe.barrierCalls())},
        {"transactionSettled", probe.settled() ? "true" : "false"},
        {"controlQueueDepth", OpenTuneTest::jsonNumber(static_cast<long long>(runtime.controlQueueDepth()))},
        {"deferredRetryCount", OpenTuneTest::jsonNumber(static_cast<long long>(runtime.deferredRetryCount()))},
        {"vocoderReady", runtime.isVocoderReady() ? "true" : "false"},
        {"vocoderGeneration", OpenTuneTest::jsonNumber(static_cast<long long>(runtime.vocoderGeneration()))},
        {"renderQueueDepth", OpenTuneTest::jsonNumber(static_cast<long long>(service->renderQueueDepth()))},
        {"renderInFlight", OpenTuneTest::jsonNumber(service->renderInFlight())},
        {"renderAsyncInFlight", OpenTuneTest::jsonNumber(service->renderAsyncInFlight())}
    });
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "backend_readiness_assertion_skipped", {
        {"reason", OpenTuneTest::jsonString("no real Domain factory injection or test model seam")}
    });

    if (! lifecyclePassed)
        return 1;

    service.reset();
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "owner_destructor_complete");

    const auto finalStats = cache->getChunkStats();
    if (finalStats.pending != 0
        || finalStats.running != 0
        || (!cache->isCanonicalSettled() && failed.load(std::memory_order_relaxed) == 0)
        || (failed.load(std::memory_order_relaxed) != 0
            && (failureReason.isEmpty() || failedContentKey != key))
        || applicationFailures.load(std::memory_order_relaxed)
            != failed.load(std::memory_order_relaxed))
    {
        std::fprintf(stderr, "{\"event\":\"lifecycle_boundary\",\"result\":\"failure\"}\n");
        return 1;
    }
    std::printf("{\"event\":\"lifecycle_boundary\",\"result\":\"pass\"}\n");
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "process_end");
    return 0;
}

int main(int argc, char** argv)
{
    return OpenTuneTest::run(argc, argv, "RenderRuntimeLifecycleTests", runChild);
}
