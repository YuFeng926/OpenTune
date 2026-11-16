#include "../Source/Render/ContentRenderService.h"
#include "../Source/Runtime/ProcessRenderRuntime.h"
#include "../Source/Utils/PitchCurve.h"
#include <juce_events/juce_events.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace OpenTune
{
struct ProcessRenderRuntimeTestAccess
{
    static void setReconfiguring(ProcessRenderRuntime& runtime, bool value)
    {
        std::lock_guard<std::mutex> lock(runtime.vocoderMutex_);
        runtime.vocoderReconfiguring_ = value;
    }

    static void setVocoderAvailable(ProcessRenderRuntime& runtime, bool available)
    {
        std::unique_ptr<VocoderDomain> retired;
        {
            std::lock_guard<std::mutex> lock(runtime.vocoderMutex_);
            runtime.vocoderReconfiguring_ = !available;
            retired = std::move(runtime.vocoderDomain_);
            if (available)
                runtime.vocoderDomain_ = std::make_unique<VocoderDomain>(nullptr);
        }
    }

    static void resume(ProcessRenderRuntime& runtime)
    {
        runtime.resumeDeferredRetries();
    }

    static bool fillRetryBudget(ProcessRenderRuntime& runtime,
                                const std::shared_ptr<RenderCache>& cache)
    {
        std::lock_guard<std::mutex> lock(runtime.vocoderMutex_);
        auto retry = std::find_if(runtime.deferredRetries_.begin(), runtime.deferredRetries_.end(),
            [&cache](const ProcessRenderRuntime::DeferredRetry& current) {
                return current.job.renderCache == cache;
            });
        if (retry == runtime.deferredRetries_.end()
            || runtime.deferredSnapshotBytes_ > ProcessRenderRuntime::kDeferredSnapshotBudgetBytes)
            return false;

        const auto remaining = ProcessRenderRuntime::kDeferredSnapshotBudgetBytes
            - runtime.deferredSnapshotBytes_;
        for (auto& allocation : retry->payloads)
        {
            const auto entry = runtime.deferredPayloads_.find(allocation.identity);
            if (entry == runtime.deferredPayloads_.end()
                || entry->second.references != 1
                || entry->second.bytes != allocation.bytes)
                continue;
            allocation.bytes += remaining;
            entry->second.bytes = allocation.bytes;
            runtime.deferredSnapshotBytes_ += remaining;
            return true;
        }
        return false;
    }

    static void defer(ProcessRenderRuntime& runtime,
                      std::shared_ptr<ContentRenderService> crs,
                      RenderJob job,
                      ProcessRenderRuntime::CompletionContext completion,
                      std::size_t snapshotBudgetBytes = ProcessRenderRuntime::kDeferredSnapshotBudgetBytes)
    {
        runtime.deferOrRequeue(std::move(crs), std::move(job), std::move(completion),
                               "test retry", snapshotBudgetBytes);
    }
};
}

namespace
{
bool check(bool value, const char* reason)
{
    if (!value)
        std::fprintf(stderr,
                     "{\"event\":\"deferred_retry_contract\",\"result\":\"failure\",\"reason\":\"%s\"}\n",
                     reason);
    return value;
}

/** Test fixtures publish 44.1 kHz native PCM: canonical and native share the
 *  same grid, so the first canonical sample is source sample zero. */
OpenTune::PlaybackReadSource makeTestPlaybackSource(
    const OpenTune::ContentKey& key,
    const std::shared_ptr<const OpenTune::EditableContentSnapshot>& content,
    const std::shared_ptr<const juce::AudioBuffer<float>>& audio,
    const std::shared_ptr<OpenTune::RenderCache>& cache,
    OpenTune::TimeStretchCache& timeStretchCache)
{
    return OpenTune::makePlaybackReadSource(
        key, content, OpenTune::NativePcmSource{audio, 44100.0, 0, 0},
        audio, 44100.0, 0, cache, timeStretchCache);
}

bool runDeferredRetryContract()
{
    using namespace OpenTune;
    auto& runtime = ProcessRenderRuntime::getInstance();
    auto service = std::make_shared<ContentRenderService>();
    auto snapshot = std::make_shared<EditableContentSnapshot>();
    snapshot->contentRevision = 1;
    snapshot->audioRevision = 7;
    snapshot->timeGrid = TimeGridSnapshot::makeIdentity(1.0);
    snapshot->notes.reserve(4);

    std::vector<float> originalF0(512, 220.0f);
    std::vector<float> originalEnergy(512, 1.0f);
    PitchCorrectionSegment correction{};
    correction.startFrame = 0;
    correction.endFrame = 512;
    correction.f0Data.assign(512, 440.0f);
    correction.baseF0Data.assign(512, 220.0f);
    snapshot->pitchCurve = std::make_shared<const PitchCurveSnapshot>(
        std::move(originalF0), std::move(originalEnergy),
        std::vector<PitchCorrectionSegment>{std::move(correction)}, 512, 16000.0);

    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 44100);
    audio->clear();
    auto gate = std::make_shared<ProcessRenderRuntime::CompletionGate>();
    std::vector<std::shared_ptr<RenderCache>> caches;
    std::vector<ContentKey> keys;

