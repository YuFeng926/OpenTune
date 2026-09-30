#include "../Source/Render/ContentRenderService.h"
#include "../Source/Runtime/ProcessRenderRuntime.h"
#include "LifecycleTestTrace.h"
#include "LifecycleWatchdog.h"
#include <juce_events/juce_events.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

int runChild(int argc, char** argv)
{
    juce::ignoreUnused(argc, argv);
    OpenTuneTest::trace("RenderRuntimeStressTests", "process_begin", {
        {"watchdogMs", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleWatchdogApprovalTimeoutMs)},
        {"queueLimit", OpenTuneTest::jsonNumber(OpenTuneTest::lifecycleQueueApprovalLimit)}
    });
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    auto service = std::make_shared<OpenTune::ContentRenderService>();
    auto& runtime = OpenTune::ProcessRenderRuntime::getInstance();
    runtime.retainOwner();
    std::vector<std::shared_ptr<OpenTune::RenderCache>> caches;
    std::vector<OpenTune::ContentKey> keys;
    std::vector<std::shared_ptr<const OpenTune::EditableContentSnapshot>> snapshots;
    snapshots.reserve(8);
    const auto completionGate = std::make_shared<OpenTune::ProcessRenderRuntime::CompletionGate>();
    std::atomic<int> failed{0};
    const auto weakService = std::weak_ptr<OpenTune::ContentRenderService>(service);
    int owner = 0;
    service->attachExecutionLease({&owner, [weakService, &runtime, completionGate, &failed](OpenTune::RenderJob& job) {
        OpenTuneTest::trace("RenderRuntimeStressTests", "job_claim", {
            {"startSample", OpenTuneTest::jsonNumber(job.startSample)},
            {"endSampleExclusive", OpenTuneTest::jsonNumber(job.endSampleExclusive)},
            {"targetRevision", OpenTuneTest::jsonNumber(static_cast<long long>(job.targetRevision))}
        });
        auto strongService = weakService.lock();
        if (!strongService)
            return;
        OpenTune::ProcessRenderRuntime::CompletionContext completion;
        completion.gate = completionGate;
        completion.chunkFailed = [&failed](OpenTune::ContentKey) {
            failed.fetch_add(1, std::memory_order_relaxed);
            OpenTuneTest::trace("RenderRuntimeStressTests", "message_completion_failed");
        };
        runtime.processChunkRenderJob(std::move(strongService), job, false, std::move(completion));
        OpenTuneTest::trace("RenderRuntimeStressTests", "job_completion_posted");
    }});
    for (int i = 0; i < 8; ++i)
    {
        keys.push_back({OpenTune::DomainKind::StandaloneClip, 1000u + static_cast<uint64_t>(i), 0});
        caches.push_back(service->getOrCreateRenderCache(keys.back()));
        auto snapshot = std::make_shared<OpenTune::EditableContentSnapshot>();
        snapshots.push_back(snapshot);
    }
    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i)
        producers.emplace_back([&, i] {
            service->enqueueRender({OpenTune::RenderJob::Kind::Stage1Render, keys[i], snapshots[i],
                                   caches[i], std::make_shared<juce::AudioBuffer<float>>(1, 512),
                                   44100.0, 0, 512, -1, snapshots[i]->contentRevision});
        });
    for (auto& producer : producers)
        producer.join();
    OpenTuneTest::trace("RenderRuntimeStressTests", "producers_joined");
    service->drainRenderWorker();
    service->detachExecutionLease(&owner);

    runtime.retainOwner();
    runtime.releaseOwner();
    runtime.releaseOwner();
    juce::MessageManager::callAsync([] {
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    OpenTuneTest::trace("RenderRuntimeStressTests", "runtime_shutdown_complete", {
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

    for (const auto& cache : caches)
    {
        const auto stats = cache->getChunkStats();
        if (stats.pending != 0 || stats.running != 0
            || (!cache->isCanonicalSettled() && stats.failed == 0))
        {
            std::fprintf(stderr,
                         "{\"event\":\"stress_boundary\",\"result\":\"failure\","
                         "\"pending\":%d,\"running\":%d,\"failed\":%d}\n",
                         stats.pending, stats.running, stats.failed);
            return 1;
        }
    }
    service.reset();
    OpenTuneTest::trace("RenderRuntimeStressTests", "owner_destructor_complete", {
        {"failedCompletions", OpenTuneTest::jsonNumber(failed.load(std::memory_order_relaxed))}
    });
    std::printf("{\"event\":\"stress_boundary\",\"result\":\"pass\",\"workers\":8}\n");
    OpenTuneTest::trace("RenderRuntimeStressTests", "process_end");
    return 0;
}

int main(int argc, char** argv)
{
    return OpenTuneTest::run(argc, argv, "RenderRuntimeStressTests", runChild);
}
