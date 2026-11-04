// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipIds.h"

#include "Content/StandaloneContentRepository.h"
#include "PluginProcessor.h"
#include "Utils/AppLogger.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>

namespace OpenTune {

namespace {

/** The highest clip id that the counter of the instance is known to have passed, with the content epoch it was
    recorded in. Message thread only. */
struct ClipIdFloor
{
    std::string instanceId;
    std::uint64_t epoch = 0;
    std::uint64_t clipId = 0;
};

ClipIdFloor& clipIdFloor()
{
    static ClipIdFloor floor;
    return floor;
}

} // namespace

std::size_t controlPossibleClipIdCollisions(ControlOpContext& context)
{
    const auto* repository = context.processor().getStandaloneContentRepository();
    // Without a StandaloneContentRepository::clear() (content epoch 0) every id came from the counter, which is past
    // them all.
    if (repository == nullptr || repository->getContentEpoch() == 0)
        return 0;
    const auto& floor = clipIdFloor();
    const std::uint64_t behind =
        floor.instanceId == context.facts().instanceId && floor.epoch == repository->getContentEpoch() ? floor.clipId
                                                                                                       : 0;
    const auto clips = repository->getAllClips();
    return static_cast<std::size_t>(
        std::count_if(clips.begin(), clips.end(), [behind](const ContentKey& key) { return key.objectId > behind; }));
}

void controlRecordCommittedClipId(ControlOpContext& context, const ContentKey& key)
{
    const auto* repository = context.processor().getStandaloneContentRepository();
    if (repository == nullptr)
        return;
    auto& floor = clipIdFloor();
    const auto epoch = repository->getContentEpoch();
    if (floor.instanceId != context.facts().instanceId || floor.epoch != epoch)
        floor = ClipIdFloor{context.facts().instanceId, epoch, 0};
    floor.clipId = std::max(floor.clipId, key.objectId);
}

std::size_t controlSkipUsedClipIds(ControlOpContext& context, const char* operation, std::size_t clipsToMake)
{
    if (clipsToMake == 0 || controlPossibleClipIdCollisions(context) == 0)
        return 0;
    auto* repository = context.processor().getStandaloneContentRepository();
    if (repository == nullptr)
        return 0;

    // The ids in use: getAllClips() lists every clip of the repository, the retired ones that are not reclaimed yet
    // as well, and createClip() refuses an id that any of them has.
    std::set<std::uint64_t> used;
    for (const auto& key : repository->getAllClips())
        used.insert(key.objectId);

    // After a probe with the id p, the creations that follow take p + 1 ... p + clipsToMake from the counter. Ids
    // come from a project file as 64-bit integers, so they can lie anywhere, up to the largest value: the run must
    // not wrap around, and the search must not depend on how high the highest id lies.
    const std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    const auto runIsFree = [&used, clipsToMake, most](std::uint64_t probeId) {
        // probeId + clipsToMake must not wrap around (an id near the largest value has no run behind it).
        if (clipsToMake > most - probeId)
            return false;
        for (std::uint64_t offset = 1; offset <= clipsToMake; ++offset)
            if (used.count(probeId + offset) != 0)
                return false;
        return true;
    };

    // Each id in use stands in the way of the probes with ids from itself down to clipsToMake below it (clipsToMake + 1
    // ids), and the counter hands out consecutive ids, so among that many probes plus one there is one with a free
    // run behind it. The bound only keeps a case that was not foreseen (the counter close to the largest value) from
    // looping.
    const std::size_t maxProbes = used.size() * (clipsToMake + 1) + 1;
    std::size_t probes = 0;
    std::size_t inUse = 0;
    bool found = false;
    ContentKey last;
    while (probes < maxProbes && !found) {
        ++probes;
        // StandaloneContentRepository::createClip() without a forced id: the call OpenTune's own functions make for a
        // new clip. It takes the next id from the counter and refuses an id in use, using the id up either way.
        const auto probe = repository->createClip();
        if (!probe.isValid()) {
            ++inUse;
            continue;
        }
        // A free id made an empty clip. Retired and then released, it is deleted (releaseClip() deletes only a retired
        // clip); it never reached the render service or an arrangement.
        repository->retireClip(probe);
        repository->releaseClip(probe);
        jassert(repository->findClip(probe) == nullptr);
        if (runIsFree(probe.objectId)) {
            found = true;
            last = probe;
        }
    }
    // The probe that found the run is the last id the counter passed: what is recorded is the id the next creation
    // comes after, and the creations record their own ids.
    if (found)
        controlRecordCommittedClipId(context, last);

    if (!found)
        AppLogger::logNoThrow(juce::String("control API: ") + operation + ": OpenTune's clip id counter was moved by "
                              + juce::String(static_cast<int>(probes))
                              + " id(s) without finding a run of free ids for the new clips (project opened before); "
                                "the call goes on and OpenTune may refuse it");
    else if (probes > 1)
        AppLogger::logNoThrow(juce::String("control API: ") + operation + ": OpenTune's clip id counter was moved past "
                              + juce::String(static_cast<int>(probes)) + " id(s) ("
                              + juce::String(static_cast<int>(inUse)) + " of them used by clips) so that the "
                              + juce::String(static_cast<int>(clipsToMake))
                              + " new clip id(s) are free (project opened before)");
    return probes;
}

} // namespace OpenTune