    const auto deferRunningChunk = [&](ContentKey key,
                                       const std::shared_ptr<RenderCache>& cache,
                                       std::shared_ptr<const EditableContentSnapshot> content,
                                       std::size_t snapshotBudgetBytes) {
        RenderCache::RunningChunkSnapshot running;
        if (!cache->getRunningChunk(0, running))
        {
            const auto pending = cache->getPendingJobs();
            if (pending.size() != 1 || !cache->claimPendingJob(pending.front())
                || !cache->getRunningChunk(0, running))
                return false;
        }
        service->publishPlaybackSource(
            key, makeTestPlaybackSource(key, content, audio,
                                        cache, service->getTimeStretchCache()));
        RenderJob job;
        job.kind = RenderJob::Kind::Stage1Render;
        job.contentKey = key;
        job.contentSnapshot = std::move(content);
        job.renderCache = cache;
        job.audioBuffer = audio;
        job.audioSampleRate = 44100.0;
        job.startSample = running.startSample;
        job.endSampleExclusive = running.endSampleExclusive;
        job.queuedChunkStartSample = running.startSample;
        job.contentRevision = job.contentSnapshot->contentRevision;
        job.targetRevision = running.runningRevision;
        ProcessRenderRuntime::CompletionContext completion;
        completion.gate = gate;
        completion.contentRevision = job.contentRevision;
        completion.renderCache = cache;
        ProcessRenderRuntimeTestAccess::defer(
            runtime, service, std::move(job), std::move(completion), snapshotBudgetBytes);
        return true;
    };

    const auto baselineCount = runtime.deferredRetryCount();
    const auto baselinePayloadBytes = runtime.deferredRetrySnapshotBytes();
    std::size_t firstPayloadBytes = 0;
    std::size_t expectedPerSnapshotBytes = 0;
    ProcessRenderRuntimeTestAccess::setReconfiguring(runtime, true);
    constexpr std::size_t deferredJobs = 102;
    for (std::size_t i = 0; i < deferredJobs; ++i)
    {
        ContentKey key{DomainKind::StandaloneClip, 7000u + static_cast<uint64_t>(i), 0};
        auto cache = service->getOrCreateRenderCache(key);
        cache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
        auto jobSnapshot = std::make_shared<EditableContentSnapshot>(*snapshot);
        jobSnapshot->notes.reserve(4);
        if (i == 0)
            expectedPerSnapshotBytes = jobSnapshot->notes.capacity() * sizeof(Note)
                + jobSnapshot->originalF0FailureReason.capacity() + 1;
        if (!deferRunningChunk(key, cache, jobSnapshot,
                               ProcessRenderRuntime::kDeferredSnapshotBudgetBytes))
            return check(false, "running_chunk_setup_failed");
        keys.push_back(key);
        caches.push_back(std::move(cache));
        if (i == 0)
            firstPayloadBytes = runtime.deferredRetrySnapshotBytes() - baselinePayloadBytes;
    }

    const auto sharedPayloadBytes = runtime.deferredRetrySnapshotBytes() - baselinePayloadBytes;
    const auto& curve = *snapshot->pitchCurve;
    const auto& curveSegments = curve.getCorrectionSegments();
    const std::size_t expectedSharedBytes =
        static_cast<std::size_t>(audio->getNumChannels())
            * static_cast<std::size_t>(audio->getNumSamples()) * sizeof(float)
        + curve.getOriginalF0().capacity() * sizeof(float)
        + curve.getOriginalEnergy().capacity() * sizeof(float)
        + curve.getErasedRanges().capacity() * sizeof(F0FrameRange)
        + curveSegments.capacity() * sizeof(PitchCorrectionSegment)
        + curveSegments.front().f0Data.capacity() * sizeof(float)
        + curveSegments.front().baseF0Data.capacity() * sizeof(float)
        + curve.getVocoderMaskPayloadBytes()
        + snapshot->timeGrid->handles().capacity() * sizeof(TimeHandle);
    const std::size_t expectedBytes = expectedSharedBytes
        + deferredJobs * expectedPerSnapshotBytes;

