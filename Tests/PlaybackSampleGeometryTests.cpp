// 播放固定映射纯函数契约测试。
// 覆盖 Q 的负半/大数/半样本行为、D/A/Z 权威端点投影、int64 半开区间求交，
// 以及 host -> prepared local 地址在分块、seek、连续切片与负 local 前缀下的不变量。

#include "../Source/Utils/PlaybackSampleGeometry.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace PSG = OpenTune::PlaybackSampleGeometry;

bool testProjectHalfTiesTowardPositiveInfinity()
{
    // 非 tie 的普通最近点。
    if (PSG::projectToSample(0.0) != 0
        || PSG::projectToSample(0.25) != 0
        || PSG::projectToSample(0.75) != 1
        || PSG::projectToSample(3.25) != 3
        || PSG::projectToSample(-0.25) != 0
        || PSG::projectToSample(-0.75) != -1
        || PSG::projectToSample(-3.25) != -3
        || PSG::projectToSample(-3.75) != -4)
        return false;

    // 半样本 tie 一律朝 +infinity，正负对称平移一致。
    if (PSG::projectToSample(0.5) != 1
        || PSG::projectToSample(1.5) != 2
        || PSG::projectToSample(2.5) != 3
        || PSG::projectToSample(-0.5) != 0
        || PSG::projectToSample(-1.5) != -1
        || PSG::projectToSample(-2.5) != -2)
        return false;

    // 0.49999999999999994 是 floor(x + 0.5) 的经典误判点。
    return PSG::projectToSample(0.49999999999999994) == 0
        && PSG::projectToSample(0.5000000000000001) == 1
        && PSG::projectToSample(-0.49999999999999994) == 0
        && PSG::projectToSample(-0.5000000000000001) == -1;
}

bool testProjectTranslationInvarianceAtLargePositions()
{
    const double fractions[] = {0.0, 0.25, 0.5, 0.75};
    const int64_t translations[] = {
        0, 1, -1, 3456, -13824,
        int64_t{1} << 40, -(int64_t{1} << 40),
        int64_t{1} << 50, -(int64_t{1} << 50)};

    for (const double fraction : fractions)
    {
        for (const int64_t translation : translations)
        {
            const double shifted = fraction + static_cast<double>(translation);
            if (PSG::projectToSample(shifted) != PSG::projectToSample(fraction) + translation)
                return false;
        }
    }

    // 2^52 - 0.5 处半样本 tie 仍朝 +infinity；整数部分与小数部分均精确。
    const double largeTie = 4503599627370495.5;
    return PSG::projectToSample(largeTie) == 4503599627370496LL
        && PSG::projectToSample(-largeTie) == -4503599627370495LL;
}

