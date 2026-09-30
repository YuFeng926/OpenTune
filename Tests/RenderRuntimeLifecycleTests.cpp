#include "../Source/Render/ContentRenderService.h"
#include "../Source/Runtime/ProcessRenderRuntime.h"
#include "LifecycleTestTrace.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

int main()
{
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "process_begin");
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    OpenTune::ContentRenderService service;
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 901, 0};
    auto cache = service.getOrCreateRenderCache(key);
    const auto snapshot = std::make_shared<OpenTune::EditableContentSnapshot>();
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, 256);
    audio->clear();

    int owner = 0;
    service.attachExecutionLease({&owner, [cache](OpenTune::RenderJob& job) {
        cache->completeChunkRenderWithAudio(job.startSample, job.endSampleExclusive,
                                            std::vector<float>(static_cast<size_t>(job.endSampleExclusive - job.startSample), 0.0f),
                                            job.targetRevision);
    }});
    service.enqueueRender(OpenTune::RenderJob{
        OpenTune::RenderJob::Kind::Stage1Render, key, snapshot, cache, audio, 44100.0,
        0, audio->getNumSamples(), -1, snapshot->contentRevision});
    const auto pending = cache->getPendingJobs();
    if (pending.size() != 1)
    {
        std::fprintf(stderr, "{\"event\":\"lifecycle_boundary\",\"result\":\"failure\"}\n");
        return 1;
    }

    service.drainRenderWorker();
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "render_drain_complete");
    service.detachExecutionLease(&owner);
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "lease_detached");

    auto& runtime = OpenTune::ProcessRenderRuntime::getInstance();
    runtime.retainOwner();
    runtime.resetVocoder(nullptr, [](OpenTune::ProcessRenderRuntime::ControlResult) {});
    std::thread firstShutdown([&runtime] {
        runtime.releaseOwner();
        juce::MessageManager::callAsync([] {
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    firstShutdown.join();
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "first_runtime_shutdown_complete");

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

    if (!cache->isCanonicalSettled())
    {
        std::fprintf(stderr, "{\"event\":\"lifecycle_boundary\",\"result\":\"failure\"}\n");
        return 1;
    }
    std::printf("{\"event\":\"lifecycle_boundary\",\"result\":\"pass\"}\n");
    OpenTuneTest::trace("RenderRuntimeLifecycleTests", "process_end");
    return 0;
}