    bool allRunning = true;
    for (const auto& cache : caches)
    {
        const auto stats = cache->getChunkStats();
        allRunning = allRunning && stats.running == 1 && stats.failed == 0;
    }
    if (!check(runtime.deferredRetryCount() == baselineCount + deferredJobs,
               "more_than_100_deferred_jobs_not_retained")
        || !check(allRunning, "deferred_jobs_failed_running_chunks")
        || !check(ProcessRenderRuntime::kDeferredSnapshotBudgetBytes
                      == static_cast<std::size_t>(6) * 1024 * 1024 * 1024,
                  "deferred_budget_is_not_6_gib")
        || !check(curve.getVocoderMaskPayloadBytes() > 0,
                  "pitch_curve_mask_has_no_counted_payload")
        || !check(firstPayloadBytes == expectedSharedBytes + expectedPerSnapshotBytes,
                  "first_snapshot_payload_footprint_mismatch")
        || !check(sharedPayloadBytes == expectedBytes,
                  "snapshot_payload_dedup_or_mask_footprint_mismatch"))
        return false;

    auto replacement = std::make_shared<EditableContentSnapshot>(*snapshot);
    replacement->notes.reserve(64);
    const auto replacementKey = keys.back();
    service->publishPlaybackSource(
        replacementKey,
        makeTestPlaybackSource(replacementKey, replacement, audio,
                               caches.back(), service->getTimeStretchCache()));
    if (!deferRunningChunk(replacementKey, caches.back(), replacement,
                           ProcessRenderRuntime::kDeferredSnapshotBudgetBytes)
        || !check(runtime.deferredRetryCount() == baselineCount + deferredJobs,
                  "same_revision_retry_not_replaced")
        || !check(runtime.deferredRetrySnapshotBytes() > baselinePayloadBytes + sharedPayloadBytes,
                  "same_revision_snapshot_payload_not_updated"))
        return false;

    ContentKey descriptorKey{DomainKind::StandaloneClip, 8000u, 0};
    auto descriptorCache = service->getOrCreateRenderCache(descriptorKey);
    descriptorCache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
    auto descriptorSnapshot = std::make_shared<EditableContentSnapshot>(*snapshot);
    descriptorSnapshot->notes.reserve(256);
    const auto bytesBeforeDescriptor = runtime.deferredRetrySnapshotBytes();
    const auto countBeforeDescriptor = runtime.deferredRetryCount();
    if (!deferRunningChunk(descriptorKey, descriptorCache, descriptorSnapshot, 1)
        || !check(runtime.deferredRetryCount() == countBeforeDescriptor + 1,
                  "over_budget_descriptor_not_retained")
        || !check(runtime.deferredRetrySnapshotBytes() == bytesBeforeDescriptor,
                  "over_budget_snapshot_payload_not_released"))
        return false;

    const auto descriptorStats = descriptorCache->getChunkStats();
    RenderJob restoredDescriptor;
    restoredDescriptor.kind = RenderJob::Kind::Stage1Render;
    restoredDescriptor.contentKey = descriptorKey;
    restoredDescriptor.renderCache = descriptorCache;
    restoredDescriptor.startSample = 0;
    restoredDescriptor.endSampleExclusive = 256;
    restoredDescriptor.targetRevision = 1;
    restoredDescriptor.contentRevision = 1;
    restoredDescriptor.audioSampleRate = 44100.0;
    if (!check(descriptorStats.running == 1 && descriptorStats.failed == 0,
               "over_budget_deferred_chunk_failed")
        || !check(service->restoreDeferredRenderJob(restoredDescriptor, 7, 44100, 1)
                      == ContentRenderService::DeferredSourceStatus::Ready
                      && restoredDescriptor.contentSnapshot != nullptr
                      && restoredDescriptor.audioBuffer == audio,
                  "descriptor_did_not_restore_from_current_source"))
        return false;

