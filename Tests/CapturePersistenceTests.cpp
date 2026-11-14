#include "../Source/Plugin/Capture/CaptureSession.h"
#include "../Source/Utils/PitchCurve.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace {

using OpenTune::ContentKey;
using OpenTune::DomainKind;
using OpenTune::F0FrameRange;
using OpenTune::PitchCurve;
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

bool sameRanges(const std::vector<F0FrameRange>& left, const std::vector<F0FrameRange>& right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].startFrame != right[i].startFrame
            || left[i].endFrameExclusive != right[i].endFrameExclusive)
            return false;
    }
    return true;
}

bool testEmptyArchive()
{
    CaptureSession source({});
    const auto archive = source.serialize();

    CaptureSession restored({});
    return restored.deserialize(archive) && restored.listSegments().empty();
}

bool testErasedRangesRoundTrip()
{
    CaptureSession source({});
    source.testInjectEditedSegment(0.0, 1.0, 11, makeAudio(256), 44100.0);
    auto curve = std::make_shared<PitchCurve>();
    curve->setOriginalF0(std::vector<float>(16, 220.0f));
    const std::vector<F0FrameRange> expected{{3, 5}, {8, 12}};
    curve->replaceErasedRangesInRange(0, 16, expected);
    source.testSegments().front()->content->applyPitchCurve(std::move(curve));

    const auto archive = source.serialize();
    CaptureSession restored({});
    if (!restored.deserialize(archive) || restored.testSegments().size() != 1)
        return false;
    const auto& pitchCurve = restored.testSegments().front()->content->content().analysis.pitchCurve;
    return pitchCurve != nullptr && sameRanges(pitchCurve->getSnapshot()->getErasedRanges(), expected);
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

    restored.testInjectEditedSegment(0.0, 1.0, 5, makeAudio(256), 44100.0);
    restored.testInjectProcessingSegment(0.0, 2.0, 7, makeAudio(512), 44100.0);

    // The restored archive reuses key 5 and replaces key 7. The stale owner of key 5
    // must not retire the cache the restored segment republishes.
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

// Restore owns the media source window: the initial capture frame count / actual
// rate defines it once; persistence does not rewrite absolute seconds.
bool testRestoreEstablishesAuthoritativeSourceWindow()
{
    CaptureSession archiveSource({});
    archiveSource.testInjectEditedSegment(0.0, 256.0 / 44100.0, 9, makeAudio(256), 44100.0);
    const auto archive = archiveSource.serialize();

    CaptureSession restored({});
    restored.prepareToPlay(96000.0, 512, 1);
    if (!restored.deserialize(archive))
        return false;

    for (const auto& seg : restored.testSegments()) {
        if (seg->contentKey.objectId != 9 || !seg->content)
            continue;
        const auto& state = seg->content->content();
        return state.sourceWindow.sourceStartSeconds == 0.0
            && std::abs(state.sourceWindow.sourceEndSeconds - 256.0 / 44100.0) <= 1e-15
            && state.firstCanonicalSample == 0
            && state.nativeSource.buffer == state.audioBuffer
            && state.nativeSource.sampleRate == 44100.0
            && state.nativeSource.firstSourceSample == 0;
    }
    return false;
}

bool testOwnerInitializesNativeOriginFromSourceWindow()
{
    OpenTune::ContentState state;
    state.audioBuffer = makeAudio(1024);
    state.sampleRate = 96000.0;
    state.sourceWindow.sourceStartSeconds = 1024.0 / 96000.0;
    state.sourceWindow.sourceEndSeconds = 2048.0 / 96000.0;
    state.firstCanonicalSample = 470;
    OpenTune::CaptureSegmentContent owner(10);
    owner.applyContentState(std::move(state));
    return owner.content().nativeSource.firstSourceSample == 1024
        && owner.content().sourceWindow.sourceStartSeconds == 1024.0 / 96000.0
        && owner.content().firstCanonicalSample == 470;
}

} // namespace

int main()
{
    if (!testOwnerInitializesNativeOriginFromSourceWindow())
    {
        std::fputs("FAIL: native origin follows the authoritative source window\n", stderr);
        return 1;
    }
    if (!testEmptyArchive())
    {
        std::fputs("FAIL: empty capture archive\n", stderr);
        return 1;
    }

    if (!testErasedRangesRoundTrip())
    {
        std::fputs("FAIL: capture erased F0 ranges serialize/deserialize\n", stderr);
        return 1;
    }

    if (!testReplaceRetireSemantics())
    {
        std::fputs("FAIL: capture replace retire semantics\n", stderr);
        return 1;
    }

    if (!testRestoreEstablishesAuthoritativeSourceWindow())
    {
        std::fputs("FAIL: capture restore establishes authoritative source window\n", stderr);
        return 1;
    }

    std::puts("CapturePersistence tests passed.");
    return 0;
}