bool testPlacementEliminatesSecondsRoundTripAtBlockBoundaries()
{
    constexpr double hostRate = 48000.0;

    // 原反例：3456 / 48000 * 48000 == 3455.9999999999995，
    // 13824 / 48000 * 48000 == 13823.999999999998；截断会退到前一个样本。
    const double start3456 = 3456.0 / hostRate;
    const double start13824 = 13824.0 / hostRate;
    if (!(start3456 * hostRate < 3456.0) || !(start13824 * hostRate < 13824.0))
        return false;
    if (static_cast<int64_t>(start3456 * hostRate) != 3455
        || static_cast<int64_t>(start13824 * hostRate) != 13823)
        return false;

    // identity placement：host 与 prepared 共用同一网格，block 地址逐样本平移。
    const auto identity = PSG::makePlacement(hostRate, 0.0, 10.0, 0.0);
    if (identity.offsetSamples != 0
        || identity.playbackBeginSample != 0
        || identity.playbackEndSample != 480000)
        return false;

    const int64_t firstBlock[] = {3456, 13824};
    const int64_t secondBlock[] = {3584, 14336};
    const int blockSizes[] = {128, 512};
    for (int i = 0; i < 2; ++i)
    {
        const auto begin = PSG::mapHostSampleToPrepared(identity, firstBlock[i], 0);
        const auto end = PSG::mapHostSampleToPrepared(identity, secondBlock[i], 0);
        if (begin.bufferLocalIndex != firstBlock[i]
            || end.bufferLocalIndex != secondBlock[i]
            || end.bufferLocalIndex - begin.bufferLocalIndex != blockSizes[i])
            return false;
    }

    // 权威播放起点落在样本边界附近：Q 修正秒数 round-trip 误差。
    const auto placement3456 = PSG::makePlacement(hostRate, start3456, 10.0, 0.0);
    if (placement3456.offsetSamples != 3456 || placement3456.playbackBeginSample != 3456)
        return false;
    if (PSG::mapHostSampleToPrepared(placement3456, 3456, 0).bufferLocalIndex != 0
        || PSG::mapHostSampleToPrepared(placement3456, 3584, 0).bufferLocalIndex != 128)
        return false;

    const auto placement13824 = PSG::makePlacement(hostRate, start13824, 10.0, 0.0);
    if (placement13824.offsetSamples != 13824 || placement13824.playbackBeginSample != 13824)
        return false;
    return PSG::mapHostSampleToPrepared(placement13824, 13824, 0).bufferLocalIndex == 0
        && PSG::mapHostSampleToPrepared(placement13824, 14336, 0).bufferLocalIndex == 512;
}

bool testHalfOpenSampleSpanIntersection()
{
    // 相邻 block 半开区间不重叠。
    const auto adjacent = PSG::intersect({0, 3456}, {3456, 3584});
    if (!adjacent.empty() || adjacent.begin != 3456 || adjacent.end != 3456)
        return false;
    if (!PSG::intersect({0, 13824}, {13824, 14336}).empty())
        return false;

    // 部分重叠、包含与负坐标。
    const auto partial = PSG::intersect({0, 1000}, {500, 1500});
    if (partial.begin != 500 || partial.end != 1000)
        return false;
    const auto contained = PSG::intersect({0, 2000}, {400, 600});
    if (contained.begin != 400 || contained.end != 600)
        return false;
    const auto negative = PSG::intersect({-500, 500}, {0, 1000});
    if (negative.begin != 0 || negative.end != 500)
        return false;

    // 大坐标求交完全在 int64 内完成。
    constexpr int64_t large = int64_t{1} << 40;
    const auto largeSpan = PSG::intersect({large, large + 512},
                                          {large + 256, large + 1024});
    if (largeSpan.begin != large + 256 || largeSpan.end != large + 512)
        return false;

    // 空 span（含非法逆序）与任何区间求交仍为空。
    return PSG::intersect({5, 5}, {0, 10}).empty()
        && PSG::intersect({7, 3}, {0, 10}).empty();
}

