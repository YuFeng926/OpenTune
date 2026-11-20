#include "../Source/Inference/RenderCache.h"
#include "../Source/Inference/TimeStretchCache.h"
#include "../Source/Render/PlaybackSourcePublisher.h"

#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

#if defined(OPENTUNE_REALTIME_AUDIT)
#include "RealtimeAudit.h"
#endif

namespace {

bool testSourceRemovalReclaimsAfterFinalReader()
{
    OpenTune::PlaybackSourcePublisher publisher;
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 2403, 0};
    const auto publishSource = [&] {
        OpenTune::PlaybackReadSource source;
        auto pcm = std::make_shared<juce::AudioBuffer<float>>(1, 256);
        pcm->clear();
        source.nativeSource.buffer = pcm;
        source.nativeSource.sampleRate = 44100.0;
        std::weak_ptr<const juce::AudioBuffer<float>> lifetime = pcm;
        publisher.publish(key, std::move(source));
        return lifetime;
    };

    const auto unborrowed = publishSource();
    publisher.remove(key);
    if (!unborrowed.expired())
        return false;

    const auto borrowed = publishSource();
    OpenTune::PlaybackReadSource read;
    if (!publisher.get(key, read))
        return false;
    publisher.remove(key);
    publisher.collectPlaybackSnapshots();
    if (borrowed.expired())
        return false;
    read = {};
    publisher.collectPlaybackSnapshots();
    return borrowed.expired();
}

bool testRetiredPreparedDryRemainsAccounted()
{
    const auto baseline = OpenTune::RenderCache::globalCacheCurrentBytes().load();
    OpenTune::PlaybackSourcePublisher publisher;
    publisher.setPlaybackSampleRate(48000.0);
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 2404, 0};
    OpenTune::PlaybackReadSource source;
    auto pcm = std::make_shared<juce::AudioBuffer<float>>(1, 256);
    pcm->clear();
    source.nativeSource = {pcm, 44100.0, 0};
    publisher.publish(key, std::move(source));

    OpenTune::PlaybackReadSource read;
    if (!publisher.get(key, read))
        return false;
    const auto preparedBytes = static_cast<size_t>(read.preparedDry.buffer->getNumSamples())
        * sizeof(float);
    publisher.remove(key);
    publisher.collectPlaybackSnapshots();
    const bool retained = preparedBytes > 0
        && OpenTune::RenderCache::globalCacheCurrentBytes().load() == baseline + preparedBytes;
    read = {};
    publisher.collectPlaybackSnapshots();
    return retained && OpenTune::RenderCache::globalCacheCurrentBytes().load() == baseline;
}

bool testBlankPreparedBuildFailureRollsBack()
{
    auto& limit = OpenTune::RenderCache::renderCachePcmLimitBytes();
    const size_t oldLimit = limit.load(std::memory_order_acquire);
    const size_t baseline = OpenTune::RenderCache::renderCacheCurrentBytes().load(
        std::memory_order_acquire);
    bool passed = false;

    {
        OpenTune::RenderCache cache;
        cache.prepareForPlaybackSampleRate(48000.0);
        cache.reconcileFullPlanAndRequest({{0, 256}, {256, 512}}, {{0, 512}}, 1);
        const auto jobs = cache.getPendingJobs();
        bool claimed = jobs.size() == 2;
        for (const auto& job : jobs)
            claimed = cache.claimPendingJob(job) && claimed;

        limit.store(baseline + 256 * sizeof(float), std::memory_order_release);
        const bool audioPublished = claimed
            && cache.completeChunkRenderWithAudio(
                   0, 256, std::vector<float>(256, 1.0f), jobs.front().targetRevision)
                == OpenTune::RenderCache::ChunkRenderResult::Published;

        limit.store(OpenTune::RenderCache::renderCacheCurrentBytes().load(
                        std::memory_order_acquire),
                    std::memory_order_release);
        const auto blankResult = audioPublished
            ? cache.markChunkAsBlank(256, jobs.back().targetRevision)
            : OpenTune::RenderCache::ChunkRenderResult::Stale;
        const auto stats = cache.getChunkStats();
        passed = audioPublished
            && blankResult == OpenTune::RenderCache::ChunkRenderResult::MemoryLimitExceeded
            && stats.idle == 1
            && stats.running == 1
            && stats.blank == 0;
    }

    limit.store(oldLimit, std::memory_order_release);
    return passed
        && OpenTune::RenderCache::renderCacheCurrentBytes().load(std::memory_order_acquire)
            == baseline;
}

