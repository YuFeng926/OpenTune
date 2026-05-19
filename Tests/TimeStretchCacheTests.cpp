/**
 * Tests/TimeStretchCacheTests.cpp — Unit tests for §6.2 TimeStretchCache.
 *
 * Covers clip-wide single-entry cache hit/miss + slice-by-output-time-range +
 * invalidation semantics.
 *
 * Spec coverage:
 *   - openspec/changes/vocal-time-stretch/specs/two-stage-render-pipeline/spec.md
 *     (§"双级缓存 — PitchCache chunk-wise + TimeStretchCache clip-wide" + invalidation)
 *
 * Suite aggregator: runTimeStretchCacheSuite() — registered in TestMain.cpp.
 */
#include "TestSupport.h"
#include "Inference/TimeStretchCache.h"

#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr uint64_t kMatId = 42;

std::vector<float> makeRamp(int n)
{
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(i) / static_cast<float>(n);
    }
    return out;
}

} // namespace

void runTimeStretchCacheStoreAndHitTest()
{
    constexpr const char* testName = "TimeStretchCache_StoreAndHit";

    TimeStretchCache cache;
    cache.store(kMatId, makeRamp(1000), /*pitchRev=*/3, /*timeGridRev=*/5, kSampleRate);

    if (!cache.hit(kMatId, 3, 5)) {
        logFail(testName, "hit() should return true for matching revisions");
        return;
    }

    // Wrong pitch rev → miss
    if (cache.hit(kMatId, 4, 5)) {
        logFail(testName, "hit() should return false when pitchRevision mismatches");
        return;
    }
    // Wrong timeGrid rev → miss
    if (cache.hit(kMatId, 3, 6)) {
        logFail(testName, "hit() should return false when timeGridRevision mismatches");
        return;
    }
    // Wrong materialization → miss
    if (cache.hit(99, 3, 5)) {
        logFail(testName, "hit() should return false for unknown materializationId");
        return;
    }

    logPass(testName);
}

void runTimeStretchCacheStoreReplacesTest()
{
    constexpr const char* testName = "TimeStretchCache_StoreReplaces";

    TimeStretchCache cache;
    cache.store(kMatId, makeRamp(1000), 1, 1, kSampleRate);
    cache.store(kMatId, makeRamp(2000), 2, 1, kSampleRate);

    if (cache.hit(kMatId, 1, 1)) {
        logFail(testName, "old entry should be replaced");
        return;
    }
    if (!cache.hit(kMatId, 2, 1)) {
        logFail(testName, "new entry should be retrievable");
        return;
    }

    logPass(testName);
}

void runTimeStretchCacheSliceForOutputRangeTest()
{
    constexpr const char* testName = "TimeStretchCache_SliceForOutputRange";

    TimeStretchCache cache;
    // 1 second of audio at sampleRate, ramp from 0 → 1
    auto audio = makeRamp(static_cast<int>(kSampleRate));
    cache.store(kMatId, audio, 1, 1, kSampleRate);

    juce::AudioBuffer<float> dest(/*channels=*/2, /*samples=*/441);
    dest.clear();

    // Read 441 samples (10 ms) starting at output time 0.5 s, target SR matches cache SR
    const int written = cache.sliceForOutputRange(kMatId,
                                                   /*outputStartSeconds=*/0.5,
                                                   dest, /*destStart=*/0,
                                                   /*numSamples=*/441,
                                                   /*targetSampleRate=*/static_cast<int>(kSampleRate));

    if (written != 441) {
        logFail(testName, ("expected 441 samples written, got " + std::to_string(written)).c_str());
        return;
    }

    // First sample should be near 0.5 (mid-ramp)
    const float firstSample = dest.getSample(0, 0);
    if (firstSample < 0.45f || firstSample > 0.55f) {
        logFail(testName, ("first sample should be ~0.5 (mid-ramp), got "
                           + std::to_string(firstSample)).c_str());
        return;
    }

    // Both channels should have identical values (mono broadcast)
    if (dest.getSample(0, 0) != dest.getSample(1, 0)) {
        logFail(testName, "stereo destination should have identical samples (mono broadcast)");
        return;
    }

    logPass(testName);
}

void runTimeStretchCacheInvalidateTest()
{
    constexpr const char* testName = "TimeStretchCache_Invalidate";

    TimeStretchCache cache;
    cache.store(kMatId, makeRamp(1000), 1, 1, kSampleRate);

    if (!cache.hit(kMatId, 1, 1)) {
        logFail(testName, "precondition: hit before invalidate should be true");
        return;
    }

    cache.invalidate(kMatId);

    if (cache.hit(kMatId, 1, 1)) {
        logFail(testName, "after invalidate, hit should return false");
        return;
    }

    logPass(testName);
}

void runTimeStretchCacheClearTest()
{
    constexpr const char* testName = "TimeStretchCache_Clear";

    TimeStretchCache cache;
    cache.store(1, makeRamp(100), 1, 1, kSampleRate);
    cache.store(2, makeRamp(100), 1, 1, kSampleRate);

    auto stats = cache.getStats();
    if (stats.materializationCount != 2 || stats.publishedCount != 2) {
        logFail(testName, "precondition: should have 2 published entries before clear");
        return;
    }

    cache.clear();

    stats = cache.getStats();
    if (stats.materializationCount != 0) {
        logFail(testName, "clear() should remove all entries");
        return;
    }

    logPass(testName);
}

void runTimeStretchCacheSuite()
{
    logSection("TimeStretchCache");
    runTimeStretchCacheStoreAndHitTest();
    runTimeStretchCacheStoreReplacesTest();
    runTimeStretchCacheSliceForOutputRangeTest();
    runTimeStretchCacheInvalidateTest();
    runTimeStretchCacheClearTest();
}
