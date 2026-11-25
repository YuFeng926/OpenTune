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
    coarse.markBuilt(0);
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

    // ── 预热欠债：窗口区块优先建（事件 → requestPreheat → 心跳清偿）， ──
    // ── 乱序位图构建过程中查询仍逐位一致，欠债在窗口建完时自动清偿。 ──
    {
        // 长音频：level 0 窗口区块远小于总量，保证欠债清偿时窗口外仍有未建区块。
        constexpr int64_t bigSamples = 3000001;
        auto bigBuffer = std::make_shared<juce::AudioBuffer<float>>(2, static_cast<int>(bigSamples));
        std::mt19937 rng(0x1EAFBEEFu);
        std::uniform_real_distribution<float> amplitude(-1.0f, 1.0f);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = bigBuffer->getWritePointer(ch);
            for (int64_t i = 0; i < bigSamples; ++i)
            {
                if (i % 8171 == 0)
                    data[i] = (i % 16343 == 0) ? 1.0f : -1.0f;
                else if (i % 4099 < 24)
                    data[i] = static_cast<float>(i % 7) * 1.0e-4f;
                else
                    data[i] = amplitude(rng);
            }
        }

        WaveformMipmap warm;
        warm.setAudioSource(bigBuffer);
        const int64_t windowStart = bigSamples / 5;
        const int64_t windowEnd = windowStart + bigSamples / 5;

        warm.requestPreheat(windowStart, windowEnd, WaveformMipmap::kNumLevels);
        check(warm.hasPreheatDebt(), "preheat window arms debt");

        // 建置过程中的窗口查询逐位等于真实 PCM 参考
        const auto wantWindow = referencePeak(*bigBuffer, windowStart, windowEnd);
        const auto midWindow = warm.queryExactPeak(windowStart, windowEnd);
        check(midWindow.has_value() && midWindow->min == wantWindow.min && midWindow->max == wantWindow.max,
              "preheat window query matches PCM mid-build");

        while (warm.hasPreheatDebt())
            warm.buildIncremental(4.0);
        check(!warm.hasPreheatDebt(), "debt cleared once window built");

        const auto& lvl0 = warm.getLevel(0);
        const int64_t spp0 = WaveformMipmap::kSamplesPerPeak[0];
        const int64_t w0 = windowStart / spp0;
        const int64_t w1 = (windowEnd + spp0 - 1) / spp0;
        bool windowFullyBuilt = true;
        for (int64_t b = w0; b < w1; ++b)
            windowFullyBuilt = windowFullyBuilt && lvl0.isBuilt(b);
        check(windowFullyBuilt, "window blocks fully built at level 0 after debt cleared");

        bool hasUnbuiltOutside = false;
        const int64_t total0 = static_cast<int64_t>(lvl0.peaks.size());
        for (int64_t b = 0; b < total0 && !hasUnbuiltOutside; ++b)
            if ((b < w0 || b >= w1) && !lvl0.isBuilt(b))
                hasUnbuiltOutside = true;
        check(hasUnbuiltOutside, "outside-window blocks lag behind window priority");

        runRangeSuite(warm, *bigBuffer, "preheat-mid-build");
        while (!warm.isComplete())
            warm.buildIncremental(4.0);
        runRangeSuite(warm, *bigBuffer, "preheat-complete");

        // 已建完范围重复置债：无欠债
        warm.requestPreheat(windowStart, windowEnd, WaveformMipmap::kNumLevels);
        check(!warm.hasPreheatDebt(), "re-arming built window sets no debt");

        // 空窗口与零层级不置债
        warm.requestPreheat(windowStart, windowStart, WaveformMipmap::kNumLevels);
        check(!warm.hasPreheatDebt(), "empty window sets no debt");
        warm.requestPreheat(windowStart, windowEnd, 0);
        check(!warm.hasPreheatDebt(), "zero level count sets no debt");
    }

    // ── 阻断 2 回归（共用 12M 样本源：窗口区块量 > 单片预算，窗口通道语义可确定断言）──
    {
        constexpr int64_t bigSamples = 12000001;
        auto bigBuffer = std::make_shared<juce::AudioBuffer<float>>(1, static_cast<int>(bigSamples));
        std::mt19937 rng(0x5EED0123u);
        std::uniform_real_distribution<float> amplitude(-1.0f, 1.0f);
        {
            auto* data = bigBuffer->getWritePointer(0);
            for (int64_t i = 0; i < bigSamples; ++i)
                data[i] = amplitude(rng);
        }
        const int64_t spp0 = WaveformMipmap::kSamplesPerPeak[0];

        // 窗口起点语义：置债后从窗口起点开始建，不从头扫前缀。
        {
            WaveformMipmap warm;
            warm.setAudioSource(bigBuffer);
            const int64_t winStart = bigSamples * 2 / 3;
            const int64_t winEnd = winStart + bigSamples / 4;

            warm.requestPreheat(winStart, winEnd, 1);
            check(warm.hasPreheatDebt(), "tail window arms debt");

            warm.buildIncremental(1.0);
            const auto& lvl = warm.getLevel(0);
            const int64_t w0 = winStart / spp0;
            // 第一片之后：窗口起点之前的区块全部未建（从头扫前缀即为本断言失败）。
            bool prefixUnbuilt = true;
            for (int64_t b = 0; b < w0 && prefixUnbuilt; ++b)
                prefixUnbuilt = prefixUnbuilt && !lvl.isBuilt(b);
            check(prefixUnbuilt, "window channel starts at window start, not sample 0");

            bool windowProgressed = false;
            for (int64_t b = w0; b < winEnd / spp0 && !windowProgressed; ++b)
                windowProgressed = lvl.isBuilt(b);
            check(windowProgressed, "window channel builds inside the window");

            while (warm.hasPreheatDebt())
                warm.buildIncremental(1.0);
            check(!warm.hasPreheatDebt(), "tail window debt clears from window start");
            bool windowFullyBuilt = true;
            const int64_t w1 = (winEnd + spp0 - 1) / spp0;
            for (int64_t b = w0; b < w1; ++b)
                windowFullyBuilt = windowFullyBuilt && lvl.isBuilt(b);
            check(windowFullyBuilt, "window blocks fully built after debt cleared");
        }

        // 跨层窗口优先：窗口涉及的最细层在粗层全曲未完成前即可推进。
        {
            WaveformMipmap warm;
            warm.setAudioSource(bigBuffer);
            const int64_t winStart = bigSamples / 3;
            const int64_t winEnd = winStart + bigSamples / 6;
            warm.requestPreheat(winStart, winEnd, 3);
            check(warm.hasPreheatDebt(), "multi-level window arms debt");

            warm.buildIncremental(1.0);
            const int64_t spp2 = WaveformMipmap::kSamplesPerPeak[2];
            const auto& lvl2 = warm.getLevel(2);
            const int64_t l2w0 = winStart / spp2;
            bool level2WindowProgress = false;
            for (int64_t b = l2w0; b < winEnd / spp2 && !level2WindowProgress; ++b)
                level2WindowProgress = lvl2.isBuilt(b);
            check(level2WindowProgress, "level 2 window builds first");

            // 真正的跨层契约：L2 全曲未完整时 L1 窗口已推进。
            const int64_t spp1 = WaveformMipmap::kSamplesPerPeak[1];
            const auto& lvl1 = warm.getLevel(1);
            const int64_t l1w0 = winStart / spp1;
            bool level1WindowProgress = false;
            for (int64_t b = l1w0; b < winEnd / spp1 && !level1WindowProgress; ++b)
                level1WindowProgress = lvl1.isBuilt(b);
            check(level1WindowProgress && !lvl2.fullyBuilt(),
                  "level 1 window advances before level 2 completes its full range");

            const auto& lvl5 = warm.getLevel(WaveformMipmap::kNumLevels - 1);
            bool level5Untouched = true;
            for (int64_t b = 0; b < static_cast<int64_t>(lvl5.peaks.size()); ++b)
                level5Untouched = level5Untouched && !lvl5.isBuilt(b);

            check(level5Untouched, "non-window coarse levels stay behind window levels");
        }

        // 右窗 → 左窗跳转：新窗独立建完，旧窗已建块保留、未建块不动。
        {
            WaveformMipmap warm;
            warm.setAudioSource(bigBuffer);
            const int64_t rightStart = bigSamples * 2 / 3;
            const int64_t rightEnd = rightStart + bigSamples / 4;
            warm.requestPreheat(rightStart, rightEnd, 1);
            warm.buildIncremental(1.0);

            const auto& lvl = warm.getLevel(0);
            const auto maskAtJump = lvl.builtMask;
            const int64_t r0 = rightStart / spp0;
            const int64_t r1 = (rightEnd + spp0 - 1) / spp0;
            bool rightPartiallyBuilt = false;
            for (int64_t b = r0; b < r1 && !rightPartiallyBuilt; ++b)
                rightPartiallyBuilt = lvl.isBuilt(b);
            check(rightPartiallyBuilt && warm.hasPreheatDebt(),
                  "right window partially built and still owes debt before jump");

            const int64_t leftStart = bigSamples / 12;
            const int64_t leftEnd = leftStart + bigSamples / 12;
            // 组件置债契约是 clear-first：清残留债后按新窗口重置债。
            warm.clearPreheat();
            warm.requestPreheat(leftStart, leftEnd, 1);
            check(warm.hasPreheatDebt(), "left jump arms debt");
            while (warm.hasPreheatDebt())
                warm.buildIncremental(1.0);
            check(!warm.hasPreheatDebt(), "left window debt clears independently");

            const int64_t l0 = leftStart / spp0;
            const int64_t l1 = (leftEnd + spp0 - 1) / spp0;
            bool leftFullyBuilt = true;
            for (int64_t b = l0; b < l1; ++b)
                leftFullyBuilt = leftFullyBuilt && lvl.isBuilt(b);
            check(leftFullyBuilt, "left window fully built after jump");

            bool rightUnchanged = true;
            for (int64_t b = r0; b < r1 && rightUnchanged; ++b)
            {
                const bool builtAtJump = (maskAtJump[static_cast<std::size_t>(b >> 6)]
                    >> (static_cast<unsigned>(b) & 63u)) & 1u;
                rightUnchanged = rightUnchanged && lvl.isBuilt(b) == builtAtJump;
            }
            check(rightUnchanged, "right window blocks unchanged across the jump");
        }

        // 同内容多 placement：两个独立窗口各自推进、各自清偿，不合并不互斥。
        {
            WaveformMipmap warm;
            warm.setAudioSource(bigBuffer);
            const int64_t winA0 = bigSamples / 12;
            const int64_t winA1 = winA0 + bigSamples / 96;
            const int64_t winB0 = bigSamples * 2 / 3;
            const int64_t winB1 = winB0 + bigSamples / 96;
            warm.requestPreheat(winA0, winA1, 1);
            warm.requestPreheat(winB0, winB1, 1);
            check(warm.hasPreheatDebt(), "two disjoint windows arm debt");

            warm.buildIncremental(1.0);
            check(!warm.hasPreheatDebt(), "both small windows clear debt in one slice");

            const auto& lvl = warm.getLevel(0);
            bool windowAFullyBuilt = true;
            for (int64_t b = winA0 / spp0; b < (winA1 + spp0 - 1) / spp0; ++b)
                windowAFullyBuilt = windowAFullyBuilt && lvl.isBuilt(b);
            bool windowBFullyBuilt = true;
            for (int64_t b = winB0 / spp0; b < (winB1 + spp0 - 1) / spp0; ++b)
                windowBFullyBuilt = windowBFullyBuilt && lvl.isBuilt(b);
            check(windowAFullyBuilt && windowBFullyBuilt,
                  "both windows fully built independently");
        }

        // 缓存层清债与欠债优先：清债后重置债按新 key 窗口先建。
        {
            WaveformMipmapCache cache;
            const ContentKey keyA{DomainKind::StandaloneClip, 77, 0};
            const ContentKey keyB{DomainKind::StandaloneClip, 78, 0};
            cache.setAudioSource(keyA, bigBuffer);
            cache.setAudioSource(keyB, bigBuffer);

            const int64_t tailStart = bigSamples * 2 / 3;
            const int64_t tailEnd = tailStart + bigSamples / 4;

            cache.requestPreheat(keyA, tailStart, tailEnd, 1);
            check(cache.hasPreheatDebt(), "cache debt armed for stale key");
            cache.clearPreheatDebt();
            check(!cache.hasPreheatDebt(), "clearPreheatDebt clears stale window debt");

            cache.requestPreheat(keyB, tailStart, tailEnd, 1);
            check(cache.hasPreheatDebt(), "debt re-armed for the fresh key");

            cache.buildIncremental(1.0);
            const auto& lvlB = cache.get(keyB)->getLevel(0);
            const int64_t tw0 = tailStart / spp0;
            bool keyBWindowProgress = false;
            for (int64_t b = tw0; b < tailEnd / spp0 && !keyBWindowProgress; ++b)
                keyBWindowProgress = lvlB.isBuilt(b);
            const auto& lvlA = cache.get(keyA)->getLevel(0);
            bool keyAUntouched = true;
            for (int64_t b = tw0; b < tailEnd / spp0; ++b)
                keyAUntouched = keyAUntouched && !lvlA.isBuilt(b);
            check(keyBWindowProgress && keyAUntouched,
                  "debted key's window builds first; cleared key untouched");
        }

        // 双欠债 key 跨拍进展：第一轮把剩余预算均分给欠债 key（份额轮转），
        // 两个 key 每拍都推进。
        {
            WaveformMipmapCache cache;
            const ContentKey keyA{DomainKind::StandaloneClip, 97, 0};
            const ContentKey keyB{DomainKind::StandaloneClip, 98, 0};
            cache.setAudioSource(keyA, bigBuffer);
            cache.setAudioSource(keyB, bigBuffer);

            const int64_t winA0 = 0;
            const int64_t winA1 = bigSamples * 1 / 3;
            const int64_t winB0 = bigSamples * 2 / 3;
            const int64_t winB1 = bigSamples;
            cache.requestPreheat(keyA, winA0, winA1, 1);
            cache.requestPreheat(keyB, winB0, winB1, 1);
            check(cache.hasPreheatDebt(), "two debtors arm debt");

            const auto builtInWindow = [&](const WaveformMipmap& m, int64_t start, int64_t end) {
                const auto& lvl = m.getLevel(0);
                int64_t count = 0;
                for (int64_t b = start / spp0; b < (end + spp0 - 1) / spp0; ++b)
                    if (lvl.isBuilt(b))
                        ++count;
                return count;
            };

            int64_t prevA = builtInWindow(*cache.get(keyA), winA0, winA1);
            int64_t prevB = builtInWindow(*cache.get(keyB), winB0, winB1);
            for (int beat = 0; beat < 3; ++beat)
            {
                cache.buildIncremental(1.0);
                const int64_t nowA = builtInWindow(*cache.get(keyA), winA0, winA1);
                const int64_t nowB = builtInWindow(*cache.get(keyB), winB0, winB1);
                check(nowA > prevA && nowB > prevB,
                      "both debtors progress within one beat (share rotation)");
                prevA = nowA;
                prevB = nowB;
            }
        }

        // 移除最后一个未完成 key 立即刷新完成标志：稳态直接生效，
        // 全建后相机移动零快照读取的快速路径不再被过期标志挡住。
        {
            WaveformMipmapCache cache;
            const ContentKey keyA{DomainKind::StandaloneClip, 95, 0};
            const ContentKey keyB{DomainKind::StandaloneClip, 96, 0};
            cache.setAudioSource(keyA, bigBuffer);
            auto slowBuffer = std::make_shared<juce::AudioBuffer<float>>(
                1, static_cast<int>(bigSamples) * 4);
            {
                std::mt19937 rng(0x5EED9876u);
                std::uniform_real_distribution<float> amplitude(-1.0f, 1.0f);
                auto* data = slowBuffer->getWritePointer(0);
                for (int64_t i = 0; i < bigSamples * 4; ++i)
                    data[i] = amplitude(rng);
            }
            cache.setAudioSource(keyB, slowBuffer);
            while (!cache.get(keyA)->isComplete())
                cache.buildIncremental(4.0);
            check(!cache.isComplete(), "incomplete key keeps the cache incomplete");

            cache.remove(keyB);
            check(cache.isComplete(),
                  "removing the last incomplete key completes the cache immediately");
            cache.requestPreheat(keyA, 0, 1024, 1);
            check(!cache.hasPreheatDebt(), "steady-state cache takes no debt after arm");
        }
    }

    // ── hasPreheatDebt 稳态 O(1)：终态快速返回不进 mipmap 扫描。 ──
    {
        constexpr int64_t smallSamples = 200001;
        auto smallBuffer = std::make_shared<juce::AudioBuffer<float>>(1, static_cast<int>(smallSamples));
        std::mt19937 rng(0x2468ACE0u);
        std::uniform_real_distribution<float> amplitude(-1.0f, 1.0f);
        {
            auto* data = smallBuffer->getWritePointer(0);
            for (int64_t i = 0; i < smallSamples; ++i)
                data[i] = amplitude(rng);
        }

        WaveformMipmapCache cache;
        const ContentKey keyA{DomainKind::StandaloneClip, 91, 0};
        const ContentKey keyB{DomainKind::StandaloneClip, 92, 0};
        cache.setAudioSource(keyA, smallBuffer);
        cache.setAudioSource(keyB, smallBuffer);
        while (!cache.isComplete())
            cache.buildIncremental(8.0);
        check(cache.isComplete() && !cache.hasPreheatDebt(), "steady state has no debt");

        const uint64_t counterBefore = cache.debtScanCounter_;
        for (int i = 0; i < 1000; ++i)
            (void) cache.hasPreheatDebt();
        check(cache.debtScanCounter_ == counterBefore,
              "steady-state hasPreheatDebt takes the O(1) fast path (no scan)");

        auto otherBuffer = std::make_shared<juce::AudioBuffer<float>>(1, static_cast<int>(smallSamples) * 2);
        {
            auto* data = otherBuffer->getWritePointer(0);
            for (int64_t i = 0; i < smallSamples * 2; ++i)
                data[i] = amplitude(rng);
        }
        cache.setAudioSource(keyB, otherBuffer);
        check(!cache.isComplete() && !cache.hasPreheatDebt(),
              "incomplete cache without debt still reports no debt");
        check(cache.debtScanCounter_ > counterBefore,
              "non-steady state scans mipmaps (counter advances)");
    }

    if (failures != 0)
    {
        std::fprintf(stderr, "WaveformRangePeakTests: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("WaveformRangePeakTests passed.");
    return 0;
}