    caches.front()->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 2);
    ProcessRenderRuntimeTestAccess::setReconfiguring(runtime, false);
    runtime.pruneStaleDeferredJobsForCache(caches.front());
    const auto prunedStats = caches.front()->getChunkStats();
    if (!check(runtime.deferredRetryCount() == countBeforeDescriptor,
               "new_plan_did_not_prune_stale_deferred_job")
        || !check(prunedStats.pending == 1 && prunedStats.failed == 0,
                  "new_plan_pruning_failed_valid_chunk"))
        return false;

    service->removeRenderCache(keys[1]);
    const auto removedStats = caches[1]->getChunkStats();
    if (!check(runtime.deferredRetryCount() == countBeforeDescriptor - 1,
               "cache_removal_did_not_release_deferred_job")
        || !check(removedStats.total() == 0 && removedStats.failed == 0,
                  "cache_removal_failed_to_clear_chunk_state"))
        return false;

    RenderJob lateCompletion;
    lateCompletion.kind = RenderJob::Kind::Stage1Render;
    lateCompletion.contentKey = keys[1];
    lateCompletion.contentSnapshot = snapshot;
    lateCompletion.renderCache = caches[1];
    lateCompletion.audioBuffer = audio;
    lateCompletion.audioSampleRate = 44100.0;
    lateCompletion.startSample = 0;
    lateCompletion.endSampleExclusive = 256;
    lateCompletion.queuedChunkStartSample = 0;
    lateCompletion.contentRevision = snapshot->contentRevision;
    lateCompletion.targetRevision = 1;
    ProcessRenderRuntime::CompletionContext lateContext;
    lateContext.gate = gate;
    ProcessRenderRuntimeTestAccess::defer(runtime, service, std::move(lateCompletion),
                                          std::move(lateContext));
    if (!check(runtime.deferredRetryCount() == countBeforeDescriptor - 1,
               "late_defer_after_cache_removal_was_retained"))
        return false;

    service->clearAll();
    ProcessRenderRuntimeTestAccess::setReconfiguring(runtime, false);
    const bool cleared = check(runtime.deferredRetryCount() == baselineCount,
                               "clear_all_did_not_release_deferred_jobs")
        && check(runtime.deferredRetrySnapshotBytes() == baselinePayloadBytes,
                 "clear_all_did_not_release_snapshot_budget");
    service.reset();
    return cleared;
}

bool runDeferredBatchCapacityContract()
{
    using namespace OpenTune;
    RenderWorker worker;
    int owner = 0;
    worker.attachExecutionLease({&owner, [](RenderJob& job) {
        if (job.kind == RenderJob::Kind::Stage1Render)
            job.renderCache->completeChunkRenderWithAudio(
                job.startSample, job.endSampleExclusive, std::vector<float>(256, 1.0f),
                job.targetRevision);
    }});
    worker.pause();

    bool passed = true;
    for (std::size_t i = 0; i < RenderWorker::kMaxQueueDepth && passed; ++i)
    {
        RenderJob queued;
        queued.kind = RenderJob::Kind::Stage2Rebuild;
        passed = worker.enqueue(std::move(queued));
    }

    std::vector<std::shared_ptr<RenderCache>> caches;
    std::vector<RenderJob> retries;
    for (int i = 0; i < 2 && passed; ++i)
    {
        auto cache = std::make_shared<RenderCache>();
        cache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
        const auto pending = cache->getPendingJobs();
        passed = pending.size() == 1 && cache->claimPendingJob(pending.front());
        if (!passed)
            break;
        RenderJob job;
        job.kind = RenderJob::Kind::Stage1Render;
        job.renderCache = cache;
        job.startSample = pending.front().startSample;
        job.endSampleExclusive = pending.front().endSampleExclusive;
        job.queuedChunkStartSample = pending.front().startSample;
        job.targetRevision = pending.front().targetRevision;
        retries.push_back(std::move(job));
        caches.push_back(std::move(cache));
    }

    const auto initial = worker.requeueStage1Chunks(retries);
    passed = passed && initial.size() == retries.size()
        && initial[0] == RenderWorker::RequeueResult::QueueFull
        && initial[1] == RenderWorker::RequeueResult::QueueFull;
    for (const auto& cache : caches)
    {
        const auto stats = cache->getChunkStats();
        passed = passed && stats.running == 1 && stats.failed == 0;
    }

    std::vector<RenderJob> pendingRetries = retries;
    std::mutex pendingMutex;
    std::atomic<int> capacityEvents{0};
    worker.setStage1CapacityCallback([&] {
        ++capacityEvents;
        std::lock_guard<std::mutex> lock(pendingMutex);
        const auto results = worker.requeueStage1Chunks(pendingRetries);
        std::vector<RenderJob> remaining;
        for (std::size_t i = 0; i < results.size(); ++i)
        {
            if (results[i] == RenderWorker::RequeueResult::QueueFull
                || results[i] == RenderWorker::RequeueResult::LeaseUnavailable)
                remaining.push_back(std::move(pendingRetries[i]));
        }
        pendingRetries = std::move(remaining);
    });

    worker.resume();
    worker.drain();
    passed = passed && capacityEvents.load() > 0 && pendingRetries.empty();
    for (const auto& cache : caches)
        passed = passed && cache->isCanonicalSettled();
    worker.detachExecutionLease(&owner);
    return check(passed, "queue_full_retry_not_resumed_exactly_once");
}

