#include "../Source/Render/ContentRenderService.h"
#include "../Source/Runtime/ProcessRenderRuntime.h"
#include "LifecycleTestTrace.h"
#include "LifecycleWatchdog.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

int runChild(int argc, char** argv)
{
    juce::ignoreUnused(argc, argv);
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "process_begin", {
        {"watchdogMs", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleWatchdogApprovalTimeoutMs)},
        {"queueLimit", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleQueueApprovalLimit)}
    });
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
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
    const auto weakService = std::weak_ptr<OpenTune::ContentRenderService>(service);
    int owner = 0;
    service->attachExecutionLease({&owner, [weakService, &runtime, completionGate, &settled, &failed](OpenTune::RenderJob& job) {
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
        completion.chunkSettled = [&settled](OpenTune::ContentKey,
                                               std::shared_ptr<const OpenTune::EditableContentSnapshot>,
                                               std::shared_ptr<const juce::AudioBuffer<float>>,
                                               double) {
            settled.fetch_add(1, std::memory_order_relaxed);
            OpenTuneTest::trace("RenderRuntimeLifecycleTests", "message_completion_settled");
        };
        completion.chunkFailed = [&failed](OpenTune::ContentKey) {
            failed.fetch_add(1, std::memory_order_relaxed);
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
        {"renderCacheBytes", OpenTuneTest::jsonNumber(static_cast<long long>(OpenTune::RenderCache::globalCacheCurrentBytes()))}
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

    if (!cache->isCanonicalSettled() && failed.load(std::memory_order_relaxed) == 0)
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
