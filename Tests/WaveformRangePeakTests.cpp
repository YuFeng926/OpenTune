// WaveformMipmap::queryExactPeak 精确查询契约：
// 1. 任意区间（含边缘、跨区块、各层尾块）与真实 PCM + setRange 量化的参考值逐位一致；
// 2. 构建中途与构建完成后，同一区间结果一致（缓存层级与构建进度不暴露到查询结果）；
// 3. 静音是数据：完全静音区间返回有效 (0,0)，不是空区间；
// 4. 空区间约定：零长、倒序、越界区间返回 std::nullopt，部分重叠按交集取值。

#include "../Source/Standalone/UI/WaveformMipmap.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <optional>
#include <random>
#include <vector>

using namespace OpenTune;

static int failures = 0;

static void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

// 参考实现：直接扫真实 PCM，用与缓存区块相同的 setRange 量化。
// 查询结果与该参考逐位一致，即证明缓存区块与 PCM 边缘聚合可交换。
static PeakSample referencePeak(const juce::AudioBuffer<float>& buffer, int64_t s0, int64_t s1)
{
    float globalMin = 0.0f;
    float globalMax = 0.0f;
    bool hasData = false;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax(
            buffer.getReadPointer(ch) + s0, static_cast<int>(s1 - s0));
        if (!hasData)
        {
            globalMin = range.getStart();
            globalMax = range.getEnd();
            hasData = true;
        }
        else
        {
            globalMin = std::min(globalMin, range.getStart());
            globalMax = std::max(globalMax, range.getEnd());
        }
    }

    PeakSample peak;
    peak.setRange(globalMin, globalMax);
    return peak;
}

// 确定性测试音频：满幅命中、低于 1/127 的细变化（量化敏感）、中段静音。
// 长度 100001 不被任何 kSamplesPerPeak 整除，每个层级都有部分覆盖的尾块。
static std::shared_ptr<juce::AudioBuffer<float>> makeTestBuffer()
{
    constexpr int64_t numSamples = 100001;
    constexpr int channels = 2;

    auto buffer = std::make_shared<juce::AudioBuffer<float>>(channels, static_cast<int>(numSamples));
    std::mt19937 rng(0x5EED1234u);
    std::uniform_real_distribution<float> amplitude(-1.0f, 1.0f);

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* data = buffer->getWritePointer(ch);
        for (int64_t i = 0; i < numSamples; ++i)
        {
            if (i % 9973 == 0)
                data[i] = (i % 19948 == 0) ? 1.0f : -1.0f;
            else if (i % 5111 < 40)
                data[i] = static_cast<float>(i % 7) * 1.0e-4f;
            else
                data[i] = amplitude(rng);
        }
    }

    // 中段静音：零值是数据
    for (int ch = 0; ch < channels; ++ch)
        std::fill(buffer->getWritePointer(ch) + 40000, buffer->getWritePointer(ch) + 60000, 0.0f);

    return buffer;
}

static void checkRange(const WaveformMipmap& mipmap, const juce::AudioBuffer<float>& buffer,
                       int64_t s0, int64_t s1, const char* phase)
{
    const auto got = mipmap.queryExactPeak(s0, s1);
    const PeakSample want = referencePeak(buffer, s0, s1);
    if (!got.has_value() || got->min != want.min || got->max != want.max)
    {
        std::fprintf(stderr, "FAIL: %s range [%lld, %lld) got (%d, %d%s) want (%d, %d)\n",
                     phase, static_cast<long long>(s0), static_cast<long long>(s1),
                     got.has_value() ? got->min : 0, got.has_value() ? got->max : 0,
                     got.has_value() ? "" : ", nullopt", want.min, want.max);
        ++failures;
    }
}

static void runRangeSuite(const WaveformMipmap& mipmap, const juce::AudioBuffer<float>& buffer,
                          const char* phase)
{
    constexpr int64_t numSamples = 100001;

    // 全段、单样本、区块边界、静音段、尾块、亚 level 0 区块宽度
    checkRange(mipmap, buffer, 0, numSamples, phase);
    checkRange(mipmap, buffer, 0, 1, phase);
    checkRange(mipmap, buffer, numSamples - 1, numSamples, phase);
    checkRange(mipmap, buffer, 32767, 32769, phase);        // 跨 level 5 区块边界
    checkRange(mipmap, buffer, 32768, 98304, phase);        // level 5 完整区块段
    checkRange(mipmap, buffer, 32768, 100001, phase);       // 完整区块 + 各层尾块
    checkRange(mipmap, buffer, 40000, 60000, phase);        // 中段静音
    checkRange(mipmap, buffer, 45000, 46000, phase);        // 完全静音
    checkRange(mipmap, buffer, 59999, 60001, phase);        // 静音边界两侧
    checkRange(mipmap, buffer, 1000, 1090, phase);          // 亚 level 0 区块宽度

    // 随机区间
    std::mt19937 rng(0xA11C0DEu);
    std::uniform_int_distribution<int64_t> position(0, numSamples);
    for (int i = 0; i < 512; ++i)
    {
        int64_t a = position(rng);
        int64_t b = position(rng);
        if (a > b) std::swap(a, b);
        if (a == b) continue;
        checkRange(mipmap, buffer, a, b, phase);
    }
}