bool testClearRejectsOldTimeStretchBuild()
{
    OpenTune::TimeStretchCache cache;
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 2401, 0};
    const uint32_t oldGeneration = cache.beginBuild(key);
    cache.clear();
    const uint32_t newGeneration = cache.beginBuild(key);

    cache.store(key, {1.0f, 1.0f, 1.0f, 1.0f}, 1, 1, 44100.0, oldGeneration);
    juce::AudioBuffer<float> destination(1, 4);
    if (cache.sliceCanonicalForOutputRange(key, 1, 1, 0, destination, 0, 4) != 0)
        return false;

    cache.store(key, {2.0f, 2.0f, 2.0f, 2.0f}, 2, 1, 44100.0, newGeneration);
    destination.clear();
    return cache.sliceCanonicalForOutputRange(key, 2, 1, 0, destination, 0, 4) == 4
        && destination.getSample(0, 0) == 2.0f
        && destination.getSample(0, 3) == 2.0f;
}

bool testConcurrentTimeStretchPublicationAndClear()
{
    const auto baseline = OpenTune::RenderCache::globalCacheCurrentBytes().load();
    bool intact = true;
    {
        OpenTune::TimeStretchCache cache;
        cache.prepareForPlaybackSampleRate(44100.0);
        const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 2402, 0};
        std::atomic<bool> ready{false};
        std::atomic<bool> done{false};
#if defined(OPENTUNE_REALTIME_AUDIT)
        OpenTune::RealtimeAudit::Result audioAudit;
#endif
        std::thread audio([&] {
            juce::AudioBuffer<float> destination(2, 256);
            ready.store(true, std::memory_order_release);
#if defined(OPENTUNE_REALTIME_AUDIT)
            OpenTune::RealtimeAudit::Scope scope("time_stretch_concurrent_read_publication_clear");
#endif
            do
            {
                const int copied = cache.sliceForOutputRange(
                    key, 1, 1, 0, destination, 0, 256, 44100.0);
                if (copied != 0 && copied != 256)
                    intact = false;
                if (copied == 256)
                {
                    const float value = destination.getSample(0, 0);
                    intact = intact && (value == 0.25f || value == 0.75f);
                    for (int channel = 0; channel < 2; ++channel)
                        for (int sample = 0; sample < 256; ++sample)
                            intact = intact && destination.getSample(channel, sample) == value;
                }
            }
            while (!done.load(std::memory_order_acquire));
#if defined(OPENTUNE_REALTIME_AUDIT)
            audioAudit = scope.finish();
#endif
        });
        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int publication = 0; publication < 2048; ++publication)
        {
            if (publication % 8 == 0)
                cache.clear();
            const auto generation = cache.beginBuild(key);
            cache.store(key, std::vector<float>(256, publication % 2 == 0 ? 0.25f : 0.75f),
                        1, 1, 44100.0, generation);
        }
        done.store(true, std::memory_order_release);
        audio.join();
#if defined(OPENTUNE_REALTIME_AUDIT)
        OpenTune::RealtimeAuditTest::reportResult(audioAudit);
        intact = OpenTune::RealtimeAuditTest::isClean(audioAudit) && intact;
#endif
        cache.collectPlaybackSnapshots();
        juce::AudioBuffer<float> finalOutput(1, 256);
        intact = intact && cache.sliceForOutputRange(
            key, 1, 1, 0, finalOutput, 0, 256, 44100.0) == 256;
        intact = intact && finalOutput.getSample(0, 255) == 0.75f;
    }
    return intact && OpenTune::RenderCache::globalCacheCurrentBytes().load() == baseline;
}

} // namespace

int main()
{
#if defined(OPENTUNE_REALTIME_AUDIT)
    const auto hooks = OpenTune::RealtimeAuditTest::installWindowsHooks();
    OpenTune::RealtimeAuditTest::reportHookCoverage(hooks);
    if (hooks.patchedImports == 0)
        return 1;
#endif
    if (!testSourceRemovalReclaimsAfterFinalReader()) {
        std::fputs("FAIL: source removal reclaims PCM after its final reader\n", stderr);
        return 1;
    }
    if (!testRetiredPreparedDryRemainsAccounted()) {
        std::fputs("FAIL: retired prepared dry remains accounted until its final reader exits\n", stderr);
        return 1;
    }
    if (!testBlankPreparedBuildFailureRollsBack()) {
        std::fputs("FAIL: blank prepared-build failure rollback\n", stderr);
        return 1;
    }
    if (!testClearRejectsOldTimeStretchBuild()) {
        std::fputs("FAIL: clear rejects an old TimeStretchCache build\n", stderr);
        return 1;
    }
    if (!testConcurrentTimeStretchPublicationAndClear()) {
        std::fputs("FAIL: concurrent time-stretch publication/clear preserves complete PCM and releases its budget\n", stderr);
        return 1;
    }
    std::puts("Playback cache realtime tests passed.");
    return 0;
}
