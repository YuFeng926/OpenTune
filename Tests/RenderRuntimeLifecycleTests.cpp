#include "../Source/Render/ContentRenderService.h"
#include "../Source/Inference/VocoderInferenceService.h"
#include "../Source/Inference/VocoderRenderScheduler.h"
#include "../Source/Runtime/ProcessRenderRuntime.h"
#include "LifecycleTestTrace.h"
#include "LifecycleWatchdog.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace
{
bool checkRenderContract(bool value, const char* reason)
{
    if (! value)
        std::fprintf(stderr, "{\"event\":\"render_cache_contract\",\"result\":\"failure\",\"reason\":\"%s\"}\n", reason);
    return value;
}

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
    if (! checkRenderContract(cache.markChunkAsBlank(0, blankRevision), "first_blank_not_accepted")
        || ! checkRenderContract(! cache.markChunkAsBlank(0, blankRevision), "repeated_blank_accepted")
        || ! checkRenderContract(cache.getChunkStats().blank == 1, "blank_state_not_blank"))
        return false;

    return checkRenderContract(cache.isCanonicalSettled(), "completion_contract_not_settled");
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
}

bool runRenderCacheCapacityContract()
{
    auto& limit = OpenTune::RenderCache::renderCachePcmLimitBytes();
    const size_t oldLimit = limit.load(std::memory_order_acquire);
    const size_t baseline = OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire);
    bool passed = true;

    {
        limit.store(baseline + 1024, std::memory_order_release);
        OpenTune::RenderCache cache;
        const std::vector<OpenTune::RenderCache::PlannedChunk> firstPlan{{0, 256}};
        cache.reconcileFullPlanAndRequest(firstPlan, 0, 256, 1);
        const auto firstJob = cache.getPendingJobs();
        passed = firstJob.size() == 1 && cache.claimPendingJob(firstJob.front());
        if (passed)
        {
            std::vector<float> firstAudio(256, 1.0f);
            passed = cache.completeChunkRenderWithAudio(0, 256, std::move(firstAudio),
                                                        firstJob.front().targetRevision)
                == OpenTune::RenderCache::ChunkRenderResult::Published;
        }

        const std::vector<OpenTune::RenderCache::PlannedChunk> expandedPlan{{0, 256}, {256, 512}};
        cache.reconcileFullPlanAndRequest(expandedPlan, 0, 512, 1);
        const auto secondJob = cache.getPendingJobs();
        passed = passed && secondJob.size() == 1 && cache.claimPendingJob(secondJob.front());
        if (passed)
        {
            std::vector<float> secondAudio(256, 2.0f);
            passed = cache.completeChunkRenderWithAudio(256, 512, std::move(secondAudio),
                                                        secondJob.front().targetRevision)
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
            passed = cache.completeChunkRenderWithAudio(0, 256, std::move(audio),
                                                        job.front().targetRevision)
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
                passed = cache.completeChunkRenderWithAudio(
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
    if (!checkRenderContract(service->requeueRenderChunk(retry), "requeue_submit_failed"))
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
    if (!checkRenderContract(!service->requeueRenderChunk(retry),
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
        || OpenTune::VocoderRenderScheduler::isJobPayloadWithinLimit(payloadFloats + 1, 0, 0))
    {
        std::fprintf(stderr, "{\"event\":\"vocoder_payload_capacity\",\"result\":\"failure\"}\n");
        return 1;
    }
    if (!runRenderCacheCapacityContract())
    {
        std::fprintf(stderr, "{\"event\":\"render_cache_capacity\",\"result\":\"failure\"}\n");
        return 1;
    }
    if (! runRenderCacheCompletionContract())
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
                                                                const juce::String&) {
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

    runtime.resetVocoder(nullptr, [](OpenTune::ProcessRenderRuntime::ControlResult) {});
    std::thread firstShutdown([&runtime] {
        runtime.releaseOwner();
        juce::MessageManager::callAsync([] {
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    firstShutdown.join();
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "first_runtime_shutdown_complete", {
        {"ownerCount", OpenTuneTest::jsonNumber(runtime.ownerCount())},
        {"controlQueueDepth", OpenTuneTest::jsonNumber(static_cast<long long>(runtime.controlQueueDepth()))},
        {"deferredRetryCount", OpenTuneTest::jsonNumber(static_cast<long long>(runtime.deferredRetryCount()))},
        {"activeTransaction", runtime.hasActiveTransaction() ? "true" : "false"},
        {"domainSubmitInFlight", OpenTuneTest::jsonNumber(runtime.domainSubmitInFlight())},
        {"domainGeneration", OpenTuneTest::jsonNumber(static_cast<long long>(runtime.vocoderGeneration()))},
        {"renderQueueDepth", OpenTuneTest::jsonNumber(static_cast<long long>(service->renderQueueDepth()))},
        {"renderInFlight", OpenTuneTest::jsonNumber(service->renderInFlight())},
        {"renderAsyncInFlight", OpenTuneTest::jsonNumber(service->renderAsyncInFlight())},
        {"controlWorkerJoinable", runtime.isControlWorkerJoinable() ? "true" : "false"}
    });

    // A host may create a new instance after the previous last instance was
    // released.  The process singleton must reopen its control worker rather
    // than inherit the prior shutdown state.
    runtime.retainOwner();
    std::thread secondShutdown([&runtime] {
        runtime.releaseOwner();
        juce::MessageManager::callAsync([] {
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    secondShutdown.join();
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "second_runtime_shutdown_complete");

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
