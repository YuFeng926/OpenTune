#include "../Source/StandaloneArrangement.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using OpenTune::DomainKind;
using OpenTune::EditableContentSnapshot;
using OpenTune::NativePcmSource;
using OpenTune::PlaybackReadSource;
using OpenTune::SourceWindow;
using OpenTune::StandaloneArrangement;
using OpenTune::TimeGridSnapshot;

constexpr double kSampleRate = 44100.0;

StandaloneArrangement::Placement makePlacement(uint64_t contentId, double start)
{
    StandaloneArrangement::Placement placement;
    placement.contentKey = {DomainKind::StandaloneClip, contentId, 0};
    placement.timelineStartSeconds = start;
    placement.durationSeconds = 2.0;
    placement.name = "test";
    return placement;
}

bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1.0e-9;
}

std::shared_ptr<const juce::AudioBuffer<float>> makeNativeAudio()
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(1, 44100);
    buffer->clear();
    return buffer;
}

PlaybackReadSource makeReadSource(uint64_t contentId,
                                  SourceWindow window,
                                  std::shared_ptr<const TimeGridSnapshot> grid,
                                  const NativePcmSource& native)
{
    auto snapshot = std::make_shared<EditableContentSnapshot>();
    snapshot->sourceWindow = std::move(window);
    snapshot->timeGrid = std::move(grid);
    snapshot->nativeSource = native;

    PlaybackReadSource source;
    source.contentKey = {DomainKind::StandaloneClip, contentId, 0};
    source.contentSnapshot = std::move(snapshot);
    source.nativeSource = native;
    return source;
}

StandaloneArrangement::PlaybackPlacement makePlaybackPlacement(const PlaybackReadSource& source,
                                                               double start,
                                                               double duration,
                                                               double clipIn)
{
    StandaloneArrangement::PlaybackPlacement placement;
    placement.contentKey = source.contentKey;
    placement.timelineStartSeconds = start;
    placement.durationSeconds = duration;
    placement.clipInSeconds = clipIn;
    placement.source = source;
    return placement;
}

int64_t projectedSample(double seconds)
{
    return OpenTune::PlaybackSampleGeometry::projectToSample(seconds * kSampleRate);
}

bool testTrimmedSliceDoesNotInheritAcrossSourceGap()
{
    const NativePcmSource native{makeNativeAudio(), kSampleRate, 0, 1};
    auto leading = makeReadSource(101, SourceWindow{1, {}, 0.0, 1.0},
                                  TimeGridSnapshot::makeIdentity(1.0), native);
    auto trailing = makeReadSource(102, SourceWindow{1, {}, 1.0, 2.0},
                                   TimeGridSnapshot::makeIdentity(1.0), native);

    std::vector<StandaloneArrangement::PlaybackPlacement> placements{
        makePlaybackPlacement(leading, 0.0, 0.5, 0.0),
        makePlaybackPlacement(trailing, 0.5, 1.0, 0.0)};
    StandaloneArrangement::buildPlaybackGeometry(placements, kSampleRate);

    const auto& first = placements[0];
    const auto& second = placements[1];
    // 左片右端裁到 0.5s 后，右片虽在时间轴上紧接，却仍在来源 1s 处开始读取。
    // 独立投影：D = Q((0.5 - 1.0) * 44100) = -22050，读取起点回到 native 1s。
    const bool durationTrimmed = first.sampleGeometry.offsetSamples == 0
        && second.sampleGeometry.offsetSamples == projectedSample(0.5 - 1.0)
        && second.sampleGeometry.offsetSamples != first.sampleGeometry.offsetSamples
        && second.sampleGeometry.playbackBeginSample - second.sampleGeometry.offsetSamples
               == projectedSample(1.0);

    // clipIn 也必须参与实际读取域端点：左片从来源 0.5s 读 0.25s，右片仍从 1s 起读。
    auto clipInLeading = makeReadSource(103, SourceWindow{1, {}, 0.0, 1.0},
                                        TimeGridSnapshot::makeIdentity(1.0), native);
    auto clipInTrailing = makeReadSource(104, SourceWindow{1, {}, 1.0, 2.0},
                                         TimeGridSnapshot::makeIdentity(1.0), native);
    std::vector<StandaloneArrangement::PlaybackPlacement> clipInPlacements{
        makePlaybackPlacement(clipInLeading, 0.0, 0.25, 0.5),
        makePlaybackPlacement(clipInTrailing, 0.25, 1.0, 0.0)};
    StandaloneArrangement::buildPlaybackGeometry(clipInPlacements, kSampleRate);

    const auto& trimmed = clipInPlacements[0];
    const auto& following = clipInPlacements[1];
    const bool clipInTrimmed = trimmed.sampleGeometry.offsetSamples
               == projectedSample(0.0 - 0.5)
        && following.sampleGeometry.offsetSamples == projectedSample(0.25 - 1.0)
        && following.sampleGeometry.offsetSamples != trimmed.sampleGeometry.offsetSamples
        && following.sampleGeometry.playbackBeginSample - following.sampleGeometry.offsetSamples
               == projectedSample(1.0);
    return durationTrimmed && clipInTrimmed;
}