bool runDeferredDescriptorBudgetResumeContract()
{
    using namespace OpenTune;
    auto& runtime = ProcessRenderRuntime::getInstance();
    auto service = std::make_shared<ContentRenderService>();
    const auto baselineCount = runtime.deferredRetryCount();
    const auto baselineBytes = runtime.deferredRetrySnapshotBytes();
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 44100);
    audio->clear();
    auto gate = std::make_shared<ProcessRenderRuntime::CompletionGate>();
    int owner = 0;
    service->attachExecutionLease({&owner, [](RenderJob&) {}});
    service->pauseRenderWorker();
    ProcessRenderRuntimeTestAccess::setReconfiguring(runtime, true);

    const auto makeDeferredChunk = [&](uint64_t id, std::size_t noteCapacity,
                                       std::size_t budgetBytes,
                                       std::shared_ptr<RenderCache>& cacheOut) {
        ContentKey key{DomainKind::StandaloneClip, id, 0};
        cacheOut = service->getOrCreateRenderCache(key);
        cacheOut->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
        const auto pending = cacheOut->getPendingJobs();
        if (pending.size() != 1 || !cacheOut->claimPendingJob(pending.front()))
            return false;

        auto content = std::make_shared<EditableContentSnapshot>();
        content->contentRevision = 1;
        content->audioRevision = 7;
        content->timeGrid = TimeGridSnapshot::makeIdentity(1.0);
        content->notes.reserve(noteCapacity);
        service->publishPlaybackSource(
            key, makeTestPlaybackSource(key, content, audio,
                                        cacheOut, service->getTimeStretchCache()));

        RenderJob job;
        job.kind = RenderJob::Kind::Stage1Render;
        job.contentKey = key;
        job.contentSnapshot = std::move(content);
        job.renderCache = cacheOut;
        job.audioBuffer = audio;
        job.audioSampleRate = 44100.0;
        job.startSample = pending.front().startSample;
        job.endSampleExclusive = pending.front().endSampleExclusive;
        job.queuedChunkStartSample = pending.front().startSample;
        job.targetRevision = pending.front().targetRevision;
        ProcessRenderRuntime::CompletionContext completion;
        completion.gate = gate;
        completion.renderCache = cacheOut;
        ProcessRenderRuntimeTestAccess::defer(
            runtime, service, std::move(job), std::move(completion), budgetBytes);
        return true;
    };

    std::shared_ptr<RenderCache> descriptorCache;
    std::shared_ptr<RenderCache> pressureCache;
    bool passed = makeDeferredChunk(9101, 256, 1, descriptorCache)
        && makeDeferredChunk(9102, 4, ProcessRenderRuntime::kDeferredSnapshotBudgetBytes,
                             pressureCache)
        && ProcessRenderRuntimeTestAccess::fillRetryBudget(runtime, pressureCache);
    if (!passed)
    {
        service->clearAll();
        service->detachExecutionLease(&owner);
        return check(false, "resume_budget_pressure_setup_failed");
    }
    ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, true);

    passed = check(runtime.deferredRetrySnapshotBytes()
                       == ProcessRenderRuntime::kDeferredSnapshotBudgetBytes,
                   "resume_budget_pressure_setup_failed");
    ProcessRenderRuntimeTestAccess::resume(runtime);
    const auto descriptorHeldStats = descriptorCache->getChunkStats();
    passed = check(passed
                       && runtime.deferredRetryCount() == baselineCount + 1
                       && runtime.deferredRetrySnapshotBytes()
                           <= ProcessRenderRuntime::kDeferredSnapshotBudgetBytes
                       && service->renderQueueDepth() == 1
                       && descriptorHeldStats.running == 1
                       && descriptorHeldStats.failed == 0,
                   "over_budget_descriptor_was_requeued_or_failed");

    ProcessRenderRuntimeTestAccess::resume(runtime);
    const auto descriptorQueuedStats = descriptorCache->getChunkStats();
    passed = check(passed
                       && runtime.deferredRetryCount() == baselineCount
                       && runtime.deferredRetrySnapshotBytes() == baselineBytes
                       && service->renderQueueDepth() == 2
                       && descriptorQueuedStats.pending == 1
                       && descriptorQueuedStats.failed == 0,
                   "descriptor_did_not_requeue_after_budget_was_released");

    service->clearAll();
    service->detachExecutionLease(&owner);
    ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, false);
    service.reset();
    return passed;
}

