#include "Inference/TimeStretchCache.h"
#include "Content/ContentKey.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <iostream>
#include <vector>

using namespace OpenTune;

namespace {

bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "TimeStretchCacheRebase: " << message << "\n";
    return condition;
}

bool checkRebaseKeepsAudioAndAlignsRevision()
{
    TimeStretchCache cache;
    const ContentKey key{DomainKind::StandaloneClip, 1, 0};
    constexpr double sampleRate = 44100.0;
    constexpr int numSamples = 256;

    std::vector<float> pcm(static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i)
        pcm[static_cast<size_t>(i)] = static_cast<float>(i) / static_cast<float>(numSamples);

    const uint32_t generation = cache.beginBuild(key);
    cache.store(key, pcm, 5, 2, sampleRate, generation);

    juce::AudioBuffer<float> destination(1, numSamples);
    auto read = [&](uint64_t contentRevision, uint64_t timeGridRevision) {
        destination.clear();
        return cache.sliceForOutputRange(key, contentRevision, timeGridRevision,
                                         0, destination, 0, numSamples,
                                         static_cast<int>(sampleRate));
    };

    if (!check(read(5, 2) == numSamples, "published revision must be readable"))
        return false;
    const std::vector<float> before(destination.getReadPointer(0),
                                    destination.getReadPointer(0) + numSamples);

    // A republish advances the revision without producing new PCM.
    if (!check(read(6, 2) == 0, "unpublished revision must be silent"))
        return false;

    if (!check(cache.rebaseRevisions(key, 6, 2), "rebase must succeed for published entry"))
        return false;
    if (!check(read(5, 2) == 0, "stale revision must no longer match"))
        return false;
    if (!check(read(6, 2) == numSamples, "rebase target revision must be readable"))
        return false;

    for (int i = 0; i < numSamples; ++i)
    {
        if (!check(destination.getReadPointer(0)[i] == before[static_cast<size_t>(i)],
                   "rebase must not change PCM"))
            return false;
    }

    if (!check(!cache.rebaseRevisions(key, 6, 2), "idempotent rebase must be a no-op"))
        return false;
    if (!check(!cache.rebaseRevisions(ContentKey{DomainKind::StandaloneClip, 2, 0}, 7, 2),
               "rebase must fail for a key with no entry"))
        return false;

    // timeGridRevision is part of the version key too.
    if (!check(cache.rebaseRevisions(key, 6, 3), "rebase must accept a new timeGridRevision"))
        return false;
    if (!check(read(6, 2) == 0 && read(6, 3) == numSamples,
               "both revision components must be aligned"))
        return false;

    cache.invalidate(key);
    if (!check(!cache.rebaseRevisions(key, 7, 3), "rebase must fail after invalidate"))
        return false;
    if (!check(read(7, 3) == 0, "invalidate must remove the entry"))
        return false;

    return true;
}

} // namespace

int main()
{
    if (!checkRebaseKeepsAudioAndAlignsRevision())
        return 1;
    return 0;
}