bool testOverlappingIndependentPlacementDoesNotBreakInheritance()
{
    const NativePcmSource native{makeNativeAudio(), kSampleRate, 0, 1};
    auto firstSource = makeReadSource(201, SourceWindow{1, {}, 0.0, 0.1},
                                      TimeGridSnapshot::makeIdentity(0.1), native);
    auto independentSource = makeReadSource(202, SourceWindow{1, {}, 0.0, 1.0},
                                            TimeGridSnapshot::makeIdentity(1.0), native);
    auto secondSource = makeReadSource(203, SourceWindow{1, {}, 0.1, 0.2},
                                       TimeGridSnapshot::makeIdentity(0.1), native);

    std::vector<StandaloneArrangement::PlaybackPlacement> placements{
        makePlaybackPlacement(firstSource, 0.025, 0.1, 0.0),
        makePlaybackPlacement(independentSource, 0.075, 0.05, 0.0),
        makePlaybackPlacement(secondSource, 0.125, 0.1, 0.0)};
    StandaloneArrangement::buildPlaybackGeometry(placements, kSampleRate);

    const auto& first = placements[0];
    const auto& independent = placements[1];
    const auto& second = placements[2];
    // 独立片在排序上夹在 A/B 之间且结束于 B 起点，但实际读取域在 0.05s 结束；
    // B 必须跳过它，反向接到真正的前驱 A 并共享 D。
    return projectedSample(0.125 - 0.1) == 1102
        && first.sampleGeometry.offsetSamples == 1103
        && independent.sampleGeometry.offsetSamples == projectedSample(0.075)
        && second.sampleGeometry.offsetSamples == first.sampleGeometry.offsetSamples
        && second.sampleGeometry.playbackBeginSample - second.sampleGeometry.offsetSamples
               == first.sampleGeometry.playbackEndSample - first.sampleGeometry.offsetSamples;
}

bool testFullWindowRetainsAuthoritativeSourceEnd()
{
    const NativePcmSource native{makeNativeAudio(), kSampleRate, 0, 1};
    constexpr double sourceStart = 0.1;
    constexpr double sourceEnd = 0.350124716553288;
    const double duration = sourceEnd - sourceStart;
    const double playbackStart = sourceStart + 0.5 / kSampleRate;
    const double playbackBoundary = playbackStart + duration;
    const auto first = makeReadSource(211, SourceWindow{1, {}, sourceStart, sourceEnd},
                                      TimeGridSnapshot::makeIdentity(duration), native);
    const auto second = makeReadSource(212, SourceWindow{1, {}, sourceEnd, 0.5},
                                       TimeGridSnapshot::makeIdentity(0.5 - sourceEnd), native);
    std::vector<StandaloneArrangement::PlaybackPlacement> placements{
        makePlaybackPlacement(first, playbackStart, duration, 0.0),
        makePlaybackPlacement(second, playbackBoundary, 0.5 - sourceEnd, 0.0)};
    StandaloneArrangement::buildPlaybackGeometry(placements, kSampleRate);
    return sourceStart + duration != sourceEnd
        && projectedSample(playbackBoundary - sourceEnd) == 0
        && placements[0].sampleGeometry.offsetSamples == 1
        && placements[1].sampleGeometry.offsetSamples == 1;
}