bool testMixedBlocksAndSeekMatchContinuousPreparedReference()
{
    constexpr double hostRate = 48000.0;
    const auto placement = PSG::makePlacement(hostRate, 1.0, 3.0, 0.25);
    if (placement.offsetSamples != 36000
        || placement.playbackBeginSample != 48000
        || placement.playbackEndSample != 144000)
        return false;

    const auto hostSpan = PSG::hostPlaybackSpan(placement);
    const auto preparedSpan = PSG::requiredPreparedSpan(placement);
    if (hostSpan.begin != 48000 || hostSpan.end != 144000
        || preparedSpan.begin != 12000 || preparedSpan.end != 108000)
        return false;

    const int64_t preparedFirstSample = preparedSpan.begin;
    const int64_t preparedLength = preparedSpan.end - preparedSpan.begin;
    std::vector<int64_t> rendered(static_cast<size_t>(hostSpan.end - hostSpan.begin), -1);

    // 混合 block 长度覆盖同一 host 区间：每个 host 恰好写一次。
    const int blockLengths[] = {1, 64, 127, 128, 256, 512, 1024};
    int64_t cursor = hostSpan.begin;
    size_t blockIndex = 0;
    while (cursor < hostSpan.end)
    {
        const int64_t blockEnd = std::min(hostSpan.end,
                                          cursor + blockLengths[blockIndex % 7]);
        const auto overlap = PSG::intersect({cursor, blockEnd}, hostSpan);
        if (overlap.empty() || overlap.begin != cursor)
            return false;

        for (int64_t host = overlap.begin; host < overlap.end; ++host)
        {
            const auto address = PSG::mapHostSampleToPrepared(placement, host, preparedFirstSample);
            if (address.preparedGlobalIndex != host - placement.offsetSamples)
                return false;
            if (address.bufferLocalIndex < 0 || address.bufferLocalIndex >= preparedLength)
                return false;
            const auto slot = static_cast<size_t>(host - hostSpan.begin);
            if (rendered[slot] != -1)
                return false;
            rendered[slot] = address.preparedGlobalIndex;
        }

        cursor = blockEnd;
        ++blockIndex;
    }

    // 与连续参考（host - D）逐样本相等，无重样、无漏样。
    for (size_t i = 0; i < rendered.size(); ++i)
    {
        const int64_t host = hostSpan.begin + static_cast<int64_t>(i);
        if (rendered[i] != host - placement.offsetSamples)
            return false;
    }

    // seek 到任意位置与连续参考切片相同。
    const int64_t seekPositions[] = {48000, 51456, 100000, 143488};
    for (const int64_t seek : seekPositions)
    {
        const int64_t count = std::min<int64_t>(512, hostSpan.end - seek);
        for (int64_t i = 0; i < count; ++i)
        {
            const int64_t host = seek + i;
            const auto address = PSG::mapHostSampleToPrepared(placement, host, preparedFirstSample);
            if (rendered[static_cast<size_t>(host - hostSpan.begin)] != address.preparedGlobalIndex)
                return false;
            if (address.bufferLocalIndex != host - placement.offsetSamples - preparedFirstSample)
                return false;
        }
    }

    // 半开终点：host Z 不属于播放区间，prepared global 恰好越过有效区间。
    if (!PSG::intersect({hostSpan.end, hostSpan.end + 128}, hostSpan).empty())
        return false;
    return PSG::mapHostSampleToPrepared(placement, hostSpan.end, preparedFirstSample)
               .preparedGlobalIndex == preparedSpan.end;
}

bool testContiguousSlicesInheritOffsetAndSharedBoundary()
{
    constexpr double hostRate = 48000.0;
    const auto parent = PSG::makePlacement(hostRate, 1.0, 2.5, 0.5);
    const auto child = PSG::makeContiguousSlice(parent, 2.5, 4.0);
    const auto grandchild = PSG::makeContiguousSlice(child, 4.0, 5.25);

    if (parent.offsetSamples != 24000
        || parent.playbackBeginSample != 48000
        || parent.playbackEndSample != 120000)
        return false;

    // 连续切片共享同一个 D；公共边界投影一次，前后片端点相同。
    if (child.offsetSamples != parent.offsetSamples
        || grandchild.offsetSamples != parent.offsetSamples)
        return false;
    if (parent.playbackEndSample != child.playbackBeginSample
        || child.playbackEndSample != grandchild.playbackBeginSample)
        return false;

    const auto parentPrepared = PSG::requiredPreparedSpan(parent);
    const auto childPrepared = PSG::requiredPreparedSpan(child);
    const auto grandPrepared = PSG::requiredPreparedSpan(grandchild);
    if (parentPrepared.end != childPrepared.begin
        || childPrepared.end != grandPrepared.begin
        || parentPrepared.begin != 24000
        || grandPrepared.end != 228000)
        return false;

    // 独立放置关系（M 随切片同步平移）与继承 D 的切片结果一致。
    const auto independentChild = PSG::makePlacement(hostRate, 2.5, 4.0, 2.0);
    const auto independentGrandchild = PSG::makePlacement(hostRate, 4.0, 5.25, 3.5);
    return independentChild.offsetSamples == child.offsetSamples
        && independentChild.playbackBeginSample == child.playbackBeginSample
        && independentChild.playbackEndSample == child.playbackEndSample
        && independentGrandchild.offsetSamples == grandchild.offsetSamples
        && independentGrandchild.playbackBeginSample == grandchild.playbackBeginSample
        && independentGrandchild.playbackEndSample == grandchild.playbackEndSample;
}