bool runPlanNotSynchronizedRetryContract()
{
    using namespace OpenTune;
    auto& runtime = ProcessRenderRuntime::getInstance();
    auto service = std::make_shared<ContentRenderService>();
    const auto baselineCount = runtime.deferredRetryCount();
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 2048);
    audio->clear();
    auto oldSnapshot = std::make_shared<EditableContentSnapshot>();
    oldSnapshot->contentRevision = 1;
    oldSnapshot->audioRevision = 7;
    oldSnapshot->timeGrid = TimeGridSnapshot::makeIdentity(1.0);
    oldSnapshot->silentGaps.push_back({896, 1152, -60.0f});
    auto newSnapshot = std::make_shared<EditableContentSnapshot>(*oldSnapshot);
    newSnapshot->contentRevision = 2;

    const ContentKey key{DomainKind::StandaloneClip, 9201u, 0};
    auto cache = service->getOrCreateRenderCache(key);
    cache->reconcileFullPlanAndRequest({{0, 1024}, {1024, 2048}}, 0, 1024, 1);
    const auto pending = cache->getPendingJobs();
    const auto setup = std::find_if(pending.begin(), pending.end(),
        [](const auto& pendingJob) { return pendingJob.startSample == 0; });
    if (!check(setup != pending.end() && cache->claimPendingJob(*setup),
               "plan_sync_running_chunk_setup_failed"))
        return false;

    service->publishPlaybackSource(
        key, makeTestPlaybackSource(key, oldSnapshot, audio,
                                    cache, service->getTimeStretchCache()));
    service->publishPlaybackSource(
        key, makeTestPlaybackSource(key, newSnapshot, audio,
                                    cache, service->getTimeStretchCache()));

    RenderJob job;
    job.kind = RenderJob::Kind::Stage1Render;
    job.contentKey = key;
    job.contentSnapshot = oldSnapshot;
    job.renderCache = cache;
    job.audioBuffer = audio;
    job.audioSampleRate = 44100.0;
    job.startSample = setup->startSample;
    job.endSampleExclusive = setup->endSampleExclusive;
    job.queuedChunkStartSample = setup->startSample;
    job.contentRevision = oldSnapshot->contentRevision;
    job.targetRevision = setup->targetRevision;

    auto descriptor = job;
    if (!check(service->restoreDeferredRenderJob(descriptor, 6, 1, 2)
                   == ContentRenderService::DeferredSourceStatus::PlanNotSynchronized,
               "unsynchronized_plan_was_classified_stale"))
        return false;

    int owner = 0;
    service->attachExecutionLease({&owner, [](RenderJob&) {}});
    service->pauseRenderWorker();
    auto gate = std::make_shared<ProcessRenderRuntime::CompletionGate>();
    ProcessRenderRuntime::CompletionContext completion;
    completion.gate = gate;
    completion.renderCache = cache;
    ProcessRenderRuntimeTestAccess::setReconfiguring(runtime, true);
    ProcessRenderRuntimeTestAccess::defer(runtime, service, job, std::move(completion));
    auto stats = cache->getChunkStats();
    if (!check(runtime.deferredRetryCount() == baselineCount + 1
                   && stats.running == 1 && stats.failed == 0,
               "defer_dropped_unsynchronized_running_chunk"))
        return false;

    ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, true);
    ProcessRenderRuntimeTestAccess::resume(runtime);
    stats = cache->getChunkStats();
    if (!check(runtime.deferredRetryCount() == baselineCount + 1
                   && stats.running == 1 && stats.failed == 0
                   && service->renderQueueDepth() == 0,
               "resume_consumed_unsynchronized_running_chunk"))
        return false;

    RenderJob reconcileJob;
    reconcileJob.kind = RenderJob::Kind::Stage1Render;
    reconcileJob.contentKey = key;
    reconcileJob.contentSnapshot = newSnapshot;
    reconcileJob.renderCache = cache;
    reconcileJob.audioBuffer = audio;
    reconcileJob.audioSampleRate = 44100.0;
    reconcileJob.startSample = 1024;
    reconcileJob.endSampleExclusive = 2048;
    service->enqueueRender(std::move(reconcileJob));
    RenderCache::RunningChunkSnapshot running;
    if (!check(cache->getRunningChunk(0, running)
                   && running.lastRequestedContentRevision == newSnapshot->contentRevision,
               "nonintersecting_reconcile_invalidated_running_chunk"))
        return false;

    ProcessRenderRuntimeTestAccess::resume(runtime);
    stats = cache->getChunkStats();
    const auto ready = cache->getPendingJobs();
    const bool resumed = check(runtime.deferredRetryCount() == baselineCount
                                   && stats.running == 0 && stats.failed == 0
                                   && std::any_of(ready.begin(), ready.end(), [](const auto& item) {
                                          return item.startSample == 0;
                                      }),
                               "synchronized_running_chunk_was_not_requeued");
    service->clearAll();
    service->detachExecutionLease(&owner);
    ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, false);
    service.reset();
    return resumed;
}

