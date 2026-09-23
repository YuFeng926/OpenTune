#include "../Source/Plugin/Capture/CaptureSession.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

using OpenTune::ContentKey;
using OpenTune::DomainKind;
using OpenTune::Capture::CaptureSession;
using OpenTune::Capture::ProcessorBindings;

bool containsObjectId(const std::vector<ContentKey>& keys, uint64_t objectId)
{
    return std::any_of(keys.begin(), keys.end(), [objectId](const ContentKey& key) {
        return key.domainKind == DomainKind::RegularVST3Capture && key.objectId == objectId;
    });
}

std::shared_ptr<juce::AudioBuffer<float>> makeAudio(int samples)
{
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, samples);
    audio->clear();
    return audio;
}

bool testEmptyArchive()
{
    CaptureSession source({});
    const auto archive = source.serialize();

    CaptureSession restored({});
    return restored.deserialize(archive) && restored.listSegments().empty();
}

bool testReplaceRetireSemantics()
{
    CaptureSession archiveSource({});
    archiveSource.testInjectEditedSegment(0.0, 1.0, 5, makeAudio(256), 44100.0);
    const auto archive = archiveSource.serialize();

    std::vector<ContentKey> retired;
    ProcessorBindings bindings;
    bindings.retireSegment = [&retired](ContentKey key) { retired.push_back(key); };
    CaptureSession restored(std::move(bindings));

    // Segment 7 covers segment 5, parking key 5 before restore. The archive
    // then reuses key 5 and replaces key 7.
    restored.testInjectEditedSegment(0.0, 1.0, 5, makeAudio(256), 44100.0);
    restored.testInjectProcessingSegment(0.0, 2.0, 7, makeAudio(512), 44100.0);
    restored.onRenderComplete(ContentKey{DomainKind::RegularVST3Capture, 7, 0});

    const auto compactedSegments = restored.listSegments();
    if (compactedSegments.size() != 1 || compactedSegments.front().contentKey.objectId != 7)
        return false;

    if (!restored.deserialize(archive))
        return false;

    for (int tick = 0; tick < 8; ++tick)
        restored.tick();

    const auto segments = restored.listSegments();
    return segments.size() == 1
        && segments.front().contentKey.objectId == 5
        && !containsObjectId(retired, 5)
        && containsObjectId(retired, 7);
}

} // namespace

int main()
{
    if (!testEmptyArchive())
    {
        std::fputs("FAIL: empty capture archive\n", stderr);
        return 1;
    }

    if (!testReplaceRetireSemantics())
    {
        std::fputs("FAIL: capture replace retire semantics\n", stderr);
        return 1;
    }

    std::puts("CapturePersistence tests passed.");
    return 0;
}
