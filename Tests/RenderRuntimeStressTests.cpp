#include "../Source/Render/ContentRenderService.h"
#include "../Source/Runtime/ProcessRenderRuntime.h"
#include "LifecycleTestTrace.h"
#include <juce_events/juce_events.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

int main()
{
    OpenTuneTest::trace("RenderRuntimeStressTests", "process_begin");
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    OpenTune::ContentRenderService service;
    std::vector<std::shared_ptr<OpenTune::RenderCache>> caches;
    std::vector<OpenTune::ContentKey> keys;
    std::vector<std::shared_ptr<const OpenTune::EditableContentSnapshot>> snapshots;
    snapshots.reserve(8);
    int owner = 0;
    service.attachExecutionLease({&owner, [](OpenTune::RenderJob& job) {
        job.renderCache->completeChunkRenderWithAudio(
            job.startSample, job.endSampleExclusive,
            std::vector<float>(static_cast<size_t>(job.endSampleExclusive - job.startSample), 0.0f),
            job.targetRevision);
    }});
    for (int i = 0; i < 8; ++i)
    {
        keys.push_back({OpenTune::DomainKind::StandaloneClip, 1000u + static_cast<uint64_t>(i), 0});
        caches.push_back(service.getOrCreateRenderCache(keys.back()));
        auto snapshot = std::make_shared<OpenTune::EditableContentSnapshot>();
        snapshots.push_back(snapshot);
    }
    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i)
        producers.emplace_back([&, i] {
            service.enqueueRender({OpenTune::RenderJob::Kind::Stage1Render, keys[i], snapshots[i],
                                   caches[i], std::make_shared<juce::AudioBuffer<float>>(1, 512),
                                   44100.0, 0, 512, -1, snapshots[i]->contentRevision});
        });
    for (auto& producer : producers)
        producer.join();
    OpenTuneTest::trace("RenderRuntimeStressTests", "producers_joined");
    service.drainRenderWorker();
    service.detachExecutionLease(&owner);

    auto& runtime = OpenTune::ProcessRenderRuntime::getInstance();
    runtime.retainOwner();
    runtime.retainOwner();
    runtime.releaseOwner();
    runtime.releaseOwner();
    juce::MessageManager::callAsync([] {
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    OpenTuneTest::trace("RenderRuntimeStressTests", "runtime_shutdown_complete");

    for (const auto& cache : caches)
        if (!cache->isCanonicalSettled())
        {
            std::fprintf(stderr, "{\"event\":\"stress_boundary\",\"result\":\"failure\"}\n");
            return 1;
        }
    std::printf("{\"event\":\"stress_boundary\",\"result\":\"pass\",\"workers\":8}\n");
    OpenTuneTest::trace("RenderRuntimeStressTests", "process_end");
    return 0;
}