bool testIdentityAndNonIdentityNeverShareGeometry()
{
    const NativePcmSource native{makeNativeAudio(), kSampleRate, 0, 1};
    auto root = makeReadSource(301, SourceWindow{1, {}, 0.0, 2.0},
                               TimeGridSnapshot::makeIdentity(2.0), native);
    auto identity = makeReadSource(302, SourceWindow{1, {}, 0.0, 2.0},
                                   TimeGridSnapshot::makeIdentity(2.0), native);

    std::vector<OpenTune::TimeHandle> handles(4);
    handles[0].id = 1;
    handles[0].kind = OpenTune::HandleKind::ClipStart;
    handles[1].id = 2;
    handles[1].source_seconds = 0.5;
    handles[1].output_seconds = 0.4;
    handles[1].kind = OpenTune::HandleKind::UserAdded;
    handles[2].id = 3;
    handles[2].source_seconds = 1.0;
    handles[2].output_seconds = 1.0;
    handles[2].kind = OpenTune::HandleKind::UserAdded;
    handles[3].id = 4;
    handles[3].source_seconds = 2.0;
    handles[3].output_seconds = 2.0;
    handles[3].kind = OpenTune::HandleKind::ClipEnd;
    auto stretched = TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (stretched == nullptr)
        return false;

    auto nonIdentity = makeReadSource(303, SourceWindow{1, {}, 0.0, 2.0},
                                      std::move(stretched), native);

    std::vector<StandaloneArrangement::PlaybackPlacement> placements{
        makePlaybackPlacement(root, 0.1, 0.025, 0.075),
        makePlaybackPlacement(identity, 0.125, 0.9, 0.1),
        makePlaybackPlacement(nonIdentity, 1.025, 0.125, 1.0)};
    StandaloneArrangement::buildPlaybackGeometry(placements, kSampleRate);

    const auto& rootPlacement = placements[0];
    const auto& identityPlacement = placements[1];
    const auto& nonIdentityPlacement = placements[2];
    // identity 片继承 root 的 D（1103）；nonIdentity 的输出锚点仍与它的
    // 播放起点相接，但 identity/nonidentity 不得共享几何，必须独立投影。
    return identityPlacement.sampleGeometry.offsetSamples
               == rootPlacement.sampleGeometry.offsetSamples
        && nonIdentityPlacement.sampleGeometry.offsetSamples
               == projectedSample(1.025 - 1.0)
        && nonIdentityPlacement.sampleGeometry.offsetSamples
               != identityPlacement.sampleGeometry.offsetSamples;
}

bool testAppliedMovePublishesOnce()
{
    StandaloneArrangement arrangement;
    auto first = makePlacement(1, 1.0);
    auto second = makePlacement(2, 3.0);
    if (!arrangement.insertPlacement(0, first) || !arrangement.insertPlacement(0, second))
        return false;

    const auto before = arrangement.loadPlaybackSnapshot()->epoch;
    const auto result = arrangement.applyPlacementMoves({
        {0, 1, first.placementId, 10.0},
        {0, 1, second.placementId, 12.0}
    });

    StandaloneArrangement::Placement movedFirst;
    StandaloneArrangement::Placement movedSecond;
    const auto after = arrangement.loadPlaybackSnapshot()->epoch;
    return result == StandaloneArrangement::PlacementMoveResult::Applied
        && after == before + 1
        && !arrangement.getPlacementById(0, first.placementId, movedFirst)
        && !arrangement.getPlacementById(0, second.placementId, movedSecond)
        && arrangement.getPlacementById(1, first.placementId, movedFirst)
        && arrangement.getPlacementById(1, second.placementId, movedSecond)
        && near(movedFirst.timelineStartSeconds, 10.0)
        && near(movedSecond.timelineStartSeconds, 12.0);
}

bool testRejectedMoveIsAtomic()
{
    StandaloneArrangement arrangement;
    auto placement = makePlacement(3, 2.0);
    if (!arrangement.insertPlacement(0, placement))
        return false;

    const auto before = arrangement.loadPlaybackSnapshot()->epoch;
    const auto result = arrangement.applyPlacementMoves({
        {0, 1, placement.placementId, 8.0},
        {0, 1, 999999, 9.0}
    });

    StandaloneArrangement::Placement current;
    const bool stillAtSource = arrangement.getPlacementById(0, placement.placementId, current);
    const double sourceStart = current.timelineStartSeconds;
    const bool absentAtTarget = !arrangement.getPlacementById(1, placement.placementId, current);
    return result == StandaloneArrangement::PlacementMoveResult::Rejected
        && arrangement.loadPlaybackSnapshot()->epoch == before
        && stillAtSource
        && absentAtTarget
        && near(sourceStart, 2.0);
}

bool testUnchangedMoveDoesNotPublish()
{
    StandaloneArrangement arrangement;
    auto placement = makePlacement(4, 2.0);
    if (!arrangement.insertPlacement(0, placement))
        return false;

    const auto before = arrangement.loadPlaybackSnapshot()->epoch;
    const auto result = arrangement.applyPlacementMoves({{0, 0, placement.placementId, 2.0}});
    return result == StandaloneArrangement::PlacementMoveResult::Unchanged
        && arrangement.loadPlaybackSnapshot()->epoch == before;
}

} // namespace

int main()
{
    const bool passed = testAppliedMovePublishesOnce()
        && testRejectedMoveIsAtomic()
        && testUnchangedMoveDoesNotPublish()
        && testTrimmedSliceDoesNotInheritAcrossSourceGap()
        && testOverlappingIndependentPlacementDoesNotBreakInheritance()
        && testFullWindowRetainsAuthoritativeSourceEnd()
        && testIdentityAndNonIdentityNeverShareGeometry();
    if (!passed)
        std::cerr << "StandaloneArrangementMoveTests failed\n";
    return passed ? 0 : 1;
}