bool testNegativeLocalPrefixClipsToValidPreparedSuffix()
{
    constexpr double hostRate = 48000.0;
    const auto placement = PSG::makePlacement(hostRate, 0.0, 1.0, 0.0);
    const auto hostSpan = PSG::hostPlaybackSpan(placement);
    const auto preparedSpan = PSG::requiredPreparedSpan(placement);
    if (hostSpan.begin != 0 || hostSpan.end != 48000
        || preparedSpan.begin != 0 || preparedSpan.end != 48000)
        return false;

    // prepared buffer 从来源全局 24 开始，有效 local 区间 [0, 100)。
    const int64_t preparedFirstSample = 24;
    const int64_t preparedLength = 100;
    const auto block = PSG::intersect(hostSpan, PSG::SampleSpan{0, 128});
    if (block.begin != 0 || block.end != 128)
        return false;

    // prepared 有效全局区间平移到 host 坐标后与 block 求交。
    const auto validHost = PSG::intersect(
        block,
        PSG::SampleSpan{preparedFirstSample + placement.offsetSamples,
                        preparedFirstSample + placement.offsetSamples + preparedLength});
    if (validHost.begin != 24 || validHost.end != 124)
        return false;

    // 负 local 前缀被裁剪，有效后缀从 local 0 连续到 local 99。
    int64_t negativePrefix = 0;
    for (int64_t host = block.begin; host < validHost.begin; ++host)
    {
        if (PSG::mapHostSampleToPrepared(placement, host, preparedFirstSample).bufferLocalIndex >= 0)
            return false;
        ++negativePrefix;
    }
    if (negativePrefix != 24)
        return false;
    if (PSG::mapHostSampleToPrepared(placement, validHost.begin, preparedFirstSample)
            .bufferLocalIndex != 0
        || PSG::mapHostSampleToPrepared(placement, validHost.end - 1, preparedFirstSample)
               .bufferLocalIndex != preparedLength - 1
        || PSG::mapHostSampleToPrepared(placement, validHost.end, preparedFirstSample)
               .bufferLocalIndex != preparedLength)
        return false;

    // 非零 D：host 起点先落入负 local 前缀，有效后缀从 J0 + D 开始。
    const auto shifted = PSG::makePlacement(hostRate, 1.0, 1.25, 0.25);
    if (shifted.offsetSamples != 36000 || shifted.playbackBeginSample != 48000)
        return false;
    const int64_t shiftedFirst = PSG::requiredPreparedSpan(shifted).begin + preparedFirstSample;
    const auto prefixAddress = PSG::mapHostSampleToPrepared(shifted,
                                                            shifted.playbackBeginSample,
                                                            shiftedFirst);
    if (prefixAddress.preparedGlobalIndex != 12000 || prefixAddress.bufferLocalIndex != -24)
        return false;
    const auto suffixAddress = PSG::mapHostSampleToPrepared(shifted,
                                                            shiftedFirst + shifted.offsetSamples,
                                                            shiftedFirst);
    return suffixAddress.preparedGlobalIndex == 12024
        && suffixAddress.bufferLocalIndex == 0;
}