bool runExportDrainDeferredBatchContract()
{
    using namespace OpenTune;
    auto& runtime = ProcessRenderRuntime::getInstance();
    auto service = std::make_shared<ContentRenderService>();
    const auto baseline = runtime.deferredRetryCount();
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 256);
    audio->clear();
    auto snapshot = std::make_shared<EditableContentSnapshot>();
    snapshot->contentRevision = 1;
    snapshot->audioRevision = 7;
    snapshot->timeGrid = TimeGridSnapshot::makeIdentity(1.0);
    auto gate = std::make_shared<ProcessRenderRuntime::CompletionGate>();
    auto otherService = std::make_shared<ContentRenderService>();
    int otherOwner = 0;
    otherService->attachExecutionLease({&otherOwner, [](RenderJob& job) {
        const auto sampleCount = static_cast<std::size_t>(
            job.endSampleExclusive - job.startSample);
        job.renderCache->completeChunkRenderWithAudio(
            job.startSample, job.endSampleExclusive,
            std::vector<float>(sampleCount, 0.5f), job.targetRevision);
    }});
    otherService->pauseRenderWorker();
    const ContentKey otherKey{DomainKind::StandaloneClip, 9399, 0};
    auto otherCache = otherService->getOrCreateRenderCache(otherKey);
    otherCache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
    const auto otherPending = otherCache->getPendingJobs();
    if (otherPending.size() != 1 || !otherCache->claimPendingJob(otherPending.front()))
        return check(false, "export_scope_running_chunk_setup_failed");
    otherService->publishPlaybackSource(
        otherKey, makeTestPlaybackSource(otherKey, snapshot, audio,
                                         otherCache, otherService->getTimeStretchCache()));
    RenderJob otherJob;
    otherJob.kind = RenderJob::Kind::Stage1Render;
    otherJob.contentKey = otherKey;
    otherJob.contentSnapshot = snapshot;
    otherJob.renderCache = otherCache;
    otherJob.audioBuffer = audio;
    otherJob.audioSampleRate = 44100.0;
    otherJob.startSample = otherPending.front().startSample;
    otherJob.endSampleExclusive = otherPending.front().endSampleExclusive;
    otherJob.queuedChunkStartSample = otherPending.front().startSample;
    otherJob.contentRevision = snapshot->contentRevision;
    otherJob.targetRevision = otherPending.front().targetRevision;
    ProcessRenderRuntime::CompletionContext otherCompletion;
    otherCompletion.gate = gate;
    otherCompletion.renderCache = otherCache;

    int owner = 0;
    service->attachExecutionLease({&owner, [](RenderJob& job) {
        const auto sampleCount = static_cast<std::size_t>(
            job.endSampleExclusive - job.startSample);
        job.renderCache->completeChunkRenderWithAudio(
            job.startSample, job.endSampleExclusive,
            std::vector<float>(sampleCount, 0.5f), job.targetRevision);
    }});

    ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, false);
    ProcessRenderRuntimeTestAccess::defer(
        runtime, otherService, std::move(otherJob), std::move(otherCompletion));
    constexpr std::size_t deferredJobs = RenderWorker::kMaxQueueDepth + 1;
    std::vector<std::shared_ptr<RenderCache>> caches;
    caches.reserve(deferredJobs);
    for (std::size_t i = 0; i < deferredJobs; ++i)
    {
        const ContentKey key{
            DomainKind::StandaloneClip, 9400u + static_cast<uint64_t>(i), 0};
        auto cache = service->getOrCreateRenderCache(key);
        cache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
        const auto pending = cache->getPendingJobs();
        if (pending.size() != 1 || !cache->claimPendingJob(pending.front()))
            return check(false, "export_batch_running_chunk_setup_failed");

        service->publishPlaybackSource(
            key, makeTestPlaybackSource(key, snapshot, audio,
                                        cache, service->getTimeStretchCache()));
        RenderJob job;
        job.kind = RenderJob::Kind::Stage1Render;
        job.contentKey = key;
        job.contentSnapshot = snapshot;
        job.renderCache = cache;
        job.audioBuffer = audio;
        job.audioSampleRate = 44100.0;
        job.startSample = pending.front().startSample;
        job.endSampleExclusive = pending.front().endSampleExclusive;
        job.queuedChunkStartSample = pending.front().startSample;
        job.contentRevision = snapshot->contentRevision;
        job.targetRevision = pending.front().targetRevision;
        ProcessRenderRuntime::CompletionContext completion;
        completion.gate = gate;
        completion.renderCache = cache;
        ProcessRenderRuntimeTestAccess::defer(
            runtime, service, std::move(job), std::move(completion));
        caches.push_back(std::move(cache));
    }

    ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, true);
    const bool drained = service->drainRenderWorkerForExport();
    bool allSettled = true;
    for (const auto& cache : caches)
        allSettled = allSettled && cache->isCanonicalSettled();
    const auto otherStats = otherCache->getChunkStats();
    const bool completed = check(drained && allSettled
                                     && !runtime.hasDeferredRetriesForService(service.get())
                                     && runtime.hasDeferredRetriesForService(otherService.get())
                                     && otherStats.running == 1 && otherStats.failed == 0
                                     && runtime.deferredRetryCount() == baseline + 1,
                                 "export_drain_dropped_or_failed_deferred_batch");
    service->clearAll();
    service->detachExecutionLease(&owner);
    otherService->clearAll();
    otherService->resumeRenderWorker();
    otherService->detachExecutionLease(&otherOwner);
    ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, false);
    service.reset();
    otherService.reset();
    return completed;
}