static void testEmptyRangeConvention(const WaveformMipmap& mipmap, int64_t numSamples)
{
    check(!mipmap.queryExactPeak(0, 0).has_value(), "zero-length range is empty");
    check(!mipmap.queryExactPeak(500, 500).has_value(), "zero-length mid range is empty");
    check(!mipmap.queryExactPeak(50, 40).has_value(), "reversed range is empty");
    check(!mipmap.queryExactPeak(numSamples, numSamples + 100).has_value(), "range past audio is empty");
    check(!mipmap.queryExactPeak(-100, -10).has_value(), "negative range is empty");
    check(mipmap.queryExactPeak(-50, 10).has_value(), "head overlap is non-empty");
    check(mipmap.queryExactPeak(numSamples - 10, numSamples + 50).has_value(), "tail overlap is non-empty");
}

int main()
{
    auto buffer = makeTestBuffer();
    const int64_t numSamples = buffer->getNumSamples();

    WaveformMipmap mipmap;
    mipmap.setAudioSource(buffer);

    testEmptyRangeConvention(mipmap, numSamples);

    // 部分重叠按交集取值（构建前，全部走真实 PCM）
    {
        const auto head = mipmap.queryExactPeak(-50, 10);
        const PeakSample want = referencePeak(*buffer, 0, 10);
        check(head.has_value() && head->min == want.min && head->max == want.max,
              "head overlap matches intersection reference");
        const auto tail = mipmap.queryExactPeak(numSamples - 10, numSamples + 50);
        const PeakSample wantTail = referencePeak(*buffer, numSamples - 10, numSamples);
        check(tail.has_value() && tail->min == wantTail.min && tail->max == wantTail.max,
              "tail overlap matches intersection reference");
    }

    // 构建前：缓存全空，查询全部下沉到真实 PCM
    runRangeSuite(mipmap, *buffer, "no-build");

    // 构建中途：粗层已建、细层未建，查询仍与真实 PCM 一致
    constexpr int level = WaveformMipmap::kNumLevels - 1;
    auto& coarse = mipmap.getLevel(level);
    coarse.peaks[0] = referencePeak(*buffer, 0, WaveformMipmap::kSamplesPerPeak[level]);
    coarse.buildProgress = 1;
    runRangeSuite(mipmap, *buffer, "partial-build");

    // 层级透明：同一区间在构建中途与构建完成后的结果逐位一致
    const int64_t probes[][2] = {
        {0, numSamples}, {32768, 98304}, {100, 500}, {40000, 60000}, {99800, 100001},
    };
    std::vector<std::optional<PeakSample>> midResults;
    for (const auto& probe : probes)
        midResults.push_back(mipmap.queryExactPeak(probe[0], probe[1]));

    while (!mipmap.isComplete())
        mipmap.buildIncremental(4.0);
    check(mipmap.isComplete(), "build completes to full");

    for (std::size_t i = 0; i < std::size(probes); ++i)
    {
        const auto after = mipmap.queryExactPeak(probes[i][0], probes[i][1]);
        check(midResults[i].has_value() == after.has_value()
              && (!after.has_value() || (midResults[i]->min == after->min && midResults[i]->max == after->max)),
              "query result independent of built levels");
    }

    // 构建完成后：同一区间集合仍与真实 PCM 参考值一致
    runRangeSuite(mipmap, *buffer, "complete-build");

    // 静音是数据：完全静音区间是有效 (0,0)
    {
        const auto silent = mipmap.queryExactPeak(45000, 46000);
        check(silent.has_value() && silent->isZero(),
              "fully silent range is valid zero data, not empty");
    }

    if (failures != 0)
    {
        std::fprintf(stderr, "WaveformRangePeakTests: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("WaveformRangePeakTests passed.");
    return 0;
}
