#include "WaveformMipmap.h"
#include <algorithm>
#include <cmath>

namespace OpenTune {

void WaveformMipmap::setAudioSource(std::shared_ptr<const juce::AudioBuffer<float>> buffer)
{
    if (!buffer || buffer->getNumSamples() == 0)
    {
        clear();
        return;
    }
    
    audioBuffer_ = buffer;
    numSamples_ = buffer->getNumSamples();
    numChannels_ = buffer->getNumChannels();
    
    initializeLevels();
}

bool WaveformMipmap::isSourceChanged(std::shared_ptr<const juce::AudioBuffer<float>> buffer) const noexcept
{
    if (audioBuffer_.get() != buffer.get())
        return true;
    
    if (buffer)
    {
        if (numSamples_ != buffer->getNumSamples() || numChannels_ != buffer->getNumChannels())
            return true;
    }
    
    return false;
}

void WaveformMipmap::initializeLevels()
{
    for (int i = 0; i < kNumLevels; ++i)
    {
        const int64_t numPeaks = (numSamples_ + kSamplesPerPeak[i] - 1) / kSamplesPerPeak[i];
        levels_[i].peaks.assign(static_cast<std::size_t>(numPeaks), PeakSample());
        levels_[i].builtMask.assign(static_cast<std::size_t>((numPeaks + 63) / 64), 0ull);
        levels_[i].builtCount = 0;
        levels_[i].buildProgress = 0;
    }
    preheat_.clear();
}

void WaveformMipmap::requestPreheat(int64_t startSample, int64_t endSampleExclusive, int levelCount)
{
    if (!audioBuffer_ || numSamples_ <= 0 || levelCount <= 0)
        return;

    PreheatWindow window;
    window.startSample = std::clamp<int64_t>(startSample, 0, numSamples_);
    window.endSampleExclusive = std::clamp<int64_t>(endSampleExclusive, 0, numSamples_);
    window.levelCount = std::min(levelCount, kNumLevels);
    if (window.endSampleExclusive <= window.startSample)
        return;

    // 窗口覆盖的显示层级区块已全部建完：无欠债，不入列表。
    if (!isPreheatCompleteWindow(window))
        preheat_.push_back(window);
}

void WaveformMipmap::preheatBlockRange(int level, const PreheatWindow& window,
                                       int64_t& blockStart, int64_t& blockEnd) const noexcept
{
    const int64_t totalPeaks = static_cast<int64_t>(levels_[level].peaks.size());
    const int64_t samplesPerPeak = kSamplesPerPeak[level];
    blockStart = std::clamp<int64_t>(window.startSample / samplesPerPeak, 0, totalPeaks);
    blockEnd = std::clamp<int64_t>((window.endSampleExclusive + samplesPerPeak - 1) / samplesPerPeak,
                                   blockStart, totalPeaks);
}

bool WaveformMipmap::isPreheatCompleteWindow(const PreheatWindow& window) const
{
    for (int level = 0; level < window.levelCount; ++level)
    {
        int64_t blockStart = 0;
        int64_t blockEnd = 0;
        preheatBlockRange(level, window, blockStart, blockEnd);
        const auto& lvl = levels_[level];
        int64_t block = blockStart;
        while (block < blockEnd)
        {
            const int64_t wordIndex = block >> 6;
            const int64_t wordEnd = std::min(blockEnd, ((wordIndex + 1) << 6));
            const unsigned firstBit = static_cast<unsigned>(block) & 63u;
            const unsigned lastBit = static_cast<unsigned>(wordEnd - 1) & 63u;
            uint64_t mask = (lastBit == 63u)
                ? ~0ull
                : ((1ull << (lastBit + 1u)) - 1ull);
            mask &= ~0ull << firstBit;
            if ((lvl.builtMask[static_cast<std::size_t>(wordIndex)] & mask) != mask)
                return false;
            block = wordEnd;
        }
    }
    return true;
}

bool WaveformMipmap::buildIncremental(double timeBudgetMs)
{
    if (!audioBuffer_ || numSamples_ <= 0)
        return false;

    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool levelCompleted = false;
    bool outOfBudget = false;

    // 第一轮：预热窗口通道（粗→细，不因某层未完整而阻塞更细层）。
    levelCompleted = sweepPreheatWindows(startMs, timeBudgetMs, outOfBudget) || levelCompleted;

    // 第二轮：默认回填（粗→细 + 「上一整层完成才进入下一层」，含窗口层的剩余区块）。
    if (!outOfBudget)
    {
        for (int level = kNumLevels - 1; level >= 0; --level)
        {
            if (levels_[level].fullyBuilt())
                continue;

            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            const double remain = timeBudgetMs - (nowMs - startMs);
            if (remain <= 0.0)
                break;

            buildLevelSlice(level, remain);

            if (levels_[level].fullyBuilt())
            {
                levelCompleted = true;
                continue;
            }

            // 该层未完整时不进入下一层（原顺序语义）。
            break;
        }
    }

    clearCompletedPreheatWindows();
    // 返回值语义：是否有新 level 从 incomplete 转为 complete（可绘制事件）
    return levelCompleted;
}

bool WaveformMipmap::buildPreheatWindows(double timeBudgetMs)
{
    if (!audioBuffer_ || numSamples_ <= 0)
        return false;

    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool outOfBudget = false;
    const bool levelCompleted = sweepPreheatWindows(startMs, timeBudgetMs, outOfBudget);
    clearCompletedPreheatWindows();
    return levelCompleted;
}

bool WaveformMipmap::sweepPreheatWindows(double startMs, double timeBudgetMs, bool& outOfBudget)
{
    bool levelCompleted = false;
    for (int level = kNumLevels - 1; level >= 0; --level)
    {
        if (levels_[level].fullyBuilt())
            continue;
        const bool windowLevel = std::any_of(preheat_.begin(), preheat_.end(),
            [level](const PreheatWindow& window) { return level < window.levelCount; });
        if (!windowLevel)
            continue;

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double remain = timeBudgetMs - (nowMs - startMs);
        if (remain <= 0.0)
        {
            outOfBudget = true;
            break;
        }

        // 窗口 pass 只建窗口内块，建完即止，剩余预算留给下一窗口层与默认通道。
        buildPreheatWindowSlice(level, remain);

        if (levels_[level].fullyBuilt())
            levelCompleted = true;
        // 窗口层继续下沉：更细层的窗口部分优先于本层回填。
    }
    return levelCompleted;
}

void WaveformMipmap::clearCompletedPreheatWindows()
{
    // 清理无条件执行（还清契约的一部分：窗口建完即出列表，债当拍归零）；
    // 每窗口判定是字级扫描，成本与窗口列表成正比，跨拍公平由第一轮
    // 份额轮转与位图跳过内的时钟检查承担。
    if (preheat_.empty())
        return;
    for (auto it = preheat_.begin(); it != preheat_.end();)
    {
        if (isPreheatCompleteWindow(*it))
            it = preheat_.erase(it);
        else
            ++it;
    }
}

void WaveformMipmap::buildBlockRange(int level, int64_t startIdx, int64_t endIdx)
{
    auto& lvl = levels_[level];
    const int samplesPerPeak = kSamplesPerPeak[level];
    for (int64_t peakIdx = startIdx; peakIdx < endIdx; ++peakIdx)
    {
        if (lvl.isBuilt(peakIdx))
            continue;
        const int64_t sampleStart = peakIdx * samplesPerPeak;
        const int64_t sampleEnd = std::min(sampleStart + samplesPerPeak, numSamples_);

        // 区块峰值与 PCM 边缘聚合共用 scanPcmPeak 的同一量化函数，
        // 保证区块缓存与真实 PCM 聚合可交换（逐位一致）。
        lvl.peaks[static_cast<std::size_t>(peakIdx)] = scanPcmPeak(sampleStart, sampleEnd);
        lvl.markBuilt(peakIdx);
    }
}

void WaveformMipmap::buildLevelSlice(int level, double timeBudgetMs)
{
    auto& lvl = levels_[level];

    if (lvl.fullyBuilt())
        return;

    if (!audioBuffer_ || numSamples_ <= 0)
        return;

    const int64_t totalPeaks = static_cast<int64_t>(lvl.peaks.size());

    if (totalPeaks <= 0)
        return;

    const double startMs = juce::Time::getMillisecondCounterHiRes();

    // 默认通道：左→右补齐剩余区块（跳过窗口通道已建区块）。
    // 按采样工作量限批：单批最坏扫描量恒定（粗层批更少块），时钟检查更频繁，
    // 单拍越过软预算的幅度以一批为上界。
    constexpr int64_t kBatchSamples = 65536;
    const int64_t batchSize = std::max<int64_t>(1, kBatchSamples / kSamplesPerPeak[level]);
    while (lvl.buildProgress < totalPeaks)
    {
        const int64_t endIdx = std::min(lvl.buildProgress + batchSize, totalPeaks);
        buildBlockRange(level, lvl.buildProgress, endIdx);
        lvl.buildProgress = endIdx;

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if ((nowMs - startMs) >= timeBudgetMs)
            break;
    }
}

void WaveformMipmap::buildPreheatWindowSlice(int level, double timeBudgetMs)
{
    auto& lvl = levels_[level];

    if (lvl.fullyBuilt())
        return;

    if (!audioBuffer_ || numSamples_ <= 0)
        return;

    const int64_t totalPeaks = static_cast<int64_t>(lvl.peaks.size());

    if (totalPeaks <= 0)
        return;

    const double startMs = juce::Time::getMillisecondCounterHiRes();
    constexpr int64_t kBatchSamples = 65536;
    const int64_t batchSize = std::max<int64_t>(1, kBatchSamples / kSamplesPerPeak[level]);

    // 逐窗口：按位图单调推进枚举窗口内未建块（全建字 64 块一次跳过），
    // 左右跳窗与断点续建天然成立；窗口内建完即止，不做全层回填。
    for (const auto& window : preheat_)
    {
        if (level >= window.levelCount)
            continue;

        int64_t windowStart = 0;
        int64_t windowEnd = 0;
        preheatBlockRange(level, window, windowStart, windowEnd);

        int64_t scan = windowStart;
        int64_t skippedSinceClockCheck = 0;
        for (;;)
        {
            // 已建前缀的位图跳过纳入同一截止时间：全建字 64 块快进，每 64 次
            // 快进事件（最坏 4096 块 ≈ 单批时长量级）查一次时钟。
            while (scan < windowEnd && lvl.isBuilt(scan))
            {
                const auto wordIdx = static_cast<std::size_t>(scan >> 6);
                const uint64_t word = lvl.builtMask[wordIdx];
                scan = (~word) == 0u
                    ? (static_cast<int64_t>(wordIdx) + 1) << 6
                    : scan + 1;
                if (++skippedSinceClockCheck >= 64)
                {
                    skippedSinceClockCheck = 0;
                    if ((juce::Time::getMillisecondCounterHiRes() - startMs) >= timeBudgetMs)
                        return;
                }
            }
            if (scan >= windowEnd)
                break;  // 本窗口建完：窗口 pass 职责止于此

            const int64_t endIdx = std::min(scan + batchSize, windowEnd);
            buildBlockRange(level, scan, endIdx);
            scan = endIdx;

            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            if ((nowMs - startMs) >= timeBudgetMs)
                return;
        }
    }
}

bool WaveformMipmap::isComplete() const noexcept
{
    for (int i = 0; i < kNumLevels; ++i)
    {
        if (!levels_[i].fullyBuilt())
            return false;
    }
    return true;
}

float WaveformMipmap::getBuildProgress() const noexcept
{
    if (!audioBuffer_ || numSamples_ <= 0)
        return 0.0f;

    int64_t totalPeaks = 0;
    int64_t completedPeaks = 0;

    for (int i = 0; i < kNumLevels; ++i)
    {
        totalPeaks += static_cast<int64_t>(levels_[i].peaks.size());
        completedPeaks += levels_[i].builtCount;
    }

    if (totalPeaks == 0)
        return 0.0f;

    return static_cast<float>(completedPeaks) / static_cast<float>(totalPeaks);
}

int WaveformMipmap::selectBestLevelIndex(double pixelsPerSecond) const
{
    const double secondsPerPixel = 1.0 / pixelsPerSecond;
    
    for (int i = kNumLevels - 1; i >= 0; --i)
    {
        const double secondsPerPeak = static_cast<double>(kSamplesPerPeak[i]) / kBaseSampleRate;

        if (secondsPerPeak <= secondsPerPixel * 2.0 && levels_[i].fullyBuilt())
        {
            return i;
        }
    }

    for (int i = 0; i < kNumLevels; ++i)
    {
        if (levels_[i].fullyBuilt())
            return i;
    }
    
    return -1;
}

std::optional<PeakSample> WaveformMipmap::queryExactPeak(int64_t startSample,
                                                         int64_t endSampleExclusive) const
{
    if (numSamples_ <= 0)
        return std::nullopt;

    // 与真实样本范围求交集；交集为空即空区间
    const int64_t s0 = std::max<int64_t>(startSample, 0);
    const int64_t s1 = std::min<int64_t>(endSampleExclusive, numSamples_);
    if (s1 <= s0)
        return std::nullopt;

    PeakSample accumulator;
    bool hasData = false;
    accumulateRange(s0, s1, kNumLevels - 1, accumulator, hasData);

    // s1 > s0 时最细层必然扫到真实 PCM，accumulator 一定取到数据
    return accumulator;
}

void WaveformMipmap::accumulateRange(int64_t startSample, int64_t endSampleExclusive, int level,
                                     PeakSample& accumulator, bool& hasData) const
{
    if (startSample >= endSampleExclusive)
        return;

    if (level < 0)
    {
        const PeakSample peak = scanPcmPeak(startSample, endSampleExclusive);
        if (!hasData)
        {
            accumulator = peak;
            hasData = true;
        }
        else
        {
            accumulator.merge(peak);
        }
        return;
    }

    const auto& lvl = levels_[level];
    const int64_t samplesPerPeak = kSamplesPerPeak[level];
    const int64_t alignedStart = ((startSample + samplesPerPeak - 1) / samplesPerPeak) * samplesPerPeak;
    const int64_t alignedEnd = (endSampleExclusive / samplesPerPeak) * samplesPerPeak;

    if (alignedStart >= alignedEnd)
    {
        // 区间内没有本层完整区块，整段交给更细层
        accumulateRange(startSample, endSampleExclusive, level - 1, accumulator, hasData);
        return;
    }

    // 头部未对齐段交给更细层
    accumulateRange(startSample, alignedStart, level - 1, accumulator, hasData);

    // 连续已建段直接消费，未建段下沉更细层（结果与构建状态无关，
    // 任意顺序构建下的空洞只把空洞范围交给细层，已建段不重复下沉）。
    int64_t block = alignedStart / samplesPerPeak;
    const int64_t blockEnd = alignedEnd / samplesPerPeak;
    while (block < blockEnd)
    {
        if (lvl.isBuilt(block))
        {
            int64_t runEnd = block + 1;
            while (runEnd < blockEnd && lvl.isBuilt(runEnd))
                ++runEnd;
            for (int64_t b = block; b < runEnd; ++b)
            {
                const PeakSample& peak = lvl.peaks[static_cast<std::size_t>(b)];
                if (!hasData)
                {
                    accumulator = peak;
                    hasData = true;
                }
                else
                {
                    accumulator.merge(peak);
                }
            }
            block = runEnd;
        }
        else
        {
            int64_t holeEnd = block + 1;
            while (holeEnd < blockEnd && !lvl.isBuilt(holeEnd))
                ++holeEnd;
            accumulateRange(block * samplesPerPeak, holeEnd * samplesPerPeak, level - 1,
                            accumulator, hasData);
            block = holeEnd;
        }
    }

    // 尾部不足一个区块的余量交给更细层
    accumulateRange(block * samplesPerPeak, endSampleExclusive, level - 1, accumulator, hasData);
}

PeakSample WaveformMipmap::scanPcmPeak(int64_t startSample, int64_t endSampleExclusive) const
{
    float globalMin = 0.0f;
    float globalMax = 0.0f;
    bool hasData = false;

    for (int ch = 0; ch < numChannels_; ++ch)
    {
        const float* channelData = audioBuffer_->getReadPointer(ch);
        const auto range = juce::FloatVectorOperations::findMinAndMax(
            channelData + startSample, static_cast<int>(endSampleExclusive - startSample));
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

void WaveformMipmap::clear()
{
    audioBuffer_.reset();
    numSamples_ = 0;
    numChannels_ = 0;

    for (int i = 0; i < kNumLevels; ++i)
    {
        levels_[i].peaks.clear();
        levels_[i].builtMask.clear();
        levels_[i].builtCount = 0;
        levels_[i].buildProgress = 0;
    }
    preheat_.clear();
}

WaveformMipmap& WaveformMipmapCache::getOrCreate(ContentKey contentKey)
{
    auto it = caches_.find(contentKey);
    if (it != caches_.end())
        return *it->second;
    
    auto inserted = caches_.emplace(contentKey, std::make_unique<WaveformMipmap>());
    allMipmapsComplete_ = false;
    return *inserted.first->second;
}

void WaveformMipmapCache::remove(ContentKey contentKey)
{
    if (caches_.erase(contentKey) != 0)
        recomputeAllComplete();
}

void WaveformMipmapCache::prune(const std::set<ContentKey>& alive)
{
    bool removed = false;
    for (auto it = caches_.begin(); it != caches_.end();)
    {
        if (alive.find(it->first) == alive.end())
        {
            it = caches_.erase(it);
            removed = true;
            continue;
        }
        ++it;
    }
    if (removed)
        recomputeAllComplete();
}

void WaveformMipmapCache::recomputeAllComplete()
{
    for (const auto& kv : caches_)
    {
        if (!kv.second->isComplete())
            return;
    }
    allMipmapsComplete_ = true;
}

void WaveformMipmapCache::clear()
{
    caches_.clear();
    allMipmapsComplete_ = true;
}

bool WaveformMipmapCache::setAudioSource(ContentKey contentKey,
                                        std::shared_ptr<const juce::AudioBuffer<float>> buffer)
{
    if (!buffer || buffer->getNumSamples() == 0)
    {
        const bool removed = caches_.erase(contentKey) != 0;
        if (removed)
            recomputeAllComplete();
        return removed;
    }

    auto& mipmap = getOrCreate(contentKey);
    if (!mipmap.isSourceChanged(buffer))
        return false;
    mipmap.setAudioSource(std::move(buffer));
    allMipmapsComplete_ = false;
    return true;
}

void WaveformMipmapCache::requestPreheat(ContentKey contentKey, int64_t startSample,
                                         int64_t endSampleExclusive, int levelCount)
{
    auto it = caches_.find(contentKey);
    if (it == caches_.end())
        return;
    it->second->requestPreheat(startSample, endSampleExclusive, levelCount);
}

bool WaveformMipmapCache::hasPreheatDebt() const noexcept
{
    // 欠债蕴含某个 mipmap 未完成：终态直接快速返回，稳态每拍 O(1)。
    if (allMipmapsComplete_)
        return false;
    ++debtScanCounter_;
    for (const auto& kv : caches_)
    {
        if (kv.second->hasPreheatDebt())
            return true;
    }
    return false;
}

void WaveformMipmapCache::clearPreheatDebt() noexcept
{
    for (auto& kv : caches_)
        kv.second->clearPreheat();
}

bool WaveformMipmapCache::buildIncremental(double timeBudgetMs)
{
    if (timeBudgetMs <= 0.0)
        return false;

    if (allMipmapsComplete_)
        return false;

    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool levelCompleted = false;

    // 三轮：第一轮把剩余预算均分给全部欠债 mipmap 的窗口通道（跨拍逐 key 都有
    // 进展，首个 key 的大窗口不占满整拍），第二轮欠债回填，第三轮按 key 序
    // 处理无欠债 mipmap。
    std::size_t debtorCount = 0;
    for (const auto& kv : caches_)
    {
        if (kv.second->hasPreheatDebt())
            ++debtorCount;
    }
    if (debtorCount > 0)
    {
        // 分母随已服务 key 递减：每个 key 拿到「剩余预算 / 尚未获份额的欠债
        // key 数」，末个 key 消化全部剩余（两 key 各得一半，不回流首 key）。
        std::size_t remainingDebtors = debtorCount;
        for (auto& kv : caches_)
        {
            if (!kv.second->hasPreheatDebt())
                continue;
            const double remainAll = timeBudgetMs - (juce::Time::getMillisecondCounterHiRes() - startMs);
            if (remainAll <= 0.0)
                break;
            const double share = remainAll / static_cast<double>(remainingDebtors);
            if (kv.second->buildPreheatWindows(share))
                levelCompleted = true;
            --remainingDebtors;
            if (remainingDebtors == 0)
                break;
        }
    }
    for (auto& kv : caches_)
    {
        if (!kv.second->hasPreheatDebt())
            continue;
        const double remain = timeBudgetMs - (juce::Time::getMillisecondCounterHiRes() - startMs);
        if (remain <= 0.0)
            break;
        if (kv.second->buildIncremental(remain))
            levelCompleted = true;
    }
    for (auto& kv : caches_)
    {
        if (kv.second->hasPreheatDebt())
            continue;
        const double remain = timeBudgetMs - (juce::Time::getMillisecondCounterHiRes() - startMs);
        if (remain <= 0.0)
            break;
        if (kv.second->buildIncremental(remain))
            levelCompleted = true;
    }

    recomputeAllComplete();
    // 返回值语义：是否有新 level 完成（可绘制事件）
    return levelCompleted;
}

} // namespace OpenTune