bool testAuthoritativeEndAvoidsReconstructionRounding()
{
    constexpr double hostRate = 48000.0;

    // 权威终点恰好落在半样本 tie 上：Q(E * H) = 7。
    const double authoritativeStart = -130.75 / hostRate;
    const double authoritativeEnd = 6.5 / hostRate;
    const auto placement = PSG::makePlacement(hostRate,
                                              authoritativeStart,
                                              authoritativeEnd,
                                              0.0);
    if (placement.playbackEndSample != 7)
        return false;
    if (PSG::projectToSample(authoritativeEnd * hostRate) != 7)
        return false;

    // 由起点与时长重建的终点低一个 ULP，投影落到 6；
    // 直接消费权威 E 消除该差异。
    const double reconstructedEnd = authoritativeStart + (authoritativeEnd - authoritativeStart);
    if (reconstructedEnd == authoritativeEnd)
        return false;
    return PSG::projectToSample(reconstructedEnd * hostRate) == 6;
}

bool testRequiredPreparedSpanUsesFrozenOffset()
{
    constexpr double hostRate = 44100.0;
    const auto positive = PSG::makePlacement(hostRate, 0.0011, 0.0021, 0.0005);
    if (positive.playbackBeginSample != 49
        || positive.offsetSamples != 26
        || positive.playbackEndSample != 93)
        return false;

    const auto positivePrepared = PSG::requiredPreparedSpan(positive);
    if (positivePrepared.begin != 23 || positivePrepared.end != 67)
        return false;
    // A - D 与独立窗口起点投影可相差 +1 个目标样本，权威窗口时间保持原值。
    if (PSG::projectToSample(0.0005 * hostRate) != 22)
        return false;

    const auto negative = PSG::makePlacement(hostRate, -0.0011, -0.0002, -0.0005);
    if (negative.playbackBeginSample != -49
        || negative.offsetSamples != -26
        || negative.playbackEndSample != -9)
        return false;

    const auto negativePrepared = PSG::requiredPreparedSpan(negative);
    return negativePrepared.begin == -23
        && negativePrepared.end == 17
        && PSG::projectToSample(-0.0005 * hostRate) == -22;
}

} // namespace

int main()
{
    if (!testProjectHalfTiesTowardPositiveInfinity())
    {
        std::fputs("FAIL: playback projection half ties toward +infinity\n", stderr);
        return 1;
    }

    if (!testProjectTranslationInvarianceAtLargePositions())
    {
        std::fputs("FAIL: playback projection translation/large-position invariance\n", stderr);
        return 1;
    }

    if (!testPlacementEliminatesSecondsRoundTripAtBlockBoundaries())
    {
        std::fputs("FAIL: placement at original 3456/3584 and 13824/14336 boundaries\n", stderr);
        return 1;
    }

    if (!testHalfOpenSampleSpanIntersection())
    {
        std::fputs("FAIL: half-open sample span intersection\n", stderr);
        return 1;
    }

    if (!testMixedBlocksAndSeekMatchContinuousPreparedReference())
    {
        std::fputs("FAIL: mixed block/seek prepared address mapping\n", stderr);
        return 1;
    }

    if (!testContiguousSlicesInheritOffsetAndSharedBoundary())
    {
        std::fputs("FAIL: contiguous slice shared offset/boundary\n", stderr);
        return 1;
    }

    if (!testNegativeLocalPrefixClipsToValidPreparedSuffix())
    {
        std::fputs("FAIL: negative local prefix clipped to valid suffix\n", stderr);
        return 1;
    }

    if (!testAuthoritativeEndAvoidsReconstructionRounding())
    {
        std::fputs("FAIL: authoritative end projection vs reconstructed end\n", stderr);
        return 1;
    }

    if (!testRequiredPreparedSpanUsesFrozenOffset())
    {
        std::fputs("FAIL: required prepared span from frozen offset\n", stderr);
        return 1;
    }

    std::puts("PlaybackSampleGeometry tests passed.");
    return 0;
}