bool runExportDrainNoProgressContracts()
{
    using namespace OpenTune;
    auto& runtime = ProcessRenderRuntime::getInstance();
    enum class Blocker { Domain, Source, PlanSync, Lease };
    const auto runBlockedExport = [&](Blocker blocker, uint64_t contentId) {
        auto service = std::make_shared<ContentRenderService>();
        auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 256);
        audio->clear();
        auto snapshot = std::make_shared<EditableContentSnapshot>();
        snapshot->contentRevision = 1;
        snapshot->audioRevision = 7;
        snapshot->timeGrid = TimeGridSnapshot::makeIdentity(1.0);
        auto gate = std::make_shared<ProcessRenderRuntime::CompletionGate>();
        const ContentKey key{DomainKind::StandaloneClip, contentId, 0};
        auto cache = service->getOrCreateRenderCache(key);
        cache->reconcileFullPlanAndRequest({{0, 256}}, 0, 256, 1);
        const auto pending = cache->getPendingJobs();
        if (pending.size() != 1 || !cache->claimPendingJob(pending.front()))
            return check(false, "blocked_export_running_chunk_setup_failed");

        service->publishPlaybackSource(
            key, makeTestPlaybackSource(key, snapshot, audio,
                                        cache, service->getTimeStretchCache()));
        RenderJob job;
        job.kind = RenderJob::Kind::Stage1Render;
        job.contentKey = key;
        job.contentSnapshot = snapshot;
        job.renderCache = cache;
        job.audioBuffer = audio;
        job.audioSampleRate = 44100.0;
        job.startSample = pending.front().startSample;
        job.endSampleExclusive = pending.front().endSampleExclusive;
        job.queuedChunkStartSample = pending.front().startSample;
        job.contentRevision = snapshot->contentRevision;
        job.targetRevision = pending.front().targetRevision;
        ProcessRenderRuntime::CompletionContext completion;
        completion.gate = gate;
        completion.renderCache = cache;

        int owner = 0;
        ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, false);
        ProcessRenderRuntimeTestAccess::defer(
            runtime, service, std::move(job), std::move(completion));
        if (blocker == Blocker::Source)
            service->removePlaybackSource(key);
        else if (blocker == Blocker::PlanSync)
        {
            auto newerSnapshot = std::make_shared<EditableContentSnapshot>(*snapshot);
            newerSnapshot->contentRevision = 2;
            service->publishPlaybackSource(
                key, makeTestPlaybackSource(key, newerSnapshot, audio,
                                            cache, service->getTimeStretchCache()));
        }
        if (blocker != Blocker::Lease)
            service->attachExecutionLease({&owner, [](RenderJob& queued) {
                const auto sampleCount = static_cast<std::size_t>(
                    queued.endSampleExclusive - queued.startSample);
                queued.renderCache->completeChunkRenderWithAudio(
                    queued.startSample, queued.endSampleExclusive,
                    std::vector<float>(sampleCount, 0.5f), queued.targetRevision);
            }});
        if (blocker != Blocker::Domain)
            ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, true);

        const bool drainResult = service->drainRenderWorkerForExport();
        const auto stats = cache->getChunkStats();
        const bool held = check(!drainResult
                                    && runtime.hasDeferredRetriesForService(service.get())
                                    && stats.running == 1 && stats.failed == 0,
                                "blocked_export_did_not_preserve_running_chunk");
        service->clearAll();
        service->detachExecutionLease(&owner);
        ProcessRenderRuntimeTestAccess::setVocoderAvailable(runtime, false);
        service.reset();
        return held;
    };

    return runBlockedExport(Blocker::Domain, 9501)
        && runBlockedExport(Blocker::Source, 9502)
        && runBlockedExport(Blocker::PlanSync, 9503)
        && runBlockedExport(Blocker::Lease, 9504);
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    if (!runDeferredRetryContract() || !runDeferredBatchCapacityContract()
        || !runDeferredDescriptorBudgetResumeContract()
        || !runPlanNotSynchronizedRetryContract()
        || !runExportDrainDeferredBatchContract()
        || !runExportDrainNoProgressContracts())
        return 1;
    std::puts("{\"event\":\"deferred_retry_contract\",\"result\":\"pass\"}");
    return 0;
}
